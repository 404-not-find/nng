//
// Copyright 2018 Staysail Systems, Inc. <info@staysail.tech>
// Copyright 2018 Capitar IT Group BV <info@capitar.com>
// Copyright 2018 Devolutions <info@devolutions.net>
//
// This software is supplied under the terms of the MIT License, a
// copy of which should be located in the distribution where this
// file was obtained (LICENSE.txt).  A copy of the license may also be
// found online at https://opensource.org/licenses/MIT.
//

#include "core/nng_impl.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>
#ifdef NNG_HAVE_ALLOCA
#include <alloca.h>
#endif

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

#include "posix_tcp.h"

static void
tcp_conn_dowrite(nni_tcp_conn *c)
{
	nni_aio *aio;
	int      fd;

	if (c->closed || ((fd = nni_posix_pfd_fd(c->pfd)) < 0)) {
		return;
	}

	while ((aio = nni_list_first(&c->writeq)) != NULL) {
		unsigned      i;
		int           n;
		int           niov;
		unsigned      naiov;
		nni_iov *     aiov;
		struct msghdr hdr;
#ifdef NNG_HAVE_ALLOCA
		struct iovec *iovec;
#else
		struct iovec iovec[16];
#endif

		memset(&hdr, 0, sizeof(hdr));
		nni_aio_get_iov(aio, &naiov, &aiov);

#ifdef NNG_HAVE_ALLOCA
		if (naiov > 64) {
			nni_aio_list_remove(aio);
			nni_aio_finish_error(aio, NNG_EINVAL);
			continue;
		}
		iovec = alloca(naiov * sizeof(*iovec));
#else
		if (naiov > NNI_NUM_ELEMENTS(iovec)) {
			nni_aio_list_remove(aio);
			nni_aio_finish_error(aio, NNG_EINVAL);
			continue;
		}
#endif

		for (niov = 0, i = 0; i < naiov; i++) {
			if (aiov[i].iov_len > 0) {
				iovec[niov].iov_len  = aiov[i].iov_len;
				iovec[niov].iov_base = aiov[i].iov_buf;
				niov++;
			}
		}

		hdr.msg_iovlen = niov;
		hdr.msg_iov    = iovec;

		if ((n = sendmsg(fd, &hdr, MSG_NOSIGNAL)) < 0) {
			switch (errno) {
			case EINTR:
				continue;
			case EAGAIN:
#ifdef EWOULDBLOCK
#if EWOULDBLOCK != EAGAIN
			case EWOULDBLOCK:
#endif
#endif
				return;
			default:
				nni_aio_list_remove(aio);
				nni_aio_finish_error(
				    aio, nni_plat_errno(errno));
				return;
			}
		}

		nni_aio_bump_count(aio, n);
		// We completed the entire operation on this aio.
		// (Sendmsg never returns a partial result.)
		nni_aio_list_remove(aio);
		nni_aio_finish(aio, 0, nni_aio_count(aio));

		// Go back to start of loop to see if there is another
		// aio ready for us to process.
	}
}

static void
tcp_conn_doread(nni_tcp_conn *c)
{
	nni_aio *aio;
	int      fd;

	if (c->closed || ((fd = nni_posix_pfd_fd(c->pfd)) < 0)) {
		return;
	}

	while ((aio = nni_list_first(&c->readq)) != NULL) {
		unsigned i;
		int      n;
		int      niov;
		unsigned naiov;
		nni_iov *aiov;
#ifdef NNG_HAVE_ALLOCA
		struct iovec *iovec;
#else
		struct iovec iovec[16];
#endif

		nni_aio_get_iov(aio, &naiov, &aiov);
#ifdef NNG_HAVE_ALLOCA
		if (naiov > 64) {
			nni_aio_list_remove(aio);
			nni_aio_finish_error(aio, NNG_EINVAL);
			continue;
		}
		iovec = alloca(naiov * sizeof(*iovec));
#else
		if (naiov > NNI_NUM_ELEMENTS(iovec)) {
			nni_aio_list_remove(aio);
			nni_aio_finish_error(aio, NNG_EINVAL);
			continue;
		}
#endif
		for (niov = 0, i = 0; i < naiov; i++) {
			if (aiov[i].iov_len != 0) {
				iovec[niov].iov_len  = aiov[i].iov_len;
				iovec[niov].iov_base = aiov[i].iov_buf;
				niov++;
			}
		}

		if ((n = readv(fd, iovec, niov)) < 0) {
			switch (errno) {
			case EINTR:
				continue;
			case EAGAIN:
				return;
			default:
				nni_aio_list_remove(aio);
				nni_aio_finish_error(
				    aio, nni_plat_errno(errno));
				return;
			}
		}

		if (n == 0) {
			// No bytes indicates a closed descriptor.
			// This implicitly completes this (all!) aio.
			nni_aio_list_remove(aio);
			nni_aio_finish_error(aio, NNG_ECLOSED);
			continue;
		}

		nni_aio_bump_count(aio, n);

		// We completed the entire operation on this aio.
		nni_aio_list_remove(aio);
		nni_aio_finish(aio, 0, nni_aio_count(aio));

		// Go back to start of loop to see if there is another
		// aio ready for us to process.
	}
}

