#include <rtems.h>
#include <rtems/libio.h>
#include <rtems/seterr.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>

#include <nfs_prot.h>

#include "rpcio.h"

#ifdef TEST_COMPILATION
#define DECLARE_BODY { return 0; }
#elif defined(FWD_DECL)
#define DECLARE_BODY ;
#endif

#define DELIM							'/'
#define NFS_VERSION_2					NFS_VERSION

#define CONFIG_NFS_BIG_XACT_SIZE		UDPMSGSIZE
#define CONFIG_NFS_SMALL_XACT_SIZE		UDPMSGSIZE
#define NFSCALL_TIMEOUT					_nfscalltimeout
#define NFSCALL_RETRYPERIOD				_nfscallretry

#undef  TSILLDEBUG
#define STATIC

static struct timeval _nfscalltimeout = { 10, 0 };
static struct timeval _nfscallretry   = { 1,  0 };

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


typedef struct NfsNodeRec_ {
	serporid	serporid;
} NfsNodeRec, *NfsNode;

/* Per mounted FS structure */
typedef struct NfsRec_ {
	RpcServer	server;
} NfsRec, *Nfs;

#define SERP_ARGS(node) ((node)->serporid.serporid_u.serporid.arg_u)
#define SERP_ATTR(node) ((node)->serporid.serporid_u.serporid.attributes)
#define SERP_FILE(node) ((node)->serporid.serporid_u.serporid.file)

#ifndef TSILLDEBUG
NfsNodeRec	dbgRoot;
nfs_fh		*dbgFhP = &SERP_FILE(&dbgRoot);
Nfs			dbgNfs;
#endif

static RpcUdpXactPool smallPool = 0;
static RpcUdpXactPool bigPool   = 0;

Nfs
nfsCreate(struct sockaddr_in *psa)
{
Nfs rval = malloc(sizeof(*rval));

	if (rval)
		rval->server = rpcServerCreate(
							psa,
							NFSCALL_RETRYPERIOD);
	return rval;
}

void
nfsDestroy(Nfs nfs)
{
	rpcServerDestroy(nfs->server);
	free(nfs);
}

static NfsNode
nfsNodeAlloc(void)
{
NfsNode	rval = malloc(sizeof(*rval));

	return rval;
}

/* get a transaction for a mounted nfs
 * a limited number are hold in the xbox;
 * if more are needed, they are created
 * on the fly.
 */

static void
nfsNodeFree(NfsNode node)
{
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
#ifndef TSILLDEBUG
	extern struct sockaddr_in dbg_server;
	dbgNfs = nfsCreate(&dbg_server);
#endif
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
#ifndef TSILLDEBUG
	nfsDestroy(dbgNfs);
#endif
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


/*
 * rtems_filesystem_freenode_t must be called by the generic after
 * calling this routine
 */

int
nfscallSmall(
	RpcServer	srvr,
	int			proc,
	xdrproc_t	xargs,
	void *		pargs,
	xdrproc_t	xres,
	void *		pres)
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
	     RPC_SUCCESS != (stat=rpcUdpRcv(xact)) ){

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
		if (NFS_OK == (errno=*(nfsstat*)pres)) {
			rval = 0;
		}
	}

	/* release the transaction back into the pool */
	rpcUdpXactPoolPut(xact);

	return rval;
}

