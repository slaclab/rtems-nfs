
#ifdef __rtems
#include <rtems.h>
#endif
#include <stdlib.h>
#include <rpc/rpc.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <assert.h>
#include <stdio.h>

#include "rpcio.h"

#define LD_XACT_HASH	8
#define XACT_HASHS		(1<<(LD_XACT_HASH))
#define XACT_HASH_MSK	((XACT_HASHS)-1)

#ifdef __rtems
#define RTEMS_NFS_EVENT		RTEMS_EVENT_30
#endif


#define HASH_TBL_LOCK()		do {} while(0)
#define HASH_TBL_UNLOCK()	do {} while(0)


typedef struct RpcServerRec_ {
		struct sockaddr_in	addr;
		AUTH				*auth;
		struct timeval		retry_period;
} RpcServerRec;

typedef struct RpcUdpXactRec_ {
		RpcServer			server;
		struct timeval		timeout;
		struct rpc_err		status;
#ifdef __rtems
		rtems_id			requestor;
		rtems_id			retrans_timer;
#endif
		XDR					xdrs;
		int					xdrpos;
		xdrproc_t			xres;
		caddr_t				pres;
		int					bufsize;	/* size of the obuf */
		union {
			u_long				xid;
			char				buf[1];
		}					obuf;
} RpcUdpXactRec;

static RpcUdpXact xactHashTbl[XACT_HASHS]={0};

static int ourSock = -1;

RpcServer
rpcServerCreate(struct sockaddr_in *paddr, struct timeval retry_period)
{
RpcServer	rval;
u_short		port;
	if (0==paddr->sin_port) {
			return 0;
	} 
	rval       			= (RpcServer)malloc(sizeof(*rval));
	rval->addr 			= *paddr;
	rval->retry_period	= retry_period;
	rval->auth 			= authunix_create_default();
	return rval;
}

/* make sure no outstanding XACT references this server */
void
rpcServerDestroy(RpcServer s)
{
	auth_destroy(s->auth);
	free(s);
}

RpcUdpXact
rpcUdpXactCreate(
	u_long	program,
	u_long	version,
	u_long	size
	)
{
RpcUdpXact		rval=0;
struct rpc_msg	header;
register int	i,j;

	if (!size)
		size = UDPMSGSIZE;
	/* word align */
	size = (size + 3) & ~3;

	rval = (RpcUdpXact)malloc(sizeof(*rval) - sizeof(rval->obuf) + size);

	if (rval) {
	
		header.rm_xid             = 0;
		header.rm_direction       = CALL;
		header.rm_call.cb_rpcvers = RPC_MSG_VERSION;
		header.rm_call.cb_prog    = program;
		header.rm_call.cb_vers    = version;
		xdrmem_create(&(rval->xdrs), rval->obuf.buf, size, XDR_ENCODE);
		if (!xdr_callhdr(&(rval->xdrs), &header)) {
			free(rval);
			return 0;
		}
		/* pick a free table slot and initialize the XID */
		rval->obuf.xid = time(0) ^ (unsigned long)rval;
		HASH_TBL_LOCK();
		i=j=(rval->obuf.xid & XACT_HASH_MSK);
		do {
			i=(i+1) & XACT_HASH_MSK; /* cheap modulo */
			if (!xactHashTbl[i]) {
			printf("TSILL entering index %i, val %x\n",i,rval);
				xactHashTbl[i]=rval;
				j=-1;
				break;
			}
		} while (i!=j);
		HASH_TBL_UNLOCK();
		if (i==j) {
			XDR_DESTROY(&rval->xdrs);
			free(rval);
			return 0;
		}
		rval->obuf.xid= (rval->obuf.xid << LD_XACT_HASH) | i;
		rval->xdrpos  = XDR_GETPOS(&(rval->xdrs));
		rval->bufsize = size;
#ifdef __rtems
		rval->retrans_timer = 0;
#endif
	}
	return rval;
}

void
rpcUdpXactDestroy(RpcUdpXact xact)
{
int i = xact->obuf.xid & XACT_HASH_MSK;

			printf("TSILL removing index %i, val %x\n",i,xact);
		assert(xactHashTbl[i]==xact);

		HASH_TBL_LOCK();
		xactHashTbl[i]=0;
		HASH_TBL_UNLOCK();

		XDR_DESTROY(&xact->xdrs);
		free(xact);
}



