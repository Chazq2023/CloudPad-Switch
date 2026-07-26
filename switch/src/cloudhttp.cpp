// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include "cloudhttp.h"

#include <cctype>

#include <curl/curl.h>
#include <json-c/json.h>

namespace
{
	const char *kBrowserUserAgent = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36";

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
}

namespace cloudhttp
{
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
		// Newer TLS stacks cause connection instability against Sony's OAuth
		// endpoints; pin to the classic curve set (matches the upstream
		// nsurlsession_oauth_fallback.cpp workaround). Ignored by backends that
		// don't support it.
		curl_easy_setopt(curl, CURLOPT_SSL_EC_CURVES, "X25519:P-256:P-384");
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
			if(pos == 0 || url[pos - 1] == '?' || url[pos - 1] == '&' || url[pos - 1] == '#')
			{
				size_t value_start = pos + needle.length();
				size_t value_end = url.find_first_of("&#", value_start);
				return url.substr(value_start, value_end == std::string::npos ? std::string::npos : value_end - value_start);
			}
			search_from = pos + needle.length();
		}
	}

	std::string ExtractCookieValue(const std::string &headers, const std::string &cookie_name)
	{
		std::string needle = cookie_name + "=";
		size_t pos = headers.find(needle);
		if(pos == std::string::npos)
			return "";
		size_t value_start = pos + needle.length();
		size_t value_end = headers.find_first_of(";\r\n", value_start);
		return headers.substr(value_start, value_end == std::string::npos ? std::string::npos : value_end - value_start);
	}

	std::string ExtractHeaderValue(const std::string &headers, const std::string &header_name)
	{
		size_t pos = 0;
		while(pos < headers.size())
		{
			size_t line_end = headers.find("\r\n", pos);
			if(line_end == std::string::npos)
				line_end = headers.size();
			std::string line = headers.substr(pos, line_end - pos);

			size_t colon = line.find(':');
			if(colon != std::string::npos && colon == header_name.size())
			{
				bool match = true;
				for(size_t i = 0; i < header_name.size(); i++)
				{
					if(std::tolower((unsigned char)line[i]) != std::tolower((unsigned char)header_name[i]))
					{
						match = false;
						break;
					}
				}
				if(match)
				{
					size_t value_start = colon + 1;
					while(value_start < line.size() && line[value_start] == ' ')
						value_start++;
					return line.substr(value_start);
				}
			}

			if(line_end >= headers.size())
				break;
			pos = line_end + 2;
		}
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
}
