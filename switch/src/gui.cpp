// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include <ctime>
#include <utility>

#include "gui.h"
#include "theme.h"
#include <chiaki/log.h>

// TODO
using namespace brls::literals; // for _i18n

MainApplication::MainApplication()
{
	this->settings = Settings::GetInstance();
	this->log = this->settings->GetLogger();
	this->io = IO::GetInstance();
}

MainApplication::~MainApplication()
{
	this->io->FreeController();
	this->io->FreeVideo();
}

bool MainApplication::Load()
{
	// Init the app
	brls::Logger::setLogLevel(brls::LogLevel::LOG_DEBUG);

	if(!brls::Application::init())
	{
		brls::Logger::error("Unable to init Borealis application");
		return false;
	}
	brls::Application::createWindow("CloudPad");
	ApplyCloudPadTheme();

	// init chiaki gl after borealis
	// let borealis manage the main screen/window
	// if(!io->InitVideo(0, 0, SCREEN_W, SCREEN_H))
	// {
	// 	brls::Logger::error("Failed to initiate Video");
	// }

	brls::Logger::info("Load SDL/HiD controller");
	if(!io->InitController())
	{
		brls::Logger::error("Failed to initiate Controller");
	}

	// Create a view. TabFrame::addTab (new Borealis) takes a lazy
	// std::function<View*(void)> rather than an eagerly-built View* - it
	// frees the current tab's view and calls the creator again on every
	// switch ("only one tab is kept in memory at all times"), so each
	// lambda below must build a fresh view rather than capture one built
	// up front, or the second visit to a tab would dereference an already
	// freed pointer.
	brls::TabFrame *rootFrame = new brls::TabFrame();

	MainApplication *self = this;
	rootFrame->addTab("Account", [self]() -> brls::View* {
		brls::Box *box = new brls::Box();
		box->setAxis(brls::Axis::COLUMN);
		self->BuildAccountMenu(box);
		brls::ScrollingFrame *scroll = new brls::ScrollingFrame();
		scroll->setContentView(box);
		return scroll;
	});

	rootFrame->addSeparator();
	brls::Logger::info("Building cloud catalog tabs");
	const std::pair<std::string, std::string> cloud_tabs[] = {
		{ "PS3", "ps3" }, { "PS4", "ps4" }, { "PS5", "ps5" }
	};
	Settings *settings = this->settings;
	ChiakiLog *log = this->log;
	for(const auto &tab : cloud_tabs)
	{
		std::string platform = tab.second;
		rootFrame->addTab(tab.first, [settings, log, platform]() -> brls::View* {
			return new CloudGameList(settings, log, platform);
		});
	}
	brls::Logger::info("Cloud catalog tabs built");

	brls::Application::pushActivity(new brls::Activity(rootFrame));
	brls::Logger::info("Root view pushed, entering main loop");

	while (brls::Application::mainLoop()) {
	}

	return true;
}