enum clnt_stat
rpcUdpSend(
	RpcUdpXact		xact,
	RpcServer		srvr,
	struct timeval	timeout,
	u_long			proc,
	xdrproc_t		xres, caddr_t pres,
	xdrproc_t		xargs, caddr_t pargs,
	...
   )
{
register XDR	*xdrs;
int				len;
va_list			ap;
#ifdef __rtems
rtems_event_set	gotEvents;
#endif

	va_start(ap,pargs);

	xact->timeout = timeout;
	xact->xres    = xres;
	xact->pres    = pres;
	xact->server  = srvr;

	xdrs=&xact->xdrs;
	xdrs->x_op = XDR_ENCODE;
	/* increment transaction ID */
	xact->obuf.xid += XACT_HASHS;
	XDR_SETPOS(xdrs, xact->xdrpos);
	if ( !XDR_PUTLONG(xdrs,&proc) || !AUTH_MARSHALL(srvr->auth, xdrs) ||
		 !xargs(xdrs, pargs) ) {
		va_end(ap);
		return(xact->status.re_status=RPC_CANTENCODEARGS);
	}
	while ((xargs=va_arg(ap,xdrproc_t))) {
		if (!xargs(xdrs, va_arg(ap,caddr_t)))
		va_end(ap);
		return(xact->status.re_status=RPC_CANTENCODEARGS);
	}
	va_end(ap);
	len = (int)XDR_GETPOS(xdrs);
#ifdef __rtems
	rtems_task_ident(RTEMS_SELF, RTEMS_WHO_AM_I, &xact->requestor);
	if (rtems_message_queue_send(
								rpcQ,
								&xact,
								sizeof(xact))) {
		return RPC_CANTSEND;
	}
	/* block for the reply */
	rtems_event_receive(
			RTEMS_NFS_EVENT,
			RTEMS_WAIT | RTEMS_EVENT_ANY,
			RTEMS_NO_TIMEOUT,
			&gotEvents);
	return xact->status.re_status;
#else

	if ( sendto(ourSock,
				xact->obuf.buf,
				len,
				0,
				(struct sockaddr*) &srvr->addr,
				sizeof(srvr->addr)) != len ) {
		xact->status.re_errno = errno;
		return(xact->status.re_status=RPC_CANTSEND);
	}

	return RPC_SUCCESS;
#endif
}

enum clnt_stat
rpcUdpRcv(RpcUdpXact *pxact)
{
int					len,i;
union			{
	u_long	xid;
	char	buf[UDPMSGSIZE];
}					ibuf;
XDR					reply_xdrs;
struct rpc_msg		reply_msg;
RpcUdpXact			xact;
struct sockaddr_in	fromAddr;
int					fromLen = sizeof(fromAddr);

	if (pxact)
		*pxact=0;

	len = recvfrom(ourSock,
				   ibuf.buf, sizeof(ibuf.buf),
				   0,
				   (struct sockaddr*)&fromAddr, &fromLen);
	if (len <= 0) {
		fprintf(stderr,"RECV failed: %s\n",strerror(errno));
		return RPC_CANTRECV;
	}

	i = ibuf.xid & XACT_HASH_MSK;

	if ( !(xact=xactHashTbl[i])       					||
		   xact->obuf.xid != ibuf.xid					|| 
		   xact->server->addr.sin_addr.s_addr != fromAddr.sin_addr.s_addr	||
		   xact->server->addr.sin_port != fromAddr.sin_port ) {
		fprintf(stderr,"WARNING rpcUdpRcv(): transaction mismatch\n");
		return RPC_CANTRECV;
	}


	xdrmem_create(&reply_xdrs, ibuf.buf, sizeof(ibuf.buf), XDR_DECODE);

	reply_msg.acpted_rply.ar_verf          = _null_auth;
	reply_msg.acpted_rply.ar_results.where = xact->pres;
	reply_msg.acpted_rply.ar_results.proc  = xact->xres;

	if (xdr_replymsg(&reply_xdrs, &reply_msg)) {
		/* OK */
		_seterr_reply(&reply_msg, &xact->status);
		if (RPC_SUCCESS == xact->status.re_status) {
			if ( !AUTH_VALIDATE(xact->server->auth,
								&reply_msg.acpted_rply.ar_verf) ) {
				xact->status.re_status = RPC_AUTHERROR;
				xact->status.re_why    = AUTH_INVALIDRESP;
			}
			if (reply_msg.acpted_rply.ar_verf.oa_base) {
				reply_xdrs.x_op = XDR_FREE;
				xdr_opaque_auth(&reply_xdrs, &reply_msg.acpted_rply.ar_verf);
			}
			XDR_DESTROY(&reply_xdrs);
			return RPC_SUCCESS;
		}
	}
	reply_xdrs.x_op = XDR_FREE;
	xdr_replymsg(&reply_xdrs, &reply_msg);
	xact->status.re_status = RPC_CANTDECODERES;
	XDR_DESTROY(&reply_xdrs);

	if (pxact)
		*pxact=xact;

	return xact->status.re_status;
}

