/* $Id$ */

/* RPC multiplexor for a multitasking environment */

/* Author: Till Straumann <strauman@slac.stanford.edu>, 2002 */

/* This code funnels arbitrary task's UDP/RPC requests
 * through one socket to arbitrary servers.
 * The replies are gathered and dispatched to the 
 * requestors.
 * One task handles all the sending and receiving
 * work including retries.
 * It is up to the requestor, however, to do
 * the XDR encoding of the arguments / decoding
 * of the results (except for the RPC header which
 * is handled by the daemon).
 */

#ifdef __rtems
#include <rtems.h>
#endif
#include <stdlib.h>
#include <rpc/rpc.h>
#include <rpc/pmap_prot.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <assert.h>
#include <stdio.h>

#include "rpcio.h"

#define LD_XACT_HASH	8
#define XACT_HASHS		(1<<(LD_XACT_HASH))
#define XACT_HASH_MSK	((XACT_HASHS)-1)
#define MBUF_RX			/* use mbuf XDR stream for decoding directly out of mbufs */

#undef  DEBUG

#ifdef __rtems
#define RTEMS_NFS_EVENT		RTEMS_EVENT_30
#define RPCIOD_RX_EVENT		RTEMS_EVENT_1
#define RPCIOD_TX_EVENT		RTEMS_EVENT_2
#define RPCIOD_KILL_EVENT	RTEMS_EVENT_3
#define RPCIOD_STACK		10000
#define RPCIOD_PRIO			50
#define RPCIOD_QDEPTH		20
#define HASH_TBL_LOCK()		do {rtems_semaphore_obtain(hlock, RTEMS_WAIT, RTEMS_NO_TIMEOUT);} while(0)
#define HASH_TBL_UNLOCK()	do {rtems_semaphore_release(hlock);} while(0)

#define MUTEX_ATTRIBUTES	(RTEMS_LOCAL           | \
			   				RTEMS_PRIORITY         | \
			   				RTEMS_INHERIT_PRIORITY | \
						   	RTEMS_BINARY_SEMAPHORE)

#else

#define HASH_TBL_LOCK()		do {} while(0)
#define HASH_TBL_UNLOCK()	do {} while(0)

#endif

#ifdef __rtems
typedef	rtems_interval		TimeoutT;
#else
typedef struct timeval		TimeoutT;
#endif


#if defined(MBUF_RX) && !defined(__rtems)
#warning MBUF_RX is only supported on RTEMS; disabling
#undef	 MBUF_RX
#endif

/* 100000th implementation of a doubly linked list;
 * since only one thread is looking at these,
 * we need no locking
 */
typedef struct ListNodeRec_ {
	struct ListNodeRec_ *next, *prev;
} ListNodeRec, *ListNode;


/* Structure representing an RPC server */
typedef struct RpcUdpServerRec_ {
		struct sockaddr_in	addr;
		AUTH				*auth;
		TimeoutT			retry_period;
} RpcUdpServerRec;

typedef union  RpcBufU_ {
		u_long				xid;
		char				buf[1];
} RpcBufU, *RpcBuf;

#ifdef MBUF_RX
typedef	struct mbuf *		RxBuf;	/* an MBUF chain */
static  void   bufFree(struct mbuf *m);
#else
typedef RpcBuf				RxBuf;
#define	bufFree(b)	do { FREE(b); } while(0)
#endif

/* A RPC 'transaction' consisting
 * of server and requestor information,
 * buffer space and an XDR object
 */
typedef struct RpcUdpXactRec_ {
		ListNodeRec			node;
		RpcUdpServer		server;
		long				retrans;
		struct rpc_err		status;
#ifdef __rtems
		long				age;
		rtems_id			requestor;
		RpcUdpXactPool		pool;
#endif
		XDR					xdrs;
		int					xdrpos;
		xdrproc_t			xres;
		caddr_t				pres;
		int					ibufsize;	/* size of the obuf */
		int					obufsize;	/* size of the obuf */
		RxBuf				ibuf;
		RpcBufU				obuf;
} RpcUdpXactRec;

#ifdef __rtems
typedef struct RpcUdpXactPoolRec_ {
	rtems_id	box;
	int			prog;
	int			version;
	int			xactSize;
} RpcUdpXactPoolRec;
#endif

