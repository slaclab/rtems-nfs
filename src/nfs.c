/* $Id$ */

/* NFS client implementation for RTEMS; hooks into the RTEMS filesystem */

/* Author: Till Straumann <strauman@slac.stanford.edu> 2002 */

#include <rtems.h>
#include <rtems/libio.h>
#include <rtems/libio_.h>
#include <rtems/seterr.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
#include <sys/stat.h>
#include <dirent.h>

#define  DIRENT_HEADER_SIZE ( sizeof(struct dirent) - \
			sizeof( ((struct dirent *)0)->d_name ) )

#include <nfs_prot.h>
#include <mount_prot.h>

#include "rpcio.h"

#ifdef TEST_COMPILATION
#define DECLARE_BODY { return 0; }
#elif defined(FWD_DECL)
#define DECLARE_BODY ;
#endif

#define DELIM							'/'
#define HOSTDELIM						':'
#define UPDIR							".."
#define NFS_VERSION_2					NFS_VERSION

#define CONFIG_NFS_BIG_XACT_SIZE		UDPMSGSIZE
#define CONFIG_NFS_SMALL_XACT_SIZE		256
#define NFSCALL_TIMEOUT					_nfscalltimeout
#define MNTCALL_TIMEOUT					_nfscalltimeout
#define NFSCALL_RETRYPERIOD				_nfscallretry
#define MNTCALL_RETRYPERIOD				_nfscallretry


/* TODO: we should get a proper device identifier for THIS fs... */
/* NOTE: RTEMS uses short st_ino :-(. Therefore, we merge the
 * upper 16bit of the fileid into the device no. This has an impact
 * on performance, as e.g. getcwd() stats() all directory entries
 * when it believes it has crossed a mount point (a.st_dev != b.st_dev)
 */
#define NFS_MAJOR	0xcafe
#warning TSILL need to fix device type
#ifndef	INO_T_IS_4_BYTES
#warning using short st_ino hits performance and may fail to access/find correct files
#define	NFS_MAKE_DEV_T(node) \
		rtems_filesystem_make_dev_t( NFS_MAJOR, \
						(((node)->nfs->id)<<16) | (SERP_ATTR((node)).fileid >> 16) )
#else
/* TODO: should probably work in the server's fsid somehow */
#define	NFS_MAKE_DEV_T(node) \
		rtems_filesystem_make_dev_t( NFS_MAJOR, (((node)->nfs->id)<<16) )
#endif

#undef  TSILLDEBUG

#define DEBUG_COUNT_NODES	(1<<0)
#define DEBUG_TRACK_NODES	(1<<1)
#define DEBUG_EVALPATH		(1<<2)
#define DEBUG_READDIR		(1<<3)
#define DEBUG_UNLINK		(1<<4)

#define DEBUG  ( DEBUG_COUNT_NODES | DEBUG_UNLINK )

#ifdef DEBUG
#define STATIC
#else
#define STATIC static
#endif

#define MUTEX_ATTRIBUTES    (RTEMS_LOCAL           | \
                            RTEMS_PRIORITY         | \
                            RTEMS_INHERIT_PRIORITY | \
                            RTEMS_BINARY_SEMAPHORE)

#define LOCK(s)		do { rtems_semaphore_obtain((s),RTEMS_WAIT,RTEMS_NO_TIMEOUT); } while (0) 
#define UNLOCK(s)	do { rtems_semaphore_release((s)); } while (0)

static struct timeval _nfscalltimeout = { 10, 0 };
static struct timeval _nfscallretry   = { 1,  0 };

/* These are (except for MAXNAMLEN/MAXPATHLEN) copied from IMFS */

static rtems_filesystem_limits_and_options_t nfs_limits_and_options = {
   5, 				/* link_max */
   6, 				/* max_canon */
   7, 				/* max_input */
   NFS_MAXNAMLEN,	/* name_max */
   NFS_MAXPATHLEN,	/* path_max */
   2,				/* pipe_buf */
   1,				/* posix_async_io */
   2,				/* posix_chown_restrictions */
   3,				/* posix_no_trunc */
   4,				/* posix_prio_io */
   5,				/* posix_sync_io */
   6				/* posix_vdisable */
};

/* size of an encoded 'entry' object */
static int dirres_entry_size;

/* Estimated average length of a filename (including terminating 0).
 * This was calculated by doing
 *
 * 	find <some root> -print -exec basename '{}' \; > feil
 * 	wc feil
 *
 * AVG_NAMLEN = (num_chars + num_lines)/num_lines
 */
#define AVG_NAMLEN	10


static struct nfsstats {
	rtems_id	lock;
	int			mounted_fs;
	/* assume we are not going to do more than 16k mounts
	 * during the system lifetime
	 */
	u_short		fs_ids;
} nfsStats = {0};


/* 'time()' hack with less overhead;
 * 
 */

/* assume reading a long word is atomic */
#define READ_LONG_IS_ATOMIC

typedef rtems_unsigned32	TimeStamp;


static inline TimeStamp
nowSeconds(void)
{
register rtems_unsigned32	rval;
#ifndef READ_LONG_IS_ATOMIC
rtems_interrupt_level		l;

	rtems_interrupt_disable(level);
#endif

	rval = _TOD_Seconds_since_epoch;

#ifndef READ_LONG_IS_ATOMIC
	rtems_interrupt_enable(level);
#endif
	return rval;
}

typedef struct DirInfoRec_ {
	readdirargs	readdirargs;
	/* clone of the 'readdirres' fields;
	 * the cookie is put into the readdirargs above
	 */
	nfsstat		status;
	char		*buf, *ptr;
	int			len;
	bool_t		eofreached;
} DirInfoRec, *DirInfo;

/* a string buffer with a maximal length.
 * If the buffer pointer is NULL, it is updated
 * with an appropriately allocated area.
 */
typedef struct strbuf {
	char	*buf;
	u_int	max;
} strbuf;

static bool_t
xdr_strbuf(XDR *xdrs, strbuf *obj)
{
	return xdr_string(xdrs, &obj->buf, obj->max);
}

/* a type better suited for node operations
 * than diropres.
 * fattr and fhs are swapped so parts of this
 * structure may be used as a diroparg
 */

typedef struct serporidok {
	fattr					attributes;
	nfs_fh					file;
	union	{
		struct {
			filename	name;
		}					diroparg;
		struct {
			sattr		attributes;
		}					sattrarg;
		struct {
			u_int 		offset;
			u_int		count;
			u_int		totalcount;
		}					readarg;
		struct {
			u_int		beginoffset;
			u_int		offset;
			u_int		totalcount;
			struct {
				u_int data_len;
				char* data_val;
			}			data;
		}					writearg;
		struct {
			filename	name;
			sattr		attributes;
		}					createarg;
		struct {
			filename	name;
			diropargs	to;
		}					renamearg;
		struct {
			diropargs	to;
		}					linkarg;
		struct {
			filename	name;
			nfspath		to;
			sattr		attributes;
		}					symlinkarg;
		struct {
			nfscookie	cookie;
			u_int		count;
		}					readdirarg;
	}							arg_u;
} serporidok;

