// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#ifndef CHIAKI_CLOUDAUTH_H
#define CHIAKI_CLOUDAUTH_H

#include <string>

#include <chiaki/log.h>

// Exchanges a PSN "npsso" session cookie value (obtained by the user from a
// browser on another device, since the Switch has no browser of its own) for
// a cloud gaming OAuth token pair. Mirrors the upstream CloudPad-Android
// PSCloudAuth flow (gui/src/cloudstreaming/pscloudauth.cpp).
class CloudAuth
{
	public:
		struct Tokens
		{
			std::string access_token;
			std::string id_token;
			int expires_in = 0;
		};

		explicit CloudAuth(ChiakiLog *log);

		// Pulls the raw npsso value out of either a bare token or the full
		// {"npsso":"..."} JSON some browsers show at the ssocookie endpoint, and
		// strips whitespace/surrounding quotes left over from copy-paste.
		static std::string ExtractNPSSO(const std::string &pasted);

		// Blocking network call (matches the existing DiscoveryManager::makeRequest
		// precedent already used on this Switch target) - callers should block
		// input / show a busy state around this.
		bool ExchangeNPSSO(const std::string &npsso, const std::string &duid,
			Tokens *out_tokens, std::string *out_error);

	private:
		ChiakiLog *log;
};

#endif // CHIAKI_CLOUDAUTH_H
