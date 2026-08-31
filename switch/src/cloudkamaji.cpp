// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include "cloudkamaji.h"
#include "cloudhttp.h"

#include <json-c/json.h>

using cloudhttp::HttpRequest;
using cloudhttp::HttpResponse;
using cloudhttp::JsonGetString;
using cloudhttp::UrlEncode;
using cloudhttp::ExtractQueryParam;

namespace
{
	const char *kAccountBase = "https://ca.account.sony.com/api";
	const char *kKamajiBase = "https://psnow.playstation.com/kamaji/api/pcnow/00_09_000";
	const char *kKamajiClientId = "bc6b0777-abb5-40da-92ca-e133cf18e989";
	const char *kPs3Scopes = "kamaji:commerce_native";
	const char *kPs4Scopes = "kamaji:commerce_native kamaji:commerce_container kamaji:lists kamaji:s2s.subscriptionsPremium.get";
	const char *kOrigin = "https://psnow.playstation.com";
	const char *kReferer = "https://psnow.playstation.com/app/2.2.0/133/5cdcc037d/";
	const char *kCommerceClientId = "dc523cc2-b51b-4190-bff0-3397c06871b3";

	std::string ExtractJSessionId(const std::string &headers)
	{
		return cloudhttp::ExtractCookieValue(headers, "JSESSIONID");
	}
}

CloudKamaji::CloudKamaji(ChiakiLog *log, std::string duid, std::string product_id,
	std::string npsso, std::string redirect_uri, std::string user_agent)
	: log(log), duid(duid), product_id(product_id), npsso(npsso),
	  redirect_uri(redirect_uri), user_agent(user_agent), scopes(kPs4Scopes)
{
}

bool CloudKamaji::Step0_5b_GetAnonAuthCode(std::string *out_code, std::string *out_error)
{
	std::string url = std::string(kAccountBase) + "/v1/oauth/authorize"
		"?smcid=" + UrlEncode("pc:psnow") +
		"&applicationId=psnow"
		"&response_type=code"
		"&scope=" + UrlEncode(scopes) +
		"&client_id=" + kKamajiClientId +
		"&redirect_uri=" + UrlEncode(redirect_uri) +
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
		std::string("User-Agent: ") + user_agent,
		"Cookie: npsso=" + npsso,
	};

	HttpResponse resp;
	if(!HttpRequest(url, "GET", headers, "", &resp, out_error))
		return false;

	if(resp.status != 302 || resp.redirect_url.empty())
	{
		*out_error = "Kamaji anonymous OAuth request failed (HTTP " + std::to_string(resp.status) + ")";
		return false;
	}

	std::string code = ExtractQueryParam(resp.redirect_url, "code");
	if(code.empty())
	{
		*out_error = "No authorization code in Kamaji anonymous OAuth redirect";
		return false;
	}

	*out_code = code;
	return true;
}

bool CloudKamaji::Step0_5c_CreateAnonSession(const std::string &code, std::string *out_error)
{
	std::string url = std::string(kKamajiBase) + "/user/session";
	std::string body = "code=" + code + "&client_id=" + kKamajiClientId + "&duid=" + duid;

	std::vector<std::string> headers = {
		"Content-Type: text/plain;charset=UTF-8",
		std::string("User-Agent: ") + user_agent,
		"X-Alt-Referer: " + redirect_uri,
		"Accept: */*",
		std::string("Origin: ") + kOrigin,
		std::string("Referer: ") + kReferer,
	};

	HttpResponse resp;
	if(!HttpRequest(url, "POST", headers, body, &resp, out_error))
		return false;

	if(resp.status != 200)
	{
		*out_error = "Kamaji anonymous session failed (HTTP " + std::to_string(resp.status) + ")";
		return false;
	}

	std::string jsessionid = ExtractJSessionId(resp.headers);
	if(jsessionid.empty())
	{
		*out_error = "No JSESSIONID in Kamaji anonymous session response";
		return false;
	}

	this->jsessionid = jsessionid;

	// Capture the account's real PSN store country/language from the session
	// response body ({"data": {"country": ..., "language": ...}}), so
	// Step0_5d_ConvertProductId can query the container endpoint under the
	// right region instead of always assuming US/en - matches upstream's
	// PSKamajiSession.step0_5c_CreateAnonSession (Qt CloudCatalogBackend
	// lines 432-440). Best-effort: a missing/unparseable body just leaves the
	// US/en fallback in place, same as upstream.
	json_object *root = json_tokener_parse(resp.body.c_str());
	if(root)
	{
		json_object *data = nullptr;
		if(json_object_object_get_ex(root, "data", &data))
		{
			std::string country = JsonGetString(data, "country");
			std::string language = JsonGetString(data, "language");
			if(!country.empty() && !language.empty())
			{
				this->resolved_country = country;
				this->resolved_language = language;
				CHIAKI_LOGI(log, "CloudKamaji: resolved store locale from session: country=%s language=%s",
					country.c_str(), language.c_str());
			}
		}
		json_object_put(root);
	}

	return true;
}