typedef struct serporid {
	nfsstat			status;
	union	{
		serporidok	serporid;
	}				serporid_u;
} serporid;

/* an XDR routine to encode/decode the inverted diropres 
 * into an nfsnodestat;
 *
 * NOTE: this routine only acts on
 *   - 'serporid.status'
 *   - 'serporid.file'
 *   - 'serporid.attributes'
 * and leaves the 'arg_u' alone.
 * 
 * The idea is that a 'diropres' is read into 'serporid'
 * which can then be used as an argument to subsequent
 * NFS-RPCs (after filling in the node's arg_u).
 */
bool_t
xdr_serporidok(XDR *xdrs, serporidok *objp)
{
    register int32_t *buf;
    
     if (!xdr_nfs_fh (xdrs, &objp->file))
         return FALSE;
     if (!xdr_fattr (xdrs, &objp->attributes))
         return FALSE;
    return TRUE;
}   

bool_t
xdr_serporid(XDR *xdrs, serporid *objp)
{
    register int32_t *buf;

     if (!xdr_nfsstat (xdrs, &objp->status))
         return FALSE;
    switch (objp->status) {
    case NFS_OK:
         if (!xdr_serporidok(xdrs, &objp->serporid_u.serporid))
             return FALSE;
        break;
    default:
        break;
    }
    return TRUE;
}

bool_t
xdr_dir_info_entry(XDR *xdrs, DirInfo di)
{
union	{
	char			nambuf[NFS_MAXNAMLEN+1];
	nfscookie		cookie;
}				dummy;
struct dirent	*pde = (struct dirent *)di->ptr;
u_int			fileid;
char			*name;
register int	nlen,len,naligned;
nfscookie		*pcookie;

	len = di->len;

	if ( !xdr_u_int(xdrs, &fileid) )
		return FALSE;

	/* we must pass the address of a char* */
	name = (len > NFS_MAXNAMLEN) ? pde->d_name : dummy.nambuf;

	if ( !xdr_filename(xdrs, &name) )
		return FALSE;

	if (len >= 0) {
		nlen      = strlen(name);
		naligned  = nlen + 1 /* string delimiter */ + 3 /* alignment */;
		naligned &= ~3;
		len      -= naligned;
	}

	/* if the cookie goes into the DirInfo, we hope this doesn't fail
	 * - the caller ends up with an invalid readdirargs cookie otherwise...
	 */
	pcookie = (len >= 0) ? &di->readdirargs.cookie : &dummy.cookie;
	if ( !xdr_nfscookie(xdrs, pcookie) )
		return FALSE;

	di->len = len;
	/* adjust the buffer pointer */
	if (len >= 0) {
		pde->d_ino    = fileid;
		pde->d_namlen = nlen;
		pde->d_off	  = di->ptr - di->buf;
		if (name == dummy.nambuf) {
			memcpy(pde->d_name, dummy.nambuf, nlen + 1);
		}
		pde->d_reclen = DIRENT_HEADER_SIZE + naligned;
		di->ptr      += pde->d_reclen;
	}

	return TRUE;
}

bool_t
xdr_dir_info(XDR *xdrs, DirInfo di)
{
DirInfo	dip;
int		good,decoded;
int		remaining;
int		tsill=0;

	if ( !xdr_nfsstat(xdrs, &di->status) )
		return FALSE;

	if ( NFS_OK != di->status )
		return TRUE;

	dip = di;

	while (dip) {
		/* reserve space for the dirent 'header' - we assume it's word aligned! */
#ifdef DEBUG
		assert( DIRENT_HEADER_SIZE % 4 == 0 );
#endif
		dip->len -= DIRENT_HEADER_SIZE;

		/* we pass a 0 size - size is unused since
		 * we always pass a non-NULL pointer
		 */
		if ( !xdr_pointer(xdrs, (caddr_t*)&dip, 0 /* size */, xdr_dir_info_entry) )
			return FALSE;
		tsill++;
	}

	if ( ! xdr_bool(xdrs, &di->eofreached) )
		return FALSE;

	return TRUE;
}

/* Per mounted FS structure */
typedef struct NfsRec_ {
	RpcUdpServer	server;
	int				nodesInUse;
	u_short			id;
} NfsRec, *Nfs;

typedef struct NfsNodeRec_ {
	diropres	res;		/* we must remember the directory where evalpath
							 * found this entry for unlink/rmnod etc.
							 */
	serporid	args;
	Nfs			nfs;		/* fs this node belongs to */
} NfsNodeRec, *NfsNode;

#define SERP_ARGS(node) ((node)->serporid.serporid_u.serporid.arg_u)
#define SERP_ATTR(node) ((node)->serporid.serporid_u.serporid.attributes)
#define SERP_FILE(node) ((node)->serporid.serporid_u.serporid.file)

#ifdef TSILLDEBUG
NfsNode		 dbgRoot=0;
RpcUdpServer dbgSrv=0;
#endif

static RpcUdpXactPool smallPool = 0;
static RpcUdpXactPool bigPool   = 0;

extern struct _rtems_filesystem_operations_table		nfs_fs_ops;
extern struct _rtems_filesystem_file_handlers_r	nfs_file_file_handlers;
extern struct _rtems_filesystem_file_handlers_r	nfs_dir_file_handlers;

Nfs
nfsCreate(RpcUdpServer server)
{
Nfs rval = malloc(sizeof(*rval));

	if (rval) {
		rval->server     = server;
		rval->nodesInUse = 0;
	}
	return rval;
}

void
nfsDestroy(Nfs nfs)
{
	if (!nfs)
		return;
	rpcUdpServerDestroy(nfs->server);
	free(nfs);
}

static NfsNode
nfsNodeCreate(Nfs nfs, nfs_fh *fh)
{
NfsNode	rval = malloc(sizeof(*rval));

#if DEBUG & DEBUG_TRACK_NODES
	fprintf(stderr,"NFS: creating a node\n");
#endif

	if (rval) {
		if (fh)
			memcpy( &SERP_FILE(rval), fh, sizeof(*fh) );
		rval->nfs = nfs;
		LOCK(nfsStats.lock);
		nfs->nodesInUse++;
		UNLOCK(nfsStats.lock);
	} else {
		errno = ENOMEM;
	}

	return rval;
}

/* get a transaction for a mounted nfs
 * a limited number are hold in the xbox;
 * if more are needed, they are created
 * on the fly.
 */

static void
nfsNodeDestroy(NfsNode node)
{
#if DEBUG & DEBUG_TRACK_NODES
	fprintf(stderr,"NFS: destroying a node\n");
#endif
#if 0
	if (!node)
		return;
	/* this probably does nothing... */
  	xdr_free(xdr_serporid, &node->serporid);
#endif

	LOCK(nfsStats.lock);
	node->nfs->nodesInUse--;
	UNLOCK(nfsStats.lock);

	free(node);
}

