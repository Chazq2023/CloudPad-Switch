// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#ifndef CHIAKI_GUI_H
#define CHIAKI_GUI_H

#include <borealis.hpp>
#include <borealis/views/cells/cell_detail.hpp>

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
		brls::DetailCell *cloud_login_status = nullptr;
		brls::DetailCell *sign_in_item = nullptr;
		brls::DetailCell *sign_out_item = nullptr;

		void BuildAccountMenu(brls::Box *box);
		void SignIn();
		void SignOut();
		void RefreshLoginStatus();

	public:
		MainApplication();
		~MainApplication();
		bool Load();
};

#endif // CHIAKI_GUI_H
