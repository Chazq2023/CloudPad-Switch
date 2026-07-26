// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#ifndef CHIAKI_CLOUD_GAME_LIST_H
#define CHIAKI_CLOUD_GAME_LIST_H

#include <string>

#include <borealis.hpp>

#include "settings.h"

// One PS3/PS4/PS5 tab's contents: fetches that platform's cloud catalog at
// construction time and lists the results. Selecting a PSNOW (PS3/PS4) row
// starts a real cloud stream; PS5 rows are catalog-browse only for now (see
// cloudcatalog.h for why).
class CloudGameList : public brls::List
{
	public:
		// platform must be "ps3", "ps4", or "ps5". root_frame is needed so the
		// "Refresh catalog" row can swap in a freshly-fetched replacement for
		// this tab (via SidebarItem::setAssociatedView + TabFrame::switchToView -
		// the same path normal tab switching already uses - rather than
		// mutating this list's own rows in place, which Borealis's
		// BoxLayout::removeView warns isn't safe at runtime).
		// force_refresh bypasses the on-disk catalog cache for this fetch.
		CloudGameList(Settings *settings, ChiakiLog *log, std::string platform,
			brls::TabFrame *root_frame, bool force_refresh = false);

		// Called once by whoever calls TabFrame::addTab, with the SidebarItem
		// that came back for this tab - needed so Refresh() can retarget it at
		// the replacement list it builds.
		void SetSidebarItem(brls::SidebarItem *item);

	private:
		Settings *settings;
		ChiakiLog *log;
		std::string platform;
		brls::TabFrame *root_frame;
		brls::SidebarItem *sidebar_item = nullptr;

		void Refresh();
};

#endif // CHIAKI_CLOUD_GAME_LIST_H
