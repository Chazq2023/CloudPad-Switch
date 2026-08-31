// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include "cloudcatalog.h"
#include "cloudhttp.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <map>
#include <regex>
#include <sstream>
#include <thread>
#include <utility>

#include <json-c/json.h>

using cloudhttp::HttpRequest;
using cloudhttp::HttpResponse;
using cloudhttp::JsonGetString;
using cloudhttp::UrlEncode;
using cloudhttp::ExtractQueryParam;

namespace
{
	// Matches KamajiConsts in the upstream gui/include/cloudstreaming/pskamajisession.h
	const char *kKamajiBase = "https://psnow.playstation.com/kamaji/api/pcnow/00_09_000";
	const char *kKamajiClientId = "bc6b0777-abb5-40da-92ca-e133cf18e989";
	const char *kPs4Scopes = "kamaji:commerce_native kamaji:commerce_container kamaji:lists kamaji:s2s.subscriptionsPremium.get";
	const char *kOrigin = "https://psnow.playstation.com";
	const char *kReferer = "https://psnow.playstation.com/app/2.2.0/133/5cdcc037d/";
	const char *kRedirectUri = "https://psnow.playstation.com/app/2.2.0/133/5cdcc037d/grc-response.html";
	const char *kKamajiUserAgent =
		"Mozilla/5.0 (Windows NT 10.0; WOW64) AppleWebKit/537.36 (KHTML, like Gecko) "
		"playstation-now/0.0.0 Chrome/83.0.4103.104 Electron/9.0.4 Safari/537.36 gkApollo";
	const char *kAccountBase = "https://ca.account.sony.com/api";
	const char *kBrowserUserAgent = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36";

	const int kCacheDurationSeconds = 24 * 60 * 60; // 24h, matches CACHE_DURATION_CATALOG upstream

	// Alphabetical category link names PSNOW's root container exposes -
	// matches categoryPatterns in the upstream fetchPsnowRootContainer.
	const char *kPsnowCategoryNames[] = { "A - B", "C - D", "E - G", "H - L", "M - O", "P - R", "S", "T", "U - Z" };

	std::string ExtractJSessionId(const std::string &headers)
	{
		return cloudhttp::ExtractCookieValue(headers, "JSESSIONID");
	}

	std::string ExtractCoverImageUrl(json_object *game_obj)
	{
		json_object *images = nullptr;
		if(json_object_object_get_ex(game_obj, "images", &images) && json_object_is_type(images, json_type_array))
		{
			int landscape_idx = -1;
			int len = json_object_array_length(images);
			for(int i = 0; i < len; i++)
			{
				json_object *img = json_object_array_get_idx(images, i);
				json_object *type_obj = nullptr;
				json_object *url_obj = nullptr;
				if(!json_object_object_get_ex(img, "type", &type_obj) || !json_object_object_get_ex(img, "url", &url_obj))
					continue;
				int type = json_object_get_int(type_obj);
				if(type == 10) // cover/box art, preferred
					return json_object_get_string(url_obj);
				if((type == 12 || type == 13) && landscape_idx < 0)
					landscape_idx = i;
			}
			if(landscape_idx >= 0)
			{
				json_object *img = json_object_array_get_idx(images, landscape_idx);
				json_object *url_obj = nullptr;
				if(json_object_object_get_ex(img, "url", &url_obj))
					return json_object_get_string(url_obj);
			}
		}

		json_object *image_url_obj = nullptr;
		if(json_object_object_get_ex(game_obj, "imageUrl", &image_url_obj))
			return json_object_get_string(image_url_obj);

		return "";
	}

	// --- PS5 owned-games entitlement parsing/filtering -----------------------
	// Ports upstream Android's PsCloudOwnership.kt (filterOwnedPs5Games +
	// buildOwnedGamesFromEntitlements) as closely as C++/json-c allow, since
	// that's the algorithm CloudPad's own PS5 Library tab uses - the previous
	// port here instead cross-referenced owned entitlements against
	// FetchPs5CloudCatalog and required package_type=="PSGD", neither of which
	// upstream's real Library-building code does, and which silently dropped
	// every owned title whose SKU didn't happen to also appear in the public
	// browse catalog.

	struct Ps5Entitlement
	{
		std::string id;
		std::string product_id;
		bool active_flag = false;
		std::string package_type;
		std::string name;
		int feature_type = 0; // PSN feature_type: 3=full game, 1=trial/free, 0=add-on/DLC
		std::string sku_type; // "GAME_TRIAL" for limited-play game trials
		std::string icon_url; // game_meta.icon_url - box art straight from the entitlement itself
	};

	std::string ToLowerAscii(const std::string &s)
	{
		std::string out = s;
		for(char &c : out)
			if(c >= 'A' && c <= 'Z')
				c = c - 'A' + 'a';
		return out;
	}