bool CloudKamaji::Step0_5d_ConvertProductId(std::string *out_error)
{
	// Prefer the server-authoritative store locale captured in
	// Step0_5c_CreateAnonSession over a hardcoded default - querying under
	// the wrong region 404s for region-specific catalog SKUs (confirmed
	// against a European PS3 disc id "EP0102-BLES01227_00-..." 404ing under
	// a hardcoded US/en path). Falls back to en-US when the session response
	// didn't carry a locale, matching upstream's own fallback.
	std::string country = !resolved_country.empty() ? resolved_country : "US";
	std::string language = !resolved_language.empty() ? resolved_language : "en";

	std::string url = "https://psnow.playstation.com/store/api/pcnow/00_09_000/container/" +
		country + "/" + language + "/19/" + product_id + "?useOffers=true&gkb=1&gkb2=1";

	std::vector<std::string> headers = {
		"Accept: application/json",
		"User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36",
	};

	HttpResponse resp;
	if(!HttpRequest(url, "GET", headers, "", &resp, out_error))
		return false;

	if(resp.status != 200)
	{
		// Diagnostics: log the exact request (country/language included in
		// url above) and Sony's raw response body, in case a title still
		// 404s for a reason other than the region mismatch this function now
		// resolves against the account's real store locale.
		CHIAKI_LOGE(log, "CloudKamaji: container lookup failed (HTTP %ld) url=%s body=%s",
			resp.status, url.c_str(), resp.body.c_str());

		if(resp.status == 404)
			*out_error = "Game not found: product id '" + product_id + "' is not available for cloud streaming";
		else
			*out_error = "Failed to look up product id '" + product_id + "' (HTTP " + std::to_string(resp.status) + ")";
		return false;
	}

	json_object *root = json_tokener_parse(resp.body.c_str());
	if(!root)
	{
		*out_error = "Invalid JSON in product lookup response";
		return false;
	}

	// Streaming entitlements have license_type == 4. Prefer default_sku, then
	// fall back to scanning the skus array, same order upstream checks.
	std::string entitlement_id;
	std::string sku;

	auto scan_entitlements = [&](json_object *sku_obj) -> bool {
		json_object *entitlements = nullptr;
		if(!json_object_object_get_ex(sku_obj, "entitlements", &entitlements) || !json_object_is_type(entitlements, json_type_array))
			return false;
		int len = json_object_array_length(entitlements);
		for(int i = 0; i < len; i++)
		{
			json_object *ent = json_object_array_get_idx(entitlements, i);
			json_object *license_type_obj = nullptr;
			if(json_object_object_get_ex(ent, "license_type", &license_type_obj) && json_object_get_int(license_type_obj) == 4)
			{
				std::string id = JsonGetString(ent, "id");
				if(!id.empty())
				{
					entitlement_id = id;
					sku = JsonGetString(sku_obj, "id");
					return true;
				}
			}
		}
		return false;
	};

	json_object *default_sku = nullptr;
	if(json_object_object_get_ex(root, "default_sku", &default_sku))
		scan_entitlements(default_sku);

	if(entitlement_id.empty())
	{
		json_object *skus = nullptr;
		if(json_object_object_get_ex(root, "skus", &skus) && json_object_is_type(skus, json_type_array))
		{
			int len = json_object_array_length(skus);
			for(int i = 0; i < len && entitlement_id.empty(); i++)
				scan_entitlements(json_object_array_get_idx(skus, i));
		}
	}

	// Detect platform from playable_platform (root, or metadata.playable_platform.values)
	json_object *playable_platform = nullptr;
	if(!json_object_object_get_ex(root, "playable_platform", &playable_platform) || !json_object_is_type(playable_platform, json_type_array))
	{
		json_object *metadata = nullptr;
		json_object *pp_obj = nullptr;
		json_object *values = nullptr;
		if(json_object_object_get_ex(root, "metadata", &metadata) &&
			json_object_object_get_ex(metadata, "playable_platform", &pp_obj) &&
			json_object_object_get_ex(pp_obj, "values", &values) && json_object_is_type(values, json_type_array))
			playable_platform = values;
		else
			playable_platform = nullptr;
	}

	bool has_ps4 = false, has_ps3 = false;
	if(playable_platform)
	{
		int len = json_object_array_length(playable_platform);
		for(int i = 0; i < len; i++)
		{
			std::string s = json_object_get_string(json_object_array_get_idx(playable_platform, i));
			if(s.find("PS4") != std::string::npos)
				has_ps4 = true;
			else if(s.find("PS3") != std::string::npos)
				has_ps3 = true;
		}
	}
	platform = has_ps4 ? "ps4" : (has_ps3 ? "ps3" : "ps4");
	scopes = (platform == "ps3") ? kPs3Scopes : kPs4Scopes;

	json_object_put(root);

	if(entitlement_id.empty())
	{
		*out_error = "Could not determine a streaming entitlement for product id '" + product_id + "'";
		return false;
	}

	this->entitlement_id = entitlement_id;
	streaming_sku = sku;
	CHIAKI_LOGI(log, "CloudKamaji: product '%s' -> entitlement '%s' (platform %s)",
		product_id.c_str(), this->entitlement_id.c_str(), platform.c_str());
	return true;
}