void
nni_tcp_conn_close(nni_tcp_conn *c)
{
	nni_mtx_lock(&c->mtx);
	if (!c->closed) {
		nni_aio *aio;
		c->closed = true;
		while (((aio = nni_list_first(&c->readq)) != NULL) ||
		    ((aio = nni_list_first(&c->writeq)) != NULL)) {
			nni_aio_list_remove(aio);
			nni_aio_finish_error(aio, NNG_ECLOSED);
		}
		nni_posix_pfd_close(c->pfd);
	}
	nni_mtx_unlock(&c->mtx);
}

static void
tcp_conn_cb(nni_posix_pfd *pfd, int events, void *arg)
{
	nni_tcp_conn *c = arg;

	if (events & (POLLHUP | POLLERR | POLLNVAL)) {
		nni_tcp_conn_close(c);
		return;
	}
	nni_mtx_lock(&c->mtx);
	if (events & POLLIN) {
		tcp_conn_doread(c);
	}
	if (events & POLLOUT) {
		tcp_conn_dowrite(c);
	}
	events = 0;
	if (!nni_list_empty(&c->writeq)) {
		events |= POLLOUT;
	}
	if (!nni_list_empty(&c->readq)) {
		events |= POLLIN;
	}
	if ((!c->closed) && (events != 0)) {
		nni_posix_pfd_arm(pfd, events);
	}
	nni_mtx_unlock(&c->mtx);
}

static void
tcp_conn_cancel(nni_aio *aio, void *arg, int rv)
{
	nni_tcp_conn *c = arg;

	nni_mtx_lock(&c->mtx);
	if (nni_aio_list_active(aio)) {
		nni_aio_list_remove(aio);
		nni_aio_finish_error(aio, rv);
	}
	nni_mtx_unlock(&c->mtx);
}

void
nni_tcp_conn_send(nni_tcp_conn *c, nni_aio *aio)
{

	int rv;

	if (nni_aio_begin(aio) != 0) {
		return;
	}
	nni_mtx_lock(&c->mtx);

	if ((rv = nni_aio_schedule(aio, tcp_conn_cancel, c)) != 0) {
		nni_mtx_unlock(&c->mtx);
		nni_aio_finish_error(aio, rv);
		return;
	}
	nni_aio_list_append(&c->writeq, aio);

	if (nni_list_first(&c->writeq) == aio) {
		tcp_conn_dowrite(c);
		// If we are still the first thing on the list, that
		// means we didn't finish the job, so arm the poller to
		// complete us.
		if (nni_list_first(&c->writeq) == aio) {
			nni_posix_pfd_arm(c->pfd, POLLOUT);
		}
	}
	nni_mtx_unlock(&c->mtx);
}