	bool ContainsCI(const std::string &haystack, const char *needle_lower)
	{
		return ToLowerAscii(haystack).find(needle_lower) != std::string::npos;
	}

	bool IsAsciiAlnum(char c)
	{
		return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
	}

	// Case-insensitive whole-word search - matches upstream's \bword\b regex
	// (so e.g. "Demolition" doesn't false-positive on "demo").
	bool ContainsWordCI(const std::string &haystack, const char *word_lower)
	{
		std::string lower = ToLowerAscii(haystack);
		size_t word_len = strlen(word_lower);
		size_t pos = 0;
		while((pos = lower.find(word_lower, pos)) != std::string::npos)
		{
			bool left_ok = (pos == 0) || !IsAsciiAlnum(lower[pos - 1]);
			size_t end = pos + word_len;
			bool right_ok = (end == lower.size()) || !IsAsciiAlnum(lower[end]);
			if(left_ok && right_ok)
				return true;
			pos++;
		}
		return false;
	}

	bool EndsWithCI(const std::string &s, const char *suffix_lower)
	{
		std::string lower = ToLowerAscii(s);
		size_t suffix_len = strlen(suffix_lower);
		if(lower.size() < suffix_len)
			return false;
		return lower.compare(lower.size() - suffix_len, suffix_len, suffix_lower) == 0;
	}

	// Ports PsCloudOwnership.parseEntitlement.
	bool ParsePs5Entitlement(json_object *ent, Ps5Entitlement *out)
	{
		std::string id = JsonGetString(ent, "id");
		if(id.empty())
			return false;

		json_object *game_meta = nullptr;
		bool has_meta = json_object_object_get_ex(ent, "game_meta", &game_meta);

		out->id = id;
		out->product_id = JsonGetString(ent, "product_id");

		json_object *active_obj = nullptr;
		out->active_flag = json_object_object_get_ex(ent, "active_flag", &active_obj) && json_object_get_boolean(active_obj);

		out->package_type = has_meta ? JsonGetString(game_meta, "package_type") : "";

		out->name = has_meta ? JsonGetString(game_meta, "name") : "";
		if(out->name.empty())
			out->name = id;

		out->sku_type = JsonGetString(ent, "sku_type");
		if(out->sku_type.empty() && has_meta)
			out->sku_type = JsonGetString(game_meta, "sku_type");

		json_object *feature_obj = nullptr;
		out->feature_type = json_object_object_get_ex(ent, "feature_type", &feature_obj) ? json_object_get_int(feature_obj) : 0;

		out->icon_url = has_meta ? JsonGetString(game_meta, "icon_url") : "";
		return true;
	}

	// Ports PsCloudOwnership.filterOwnedPs5Games exactly. feature_type 0 is
	// DLC/add-ons/themes/avatars (never streamable games); feature_type 1
	// (PS Plus subscription access and Game Trials) is intentionally kept -
	// Game Trials are valid streaming entries.
	bool KeepPs5Entitlement(const Ps5Entitlement &ent)
	{
		if(!ent.active_flag)
			return false;
		if(ent.feature_type == 0)
			return false;
		if(EndsWithCI(ent.package_type, "gt"))
			return false;
		if(ContainsCI(ent.sku_type, "trial"))
			return false;
		if(ContainsWordCI(ent.name, "demo") || ContainsWordCI(ent.name, "trial"))
			return false;
		return true;
	}

	// Digital extras (artbooks, soundtracks, bonus-content viewer apps) are
	// sold as their own entitlement but Sony gives them the exact same
	// package_type/featureType shape as a real game, so name matching is the
	// only signal available - matches upstream's DIGITAL_EXTRA_NAME_PATTERNS.
	bool IsDigitalExtraName(const std::string &name)
	{
		std::string lower = ToLowerAscii(name);
		static const char *kSubstrings[] = {
			"artbook", "art book", "soundtrack", "content viewer", "bonus content",
		};
		for(const char *needle : kSubstrings)
			if(lower.find(needle) != std::string::npos)
				return true;
		return lower.rfind("the art of ", 0) == 0; // e.g. "The Art of Starfield"
	}

	bool IsFullGamePs5Entitlement(const Ps5Entitlement &ent)
	{
		return ent.feature_type == 3 || EndsWithCI(ent.package_type, "gd");
	}

	// Ports PsCloudOwnership.ownedStreamRank - picks which entitlement wins
	// when several resolve to the same stream id, preferring PS5-native
	// (PPSA) full-game entitlements whose id equals their own product_id.
	int OwnedPs5StreamRank(const Ps5Entitlement &ent)
	{
		int rank = 0;
		if(!ent.product_id.empty() && ent.product_id == ent.id)
			rank += 4;
		if(IsFullGamePs5Entitlement(ent))
			rank += 2;
		if(!ent.id.empty())
			rank += 1;
		if(ent.id.find("PPSA") != std::string::npos)
			rank += 3;
		return rank;
	}

