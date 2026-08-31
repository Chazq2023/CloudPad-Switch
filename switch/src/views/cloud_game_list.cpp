// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include "views/cloud_game_list.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <memory>
#include <mutex>
#include <strings.h>
#include <thread>

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
		switch(state)
		{
			case StreamableState::Streamable:
				return "✓"; // check mark
			case StreamableState::NotStreamable:
				return "✕"; // multiplication x
			case StreamableState::Unknown:
			default:
				return "?";
		}
	}

	// Shared between the click handler (main thread), the background thread
	// that runs the actual sign-in/allocation/session-start flow, and
	// StreamStartDialog's per-frame poll (main thread, via draw()) - see
	// LaunchStream below for how these three fit together.
	struct StreamStartState
	{
		std::mutex mutex;
		std::string stage_text = "Starting stream...";
		std::string error;

		// done/success/host follow the standard atomic-flag-guards-plain-data
		// pattern: host is written on the worker thread strictly before the
		// done/success stores, and only ever read on the main thread after
		// observing done == true, so the atomic store/load pair is enough to
		// make that plain write visible - no separate lock needed for it.
		std::atomic<bool> done{ false };
		std::atomic<bool> success{ false };
		Host *host = nullptr;

		// Known up front on the main thread, only ever touched from there
		// (in StreamStartDialog::Poll once done == true).
		Settings *settings = nullptr;
		std::string product_id;
		brls::ListItem *item = nullptr;

		void SetStage(const std::string &text)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			this->stage_text = text;
		}

		void Fail(const std::string &message)
		{
			{
				std::lock_guard<std::mutex> lock(this->mutex);
				this->error = message;
			}
			this->success.store(false);
			this->done.store(true);
		}

		void Succeed(Host *connected_host)
		{
			this->host = connected_host;
			this->success.store(true);
			this->done.store(true);
		}
	};

	// A non-cancelable modal showing live stage text while a stream starts.
	// Polls `state` from draw() (called every frame regardless of dirty
	// state) rather than any new cross-thread dispatch mechanism - this
	// mirrors the existing precedent in this codebase for handing control
	// back to the main thread from a background/session thread (see
	// PSRemotePlay::draw polling IO::exit_stream_requested, and the comment
	// on SetEventQuitCallback below).
	class StreamStartDialog : public brls::Dialog
	{
		public:
			StreamStartDialog(std::shared_ptr<StreamStartState> state, brls::Label *label)
				: brls::Dialog(label), state(std::move(state)), label(label)
			{
				this->setCancelable(false);
			}

			void draw(NVGcontext *vg, int x, int y, unsigned width, unsigned height,
				brls::Style *style, brls::FrameContext *ctx) override
			{
				brls::Dialog::draw(vg, x, y, width, height, style, ctx);
				this->Poll();
			}

		private:
			std::shared_ptr<StreamStartState> state;
			brls::Label *label;
			bool finished = false;

			void Poll()
			{
				if(this->finished)
					return;

				std::string stage_text;
				{
					std::lock_guard<std::mutex> lock(this->state->mutex);
					stage_text = this->state->stage_text;
				}
				this->label->setText(stage_text);

				if(!this->state->done.load())
					return;

				this->finished = true;

				bool success = this->state->success.load();
				Host *host = this->state->host;
				std::string error;
				{
					std::lock_guard<std::mutex> lock(this->state->mutex);
					error = this->state->error;
				}

				if(this->state->settings != nullptr && !this->state->product_id.empty())
				{
					this->state->settings->SetTitleStreamable(this->state->product_id,
						success ? StreamableState::Streamable : StreamableState::NotStreamable);
					this->state->settings->WriteFile();
				}
				if(this->state->item != nullptr)
					this->state->item->setValue(GlyphForStreamable(
						success ? StreamableState::Streamable : StreamableState::NotStreamable));

				this->close([success, host, error]() {
					if(success && host != nullptr)
					{
						// The source is at most 60 FPS. An uncapped Borealis loop
						// repeatedly uploaded and drew the same 1080p frame, wasting
						// enough CPU/GPU time to require the maximum Switch CPU
						// overclock.
						brls::Application::setMaximumFPS(60);
						PSRemotePlay *stream_view = new PSRemotePlay(host);
						brls::Application::pushView(stream_view);
						// pushView automatically binds Plus to Application::quit().
						// That makes ZL+ZR+Plus close the entire NRO before
						// UpdateControllerState can turn it into a stream-exit
						// request. Consume Borealis's Plus action on this view
						// only; IO still forwards Plus to the remote controller
						// state, and handles the full combo during draw.
						stream_view->registerAction("", brls::Key::PLUS, []() {
							return true;
						}, true);
					}
					else if(!error.empty())
					{
						brls::Application::notify(error);
					}
				});
			}
	};

	// Wires up the same connected/quit/rumble/controller callbacks
	// HostInterface sets for local sessions (switch/src/gui.cpp), without the
	// per-host settings list rows HostInterface also builds - a cloud stream
	// has no "host" to configure, just a session to start. Runs on the
	// background thread LaunchStream spawns; must not touch Borealis
	// directly - state carries the outcome back to StreamStartDialog::Poll
	// on the main thread instead.
	void StartCloudStream(Host *host, std::shared_ptr<StreamStartState> state)
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
		host->SetEventConnectedCallback([host, state]() {
			// Also runs on chiaki's worker thread - just record the outcome,
			// StreamStartDialog::Poll does the actual view push on the main
			// thread once it observes state->done.
			state->Succeed(host);
		});

		try
		{
			host->InitSession(io);
			host->StartSession();
		}
		catch(const Exception &e)
		{
			state->Fail(fmt::format("Failed to start stream: {}", e.what()));
		}
	}

	// Runs the full Kamaji (PSNOW auth) + Gaikai (allocation) flow and starts
	// the stream on success, entirely on a background thread (see
	// LaunchStream) - blocking, including through however long Sony's
	// allocation queue takes (up to ~15 minutes), with StreamStartDialog
	// showing live progress instead of freezing the UI thread for that whole
	// window like earlier synchronous builds did.
	void StartPsnowGame(Settings *settings, ChiakiLog *log, const CloudGame &game, std::shared_ptr<StreamStartState> state)
	{
		std::string npsso = settings->GetNPSSO();
		std::string duid = settings->GetOrCreateDUID();

		state->SetStage("Signing in to PlayStation Now...");

		CloudKamaji kamaji(log, duid, game.product_id, npsso, kPsnowRedirectUri, kPsnowUserAgent);
		CloudKamaji::Result kamaji_result = kamaji.Run();
		if(!kamaji_result.success)
		{
			state->Fail(fmt::format("Sign in failed: {}", kamaji_result.error));
			return;
		}

		const int resolution_height = settings->ResolutionPresetToInt(settings->GetPs34CatalogResolution());
		const int configured_bitrate_kbps = settings->GetPs34CatalogBitrateKbps();
		const int bitrate_kbps = configured_bitrate_kbps > 0 ? configured_bitrate_kbps : 10000;
		CloudGaikai gaikai(log, duid, "psnow", kamaji_result.platform, npsso,
			kamaji_result.entitlement_id, resolution_height, bitrate_kbps);
		CloudGaikai::Result gaikai_result = gaikai.Run([state](const std::string &message) {
			state->SetStage(message);
		});

		if(!gaikai_result.success)
		{
			state->Fail(fmt::format("Streaming setup failed: {}", gaikai_result.error));
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

		state->SetStage("Connecting...");
		StartCloudStream(host, state);
	}

	// PSCLOUD (PS5) skips CloudKamaji entirely - game.entitlement_id already
	// came straight from FetchOwnedPs5CloudGames's ownership cross-reference,
	// which is the PS5 equivalent of what Kamaji's product-id conversion does
	// for PSNOW (see cloudcatalog.h).
	void StartPscloudGame(Settings *settings, ChiakiLog *log, const CloudGame &game, std::shared_ptr<StreamStartState> state)
	{
		if(game.entitlement_id.empty())
		{
			state->Fail("This title is missing an entitlement id - try refreshing the PS5 library");
			return;
		}

		std::string npsso = settings->GetNPSSO();
		std::string duid = settings->GetOrCreateDUID();

		state->SetStage("Starting PS5 cloud stream...");

		const int resolution_height = settings->ResolutionPresetToInt(settings->GetPs5LibraryResolution());
		const int configured_bitrate_kbps = settings->GetPs5LibraryBitrateKbps();
		const int bitrate_kbps = configured_bitrate_kbps > 0 ? configured_bitrate_kbps : 15000;
		CloudGaikai gaikai(log, duid, "pscloud", "ps5", npsso, game.entitlement_id,
			resolution_height, bitrate_kbps);
		CloudGaikai::Result gaikai_result = gaikai.Run([state](const std::string &message) {
			state->SetStage(message);
		});

		if(!gaikai_result.success)
		{
			state->Fail(fmt::format("Streaming setup failed: {}", gaikai_result.error));
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

		state->SetStage("Connecting...");
		StartCloudStream(host, state);
	}

	// Opens the progress dialog on the main thread, then spawns the
	// background thread that does all the actual work (see StartPsnowGame /
	// StartPscloudGame above). item is this row's ListItem*, used to flip its
	// streamable glyph once the outcome is known.
	void LaunchStream(Settings *settings, ChiakiLog *log, CloudGame game, brls::ListItem *item, bool is_ps5)
	{
		auto state = std::make_shared<StreamStartState>();
		state->settings = settings;
		state->product_id = game.product_id;
		state->item = item;

		brls::Label *label = new brls::Label(brls::LabelStyle::REGULAR, "Starting stream...", true);
		StreamStartDialog *dialog = new StreamStartDialog(state, label);
		dialog->open();

		std::thread([settings, log, game, state, is_ps5]() {
			if(is_ps5)
				StartPscloudGame(settings, log, game, state);
			else
				StartPsnowGame(settings, log, game, state);
		}).detach();
	}
}

CloudGameList::CloudGameList(Settings *settings, ChiakiLog *log, std::string platform,
	brls::TabFrame *root_frame, bool force_refresh, std::string filter)
	: settings(settings), log(log), platform(platform), root_frame(root_frame), filter(filter)
{
	this->registerAction("Search", brls::Key::MINUS, [this]() {
		this->Search();
		return true;
	});

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
	this->addView(status);

	if(!ok)
	{
		CHIAKI_LOGE(log, "CloudGameList: failed to load %s catalog: %s", platform.c_str(), error.c_str());
		status->setValue("Failed to load");
		brls::Label *error_label = new brls::Label(brls::LabelStyle::REGULAR, error, true);
		this->addView(error_label);
		return;
	}

	if(!this->filter.empty())
	{
		brls::ListItem *clear_search = new brls::ListItem("Clear search (\"" + this->filter + "\")");
		clear_search->getClickEvent()->subscribe([this](brls::View *view) {
			if(!this->sidebar_item || !this->root_frame)
				return;
			CloudGameList *fresh = new CloudGameList(this->settings, this->log, this->platform, this->root_frame, false, "");
			fresh->SetSidebarItem(this->sidebar_item);
			this->sidebar_item->setAssociatedView(fresh);
			this->root_frame->switchToView(fresh);
		});
		this->addView(clear_search);
	}

	int shown_count = 0;
	bool is_ps5 = platform == "ps5";
	for(const auto &game : games)
	{
		if(!this->filter.empty() && !ContainsCaseInsensitive(game.name, this->filter))
			continue;

		brls::ListItem *item = new brls::ListItem(game.name.empty() ? game.product_id : game.name);
		item->setValue(GlyphForStreamable(settings->GetTitleStreamable(game.product_id)));
		item->getClickEvent()->subscribe([settings, log, game, item, is_ps5](brls::View *view) {
			LaunchStream(settings, log, game, item, is_ps5);
		});
		this->addView(item);
		shown_count++;
	}

	status->setValue(this->filter.empty()
		? fmt::format("{} games", games.size())
		: fmt::format("{} of {} games", shown_count, games.size()));
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
	CloudGameList *fresh = new CloudGameList(this->settings, this->log, this->platform, this->root_frame, true, this->filter);
	fresh->SetSidebarItem(this->sidebar_item);
	this->sidebar_item->setAssociatedView(fresh);
	this->root_frame->switchToView(fresh);
}

void CloudGameList::Search()
{
	if(!this->sidebar_item || !this->root_frame)
		return;

	auto submit_cb = [this](std::string text) {
		CloudGameList *fresh = new CloudGameList(this->settings, this->log, this->platform, this->root_frame, false, text);
		fresh->SetSidebarItem(this->sidebar_item);
		this->sidebar_item->setAssociatedView(fresh);
		this->root_frame->switchToView(fresh);
	};

	brls::Swkbd::openForText(submit_cb, "Search titles", "", 64, this->filter, 0);
}