void
nfsInit(int smallPoolDepth, int bigPoolDepth)
{  
entry	dummy;

	if (0==smallPoolDepth)
		smallPoolDepth = 20;
	if (0==bigPoolDepth)
		bigPoolDepth   = 10;

	/* it's crucial to zero out the 'next' pointer
	 * because it terminates the xdr_entry recursion
	 */

	dummy.nextentry = 0;
	dirres_entry_size = xdr_sizeof(xdr_entry, &dummy);

	assert( smallPool = rpcUdpXactPoolCreate(
							NFS_PROGRAM,
							NFS_VERSION_2,
							CONFIG_NFS_SMALL_XACT_SIZE,
							smallPoolDepth) );

	assert( bigPool   = rpcUdpXactPoolCreate(
							NFS_PROGRAM,
							NFS_VERSION_2,
							CONFIG_NFS_BIG_XACT_SIZE,
							bigPoolDepth) );

	assert( RTEMS_SUCCESSFUL == rtems_semaphore_create(
							rtems_build_name('N','F','S','s'),
							1,
							MUTEX_ATTRIBUTES,
							0,
							&nfsStats.lock) );
}

/* this should probably be called with the lock held */
void
nfsCleanup(void)
{
rtems_id l;

	rpcUdpXactPoolDestroy(smallPool);
	rpcUdpXactPoolDestroy(bigPool);
	l = nfsStats.lock;
	rtems_semaphore_delete(l);
	nfsStats.lock = 0;
}

static void
_cexpModuleInitialize(void *mod)
{	
	nfsInit(0,0);
}

static int
_cexpModuleFinalize(void *mod)
{	
int refuse;


	LOCK(nfsStats.lock);
	if (refuse = nfsStats.mounted_fs) {
		UNLOCK(nfsStats.lock);
		fprintf(stderr,"Refuse to unload NFS; %i filesystems still mounted\n",
						refuse);
		return -1;
	}
	/* hold the lock while cleaning up... */
	nfsCleanup();
	return 0;
}

/*
 * File system types
 */

#if 0 /* for reference */

struct rtems_filesystem_location_info_tt
{
   void                                 *node_access;
   rtems_filesystem_file_handlers_r     *handlers;
   rtems_filesystem_operations_table    *ops;
   rtems_filesystem_mount_table_entry_t *mt_entry;
};

#endif


int
nfscallSmall(
	RpcUdpServer	srvr,
	int				proc,
	xdrproc_t		xargs,
	void *			pargs,
	xdrproc_t		xres,
	void *			pres)
{
RpcUdpXact		xact;
enum clnt_stat	stat;
int				rval=-1;
 
	xact = rpcUdpXactPoolGet(smallPool, XactGetCreate);

	if ( RPC_SUCCESS != (stat=rpcUdpSend(
								xact,
								srvr,
								NFSCALL_TIMEOUT,
								proc,
								xres,
								pres,
								xargs,
								pargs,
								0)) ||
	     RPC_SUCCESS != (stat=rpcUdpRcv(xact)) ) {

		fprintf(stderr,"NFS - %s\n",clnt_sperrno(stat));

		switch (stat) {
			/* TODO: this is probably not complete and/or fully accurate */
			case RPC_CANTENCODEARGS : errno = EINVAL;	break;
			case RPC_AUTHERROR  	: errno = EPERM;	break;

			case RPC_CANTSEND		:
			case RPC_CANTRECV		: /* hope they have errno set */
			case RPC_SYSTEMERROR	: break;

			default             	: errno = EIO;		break;
		}
	} else {
		rval = 0;
	}

	/* release the transaction back into the pool */
	rpcUdpXactPoolPut(xact);

	return rval;
}

/* initialize a sockaddr_in from a
 * "<host>':'<path>" string and let
 * pPath point to the <path> part
 *
 * RETURNS: 0 on success, -1 on failure with errno set
 */
static int
buildIpAddr(char **pPath, struct sockaddr_in *psa)
{
char	host[30];
char	*chpt = *pPath;
char	*path;
int		len;

	/* split the device name which is in the form
	 *
	 * <host_ip> ':' <path>
	 *
	 * into its components using a local buffer
	 */

	if ( !(chpt) ||
		 !(path = strchr(chpt, HOSTDELIM)) ||
	      (len  = path - chpt) >= sizeof(host) - 1 ) {
		errno = EINVAL;
		return -1;
	}
	/* point to path beyond ':' */
	path++;

	strncpy(host, chpt, len);
	host[len]=0;

	if ( ! inet_aton(host, &psa->sin_addr) ) {
		errno = ENXIO;
		return -1;
	}

	psa->sin_family = AF_INET;
	psa->sin_port   = 0;
	*pPath   = path;
	return 0;
}

enum clnt_stat
mntcall(
	struct sockaddr_in	*psrvr,
	int					proc,
	xdrproc_t			xargs,
	void *				pargs,
	xdrproc_t			xres,
	void *				pres)
{
RpcUdpClnt			clp;
int					retry;
enum clnt_stat		stat;

#ifdef MOUNT_V1_PORT
	/* if the portmapper fails, retry a fixed port */
	for (retry = 1, clp = 0;
		 retry >= 0 && !clp;
		 saddr.sin_port = htons(MOUNT_V1_PORT), retry-- )
#endif
		clp  = rpcUdpClntCreate(
				psrvr,
				MOUNT_PROGRAM,
				MOUNT_V1,
				MNTCALL_RETRYPERIOD);

	if (!clp) {
		fprintf(stderr,
				"Unable to create MOUNT client - invalid server/port?\n");
		return RPC_UNKNOWNPROTO;
	}

	stat = rpcUdpClntCall(
					clp,
					proc,
					xargs, pargs,
					xres,  pres,
					MNTCALL_TIMEOUT);

	rpcUdpClntDestroy(clp);

	return stat;
}
	

#if 0
int
lk1(char *name)
{
diropres			res;
fattr				*f;
enum clnt_stat		stat;
extern diropargs dbg_diropargs;


	dbg_diropargs.name=name;
	if (nfscallSmall(dbgSrv,
				NFSPROC_LOOKUP,
				xdr_diropargs, &dbg_diropargs,
				xdr_diropres,  &res
				)) {
		fprintf(stderr,"Error %s\n", strerror(errno));
		goto cleanup;
	}

	if (res.status) {
		fprintf(stderr,"NFS Error %i (?%s?)\n", res.status, strerror(res.status));
		goto cleanup;
	}
	f = &res.diropres_u.diropres.attributes;
	fprintf(stderr,"type   %i\n", f->type);
	fprintf(stderr,"mode   %i\n", f->mode);
	fprintf(stderr,"nlink  %i\n", f->nlink);
	fprintf(stderr,"fileid %i\n", f->fileid);
	fprintf(stderr,"uid    %i\n", f->uid);
	if (NFDIR == f->type)
		memcpy( &dbg_diropargs.dir,
				&res.diropres_u.diropres.file,
				sizeof(dbg_diropargs.dir));

cleanup:
	xdr_free(xdr_diropres,(void*)&res);

}
#endif

