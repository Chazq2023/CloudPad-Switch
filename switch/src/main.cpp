// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

// chiaki modules
#include <chiaki/discovery.h>
#include <chiaki/log.h>

#include "gui.h"
#include "io.h"
#include "settings.h"

#ifdef __SWITCH__
#include <switch.h>
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

	// Takion (lib/src/takion.c, TAKION_A_RWND) always requests a 4MB
	// SO_RCVBUF on its UDP socket - libnx's UDP buffers are a fixed size (no
	// "max" growth field like TCP has), so udp_rx_buf_size has to be at least
	// that big or the setsockopt call fails outright with ENOBUFS ("No buffer
	// space available") the moment a stream actually tries to connect. This
	// was never caught before because nothing in this app's history had
	// reached an actual Takion connection attempt on real hardware yet.
	.udp_tx_buf_size = 0x40000,
	.udp_rx_buf_size = 0x500000, // 5MB: 4MB required + headroom

	// The shared socket-buffer pool size is sb_efficiency * (tcp_tx_buf_max_size
	// + tcp_rx_buf_max_size + udp_tx_buf_size + udp_rx_buf_size). Doubling this
	// from 8 to 16 was tried on-device and made zero difference to the
	// stop-pipe ENOBUFS failure below (identical errno=105 every attempt, and
	// nxlink/socketInitialize still came up fine at this size, which it would
	// NOT have if tmemCreate() itself had failed to allocate the pool) - so
	// buffer bytes are not the actual constraint here. Left doubled anyway
	// since it's proven harmless and gives real headroom for the
	// udp_rx_buf_size bump above.
	.sb_efficiency = 16,

	// Cloud session establishment makes 14+ sequential HTTP calls (client IDs,
	// config, tokens, auth, lock, datacenter select, allocation), each via its
	// own curl_easy_init/cleanup pair - closed properly on the client side. By
	// the time chiaki_stop_pipe_init opens this app's first-ever UDP socket
	// right after, it fails with ENOBUFS on every one of 10 retry attempts
	// spread across 3 real seconds (see lib/src/stoppipe.c) - not a timing
	// race, a hard/permanent exhaustion. Since sb_efficiency (buffer bytes)
	// provably isn't it, and nothing leaks a client-side fd, the remaining
	// candidate is this session budget itself: 16 is proven insufficient, 32
	// broke socketInitialize()/nxlinkStdio() outright (total silence, no logs
	// at all). Bisecting to 24 as the next untested value between those two.
	.num_bsd_sessions = 24,
	.bsd_service_type = BsdServiceType_User,
};
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
