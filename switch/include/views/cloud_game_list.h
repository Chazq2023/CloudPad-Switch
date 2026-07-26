// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#ifndef CHIAKI_CLOUD_GAME_LIST_H
#define CHIAKI_CLOUD_GAME_LIST_H

#include <string>

#include <borealis.hpp>

#include "settings.h"

// One PS3/PS4/PS5 tab's contents: fetches that platform's cloud catalog once
// at construction time and lists the results. Game rows don't start a stream
// yet - that lands once the cloud session (Kamaji/Gaikai) port is in place.
class CloudGameList : public brls::List
{
	public:
		// platform must be "ps3", "ps4", or "ps5"
		CloudGameList(Settings *settings, ChiakiLog *log, std::string platform);
};

#endif // CHIAKI_CLOUD_GAME_LIST_H
