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
#define CONFIG_NFS_SMALL_XACT_SIZE		UDPMSGSIZE
#define NFSCALL_TIMEOUT					_nfscalltimeout
#define MNTCALL_TIMEOUT					_nfscalltimeout
#define NFSCALL_RETRYPERIOD				_nfscallretry
#define MNTCALL_RETRYPERIOD				_nfscallretry

#undef  TSILLDEBUG
#define STATIC
#define DEBUG

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
	void		*buf;
	int			len;
} DirInfoRec, *DirInfo;

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
struct dirent	*pde = (struct dirent *)di->buf;
u_long			fileid;
char			*name;
register int	nlen,len;
nfscookie		*pcookie;

	len = di->len;

	if ( !xdr_u_int(xdrs, &fileid) )
		return FALSE;

	/* we must pass the address of a char* */
	name = (len > NFS_MAXNAMLEN) ? pde->d_name : dummy.nambuf;

	if ( !xdr_filename(xdrs, &name) )
		return FALSE;

	if (len >= 0) {
		len -= (nlen=strlen(name)) + 1;
		pcookie = di->readdirargs.cookie;
	} else {
		pcookie = dummy.cookie;
	}

	/* if the cookie goes into the DirInfo, we hope this doesn't fail
	 * - the caller ends up with an invalid readdirargs cookie otherwise...
	 */
	if ( !xdr_cookie(xdrs, pcookie) )
		return FALSE;

	di->len = len;
	/* adjust the buffer pointer */
	if (len >= 0) {
		pde->d_ino    = fileid;
		pde->d_namlen = nlen;
		if (name == dummy.nambuf) {
			memcpy(pde->d_name, dummy.nambuf, nlen + 1);
		}
		pde->d_reclen += nlen+1 /* string delimiter */ + 3 /* alignment */;
		/* word align */
		pde->d_reclen &= ~3;
		di->buf += pde->d_reclen;
	}

	return TRUE;
}

bool_t
xdr_dir_info(XDR *xdrs, DirInfo di)
{
DirInfo	dip;
int		good,decoded;
int		remaining;
bool_t	endof;

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
		if ( !xdr_pointer(xdrs, &dip, 0 /* size */, xdr_dir_info_entry) )
			return FALSE;
	}

	if ( ! xdr_bool(xdrs, &endof) )
		return FALSE;

	if (endof && (dip->len -= DIRENT_HEADER_SIZE) >= 0) {
		struct dirent *pde = (struct dirent *) di->buf;
		pde->d_ino    = 0;
		pde->d_reclen = DIRENT_HEADER_SIZE;
		pde->d_namlen = 0;
		dip->buf     += DIRENT_HEADER_SIZE;
	}

	return TRUE;
}

typedef struct NfsNodeRec_ {
	serporid	serporid;
} NfsNodeRec, *NfsNode;

/* Per mounted FS structure */
typedef struct NfsRec_ {
	RpcUdpServer	server;
} NfsRec, *Nfs;

#define SERP_ARGS(node) ((node)->serporid.serporid_u.serporid.arg_u)
#define SERP_ATTR(node) ((node)->serporid.serporid_u.serporid.attributes)
#define SERP_FILE(node) ((node)->serporid.serporid_u.serporid.file)

