// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#ifndef CHIAKI_CLOUDCATALOG_H
#define CHIAKI_CLOUDCATALOG_H

#include <string>
#include <vector>

#include <chiaki/log.h>

// A single game entry from either the PSNOW (PS3/PS4) catalog or the PS5
// cloud streaming library, trimmed down to what the Switch UI needs to show
// a list and start a stream.
struct CloudGame
{
	std::string product_id;
	// PSCLOUD (PS5) only: the real per-account entitlement id CloudGaikai
	// needs to allocate a streaming slot (see cloudgaikai.h). Populated by
	// FetchOwnedPs5CloudGames's cross-reference; empty for PSNOW games, which
	// get their entitlement id from CloudKamaji's product-id conversion
	// instead (Sony's PS Plus catalog is subscription-wide, not per-title
	// owned, so there's nothing to cross-reference there).
	std::string entitlement_id;
	std::string name;
	std::string image_url;
	std::string platform;     // "ps3", "ps4", or "ps5"
	std::string service_type; // "psnow" or "pscloud"
};

// Ports the catalog-fetch half of the upstream CloudCatalogBackend
// (gui/src/cloudcatalogbackend.cpp) needed to populate the PS3/PS4/PS5 tabs.
// Per-game detail/image fetch and Steam shortcut creation from that file are
// intentionally not ported here - they aren't needed to browse and launch a
// cloud game.
class CloudCatalog
{
	public:
		explicit CloudCatalog(ChiakiLog *log);

		// Fetches (or returns a cached copy of) the PSNOW catalog, which covers
		// both PS3 and PS4 titles - Sony serves them from one dataset, split
		// client-side by product id SKU prefix (see PlatformForPsnowProductId).
		// This is PS Plus's subscription-wide catalog (any title in it is
		// playable), unlike PS5's owned-only library below.
		// Requires a signed-in npsso/duid. Blocking network call.
		// force_refresh skips the cache and re-fetches from Sony - use after
		// sign-in, or from an explicit "Refresh" action.
		bool FetchPsnowCatalog(const std::string &npsso, const std::string &duid,
			std::vector<CloudGame> *out_games, std::string *out_error, bool force_refresh = false);

		// Fetches (or returns a cached copy of) the public PS5 cloud streaming
		// catalog - every PS5 title Sony's cloud infrastructure can stream,
		// regardless of who owns it. No login required. Only useful here as
		// input to FetchOwnedPs5CloudGames's cross-reference (matches
		// upstream's own use of it - see getOwnedPs5CloudGames), since a
		// catalog entry's id isn't an entitlement id (see CloudGame above).
		// Blocking network call. force_refresh: see FetchPsnowCatalog.
		bool FetchPs5CloudCatalog(const std::string &locale,
			std::vector<CloudGame> *out_games, std::string *out_error, bool force_refresh = false);

		// Fetches the PS5 titles this signed-in account actually owns and
		// cross-references them against FetchPs5CloudCatalog, matching upstream
		// CloudCatalogBackend::getOwnedPs5CloudGames - this is what CloudPad's
		// Android "PS5 Library" tab shows, and it's the only place a real
		// entitlement_id (CloudGame::entitlement_id) comes from, so it's the
		// PS5 equivalent of FetchPsnowCatalog for actually starting a stream.
		// Requires a signed-in npsso. Blocking network call (OAuth + paginated
		// entitlements fetch + catalog fetch/cross-reference).
		bool FetchOwnedPs5CloudGames(const std::string &npsso, const std::string &locale,
			std::vector<CloudGame> *out_games, std::string *out_error, bool force_refresh = false);

	private:
		ChiakiLog *log;

		bool FetchPsnowOAuthCode(const std::string &npsso, const std::string &duid,
			std::string *out_code, std::string *out_error);
		bool FetchPsnowSession(const std::string &code, const std::string &duid,
			std::string *out_jsessionid, std::string *out_error);
		bool FetchPsnowBaseUrl(const std::string &jsessionid,
			std::string *out_base_url, std::string *out_error);
		bool FetchPsnowCategoryUrls(const std::string &jsessionid, const std::string &base_url,
			std::vector<std::string> *out_category_urls, std::string *out_error);
		bool FetchPsnowCategoryGames(const std::string &jsessionid, const std::string &category_url,
			std::vector<CloudGame> *out_games, std::string *out_error);

		bool FetchOwnedPs5EntitlementToken(const std::string &npsso,
			std::string *out_token, std::string *out_error);
		bool FetchOwnedPs5EntitlementsPage(const std::string &token, int start,
			std::vector<CloudGame> *out_page, bool *out_has_more, std::string *out_error);
};

// PSNOW product ids for PS4 titles use "-CUSA" or "-PPSA" SKU prefixes;
// anything else in the same PSNOW dataset is a PS3 title. Matches the
// heuristic upstream's CloudPlayFragment.kt uses to route PSNOW entries to
// the PS3 vs PS4 tab.
std::string PlatformForPsnowProductId(const std::string &product_id);

#endif // CHIAKI_CLOUDCATALOG_H