static inline int
locIsRoot(rtems_filesystem_location_info_t *l)
{
NfsNode me = (NfsNode) l->node_access;
NfsNode r;
	r = (NfsNode)l->mt_entry->mt_fs_root.node_access;
	return SERP_ATTR(r).fileid == SERP_ATTR(me).fileid &&
		   SERP_ATTR(r).fsid   == SERP_ATTR(me).fsid;
}

/*
 * rtems_filesystem_freenode_t must be called by the generic after
 * calling this routine
 */


STATIC int nfs_evalpath(
	const char                        *pathname,      /* IN     */
	int                                flags,         /* IN     */
	rtems_filesystem_location_info_t  *pathloc        /* IN/OUT */
)
{
char			*del, *part;
int				e;
NfsNode			node   = 0;
char			*p     = strdup(pathname);
Nfs				nfs    = (Nfs)pathloc->mt_entry->fs_info;
RpcUdpServer	server = nfs->server;

	if ( !p ) {
		e = ENOMEM;
		goto cleanup;
	}

	/* copy the start node (we must copy the stats also,
	 * since the root node itself might be looked up...
	 */
 	if ( ! (node = nfsNodeCreate(nfs,0)) ) {
		e = ENOMEM;
		goto cleanup;
	}
	memcpy(node, pathloc->node_access, sizeof(*node));

	pathloc->node_access = node;

	for (part=p; part && *part; part=del) {
		/* find delimiter and eat /// sequences */
		if ((del = strchr(part, DELIM))) {
			do {
				*del++=0;
			} while (DELIM==*del);
		}

		/* cross mountpoint upwards */
		if (0==strcmp(part,UPDIR) && locIsRoot(pathloc)) {

			rtems_filesystem_location_info_t *mp_node;

			mp_node = &pathloc->mt_entry->mt_point_node;

			nfsNodeDestroy(node);

			*pathloc = *mp_node;

			/* re-append the rest of the path */
			if (del) {
				while (0==*--del)
					*del=DELIM;
			}
#if DEBUG & DEBUG_EVALPATH
			fprintf(stderr,
					"Sending '%s' for evaluation across mount point\n",
					part);
#endif

			return mp_node->ops->evalpath_h(part, flags, pathloc);
		}

		/* lookup one element */
		SERP_ARGS(node).diroparg.name = part;

#if DEBUG & DEBUG_EVALPATH
		fprintf(stderr,"Looking up '%s'\n",part);
#endif
		if (nfscallSmall(server,
						 NFSPROC_LOOKUP,
						 xdr_diropargs, &SERP_FILE(node),
						 xdr_serporid,  &node->serporid)) {
			e = errno ? errno : EIO;
			goto cleanup;
		} else {
			if (e = node->serporid.status)
				goto cleanup;
		}
		
	}

#ifdef TSILLDEBUG
	fprintf(stderr,"pAttr 0x%08x Type %i, ino %i, fsid %i, rdev %i mode 0%o\n",
					&SERP_ATTR(node),
					SERP_ATTR(node).type,
					SERP_ATTR(node).fileid,
					SERP_ATTR(node).fsid,
					SERP_ATTR(node).rdev,
					SERP_ATTR(node).mode);
#endif

	if (locIsRoot(pathloc)) {
		/* stupid filesystem code has no 'op' for comparing nodes
		 * but just compares the 'node_access' pointers.
		 * Luckily, this is only done for comparing the root nodes.
		 * Hence, we never give them a copy of the root but always
		 * the root itself.
		 */
		pathloc->node_access = pathloc->mt_entry->mt_fs_root.node_access;
		/* increment the 'in use' counter since we return one more
		 * reference to the root node
		 */
		LOCK(nfsStats.lock);
			nfs->nodesInUse++;
		UNLOCK(nfsStats.lock);
		nfsNodeDestroy(node);
	} else {
		switch (SERP_ATTR(node).type) {
			case NFDIR:	pathloc->handlers = &nfs_dir_file_handlers;  break;
			case NFREG:	pathloc->handlers = &nfs_file_file_handlers; break;
			default: 	pathloc->handlers = &rtems_filesystem_null_handlers; break;
		}
		pathloc->node_access = node;
	}
	node = 0;

	e = 0;

cleanup:
	free(p);
	if (node) {
		nfsNodeDestroy(node);
		pathloc->node_access = 0;
	}
#if DEBUG & DEBUG_COUNT_NODES
	fprintf(stderr,"leaving evalpath, in use count is %i\n",nfs->nodesInUse);
#endif
	if (e)
		rtems_set_errno_and_return_minus_one(e);
	else
		return 0;
}

/* MANDATORY; may set errno=ENOSYS and return -1 */
static int nfs_evalformake(
	const char                       *path,       /* IN */
	rtems_filesystem_location_info_t *pathloc,    /* IN/OUT */
	const char                      **name        /* OUT    */
)
{
	/* leave pathloc alone */
	rtems_set_errno_and_return_minus_one(ENOSYS);
}

#ifdef DECLARE_BODY
/* OPTIONAL; may be NULL */
static int nfs_link(
	rtems_filesystem_location_info_t  *to_loc,      /* IN */
	rtems_filesystem_location_info_t  *parent_loc,  /* IN */
	const char                        *name         /* IN */
)DECLARE_BODY
#else
#define nfs_link 0
#endif

static int nfs_unlink(
	rtems_filesystem_location_info_t  *loc       /* IN */
)
{
nfsstat			status;
NfsNode			parent;
struct dirent	*pent;
struct stat		sb;
NfsNode			node = loc->node_access;
Nfs				nfs  = node->nfs;
int				rval = -1;
DIR				*dp  = 0;

	/* We have to lookup pathloc in the parent directory
	 * to find its name - NFS needs the parent node + the
	 * child's name :-(
	 */

	/* The FS generics have determined that pathloc is _not_
	 * a directory. Hence we may assume that the parent
	 * is in our NFS
	 */
	if ( !(dp = opendir(".")) )
		return -1;

	/* now we must search the parent dir */
	while ( (pent = readdir(dp)) ) {
#if DEBUG & DEBUG_UNLINK
		fprintf(stderr,"\nunlink: checking %s",pent->d_name);
#endif
		if (pent->d_ino == (ino_t) SERP_ATTR(node).fileid) {
#if DEBUG & DEBUG_UNLINK
			fprintf(stderr," ...inodes match");
#endif
#ifndef	INO_T_IS_4_BYTES
			/* we must stat the entry and compare the device
			 * entries as well :-(
			 */
			if (stat(pent->d_name, &sb))
				goto cleanup;
			if ( sb.st_dev != NFS_MAKE_DEV_T(node) ) {
#if DEBUG & DEBUG_UNLINK
				fprintf(stderr,"...device mismatch\n");
#endif
				continue;
			}
#endif
			/* ok; this seems to be the correct one */
#if DEBUG & DEBUG_UNLINK
			fprintf(stderr,"...OK\n");
#endif
			break;
		}
	}

	if (pent) {
		/* move 'pathloc', 'nfs' and 'node' to the parent */
		loc  = & rtems_libio_iop(dp->dd_fd)->pathinfo;
		node = loc->node_access;
		nfs  = node->nfs;

		SERP_ARGS(node).diroparg.name = pent->d_name;

		if ( nfscallSmall(nfs->server,
						NFSPROC_REMOVE,
						xdr_diropargs,	&SERP_FILE(node),
						xdr_nfsstat,	&status) ) {
			if (!errno)
				errno = EIO;
		} else if ( NFS_OK != status ) {
				errno = status;
		} else {
			rval = 0;
		}
	}

cleanup:
	if (dp)
		closedir(dp);
	return rval;

}

