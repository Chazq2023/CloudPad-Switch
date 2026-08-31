// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include "views/cloud_game_list.h"

#include <algorithm>
#include <strings.h>

#include <fmt/format.h>

#include <chiaki/common.h>

#include "cloudcatalog.h"
#include "cloudkamaji.h"
#include "cloudgaikai.h"
#include "exception.h"
#include "host.h"
#include "io.h"
#include "views/ps_remote_play.h"

namespace
{
	// Matches KamajiConsts::REDIRECT_URI/USER_AGENT (gui/include/cloudstreaming/pskamajisession.h) -
	// CloudKamaji needs these passed in since it has no Settings access of its own.
	const char *kPsnowRedirectUri = "https://psnow.playstation.com/app/2.2.0/133/5cdcc037d/grc-response.html";
	const char *kPsnowUserAgent =
		"Mozilla/5.0 (Windows NT 10.0; WOW64) AppleWebKit/537.36 (KHTML, like Gecko) "
		"playstation-now/0.0.0 Chrome/83.0.4103.104 Electron/9.0.4 Safari/537.36 gkApollo";

	// Wires up the same connected/quit/rumble/controller callbacks
	// HostInterface sets for local sessions (switch/src/gui.cpp), without the
	// per-host settings list rows HostInterface also builds - a cloud stream
	// has no "host" to configure, just a session to start.
	void StartCloudStream(Host *host)
	{
		IO *io = IO::GetInstance();

		host->SetEventQuitCallback([io](ChiakiQuitEvent *quit) {
			// Chiaki invokes this callback on its session worker thread. Joining
			// or manipulating Borealis views here can self-join the worker and
			// race the UI thread. Let PSRemotePlay::draw perform all teardown on
			// the main thread instead.
			(void)quit;
			io->exit_stream_requested.store(true);
		});
		host->SetEventRumbleCallback(std::bind(&IO::SetRumble, io, std::placeholders::_1, std::placeholders::_2));
		host->SetReadControllerCallback(std::bind(&IO::UpdateControllerState, io, std::placeholders::_1, std::placeholders::_2));
		host->SetEventConnectedCallback([host]() {
			// The source is at most 60 FPS. An uncapped Borealis loop repeatedly
			// uploaded and drew the same 1080p frame, wasting enough CPU/GPU time
			// to require the maximum Switch CPU overclock.
			brls::Application::setMaximumFPS(60);
			PSRemotePlay *stream_view = new PSRemotePlay(host);
			brls::Application::pushView(stream_view);
			// pushView automatically binds Plus to Application::quit(). That
			// makes ZL+ZR+Plus close the entire NRO before UpdateControllerState
			// can turn it into a stream-exit request. Consume Borealis's Plus
			// action on this view only; IO still forwards Plus to the remote
			// controller state, and handles the full combo during draw.
			stream_view->registerAction("", brls::Key::PLUS, []() {
				return true;
			}, true);
		});

		try
		{
			host->InitSession(io);
			host->StartSession();
		}
		catch(const Exception &e)
		{
			brls::Application::unblockInputs();
			brls::Application::notify(fmt::format("Failed to start stream: {}", e.what()));
		}
	}

