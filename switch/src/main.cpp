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

	// libnx's bsdInitialize() (nx/source/services/bsd.c,
	// _bsdGetTransferMemSizeForConfig) sizes its transfer memory pool as
	// sb_efficiency * sum-of-buffers, which looked like it explained every
	// earlier "raising udp_rx_buf_size breaks socket creation" result as a
	// total-memory problem - so this was tried: sb_efficiency 16->4 (this
	// app only ever has 4 concurrent UDP sockets, not 16) while raising
	// udp_rx_buf_size to the full 4MB Takion actually wants (TAKION_A_RWND),
	// for a total LOWER than what already worked (~18.25MB vs ~25MB).
	// Confirmed on-device this theory is wrong: it broke boot outright
	// (0/20 throwaway sockets, ENOBUFS on every single HTTP call) despite
	// using less total memory than the working 1MB config. That rules out
	// total pool size as the real constraint and confirms the ceiling is on
	// the individual udp_rx_buf_size value itself, independent of
	// sb_efficiency - matching the very first bisection done on this buffer
	// long before the tmem formula was known (512KB worked, 2MB broke it
	// outright). Reverted to the known-working 1MB/16 pair.
	.udp_tx_buf_size = 0x10000,
	.udp_rx_buf_size = 0x100000, // 1MB - confirmed hard ceiling sits below 2MB, independent of sb_efficiency

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