#ifdef DECLARE_BODY
/* OPTIONAL; may be NULL */
static int nfs_chown(
	rtems_filesystem_location_info_t  *pathloc,       /* IN */
	uid_t                              owner,         /* IN */
	gid_t                              group          /* IN */
)DECLARE_BODY
#else
#define nfs_chown 0
#endif

/* Cleanup the FS private info attached to pathloc->node_access */
static int nfs_freenode(
	rtems_filesystem_location_info_t      *pathloc       /* IN */
)
{
Nfs	nfs    = ((NfsNode)pathloc->node_access)->nfs;

	/* never destroy the root node; it is released by the unmount
	 * code
	 */
	if (locIsRoot(pathloc)) {
		/* just adjust the references to the root node but
		 * don't really release it
		 */
		LOCK(nfsStats.lock);
			nfs->nodesInUse--;
		UNLOCK(nfsStats.lock);
	} else {
		nfsNodeDestroy(pathloc->node_access);
		pathloc->node_access = 0;
	}
#if DEBUG & DEBUG_COUNT_NODES
	fprintf(stderr,"leaving freenode, in use count is %i\n",nfs->nodesInUse);
#endif
	return 0;
}

#ifdef DECLARE_BODY
/* This routine is called when they try to mount something
 * on top of THIS filesystem, i.e. if one of our directories
 * is used as a mount point
 */
static int nfs_mount(
	rtems_filesystem_mount_table_entry_t *mt_entry     /* in */
)DECLARE_BODY
#else
#define nfs_mount 0
#endif

#if 0

/* for reference (libio.h) */

struct rtems_filesystem_mount_table_entry_tt {
  Chain_Node                             Node;
  rtems_filesystem_location_info_t       mt_point_node;
  rtems_filesystem_location_info_t       mt_fs_root;
  int                                    options;
  void                                  *fs_info;

  rtems_filesystem_limits_and_options_t  pathconf_limits_and_options;

  /*
   *  When someone adds a mounted filesystem on a real device,
   *  this will need to be used.
   *
   *  The best option long term for this is probably an open file descriptor.
   */
  char                                  *dev;
};
#endif


/* This op is called as the last step of mounting this FS */
STATIC int nfs_fsmount_me(
	rtems_filesystem_mount_table_entry_t *mt_entry
)
{
char				*host;
int					retry;
struct sockaddr_in	saddr;
enum clnt_stat		stat;
fhstatus			fhstat;
Nfs					nfs       = 0;
NfsNode				rootNode  = 0;
RpcUdpServer		nfsServer = 0;
int					e         = -1;
char				*path     = mt_entry->dev;


	host = path;
	if (buildIpAddr(&path, &saddr))
		return -1;


#ifdef NFS_V2_PORT
	/* if the portmapper fails, retry a fixed port */
	for (retry = 1;
		 retry >= 0 && !nfsServer;
		 saddr.sin_port = htons(NFS_V2_PORT), retry-- )
#endif
		nfsServer = rpcUdpServerCreate(
							&saddr,
							NFS_PROGRAM,
							NFS_VERSION_2,
							NFSCALL_RETRYPERIOD);

	if ( !nfsServer ) {
		fprintf(stderr,
				"Unable to contact NFS server - invalid port?\n");
		e = EPROTONOSUPPORT;
		goto cleanup;
	}

	/* first, try to ping the NFS server by
	 * calling the NULL proc.
	 */
	if (nfscallSmall(nfsServer,
					 NFSPROC_NULL,
					 xdr_void, 0,
					 xdr_void, 0)) {

		fputs("NFS Ping ",stderr);
		fwrite(host, 1, path-host-1, stderr);
		fprintf(stderr," failed: %s\n", strerror(errno));

		e = errno ? errno : EIO;
		goto cleanup;
	}


	/* that seemed to work - we now try the
	 * actual mount
	 */

	/* reuse server address but let the mntcall()
	 * search for the mountd's port
	 */
	saddr.sin_port = 0;

	stat = mntcall( &saddr,
					MOUNTPROC_MNT,
					xdr_dirpath,
					&path,
					xdr_fhstatus,
					&fhstat );

	if (stat) {
		fprintf(stderr,"MOUNT -- %s\n",clnt_sperrno(stat));
		if ( e<=0 )
			e = EIO;
		goto cleanup;
	} else if (NFS_OK != (e=fhstat.fhs_status)) {
		fprintf(stderr,"MOUNT: %s\n",strerror(e));
		goto cleanup;
	}

#ifdef TSILLDEBUG
	dbgSrv = nfsServer;
#endif

	assert( nfs = nfsCreate(nfsServer) );
	nfsServer = 0;

	/* that seemed to work - we now create the root node
	 * and we also must obtain the root node attributes
	 */
	assert( rootNode = nfsNodeCreate(nfs, (nfs_fh*)&fhstat.fhstatus_u.fhs_fhandle ) );

	if ( nfscallSmall(  nfs->server,
						NFSPROC_GETATTR,
						xdr_nfs_fh,   &fhstat.fhstatus_u.fhs_fhandle,
						xdr_attrstat, &rootNode->serporid) ) {
		e = errno ? errno : EIO;
		goto cleanup;
	} else if ( e = rootNode->serporid.status )
		goto cleanup;

	/* looks good so far */

#ifdef TSILLDEBUG
	dbgRoot = rootNode;
#endif
	mt_entry->mt_fs_root.node_access = rootNode;

	rootNode = 0;

	mt_entry->mt_fs_root.ops		 = &nfs_fs_ops;
	mt_entry->mt_fs_root.handlers	 = &nfs_dir_file_handlers;
	mt_entry->pathconf_limits_and_options = nfs_limits_and_options;

	LOCK(nfsStats.lock);
		nfsStats.mounted_fs++;
		/* allocate a new ID for this FS */
		nfs->id = nfsStats.fs_ids++;
	UNLOCK(nfsStats.lock);

	mt_entry->fs_info				 = nfs;
	nfs = 0;

	e = 0;

cleanup:
	if (nfs)
		nfsDestroy(nfs);
	if (nfsServer)
		rpcUdpServerDestroy(nfsServer);
	if (rootNode)
		nfsNodeDestroy(rootNode);
	if (e)
		rtems_set_errno_and_return_minus_one(e);
	else
		return 0;
}

