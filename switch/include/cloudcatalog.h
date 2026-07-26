// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#ifndef CHIAKI_CLOUDCATALOG_H
#define CHIAKI_CLOUDCATALOG_H

#include <string>
#include <vector>

#include <chiaki/log.h>

// A single game entry from either the PSNOW (PS3/PS4) or PS5 cloud streaming
// catalog, trimmed down to what the Switch UI needs to show a list and start
// a stream (full entitlement/ownership details are handled separately at
// stream-start time, not here).
struct CloudGame
{
	std::string product_id;
	std::string name;
	std::string image_url;
	std::string platform;     // "ps3", "ps4", or "ps5"
	std::string service_type; // "psnow" or "pscloud"
};

// Ports the catalog-fetch half of the upstream CloudCatalogBackend
// (gui/src/cloudcatalogbackend.cpp) needed to populate the PS3/PS4/PS5 tabs.
// Ownership cross-referencing, per-game detail/image fetch, and Steam
// shortcut creation from that file are intentionally not ported here - they
// aren't needed to browse and launch a cloud game.
class CloudCatalog
{
	public:
		explicit CloudCatalog(ChiakiLog *log);

		// Fetches (or returns a cached copy of) the PSNOW catalog, which covers
		// both PS3 and PS4 titles - Sony serves them from one dataset, split
		// client-side by product id SKU prefix (see PlatformForPsnowProductId).
		// Requires a signed-in npsso/duid. Blocking network call.
		bool FetchPsnowCatalog(const std::string &npsso, const std::string &duid,
			std::vector<CloudGame> *out_games, std::string *out_error);

		// Fetches (or returns a cached copy of) the public PS5 cloud streaming
		// catalog. No login required - this endpoint is unauthenticated.
		// Blocking network call.
		bool FetchPs5CloudCatalog(const std::string &locale,
			std::vector<CloudGame> *out_games, std::string *out_error);

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
};

// PSNOW product ids for PS4 titles use "-CUSA" or "-PPSA" SKU prefixes;
// anything else in the same PSNOW dataset is a PS3 title. Matches the
// heuristic upstream's CloudPlayFragment.kt uses to route PSNOW entries to
// the PS3 vs PS4 tab.
std::string PlatformForPsnowProductId(const std::string &product_id);

#endif // CHIAKI_CLOUDCATALOG_H