	// Runs the full Kamaji (PSNOW auth) + Gaikai (allocation) flow and starts
	// the stream on success. Blocking, including through however long Sony's
	// allocation queue takes (up to ~15 minutes) - there's no cancel button or
	// progress UI beyond toast notifications yet; that's a known follow-up,
	// same as the "no CA bundle / no real datacenter ping" ones already called
	// out in cloudgaikai.h and cloudhttp.cpp.
	void StartPsnowGame(Settings *settings, ChiakiLog *log, const CloudGame &game)
	{
		std::string npsso = settings->GetNPSSO();
		std::string duid = settings->GetOrCreateDUID();

		brls::Application::blockInputs();
		brls::Application::notify("Signing in to PlayStation Now...");

		CloudKamaji kamaji(log, duid, game.product_id, npsso, kPsnowRedirectUri, kPsnowUserAgent);
		CloudKamaji::Result kamaji_result = kamaji.Run();
		if(!kamaji_result.success)
		{
			brls::Application::unblockInputs();
			brls::Application::notify(fmt::format("Sign in failed: {}", kamaji_result.error));
			return;
		}

		// PSNOW's PS3/PS4 H.264 path is fixed at 720p. This reduces packet and
		// render pressure while keeping allocation and decoder profiles aligned.
		const int resolution_height = 720;
		const int configured_bitrate_kbps = settings->GetCustomBitrateKbps();
		const int bitrate_kbps = configured_bitrate_kbps > 0 ? configured_bitrate_kbps : 10000;
		CloudGaikai gaikai(log, duid, "psnow", kamaji_result.platform, npsso,
			kamaji_result.entitlement_id, resolution_height, bitrate_kbps);
		CloudGaikai::Result gaikai_result = gaikai.Run([](const std::string &message) {
			brls::Application::notify(message);
		});

		if(!gaikai_result.success)
		{
			brls::Application::unblockInputs();
			brls::Application::notify(fmt::format("Streaming setup failed: {}", gaikai_result.error));
			return;
		}

		// Intentionally never freed: an app-lifetime cloud session, same
		// ownership simplicity local Host objects already have (they live in
		// Settings::hosts for the rest of the process too).
		Host *host = new Host("Cloud: " + game.name);
		// Copy the global settings explicitly so cloud hosts remain aligned
		// with the requested server profile if the Host defaults ever change.
		settings->SetVideoResolution(host, CHIAKI_VIDEO_RESOLUTION_PRESET_720p);
		settings->SetVideoFPS(host, settings->GetVideoFPS(nullptr));
		host->SetCloudConnectInfo(CHIAKI_SERVICE_TYPE_PSNOW, kamaji_result.platform,
			gaikai_result.server_ip, gaikai_result.server_port, gaikai_result.launch_spec,
			gaikai_result.handshake_key, gaikai_result.session_id, gaikai_result.psn_wrapper_type,
			gaikai_result.mtu_in, gaikai_result.mtu_out, gaikai_result.rtt_us);

		StartCloudStream(host);
	}

	// PSCLOUD (PS5) skips CloudKamaji entirely - game.entitlement_id already
	// came straight from FetchOwnedPs5CloudGames's ownership cross-reference,
	// which is the PS5 equivalent of what Kamaji's product-id conversion does
	// for PSNOW (see cloudcatalog.h).
	void StartPscloudGame(Settings *settings, ChiakiLog *log, const CloudGame &game)
	{
		if(game.entitlement_id.empty())
		{
			brls::Application::notify("This title is missing an entitlement id - try refreshing the PS5 library");
			return;
		}

		std::string npsso = settings->GetNPSSO();
		std::string duid = settings->GetOrCreateDUID();

		brls::Application::blockInputs();
		brls::Application::notify("Starting PS5 cloud stream...");

		const int resolution_height = 1080;
		const int configured_bitrate_kbps = settings->GetCustomBitrateKbps();
		const int bitrate_kbps = configured_bitrate_kbps > 0 ? configured_bitrate_kbps : 15000;
		CloudGaikai gaikai(log, duid, "pscloud", "ps5", npsso, game.entitlement_id,
			resolution_height, bitrate_kbps);
		CloudGaikai::Result gaikai_result = gaikai.Run([](const std::string &message) {
			brls::Application::notify(message);
		});

		if(!gaikai_result.success)
		{
			brls::Application::unblockInputs();
			brls::Application::notify(fmt::format("Streaming setup failed: {}", gaikai_result.error));
			return;
		}

		Host *host = new Host("Cloud: " + game.name);
		// Keep the client decoder and requested cloud profile in sync.
		settings->SetVideoResolution(host, CHIAKI_VIDEO_RESOLUTION_PRESET_1080p);
		settings->SetVideoFPS(host, settings->GetVideoFPS(nullptr));
		host->SetCloudConnectInfo(CHIAKI_SERVICE_TYPE_PSCLOUD, "ps5",
			gaikai_result.server_ip, gaikai_result.server_port, gaikai_result.launch_spec,
			gaikai_result.handshake_key, gaikai_result.session_id, gaikai_result.psn_wrapper_type,
			gaikai_result.mtu_in, gaikai_result.mtu_out, gaikai_result.rtt_us);

		StartCloudStream(host);
	}
}