#ifdef DECLARE_BODY
/* This op is called when they try to unmount a FS
 * from a mountpoint managed by THIS FS.
 */
static int nfs_unmount(
	rtems_filesystem_mount_table_entry_t *mt_entry     /* in */
)DECLARE_BODY
#else
#define nfs_unmount 0
#endif

/* This op is called when they try to unmount THIS fs */
STATIC int nfs_fsunmount_me(
	rtems_filesystem_mount_table_entry_t *mt_entry    /* in */
)
{
enum clnt_stat		stat;
struct sockaddr_in	saddr;
char				*path = mt_entry->dev;
int					nodesInUse;

	LOCK(nfsStats.lock);
		nodesInUse = ((Nfs)mt_entry->fs_info)->nodesInUse;
	UNLOCK(nfsStats.lock);

	if (nodesInUse > 1 /* one ref to the root node used by us */) {
		fprintf(stderr,"Refuse to unmount; there are still %i nodes in use (1 used by us)", nodesInUse);
		rtems_set_errno_and_return_minus_one(EBUSY);
	}

	assert( 0 == buildIpAddr(&path,&saddr) );
	
	stat = mntcall( &saddr,
					MOUNTPROC_UMNT,
					xdr_dirpath, &path,
					xdr_void,	 0 );

	if (stat) {
		fprintf(stderr,"NFS UMOUNT -- %s\n", clnt_sperrno(stat));
		errno = EIO;
		return -1;
	}

	nfsNodeDestroy(mt_entry->mt_fs_root.node_access);
	mt_entry->mt_fs_root.node_access = 0;
	
	nfsDestroy(mt_entry->fs_info);
	mt_entry->fs_info = 0;

	LOCK(nfsStats.lock);
		nfsStats.mounted_fs--;
	UNLOCK(nfsStats.lock);

	return 0;
}

/* OPTIONAL; may be NULL - BUT: CAUTION; mount() doesn't check
 * for this handler to be present - a fs bug
 */
static rtems_filesystem_node_types_t nfs_node_type(
	rtems_filesystem_location_info_t    *pathloc      /* in */
)
{
NfsNode node = pathloc->node_access;
	switch( SERP_ATTR(node).type ) {
		default:
			/* rtems has no value for 'unknown';
			 */
		case NFNON:
		case NFSOCK:
		case NFBAD:
		case NFFIFO:
				break;


		case NFREG: return RTEMS_FILESYSTEM_MEMORY_FILE;
		case NFDIR:	return RTEMS_FILESYSTEM_DIRECTORY;

		case NFBLK:
		case NFCHR:	return RTEMS_FILESYSTEM_DEVICE;

		case NFLNK: return RTEMS_FILESYSTEM_SYM_LINK;
	}
	return -1;
}

/*
 *  XXX
 *  This routine does not allocate any space and rtems_filesystem_freenode_t 
 *  is not called by the generic after calling this routine.
 *  ie. node_access does not have to contain valid data when the 
 *  routine returns.
 */

#ifdef DECLARE_BODY
/* OPTIONAL; may be NULL */
static int nfs_mknod(
	const char                        *path,       /* IN */
	mode_t                             mode,       /* IN */
	dev_t                              dev,        /* IN */
	rtems_filesystem_location_info_t  *pathloc     /* IN/OUT */
)DECLARE_BODY
#else
#define nfs_mknod 0
#endif

#ifdef DECLARE_BODY
static int nfs_utime(
	rtems_filesystem_location_info_t  *pathloc,       /* IN */
	time_t                             actime,        /* IN */
	time_t                             modtime        /* IN */
)DECLARE_BODY
#else
#define nfs_utime 0
#endif

#ifdef DECLARE_BODY
static int nfs_symlink(
	rtems_filesystem_location_info_t  *loc,         /* IN */
	const char                        *link_name,   /* IN */
	const char                        *node_name
)DECLARE_BODY
#else
#define nfs_symlink 0
#endif

typedef struct readlinkres_strbuf {
	nfsstat	status;
	strbuf	strbuf;
} readlinkres_strbuf;

static bool_t
xdr_readlinkres_strbuf(XDR *xdrs, readlinkres_strbuf *objp)
{
	if ( !xdr_nfsstat(xdrs, &objp->status) )
		return FALSE;

	if ( NFS_OK == objp->status ) {
		if ( !xdr_string(xdrs, &objp->strbuf.buf, objp->strbuf.max) )
			return FALSE;
	}
	return TRUE;
}

static int nfs_do_readlink(
	rtems_filesystem_location_info_t  *loc,     	/* IN  */       
	strbuf							  *psbuf		/* IN/OUT */
)
{
NfsNode				node = loc->node_access;
Nfs					nfs  = node->nfs;
readlinkres_strbuf	rr;
int					wasAlloced;
int					rval;

	rr.strbuf  = *psbuf;

	wasAlloced = (0 == psbuf->buf);

	if ( (rval = nfscallSmall(nfs->server,
							NFSPROC_READLINK,
							xdr_nfs_fh,      		&SERP_FILE(node),
							xdr_readlinkres_strbuf, &rr)) ) {
		if ( !errno )
			errno = EIO;
		if (wasAlloced)
			xdr_free( xdr_strbuf, (caddr_t)&rr.strbuf );
	}


	if (NFS_OK != rr.status) {
		if (wasAlloced)
			xdr_free( xdr_strbuf, (caddr_t)&rr.strbuf );
		rtems_set_errno_and_return_minus_one(rr.status);
	}

	*psbuf = rr.strbuf;

	return 0;
}

static int nfs_readlink(
	rtems_filesystem_location_info_t  *loc,     	/* IN  */       
	char							  *buf,			/* OUT */
	size_t							  len
)
{
strbuf sbuf;
	sbuf.buf = buf;
	sbuf.max = len;

	return nfs_do_readlink(loc, &sbuf);
}

static int nfs_evaluate_link(
	rtems_filesystem_location_info_t *pathloc,     /* IN/OUT */
	int                               flags        /* IN     */
)
{
int	 								rval;
strbuf								sbuf;
rtems_filesystem_location_info_t	locbuf;

	/* let XDR allocate the proper string length */
	sbuf.buf = 0;
	sbuf.max = NFS_MAXPATHLEN;

	/* assume the generics have verified 'pathloc' to be
	 * a link...
	 */
	if ( nfs_do_readlink(pathloc, &sbuf) ) {
		rval = -1;
	} else {
		/* evaluate path will allocate a new node, hence we must remember
		 * the current one and free it eventually.
		 */
		locbuf = *pathloc;
		rval = rtems_filesystem_evaluate_path(sbuf.buf, flags, pathloc, 1);
		rtems_filesystem_freenode(&locbuf);
	}

	xdr_free(xdr_strbuf, (caddr_t)&sbuf);
	return rval;
}


