// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include <chiaki/stoppipe.h>
#include <chiaki/sock.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <ws2tcpip.h>
#else
#include <unistd.h>
#include <sys/socket.h>
#include <sys/select.h>
#endif

CHIAKI_EXPORT ChiakiErrorCode chiaki_stop_pipe_init(ChiakiStopPipe *stop_pipe)
{
#ifdef _WIN32
	stop_pipe->event = WSACreateEvent();
	if(stop_pipe->event == WSA_INVALID_EVENT)
		return CHIAKI_ERR_UNKNOWN;
#elif defined(__SWITCH__)
	// currently pipe or socketpare are not available on switch
	// use a custom udp socket as pipe

	// struct sockaddr_in addr;
	int addr_size = sizeof(stop_pipe->addr);

	// Confirmed on-device across several rounds: sb_efficiency (buffer bytes)
	// doubled from 8 to 16 with zero effect; the retry window widened to 3
	// real seconds with zero effect (rules out a settling-time race); and
	// num_bsd_sessions raised to 18/20/24/32 all broke socketInitialize()/
	// nxlinkStdio() outright (total log silence) - 16 is the hard ceiling on
	// this device/firmware, so this can't be fixed by raising the session
	// budget either. The remaining explanation is a real leak: cloud session
	// establishment makes 14+ sequential HTTP calls (client IDs, config,
	// tokens, auth, lock, datacenter select, allocation) before this, the
	// app's first-ever UDP socket, tries to open - if each one leaves behind a
	// BSD session slot that's never reclaimed on close(), 14+ calls against a
	// budget of 16 explains the exhaustion exactly. Keeping a small retry as
	// a harmless safety net, then falling into a diagnostic probe below if it
	// still fails: opening and immediately closing a throwaway socket in a
	// loop tells us whether close() actually frees a slot at all (if it does,
	// every throwaway attempt should succeed since each is closed before the
	// next opens; if none succeed, the exhaustion is total and permanent by
	// this point, not something a session-budget bump could ever outrun).
	const int kMaxSocketAttempts = 3;
	for(int attempt = 0; attempt < kMaxSocketAttempts; attempt++)
	{
		errno = 0;
		stop_pipe->fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
		if(stop_pipe->fd >= 0)
			break;
		printf("[STOP PIPE] socket() failed (attempt %d/%d), errno=%d (%s)\n",
			attempt + 1, kMaxSocketAttempts, errno, strerror(errno));
		fflush(stdout);
		usleep(100000);
	}
	if(stop_pipe->fd < 0)
	{
		// Diagnostic only: probe whether close() actually reclaims a session
		// slot. Each iteration opens then immediately closes a throwaway UDP
		// socket - if slots are reclaimed properly, every iteration should
		// succeed; a failure here (especially on the very first iteration)
		// means the exhaustion is total, not something more retries or a
		// higher num_bsd_sessions could ever fix.
		const int kProbeCount = 20;
		int probe_successes = 0;
		for(int i = 0; i < kProbeCount; i++)
		{
			errno = 0;
			int probe_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
			if(probe_fd < 0)
			{
				printf("[STOP PIPE PROBE] throwaway socket() failed at iteration %d/%d, errno=%d (%s)\n",
					i + 1, kProbeCount, errno, strerror(errno));
				fflush(stdout);
				break;
			}
			probe_successes++;
			close(probe_fd);
		}
		printf("[STOP PIPE PROBE] %d/%d throwaway open+close cycles succeeded before failure\n",
			probe_successes, kProbeCount);
		fflush(stdout);
		return CHIAKI_ERR_UNKNOWN;
	}
	stop_pipe->addr.sin_family = AF_INET;
	// bind to localhost
	stop_pipe->addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	// use a random port (dedicate one socket per object)
	stop_pipe->addr.sin_port = htons(0);
	// bind on localhost
	printf("[STOP PIPE] probe: before bind()\n"); fflush(stdout);
	bind(stop_pipe->fd, (struct sockaddr *) &stop_pipe->addr, addr_size);
	printf("[STOP PIPE] probe: after bind()\n"); fflush(stdout);
	// listen
	getsockname(stop_pipe->fd, (struct sockaddr *) &stop_pipe->addr, &addr_size);
	printf("[STOP PIPE] probe: after getsockname()\n"); fflush(stdout);
	int r = fcntl(stop_pipe->fd, F_SETFL, O_NONBLOCK);
	printf("[STOP PIPE] probe: after fcntl(), r=%d\n", r); fflush(stdout);
	if(r == -1)
	{
		close(stop_pipe->fd);
		return CHIAKI_ERR_UNKNOWN;
	}
#else
	int r = pipe(stop_pipe->fds);
	if(r < 0)
		return CHIAKI_ERR_UNKNOWN;
	r = fcntl(stop_pipe->fds[0], F_SETFL, O_NONBLOCK);
	if(r == -1)
	{
		close(stop_pipe->fds[0]);
		close(stop_pipe->fds[1]);
		return CHIAKI_ERR_UNKNOWN;
	}
#endif
	return CHIAKI_ERR_SUCCESS;
}