	// Ports PsCloudOwnership.buildOwnedGamesFromEntitlements: builds the
	// Library list directly from filtered entitlements, restricted to
	// PS5-native (PPSA) ids since this Library is the PS Cloud tab, not
	// PSNOW - no public-catalog membership check, nothing to hardcode per
	// title.
	std::vector<CloudGame> BuildOwnedPs5GamesFromEntitlements(const std::vector<Ps5Entitlement> &filtered)
	{
		std::vector<std::pair<std::string, const Ps5Entitlement *>> resolved;
		for(const auto &ent : filtered)
		{
			// PSTRACK entitlements are Sony-issued analytics placeholders that
			// ride along with a real purchase under the same product_id, not a
			// separately streamable product; PSMEDIA is a PS5-native media app
			// (Netflix, YouTube, etc), not a game.
			if(ent.package_type == "PSMEDIA" || ent.package_type == "PSTRACK")
				continue;
			if(IsDigitalExtraName(ent.name))
				continue;

			// bestStreamIdentifier(id, productId): id always wins when present,
			// and parseEntitlement already guarantees id is non-empty here.
			const std::string &stream_id = ent.id.empty() ? ent.product_id : ent.id;
			if(stream_id.find("PPSA") == std::string::npos)
				continue;

			resolved.push_back({stream_id, &ent});
		}

		std::vector<std::string> order;
		std::map<std::string, const Ps5Entitlement *> best_by_id;
		std::map<std::string, int> best_rank_by_id;
		for(const auto &entry : resolved)
		{
			const std::string &stream_id = entry.first;
			const Ps5Entitlement *ent = entry.second;
			int rank = OwnedPs5StreamRank(*ent);
			auto it = best_rank_by_id.find(stream_id);
			if(it == best_rank_by_id.end())
			{
				order.push_back(stream_id);
				best_by_id[stream_id] = ent;
				best_rank_by_id[stream_id] = rank;
			}
			else if(rank > it->second)
			{
				best_by_id[stream_id] = ent;
				it->second = rank;
			}
		}

		std::vector<CloudGame> games;
		for(const std::string &stream_id : order)
		{
			const Ps5Entitlement *best = best_by_id[stream_id];
			CloudGame game;
			game.product_id = stream_id;
			game.entitlement_id = best->id;
			game.name = best->name;
			game.image_url = best->icon_url;
			game.platform = "ps5";
			game.service_type = "pscloud";
			games.push_back(game);
		}
		return games;
	}

	// One page of the paginated internal_entitlements fetch, parsed into raw
	// entitlements (unfiltered) - matches upstream's fetchEntitlementsPaginated.
	bool FetchPs5EntitlementsPage(const std::string &token, int start,
		std::vector<Ps5Entitlement> *out_page, int *out_page_count, std::string *out_error)
	{
		const int page_size = 300;
		std::string url = "https://commerce.api.np.km.playstation.net/commerce/api/v1/users/me/internal_entitlements"
			"?fields=game_meta&entitlement_type=5&start=" + std::to_string(start) + "&size=" + std::to_string(page_size);

		std::vector<std::string> headers = {
			"Authorization: Bearer " + token,
			"Accept: application/json",
		};

		HttpResponse resp;
		if(!HttpRequest(url, "GET", headers, "", &resp, out_error))
			return false;

		if(resp.status == 401 || resp.status == 403)
		{
			*out_error = "Authentication failed fetching owned games (HTTP " + std::to_string(resp.status) + ")";
			return false;
		}
		if(resp.status != 200)
		{
			*out_error = "Owned games request failed (HTTP " + std::to_string(resp.status) + ")";
			return false;
		}

		json_object *root = json_tokener_parse(resp.body.c_str());
		if(!root)
		{
			*out_error = "Invalid JSON from owned games response";
			return false;
		}

		int count = 0;
		json_object *entitlements = nullptr;
		if(json_object_object_get_ex(root, "entitlements", &entitlements) && json_object_is_type(entitlements, json_type_array))
		{
			count = json_object_array_length(entitlements);
			for(int i = 0; i < count; i++)
			{
				Ps5Entitlement ent;
				if(ParsePs5Entitlement(json_object_array_get_idx(entitlements, i), &ent))
					out_page->push_back(ent);
			}
		}
		json_object_put(root);

		*out_page_count = count;
		return true;
	}

	std::string CacheFilePath(const std::string &key)
	{
		return "cloud_cache_" + key + ".json";
	}