void MainApplication::BuildAccountMenu(brls::Box *box)
{
	brls::Label *info = new brls::Label();
	info->setText("PlayStation Account");
	box->addView(info);

	brls::Label *instructions = new brls::Label();
	instructions->setText(
		"On a phone or PC: sign in at store.playstation.com, then open "
		"https://ca.account.sony.com/api/v1/ssocookie in the same signed-in browser "
		"and copy the npsso value shown there. Paste it below to sign in.");
	// Label only wraps once its width is constrained - see label.hpp.
	instructions->setWidthPercentage(100);
	box->addView(instructions);

	this->cloud_login_status = new brls::DetailCell();
	this->cloud_login_status->setText("Status");
	box->addView(this->cloud_login_status);

	this->sign_in_item = new brls::DetailCell();
	this->sign_in_item->setText("Sign in with PlayStation");
	this->sign_in_item->registerClickAction([this](brls::View *view) {
		this->SignIn();
		return true;
	});
	box->addView(this->sign_in_item);

	this->sign_out_item = new brls::DetailCell();
	this->sign_out_item->setText("Sign out");
	this->sign_out_item->registerClickAction([this](brls::View *view) {
		this->SignOut();
		return true;
	});
	box->addView(this->sign_out_item);

	brls::Label *stream_info = new brls::Label();
	stream_info->setText("Stream settings");
	box->addView(stream_info);

	static const std::vector<std::string> kResolutionChoices = { "720p", "1080p" };

	int ps5_resolution_index = this->settings->GetPs5LibraryResolution() == CHIAKI_VIDEO_RESOLUTION_PRESET_720p ? 0 : 1;
	brls::SelectorCell *ps5_resolution = new brls::SelectorCell();
	ps5_resolution->init("PS5 Library Resolution", kResolutionChoices, ps5_resolution_index,
		[](int selected) {},
		[this](int result) {
			this->settings->SetPs5LibraryResolution(result == 1
				? CHIAKI_VIDEO_RESOLUTION_PRESET_1080p
				: CHIAKI_VIDEO_RESOLUTION_PRESET_720p);
			this->settings->WriteFile();
		});
	box->addView(ps5_resolution);

	int ps34_resolution_index = this->settings->GetPs34CatalogResolution() == CHIAKI_VIDEO_RESOLUTION_PRESET_720p ? 0 : 1;
	brls::SelectorCell *ps34_resolution = new brls::SelectorCell();
	ps34_resolution->init("PS3/PS4 Catalog Resolution", kResolutionChoices, ps34_resolution_index,
		[](int selected) {},
		[this](int result) {
			this->settings->SetPs34CatalogResolution(result == 1
				? CHIAKI_VIDEO_RESOLUTION_PRESET_1080p
				: CHIAKI_VIDEO_RESOLUTION_PRESET_720p);
			this->settings->WriteFile();
		});
	box->addView(ps34_resolution);

	// 0 = use the resolution preset's own default bitrate (10/15 Mbps for
	// 720p/1080p respectively, set by chiaki_connect_video_profile_preset).
	static const int kBitrateChoicesKbps[] = { 0, 5000, 8000, 10000, 15000, 20000, 25000 };
	static const std::vector<std::string> kBitrateChoiceLabels =
		{ "Auto (resolution default)", "5 Mbps", "8 Mbps", "10 Mbps", "15 Mbps", "20 Mbps", "25 Mbps" };

	auto bitrate_index_for = [](int bitrate_kbps) {
		int index = 0;
		for(size_t i = 0; i < sizeof(kBitrateChoicesKbps) / sizeof(int); i++)
			if(kBitrateChoicesKbps[i] == bitrate_kbps)
				index = (int)i;
		return index;
	};

	brls::SelectorCell *ps5_bitrate = new brls::SelectorCell();
	ps5_bitrate->init("PS5 Library Bitrate", kBitrateChoiceLabels,
		bitrate_index_for(this->settings->GetPs5LibraryBitrateKbps()),
		[](int selected) {},
		[this](int result) {
			this->settings->SetPs5LibraryBitrateKbps(kBitrateChoicesKbps[result]);
			this->settings->WriteFile();
		});
	box->addView(ps5_bitrate);

	brls::SelectorCell *ps34_bitrate = new brls::SelectorCell();
	ps34_bitrate->init("PS3/PS4 Catalog Bitrate", kBitrateChoiceLabels,
		bitrate_index_for(this->settings->GetPs34CatalogBitrateKbps()),
		[](int selected) {},
		[this](int result) {
			this->settings->SetPs34CatalogBitrateKbps(kBitrateChoicesKbps[result]);
			this->settings->WriteFile();
		});
	box->addView(ps34_bitrate);

	brls::SelectorCell *sharpen = new brls::SelectorCell();
	sharpen->init("Image sharpening", { "Off", "Low", "Medium", "High" }, this->settings->GetSharpenLevel(),
		[](int selected) {},
		[this](int result) {
			this->settings->SetSharpenLevel(result);
			this->settings->WriteFile();
		});
	box->addView(sharpen);

	brls::SelectorCell *pacing = new brls::SelectorCell();
	pacing->init("Video pacing", { "Standard (lowest latency)", "Smooth (steadier motion)" },
		this->settings->GetVideoPacingSmooth() ? 1 : 0,
		[](int selected) {},
		[this](int result) {
			this->settings->SetVideoPacingSmooth(result == 1);
			this->settings->WriteFile();
		});
	box->addView(pacing);

	this->RefreshLoginStatus();
}

void MainApplication::RefreshLoginStatus()
{
	if(this->cloud_login_status == nullptr)
		return;

	bool logged_in = this->settings->IsCloudLoggedIn();
	this->cloud_login_status->setDetailText(logged_in ? "Signed in" : "Not signed in");

	if(this->sign_in_item != nullptr && this->sign_out_item != nullptr)
	{
		if(logged_in)
		{
			this->sign_in_item->hide([]() {});
			this->sign_out_item->show([]() {});
		}
		else
		{
			this->sign_in_item->show([]() {});
			this->sign_out_item->hide([]() {});
		}
	}
}

void MainApplication::SignIn()
{
	auto npsso_cb = [this](std::string pasted) {
		std::string npsso = CloudAuth::ExtractNPSSO(pasted);
		if(npsso.empty())
		{
			brls::Application::notify("No npsso value found in what was pasted");
			return;
		}

		// blocking network call, same pattern the other cloud HTTP helpers use
		brls::Application::blockInputs();
		CloudAuth auth(this->log);
		CloudAuth::Tokens tokens;
		std::string error;
		std::string duid = this->settings->GetOrCreateDUID();
		bool ok = auth.ExchangeNPSSO(npsso, duid, &tokens, &error);
		brls::Application::unblockInputs();

		if(!ok)
		{
			CHIAKI_LOGE(this->log, "PSN sign in failed: %s", error.c_str());
			brls::Application::notify(fmt::format("Sign in failed: {}", error));
			return;
		}

		this->settings->SetNPSSO(npsso);
		this->settings->SetPSNAccessToken(tokens.access_token);
		this->settings->SetPSNIdToken(tokens.id_token);
		this->settings->SetPSNTokenExpiry((int64_t)std::time(nullptr) + tokens.expires_in);
		this->settings->WriteFile();

		this->RefreshLoginStatus();
		brls::Application::notify("Signed in with PlayStation");
	};

	brls::Application::getImeManager()->openForText(npsso_cb,
		"Paste your npsso value",
		"From the ssocookie page on your phone/PC browser", 512, "", 0);
}

void MainApplication::SignOut()
{
	this->settings->ClearCloudLogin();
	this->settings->WriteFile();
	this->RefreshLoginStatus();
	brls::Application::notify("Signed out");
}