CHIAKI_EXPORT void chiaki_stop_pipe_fini(ChiakiStopPipe *stop_pipe)
{
#ifdef _WIN32
	WSACloseEvent(stop_pipe->event);
#elif defined(__SWITCH__)
	close(stop_pipe->fd);
#else
	close(stop_pipe->fds[0]);
	close(stop_pipe->fds[1]);
#endif
}

CHIAKI_EXPORT void chiaki_stop_pipe_stop(ChiakiStopPipe *stop_pipe)
{
#ifdef _WIN32
	WSASetEvent(stop_pipe->event);
#elif defined(__SWITCH__)
	// send to local socket (FIXME MSG_CONFIRM)
	sendto(stop_pipe->fd, "\x00", 1, 0,
		(struct sockaddr*)&stop_pipe->addr, sizeof(struct sockaddr_in));
#else
	write(stop_pipe->fds[1], "\x00", 1);
#endif
}

CHIAKI_EXPORT ChiakiErrorCode chiaki_stop_pipe_select_single(ChiakiStopPipe *stop_pipe, chiaki_socket_t fd, bool write, uint64_t timeout_ms)
{
#ifdef _WIN32
	WSAEVENT events[2];
	DWORD events_count = 1;
	events[0] = stop_pipe->event;

	if(!CHIAKI_SOCKET_IS_INVALID(fd))
	{
		events_count = 2;
		events[1] = WSACreateEvent();
		if(events[1] == WSA_INVALID_EVENT)
			return CHIAKI_ERR_UNKNOWN;
		WSAEventSelect(fd, events[1], write ? FD_WRITE : FD_READ);
	}

	DWORD r = WSAWaitForMultipleEvents(events_count, events, FALSE, timeout_ms == UINT64_MAX ? WSA_INFINITE : (DWORD)timeout_ms, FALSE);

	if(events_count == 2)
		WSACloseEvent(events[1]);

	switch(r)
	{
		case WSA_WAIT_EVENT_0:
			return CHIAKI_ERR_CANCELED;
		case WSA_WAIT_EVENT_0+1:
			return CHIAKI_ERR_SUCCESS;
		case WSA_WAIT_TIMEOUT:
			return CHIAKI_ERR_TIMEOUT;
		default:
			return CHIAKI_ERR_UNKNOWN;
	}
#else
	fd_set rfds;
	FD_ZERO(&rfds);
#if defined(__SWITCH__)
	// push udp local socket as fd
	int stop_fd = stop_pipe->fd;
#else
	int stop_fd = stop_pipe->fds[0];
#endif
	FD_SET(stop_fd, &rfds);
	int nfds = stop_fd;

	fd_set wfds;
	FD_ZERO(&wfds);
	if(!CHIAKI_SOCKET_IS_INVALID(fd))
	{
		FD_SET(fd, write ? &wfds : &rfds);
		if(fd > nfds)
			nfds = fd;
	}
	nfds++;

	struct timeval timeout_s;
	struct timeval *timeout = NULL;
	if(timeout_ms != UINT64_MAX)
	{
		timeout_s.tv_sec = timeout_ms / 1000;
		timeout_s.tv_usec = (timeout_ms % 1000) * 1000;
		timeout = &timeout_s;
	}

	int r;
	do
	{
		r = select(nfds, &rfds, write ? &wfds : NULL, NULL, timeout);
#ifdef _WIN32
	} while(r < 0 && WSAGetLastError() == WSAEINTR)
#else
	} while(r < 0 && errno == EINTR);
