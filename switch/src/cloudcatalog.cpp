// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include "cloudcatalog.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>
#include <thread>

#include <curl/curl.h>
#include <json-c/json.h>

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

	struct HttpResponse
	{
		long status = 0;
		std::string body;
		std::string headers;
		std::string redirect_url;
	};

	size_t BodyWriteCb(void *contents, size_t size, size_t nmemb, std::string *out)
	{
		out->append((char *)contents, size * nmemb);
		return size * nmemb;
	}

	size_t HeaderWriteCb(void *contents, size_t size, size_t nmemb, std::string *out)
	{
		out->append((char *)contents, size * nmemb);
		return size * nmemb;
	}

	// Blocking HTTP call, matches the DiscoveryManager::makeRequest precedent
	// already used on this Switch target (peer verification disabled - no CA
	// bundle wired up on-device yet).
	bool HttpRequest(const std::string &url, const std::string &method,
		const std::vector<std::string> &headers, const std::string &body,
		HttpResponse *out, std::string *out_error)
	{
		CURL *curl = curl_easy_init();
		if(!curl)
		{
			*out_error = "Failed to initialize CURL";
			return false;
		}

		struct curl_slist *header_list = NULL;
		for(const auto &h : headers)
			header_list = curl_slist_append(header_list, h.c_str());

		curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
		curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list);
		curl_easy_setopt(curl, CURLOPT_USERAGENT, kBrowserUserAgent);
		curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);
		curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
		curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
		curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
		curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, BodyWriteCb);
		curl_easy_setopt(curl, CURLOPT_WRITEDATA, &out->body);
		curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, HeaderWriteCb);
		curl_easy_setopt(curl, CURLOPT_HEADERDATA, &out->headers);

		if(method == "POST")
		{
			curl_easy_setopt(curl, CURLOPT_POST, 1L);
			curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
		}

		CURLcode res = curl_easy_perform(curl);
		if(res != CURLE_OK)
		{
			*out_error = curl_easy_strerror(res);
			curl_slist_free_all(header_list);
			curl_easy_cleanup(curl);
			return false;
		}

		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &out->status);
		char *redirect = nullptr;
		curl_easy_getinfo(curl, CURLINFO_REDIRECT_URL, &redirect);
		if(redirect)
			out->redirect_url = redirect;

		curl_slist_free_all(header_list);
		curl_easy_cleanup(curl);
		return true;
	}

	std::string UrlEncode(const std::string &value)
	{
		CURL *curl = curl_easy_init();
		if(!curl)
			return value;
		char *escaped = curl_easy_escape(curl, value.c_str(), (int)value.length());
		std::string result = escaped ? escaped : value;
		if(escaped)
			curl_free(escaped);
		curl_easy_cleanup(curl);
		return result;
	}

	std::string ExtractQueryParam(const std::string &url, const std::string &param)
	{
		std::string needle = param + "=";
		size_t search_from = 0;
		while(true)
		{
			size_t pos = url.find(needle, search_from);
			if(pos == std::string::npos)
				return "";
			// only match at a query/fragment boundary (?, &, or #)
			if(pos == 0 || url[pos - 1] == '?' || url[pos - 1] == '&' || url[pos - 1] == '#')
			{
				size_t value_start = pos + needle.length();
				size_t value_end = url.find_first_of("&#", value_start);
				return url.substr(value_start, value_end == std::string::npos ? std::string::npos : value_end - value_start);
			}
			search_from = pos + needle.length();
		}
	}

	// Pulls "JSESSIONID=..." out of a raw Set-Cookie header blob.
	std::string ExtractJSessionId(const std::string &headers)
	{
		size_t pos = headers.find("JSESSIONID=");
		if(pos == std::string::npos)
			return "";
		size_t value_start = pos + strlen("JSESSIONID=");
		size_t value_end = headers.find_first_of(";\r\n", value_start);
		return headers.substr(value_start, value_end == std::string::npos ? std::string::npos : value_end - value_start);
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

	std::string JsonGetString(json_object *obj, const char *key)
	{
		json_object *value = nullptr;
		if(obj && json_object_object_get_ex(obj, key, &value))
		{
			const char *s = json_object_get_string(value);
			return s ? s : "";
		}
		return "";
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
			game.name = JsonGetString(g, "name");
			game.image_url = JsonGetString(g, "image_url");
			game.platform = JsonGetString(g, "platform");
			game.service_type = JsonGetString(g, "service_type");
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
			json_object_object_add(g, "name", json_object_new_string(game.name.c_str()));
			json_object_object_add(g, "image_url", json_object_new_string(game.image_url.c_str()));
			json_object_object_add(g, "platform", json_object_new_string(game.platform.c_str()));
			json_object_object_add(g, "service_type", json_object_new_string(game.service_type.c_str()));
			json_object_array_add(games_arr, g);
		}
		json_object_object_add(root, "games", games_arr);

		std::ofstream file(CacheFilePath(key), std::ofstream::trunc);
		if(file.is_open())
			file << json_object_to_json_string(root);

		json_object_put(root);
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
	std::vector<CloudGame> *out_games, std::string *out_error)
{
	if(ReadCache("psnow", out_games))
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
	std::vector<CloudGame> *out_games, std::string *out_error)
{
	if(ReadCache("ps5cloud", out_games))
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