int
lk1(char *name)
{
diropres			res;
fattr				*f;
enum clnt_stat		stat;
extern diropargs dbg_diropargs;


	dbg_diropargs.name=name;
	if (nfscallSmall(dbgNfs->server,
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

STATIC int nfs_evalpath(
	const char                        *pathname,      /* IN     */
	int                                flags,         /* IN     */
	rtems_filesystem_location_info_t  *pathloc        /* IN/OUT */
)
{
char		*p = strdup(pathname);
char		*del, *part;
NfsNode		node=nfsNodeAlloc();
int			e;
#ifndef TSILLDEBUG
#warning TSILLDEBUG still unset
RpcServer	server = dbgNfs->server;
#else
RpcServer	server = ((Nfs)pathloc->mt_entry->fs_info)->server;
#endif

	if (!p || !node) {
		e = ENOMEM;
		goto bailout;
	}

#ifndef TSILLDEBUG
	/* copy the start node */
	memcpy(node, pathloc, sizeof(*node));
#else
	memcpy(node, pathloc->node_access, sizeof(*node));
#endif

	for (part=p; p && *p; p=del) {
		/* find delimiter and eat /// sequences */
		if ((del = strchr(p, DELIM))) {
			do {
				*del++=0;
			} while (DELIM==*del);
		}

		/* lookup one element */
		SERP_ARGS(node).diroparg.name = p;

		if (nfscallSmall(server,
						 NFSPROC_LOOKUP,
						 xdr_diropargs,
						 &SERP_FILE(node),
						 xdr_serporid,
						 &node->serporid)) {
			e = errno;
			goto bailout;
		}
	}

	free(p);
#ifndef TSILLDEBUG
	fprintf(stderr,"Type %i, ino %i\n",
					SERP_ATTR(node).type,
					SERP_ATTR(node).fileid);
	nfsNodeFree(node);
#endif

	pathloc->node_access = node;
	return 0;

bailout:
	free(p);
	nfsNodeFree(node);
	rtems_set_errno_and_return_minus_one(e);
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

#ifdef DECLARE_BODY
/* Cleanup the FS private info attached to pathloc->node_access */
static int nfs_freenode(
	rtems_filesystem_location_info_t      *pathloc       /* IN */
)DECLARE_BODY
#else
#define nfs_freenode 0
#endif

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

#ifdef DECLARE_BODY
/* This op is called as the last step of mounting this FS */
static int nfs_fsmount_me(
	rtems_filesystem_mount_table_entry_t *mt_entry
)DECLARE_BODY
#else
#define nfs_fsmount_me 0
#endif

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

#ifdef DECLARE_BODY
/* This op is called when they try to unmount THIS fs */
static int nfs_fsunmount_me(
	rtems_filesystem_mount_table_entry_t *mt_entry    /* in */
)DECLARE_BODY
#else
#define nfs_fsunmount_me 0
#endif

#ifdef DECLARE_BODY
/* OPTIONAL; may be NULL - BUT: CAUTION; mount() doesn't check
 * for this handler to be present - a fs bug
 */
static rtems_filesystem_node_types_t nfs_node_type(
	rtems_filesystem_location_info_t    *pathloc      /* in */
)DECLARE_BODY
#else
#define nfs_node_type 0
#endif

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

static
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

/*
 *  File Handler Operations Table
 */

#ifdef DECLARE_BODY
static int nfs_open(
	rtems_libio_t *iop,
	const char    *pathname,
	unsigned32     flag,
	unsigned32     mode
)DECLARE_BODY
#else
#define nfs_open 0
#endif

#ifdef DECLARE_BODY
static int nfs_close(
	rtems_libio_t *iop
)DECLARE_BODY
#else
#define nfs_close 0
#endif

#ifdef DECLARE_BODY
static int nfs_read(
	rtems_libio_t *iop,
	void          *buffer,
	unsigned32     count
)DECLARE_BODY
#else
#define nfs_read 0
#endif

#ifdef DECLARE_BODY
static int nfs_write(
	rtems_libio_t *iop,
	const void    *buffer,
	unsigned32    count
)DECLARE_BODY
#else
#define nfs_write 0
#endif

#ifdef DECLARE_BODY
static int nfs_ioctl(
	rtems_libio_t *iop,
	unsigned32     command,
	void          *buffer
)DECLARE_BODY
#else
#define nfs_ioctl 0
#endif

#ifdef DECLARE_BODY
static int nfs_lseek(
	rtems_libio_t *iop,
	off_t          length,
	int            whence
)DECLARE_BODY
#else
#define nfs_lseek 0
#endif

#ifdef DECLARE_BODY
static int nfs_fstat(
	rtems_filesystem_location_info_t *loc,
	struct stat                      *buf
)DECLARE_BODY
#else
#define nfs_fstat 0
#endif

#ifdef DECLARE_BODY
static int nfs_fchmod(
	rtems_filesystem_location_info_t *loc,
	mode_t                            mode
)DECLARE_BODY
#else
#define nfs_fchmod 0
#endif

#ifdef DECLARE_BODY
static int nfs_ftruncate(
	rtems_libio_t *iop,
	off_t          length
)DECLARE_BODY
#else
#define nfs_ftruncate 0
#endif

#ifdef DECLARE_BODY
static int nfs_fpathconf(
	rtems_libio_t *iop,
	int name
)DECLARE_BODY
#else
#define nfs_fpathconf 0
#endif

#ifdef DECLARE_BODY
static int nfs_fsync(
	rtems_libio_t *iop
)DECLARE_BODY
#else
#define nfs_fsync 0
#endif

#ifdef DECLARE_BODY
static int nfs_fdatasync(
	rtems_libio_t *iop
)DECLARE_BODY
#else
#define nfs_fdatasync 0
#endif

#ifdef DECLARE_BODY
static int nfs_fcntl(
	int            cmd,
	rtems_libio_t *iop
)DECLARE_BODY
#else
#define nfs_fcntl 0
#endif

#ifdef DECLARE_BODY
/* OPTIONAL; may be NULL */
static int nfs_rmnod(
	rtems_filesystem_location_info_t      *pathloc       /* IN */
)DECLARE_BODY
#else
#define nfs_rmnod 0
#endif

static
struct _rtems_filesystem_file_handlers_r nfs_file_handlers = {
		nfs_open,			/* OPTIONAL; may be NULL */
		nfs_close,			/* OPTIONAL; may be NULL */
		nfs_read,			/* OPTIONAL; may be NULL */
		nfs_write,			/* OPTIONAL; may be NULL */
		nfs_ioctl,			/* OPTIONAL; may be NULL */
		nfs_lseek,			/* OPTIONAL; may be NULL */
		nfs_fstat,			/* OPTIONAL; may be NULL */
		nfs_fchmod,			/* OPTIONAL; may be NULL */
		nfs_ftruncate,		/* OPTIONAL; may be NULL */
		nfs_fpathconf,		/* OPTIONAL; may be NULL - UNUSED */
		nfs_fsync,			/* OPTIONAL; may be NULL */
		nfs_fdatasync,		/* OPTIONAL; may be NULL */
		nfs_fcntl,			/* OPTIONAL; may be NULL */
		nfs_rmnod,			/* OPTIONAL; may be NULL */
};


