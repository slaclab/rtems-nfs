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

/****************************************************************/
/* CONFIGURABLE PARAMETERS                                      */
/****************************************************************/

#define MBUF_RX			/* If defined: use mbuf XDR stream for
						 * decoding directly out of mbufs
						 * Otherwise, the regular sendto/recvfrom
						 * interface will be used involving an
						 * extra buffer allocation + copy step.
						 */

/* daemon task parameters */
#define RPCIOD_STACK		10000
#define RPCIOD_PRIO			50

/* depth of the message queue for sending
 * RPC requests to the daemon
 */
#define RPCIOD_QDEPTH		20

/* Maximum retry limit for retransmission */
#define RPCIOD_RETX_CAP_S	3 /* seconds */

#ifdef __rtems
/* Events we are using; the RPC_EVENT
 * MUST NOT be used by any application
 * thread doing RPC IO (e.g. NFS)
 */
#define RTEMS_RPC_EVENT		RTEMS_EVENT_30	/* THE event used by RPCIO. Every task doing
											 * RPC IO will receive this - hence it is 
											 * RESERVED
											 */
#define RPCIOD_RX_EVENT		RTEMS_EVENT_1	/* Events the RPCIOD is using/waiting for */
#define RPCIOD_TX_EVENT		RTEMS_EVENT_2
#define RPCIOD_KILL_EVENT	RTEMS_EVENT_3	/* send to the daemon to kill it          */

#endif

#define LD_XACT_HASH	8					/* ld of the size of the transaction hash table  */


/* Debugging Flags                                              */

#define	DEBUG_TRACE_XACT	(1<<0)
#define DEBUG_EVENTS		(1<<1)
#define DEBUG_MALLOC		(1<<2)
#define DEBUG_TIMEOUT		(1<<3)

#define DEBUG	DEBUG_TIMEOUT

/****************************************************************/
/* END OF CONFIGURABLE SECTION                                  */
/****************************************************************/

#if defined(MBUF_RX) && !defined(__rtems)
#warning MBUF_RX is only supported on RTEMS; disabling
#undef	 MBUF_RX
#endif


#ifdef __rtems

/****************************************************************/
/* MACROS                                                       */
/****************************************************************/


#define XACT_HASHS		(1<<(LD_XACT_HASH))	/* the hash table size derived from the ld       */
#define XACT_HASH_MSK	((XACT_HASHS)-1)	/* mask to extract the hash index from a RPC-XID */


#define HASH_TBL_LOCK()		do { 							\
								rtems_semaphore_obtain(		\
										hlock, 				\
										RTEMS_WAIT,			\
										RTEMS_NO_TIMEOUT	\
										);					\
							} while(0)

#define HASH_TBL_UNLOCK()	do {							\
								rtems_semaphore_release(	\
										hlock				\
										);					\
							} while(0)

#define MUTEX_ATTRIBUTES	(RTEMS_LOCAL           | 		\
			   				RTEMS_PRIORITY         | 		\
			   				RTEMS_INHERIT_PRIORITY | 		\
						   	RTEMS_BINARY_SEMAPHORE)

#define FIRST_ATTEMPT		0x88888888 /* some time that is never reached */

#else

#define HASH_TBL_LOCK()		do {} while(0)
#define HASH_TBL_UNLOCK()	do {} while(0)

#endif

/****************************************************************/
/* TYPE DEFINITIONS                                             */
/****************************************************************/

#ifdef __rtems
typedef	rtems_interval		TimeoutT;
#else
typedef struct timeval		TimeoutT;
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
		TimeoutT			retry_period;	/* dynamically adjusted retry period
											 * (based on packet roundtrip time)
											 */
		unsigned long		retrans;		/* how many retries were issued by this server */
		char				name[20];		/* server's address in IP 'dot' notation       */
} RpcUdpServerRec;

typedef union  RpcBufU_ {
		u_long				xid;
		char				buf[1];
} RpcBufU, *RpcBuf;

