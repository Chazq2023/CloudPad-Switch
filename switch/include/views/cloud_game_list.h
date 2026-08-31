// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#ifndef CHIAKI_CLOUD_GAME_LIST_H
#define CHIAKI_CLOUD_GAME_LIST_H

#include <string>

#include <borealis.hpp>
#include <borealis/views/cells/cell_detail.hpp>

#include "settings.h"

// One PS3/PS4/PS5 tab's contents: fetches that platform's cloud catalog at
// construction time and lists the results. Selecting a PSNOW (PS3/PS4) row
// starts a real cloud stream; PS5 rows are catalog-browse only for now (see
// cloudcatalog.h for why).
//
// A ScrollingFrame so the row list can scroll (TabFrame gives each tab the
// full content pane, no scrolling of its own). Refresh()/Search() rebuild
// rowsBox in place rather than swapping in a whole new CloudGameList - the
// old fork's BoxLayout::removeView was documented as unsafe to use at
// runtime, which forced a SidebarItem-swap workaround; this fork's Box
// (see settings_general_view.cpp's rebuildSubnetsUI in the reference
// Borealis fork this was ported from) supports clearViews()+rebuild safely,
// so that workaround is gone along with the root_frame/SidebarItem plumbing
// it needed.
class CloudGameList : public brls::ScrollingFrame
{
	public:
		// platform must be "ps3", "ps4", or "ps5".
		CloudGameList(Settings *settings, ChiakiLog *log, std::string platform);

	private:
		Settings *settings;
		ChiakiLog *log;
		std::string platform;
		std::string filter;
		brls::Box *rowsBox;
		brls::DetailCell *status;

		// force_refresh bypasses the on-disk catalog cache for this fetch.
		void BuildRows(bool force_refresh);
		void Refresh();
		// Opens the system keyboard (bound to the Minus button) and, on
		// submit, rebuilds the row list filtered to titles whose name
		// contains what was typed (case-insensitive).
		void Search();
};

#endif // CHIAKI_CLOUD_GAME_LIST_H