	// Cache format: {"cached_at": <unix seconds>, "games": [...]}
	bool ReadCache(const std::string &key, std::vector<CloudGame> *out_games)
	{
		std::ifstream file(CacheFilePath(key));
		if(!file.is_open())
			return false;

		std::stringstream ss;
		ss << file.rdbuf();
		std::string contents = ss.str();
		if(contents.empty())
			return false;

		json_object *root = json_tokener_parse(contents.c_str());
		if(!root)
			return false;

		json_object *cached_at_obj = nullptr;
		if(!json_object_object_get_ex(root, "cached_at", &cached_at_obj))
		{
			json_object_put(root);
			return false;
		}

		int64_t cached_at = json_object_get_int64(cached_at_obj);
		int64_t now = (int64_t)std::time(nullptr);
		if(now - cached_at > kCacheDurationSeconds)
		{
			json_object_put(root);
			return false;
		}

		json_object *games_obj = nullptr;
		if(!json_object_object_get_ex(root, "games", &games_obj) || !json_object_is_type(games_obj, json_type_array))
		{
			json_object_put(root);
			return false;
		}

		int len = json_object_array_length(games_obj);
		for(int i = 0; i < len; i++)
		{
			json_object *g = json_object_array_get_idx(games_obj, i);
			CloudGame game;
			game.product_id = JsonGetString(g, "product_id");
			game.entitlement_id = JsonGetString(g, "entitlement_id");
			game.name = JsonGetString(g, "name");
			game.image_url = JsonGetString(g, "image_url");
			game.platform = JsonGetString(g, "platform");
			game.service_type = JsonGetString(g, "service_type");
			json_object *catalog_streamable_obj = nullptr;
			game.catalog_streamable = json_object_object_get_ex(g, "catalog_streamable", &catalog_streamable_obj)
				&& json_object_get_boolean(catalog_streamable_obj);
			out_games->push_back(game);
		}

		json_object_put(root);
		return true;
	}

	void WriteCache(const std::string &key, const std::vector<CloudGame> &games)
	{
		json_object *root = json_object_new_object();
		json_object_object_add(root, "cached_at", json_object_new_int64((int64_t)std::time(nullptr)));

		json_object *games_arr = json_object_new_array();
		for(const auto &game : games)
		{
			json_object *g = json_object_new_object();
			json_object_object_add(g, "product_id", json_object_new_string(game.product_id.c_str()));
			json_object_object_add(g, "entitlement_id", json_object_new_string(game.entitlement_id.c_str()));
			json_object_object_add(g, "name", json_object_new_string(game.name.c_str()));
			json_object_object_add(g, "image_url", json_object_new_string(game.image_url.c_str()));
			json_object_object_add(g, "platform", json_object_new_string(game.platform.c_str()));
			json_object_object_add(g, "service_type", json_object_new_string(game.service_type.c_str()));
			json_object_object_add(g, "catalog_streamable", json_object_new_boolean(game.catalog_streamable));
			json_object_array_add(games_arr, g);
		}
		json_object_object_add(root, "games", games_arr);

		std::ofstream file(CacheFilePath(key), std::ofstream::trunc);
		if(file.is_open())
			file << json_object_to_json_string(root);

		json_object_put(root);
	}

	// Reduces various PS5 product-id formats to a comparable "stable key" so
	// an owned entitlement's product id (e.g. "PPSA01147_00") matches the
	// public catalog's differently-formatted id for the same title (e.g.
	// "EP9000-PPSA01147_00-SUFFIX" - both reduce to "PPSA01147"). Plain
	// product_id/entitlement_id equality alone matches almost nothing
	// between these two id spaces; mirrors CloudPad Android's
	// PsCloudOwnership.productIdStableKey exactly (including the multi-token
	// fallback for ids with no PPSA/CUSA number), since that's the actual
	// signal upstream's own streamability cross-reference relies on.
	std::string ProductIdStableKey(const std::string &product_id)
	{
		if(product_id.empty())
			return "";

		static const std::regex title_id_re("(PPSA|CUSA)[0-9]+");
		std::smatch m;
		if(std::regex_search(product_id, m, title_id_re))
			return m[0];

		std::vector<std::string> tokens;
		std::stringstream dash_stream(product_id);
		std::string dash_part;
		while(std::getline(dash_stream, dash_part, '-'))
		{
			std::stringstream underscore_stream(dash_part);
			std::string token;
			while(std::getline(underscore_stream, token, '_'))
				if(!token.empty())
					tokens.push_back(token);
		}
		if(tokens.size() < 2)
			return "";

		tokens.pop_back();
		std::string key;
		for(size_t i = 0; i < tokens.size(); i++)
		{
			if(i > 0)
				key += "|";
			key += tokens[i];
		}
		return key;
	}
}

std::string PlatformForPsnowProductId(const std::string &product_id)
{
	if(product_id.find("-CUSA") != std::string::npos || product_id.find("-PPSA") != std::string::npos)
		return "ps4";
	return "ps3";
}