/* RX Buffer implementation; this is either
 * an MBUF chain (MBUF_RX configuration)
 * or a buffer allocated from the heap
 * where recvfrom copies the (encoded) reply
 * to. The XDR routines the copy/decode
 * it into the user's data structures.
 */
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
 * (for encoding arguments).
 */
typedef struct RpcUdpXactRec_ {
		ListNodeRec			node;		/* so we can put XACTs on a list                */
		RpcUdpServer		server;		/* server this XACT goes to                     */
		long				lifetime;	/* during the lifetime, retry attempts are made */
		struct rpc_err		status;		/* RPC reply error status                       */
#ifdef __rtems
		long				age;		/* age info; needed to manage retransmission    */
		long				trip;		/* record round trip time in ticks              */
		rtems_id			requestor;	/* the task waiting for this XACT to complete   */
		RpcUdpXactPool		pool;		/* if this XACT belong to a pool, this is it    */
#endif
		XDR					xdrs;		/* argument encoder stream                      */
		int					xdrpos;     /* stream position after the (permanent) header */
		xdrproc_t			xres;		/* reply decoder proc - TODO needn't be here    */
		caddr_t				pres;		/* reply decoded obj  - TODO needn't be here    */
#ifndef MBUF_RX
		int					ibufsize;	/* size of the ibuf (bytes)                     */
#endif
		int					obufsize;	/* size of the obuf (bytes)                     */
		RxBuf				ibuf;		/* pointer to input buffer assigned by daemon   */
		RpcBufU				obuf;       /* output buffer (encoded args) APPENDED HERE   */
} RpcUdpXactRec;

#ifdef __rtems
typedef struct RpcUdpXactPoolRec_ {
	rtems_id	box;
	int			prog;
	int			version;
	int			xactSize;
} RpcUdpXactPoolRec;
#endif

/* a global hash table where all 'living' transaction
 * objects are registered.
 * A number of bits in a transaction's XID maps 1:1 to
 * an index in this table. Hence, the XACT matching
 * an RPC/UDP reply packet can quickly be found 
 * The size of this table imposes a hard limit on the
 * number of all created transactions in the system.
 */
static RpcUdpXact xactHashTbl[XACT_HASHS]={0};

/* forward declarations */
static RpcUdpXact
sockRcv(RxBuf *pibuf, int ibufsize);

static void
rpcio_daemon(rtems_task_argument);

static int				ourSock = -1;	/* the socket we are using for communication */
#ifdef __rtems
static rtems_id			rpciod  = 0;	/* task id of the RPC daemon                 */
static rtems_id			msgQ    = 0;	/* message queue where the daemon picks up
										 * requests
										 */
static rtems_id			hlock	= 0;	/* MUTEX protecting the hash table           */
static rtems_id			fini	= 0;	/* a synchronization semaphore we use during
										 * module cleanup / driver unloading
										 */
static rtems_interval	ticksPerSec;	/* cached system clock rate (WHO IS ASSUMED NOT
										 * TO CHANGE
										 */
#endif

#if DEBUG & DEBUG_MALLOC
/* malloc wrappers for debugging */
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
	)
{
RpcUdpServer	rval;
u_short			port;

	/* if they specified no port try to ask the portmapper */
	if (!paddr->sin_port) {
		paddr->sin_port = htons(PMAPPORT);
		port            = pmap_getport(paddr, prog, vers ,IPPROTO_UDP);
		paddr->sin_port = htons(port);
	}

	if (0==paddr->sin_port) {
			return 0;
	} 

	rval       			= (RpcUdpServer)MALLOC(sizeof(*rval));
	memset(rval, 0, sizeof(*rval));

	if (!inet_ntop(AF_INET, paddr, rval->name, sizeof(rval->name)))
		sprintf(rval->name,"?.?.?.?");
	rval->addr 			= *paddr;

	/* start with a long retransmission interval - it
	 * will be adapted dynamically
	 */
	rval->retry_period  = RPCIOD_RETX_CAP_S * ticksPerSec; 

	/* TODO handle / configure credentials - lifetime? */
	rval->auth 			= authunix_create_default();
	return rval;
}