#ifndef TSILLDEBUG
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

	if (rval)
		rval->server = server;
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
nfsNodeCreate(nfs_fh *fh)
{
NfsNode	rval = malloc(sizeof(*rval));

fprintf(stderr,"TSILL creating a node\n");

	if (rval) {
		if (fh)
			memcpy( &SERP_FILE(rval), fh, sizeof(*fh) );
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
fprintf(stderr,"TSILL destroying a node\n");
#if 0
	if (!node)
		return;
	/* this probably does nothing... */
  	xdr_free(xdr_serporid, &node->serporid);
#endif

	free(node);
}

void
nfsInit(int smallPoolDepth, int bigPoolDepth)
{  
	if (0==smallPoolDepth)
		smallPoolDepth = 20;
	if (0==bigPoolDepth)
		bigPoolDepth   = 10;

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
}

void
nfsCleanup(void)
{
	rpcUdpXactPoolDestroy(smallPool);
	rpcUdpXactPoolDestroy(bigPool);
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


/*
 *  XXX
 *  This routine does not allocate any space and rtems_filesystem_freenode_t 
 *  is not called by the generic after calling this routine.
 *  ie. node_access does not have to contain valid data when the 
 *  routine returns.
 */


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

/*
 * rtems_filesystem_freenode_t must be called by the generic after
 * calling this routine
 */

static inline int
locIsRoot(rtems_filesystem_location_info_t *l)
{
NfsNode me = (NfsNode) l->node_access;
NfsNode r;
	r = (NfsNode)l->mt_entry->mt_fs_root.node_access;
	return SERP_ATTR(r).fileid == SERP_ATTR(me).fileid &&
		   SERP_ATTR(r).rdev   == SERP_ATTR(me).rdev;
}


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
RpcUdpServer	server = ((Nfs)pathloc->mt_entry->fs_info)->server;

	if ( !p ) {
		e = ENOMEM;
		goto cleanup;
	}

	/* copy the start node */
	memcpy(node, pathloc->node_access, sizeof(*node));
 	if ( ! (node = nfsNodeCreate( & SERP_FILE((NfsNode)pathloc->node_access))) ) {
		e = ENOMEM;
		goto cleanup;
	}

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

			return mp_node->ops->evalpath_h(part, flags, mp_node);
		}

		/* lookup one element */
		SERP_ARGS(node).diroparg.name = part;

fprintf(stderr,"Looking up '%s'\n",part);
		if (nfscallSmall(server,
						 NFSPROC_LOOKUP,
						 xdr_diropargs,
						 &SERP_FILE(node),
						 xdr_serporid,
						 &node->serporid)) {
fprintf(stderr,"Errout \n");
			e = errno ? errno : EIO;
			goto cleanup;
		} else {
			if (e = node->serporid.status)
				goto cleanup;
		}
		
	}

#ifndef TSILLDEBUG
	fprintf(stderr,"Type %i, ino %i, mode 0%o\n",
					SERP_ATTR(node).type,
					SERP_ATTR(node).fileid,
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

#ifdef DECLARE_BODY
/* OPTIONAL; may be NULL */
static int nfs_unlink(
	rtems_filesystem_location_info_t  *pathloc       /* IN */
)DECLARE_BODY
#else
#define nfs_unlink 0
#endif

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

	/* never destroy the root node; it is released by the unmount
	 * code
	 */
	if (locIsRoot(pathloc))
		return 0;
	nfsNodeDestroy(pathloc->node_access);
	pathloc->node_access = 0;
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

	/* that seemed to work - we now create the root node
	 * and we also must obtain the root node attributes
	 */
	assert( rootNode = nfsNodeCreate( (nfs_fh*)&fhstat.fhstatus_u.fhs_fhandle ) );

	if ( nfscallSmall(  nfsServer,
						NFSPROC_GETATTR,
						xdr_nfs_fh,   &fhstat.fhstatus_u.fhs_fhandle,
						xdr_attrstat, &rootNode->serporid) ) {
		e = errno ? errno : EIO;
		goto cleanup;
	} else if ( e = rootNode->serporid.status )
		goto cleanup;

	/* looks good so far */

#ifndef TSILLDEBUG
	dbgSrv = nfsServer;
#endif

	assert( nfs = nfsCreate(nfsServer) );
	nfsServer = 0;

#ifndef TSILLDEBUG
	dbgRoot = rootNode;
#endif
	mt_entry->mt_fs_root.node_access = rootNode;

	rootNode = 0;

	mt_entry->mt_fs_root.ops		 = &nfs_fs_ops;
	mt_entry->mt_fs_root.handlers	 = &nfs_dir_file_handlers;
	mt_entry->pathconf_limits_and_options = nfs_limits_and_options;
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
static int nfs_evaluate_link(
	rtems_filesystem_location_info_t *pathloc,     /* IN/OUT */
	int                               flags        /* IN     */
)DECLARE_BODY
#else
#define nfs_evaluate_link 0
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

#ifdef DECLARE_BODY
static int nfs_readlink(
	rtems_filesystem_location_info_t  *loc,     /* IN  */       
	char                              *buf,     /* OUT */       
	size_t                            bufsize    
)DECLARE_BODY
#else
#define nfs_readlink 0
#endif

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

#ifdef DECLARE_BODY
static int nfs_file_open(
	rtems_libio_t *iop,
	const char    *pathname,
	unsigned32     flag,
	unsigned32     mode
)DECLARE_BODY
#else
#define nfs_file_open 0
#endif

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

	return 0;
}