void
nni_tcp_conn_recv(nni_tcp_conn *c, nni_aio *aio)
{
	int rv;

	if (nni_aio_begin(aio) != 0) {
		return;
	}
	nni_mtx_lock(&c->mtx);

	if ((rv = nni_aio_schedule(aio, tcp_conn_cancel, c)) != 0) {
		nni_mtx_unlock(&c->mtx);
		nni_aio_finish_error(aio, rv);
		return;
	}
	nni_aio_list_append(&c->readq, aio);

	// If we are only job on the list, go ahead and try to do an
	// immediate transfer. This allows for faster completions in
	// many cases.  We also need not arm a list if it was already
	// armed.
	if (nni_list_first(&c->readq) == aio) {
		tcp_conn_doread(c);
		// If we are still the first thing on the list, that
		// means we didn't finish the job, so arm the poller to
		// complete us.
		if (nni_list_first(&c->readq) == aio) {
			nni_posix_pfd_arm(c->pfd, POLLIN);
		}
	}
	nni_mtx_unlock(&c->mtx);
}

int
nni_tcp_conn_peername(nni_tcp_conn *c, nni_sockaddr *sa)
{
	struct sockaddr_storage ss;
	socklen_t               sslen = sizeof(ss);
	int                     fd    = nni_posix_pfd_fd(c->pfd);

	if (getpeername(fd, (void *) &ss, &sslen) != 0) {
		return (nni_plat_errno(errno));
	}
	return (nni_posix_sockaddr2nn(sa, &ss));
}

int
nni_tcp_conn_sockname(nni_tcp_conn *c, nni_sockaddr *sa)
{
	struct sockaddr_storage ss;
	socklen_t               sslen = sizeof(ss);
	int                     fd    = nni_posix_pfd_fd(c->pfd);

	if (getsockname(fd, (void *) &ss, &sslen) != 0) {
		return (nni_plat_errno(errno));
	}
	return (nni_posix_sockaddr2nn(sa, &ss));
}

int
nni_tcp_conn_set_keepalive(nni_tcp_conn *c, bool keep)
{
	int val = keep ? 1 : 0;
	int fd  = nni_posix_pfd_fd(c->pfd);

	if (setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &val, sizeof(val)) != 0) {
		return (nni_plat_errno(errno));
	}
	return (0);
}

int
nni_tcp_conn_set_nodelay(nni_tcp_conn *c, bool nodelay)
{

	int val = nodelay ? 1 : 0;
	int fd  = nni_posix_pfd_fd(c->pfd);

	if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &val, sizeof(val)) != 0) {
		return (nni_plat_errno(errno));
	}
	return (0);
}

static int
tcp_conn_get_peername(void *arg, void *buf, size_t *szp, nni_type t)
{
	nni_tcp_conn *          c = arg;
	struct sockaddr_storage ss;
	socklen_t               sslen = sizeof(ss);
	int                     fd    = nni_posix_pfd_fd(c->pfd);
	int                     rv;
	nng_sockaddr            sa;

	if (getpeername(fd, (void *) &ss, &sslen) != 0) {
		return (nni_plat_errno(errno));
	}
	if ((rv = nni_posix_sockaddr2nn(&sa, &ss)) == 0) {
		rv = nni_copyout_sockaddr(&sa, buf, szp, t);
	}
	return (rv);
}

static int
tcp_conn_get_sockname(void *arg, void *buf, size_t *szp, nni_type t)
{
	nni_tcp_conn *          c = arg;
	struct sockaddr_storage ss;
	socklen_t               sslen = sizeof(ss);
	int                     fd    = nni_posix_pfd_fd(c->pfd);
	int                     rv;
	nng_sockaddr            sa;

	if (getsockname(fd, (void *) &ss, &sslen) != 0) {
		return (nni_plat_errno(errno));
	}
	if ((rv = nni_posix_sockaddr2nn(&sa, &ss)) == 0) {
		rv = nni_copyout_sockaddr(&sa, buf, szp, t);
	}
	return (rv);
}

static int
tcp_conn_set_nodelay(void *arg, const void *buf, size_t sz, nni_type t)
{
	nni_tcp_conn *c = arg;
	int           fd;
	bool          b;
	int           val;
	int           rv;

	if (((rv = nni_copyin_bool(&b, buf, sz, t)) != 0) || (c == NULL)) {
		return (rv);
	}
	val = b ? 1 : 0;
	fd  = nni_posix_pfd_fd(c->pfd);
	if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &val, sizeof(val)) != 0) {
		return (nni_plat_errno(errno));
	}
	return (0);
}