void
rpcUdpServerDestroy(RpcUdpServer s)
{
	if (!s)
		return;
	/* we should probably verify (but how?) that nobody
	 * (at least: no outstanding XACTs) is using this
	 */
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
#if DEBUG & DEBUG_TRACE_XACT
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

#if DEBUG & DEBUG_TRACE_XACT
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
	xact->lifetime  = ms * ticksPerSec / 1000;
#if DEBUG & DEBUG_TIMEOUT
{
	static int once=0;
	if (!once++) {
		fprintf(stderr,
				"Initial lifetime: %i (ticks)\n",
				xact->lifetime);
	}
}
#endif
#else
	xact->lifetime  = ms / (1000 * srvr->retry_period.tv_sec +
						  srvr->retry_period.tv_usec/1000);
#endif

	xact->xres      = xres;
	xact->pres      = pres;
	xact->server    = srvr;

	xdrs=&xact->xdrs;
	xdrs->x_op      = XDR_ENCODE;
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
			RTEMS_RPC_EVENT,
			RTEMS_WAIT | 
"todo - is this the correct flag? we don't want to lose other events that
task might receive"
RTEMS_EVENT_ANY,
			RTEMS_NO_TIMEOUT,
			&gotEvents);

	if (xact->status.re_status) {
#ifdef MBUF_RX
		assert( !xact->ibuf );
#endif
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
		struct timeval		retry_timeout /* ignored */
)
{
RpcUdpXact		x;
RpcUdpServer	s;

	if ( !(s = rpcUdpServerCreate(psaddr, prog, vers)) )
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
		} while (xact->lifetime--);

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
enum clnt_stat   rxerr;
RpcUdpXact       xact;
RpcUdpServer     srv;
char             strbuf[20];
char             *chpt;
rtems_interval   next_retrans, then, now;
rtems_event_set  events;
rtems_id         q;
ListNode         newList;
rtems_unsigned32 size;
RxBuf            buf      =  0;
ListNodeRec      listHead = {0};

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

