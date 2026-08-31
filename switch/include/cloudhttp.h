// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#ifndef CHIAKI_CLOUDHTTP_H
#define CHIAKI_CLOUDHTTP_H

#include <string>
#include <vector>

struct json_object;

// Shared HTTP/JSON helpers for the cloud login/catalog/session files
// (cloudauth, cloudcatalog, cloudkamaji, cloudgaikai) - they all talk to the
// same family of Sony REST endpoints using the same manual-cookie,
// manual-redirect style.
namespace cloudhttp
{
	struct HttpResponse
	{
		long status = 0;
		std::string body;
		std::string headers;
		std::string redirect_url;
	};

	// Blocking HTTP call, matches the DiscoveryManager::makeRequest precedent
	// already used on this Switch target (peer verification disabled - no CA
	// bundle wired up on-device yet). Pins the TLS EC curve list to
	// X25519:P-256:P-384 for ca.account.sony.com/oauth-authorize style calls,
	// matching a documented workaround for connection instability against
	// that endpoint with newer TLS stacks.
	bool HttpRequest(const std::string &url, const std::string &method,
		const std::vector<std::string> &headers, const std::string &body,
		HttpResponse *out, std::string *out_error);

	std::string UrlEncode(const std::string &value);
	std::string ExtractQueryParam(const std::string &url, const std::string &param);
	std::string ExtractCookieValue(const std::string &headers, const std::string &cookie_name);
	// Reads a plain "Name: value" response header (case-insensitive name match)
	// out of a raw header blob - distinct from ExtractCookieValue, which looks
	// for "name=value" inside a Set-Cookie line, not a header of its own.
	std::string ExtractHeaderValue(const std::string &headers, const std::string &header_name);
	std::string JsonGetString(json_object *obj, const char *key);
}

#endif // CHIAKI_CLOUDHTTP_H