static RpcUdpXact xactHashTbl[XACT_HASHS]={0};

static RpcUdpXact
sockRcv(RxBuf *pibuf, int ibufsize);

static int				ourSock = -1;
#ifdef __rtems
static rtems_id			rpciod  = 0;
static rtems_id			msgQ    = 0;
static rtems_id			hlock	= 0;
static rtems_id			fini	= 0;
static rtems_interval	ticksPerSec;

static void rpcio_daemon(rtems_task_argument);
#endif

#ifdef DEBUG
static int nibufs = 0;

static inline void *MALLOC(int s)
{
	if (s) {
		void *rval;
		HASH_TBL_LOCK();
		assert(nibufs++ < 2000);
		HASH_TBL_UNLOCK();
		assert(rval = malloc(s));
		return rval;
	}
	return 0;
}

static inline void FREE(void *p)
{
	if (p) {
		HASH_TBL_LOCK();
		nibufs--;
		HASH_TBL_UNLOCK();
		free(p);
	}
}
#else
#define MALLOC	malloc
#define FREE	free
#endif

/* Create a server object
 *
 */
RpcUdpServer
rpcUdpServerCreate(
	struct sockaddr_in	*paddr,
	int					prog,
	int					vers,
	struct timeval		retry_period)
{
RpcUdpServer	rval;
u_short			port;

	if (!paddr->sin_port) {
		paddr->sin_port = htons(PMAPPORT);
		port            = pmap_getport(paddr, prog, vers ,IPPROTO_UDP);
		paddr->sin_port = htons(port);
	}

	if (0==paddr->sin_port) {
			return 0;
	} 

	rval       			= (RpcUdpServer)MALLOC(sizeof(*rval));
	rval->addr 			= *paddr;

	rval->retry_period  = retry_period.tv_usec * ticksPerSec / 1000000;
	rval->retry_period += retry_period.tv_sec * ticksPerSec; 

	rval->auth 			= authunix_create_default();
	return rval;
}

/* make sure no outstanding XACT references this server */
void
rpcUdpServerDestroy(RpcUdpServer s)
{
	if (!s)
		return;
	auth_destroy(s->auth);
	FREE(s);
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

	rval = (RpcUdpXact)calloc(1,sizeof(*rval) - sizeof(rval->obuf) + size);

	if (rval) {
	
		header.rm_xid             = 0;
		header.rm_direction       = CALL;
		header.rm_call.cb_rpcvers = RPC_MSG_VERSION;
		header.rm_call.cb_prog    = program;
		header.rm_call.cb_vers    = version;
		xdrmem_create(&(rval->xdrs), rval->obuf.buf, size, XDR_ENCODE);
		if (!xdr_callhdr(&(rval->xdrs), &header)) {
			FREE(rval);
			return 0;
		}
		/* pick a free table slot and initialize the XID */
		rval->obuf.xid = time(0) ^ (unsigned long)rval;
		HASH_TBL_LOCK();
		i=j=(rval->obuf.xid & XACT_HASH_MSK);
#ifdef __rtems
		if (msgQ) {
#endif
			/* if there's no message queue, refuse to 
			 * give them transactions; we might be in the process to
			 * go away...
			 */
			do {
				i=(i+1) & XACT_HASH_MSK; /* cheap modulo */
				if (!xactHashTbl[i]) {
#if DEBUG & 1
					fprintf(stderr,"RPCIO: entering index %i, val %x\n",i,rval);
#endif
					xactHashTbl[i]=rval;
					j=-1;
					break;
				}
			} while (i!=j);
#ifdef __rtems
		}
#endif
		HASH_TBL_UNLOCK();
		if (i==j) {
			XDR_DESTROY(&rval->xdrs);
			FREE(rval);
			return 0;
		}
		rval->obuf.xid  = (rval->obuf.xid << LD_XACT_HASH) | i;
		rval->xdrpos    = XDR_GETPOS(&(rval->xdrs));
		rval->obufsize  = size;
	}
	return rval;
}