struct _rtems_filesystem_operations_table nfs_fs_ops = {
		nfs_evalpath,		/* MANDATORY */
		nfs_evalformake,	/* MANDATORY; may set errno=ENOSYS and return -1 */
		nfs_link,			/* OPTIONAL; may be NULL */
		nfs_unlink,			/* OPTIONAL; may be NULL */
		nfs_node_type,		/* OPTIONAL; may be NULL; BUG in mount - no test!! */
		nfs_mknod,			/* OPTIONAL; may be NULL */
		nfs_chown,			/* OPTIONAL; may be NULL */
		nfs_freenode,		/* OPTIONAL; may be NULL; (release node_access) */
		nfs_mount,			/* OPTIONAL; may be NULL */
		nfs_fsmount_me,		/* OPTIONAL; may be NULL -- but this makes NO SENSE */
		nfs_unmount,		/* OPTIONAL; may be NULL */
		nfs_fsunmount_me,	/* OPTIONAL; may be NULL */
		nfs_utime,			/* OPTIONAL; may be NULL */
		nfs_evaluate_link,	/* OPTIONAL; may be NULL */
		nfs_symlink,		/* OPTIONAL; may be NULL */
		nfs_readlink,		/* OPTIONAL; may be NULL */
};

#if 0
/* from rtems/libio.h for convenience */
struct rtems_libio_tt {
		rtems_driver_name_t              *driver;
		off_t                             size;      /* size of file */
		off_t                             offset;    /* current offset into file */
		unsigned32                        flags;
		rtems_filesystem_location_info_t  pathinfo;
		Objects_Id                        sem;
		unsigned32                        data0;     /* private to "driver" */
		void                             *data1;     /* ... */
		void                             *file_info; /* used by file handlers */
		rtems_filesystem_file_handlers_r *handlers;  /* type specific handlers */
};
#endif

/*
 *  File Handler Operations Table
 */

static int nfs_file_open(
	rtems_libio_t *iop,
	const char    *pathname,
	unsigned32     flag,
	unsigned32     mode
)
{
	iop->file_info = 0;
	return 0;
}

static int nfs_dir_open(
	rtems_libio_t *iop,
	const char    *pathname,
	unsigned32     flag,
	unsigned32     mode
)
{
NfsNode		node = iop->pathinfo.node_access;
DirInfo		di;

	/* create a readdirargs object and copy the file handle;
	 * attach to the file_info.
	 */

	di = (DirInfo) malloc(sizeof(*di));
	iop->file_info = di;

	if ( !di  ) {
		errno = ENOMEM;
		return -1;
	}

	memcpy( &di->readdirargs.dir,
			&SERP_FILE(node),
			sizeof(di->readdirargs.dir) );

	/* rewind cookie */
	memset( &di->readdirargs.cookie,
	        0,
	        sizeof(di->readdirargs.cookie) );

	di->eofreached = FALSE;

	return 0;
}

static int nfs_file_close(
	rtems_libio_t *iop
)
{
	return 0;
}

static int nfs_dir_close(
	rtems_libio_t *iop
)
{
	free(iop->file_info);
	iop->file_info = 0;
	return 0;
}

static int nfs_file_read(
	rtems_libio_t *iop,
	void          *buffer,
	unsigned32     count
)
{
NfsNode node = iop->pathinfo.node_access;
Nfs		nfs  = node->nfs;
readres	rr;

	if (count > UDPMSGSIZE)
		count = UDPMSGSIZE;

	SERP_ARGS(node).readarg.offset		= iop->offset;
	SERP_ARGS(node).readarg.count	  	= count;
	SERP_ARGS(node).readarg.totalcount	= 0xdeadbeef;

	rr.readres_u.reply.data.data_val	= buffer;

	if ( nfscallSmall(	nfs->server,
						NFSPROC_READ,
						xdr_readargs,	&SERP_FILE(node),
						xdr_readres,	&rr) ) {
		if ( !errno )
			errno = EIO;
		return -1;
	}


	if (NFS_OK != rr.status) {
		rtems_set_errno_and_return_minus_one(rr.status);
	}

	return rr.readres_u.reply.data.data_len;
}

/* this is called by readdir() / getdents() */
static int nfs_dir_read(
	rtems_libio_t *iop,
	void          *buffer,
	unsigned32     count
)
{
int				i;
DirInfo			di     = iop->file_info;
RpcUdpServer	server = ((Nfs)iop->pathinfo.mt_entry->fs_info)->server;

	if ( di->eofreached )
		return 0;

	di->ptr = di->buf = buffer;

	/* align + round down the buffer */
	count &= ~ (DIRENT_HEADER_SIZE - 1);
	di->len = count;

#if 0
	/* now estimate the number of entries we should ask for */
	count /= DIRENT_HEADER_SIZE + AVG_NAMLEN;

	/* estimate the encoded size that might take up */
	count *= dirres_entry_size + AVG_NAMLEN;
#else
	/* integer arithmetics are better done the other way round */
	count *= dirres_entry_size + AVG_NAMLEN;
	count /= DIRENT_HEADER_SIZE + AVG_NAMLEN;
#endif

	if (count > UDPMSGSIZE)
		count = UDPMSGSIZE;

	di->readdirargs.count = count;

#if DEBUG & DEBUG_READDIR
	fprintf(stderr,
			"Readdir: asking for %i XDR bytes, buffer is %i\n",
			count, di->len);
#endif

	if ( nfscallSmall(
					server,
					NFSPROC_READDIR,
					xdr_readdirargs, &di->readdirargs,
					xdr_dir_info,    di) ) {
		if ( !errno )
			errno = EIO;
		return -1;
	}


	if (NFS_OK != di->status) {
		rtems_set_errno_and_return_minus_one(di->status);
	}

	return (char*)di->ptr - (char*)buffer;
}

#ifdef DECLARE_BODY
static int nfs_file_write(
	rtems_libio_t *iop,
	const void    *buffer,
	unsigned32    count
)DECLARE_BODY
#else
#define nfs_file_write 0
#define nfs_dir_write 0
#endif

#ifdef DECLARE_BODY
static int nfs_file_ioctl(
	rtems_libio_t *iop,
	unsigned32     command,
	void          *buffer
)DECLARE_BODY
#else
#define nfs_file_ioctl 0
#define nfs_dir_ioctl 0
#endif

static int nfs_file_lseek(
	rtems_libio_t *iop,
	off_t          length,
	int            whence
)
{
	/* this is particularly easy :-) */
	return 0;
}

static int nfs_dir_lseek(
	rtems_libio_t *iop,
	off_t          length,
	int            whence
)
{
DirInfo di = iop->file_info;

	if (SEEK_SET != whence || 0 != length) {
		errno = ENOTSUP;
		return -1;
	}

	/* rewind cookie */
	memset( &di->readdirargs.cookie,
	        0,
	        sizeof(di->readdirargs.cookie) );

	di->eofreached = FALSE;

	return 0;
}