bool CloudKamaji::Step0_5e1_GetCommerceOAuthToken(std::string *out_error)
{
	std::string url = std::string(kAccountBase) + "/v1/oauth/authorize"
		"?smcid=" + UrlEncode("pc:psnow") +
		"&applicationId=psnow"
		"&response_type=token"
		"&scope=" + UrlEncode("kamaji:get_internal_entitlements user:account.attributes.validate kamaji:get_privacy_settings user:account.settings.privacy.get kamaji:s2s.subscriptionsPremium.get") +
		"&client_id=" + kCommerceClientId +
		"&redirect_uri=" + UrlEncode(redirect_uri) +
		"&grant_type=authorization_code"
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
		std::string("User-Agent: ") + user_agent,
		"Cookie: npsso=" + npsso,
	};

	HttpResponse resp;
	if(!HttpRequest(url, "GET", headers, "", &resp, out_error))
		return false;

	if(resp.status != 302 || resp.redirect_url.empty())
	{
		*out_error = "Commerce OAuth token request failed (HTTP " + std::to_string(resp.status) + ")";
		return false;
	}

	std::string token = ExtractQueryParam(resp.redirect_url, "access_token");
	if(token.empty())
	{
		*out_error = "Could not extract access token from Commerce OAuth response";
		return false;
	}

	commerce_oauth_token = token;
	return true;
}

bool CloudKamaji::Step0_5e2_CheckEntitlementExists(bool *out_has_entitlement, std::string *out_error)
{
	std::string url = "https://commerce.api.np.km.playstation.net/commerce/api/v1/users/me/internal_entitlements/" +
		entitlement_id + "?fields=game_meta";

	std::vector<std::string> headers = {
		"Authorization: Bearer " + commerce_oauth_token,
		std::string("User-Agent: ") + user_agent,
		"Accept: application/json",
	};

	HttpResponse resp;
	if(!HttpRequest(url, "GET", headers, "", &resp, out_error))
		return false;

	if(resp.status == 200)
	{
		*out_has_entitlement = true;
		return true;
	}
	if(resp.status == 404)
	{
		*out_has_entitlement = false;
		return true;
	}

	*out_error = "Entitlement check failed (HTTP " + std::to_string(resp.status) + ")";
	return false;
}

