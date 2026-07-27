// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

// chiaki modules
#include <chiaki/discovery.h>
#include <chiaki/log.h>

#include "gui.h"
#include "io.h"
#include "settings.h"

#ifdef __SWITCH__
#include <switch.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#else
bool appletMainLoop()
{
	return true;
}
#endif

#if __SWITCH__
#define CHIAKI_ENABLE_SWITCH_NXLINK 1
#endif

#ifdef __SWITCH__
// use a custom nintendo switch socket config
// chiaki requiers many threads with udp/tcp sockets
static const SocketInitConfig g_chiakiSocketInitConfig = {

	.tcp_tx_buf_size = 0x8000,
	.tcp_rx_buf_size = 0x10000,
	.tcp_tx_buf_max_size = 0x40000,
	.tcp_rx_buf_max_size = 0x40000,

	// A boot-time diagnostic probe (right after nxlink comes up, before any of
	// the app's own code runs - see ProbeSocketBudgetAtBoot below) proved this
	// pool is exhausted after the SECOND socket the process ever creates: one
	// TCP connection (nxlink's own persistent stdout link) plus one throwaway
	// UDP socket, and the UDP one already fails outright with ENOBUFS. That
	// ruled out every earlier theory in one shot: it's not a leak across the
	// cloud session's HTTP calls (fails before any of them run), not a
	// session-count problem (num_bsd_sessions=16 is nowhere near used up by 2
	// sockets), and sb_efficiency scaling (8->16, zero measured effect) was
	// never the real lever - the actual effective pool is evidently far
	// smaller than the nominal sb_efficiency * sum-of-buffers formula
	// suggests, likely clamped by the sysmodule itself. udp_rx_buf_size was
	// 0x500000 (5MB) - sized for Takion's later 4MB SO_RCVBUF requirement
	// (lib/src/takion.c, TAKION_A_RWND) - but on Switch that default is
	// reserved for EVERY UDP socket at creation, not just Takion's, so even
	// this app's trivial loopback stop-pipe signaling socket was demanding 5MB
	// up front. Against a tiny real pool, nxlink's TCP socket plus one 5MB UDP
	// reservation was apparently already at or past the edge. Shrinking this
	// down to 0x10000 unblocked the whole connection flow (confirmed on-device:
	// full session/Takion handshake succeeded), but on-device streaming then
	// showed severe packet loss and no audio - Takion's own setsockopt call
	// requesting 4MB (lib/src/takion.c:278) can't grow the socket past this
	// process-wide default at all (Switch UDP sockets are fixed-size from
	// creation, confirmed via TAKION_RCVBUF_DETAIL logging actual=65536 despite
	// requesting 4194304), and 64KB is nowhere near enough to absorb bursts at
	// the ~25-48 Mbit/s this stream negotiates before the kernel starts
	// dropping packets. Bisecting upward from the proven-safe 0x10000 toward
	// something Takion can actually use, without getting anywhere near the 5MB
	// that broke the connection outright with just 2 sockets open.
	.udp_tx_buf_size = 0x10000,
	.udp_rx_buf_size = 0x80000, // 512KB

	.sb_efficiency = 16,

	.num_bsd_sessions = 16,
	.bsd_service_type = BsdServiceType_User,
};
#endif // __SWITCH__

#ifdef __SWITCH__
// Diagnostic only: establishes the actual starting socket budget right after
// nxlink comes up, before any of the app's own HTTP code runs. On-device,
// the throwaway-socket probe inside a cloud stream launch (switch/src/
// cloudhttp.cpp) already showed 0/20 succeeding after just the FIRST
// HttpRequest call of that launch attempt - meaning something earlier in the
// same app session (login, catalog browsing/pagination) had already
// exhausted the budget before the launch flow even began. This runs at boot,
// before any of that, to see whether the budget starts full (~16) or is
// already suspiciously low from something in app init itself (e.g. nxlink's
// own permanently-open debug socket).
static void ProbeSocketBudgetAtBoot()
{
	int successes = 0;
	int fds[20];
	for(int i = 0; i < 20; i++)
	{
		errno = 0;
		fds[i] = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
		if(fds[i] < 0)
		{
			printf("[BOOT PROBE] throwaway socket() failed at iteration %d/20, errno=%d (%s)\n",
				i + 1, errno, strerror(errno));
			fflush(stdout);
			break;
		}
		successes++;
	}
	printf("[BOOT PROBE] %d/20 throwaway sockets opened successfully at boot (closing them all now)\n", successes);
	fflush(stdout);
	for(int i = 0; i < successes; i++)
		close(fds[i]);
}
#endif // __SWITCH__

#ifdef CHIAKI_ENABLE_SWITCH_NXLINK
static int s_nxlinkSock = -1;

static void initNxLink()
{
	// use chiaki socket config initialization
	if(R_FAILED(socketInitialize(&g_chiakiSocketInitConfig)))
		return;

	s_nxlinkSock = nxlinkStdio();
	if(s_nxlinkSock >= 0)
	{
		// stdout is a socket now, not a TTY, so libc defaults to full
		// buffering - on a hard crash whatever's unflushed is lost. Force
		// unbuffered so every line reaches the nxlink listener immediately,
		// otherwise crash diagnostics over this link are unreliable.
		setvbuf(stdout, NULL, _IONBF, 0);
		printf("initNxLink\n");
		ProbeSocketBudgetAtBoot();
	}
	else
		socketExit();
}

static void deinitNxLink()
{
	if(s_nxlinkSock >= 0)
	{
		close(s_nxlinkSock);
		s_nxlinkSock = -1;
	}
}
#endif // CHIAKI_ENABLE_SWITCH_NXLINK

#ifdef __SWITCH__
extern "C" void userAppInit()
{
#ifdef CHIAKI_ENABLE_SWITCH_NXLINK
	initNxLink();
#endif
	// to load gui resources
	romfsInit();
	plInitialize(PlServiceType_User);
	// load socket custom config
	socketInitialize(&g_chiakiSocketInitConfig);
	setsysInitialize();
}

extern "C" void userAppExit()
{
#ifdef CHIAKI_ENABLE_SWITCH_NXLINK
	deinitNxLink();
#endif // CHIAKI_ENABLE_SWITCH_NXLINK
	socketExit();
	/* Cleanup tesla required services. */
	hidsysExit();
	pmdmntExit();
	plExit();

	/* Cleanup default services. */
	fsExit();
	hidExit();
	appletExit();
	setsysExit();
	smExit();
}
#endif // __SWITCH__

int main(int argc, char *argv[])
{
	// load chiaki lib
	Settings *settings = Settings::GetInstance();
	ChiakiLog *log = settings->GetLogger();

	CHIAKI_LOGI(log, "Loading chaki lib");

	ChiakiErrorCode err = chiaki_lib_init();
	if(err != CHIAKI_ERR_SUCCESS)
	{
		CHIAKI_LOGE(log, "Chiaki lib init failed: %s\n", chiaki_error_string(err));
		return 1;
	}

	CHIAKI_LOGI(log, "Loading SDL audio / joystick / haptic");
	if(SDL_Init(SDL_INIT_AUDIO | SDL_INIT_JOYSTICK))
	{
		CHIAKI_LOGE(log, "SDL initialization failed: %s", SDL_GetError());
		return 1;
	}

	// build sdl OpenGl and AV decoders graphical interface
	{
		// scope to delete MainApplication before SDL_Quit()
		MainApplication app;
		app.Load();
	}

	CHIAKI_LOGI(log, "Quit applet");
	SDL_Quit();
	return 0;
}