CloudCatalog::CloudCatalog(ChiakiLog *log)
	: log(log)
{
}

bool CloudCatalog::FetchPsnowOAuthCode(const std::string &npsso, const std::string &duid,
	std::string *out_code, std::string *out_error)
{
	std::string url = std::string(kAccountBase) + "/v1/oauth/authorize"
		"?smcid=" + UrlEncode("pc:psnow") +
		"&applicationId=psnow"
		"&response_type=code"
		"&scope=" + UrlEncode(kPs4Scopes) +
		"&client_id=" + kKamajiClientId +
		"&redirect_uri=" + UrlEncode(kRedirectUri) +
		"&service_entity=" + UrlEncode("urn:service-entity:psn") +
		"&prompt=none"
		"&renderMode=mobilePortrait"
		"&hidePageElements=forgotPasswordLink"
		"&displayFooter=none"
		"&disableLinks=qriocityLink"
		"&mid=PSNOW"
		"&duid=" + UrlEncode(duid) +
		"&layout_type=popup"
		"&service_logo=ps"
		"&tp_psn=true"
		"&noEVBlock=true";

	std::vector<std::string> headers = {
		std::string("User-Agent: ") + kKamajiUserAgent,
		"Cookie: npsso=" + npsso,
	};

	HttpResponse resp;
	if(!HttpRequest(url, "GET", headers, "", &resp, out_error))
		return false;

	if(resp.status != 302 || resp.redirect_url.empty())
	{
		*out_error = "PSNOW OAuth request failed (HTTP " + std::to_string(resp.status) + ")";
		return false;
	}

	std::string code = ExtractQueryParam(resp.redirect_url, "code");
	if(code.empty())
	{
		*out_error = "No authorization code in PSNOW OAuth redirect";
		return false;
	}

	*out_code = code;
	return true;
}

bool CloudCatalog::FetchPsnowSession(const std::string &code, const std::string &duid,
	std::string *out_jsessionid, std::string *out_error)
{
	std::string url = std::string(kKamajiBase) + "/user/session";
	std::string body = "code=" + code + "&client_id=" + kKamajiClientId + "&duid=" + duid;

	std::vector<std::string> headers = {
		"Content-Type: text/plain;charset=UTF-8",
		std::string("User-Agent: ") + kKamajiUserAgent,
		std::string("X-Alt-Referer: ") + kRedirectUri,
		std::string("Origin: ") + kOrigin,
		std::string("Referer: ") + kReferer,
		"Accept: */*",
	};

	HttpResponse resp;
	if(!HttpRequest(url, "POST", headers, body, &resp, out_error))
		return false;

	if(resp.status != 200)
	{
		*out_error = "PSNOW session creation failed (HTTP " + std::to_string(resp.status) + ")";
		return false;
	}

	json_object *root = json_tokener_parse(resp.body.c_str());
	if(!root)
	{
		*out_error = "Invalid JSON in PSNOW session response";
		return false;
	}
	json_object *header_obj = nullptr;
	json_object_object_get_ex(root, "header", &header_obj);
	std::string status_code = JsonGetString(header_obj, "status_code");
	json_object_put(root);

	if(status_code != "0x0000")
	{
		*out_error = "PSNOW session failed with status: " + status_code;
		return false;
	}

	std::string jsessionid = ExtractJSessionId(resp.headers);
	if(jsessionid.empty())
	{
		*out_error = "No JSESSIONID in PSNOW session response";
		return false;
	}

	*out_jsessionid = jsessionid;
	return true;
}

bool CloudCatalog::FetchPsnowBaseUrl(const std::string &jsessionid,
	std::string *out_base_url, std::string *out_error)
{
	std::string url = std::string(kKamajiBase) + "/user/stores";
	std::vector<std::string> headers = {
		std::string("User-Agent: ") + kKamajiUserAgent,
		"Cookie: JSESSIONID=" + jsessionid,
		std::string("Origin: ") + kOrigin,
		std::string("Referer: ") + kReferer,
		"Accept: application/json",
	};

	HttpResponse resp;
	if(!HttpRequest(url, "GET", headers, "", &resp, out_error))
		return false;

	if(resp.status != 200)
	{
		*out_error = "PSNOW stores request failed (HTTP " + std::to_string(resp.status) + ")";
		return false;
	}

	json_object *root = json_tokener_parse(resp.body.c_str());
	if(!root)
	{
		*out_error = "Invalid JSON in PSNOW stores response";
		return false;
	}
	json_object *header_obj = nullptr;
	json_object *data_obj = nullptr;
	json_object_object_get_ex(root, "header", &header_obj);
	json_object_object_get_ex(root, "data", &data_obj);
	std::string status_code = JsonGetString(header_obj, "status_code");
	std::string base_url = JsonGetString(data_obj, "base_url");
	json_object_put(root);

	if(status_code != "0x0000" || base_url.empty())
	{
		*out_error = "PSNOW stores request did not return a base_url";
		return false;
	}

	*out_base_url = base_url;
	return true;
}

