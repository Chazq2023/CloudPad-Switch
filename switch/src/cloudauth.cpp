// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include "cloudauth.h"

#include <cctype>

#include <curl/curl.h>
#include <json-c/json.h>

namespace
{
	// OAuth credentials for cloud streaming, matching
	// PSCloudAuthConsts in the upstream gui/include/cloudstreaming/pscloudauth.h
	const char *kTokenUrl = "https://ca.account.sony.com/api/authz/v3/oauth/token";
	const char *kClientId = "d5df3976-b7fa-4651-bcc9-05ac9f0cad47";
	const char *kClientSecret = "VF8B50Lt0aqyAZH4";
	const char *kScopes =
		"id_token:email id_token:is_child id_token:age openid kamaji:get_privacy_settings "
		"user:basicProfile.get user:basicProfile.update";
	const char *kUserAgent =
		"Mozilla/5.0 (Windows NT 10.0; WOW64) AppleWebKit/537.36 (KHTML, like Gecko) "
		"playstation-now/0.0.0 Chrome/83.0.4103.104 Electron/9.0.4 Safari/537.36 gkApollo";

	size_t WriteCallback(void *contents, size_t size, size_t nmemb, std::string *userp)
	{
		userp->append((char *)contents, size * nmemb);
		return size * nmemb;
	}

	std::string Trim(const std::string &s)
	{
		size_t begin = s.find_first_not_of(" \t\r\n");
		if(begin == std::string::npos)
			return "";
		size_t end = s.find_last_not_of(" \t\r\n");
		return s.substr(begin, end - begin + 1);
	}
}

CloudAuth::CloudAuth(ChiakiLog *log)
	: log(log)
{
}

std::string CloudAuth::ExtractNPSSO(const std::string &pasted)
{
	std::string trimmed = Trim(pasted);
	if(trimmed.empty())
		return "";

	if(trimmed.front() == '{')
	{
		struct json_object *parsed = json_tokener_parse(trimmed.c_str());
		if(parsed)
		{
			struct json_object *npsso_obj = nullptr;
			std::string result;
			if(json_object_object_get_ex(parsed, "npsso", &npsso_obj))
				result = json_object_get_string(npsso_obj);
			json_object_put(parsed);
			return result;
		}
		return "";
	}

	if(trimmed.size() >= 2 && trimmed.front() == '"' && trimmed.back() == '"')
		return trimmed.substr(1, trimmed.size() - 2);

	return trimmed;
}

bool CloudAuth::ExchangeNPSSO(const std::string &npsso, const std::string &duid,
	Tokens *out_tokens, std::string *out_error)
{
	if(npsso.empty())
	{
		*out_error = "No npsso value provided";
		return false;
	}

	CURL *curl = curl_easy_init();
	if(!curl)
	{
		*out_error = "Failed to initialize CURL";
		return false;
	}

	char *encoded_scope = curl_easy_escape(curl, kScopes, 0);
	char *encoded_npsso = curl_easy_escape(curl, npsso.c_str(), (int)npsso.length());

	// Built by concatenation rather than a format helper so that literal '%'
	// characters from the percent-encoded scope are never re-interpreted.
	std::string body = std::string("scope=") + encoded_scope +
		"&npsso=" + encoded_npsso +
		"&client_id=" + kClientId +
		"&client_secret=" + kClientSecret +
		"&grant_type=sso_token" +
		"&duid=" + duid;

	curl_free(encoded_scope);
	curl_free(encoded_npsso);

	std::string response_data;
	struct curl_slist *headers = NULL;
	headers = curl_slist_append(headers, "Content-Type: application/x-www-form-urlencoded");

	curl_easy_setopt(curl, CURLOPT_URL, kTokenUrl);
	curl_easy_setopt(curl, CURLOPT_POST, 1L);
	curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(curl, CURLOPT_USERAGENT, kUserAgent);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
	// Matches the existing DiscoveryManager::makeRequest precedent already used
	// on this Switch target: no CA bundle is wired up on-device yet, so peer
	// verification is disabled here the same way it already is for the
	// account-id lookup call.
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_data);

	CURLcode res = curl_easy_perform(curl);
	curl_slist_free_all(headers);

	if(res != CURLE_OK)
	{
		*out_error = curl_easy_strerror(res);
		curl_easy_cleanup(curl);
		return false;
	}

	long http_code = 0;
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
	curl_easy_cleanup(curl);

	struct json_object *parsed = json_tokener_parse(response_data.c_str());
	if(!parsed)
	{
		*out_error = "Failed to parse token response";
		return false;
	}

	struct json_object *access_token_obj = nullptr;
	json_object_object_get_ex(parsed, "access_token", &access_token_obj);

	if(!access_token_obj)
	{
		struct json_object *error_obj = nullptr;
		if(json_object_object_get_ex(parsed, "error_description", &error_obj))
			*out_error = json_object_get_string(error_obj);
		else if(json_object_object_get_ex(parsed, "error", &error_obj))
			*out_error = json_object_get_string(error_obj);
		else
			*out_error = "No access_token in response (HTTP " + std::to_string(http_code) + ")";
		json_object_put(parsed);
		return false;
	}

	struct json_object *id_token_obj = nullptr;
	struct json_object *expires_in_obj = nullptr;
	json_object_object_get_ex(parsed, "id_token", &id_token_obj);
	json_object_object_get_ex(parsed, "expires_in", &expires_in_obj);

	out_tokens->access_token = json_object_get_string(access_token_obj);
	out_tokens->id_token = id_token_obj ? json_object_get_string(id_token_obj) : "";
	out_tokens->expires_in = expires_in_obj ? json_object_get_int(expires_in_obj) : 0;

	json_object_put(parsed);
	CHIAKI_LOGI(log, "CloudAuth: Successfully obtained access token (expires in %d seconds)", out_tokens->expires_in);
	return true;
}