void
rpcUdpXactDestroy(RpcUdpXact xact)
{
int i = xact->obuf.xid & XACT_HASH_MSK;

#if DEBUG & 1
		fprintf(stderr,"RPCIO: removing index %i, val %x\n",i,xact);
#endif

		assert(xactHashTbl[i]==xact);

		HASH_TBL_LOCK();
		xactHashTbl[i]=0;
		HASH_TBL_UNLOCK();

		bufFree(xact->ibuf);

		XDR_DESTROY(&xact->xdrs);
		FREE(xact);
}



/* Send a transaction, i.e. enqueue it to the
 * RPC daemon who will actually send it.
 */
enum clnt_stat
rpcUdpSend(
	RpcUdpXact		xact,
	RpcUdpServer	srvr,
	struct timeval	timeout,
	u_long			proc,
	xdrproc_t		xres, caddr_t pres,
	xdrproc_t		xargs, caddr_t pargs,
	...
   )
{
register XDR	*xdrs;
int				len;
unsigned long	ms;
va_list			ap;

	va_start(ap,pargs);

	ms = 1000 * timeout.tv_sec + timeout.tv_usec/1000;

#ifdef __rtems
	xact->retrans = ms * ticksPerSec / 1000 / srvr->retry_period;
#else
	xact->retrans = ms / (1000 * srvr->retry_period.tv_sec +
						  srvr->retry_period.tv_usec/1000);
#endif

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
#ifdef __rtems
	rtems_task_ident(RTEMS_SELF, RTEMS_WHO_AM_I, &xact->requestor);
	if (rtems_message_queue_send(
								msgQ,
								&xact,
								sizeof(xact))) {
		return RPC_CANTSEND;
	}
	/* wakeup the rpciod */
	assert(RTEMS_SUCCESSFUL==rtems_event_send(rpciod, RPCIOD_TX_EVENT));
#endif

	return RPC_SUCCESS;
}

#ifdef MBUF_RX
extern void xdrmbuf_create(XDR *, struct mbuf *, enum xdr_op);
#endif

/* Block for the RPC reply to an outstanding
 * transaction.
 * The caller is woken by the RPC daemon either
 * upon reception of the reply or on timeout.
 */
enum clnt_stat
rpcUdpRcv(RpcUdpXact xact)
{
int					i;
XDR					reply_xdrs;
struct rpc_msg		reply_msg;
#ifdef __rtems
rtems_event_set		gotEvents;
#endif

#ifdef __rtems
	/* block for the reply */
	rtems_event_receive(
			RTEMS_NFS_EVENT,
			RTEMS_WAIT | RTEMS_EVENT_ANY,
			RTEMS_NO_TIMEOUT,
			&gotEvents);
	if (xact->status.re_status) {
		return xact->status.re_status;
	}
#endif

#ifdef MBUF_RX
	xdrmbuf_create(&reply_xdrs, xact->ibuf, XDR_DECODE);
#else
	xdrmem_create(&reply_xdrs, xact->ibuf->buf, xact->ibufsize, XDR_DECODE);
#endif

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
		}
	} else {
		reply_xdrs.x_op        = XDR_FREE;
		xdr_replymsg(&reply_xdrs, &reply_msg);
		xact->status.re_status = RPC_CANTDECODERES;
	}
	XDR_DESTROY(&reply_xdrs);

	bufFree(xact->ibuf);
	xact->ibuf     = 0;
	xact->ibufsize = 0;

	return xact->status.re_status;
}

/* sent from the socket */
static enum clnt_stat
sockSnd(RpcUdpXact xact)
{
int len = (int)XDR_GETPOS(&xact->xdrs);
	if ( sendto(ourSock,
				xact->obuf.buf,
				len,
				0,
				(struct sockaddr*) &xact->server->addr,
				sizeof(xact->server->addr)) != len ) {
		xact->status.re_errno = errno;
		return(xact->status.re_status=RPC_CANTSEND);
	}
	return RPC_SUCCESS;
}

#ifdef MBUF_RX
#define XID(ibuf) (*(mtod((ibuf), u_long *)))
#else
#define XID(ibuf) ((ibuf)->xid)
#endif


#ifdef __rtems
/* On RTEMS, I'm told to avoid select(); this seems to
 * be more efficient
 */