bool CloudCatalog::FetchPsnowCategoryUrls(const std::string &jsessionid, const std::string &base_url,
	std::vector<std::string> *out_category_urls, std::string *out_error)
{
	std::string url = base_url + "?size=100";
	std::vector<std::string> headers = {
		std::string("User-Agent: ") + kKamajiUserAgent,
		"Cookie: JSESSIONID=" + jsessionid,
		std::string("Origin: ") + kOrigin,
		std::string("Referer: ") + kReferer,
		"Accept: application/json",
	};

	HttpResponse resp;
	if(!HttpRequest(url, "GET", headers, "", &resp, out_error))
		return false;

	if(resp.status != 200)
	{
		*out_error = "PSNOW root container request failed (HTTP " + std::to_string(resp.status) + ")";
		return false;
	}

	json_object *root = json_tokener_parse(resp.body.c_str());
	if(!root)
	{
		*out_error = "Invalid JSON in PSNOW root container response";
		return false;
	}

	json_object *links = nullptr;
	if(json_object_object_get_ex(root, "links", &links) && json_object_is_type(links, json_type_array))
	{
		int len = json_object_array_length(links);
		for(int i = 0; i < len; i++)
		{
			json_object *link = json_object_array_get_idx(links, i);
			std::string name = JsonGetString(link, "name");
			for(const char *pattern : kPsnowCategoryNames)
			{
				if(name == pattern)
				{
					std::string cat_url = JsonGetString(link, "url");
					if(!cat_url.empty())
						out_category_urls->push_back(cat_url);
					break;
				}
			}
		}
	}
	json_object_put(root);

	if(out_category_urls->empty())
	{
		*out_error = "No alphabetical category URLs found in PSNOW root container";
		return false;
	}

	return true;
}

bool CloudCatalog::FetchPsnowCategoryGames(const std::string &jsessionid, const std::string &category_url,
	std::vector<CloudGame> *out_games, std::string *out_error)
{
	std::string url = category_url + (category_url.find('?') == std::string::npos ? "?" : "&") + "start=0&size=500";
	std::vector<std::string> headers = {
		"Content-Type: application/json",
		"Accept: application/json",
		std::string("User-Agent: ") + kBrowserUserAgent,
	};

	HttpResponse resp;
	if(!HttpRequest(url, "GET", headers, "", &resp, out_error))
		return false;

	if(resp.status != 200)
	{
		*out_error = "PSNOW category request failed (HTTP " + std::to_string(resp.status) + ")";
		return false;
	}

	json_object *root = json_tokener_parse(resp.body.c_str());
	if(!root)
		return true; // treat unparsable category page as "no games", not fatal

	json_object *links = nullptr;
	if(json_object_object_get_ex(root, "links", &links) && json_object_is_type(links, json_type_array))
	{
		int len = json_object_array_length(links);
		for(int i = 0; i < len; i++)
		{
			json_object *g = json_object_array_get_idx(links, i);
			CloudGame game;
			game.product_id = JsonGetString(g, "id");
			game.name = JsonGetString(g, "name");
			game.image_url = ExtractCoverImageUrl(g);
			game.service_type = "psnow";
			game.platform = PlatformForPsnowProductId(game.product_id);
			if(!game.product_id.empty())
				out_games->push_back(game);
		}
	}
	json_object_put(root);
	return true;
}

bool CloudCatalog::FetchPsnowCatalog(const std::string &npsso, const std::string &duid,
	std::vector<CloudGame> *out_games, std::string *out_error, bool force_refresh)
{
	if(!force_refresh && ReadCache("psnow", out_games))
	{
		CHIAKI_LOGI(log, "CloudCatalog: using cached PSNOW catalog (%zu games)", out_games->size());
		return true;
	}

	if(npsso.empty())
	{
		*out_error = "Not signed in to PlayStation";
		return false;
	}

	std::string code, jsessionid, base_url;
	std::vector<std::string> category_urls;

	if(!FetchPsnowOAuthCode(npsso, duid, &code, out_error))
		return false;
	if(!FetchPsnowSession(code, duid, &jsessionid, out_error))
		return false;
	if(!FetchPsnowBaseUrl(jsessionid, &base_url, out_error))
		return false;
	if(!FetchPsnowCategoryUrls(jsessionid, base_url, &category_urls, out_error))
		return false;

	std::vector<CloudGame> all_games;
	for(size_t i = 0; i < category_urls.size(); i++)
	{
		std::string category_error;
		if(!FetchPsnowCategoryGames(jsessionid, category_urls[i], &all_games, &category_error))
			CHIAKI_LOGW(log, "CloudCatalog: PSNOW category %zu failed: %s", i, category_error.c_str());

		// Small pacing delay between category calls, matching the 100ms
		// rate-limit cooldown the upstream implementation uses.
		if(i + 1 < category_urls.size())
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}

	// Deduplicate by product id, same as upstream processPsnowCatalogComplete
	std::vector<CloudGame> unique_games;
	for(const auto &game : all_games)
	{
		bool seen = std::any_of(unique_games.begin(), unique_games.end(),
			[&](const CloudGame &g) { return g.product_id == game.product_id; });
		if(!seen)
			unique_games.push_back(game);
	}

	if(unique_games.empty())
	{
		*out_error = "PSNOW catalog returned no games";
		return false;
	}

	WriteCache("psnow", unique_games);
	*out_games = unique_games;
	CHIAKI_LOGI(log, "CloudCatalog: fetched PSNOW catalog (%zu games)", out_games->size());
	return true;
}