bool CloudKamaji::Step5_GetAuthCode(std::string *out_code, std::string *out_error)
{
	std::string url = std::string(kAccountBase) + "/v1/oauth/authorize"
		"?smcid=" + UrlEncode("pc:psnow") +
		"&applicationId=psnow"
		"&response_type=code"
		"&scope=" + UrlEncode(scopes) +
		"&client_id=" + kKamajiClientId +
		"&redirect_uri=" + UrlEncode(redirect_uri) +
		"&service_entity=" + UrlEncode("urn:service-entity:psn") +
		"&prompt=none"
		"&mid=PSNOW"
		"&duid=" + UrlEncode(duid) +
		"&layout_type=popup"
		"&service_logo=ps"
		"&tp_psn=true"
		"&noEVBlock=true";

	std::vector<std::string> headers = {
		std::string("User-Agent: ") + user_agent,
		"Cookie: npsso=" + npsso,
	};

	HttpResponse resp;
	if(!HttpRequest(url, "GET", headers, "", &resp, out_error))
		return false;

	if(resp.status != 302 || resp.redirect_url.empty())
	{
		*out_error = "Kamaji authenticated OAuth request failed (HTTP " + std::to_string(resp.status) + ")";
		return false;
	}

	std::string code = ExtractQueryParam(resp.redirect_url, "code");
	if(code.empty())
	{
		*out_error = "No authorization code in Kamaji authenticated OAuth redirect";
		return false;
	}

	*out_code = code;
	return true;
}

bool CloudKamaji::Step6_CreateAuthSession(const std::string &code, std::string *out_error)
{
	std::string url = std::string(kKamajiBase) + "/user/session";
	std::string body = "code=" + code + "&client_id=" + kKamajiClientId + "&duid=" + duid;

	std::vector<std::string> headers = {
		"Content-Type: text/plain;charset=UTF-8",
		std::string("User-Agent: ") + user_agent,
		"X-Alt-Referer: " + redirect_uri,
		std::string("Origin: ") + kOrigin,
		std::string("Referer: ") + kReferer,
	};

	HttpResponse resp;
	if(!HttpRequest(url, "POST", headers, body, &resp, out_error))
		return false;

	if(resp.status != 200)
	{
		*out_error = "Kamaji authenticated session failed (HTTP " + std::to_string(resp.status) + ")";
		return false;
	}

	json_object *root = json_tokener_parse(resp.body.c_str());
	if(!root)
	{
		*out_error = "Invalid JSON in Kamaji authenticated session response";
		return false;
	}
	json_object *header_obj = nullptr;
	json_object *data_obj = nullptr;
	json_object_object_get_ex(root, "header", &header_obj);
	json_object_object_get_ex(root, "data", &data_obj);
	std::string status_code = JsonGetString(header_obj, "status_code");

	if(status_code != "0x0000")
	{
		*out_error = "Kamaji session failed with status: " + status_code;
		json_object_put(root);
		return false;
	}

	json_object_put(root);
	return true;
}

CloudKamaji::Result CloudKamaji::Run()
{
	Result result;

	if(npsso.empty())
	{
		result.error = "Not signed in to PlayStation";
		return result;
	}

	std::string anon_code;
	if(!Step0_5b_GetAnonAuthCode(&anon_code, &result.error))
		return result;
	if(!Step0_5c_CreateAnonSession(anon_code, &result.error))
		return result;
	if(!Step0_5d_ConvertProductId(&result.error))
		return result;

	if(!Step0_5e1_GetCommerceOAuthToken(&result.error))
		return result;

	bool has_entitlement = false;
	if(!Step0_5e2_CheckEntitlementExists(&has_entitlement, &result.error))
		return result;

	if(!has_entitlement)
	{
		result.error = "You don't own this title yet - add it to your library from the PlayStation "
			"app or website, then try again. (Automatic purchase isn't supported.)";
		return result;
	}

	std::string auth_code;
	if(!Step5_GetAuthCode(&auth_code, &result.error))
		return result;
	if(!Step6_CreateAuthSession(auth_code, &result.error))
		return result;

	result.success = true;
	result.entitlement_id = entitlement_id;
	result.platform = platform;
	CHIAKI_LOGI(log, "CloudKamaji: authentication complete, entitlement=%s platform=%s",
		entitlement_id.c_str(), platform.c_str());
	return result;
}
