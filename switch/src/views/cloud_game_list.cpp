// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include "views/cloud_game_list.h"

#include <algorithm>
#include <cctype>
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

	bool ContainsCaseInsensitive(const std::string &haystack, const std::string &needle)
	{
		auto to_lower = [](std::string s) {
			std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
			return s;
		};
		return to_lower(haystack).find(to_lower(needle)) != std::string::npos;
	}

	std::string GlyphForStreamable(StreamableState state)
	{
		// Plain ASCII rather than Unicode check/cross marks (✓/✕) - those
		// aren't in this app's bundled font, so they were rendering as an
		// identical fallback/missing-glyph box for every state, making a
		// confirmed-streamable title (Streamable) visually indistinguishable
		// from a confirmed-failed one (NotStreamable) on-device.
		switch(state)
		{
			case StreamableState::Streamable:
				return "OK";
			case StreamableState::NotStreamable:
				return "X";
			case StreamableState::Unknown:
			default:
				return "?";
		}
	}

	// Marks a title's real streamed outcome, both persisted (Settings) and on
	// its row's icon - called from every outcome point below (Kamaji/Gaikai
	// failure, session-start exception, or a real EventConnectedCallback).
	void RecordStreamOutcome(Settings *settings, const std::string &product_id, brls::DetailCell *item, bool success)
	{
		StreamableState state = success ? StreamableState::Streamable : StreamableState::NotStreamable;
		if(settings != nullptr && !product_id.empty())
		{
			settings->SetTitleStreamable(product_id, state);
			settings->WriteFile();
		}
		if(item != nullptr)
			item->setDetailText(GlyphForStreamable(state));
	}

	// Wires up the same connected/quit/rumble/controller callbacks
	// HostInterface sets for local sessions (switch/src/gui.cpp), without the
	// per-host settings list rows HostInterface also builds - a cloud stream
	// has no "host" to configure, just a session to start.
	void StartCloudStream(Host *host, Settings *settings, std::string product_id, brls::DetailCell *item)
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
		host->SetEventConnectedCallback([host, settings, product_id, item]() {
			// The source is at most 60 FPS. An uncapped Borealis loop
			// repeatedly uploaded and drew the same 1080p frame, wasting
			// enough CPU/GPU time to require the maximum Switch CPU
			// overclock.
			brls::Application::setLimitedFPS(60);
			RecordStreamOutcome(settings, product_id, item, true);
			PSRemotePlay *stream_view = new PSRemotePlay(host);
			brls::Application::pushActivity(new brls::Activity(stream_view));
			// A pushed Activity automatically binds BUTTON_START to
			// Application::quit() (the old fork's pushView bound Plus the
			// same way). That makes ZL+ZR+Plus close the entire NRO before
			// UpdateControllerState can turn it into a stream-exit request.
			// Consume Borealis's Start action on this activity's content view
			// only; IO still forwards Plus to the remote controller state,
			// and handles the full combo during draw.
			stream_view->registerAction("", brls::BUTTON_START, [](brls::View *) {
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
			RecordStreamOutcome(settings, product_id, item, false);
			brls::Application::notify(fmt::format("Failed to start stream: {}", e.what()));
		}
	}

	// Runs the full Kamaji (PSNOW auth) + Gaikai (allocation) flow and starts
	// the stream on success. Blocking, including through however long Sony's
	// allocation queue takes (up to ~15 minutes) - there's no cancel button or
	// progress UI beyond toast notifications yet; that's a known follow-up,
	// same as the "no CA bundle / no real datacenter ping" ones already called
	// out in cloudgaikai.h and cloudhttp.cpp.
	//
	// A background-thread + progress-dialog version of this was tried and
	// reverted: devkitA64/libnx reliably crashes in libstdc++'s
	// eh_globals_dtor (per-thread C++ exception-handling-state cleanup) when
	// a pthread-created worker thread doing this HTTP/JSON-heavy C++ work
	// exits - confirmed via an on-device Atmosphere crash report backtrace
	// (_EntryWrap -> __syscall_thread_exit -> threadExit -> eh_globals_dtor),
	// and reproduced identically whether that thread was detached or
	// explicitly joined via chiaki_thread_create/chiaki_thread_join. Running
	// synchronously on the main thread, as below, is the proven-stable
	// behavior.
	void StartPsnowGame(Settings *settings, ChiakiLog *log, const CloudGame &game, brls::DetailCell *item)
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
			RecordStreamOutcome(settings, game.product_id, item, false);
			brls::Application::notify(fmt::format("Sign in failed: {}", kamaji_result.error));
			return;
		}

		// PSNOW's PS3/PS4 H.264 path is fixed at 720p. This reduces packet and
		// render pressure while keeping allocation and decoder profiles aligned.
		const int resolution_height = settings->ResolutionPresetToInt(settings->GetPs34CatalogResolution());
		const int configured_bitrate_kbps = settings->GetPs34CatalogBitrateKbps();
		const int bitrate_kbps = configured_bitrate_kbps > 0 ? configured_bitrate_kbps : 10000;
		CloudGaikai gaikai(log, duid, "psnow", kamaji_result.platform, npsso,
			kamaji_result.entitlement_id, resolution_height, bitrate_kbps);
		CloudGaikai::Result gaikai_result = gaikai.Run([](const std::string &message) {
			brls::Application::notify(message);
		});

		if(!gaikai_result.success)
		{
			brls::Application::unblockInputs();
			RecordStreamOutcome(settings, game.product_id, item, false);
			brls::Application::notify(fmt::format("Streaming setup failed: {}", gaikai_result.error));
			return;
		}

		// Intentionally never freed: an app-lifetime cloud session, same
		// ownership simplicity local Host objects already have (they live in
		// Settings::hosts for the rest of the process too).
		Host *host = new Host("Cloud: " + game.name);
		// Copy the global settings explicitly so cloud hosts remain aligned
		// with the requested server profile if the Host defaults ever change.
		settings->SetVideoResolution(host, settings->GetPs34CatalogResolution());
		settings->SetVideoFPS(host, settings->GetVideoFPS(nullptr));
		host->SetCustomBitrateKbps(configured_bitrate_kbps);
		host->SetCloudConnectInfo(CHIAKI_SERVICE_TYPE_PSNOW, kamaji_result.platform,
			gaikai_result.server_ip, gaikai_result.server_port, gaikai_result.launch_spec,
			gaikai_result.handshake_key, gaikai_result.session_id, gaikai_result.psn_wrapper_type,
			gaikai_result.mtu_in, gaikai_result.mtu_out, gaikai_result.rtt_us);

		StartCloudStream(host, settings, game.product_id, item);
	}

	// PSCLOUD (PS5) skips CloudKamaji entirely - game.entitlement_id already
	// came straight from FetchOwnedPs5CloudGames's ownership cross-reference,
	// which is the PS5 equivalent of what Kamaji's product-id conversion does
	// for PSNOW (see cloudcatalog.h).
	void StartPscloudGame(Settings *settings, ChiakiLog *log, const CloudGame &game, brls::DetailCell *item)
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

		const int resolution_height = settings->ResolutionPresetToInt(settings->GetPs5LibraryResolution());
		const int configured_bitrate_kbps = settings->GetPs5LibraryBitrateKbps();
		const int bitrate_kbps = configured_bitrate_kbps > 0 ? configured_bitrate_kbps : 15000;
		CloudGaikai gaikai(log, duid, "pscloud", "ps5", npsso, game.entitlement_id,
			resolution_height, bitrate_kbps);
		CloudGaikai::Result gaikai_result = gaikai.Run([](const std::string &message) {
			brls::Application::notify(message);
		});

		if(!gaikai_result.success)
		{
			brls::Application::unblockInputs();
			RecordStreamOutcome(settings, game.product_id, item, false);
			brls::Application::notify(fmt::format("Streaming setup failed: {}", gaikai_result.error));
			return;
		}

		Host *host = new Host("Cloud: " + game.name);
		// Keep the client decoder and requested cloud profile in sync.
		settings->SetVideoResolution(host, settings->GetPs5LibraryResolution());
		settings->SetVideoFPS(host, settings->GetVideoFPS(nullptr));
		host->SetCustomBitrateKbps(configured_bitrate_kbps);
		host->SetCloudConnectInfo(CHIAKI_SERVICE_TYPE_PSCLOUD, "ps5",
			gaikai_result.server_ip, gaikai_result.server_port, gaikai_result.launch_spec,
			gaikai_result.handshake_key, gaikai_result.session_id, gaikai_result.psn_wrapper_type,
			gaikai_result.mtu_in, gaikai_result.mtu_out, gaikai_result.rtt_us);

		StartCloudStream(host, settings, game.product_id, item);
	}
}