bool CloudCatalog::FetchPs5CloudCatalog(const std::string &locale,
	std::vector<CloudGame> *out_games, std::string *out_error, bool force_refresh)
{
	if(!force_refresh && ReadCache("ps5cloud", out_games))
	{
		CHIAKI_LOGI(log, "CloudCatalog: using cached PS5 cloud catalog (%zu games)", out_games->size());
		return true;
	}

	std::string lower_locale = locale;
	std::transform(lower_locale.begin(), lower_locale.end(), lower_locale.begin(), ::tolower);

	std::string url = "https://www.playstation.com/bin/imagic/gameslist?locale=" +
		UrlEncode(lower_locale) + "&categoryList=all-ps5-list";

	std::vector<std::string> headers = {
		"Content-Type: application/json",
		"Accept: application/json",
		std::string("User-Agent: ") + kBrowserUserAgent,
	};

	HttpResponse resp;
	if(!HttpRequest(url, "GET", headers, "", &resp, out_error))
		return false;

	if(resp.status != 200)
	{
		*out_error = "PS5 cloud catalog request failed (HTTP " + std::to_string(resp.status) + ")";
		return false;
	}

	json_object *root = json_tokener_parse(resp.body.c_str());
	if(!root || !json_object_is_type(root, json_type_array))
	{
		*out_error = "Invalid response format from PS5 cloud catalog";
		if(root)
			json_object_put(root);
		return false;
	}

	std::vector<CloudGame> games;
	int cat_len = json_object_array_length(root);
	for(int i = 0; i < cat_len; i++)
	{
		json_object *category = json_object_array_get_idx(root, i);
		json_object *games_arr = nullptr;
		if(!json_object_object_get_ex(category, "games", &games_arr) || !json_object_is_type(games_arr, json_type_array))
			continue;

		int game_len = json_object_array_length(games_arr);
		for(int j = 0; j < game_len; j++)
		{
			json_object *g = json_object_array_get_idx(games_arr, j);
			json_object *streaming_obj = nullptr;
			if(!json_object_object_get_ex(g, "streamingSupported", &streaming_obj) || !json_object_get_boolean(streaming_obj))
				continue;

			CloudGame game;
			// "productId" is the field FetchOwnedPs5CloudGames's cross-reference
			// matches against (that's the literal key name upstream's own
			// processCrossReferenceComplete() reads from this same catalog
			// response) - "id"/"conceptId" are content ids, not product ids,
			// and only useful as a last-resort display key if productId is
			// somehow absent.
			game.product_id = JsonGetString(g, "productId");
			if(game.product_id.empty())
				game.product_id = JsonGetString(g, "id");
			if(game.product_id.empty())
				game.product_id = JsonGetString(g, "conceptId");
			game.name = JsonGetString(g, "name");
			game.image_url = ExtractCoverImageUrl(g);
			game.platform = "ps5";
			game.service_type = "pscloud";
			if(!game.product_id.empty())
				games.push_back(game);
		}
	}
	json_object_put(root);

	if(games.empty())
	{
		*out_error = "PS5 cloud catalog returned no streaming-supported games";
		return false;
	}

	WriteCache("ps5cloud", games);
	*out_games = games;
	CHIAKI_LOGI(log, "CloudCatalog: fetched PS5 cloud catalog (%zu games)", out_games->size());
	return true;
}