CloudGameList::CloudGameList(Settings *settings, ChiakiLog *log, std::string platform,
	brls::TabFrame *root_frame, bool force_refresh)
	: settings(settings), log(log), platform(platform), root_frame(root_frame)
{
	CloudCatalog catalog(log);
	std::vector<CloudGame> games;
	std::string error;
	bool ok;
	bool needs_login = settings->GetNPSSO().empty();

	// PS5's "Library" only ever lists titles this account owns (PS Plus
	// Premium cloud streaming is per-title-owned, not subscription-wide like
	// PSNOW), so unlike PSNOW there's nothing useful to show while signed
	// out - matches upstream's Android showLoginRequiredState().
	if(needs_login)
	{
		ok = false;
		error = "Sign in with PlayStation (Account tab) to see this library";
	}
	else
	{
		brls::Application::blockInputs();
		if(platform == "ps5")
		{
			ok = catalog.FetchOwnedPs5CloudGames(settings->GetNPSSO(), "en-US", &games, &error, force_refresh);
		}
		else
		{
			std::string npsso = settings->GetNPSSO();
			std::string duid = settings->GetOrCreateDUID();
			std::vector<CloudGame> all_games;
			ok = catalog.FetchPsnowCatalog(npsso, duid, &all_games, &error, force_refresh);
			if(ok)
			{
				for(const auto &g : all_games)
					if(g.platform == platform)
						games.push_back(g);
			}
		}
		brls::Application::unblockInputs();

		if(ok)
		{
			std::sort(games.begin(), games.end(), [](const CloudGame &a, const CloudGame &b) {
				return strcasecmp(a.name.c_str(), b.name.c_str()) < 0;
			});
		}
	}

	brls::ListItem *refresh_item = new brls::ListItem("Refresh catalog");
	refresh_item->getClickEvent()->subscribe([this](brls::View *view) {
		this->Refresh();
	});
	this->addView(refresh_item);

	brls::ListItem *status = new brls::ListItem(platform == "ps5" ? "PS5 Library" : "PSNOW Catalog");
	status->setValue(ok ? fmt::format("{} games", games.size()) : "Failed to load");
	this->addView(status);

	if(!ok)
	{
		CHIAKI_LOGE(log, "CloudGameList: failed to load %s catalog: %s", platform.c_str(), error.c_str());
		brls::Label *error_label = new brls::Label(brls::LabelStyle::REGULAR, error, true);
		this->addView(error_label);
		return;
	}

	for(const auto &game : games)
	{
		brls::ListItem *item = new brls::ListItem(game.name.empty() ? game.product_id : game.name);
		if(platform == "ps5")
		{
			item->getClickEvent()->subscribe([settings, log, game](brls::View *view) {
				StartPscloudGame(settings, log, game);
			});
		}
		else
		{
			item->getClickEvent()->subscribe([settings, log, game](brls::View *view) {
				StartPsnowGame(settings, log, game);
			});
		}
		this->addView(item);
	}
}

void CloudGameList::SetSidebarItem(brls::SidebarItem *item)
{
	this->sidebar_item = item;
}

void CloudGameList::Refresh()
{
	if(!this->sidebar_item || !this->root_frame)
		return;

	brls::Application::notify("Refreshing catalog...");

	// Build a fresh replacement rather than mutating this list's own rows in
	// place - Borealis's BoxLayout::removeView is documented as unsafe to use
	// at runtime. TabFrame::switchToView's own tab-switching path already
	// does the equivalent swap safely (removeView(1, false), i.e. without
	// freeing), so retargeting the SidebarItem at a new view and asking the
	// frame to switch to it reuses that same proven path instead of a new one.
	CloudGameList *fresh = new CloudGameList(this->settings, this->log, this->platform, this->root_frame, true);
	fresh->SetSidebarItem(this->sidebar_item);
	this->sidebar_item->setAssociatedView(fresh);
	this->root_frame->switchToView(fresh);
}