static int
tcp_conn_set_keepalive(void *arg, const void *buf, size_t sz, nni_type t)
{
	nni_tcp_conn *c = arg;
	int           fd;
	bool          b;
	int           val;
	int           rv;

	if (((rv = nni_copyin_bool(&b, buf, sz, t)) != 0) || (c == NULL)) {
		return (rv);
	}
	val = b ? 1 : 0;
	fd  = nni_posix_pfd_fd(c->pfd);
	if (setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &val, sizeof(val)) != 0) {
		return (nni_plat_errno(errno));
	}
	return (0);
}

static int
tcp_conn_get_nodelay(void *arg, void *buf, size_t *szp, nni_type t)
{
	nni_tcp_conn *c     = arg;
	int           fd    = nni_posix_pfd_fd(c->pfd);
	int           val   = 0;
	socklen_t     valsz = sizeof(val);

	if (getsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &val, &valsz) != 0) {
		return (nni_plat_errno(errno));
	}

	return (nni_copyout_bool(val, buf, szp, t));
}

static int
tcp_conn_get_keepalive(void *arg, void *buf, size_t *szp, nni_type t)
{
	nni_tcp_conn *c     = arg;
	int           fd    = nni_posix_pfd_fd(c->pfd);
	int           val   = 0;
	socklen_t     valsz = sizeof(val);

	if (getsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &val, &valsz) != 0) {
		return (nni_plat_errno(errno));
	}

	return (nni_copyout_bool(val, buf, szp, t));
}

static const nni_option tcp_conn_options[] = {
	{
	    .o_name = NNG_OPT_REMADDR,
	    .o_get  = tcp_conn_get_peername,
	},
	{
	    .o_name = NNG_OPT_LOCADDR,
	    .o_get  = tcp_conn_get_sockname,
	},
	{
	    .o_name = NNG_OPT_TCP_NODELAY,
	    .o_get  = tcp_conn_get_nodelay,
	    .o_set  = tcp_conn_set_nodelay,
	},
	{
	    .o_name = NNG_OPT_TCP_KEEPALIVE,
	    .o_get  = tcp_conn_get_keepalive,
	    .o_set  = tcp_conn_set_keepalive,
	},
	{
	    .o_name = NULL,
	},
};

int
nni_tcp_conn_getopt(
    nni_tcp_conn *c, const char *name, void *buf, size_t *szp, nni_type t)
{
	return (nni_getopt(tcp_conn_options, name, c, buf, szp, t));
}

int
nni_tcp_conn_setopt(
    nni_tcp_conn *c, const char *name, const void *buf, size_t sz, nni_type t)
{
	return (nni_setopt(tcp_conn_options, name, c, buf, sz, t));
}

int
nni_posix_tcp_conn_init(nni_tcp_conn **cp, nni_posix_pfd *pfd)
{
	nni_tcp_conn *c;

	if ((c = NNI_ALLOC_STRUCT(c)) == NULL) {
		return (NNG_ENOMEM);
	}

	c->closed = false;
	c->pfd    = pfd;

	nni_mtx_init(&c->mtx);
	nni_aio_list_init(&c->readq);
	nni_aio_list_init(&c->writeq);

	*cp = c;
	return (0);
}

void
nni_posix_tcp_conn_start(nni_tcp_conn *c, int nodelay, int keepalive)
{
	// COnfigure the initial socket options.
	(void) setsockopt(
	    c->pfd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(int));
	(void) setsockopt(
	    c->pfd, SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(int));

	nni_posix_pfd_set_cb(c->pfd, tcp_conn_cb, c);
}

void
nni_tcp_conn_fini(nni_tcp_conn *c)
{
	nni_tcp_conn_close(c);
	nni_posix_pfd_fini(c->pfd);
	nni_mtx_lock(&c->mtx); // not strictly needed, but shut up TSAN
	c->pfd = NULL;
	nni_mtx_unlock(&c->mtx);
	nni_mtx_fini(&c->mtx);

	NNI_FREE_STRUCT(c);
}
