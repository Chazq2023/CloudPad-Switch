// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#ifndef CHIAKI_GUI_H
#define CHIAKI_GUI_H

#include <glad.h>
#include <GLFW/glfw3.h>
#include "nanovg.h"
#include "nanovg_gl.h"
#include "nanovg_gl_utils.h"

#include <map>
#include <thread>
#include <fmt/format.h>

#include <borealis.hpp>
#include "cloudauth.h"
#include "host.h"
#include "io.h"
#include "settings.h"
#include "switch.h"
#include "views/cloud_game_list.h"

class MainApplication
{
	private:
		Settings *settings;
		ChiakiLog *log;
		IO *io;
		brls::TabFrame *rootFrame;
		brls::ListItem *cloud_login_status = nullptr;

		void BuildAccountMenu(brls::List *);
		void SignIn();
		void SignOut();
		void RefreshLoginStatus();

	public:
		MainApplication();
		~MainApplication();
		bool Load();
};

#endif // CHIAKI_GUI_H