#if 0	/* structure types for reference */
struct fattr {
		ftype type;
		u_int mode;
		u_int nlink;
		u_int uid;
		u_int gid;
		u_int size;
		u_int blocksize;
		u_int rdev;
		u_int blocks;
		u_int fsid;
		u_int fileid;
		nfstime atime;
		nfstime mtime;
		nfstime ctime;
};

struct  stat
{
		dev_t     st_dev;
		ino_t     st_ino;
		mode_t    st_mode;
		nlink_t   st_nlink;
		uid_t     st_uid;
		gid_t     st_gid;
		dev_t     st_rdev;
		off_t     st_size;
		/* SysV/sco doesn't have the rest... But Solaris, eabi does.  */
#if defined(__svr4__) && !defined(__PPC__) && !defined(__sun__)
		time_t    st_atime;
		time_t    st_mtime;
		time_t    st_ctime;
#else
		time_t    st_atime;
		long      st_spare1;
		time_t    st_mtime;
		long      st_spare2;
		time_t    st_ctime;
		long      st_spare3;
		long      st_blksize;
		long      st_blocks;
		long  st_spare4[2];
#endif
};
#endif

static int nfs_fstat(
	rtems_filesystem_location_info_t *loc,
	struct stat                      *buf
)
{
fattr *fa = &SERP_ATTR((NfsNode)loc->node_access);

/* done by caller 
	memset(buf, 0, sizeof(*buf));
 */

	/* translate */
	buf->st_dev		= NFS_MAKE_DEV_T((NfsNode)loc->node_access);
	buf->st_mode	= fa->mode;
	buf->st_nlink	= fa->nlink;
	buf->st_uid		= fa->uid;
	buf->st_gid		= fa->gid;
	buf->st_size	= fa->size;
	/* TODO: set to "preferred size" of this NFS client implementation */
	buf->st_blksize	= fa->blocksize;
	buf->st_rdev	= fa->rdev;
	buf->st_blocks	= fa->blocks;
	buf->st_ino     = fa->fileid;
	buf->st_atime	= fa->atime.seconds;
	buf->st_mtime	= fa->mtime.seconds;
	buf->st_ctime	= fa->ctime.seconds;

#if 0 /* NFS should return the modes */
	switch(fa->type) {
		default:
		case NFNON:
		case NFBAD:
				break;

		case NFSOCK: buf->st_mode |= S_IFSOCK; break;
		case NFFIFO: buf->st_mode |= S_IFIFO;  break;
		case NFREG : buf->st_mode |= S_IFREG;  break;
		case NFDIR : buf->st_mode |= S_IFDIR;  break;
		case NFBLK : buf->st_mode |= S_IFBLK;  break;
		case NFCHR : buf->st_mode |= S_IFCHR;  break;
		case NFLNK : buf->st_mode |= S_IFLNK;  break;
	}
#endif

	return 0;
}

#ifdef DECLARE_BODY
static int nfs_file_fchmod(
	rtems_filesystem_location_info_t *loc,
	mode_t                            mode
)DECLARE_BODY
#else
#define nfs_file_fchmod 0
#define nfs_dir_fchmod 0
#endif

#ifdef DECLARE_BODY
static int nfs_file_ftruncate(
	rtems_libio_t *iop,
	off_t          length
)DECLARE_BODY
#else
#define nfs_file_ftruncate 0
#define nfs_dir_ftruncate 0
#endif

#ifdef DECLARE_BODY
static int nfs_file_fpathconf(
	rtems_libio_t *iop,
	int name
)DECLARE_BODY
#else
#define nfs_file_fpathconf 0
#define nfs_dir_fpathconf 0
#endif

#ifdef DECLARE_BODY
static int nfs_file_fsync(
	rtems_libio_t *iop
)DECLARE_BODY
#else
#define nfs_file_fsync 0
#define nfs_dir_fsync 0
#endif

#ifdef DECLARE_BODY
static int nfs_file_fdatasync(
	rtems_libio_t *iop
)DECLARE_BODY
#else
#define nfs_file_fdatasync 0
#define nfs_dir_fdatasync 0
#endif

#ifdef DECLARE_BODY
static int nfs_file_fcntl(
	int            cmd,
	rtems_libio_t *iop
)DECLARE_BODY
#else
#define nfs_file_fcntl 0
#define nfs_dir_fcntl 0
#endif

#ifdef DECLARE_BODY
/* OPTIONAL; may be NULL */
static int nfs_file_rmnod(
	rtems_filesystem_location_info_t      *pathloc       /* IN */
)DECLARE_BODY
#else
#define nfs_file_rmnod 0
#define nfs_dir_rmnod 0
#endif

static
struct _rtems_filesystem_file_handlers_r nfs_file_file_handlers = {
		nfs_file_open,			/* OPTIONAL; may be NULL */
		nfs_file_close,			/* OPTIONAL; may be NULL */
		nfs_file_read,			/* OPTIONAL; may be NULL */
		nfs_file_write,			/* OPTIONAL; may be NULL */
		nfs_file_ioctl,			/* OPTIONAL; may be NULL */
		nfs_file_lseek,			/* OPTIONAL; may be NULL */
		nfs_fstat,				/* OPTIONAL; may be NULL */
		nfs_file_fchmod,			/* OPTIONAL; may be NULL */
		nfs_file_ftruncate,		/* OPTIONAL; may be NULL */
		nfs_file_fpathconf,		/* OPTIONAL; may be NULL - UNUSED */
		nfs_file_fsync,			/* OPTIONAL; may be NULL */
		nfs_file_fdatasync,		/* OPTIONAL; may be NULL */
		nfs_file_fcntl,			/* OPTIONAL; may be NULL */
		nfs_unlink,				/* OPTIONAL; may be NULL */
};

static
struct _rtems_filesystem_file_handlers_r nfs_dir_file_handlers = {
		nfs_dir_open,			/* OPTIONAL; may be NULL */
		nfs_dir_close,			/* OPTIONAL; may be NULL */
		nfs_dir_read,			/* OPTIONAL; may be NULL */
		nfs_dir_write,			/* OPTIONAL; may be NULL */
		nfs_dir_ioctl,			/* OPTIONAL; may be NULL */
		nfs_dir_lseek,			/* OPTIONAL; may be NULL */
		nfs_fstat,				/* OPTIONAL; may be NULL */
		nfs_dir_fchmod,			/* OPTIONAL; may be NULL */
		nfs_dir_ftruncate,		/* OPTIONAL; may be NULL */
		nfs_dir_fpathconf,		/* OPTIONAL; may be NULL - UNUSED */
		nfs_dir_fsync,			/* OPTIONAL; may be NULL */
		nfs_dir_fdatasync,		/* OPTIONAL; may be NULL */
		nfs_dir_fcntl,			/* OPTIONAL; may be NULL */
		nfs_unlink,				/* OPTIONAL; may be NULL */
};
