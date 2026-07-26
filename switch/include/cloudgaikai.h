// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#ifndef CHIAKI_CLOUDGAIKAI_H
#define CHIAKI_CLOUDGAIKAI_H

#include <cstdint>
#include <functional>
#include <string>

#include <chiaki/log.h>

struct json_object;

// Ports the Gaikai streaming-slot allocation flow (steps 0, 7-13) of the
// upstream PSGaikaiStreaming (gui/src/cloudstreaming/psgaikaistreaming.cpp).
// On success, produces exactly what ChiakiSession's cloud_* connect-info
// fields need (lib/include/chiaki/session.h) to start streaming.
//
// Simplified vs. upstream: step 11's real multi-datacenter RTT ping (which
// reuses chiaki_senkusha_run over a raw pre-session handshake) is not
// implemented here - that protocol hasn't been verified against a live
// server from this port. Instead this always takes the same "manually
// selected datacenter" bypass path upstream itself already supports,
// picking the first datacenter Sony's API offers and using its documented
// dummy ping values (RTT=20ms, mtu_in=1454, mtu_out=1254). This is a known
// follow-up: a real ping would pick a lower-latency datacenter when more
// than one is available.
class CloudGaikai
{
	public:
		struct Result
		{
			bool success = false;
			std::string error;
			std::string server_ip;
			int server_port = 0;
			std::string handshake_key;
			std::string launch_spec;
			std::string session_id;
			uint8_t psn_wrapper_type = 0x01;
			uint32_t mtu_in = 0;
			uint32_t mtu_out = 0;
			uint64_t rtt_us = 0;
		};

		// service_type: "psnow" or "pscloud". platform: "ps3", "ps4", or "ps5".
		// resolution_height: 720/1080/1440/2160, forwarded to Sony's encoder
		// (independent of the resolution chiaki's own client-side decode path
		// is configured for - the two should be kept in sync by the caller).
		CloudGaikai(ChiakiLog *log, std::string duid, std::string service_type, std::string platform,
			std::string npsso, std::string entitlement_id, int resolution_height);
		~CloudGaikai();

		// progress_cb is invoked with human-readable status strings as
		// allocation proceeds (queueing, retrying, etc.) - blocking call,
		// may take several minutes if the server queues the request.
		Result Run(std::function<void(const std::string &)> progress_cb);

	private:
		ChiakiLog *log;
		std::string duid;
		std::string service_type;
		std::string platform;
		std::string npsso;
		std::string entitlement_id;
		int resolution_height;

		std::string virt_type;
		std::string account_base_url;
		std::string redirect_uri;
		std::string user_agent;
		std::string oauth_api_path;

		std::string gk_client_id;
		std::string ps3_gk_client_id;
		std::string stream_server_client_id;
		std::string gaikai_session_id;
		std::string config_key;
		std::string gk_cloud_auth_code;
		std::string ps3_auth_code;
		std::string stream_server_auth_code;

		std::string selected_datacenter;
		std::string selected_public_ip;
		int selected_port = 0;
		int selected_rtt_ms = 20;
		int selected_mtu_in = 1454;
		int selected_mtu_out = 1254;

		json_object *request_game_spec = nullptr;

		void BuildRequestGameSpec();
		bool Step0_GetClientIds(std::string *out_error);
		bool Step7_GetConfig(std::string *out_error);
		bool Step8_StartSession(std::string *out_error);
		bool Step8a_GetGkAuthCode(std::string *out_error);
		bool Step8b_GetServerAuthCode(std::string *out_error);
		bool Step9_AuthorizeSession(std::string *out_error);
		bool Step10_LockSession(std::function<void(const std::string &)> progress_cb, std::string *out_error);
		bool Step11_PickDatacenter(std::string *out_error);
		bool Step12_SelectDatacenter(std::string *out_error);
		bool Step13_AllocateSlot(Result *out_result, std::function<void(const std::string &)> progress_cb, std::string *out_error);

		std::string PerformOAuthAuthorize(const std::string &url, std::string *out_error);
};

#endif // CHIAKI_CLOUDGAIKAI_H