static void
rxWakeupCB(struct socket *sock, caddr_t arg)
{
rtems_event_send((rtems_id)arg, RPCIOD_RX_EVENT);
}
#endif

int
rpcUdpInit(void)
{
int					noblock = 1;
#ifdef __rtems
struct sockwakeup	wkup;
#endif

	if (ourSock < 0) {
		ourSock=socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
		if (ourSock>=0) {
			bindresvport(ourSock,(struct sockaddr_in*)0);
			assert( 0==ioctl(ourSock, FIONBIO, (char*)&noblock) );
#ifdef __rtems
			/* assume nobody tampers with the clock !! */
			assert( RTEMS_SUCCESSFUL == rtems_clock_get(
											RTEMS_CLOCK_GET_TICKS_PER_SECOND,
											&ticksPerSec));
			assert( RTEMS_SUCCESSFUL == rtems_semaphore_create(
											rtems_build_name('R','P','C','l'),
											1,
											MUTEX_ATTRIBUTES,
											0,
											&hlock) );
			assert( RTEMS_SUCCESSFUL == rtems_task_create(
											rtems_build_name('R','P','C','d'),
											RPCIOD_PRIO,
											RPCIOD_STACK,
											RTEMS_DEFAULT_MODES,
											RTEMS_DEFAULT_ATTRIBUTES,
											&rpciod) );
			wkup.sw_pfn = rxWakeupCB;
			wkup.sw_arg = (caddr_t)rpciod;
			assert( 0==setsockopt(ourSock, SOL_SOCKET, SO_RCVWAKEUP, &wkup, sizeof(wkup)) );
			assert( RTEMS_SUCCESSFUL == rtems_message_queue_create(
											rtems_build_name('R','P','C','q'),
											RPCIOD_QDEPTH,
											sizeof(RpcUdpXact),
											RTEMS_DEFAULT_ATTRIBUTES,
											&msgQ) );
			assert( RTEMS_SUCCESSFUL == rtems_task_start(
											rpciod,
											rpcio_daemon,
											0 ) );

#endif
		} else {
			return -1;
		}
	}
	return 0;
}

/* Another API - simpler but less efficient.
 * For each RPCall, a server and a Xact
 * are created and destroyed on the fly.
 *
 * This should be used for infrequent calls
 * (e.g. a NFS mount request).
 *
 * This is roughly compatible with the original
 * clnt_call() etc. API - but it uses our
 * daemon and is fully reentrant.
 */
RpcUdpClnt
rpcUdpClntCreate(
		struct sockaddr_in *psaddr,
		int					prog,
		int					vers,
		struct timeval		retry_timeout)
{
RpcUdpXact		x;
RpcUdpServer	s;

	if ( !(s = rpcUdpServerCreate(psaddr, prog, vers, retry_timeout)) )
		return 0;

	if ( !(x=rpcUdpXactCreate(prog, vers, UDPMSGSIZE)) ) {
		rpcUdpServerDestroy(s);
		return 0;
	}
	/* TODO: could maintain a server cache */

	x->server = s;

	return x;
}

enum clnt_stat
rpcUdpClntDestroy(RpcUdpClnt xact)
{
	rpcUdpServerDestroy(xact->server);
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
#ifndef __rtems
fd_set			rset;
struct timeval	tmp;
int				sel_err;
RxBuf			buf = 0;
#endif
		if (stat = rpcUdpSend(xact, xact->server, timeout, proc,
					xres, pres,
					xargs, pargs,
					0)) {
			fprintf(stderr,"Send failed: %i\n",stat);
			return stat;
		}
#ifndef __rtems
		do {
			if (stat = sockSnd(xact)) {
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
				if (!buf) {
					buf = (RxBuf)MALLOC(UDPMSGSIZE);
				}
				if ( sockRcv(&buf, UDPMSGSIZE) )
					return rpcUdpRcv(xact);
				fprintf(stderr,"Rcv failed '%s'\n",strerror(errno));
			}
		} while (xact->retrans--);

		bufFree(buf);

		return RPC_TIMEDOUT;
#else
		return rpcUdpRcv(xact);
#endif
}

#ifdef __rtems