#if DEBUG & DEBUG_EVENTS
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

		rtems_clock_get(
				RTEMS_CLOCK_GET_TICKS_SINCE_BOOT,
				&now);

		/* measure everything relative to then to protect against
		 * rollover
		 */
		now-=then;

		/* NOTE: we don't lock the hash table while we are operating
		 * on transactions; the paradigm is that we 'own' a particular
		 * transaction (and hence it's hash table slot) from the
		 * time the xact was put into the message queue until we
		 * wake up the requestor.
		 */

		if (RPCIOD_RX_EVENT & events) {

#if DEBUG & DEBUG_EVENTS
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

				xact->status.re_status = RPC_SUCCESS;

				/* calculate roundtrip ticks */
				xact->trip = now - xact->trip;

				srv = xact->server;

				/* adjust the server's retry period */
				{
					register TimeoutT rtry = srv->retry_period;

					/* retry_new = 0.75*retry_old + 0.25 * 4 * roundrip */
					rtry = 3*rtry + (xact->trip << 2);
					rtry >>= 2;
					if (0==rtry)
						rtry = 1;
					srv->retry_period = rtry;
				}

				/* wakeup requestor */
				rtems_event_send(xact->requestor, RTEMS_RPC_EVENT);
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

		if (RPCIOD_TX_EVENT & events) {

#if DEBUG & DEBUG_EVENTS
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

				xact->age  = now;
				xact->trip = FIRST_ATTEMPT;
			}
		}


		/* work the timeout q */
		newList = 0;
		for ( xact=(RpcUdpXact)listHead.next;
			  xact && xact->age <= now;
			  xact=(RpcUdpXact)listHead.next ) {

				/* extract from the list */
				nodeXtract(&xact->node);

				srv = xact->server;

				if (xact->lifetime < 0) {
					/* this one timed out */
					xact->status.re_errno  = ETIMEDOUT;
					xact->status.re_status = RPC_TIMEDOUT;

#if DEBUG & DEBUG_TIMEOUT
					fprintf(stderr,"XACT timed out; waking up requestor\n");
#endif

					assert( RTEMS_SUCCESSFUL ==
							rtems_event_send(xact->requestor, RTEMS_RPC_EVENT) );

				} else {
					int len;

					len = (int)XDR_GETPOS(&xact->xdrs);
				
					if ( len != sendto( ourSock,
										xact->obuf.buf,
										len,
										0,
										(struct sockaddr*) &srv->addr,
										sizeof(srv->addr)) ) {

						xact->status.re_errno  = errno;
						xact->status.re_status = RPC_CANTSEND;

						/* wakeup requestor */
						fprintf(stderr,"RPCIO: SEND failure\n");
						assert( RTEMS_SUCCESSFUL ==
									rtems_event_send(xact->requestor, RTEMS_RPC_EVENT) );

					} else {
						/* send successful; calculate retransmission time
						 * and enqueue to temporary list
						 */
						if (FIRST_ATTEMPT != xact->trip) {
#if DEBUG & DEBUG_TIMEOUT
							fprintf(stderr,
									"timed out; retrans is %i, retry period is %i\n",
									xact->retrans,
									srv->retry_period);
#endif
							/* this is a real retry; we backup
							 * the server's retry interval
							 */
							if ( srv->retry_period < RPCIOD_RETX_CAP_S * ticksPerSec ) {

								/* If multiple transactions for this server
								 * fail (e.g. because it died) this will
								 * back-off very agressively (doubling
								 * the retransmission period for every
								 * timed out transaction up to the CAP limit)
								 * which is desirable - single packet failure
								 * is treated more gracefully by this algorithm.
								 */

								srv->retry_period<<=1;
#if DEBUG & DEBUG_TIMEOUT
								fprintf(stderr,
										"adjusted to; retry period %i\n",
										srv->retry_period);
#endif
							} else {
								/* never wait longer than RPCIOD_RETX_CAP_S seconds */
								fprintf(stderr,
										"RPCIO: server '%s' not responding - still trying\n",
										srv->name);
							}
							if ( 0 == ++srv->retrans % 1000) {
								fprintf(stderr,
										"RPCIO - statistics: already %i retries to server %s\n",
										srv->retrans,
										srv->name);
							}
						}
						xact->trip      = now;
						xact->age       = now + srv->retry_period;
						xact->lifetime -= srv->retry_period;
						/* enqueue to the list of newly sent transactions */
						xact->node.next = newList;
						newList         = &xact->node;
					}
				}
	    }

		/* insert the newly sent transactions into the
		 * sorted retransmission list
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
				/* age = oldnow + age - newnow */
				((RpcUdpXact)n)->age  -= now;
				/* roundtrip = arrive - sent = arrive + newthen - (sent + oldthen)
				 *                           = arrive + newthen - (sent + newthen - oldthen)
				 *  ==> sent = (sent - oldthen) + newthen = sent + now;
				 */
				((RpcUdpXact)n)->trip += now;
			}
		}

		next_retrans = listHead.next ? ((RpcUdpXact)listHead.next)->age - now : RTEMS_NO_TIMEOUT;
#if DEBUG & DEBUG_TIMEOUT
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
			rtems_event_send(xact->requestor, RTEMS_RPC_EVENT);
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

/* WORKAROUND: include sys/mbuf.h (or other bsdnet headers) only
 *             _after_ using malloc()/free() & friends because
 *             the RTEMS/BSDNET headers redefine those :-(
 */

#include <sys/mbuf.h>

ssize_t
recv_mbuf_from(int s, struct mbuf **ppm, long len, struct sockaddr *fromaddr, int *fromlen);

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
	len = recv_mbuf_from(ourSock,
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
 * We do this at the end of the file for the same reason we had
 * included mbuf.h only a couple of lines above - see comment up
 * there...
 */
#if RTEMS_RPC_EVENT & SOSLEEP_EVENT & SBWAIT_EVENT & NETISR_EVENTS
#error ILLEGAL EVENT CONFIGURATION
#endif
#endif
