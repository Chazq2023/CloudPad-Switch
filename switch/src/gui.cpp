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

	brls::ListItem *sign_in = new brls::ListItem("Sign in with PlayStation");
	sign_in->getClickEvent()->subscribe([this](brls::View *view) {
		this->SignIn();
	});
	ls->addView(sign_in);

	brls::ListItem *sign_out = new brls::ListItem("Sign out");
	sign_out->getClickEvent()->subscribe([this](brls::View *view) {
		this->SignOut();
	});
	ls->addView(sign_out);

	brls::Label *stream_info = new brls::Label(brls::LabelStyle::REGULAR,
		"Stream settings", true);
	ls->addView(stream_info);

	brls::ListItem *ps5_resolution = new brls::ListItem("PS5 Library resolution");
	ps5_resolution->setValue("1080p");
	ls->addView(ps5_resolution);

	brls::ListItem *ps4_resolution = new brls::ListItem("PS4 Catalog resolution");
	ps4_resolution->setValue("720p");
	ls->addView(ps4_resolution);

	brls::ListItem *ps3_resolution = new brls::ListItem("PS3 Catalog resolution");
	ps3_resolution->setValue("720p");
	ls->addView(ps3_resolution);

	int value = this->settings->GetHaptic(nullptr);
	brls::SelectListItem *haptic = new brls::SelectListItem(
		"Haptics", { "Disabled", "Weak", "Strong" }, value);

	auto haptic_cb = [this](int result) {
		HapticPreset value = HAPTIC_PRESET_DIABLED;
		switch(result)
		{
			case 0:
				value = HAPTIC_PRESET_DIABLED;
				break;
			case 1:
				value = HAPTIC_PRESET_WEAK;
				break;
			case 2:
				value = HAPTIC_PRESET_STRONG;
				break;
		}
		this->settings->SetHaptic(nullptr, value);
		this->settings->WriteFile();
	};
	haptic->getValueSelectedEvent()->subscribe(haptic_cb);
	ls->addView(haptic);

	// 0 = use the resolution preset's own default bitrate (10/15 Mbps for
	// 720p/1080p respectively, set by chiaki_connect_video_profile_preset).
	static const int kBitrateChoicesKbps[] = { 0, 5000, 8000, 10000, 15000, 20000, 25000 };
	int bitrate_kbps = this->settings->GetCustomBitrateKbps();
	int bitrate_index = 0;
	for(size_t i = 0; i < sizeof(kBitrateChoicesKbps) / sizeof(int); i++)
		if(kBitrateChoicesKbps[i] == bitrate_kbps)
			bitrate_index = (int)i;

	brls::SelectListItem *bitrate = new brls::SelectListItem("Bitrate",
		{ "Auto (resolution default)", "5 Mbps", "8 Mbps", "10 Mbps", "15 Mbps", "20 Mbps", "25 Mbps" },
		bitrate_index);
	auto bitrate_cb = [this](int result) {
		this->settings->SetCustomBitrateKbps(kBitrateChoicesKbps[result]);
		this->settings->WriteFile();
	};
	bitrate->getValueSelectedEvent()->subscribe(bitrate_cb);
	ls->addView(bitrate);

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

	if(this->settings->IsCloudLoggedIn())
		this->cloud_login_status->setValue("Signed in");
	else
		this->cloud_login_status->setValue("Not signed in");
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
