// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include <ctime>
#include <utility>

#include "gui.h"
#include <chiaki/log.h>

#define SCREEN_W 1280
#define SCREEN_H 720

// TODO
using namespace brls::i18n::literals; // for _i18n

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
	brls::Logger::setLogLevel(brls::LogLevel::DEBUG);

	brls::i18n::loadTranslations();
	if(!brls::Application::init("CloudPad"))
	{
		brls::Logger::error("Unable to init Borealis application");
		return false;
	}

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

	// Create a view
	this->rootFrame = new brls::TabFrame();
	this->rootFrame->setTitle("CloudPad");
	this->rootFrame->setIcon(BOREALIS_ASSET("cloudpad-icon.png"));

	brls::List *account = new brls::List();
	BuildAccountMenu(account);
	this->rootFrame->addTab("Account", account);

	this->rootFrame->addSeparator();
	brls::Logger::info("Building cloud catalog tabs");
	const std::pair<std::string, std::string> cloud_tabs[] = {
		{ "PS3", "ps3" }, { "PS4", "ps4" }, { "PS5", "ps5" }
	};
	for(const auto &tab : cloud_tabs)
	{
		CloudGameList *list = new CloudGameList(this->settings, this->log, tab.second, this->rootFrame);
		brls::SidebarItem *item = this->rootFrame->addTab(tab.first, list);
		list->SetSidebarItem(item);
	}
	brls::Logger::info("Cloud catalog tabs built");

	brls::Application::pushView(this->rootFrame);
	brls::Logger::info("Root view pushed, entering main loop");

	while (brls::Application::mainLoop()) {
	}
	
	return true;
}

void MainApplication::BuildAccountMenu(brls::List *ls)
{
	brls::Label *info = new brls::Label(brls::LabelStyle::REGULAR,
		"PlayStation Account", true);
	ls->addView(info);

	brls::Label *instructions = new brls::Label(brls::LabelStyle::REGULAR,
		"On a phone or PC: sign in at store.playstation.com, then open "
		"https://ca.account.sony.com/api/v1/ssocookie in the same signed-in browser "
		"and copy the npsso value shown there. Paste it below to sign in.", true);
	ls->addView(instructions);

	this->cloud_login_status = new brls::ListItem("Status");
	ls->addView(this->cloud_login_status);

	this->sign_in_item = new brls::ListItem("Sign in with PlayStation");
	this->sign_in_item->getClickEvent()->subscribe([this](brls::View *view) {
		this->SignIn();
	});
	ls->addView(this->sign_in_item);

	this->sign_out_item = new brls::ListItem("Sign out");
	this->sign_out_item->getClickEvent()->subscribe([this](brls::View *view) {
		this->SignOut();
	});
	ls->addView(this->sign_out_item);

	brls::Label *stream_info = new brls::Label(brls::LabelStyle::REGULAR,
		"Stream settings", true);
	ls->addView(stream_info);

	static const std::vector<std::string> kResolutionChoices = { "720p", "1080p" };

	int ps5_resolution_index = this->settings->GetPs5LibraryResolution() == CHIAKI_VIDEO_RESOLUTION_PRESET_720p ? 0 : 1;
	brls::SelectListItem *ps5_resolution = new brls::SelectListItem(
		"PS5 Library Resolution", kResolutionChoices, ps5_resolution_index);
	ps5_resolution->getValueSelectedEvent()->subscribe([this](int result) {
		this->settings->SetPs5LibraryResolution(result == 1
			? CHIAKI_VIDEO_RESOLUTION_PRESET_1080p
			: CHIAKI_VIDEO_RESOLUTION_PRESET_720p);
		this->settings->WriteFile();
	});
	ls->addView(ps5_resolution);

	int ps34_resolution_index = this->settings->GetPs34CatalogResolution() == CHIAKI_VIDEO_RESOLUTION_PRESET_720p ? 0 : 1;
	brls::SelectListItem *ps34_resolution = new brls::SelectListItem(
		"PS3/PS4 Catalog Resolution", kResolutionChoices, ps34_resolution_index);
	ps34_resolution->getValueSelectedEvent()->subscribe([this](int result) {
		this->settings->SetPs34CatalogResolution(result == 1
			? CHIAKI_VIDEO_RESOLUTION_PRESET_1080p
			: CHIAKI_VIDEO_RESOLUTION_PRESET_720p);
		this->settings->WriteFile();
	});
	ls->addView(ps34_resolution);

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

	brls::SelectListItem *ps5_bitrate = new brls::SelectListItem("PS5 Library Bitrate",
		kBitrateChoiceLabels, bitrate_index_for(this->settings->GetPs5LibraryBitrateKbps()));
	ps5_bitrate->getValueSelectedEvent()->subscribe([this](int result) {
		this->settings->SetPs5LibraryBitrateKbps(kBitrateChoicesKbps[result]);
		this->settings->WriteFile();
	});
	ls->addView(ps5_bitrate);

	brls::SelectListItem *ps34_bitrate = new brls::SelectListItem("PS3/PS4 Catalog Bitrate",
		kBitrateChoiceLabels, bitrate_index_for(this->settings->GetPs34CatalogBitrateKbps()));
	ps34_bitrate->getValueSelectedEvent()->subscribe([this](int result) {
		this->settings->SetPs34CatalogBitrateKbps(kBitrateChoicesKbps[result]);
		this->settings->WriteFile();
	});
	ls->addView(ps34_bitrate);

	brls::SelectListItem *sharpen = new brls::SelectListItem("Image sharpening",
		{ "Off", "Low", "Medium", "High" }, this->settings->GetSharpenLevel());
	auto sharpen_cb = [this](int result) {
		this->settings->SetSharpenLevel(result);
		this->settings->WriteFile();
	};
	sharpen->getValueSelectedEvent()->subscribe(sharpen_cb);
	ls->addView(sharpen);

	brls::SelectListItem *pacing = new brls::SelectListItem("Video pacing",
		{ "Standard (lowest latency)", "Smooth (steadier motion)" },
		this->settings->GetVideoPacingSmooth() ? 1 : 0);
	auto pacing_cb = [this](int result) {
		this->settings->SetVideoPacingSmooth(result == 1);
		this->settings->WriteFile();
	};
	pacing->getValueSelectedEvent()->subscribe(pacing_cb);
	ls->addView(pacing);

	this->RefreshLoginStatus();
}

void MainApplication::RefreshLoginStatus()
{
	if(this->cloud_login_status == nullptr)
		return;

	bool logged_in = this->settings->IsCloudLoggedIn();
	this->cloud_login_status->setValue(logged_in ? "Signed in" : "Not signed in");

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

	brls::Swkbd::openForText(npsso_cb,
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