CloudGameList::CloudGameList(Settings *settings, ChiakiLog *log, std::string platform)
	: settings(settings), log(log), platform(platform)
{
	this->rowsBox = new brls::Box();
	this->rowsBox->setAxis(brls::Axis::COLUMN);
	this->setContentView(this->rowsBox);

	this->registerAction("Search", brls::BUTTON_BACK, [this](brls::View *) {
		this->Search();
		return true;
	});

	this->BuildRows(false);
}

void CloudGameList::BuildRows(bool force_refresh)
{
	this->rowsBox->clearViews();
	this->status = nullptr;

	CloudCatalog catalog(this->log);
	std::vector<CloudGame> games;
	std::string error;
	bool ok;
	bool needs_login = this->settings->GetNPSSO().empty();

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
		if(this->platform == "ps5")
		{
			ok = catalog.FetchOwnedPs5CloudGames(this->settings->GetNPSSO(), "en-US", &games, &error, force_refresh);
		}
		else
		{
			std::string npsso = this->settings->GetNPSSO();
			std::string duid = this->settings->GetOrCreateDUID();
			std::vector<CloudGame> all_games;
			ok = catalog.FetchPsnowCatalog(npsso, duid, &all_games, &error, force_refresh);
			if(ok)
			{
				for(const auto &g : all_games)
					if(g.platform == this->platform)
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

	if(!this->filter.empty())
	{
		brls::DetailCell *reset_search = new brls::DetailCell();
		reset_search->setText("Reset Search");
		reset_search->registerClickAction([this](brls::View *view) {
			this->filter = "";
			this->BuildRows(false);
			return true;
		});
		this->rowsBox->addView(reset_search);
	}

	brls::DetailCell *refresh_item = new brls::DetailCell();
	refresh_item->setText("Refresh catalog");
	refresh_item->registerClickAction([this](brls::View *view) {
		this->Refresh();
		return true;
	});
	this->rowsBox->addView(refresh_item);

	this->status = new brls::DetailCell();
	this->status->setText(this->platform == "ps5" ? "PS5 Library" : "PSNOW Catalog");
	this->rowsBox->addView(this->status);

	if(!ok)
	{
		CHIAKI_LOGE(this->log, "CloudGameList: failed to load %s catalog: %s", this->platform.c_str(), error.c_str());
		this->status->setDetailText("Failed to load");
		brls::Label *error_label = new brls::Label();
		error_label->setText(error);
		error_label->setWidthPercentage(100);
		this->rowsBox->addView(error_label);
		return;
	}

	int shown_count = 0;
	bool is_ps5 = this->platform == "ps5";
	for(const auto &game : games)
	{
		if(!this->filter.empty() && !ContainsCaseInsensitive(game.name, this->filter))
			continue;

		brls::DetailCell *item = new brls::DetailCell();
		item->setText(game.name.empty() ? game.product_id : game.name);
		// A confirmed real outcome (a previous stream attempt) always wins;
		// otherwise fall back to the catalog-derived guess, which can only
		// ever suggest "streamable" - matches CloudPad Android's
		// applyStreamabilityHints precedence (PsCloudOwnership.kt).
		StreamableState initial_state = this->settings->GetTitleStreamable(game.product_id);
		if(initial_state == StreamableState::Unknown && game.catalog_streamable)
			initial_state = StreamableState::Streamable;
		item->setDetailText(GlyphForStreamable(initial_state));
		Settings *settings = this->settings;
		ChiakiLog *log = this->log;
		if(is_ps5)
		{
			item->registerClickAction([settings, log, game, item](brls::View *view) {
				StartPscloudGame(settings, log, game, item);
				return true;
			});
		}
		else
		{
			item->registerClickAction([settings, log, game, item](brls::View *view) {
				StartPsnowGame(settings, log, game, item);
				return true;
			});
		}
		this->rowsBox->addView(item);
		shown_count++;
	}

	this->status->setDetailText(this->filter.empty()
		? fmt::format("{} games", games.size())
		: fmt::format("{} of {} games", shown_count, games.size()));
}

void CloudGameList::Refresh()
{
	brls::Application::notify("Refreshing catalog...");
	this->BuildRows(true);
}

void CloudGameList::Search()
{
	auto submit_cb = [this](std::string text) {
		this->filter = text;
		this->BuildRows(false);
	};

	brls::Application::getImeManager()->openForText(submit_cb, "Search titles", "", 64, this->filter, 0);
}