/* linked list primitives */
static void
nodeXtract(ListNode n)
{
	if (n->prev)
		n->prev->next = n->next;
	if (n->next)
		n->next->prev = n->prev;
	n->next = n->prev = 0;
}

static void
nodeAppend(ListNode l, ListNode n)
{
	if (n->next = l->next)
		n->next->prev = n;
	l->next = n;
	n->prev = l;
	
}

/* this code does the work */
static void
rpcio_daemon(rtems_task_argument arg)
{
enum clnt_stat	 rxerr;
RpcUdpXact		 xact;
rtems_interval	 next_retrans, then, now;
rtems_event_set	 events;
rtems_id		 q;
ListNodeRec		 listHead={0};
ListNode		 newList;
rtems_unsigned32 size;
RxBuf			 buf = 0;

	assert( RTEMS_SUCCESSFUL == rtems_clock_get(
									RTEMS_CLOCK_GET_TICKS_SINCE_BOOT,
									&then) );

	for (next_retrans=RTEMS_NO_TIMEOUT;;) {

		rtems_event_receive(
						RPCIOD_RX_EVENT | RPCIOD_TX_EVENT | RPCIOD_KILL_EVENT,
						RTEMS_WAIT | RTEMS_EVENT_ANY, 
						next_retrans,
						&events);

		if (events & RPCIOD_KILL_EVENT) {
			int i;

#if DEBUG & 2
			fprintf(stderr,"RPCIO: got KILL event\n");
#endif

			HASH_TBL_LOCK();
			for (i=XACT_HASHS-1; i>=0; i--) {
				if (xactHashTbl[i]) {
					break;
				}
			}
			if (i<0) {
				/* prevent them from creating and enqueueing more messages */
				q=msgQ;
				/* messages queued after we executed this assignment will fail */
				msgQ=0;
			}
			HASH_TBL_UNLOCK();
			if (i>=0) {
				fprintf(stderr,"There are still transactions circulating; I refuse to go away\n");
				fprintf(stderr,"(1st in slot %i)\n",i);
				rtems_semaphore_release(fini);
			} else {
				break;
			}
		}

		/* NOTE: we don't lock the hash table while we are operating
		 * on transactions; the paradigm is that we 'own' a particular
		 * transaction (and hence it's hash table slot) from the
		 * time the xact was put into the message queue until we
		 * wake up the requestor.
		 */

		if (RPCIOD_RX_EVENT & events) {

#if DEBUG & 2
			fprintf(stderr,"RPCIO: got RX event\n");
#endif

#ifndef MBUF_RX
			if (!buf) {
				buf=(RxBuf)MALLOC(UDPMSGSIZE);
			}
#endif
			while ((xact=sockRcv(&buf, UDPMSGSIZE))) {
				/* extract from the retransmission list */
				nodeXtract(&xact->node);
				/* wakeup requestor */
				xact->status.re_status = RPC_SUCCESS;
				rtems_event_send(xact->requestor, RTEMS_NFS_EVENT);
#ifndef MBUF_RX
				if (!buf)
					buf = (RpcBuf)MALLOC(UDPMSGSIZE);
#else
				assert( !buf );
#endif
			}
#ifdef  MBUF_RX
			bufFree(buf);
			buf = 0;
#endif
		}

		rtems_clock_get(
				RTEMS_CLOCK_GET_TICKS_SINCE_BOOT,
				&now);
		/* measure everything relative to then to protect against
		 * rollover
		 */
		now-=then;

		if (RPCIOD_TX_EVENT & events) {

#if DEBUG & 2
			fprintf(stderr,"RPCIO: got TX event\n");
#endif

			while (RTEMS_SUCCESSFUL == rtems_message_queue_receive(
											msgQ,
											&xact,
											&size,
											RTEMS_NO_WAIT,
											RTEMS_NO_TIMEOUT)) {
				/* put to the head of timeout q */
				nodeAppend(&listHead, &xact->node);

				xact->retrans++; /* account for initial transmission */
				xact->age = now;
			}
		}


		/* work the timeout q */
		newList = 0;
		for ( xact=(RpcUdpXact)listHead.next; xact && xact->age <= now; ) {

				/* extract from the list */
				nodeXtract(&xact->node);

				if (--xact->retrans <= 0) {
					/* this one timed out */
					xact->status.re_status = RPC_TIMEDOUT;
					rtems_event_send(xact->requestor, RTEMS_NFS_EVENT);

				} else {
					int len;

					len = (int)XDR_GETPOS(&xact->xdrs);
				
					if ( sendto(ourSock,
						xact->obuf.buf,
						len,
						0,
						(struct sockaddr*) &xact->server->addr,
						sizeof(xact->server->addr)) != len ) {
						xact->status.re_errno = errno;
						xact->status.re_status=RPC_CANTSEND;
						/* wakeup requestor */
						fprintf(stderr,"RPCIO: SEND failure\n");
						rtems_event_send(xact->requestor, RTEMS_NFS_EVENT);
					} else {
						/* send successful; calculate retransmission time
						 * and enqueue to temporary list
						 */
						xact->age = now + xact->server->retry_period;
						xact->node.next = newList;
						newList = &xact->node;
					}
				}
	    }

		/* insert the newly sent transactions into the sorted retransmission
		 * list
		 */
		for (; (xact = (RpcUdpXact)newList); ) {
			register ListNode p,n;
			newList = newList->next;
			for ( p=&listHead; (n=p->next) && xact->age > ((RpcUdpXact)n)->age; p=n )
				/* nothing else to do */;
			nodeAppend(p, &xact->node);
		}

		if (now > 1000000) {
			/* every now and then, readjust the epoch */
			register ListNode n;
			then += now;
			for (n=listHead.next; n; n=n->next) {
				((RpcUdpXact)n)->age-=now;
			}
		}

		next_retrans = listHead.next ? ((RpcUdpXact)listHead.next)->age - now : RTEMS_NO_TIMEOUT;
#if DEBUG & 4
		fprintf(stderr,"RPCIO: next timeout is %x\n",next_retrans);
#endif
	}
	/* close our socket; shut down the receiver */
	close(ourSock);

#if 0 /* if we get here, no transactions exist, hence there can be none
	   * in the queue whatsoever
	   */
	/* flush the message queue */
	while (RTEMS_SUCCESSFUL == rtems_message_queue_receive(
										q,
										&xact,
										&size,
										RTEMS_NO_WAIT,
										RTEMS_NO_TIMEOUT)) {
			/* TODO enque xact */
	}

	/* flush all outstanding transactions */

	for (xact=((RpcUdpXact)listHead.next); xact; xact=((RpcUdpXact)xact->node.next)) {
			xact->status.re_status = RPC_TIMEDOUT;
			rtems_event_send(xact->requestor, RTEMS_NFS_EVENT);
	}
#endif

	bufFree(buf);

	rtems_message_queue_delete(q);

	rtems_semaphore_delete(hlock);

	fprintf(stderr,"RPC daemon exited...\n");

	rtems_semaphore_release(fini);
	rtems_task_suspend(RTEMS_SELF);
}

