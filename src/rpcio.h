#ifndef RPCIO_H
#define RPCIO_H

#ifdef __rtems
#include <rtems.h>
#endif

#include <stdlib.h>
#include <rpc/rpc.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/param.h>
#include <stdarg.h>

typedef struct RpcUdpServerRec_ 	*RpcUdpServer;
typedef struct RpcUdpXactRec_		*RpcUdpXact;

typedef RpcUdpXact					RpcUdpClnt;

#define RPCIOD_DEFAULT_ID	0xdef10000

int
rpcUdpInit(void);

enum clnt_stat
rpcUdpServerCreate(
	struct sockaddr_in *paddr,
	int					prog,
	int					vers,
	u_long				uid,		/* RPCIO_DEFAULT_ID picks default */
	u_long				gid,		/* RPCIO_DEFAULT_ID picks default */
	RpcUdpServer		*pclnt		/* new server is returned here    */
	);


void
rpcUdpServerDestroy(RpcUdpServer s);

enum clnt_stat
rpcUdpClntCreate(
	struct sockaddr_in	*psaddr,
	int					prog,
	int					vers,
	u_long				uid,		/* RPCIO_DEFAULT_ID picks default */
	u_long				gid,		/* RPCIO_DEFAULT_ID picks default */
	RpcUdpClnt			*pclnt		/* new client is returned here    */
	);

void
RpcUdpClntDestroy(RpcUdpClnt clnt);

/* mute compiler warnings */
typedef void *XdrProcT;
typedef void *CaddrT;

enum clnt_stat
rpcUdpClntCall(
	RpcUdpClnt		clnt,
	u_long			proc,
	XdrProcT		xargs,
	CaddrT			pargs,
	XdrProcT		xres,
	CaddrT			pres,
	struct timeval	*timeout	/* optional timeout; maybe NULL to pick default */
	);

RpcUdpXact
rpcUdpXactCreate(
	u_long	program,
	u_long	version,
	u_long	size
	);

void
rpcUdpXactDestroy(
	RpcUdpXact xact
	);

/* send a transaction */
enum clnt_stat
rpcUdpSend(
	RpcUdpXact		xact,
	RpcUdpServer	srvr,
	struct timeval	*timeout,	/* maybe NULL to pick default */
	u_long			proc,
	xdrproc_t		xres,
	caddr_t			pres,
	xdrproc_t		xargs,
	caddr_t			pargs,
	...				/* 0 terminated xdrproc/pobj additional argument list */	
	);

/* wait for a transaction to complete */
enum clnt_stat
rpcUdpRcv(RpcUdpXact xact);

/* a yet simpler interface */
enum clnt_stat
rpcUdpCallRp(
	struct sockaddr_in	*pserver_addr,
	u_long				prog,
	u_long				vers,
	u_long				proc,
	XdrProcT			xargs,
	CaddrT				pargs,
	XdrProcT			xres,
	CaddrT				pres,
	u_long				uid,		/* RPCIO_DEFAULT_ID picks default */
	u_long				gid,		/* RPCIO_DEFAULT_ID picks default */
	struct timeval		*timeout	/* NULL picks default		*/
);


#ifdef __rtems /* pools are implemented by RTEMS message queues */

/* manage pools of transactions */

/* A pool of transactions. The idea is not to malloc/free them
 * all the time but keep a limited number around in a 'pool'.
 * Users who need a XACT may get it from the pool and put it back
 * when done.
 * The pool is implemented by RTEMS message queues who manage
 * the required task synchronization.
 * A requestor has different options if the pool is empty:
 *  - it can wait (block) for a XACT to become available 
 *  - it can get an error status
 *  - or it can malloc an extra XACT from the heap which
 *    will eventually be released.
 */

typedef struct RpcUdpXactPoolRec_  *RpcUdpXactPool;

/* NOTE: the pool is empty initially, must get messages (in
 *       GetCreate mode
 */
RpcUdpXactPool
rpcUdpXactPoolCreate(
	int prog, 		int version,
	int xactsize,	int poolsize);

void
rpcUdpXactPoolDestroy(RpcUdpXactPool pool);

typedef enum {
	XactGetFail,	/* call fails if no transaction available */
	XactGetWait,	/* call blocks until transaction available */
	XactGetCreate	/* a new transaction is allocated (and freed when put back to the pool */
} XactPoolGetMode;

RpcUdpXact
rpcUdpXactPoolGet(RpcUdpXactPool pool, XactPoolGetMode mode);

void
rpcUdpXactPoolPut(RpcUdpXact xact);
#endif

#endif
