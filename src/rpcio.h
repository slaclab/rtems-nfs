#ifndef RPCIO_H
#define RPCIO_H

#ifdef __rtems
#include <rtems.h>
#endif
#include <stdlib.h>
#include <rpc/rpc.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <stdarg.h>

typedef struct RpcServerRec_ 	*RpcServer;
typedef struct RpcUdpXactRec_	*RpcUdpXact;

typedef RpcUdpXact				RpcUdpClnt;

int
rpcUdpInit(void);

RpcServer
rpcServerCreate(
	struct sockaddr_in *paddr,
	struct timeval		retry_period
	);

void
rpcServerDestroy(RpcServer s);

RpcUdpClnt
rpcUdpClntCreate(
	struct sockaddr_in	*psaddr,
	int					prog,
	int					vers,
	struct timeval		retry_timeout
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
	struct timeval	timeout
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

enum clnt_stat
rpcUdpSend(
	RpcUdpXact		xact,
	RpcServer		srvr,
	struct timeval	timeout,
	u_long			proc,
	xdrproc_t		xres,
	caddr_t			pres,
	xdrproc_t		xargs,
	caddr_t			pargs,
	...
	);

enum clnt_stat
rpcUdpRcv(RpcUdpXact *pxact);

#endif
