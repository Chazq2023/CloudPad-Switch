// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include <ctime>

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
	if(!brls::Application::init("pylux"))
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
	this->rootFrame->setTitle("pylux");
	this->rootFrame->setIcon(BOREALIS_ASSET("icon.png"));

	brls::List *account = new brls::List();
	BuildAccountMenu(account);
	this->rootFrame->addTab("Account", account);

	this->rootFrame->addSeparator();
	brls::Logger::info("Building cloud catalog tabs");
	this->rootFrame->addTab("PS3", new CloudGameList(this->settings, this->log, "ps3"));
	this->rootFrame->addTab("PS4", new CloudGameList(this->settings, this->log, "ps4"));
	this->rootFrame->addTab("PS5", new CloudGameList(this->settings, this->log, "ps5"));
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

	int value;
	ChiakiVideoResolutionPreset resolution_preset = this->settings->GetVideoResolution(nullptr);
	switch(resolution_preset)
	{
		case CHIAKI_VIDEO_RESOLUTION_PRESET_1080p:
			value = 0;
			break;
		case CHIAKI_VIDEO_RESOLUTION_PRESET_720p:
			value = 1;
			break;
		case CHIAKI_VIDEO_RESOLUTION_PRESET_540p:
			value = 2;
			break;
		case CHIAKI_VIDEO_RESOLUTION_PRESET_360p:
			value = 3;
			break;
	}

	brls::SelectListItem *resolution = new brls::SelectListItem(
		"Resolution", { "1080p (PS5 and PS4 Pro only)", "720p", "540p", "360p" }, value);

	auto resolution_cb = [this](int result) {
		ChiakiVideoResolutionPreset value = CHIAKI_VIDEO_RESOLUTION_PRESET_720p;
		switch(result)
		{
			case 0:
				value = CHIAKI_VIDEO_RESOLUTION_PRESET_1080p;
				break;
			case 1:
				value = CHIAKI_VIDEO_RESOLUTION_PRESET_720p;
				break;
			case 2:
				value = CHIAKI_VIDEO_RESOLUTION_PRESET_540p;
				break;
			case 3:
				value = CHIAKI_VIDEO_RESOLUTION_PRESET_360p;
				break;
		}
		this->settings->SetVideoResolution(nullptr, value);
		this->settings->WriteFile();
	};
	resolution->getValueSelectedEvent()->subscribe(resolution_cb);
	ls->addView(resolution);

	ChiakiVideoFPSPreset fps_preset = this->settings->GetVideoFPS(nullptr);
	switch(fps_preset)
	{
		case CHIAKI_VIDEO_FPS_PRESET_60:
			value = 0;
			break;
		case CHIAKI_VIDEO_FPS_PRESET_30:
			value = 1;
			break;
	}

	brls::SelectListItem *fps = new brls::SelectListItem(
		"FPS", { "60fps", "30fps" }, value);

	auto fps_cb = [this](int result) {
		ChiakiVideoFPSPreset value = CHIAKI_VIDEO_FPS_PRESET_60;
		switch(result)
		{
			case 0:
				value = CHIAKI_VIDEO_FPS_PRESET_60;
				break;
			case 1:
				value = CHIAKI_VIDEO_FPS_PRESET_30;
				break;
		}
		this->settings->SetVideoFPS(nullptr, value);
		this->settings->WriteFile();
	};
	fps->getValueSelectedEvent()->subscribe(fps_cb);
	ls->addView(fps);

	value = this->settings->GetHaptic(nullptr);
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

