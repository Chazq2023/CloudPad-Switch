// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#ifndef CHIAKI_CLOUDKAMAJI_H
#define CHIAKI_CLOUDKAMAJI_H

#include <string>

#include <chiaki/log.h>

// Ports the Kamaji authentication half (steps 0.5b-6) of the upstream
// PSKamajiSession (gui/src/cloudstreaming/pskamajisession.cpp). Turns a
// PSNOW/PSCLOUD product id into a streaming entitlement id and an
// authenticated Kamaji session, blocking, with the same step numbering as
// the source so it stays auditable against it.
//
// Deliberately not ported: the entitlement auto-purchase/checkout flow
// (upstream steps 0.5e.3/0.5e.4, "CheckoutPreview"/"CheckoutBuynow"). That
// path makes a real commerce API call against the user's account; porting it
// without a way to verify it against a live account is a real-money/account
// risk, not just a correctness risk, so a missing entitlement is surfaced as
// a plain error here instead of an automatic purchase attempt. Also skipped:
// the one-time account-privacy-attributes check (upstream step 0.5e.1a),
// which is best-effort validation most already-active PSN accounts pass -
// if it would have failed, the real entitlement/session calls below fail
// with a server error instead of a friendlier upfront message.
class CloudKamaji
{
	public:
		struct Result
		{
			bool success = false;
			std::string error;
			std::string entitlement_id;
			std::string platform;   // "ps3" or "ps4", detected from the product lookup
			std::string account_id;
			std::string online_id;
		};

		CloudKamaji(ChiakiLog *log, std::string duid, std::string product_id,
			std::string npsso, std::string redirect_uri, std::string user_agent);

		Result Run();

	private:
		ChiakiLog *log;
		std::string duid;
		std::string product_id;
		std::string npsso;
		std::string redirect_uri;
		std::string user_agent;

		std::string platform = "ps4";
		std::string scopes;
		std::string jsessionid;
		std::string entitlement_id;
		std::string streaming_sku;
		std::string commerce_oauth_token;

		bool Step0_5b_GetAnonAuthCode(std::string *out_code, std::string *out_error);
		bool Step0_5c_CreateAnonSession(const std::string &code, std::string *out_error);
		bool Step0_5d_ConvertProductId(std::string *out_error);
		bool Step0_5e1_GetCommerceOAuthToken(std::string *out_error);
		bool Step0_5e2_CheckEntitlementExists(bool *out_has_entitlement, std::string *out_error);
		bool Step5_GetAuthCode(std::string *out_code, std::string *out_error);
		bool Step6_CreateAuthSession(const std::string &code, std::string *out_error);
};

#endif // CHIAKI_CLOUDKAMAJI_H