#ifdef DECLARE_BODY
static int nfs_file_close(
	rtems_libio_t *iop
)DECLARE_BODY
#else
#define nfs_file_close 0
#endif
static int nfs_dir_close(
	rtems_libio_t *iop
)
{
	free(iop->file_info);
	iop->file_info = 0;
	return 0;
}

#ifdef DECLARE_BODY
static int nfs_file_read(
	rtems_libio_t *iop,
	void          *buffer,
	unsigned32     count
)DECLARE_BODY
#else
#define nfs_file_read 0
#endif

/* this is called by readdir() / getdents() */
static int nfs_dir_read(
	rtems_libio_t *iop,
	void          *buffer,
	unsigned32     count
)
{
int			i;
DirInfo		*di          = iop->file_info;

	di->buf = buffer;
	di->len = count;
	word align + round down the buffer;

	decode();

	if (NFS_OK != di->status) {
		rtems_set_errno_and_return_minus_one(di->status);
	}

	return (char*)di->buf - (char*)buffer;
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

#ifdef DECLARE_BODY
static int nfs_file_lseek(
	rtems_libio_t *iop,
	off_t          length,
	int            whence
)DECLARE_BODY
#else
#define nfs_file_lseek 0
#endif

static int nfs_dir_lseek(
	rtems_libio_t *iop,
	off_t          length,
	int            whence
)
{
DirInfo *di = iop->file_info;

	if (SEEK_SET != whence || 0 != length)
		return -1;

	/* rewind cookie */
	memset( &di->readdirargs.cookie,
	        0,
	        sizeof(di->readdirargs.cookie) );
	return 0;

}

#if 0
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

static int nfs_dir_fstat(
	rtems_filesystem_location_info_t *loc,
	struct stat                      *buf
)
{
fattr *fa = &SERP_ATTR((NfsNode)loc->node_access);

	memset(buf, 0, sizeof(*buf));

	/* translate */
	buf->st_mode	= fa->mode;
	buf->st_nlink	= fa->nlink;
	buf->st_uid		= fa->uid;
	buf->st_gid		= fa->gid;
	buf->st_size	= fa->size;
	/* TODO: set to "preferred size" of this NFS client implementation */
	buf->st_blksize	= fa->blocksize;
	buf->st_rdev	= fa->rdev;
	buf->st_blocks	= fa->blocks;
	/* TODO: what's this one??:  u_int fsid; */
	buf->st_ino     = fa->fileid;
	buf->st_atime	= fa->atime.seconds;
	buf->st_mtime	= fa->mtime.seconds;
	buf->st_ctime	= fa->ctime.seconds;

fprintf(stderr,"TSILL: initial mode is 0x%x 0%o\n",fa->mode, fa->mode);

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

	return 0;
}
#define nfs_file_fstat 0

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
		nfs_file_fstat,			/* OPTIONAL; may be NULL */
		nfs_file_fchmod,			/* OPTIONAL; may be NULL */
		nfs_file_ftruncate,		/* OPTIONAL; may be NULL */
		nfs_file_fpathconf,		/* OPTIONAL; may be NULL - UNUSED */
		nfs_file_fsync,			/* OPTIONAL; may be NULL */
		nfs_file_fdatasync,		/* OPTIONAL; may be NULL */
		nfs_file_fcntl,			/* OPTIONAL; may be NULL */
		nfs_file_rmnod,			/* OPTIONAL; may be NULL */
};

static
struct _rtems_filesystem_file_handlers_r nfs_dir_file_handlers = {
		nfs_dir_open,			/* OPTIONAL; may be NULL */
		nfs_dir_close,			/* OPTIONAL; may be NULL */
		nfs_dir_read,			/* OPTIONAL; may be NULL */
		nfs_dir_write,			/* OPTIONAL; may be NULL */
		nfs_dir_ioctl,			/* OPTIONAL; may be NULL */
		nfs_dir_lseek,			/* OPTIONAL; may be NULL */
		nfs_dir_fstat,			/* OPTIONAL; may be NULL */
		nfs_dir_fchmod,			/* OPTIONAL; may be NULL */
		nfs_dir_ftruncate,		/* OPTIONAL; may be NULL */
		nfs_dir_fpathconf,		/* OPTIONAL; may be NULL - UNUSED */
		nfs_dir_fsync,			/* OPTIONAL; may be NULL */
		nfs_dir_fdatasync,		/* OPTIONAL; may be NULL */
		nfs_dir_fcntl,			/* OPTIONAL; may be NULL */
		nfs_dir_rmnod,			/* OPTIONAL; may be NULL */
};
