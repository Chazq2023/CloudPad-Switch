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
		// filter, when non-empty, only shows titles whose name contains it
		// (case-insensitive) - see Search().
		CloudGameList(Settings *settings, ChiakiLog *log, std::string platform,
			brls::TabFrame *root_frame, bool force_refresh = false, std::string filter = "");

		// Called once by whoever calls TabFrame::addTab, with the SidebarItem
		// that came back for this tab - needed so Refresh() can retarget it at
		// the replacement list it builds.
		void SetSidebarItem(brls::SidebarItem *item);

		// Application::handleAction gives Application::viewStack.back() (the
		// pushed TabFrame itself) first refusal on every key, before it ever
		// walks up from the focused view - so a Minus binding registered on
		// this List's own instance (a TabFrame child, never topView) can
		// never win against the TabFrame's own hidden FPS-toggle binding
		// (registered on itself by Application::pushView). Route Minus
		// through the TabFrame instead, only while this tab is the one
		// actually showing - see willAppear/willDisappear.
		void willAppear(bool resetState = false) override;
		void willDisappear(bool resetState = false) override;

	private:
		Settings *settings;
		ChiakiLog *log;
		std::string platform;
		brls::TabFrame *root_frame;
		brls::SidebarItem *sidebar_item = nullptr;
		std::string filter;

		void Refresh();
		// Opens the system keyboard (bound to the Minus button) and, on
		// submit, swaps in a freshly-filtered replacement list the same way
		// Refresh() does.
		void Search();
};

#endif // CHIAKI_CLOUD_GAME_LIST_H
