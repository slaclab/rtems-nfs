
#include <rtems.h>
#include <rtems/libio.h>

#ifdef TEST_COMPILATION
#define DECLARE_BODY { return 0; }
#elif defined(FWD_DECL)
#define DECLARE_BODY ;
#endif

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
static int nfs_rmnod(
	rtems_filesystem_location_info_t      *pathloc       /* IN */
)DECLARE_BODY
#else
#define nfs_rmnod 0
#endif

static
struct _rtems_filesystem_file_handlers_r nfs_file_handlers = {
		nfs_open,
		nfs_close,
		nfs_read,
		nfs_write,
		nfs_ioctl,
		nfs_lseek,
		nfs_fstat,
		nfs_fchmod,
		nfs_ftruncate,
		nfs_fpathconf,
		nfs_fsync,
		nfs_fdatasync,
		nfs_fcntl,
		nfs_rmnod
};

/*
 * File system types
 */

/*
 *  XXX
 *  This routine does not allocate any space and rtems_filesystem_freenode_t 
 *  is not called by the generic after calling this routine.
 *  ie. node_access does not have to contain valid data when the 
 *  routine returns.
 */


#ifdef DECLARE_BODY
static int nfs_mknod(
	const char                        *path,       /* IN */
	mode_t                             mode,       /* IN */
	dev_t                              dev,        /* IN */
	rtems_filesystem_location_info_t  *pathloc     /* IN/OUT */
)DECLARE_BODY
#else
#define nfs_mknod 0
#endif

/*
 * rtems_filesystem_freenode_t must be called by the generic after
 * calling this routine
 */

#ifdef DECLARE_BODY
static int nfs_evalpath(
	const char                        *pathname,      /* IN     */
	int                                flags,         /* IN     */
	rtems_filesystem_location_info_t  *pathloc        /* IN/OUT */
)DECLARE_BODY
#else
#define nfs_evalpath 0
#endif

#ifdef DECLARE_BODY
static int nfs_evalformake(
	const char                       *path,       /* IN */
	rtems_filesystem_location_info_t *pathloc,    /* IN/OUT */
	const char                      **name        /* OUT    */
)DECLARE_BODY
#else
#define nfs_evalformake 0
#endif

#ifdef DECLARE_BODY
static int nfs_link(
	rtems_filesystem_location_info_t  *to_loc,      /* IN */
	rtems_filesystem_location_info_t  *parent_loc,  /* IN */
	const char                        *name         /* IN */
)DECLARE_BODY
#else
#define nfs_link 0
#endif

#ifdef DECLARE_BODY
static int nfs_unlink(
	rtems_filesystem_location_info_t  *pathloc       /* IN */
)DECLARE_BODY
#else
#define nfs_unlink 0
#endif

#ifdef DECLARE_BODY
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
static rtems_filesystem_node_types_t nfs_node_type(
	rtems_filesystem_location_info_t    *pathloc      /* in */
)DECLARE_BODY
#else
#define nfs_node_type 0
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
		nfs_evalpath,
		nfs_evalformake,
		nfs_link,
		nfs_unlink,
		nfs_node_type,
		nfs_mknod,
		nfs_chown,
		nfs_freenode,
		nfs_mount,
		nfs_fsmount_me,
		nfs_unmount,
		nfs_fsunmount_me,
		nfs_utime,
		nfs_evaluate_link,
		nfs_symlink,
		nfs_readlink,
};