/* CEXP module support (magic init) */
_cexpModuleInitialize(void *mod)
{
	rpcUdpInit();
}

_cexpModuleFinalize(void *mod)
{
	rtems_semaphore_create(
			rtems_build_name('R','P','C','f'),
			0,
			RTEMS_DEFAULT_ATTRIBUTES,
			0,
			&fini);
	rtems_event_send(rpciod, RPCIOD_KILL_EVENT);
	/* synchronize with daemon */
	rtems_semaphore_obtain(fini, RTEMS_WAIT, 5*ticksPerSec);
	/* if the message queue is still there, something went wrong */
	if (!msgQ) {
		rtems_task_delete(rpciod);
	}
	rtems_semaphore_delete(fini);
	return (msgQ !=0);
}


/* support for transaction 'pools'. A number of XACT objects
 * is always kept around. The initial number is 0 but it
 * is allowed to grow up to a maximum.
 * If the need grows beyond the maximum, behavior depends:
 * Users can either block until a transaction becomes available,
 * they can create a new XACT on the fly or get an error
 * if no free XACT is available from the pool.
 */

RpcUdpXactPool
rpcUdpXactPoolCreate(
	int prog, 		int version,
	int xactsize,	int poolsize)
{
int				i;
RpcUdpXactPool	rval = MALLOC(sizeof(*rval));

	assert( rval &&
			RTEMS_SUCCESSFUL == rtems_message_queue_create(
									rtems_build_name('R','P','C','p'),
									poolsize,
									sizeof(RpcUdpXact),
									RTEMS_DEFAULT_ATTRIBUTES,
									&rval->box) );
	rval->prog     = prog;
	rval->version  = version;
	rval->xactSize = xactsize;
	return rval;
}

