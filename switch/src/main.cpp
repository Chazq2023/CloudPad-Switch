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

	// Found the actual mechanism by reading libnx's source
	// (nx/source/services/bsd.c, _bsdGetTransferMemSizeForConfig): the total
	// transfer memory bsdInitialize() allocates - a SINGLE shared pool backing
	// every socket for the whole session - is
	//   sb_efficiency * page_round(tcp_tx_buf_max_size + tcp_rx_buf_max_size
	//                              + udp_tx_buf_size + udp_rx_buf_size)
	// sb_efficiency is a CONCURRENCY multiplier (how many sockets can be at
	// full configured size simultaneously), not a per-socket size booster.
	// Every earlier round of "raising udp_rx_buf_size breaks socket creation"
	// (5MB broke it with 2 sockets open, then 2MB broke it too) was really
	// this total allocation exceeding what a homebrew process's heap can
	// provide - at the old sb_efficiency=16 the 1MB udp_rx_buf_size already
	// in use here means ~25MB of transfer memory, for a multiplier (16
	// concurrent full-size sockets) this app never actually uses: there are
	// only ever 4 concurrent UDP sockets at streaming time (3 stop-pipes,
	// now shrunk to 2KB each post-creation via setsockopt in stoppipe.c, plus
	// Takion's own). Dropping sb_efficiency to 4 (comfortably above the real
	// count, matching libnx's own stock default) while raising
	// udp_rx_buf_size to the full 0x400000 (4MB) Takion has always actually
	// requested (TAKION_A_RWND, lib/src/takion.c) - previously always
	// silently clamped down to whatever this value was - comes to
	// 4 * 0x490000 =~ 18.25MB, LESS total memory than the 25MB already
	// working today. First real test of Takion getting its native buffer
	// size instead of a clamped-down one.
	.udp_tx_buf_size = 0x10000,
	.udp_rx_buf_size = 0x400000, // 4MB, matches TAKION_A_RWND

	.sb_efficiency = 4,

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