bool CloudCatalog::FetchOwnedPs5EntitlementToken(const std::string &npsso,
	std::string *out_token, std::string *out_error)
{
	std::string url = std::string(kAccountBase) + "/v1/oauth/authorize"
		"?response_type=token"
		"&scope=" + UrlEncode("kamaji:get_internal_entitlements user:account.attributes.validate") +
		"&client_id=dc523cc2-b51b-4190-bff0-3397c06871b3"
		"&redirect_uri=" + UrlEncode(kRedirectUri) +
		"&service_entity=" + UrlEncode("urn:service-entity:psn") +
		"&prompt=none";

	std::vector<std::string> headers = {
		"Cookie: npsso=" + npsso,
		std::string("User-Agent: ") + kBrowserUserAgent,
	};

	HttpResponse resp;
	if(!HttpRequest(url, "GET", headers, "", &resp, out_error))
		return false;

	if(resp.status != 302 || resp.redirect_url.empty())
	{
		*out_error = "OAuth request failed fetching owned PS5 games (HTTP " + std::to_string(resp.status) + ")";
		return false;
	}

	// The token comes back in the URL fragment (#access_token=...), not the
	// query string, but ExtractQueryParam already treats '#' as a boundary
	// the same way it treats '&', so it works for both.
	std::string token = ExtractQueryParam(resp.redirect_url, "access_token");
	if(token.empty())
	{
		*out_error = "Could not extract access token for owned PS5 games lookup";
		return false;
	}

	*out_token = token;
	return true;
}

bool CloudCatalog::FetchOwnedPs5CloudGames(const std::string &npsso, const std::string &locale,
	std::vector<CloudGame> *out_games, std::string *out_error, bool force_refresh)
{
	if(!force_refresh && ReadCache("ps5owned", out_games))
	{
		CHIAKI_LOGI(log, "CloudCatalog: using cached owned PS5 games (%zu games)", out_games->size());
		return true;
	}

	if(npsso.empty())
	{
		*out_error = "Not signed in to PlayStation";
		return false;
	}

	std::string token;
	if(!FetchOwnedPs5EntitlementToken(npsso, &token, out_error))
		return false;

	std::vector<Ps5Entitlement> raw;
	int start = 0;
	while(true)
	{
		std::vector<Ps5Entitlement> page;
		int page_count = 0;
		if(!FetchPs5EntitlementsPage(token, start, &page, &page_count, out_error))
			return false;
		raw.insert(raw.end(), page.begin(), page.end());
		if(page_count < 300)
			break;
		start += page_count;
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}

	std::vector<Ps5Entitlement> filtered;
	for(const auto &ent : raw)
		if(KeepPs5Entitlement(ent))
			filtered.push_back(ent);

	std::vector<CloudGame> games = BuildOwnedPs5GamesFromEntitlements(filtered);
	CHIAKI_LOGI(log, "CloudCatalog: %zu raw entitlements, %zu after filter, %zu owned PS5 games",
		raw.size(), filtered.size(), games.size());

	// Best-effort box art upgrade: entitlements only carry a small square
	// icon, while the public catalog has real cover art for most titles.
	// Never required for a game to count as owned or to stream - if this
	// fetch fails or a title has no catalog match, it just keeps its
	// entitlement icon - matches upstream's enrichWithCatalogArt intent.
	//
	// The same cross-reference also seeds catalog_streamable: FetchPs5CloudCatalog
	// already filters its results to streamingSupported==true titles, so a
	// match here is upstream's own applyStreamabilityHints signal (matched
	// against the public catalog => STREAMABLE guess) - see CloudGame above.
	std::vector<CloudGame> catalog;
	std::string catalog_error;
	if(FetchPs5CloudCatalog(locale, &catalog, &catalog_error, force_refresh))
	{
		// Owned entitlement product ids (e.g. "PPSA01147_00") and the public
		// imagic catalog's ids for the same title (e.g.
		// "EP9000-PPSA01147_00-SUFFIX") are formatted differently - plain
		// equality below only ever catches a handful of titles, so a stable
		// key index is the primary match path, with plain equality kept as a
		// cheap first try.
		std::map<std::string, const CloudGame *> catalog_by_stable_key;
		for(const auto &c : catalog)
		{
			std::string key = ProductIdStableKey(c.product_id);
			if(!key.empty() && catalog_by_stable_key.find(key) == catalog_by_stable_key.end())
				catalog_by_stable_key[key] = &c;
		}

		for(auto &game : games)
		{
			const CloudGame *match = nullptr;
			for(const auto &c : catalog)
			{
				if(c.product_id == game.product_id || c.product_id == game.entitlement_id)
				{
					match = &c;
					break;
				}
			}
			if(!match)
			{
				std::string key = ProductIdStableKey(game.product_id);
				if(key.empty())
					key = ProductIdStableKey(game.entitlement_id);
				if(!key.empty())
				{
					auto it = catalog_by_stable_key.find(key);
					if(it != catalog_by_stable_key.end())
						match = it->second;
				}
			}
			if(match)
			{
				if(!match->image_url.empty())
					game.image_url = match->image_url;
				game.catalog_streamable = true;
			}
		}
	}

	if(games.empty())
	{
		*out_error = "No owned PS5 titles found";
		return false;
	}

	WriteCache("ps5owned", games);
	*out_games = games;
	return true;
}