void
rpcUdpXactPoolDestroy(RpcUdpXactPool pool)
{
RpcUdpXact xact;

	while ((xact = rpcUdpXactPoolGet(pool, XactGetFail))) {
		rpcUdpXactDestroy(xact);
	}
	rtems_message_queue_delete(pool->box);
	FREE(pool);
}

RpcUdpXact
rpcUdpXactPoolGet(RpcUdpXactPool pool, XactPoolGetMode mode)
{
RpcUdpXact		 xact = 0;
rtems_unsigned32 size;

	if (RTEMS_SUCCESSFUL != rtems_message_queue_receive(
								pool->box,
								&xact,
								&size,
								XactGetWait == mode ?
									RTEMS_WAIT : RTEMS_NO_WAIT,
								RTEMS_NO_TIMEOUT)) {
		/* nothing found in box; should we create a new one ? */

		xact = (XactGetCreate == mode) ?
					rpcUdpXactCreate(
							pool->prog,
							pool->version,
							pool->xactSize) : 0 ;
		if (xact)
				xact->pool = pool;

	}
	return xact;
}

void
rpcUdpXactPoolPut(RpcUdpXact xact)
{
RpcUdpXactPool pool;
	assert( pool=xact->pool );
	if (RTEMS_SUCCESSFUL != rtems_message_queue_send(
								pool->box,
								&xact,
								sizeof(xact)))
		rpcUdpXactDestroy(xact);
}

#ifdef MBUF_RX

#include <sys/mbuf.h>

ssize_t
rcv_mbuf_from(int s, struct mbuf **ppm, long len, struct sockaddr *fromaddr, int *fromlen);

static void
bufFree(struct mbuf *m)
{
	if (m) {
		rtems_bsdnet_semaphore_obtain();
		m_freem(m);
		rtems_bsdnet_semaphore_release();
	}
}
#endif

/* receive from a socket and find
 * the transaction corresponding to the
 * transaction ID received in the server
 * reply.
 */
static RpcUdpXact
sockRcv(RxBuf *pibuf, int ibufsize)
{
int					len,i;
u_long				xid;
RpcUdpXact			xact = 0;
struct sockaddr_in	fromAddr;
int					fromLen  = sizeof(fromAddr);

#ifdef MBUF_RX
	len = rcv_mbuf_from(ourSock,
					pibuf,
					UDPMSGSIZE,
				   (struct sockaddr*)&fromAddr, &fromLen);
#else
	len  = recvfrom(ourSock,
				   (*pibuf)->buf, ibufsize,
				   0,
				   (struct sockaddr*)&fromAddr, &fromLen);
#endif

	if (len <= 0) {
		if (EAGAIN != errno)
			fprintf(stderr,"RECV failed: %s\n",strerror(errno));
		goto cleanup;
	}

	i = (xid=XID(*pibuf)) & XACT_HASH_MSK;

	if ( !(xact=xactHashTbl[i])   ||
		   xact->obuf.xid                     != xid						|| 
		   xact->server->addr.sin_addr.s_addr != fromAddr.sin_addr.s_addr	||
		   xact->server->addr.sin_port        != fromAddr.sin_port ) {
		fprintf(stderr,"WARNING sockRcv(): transaction mismatch\n");
		xact = 0;
		goto cleanup;
	}
	xact->ibuf     = *pibuf;
	xact->ibufsize = ibufsize;
	*pibuf         = 0;

cleanup:
	return xact;
}


#include <rtems/rtems_bsdnet_internal.h>
/* double check the event configuration; should probably globally
 * manage system events!!
 */
#if RTEMS_NFS_EVENT & SOSLEEP_EVENT & SBWAIT_EVENT & NETISR_EVENTS
#error ILLEGAL EVENT CONFIGURATION
#endif
#endif