#endif

	if(r < 0)
		return CHIAKI_ERR_UNKNOWN;

	if(FD_ISSET(stop_fd, &rfds))
		return CHIAKI_ERR_CANCELED;

	if(!CHIAKI_SOCKET_IS_INVALID(fd) && FD_ISSET(fd, write ? &wfds : &rfds))
		return CHIAKI_ERR_SUCCESS;

	return CHIAKI_ERR_TIMEOUT;
#endif
}

CHIAKI_EXPORT ChiakiErrorCode chiaki_stop_pipe_connect(ChiakiStopPipe *stop_pipe, chiaki_socket_t fd, struct sockaddr *addr, size_t addrlen, uint64_t timeout_ms)
{
	int r = connect(fd, addr, (socklen_t)addrlen);
	if(r >= 0)
		return CHIAKI_ERR_SUCCESS;

	if(CHIAKI_SOCKET_EINPROGRESS)
	{
		ChiakiErrorCode err = chiaki_stop_pipe_select_single(stop_pipe, fd, true, timeout_ms);
		if(err != CHIAKI_ERR_SUCCESS)
			return err;
	}
	else
	{
#ifdef _WIN32
		int err = WSAGetLastError();
		if(err == WSAECONNREFUSED)
			return CHIAKI_ERR_CONNECTION_REFUSED;
		else
			return CHIAKI_ERR_NETWORK;
#else
		if(errno == ECONNREFUSED)
			return CHIAKI_ERR_CONNECTION_REFUSED;
		else
			return CHIAKI_ERR_NETWORK;
#endif
	}

	struct sockaddr_storage peer;
	socklen_t peerlen = sizeof(peer);
	if(getpeername(fd, (struct sockaddr *)(&peer), &peerlen) == 0)
		return CHIAKI_ERR_SUCCESS;

#ifdef _WIN32
	int err = WSAGetLastError();
	if(err != WSAENOTCONN)
		return CHIAKI_ERR_UNKNOWN;
#else
	if(errno != ENOTCONN)
		return CHIAKI_ERR_UNKNOWN;
#endif

#ifdef _WIN32
	int sockerr;
	socklen_t sockerr_sz = sizeof(sockerr);
	if(getsockopt(fd, SOL_SOCKET, SO_ERROR, (char*)(&sockerr), &sockerr_sz) < 0)
		return CHIAKI_ERR_UNKNOWN;
#else
	int sockerr;
	socklen_t sockerr_sz = sizeof(sockerr);
	if(getsockopt(fd, SOL_SOCKET, SO_ERROR, &sockerr, &sockerr_sz) < 0)
		return CHIAKI_ERR_UNKNOWN;
#endif

#ifdef _WIN32
	switch(sockerr)
	{
		case WSAETIMEDOUT:
			return CHIAKI_ERR_TIMEOUT;
		case WSAECONNREFUSED:
			return CHIAKI_ERR_CONNECTION_REFUSED;
		case WSAEHOSTDOWN:
			return CHIAKI_ERR_HOST_DOWN;
		case WSAEHOSTUNREACH:
			return CHIAKI_ERR_HOST_UNREACH;
		default:
			return CHIAKI_ERR_UNKNOWN;
	}
#else
	switch(sockerr)
	{
		case ETIMEDOUT:
			return CHIAKI_ERR_TIMEOUT;
		case ECONNREFUSED:
			return CHIAKI_ERR_CONNECTION_REFUSED;
		case EHOSTDOWN:
			return CHIAKI_ERR_HOST_DOWN;
		case EHOSTUNREACH:
			return CHIAKI_ERR_HOST_UNREACH;
		default:
			return CHIAKI_ERR_UNKNOWN;
	}
#endif
}

CHIAKI_EXPORT ChiakiErrorCode chiaki_stop_pipe_reset(ChiakiStopPipe *stop_pipe)
{
#ifdef _WIN32
	BOOL r = WSAResetEvent(stop_pipe->event);
	return r ? CHIAKI_ERR_SUCCESS : CHIAKI_ERR_UNKNOWN;
#elif defined(__SWITCH__)
	//FIXME
	uint8_t v;
	int r;
	while((r = read(stop_pipe->fd, &v, sizeof(v))) > 0);
	return r < 0 ? CHIAKI_ERR_UNKNOWN : CHIAKI_ERR_SUCCESS;
#else
	uint8_t v;
	int r;
	while((r = read(stop_pipe->fds[0], &v, sizeof(v))) > 0);
	return r < 0 ? CHIAKI_ERR_UNKNOWN : CHIAKI_ERR_SUCCESS;
#endif
}