int
rpcUdpInit(void)
{
int				noblock = 1;
#ifdef __rtems
struct timeval	rxpoll;
#endif

	if (ourSock < 0) {
		ourSock=socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
		if (ourSock>=0) {
			bindresvport(ourSock,(struct sockaddr_in*)0);
#ifdef __linux
			ioctl(ourSock, FIONBIO, (char*)&noblock);
#endif
#ifdef __rtems
			rxpoll.tv_sec  = 3;
			rxpoll.tv_usec = 0;
			setsockopt(ourSock, SOL_SOCKET, SO_RCVTIMEO, &rxpoll);
#endif
		} else {
			return -1;
		}
	}
	return 0;
}

RpcUdpClnt
rpcUdpClntCreate(
		struct sockaddr_in *psaddr,
		int					prog,
		int					vers,
		struct timeval		retry_timeout)
{
RpcUdpXact	xact;
	if (!(xact=rpcUdpXactCreate(prog, vers, UDPMSGSIZE)))
		return 0;
	xact->server = rpcServerCreate(psaddr, retry_timeout);
	return xact;
}

enum clnt_stat
rpcUdpClntDestroy(RpcUdpClnt xact)
{
	rpcServerDestroy(xact->server);
	rpcUdpXactDestroy(xact);
}

enum clnt_stat
rpcUdpClntCall(
	RpcUdpClnt		xact,
	u_long			proc,
	XdrProcT		xargs,
	CaddrT			pargs,
	XdrProcT		xres,
	CaddrT			pres,
	struct timeval	timeout
	)
{
enum clnt_stat	stat;
int				retry;
fd_set			rset;
struct timeval	tmp = xact->server->retry_period;
int				sel_err;

	retry = timeout.tv_sec / tmp.tv_sec;

	if (0==retry)
		retry = 1;
	while (retry--) {
		if (stat = rpcUdpSend(xact, xact->server, timeout, proc,
					xres, pres,
					xargs, pargs,
					0)) {
			fprintf(stderr,"Send failed: %i\n",stat);
			return stat;
		}
		FD_ZERO(&rset);
		FD_SET(ourSock, &rset);
		/* linux tampers with this */
		tmp = xact->server->retry_period;
		sel_err=select(ourSock+1, &rset, 0, 0, &tmp);
		if (sel_err > 0) {
			/* OK */
			if ( RPC_SUCCESS == (stat=rpcUdpRcv(0)) )
					break;
		}
	}
	return stat;
}

#ifdef __rtems

int rpcIoDoRun = 1; /* so they may stop it */

static void
rpcrx_daemon(void arg)
{
enum clnt_stat	rxerr;
RpcUdpXact		xact;

	for (;rpcIoDoRun;) {

		rxerr = rpcUdpRecv(&xact);

		switch (rxerr) {
			case RPC_SUCCESS:
				/* cancel the retransmission timer */
				if (xact->retrans_timer) {
					/* here's a race condition; the timer could
					 * have gone off
					rtems_timer_delete(xact->retrans_timer);
					xact->retrans_timer = 0;
				}
				/* wake up the requestor of the transaction */
				rtems_event_send(xact->requestor, RTEMS_NFS_EVENT);
				break;

			case RPC_CANTRCV:
				if (EWOULDBLOCK == errno) {
					/* receive timeout */

					continue;
				}

			default: /* unknown error */
				sleep(2);
				break;
		}
	}

	rtems_task_delete(RTEMS_SELF);
}

static void
rpctx_daemon(void arg)
{
RpcUdpXact	xact;
u_long		size;
	for (;;) {
		rtems_message_queue_receive(
				rpcQ,
				&xact,
				&size,
				RTEMS_WAIT,
				RTEMS_NO_TIMEOUT);
		if (!xact) {
			/* empty transaction: cleanup */
			break;
		}
		if (!xact->retrans_timer) {
			assert(RTEMS_SUCCESSFUL == rtems_timer_create(
											rtems_build_name('R','P','C','t'),
											&xact->retrans_timer
											));
		}
		if ( sendto(ourSock,
				xact->obuf.buf,
				len,
				0,
				(struct sockaddr*) &srvr->addr,
				sizeof(srvr->addr)) != len ) {
			xact->status.re_errno = errno;
			xact->status.re_status=RPC_CANTSEND;
			/* wakeup requestor */
			rtems_event_send(xact->requestor, RPC_NFS_EVENT);
		} else {
			/* send successful; set timer */
			if ( ! xact->retrans_timer ) {
				xact->status.re_status = RPC_CANTSEND;
			}
		}

	}
}
#endif

#ifdef __rtems
#include <rtems/rtems_bsdnet_internal.h>
/* double check the event configuration; should probably globally
 * manage system events!!
 */
#if RTEMS_NFS_EVENT & SOSLEEP_EVENT & SBWAIT_EVENT & NETISR_EVENTS
#error ILLEGAL EVENT CONFIGURATION
#endif
#endif
