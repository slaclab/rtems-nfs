/*
 *  $Id$
 */

#include <string.h>
#include <stdarg.h>
/* #include <stdlib.h> */
#include <stdio.h>

#include <rtems.h>
#include <rtems/libio.h>
#include <rtems/error.h>
#define KERNEL
#include <rtems/rtems_bsdnet.h>

#include <sys/errno.h>
#include <sys/types.h>
#include <sys/param.h>
#include <sys/mbuf.h>
#include <sys/socket.h>
#include <sys/socketvar.h>
#include <sys/protosw.h>
#include <sys/proc.h>
#include <sys/fcntl.h>
#include <sys/filio.h>

#include <net/if.h>
#include <net/route.h>

struct socket *rtems_bsdnet_fdToSocket(int fd);

#if 0
/*
 * Package system call argument into mbuf.
 */
static int
sockargstombuf (struct mbuf **mp, const void *buf, int buflen, int type)
{
	struct mbuf *m;

	if ((u_int)buflen > MLEN)
		return (EINVAL);
	m = m_get(M_WAIT, type);
	if (m == NULL)
		return (ENOBUFS);
	m->m_len = buflen;
	memcpy (mtod(m, caddr_t), buf, buflen);
	*mp = m;
	if (type == MT_SONAME) {
		struct sockaddr *sa;
		sa = mtod(m, struct sockaddr *);
		sa->sa_len = buflen;
	}
	return 0;
}

/*
 * All `transmit' operations end up calling this routine.
 */
ssize_t
sendmsg (int s, const struct msghdr *mp, int flags)
{
	int ret = -1;
	int error;
	struct uio auio;
	struct iovec *iov;
	struct socket *so;
	struct mbuf *to, *control;
	int i;
	int len;

	rtems_bsdnet_semaphore_obtain ();
	if ((so = rtems_bsdnet_fdToSocket (s)) == NULL) {
		rtems_bsdnet_semaphore_release ();
		return -1;
	}
	auio.uio_iov = mp->msg_iov;
	auio.uio_iovcnt = mp->msg_iovlen;
	auio.uio_segflg = UIO_USERSPACE;
	auio.uio_rw = UIO_WRITE;
	auio.uio_offset = 0;
	auio.uio_resid = 0;
	iov = mp->msg_iov;
	for (i = 0; i < mp->msg_iovlen; i++, iov++) {
		if ((auio.uio_resid += iov->iov_len) < 0) {
			errno = EINVAL;
			rtems_bsdnet_semaphore_release ();
			return -1;
		}
	}
	if (mp->msg_name) {
		error = sockargstombuf (&to, mp->msg_name, mp->msg_namelen, MT_SONAME);
		if (error) {
			errno = error;
			rtems_bsdnet_semaphore_release ();
			return -1;
		}
	}
	else {
		to = NULL;
	}
	if (mp->msg_control) {
		if (mp->msg_controllen < sizeof (struct cmsghdr)) {
			errno = EINVAL;
			if (to)
				m_freem(to);
			rtems_bsdnet_semaphore_release ();
			return -1;
		}
		sockargstombuf (&control, mp->msg_control, mp->msg_controllen, MT_CONTROL);
	}
	else {
		control = NULL;
	}
	len = auio.uio_resid;
	error = sosend (so, to, &auio, (struct mbuf *)0, control, flags);
	if (error) {
		if (auio.uio_resid != len && (error == EINTR || error == EWOULDBLOCK))
			error = 0;
	}
	if (error) 
		errno = error;
	else
		ret = len - auio.uio_resid;
	if (to)
		m_freem(to);
	rtems_bsdnet_semaphore_release ();
	return (ret);
}


/*
 * Send a message to a host
 */
ssize_t
sendto (int s, const void *buf, size_t buflen, int flags, const struct sockaddr *to, int tolen)
{
	struct msghdr msg;
	struct iovec iov;

	iov.iov_base = (void *)buf;
	iov.iov_len = buflen;
	msg.msg_name = (caddr_t)to;
	msg.msg_namelen = tolen;
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	msg.msg_control = NULL;
	msg.msg_controllen = 0;
	return sendmsg (s, &msg, flags);
}
#endif

/*
 * All `receive' operations end up calling this routine.
 */
ssize_t
recv_mbuf_from(int s, struct mbuf **ppm, long len, struct sockaddr *fromaddr, int *fromlen)
{
	int ret = -1;
	int error;
	struct uio auio;
	struct socket *so;
	struct mbuf *from = NULL;

	memset(&auio, 0, sizeof(auio));
	*ppm = 0;

	rtems_bsdnet_semaphore_obtain ();
	if ((so = rtems_bsdnet_fdToSocket (s)) == NULL) {
		rtems_bsdnet_semaphore_release ();
		return -1;
	}
/*	auio.uio_iov = mp->msg_iov;
	auio.uio_iovcnt = mp->msg_iovlen;
	auio.uio_segflg = UIO_USERSPACE;
	auio.uio_rw = UIO_READ;
	auio.uio_offset = 0;
*/
	auio.uio_resid = len;
	error = soreceive (so, &from, &auio, (struct mbuf **) ppm, 
			(struct mbuf **)NULL,
			NULL);
	if (error) {
		if (auio.uio_resid != len && (error == EINTR || error == EWOULDBLOCK))
			error = 0;
	}
	if (error) {
		errno = error;
	}
	else {
		ret = len - auio.uio_resid;
		if (fromaddr) {
			len = *fromlen;
			if ((len <= 0) || (from == NULL)) {
				len = 0;
			}
			else {
				if (len > from->m_len)
					len = from->m_len;
				memcpy (fromaddr, mtod(from, caddr_t), len);
			}
			*fromlen = len;
		}
	}
	if (from)
		m_freem (from);
	if (error && *ppm) {
		m_freem(*ppm);
		*ppm = 0;
	}
	rtems_bsdnet_semaphore_release ();
	return (ret);
}

#if 0
/*
 * Receive a message from a host
 */
ssize_t
recvfrom (int s, void *buf, size_t buflen, int flags, const struct sockaddr *from, int *fromlen)
{
	struct msghdr msg;
	struct iovec iov;
	int ret;

	iov.iov_base = buf;
	iov.iov_len = buflen;
	msg.msg_name = (caddr_t)from;
	if (fromlen)
		msg.msg_namelen = *fromlen;
	else
	msg.msg_namelen = 0;
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	msg.msg_control = NULL;
	msg.msg_controllen = 0;
	ret = recvmsg (s, &msg, 0);
	if ((from != NULL) && (fromlen != NULL) && (ret >= 0))
		*fromlen = msg.msg_namelen;
	return ret;
}
#endif
