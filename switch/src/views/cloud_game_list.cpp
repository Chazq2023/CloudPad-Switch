// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include "views/cloud_game_list.h"

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

		host->SetEventQuitCallback([host](ChiakiQuitEvent *quit) {
			brls::Application::unblockInputs();
			brls::Application::setMaximumFPS(60);
			brls::Application::notify(chiaki_quit_reason_string(quit->reason));
			brls::Application::popView();
			host->StopSession();
			host->FiniSession();
		});
		host->SetEventRumbleCallback(std::bind(&IO::SetRumble, io, std::placeholders::_1, std::placeholders::_2));
		host->SetReadControllerCallback(std::bind(&IO::UpdateControllerState, io, std::placeholders::_1, std::placeholders::_2));
		host->SetEventConnectedCallback([host]() {
			brls::Application::setMaximumFPS(0);
			brls::Application::pushView(new PSRemotePlay(host));
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

		int resolution_height = settings->ResolutionPresetToInt(settings->GetVideoResolution(nullptr));
		CloudGaikai gaikai(log, duid, "psnow", kamaji_result.platform, npsso,
			kamaji_result.entitlement_id, resolution_height);
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
		host->SetCloudConnectInfo(CHIAKI_SERVICE_TYPE_PSNOW, kamaji_result.platform,
			gaikai_result.server_ip, gaikai_result.server_port, gaikai_result.launch_spec,
			gaikai_result.handshake_key, gaikai_result.session_id, gaikai_result.psn_wrapper_type,
			gaikai_result.mtu_in, gaikai_result.mtu_out, gaikai_result.rtt_us);

		StartCloudStream(host);
	}
}

CloudGameList::CloudGameList(Settings *settings, ChiakiLog *log, std::string platform)
{
	CloudCatalog catalog(log);
	std::vector<CloudGame> games;
	std::string error;
	bool ok;

	brls::Application::blockInputs();
	if(platform == "ps5")
	{
		ok = catalog.FetchPs5CloudCatalog("en-US", &games, &error);
	}
	else
	{
		std::string npsso = settings->GetNPSSO();
		std::string duid = settings->GetOrCreateDUID();
		std::vector<CloudGame> all_games;
		ok = catalog.FetchPsnowCatalog(npsso, duid, &all_games, &error);
		if(ok)
		{
			for(const auto &g : all_games)
				if(g.platform == platform)
					games.push_back(g);
		}
	}
	brls::Application::unblockInputs();

	brls::ListItem *status = new brls::ListItem(platform == "ps5" ? "PS5 Cloud Catalog" : "PSNOW Catalog");
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
			// PS5 cloud streaming needs an entitlement id from the owned-games
			// cross-reference, which Phase 2 deliberately didn't port (see
			// cloudcatalog.h) - the catalog's own id is a content id, not an
			// entitlement id, so there's nothing valid to allocate a slot with yet.
			item->getClickEvent()->subscribe([](brls::View *view) {
				brls::Application::notify("PS5 cloud streaming needs entitlement lookup, which isn't wired up yet");
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
