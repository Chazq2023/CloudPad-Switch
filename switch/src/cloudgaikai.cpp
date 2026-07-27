// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include "cloudgaikai.h"
#include "cloudhttp.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <thread>

#include <netdb.h>
#include <sys/socket.h>

#include <json-c/json.h>

#include <chiaki/senkusha.h>
#include <chiaki/session.h>

using cloudhttp::HttpRequest;
using cloudhttp::HttpResponse;
using cloudhttp::JsonGetString;
using cloudhttp::UrlEncode;
using cloudhttp::ExtractQueryParam;

namespace
{
	const char *kConfigBase = "https://config.cc.prod.gaikai.com/v1";
	const char *kGaikaiBase = "https://cc.prod.gaikai.com/v1";
	const char *kPscloudRedirectUri = "gaikai://local";
	const char *kPscloudUserAgent = "PlayStation Portal/6.0.0-rel.444+6a9cea6f5";
	const char *kPsnowRedirectUri = "https://psnow.playstation.com/app/2.2.0/133/5cdcc037d/grc-response.html";
	const char *kPsnowUserAgent =
		"Mozilla/5.0 (Windows NT 10.0; WOW64) AppleWebKit/537.36 (KHTML, like Gecko) "
		"playstation-now/0.0.0 Chrome/83.0.4103.104 Electron/9.0.4 Safari/537.36 gkApollo";

	const int kMaxLockSessionRetries = 12;
	const int kDefaultAllocationWaitSeconds = 300;
	const int kMaxAllocationWaitSeconds = 900;

	std::string TimezoneString()
	{
		// Portable UTC offset without relying on the BSD tm_gmtoff extension
		// (not available in this toolchain's libc): reinterpret the UTC
		// calendar fields as if they were local time and see how far that
		// drifts from "now".
		std::time_t now = std::time(nullptr);
		struct tm utc_tm = *std::gmtime(&now);
		struct tm local_tm = *std::localtime(&now);
		utc_tm.tm_isdst = local_tm.tm_isdst;
		std::time_t utc_as_local = std::mktime(&utc_tm);
		long offset_seconds = (long)(now - utc_as_local);
		long offset_minutes = std::abs(offset_seconds / 60) % 60;
		long offset_hours = std::abs(offset_seconds) / 3600;
		char buf[16];
		snprintf(buf, sizeof(buf), "UTC%c%02ld:%02ld", offset_seconds >= 0 ? '+' : '-', offset_hours, offset_minutes);
		return buf;
	}

	std::string ParseGaikaiEventName(const std::string &headers)
	{
		size_t pos = headers.find("x-gaikai-event:");
		if(pos == std::string::npos)
			pos = headers.find("X-Gaikai-Event:");
		if(pos == std::string::npos)
			return "";
		size_t value_start = headers.find(':', pos) + 1;
		size_t value_end = headers.find_first_of("\r\n", value_start);
		std::string value = headers.substr(value_start, value_end == std::string::npos ? std::string::npos : value_end - value_start);
		json_object *event = json_tokener_parse(value.c_str());
		if(!event)
			return "";
		std::string name = JsonGetString(event, "name");
		json_object_put(event);
		return name;
	}

	// Real RTT/MTU measurement against one candidate datacenter, via the
	// same chiaki_senkusha_run echo/ping handshake CloudPad-Android uses
	// (android/app/src/main/cpp/chiaki-jni.c,
	// Java_com_metallic_chiaki_cloudplay_ping_DatacenterPingNative_performPing)
	// to actually pick the best of the datacenters Sony offers, instead of
	// blindly taking whichever one the API lists first. Builds a minimal,
	// throwaway ChiakiSession just for the ping - senkusha only needs a
	// handful of its fields (host, port, service type, cloud session key).
	// Returns false on any failure (unresolvable host, senkusha error, no
	// timing), in which case *out_rtt_us is left untouched.
	bool PingDatacenter(const std::string &public_ip, int port, const std::string &session_key,
		const std::string &service_type_str, ChiakiLog *log,
		uint64_t *out_rtt_us, uint32_t *out_mtu_in, uint32_t *out_mtu_out)
	{
		struct addrinfo hints = {};
		hints.ai_family = AF_INET;
		hints.ai_socktype = SOCK_DGRAM;
		hints.ai_protocol = IPPROTO_UDP;

		char port_str[16];
		snprintf(port_str, sizeof(port_str), "%d", port);

		struct addrinfo *addrinfo_result = nullptr;
		int err = getaddrinfo(public_ip.c_str(), port_str, &hints, &addrinfo_result);
		if(err != 0 || !addrinfo_result)
		{
			CHIAKI_LOGW(log, "CloudGaikai: ping to %s:%d failed to resolve: %s", public_ip.c_str(), port, gai_strerror(err));
			return false;
		}

		ChiakiSession *session = (ChiakiSession *)calloc(1, sizeof(ChiakiSession));
		if(!session)
		{
			freeaddrinfo(addrinfo_result);
			return false;
		}

		session->log = log;
		session->connect_info.host_addrinfo_selected = addrinfo_result;
		session->connect_info.enable_dualsense = false;
		session->target = CHIAKI_TARGET_PS5_1;
		session->cloud_port = (uint16_t)port;

		if(service_type_str == "pscloud")
		{
			session->cloud_psn_wrapper_type = 0;
			session->service_type = CHIAKI_SERVICE_TYPE_PSCLOUD;
		}
		else
		{
			session->cloud_psn_wrapper_type = 0x01;
			session->service_type = CHIAKI_SERVICE_TYPE_PSNOW;
		}

		ChiakiSenkusha senkusha;
		ChiakiErrorCode chiaki_err = chiaki_senkusha_init(&senkusha, session);
		if(chiaki_err != CHIAKI_ERR_SUCCESS)
		{
			CHIAKI_LOGW(log, "CloudGaikai: ping to %s:%d failed to init senkusha: %d", public_ip.c_str(), port, (int)chiaki_err);
			freeaddrinfo(addrinfo_result);
			free(session);
			return false;
		}

		senkusha.protocol_version = 9;
		// RTT only - the MTU sub-test isn't needed since we already use a
		// fixed, conservative MTU (see the mtu_in/mtu_out constants below),
		// and skipping it exercises less of the connect/teardown path per
		// datacenter pinged.
		senkusha.skip_mtu_test = true;
		senkusha.cloud_launch_spec = (char *)malloc(session_key.size() + 1);
		if(!senkusha.cloud_launch_spec)
		{
			chiaki_senkusha_fini(&senkusha);
			freeaddrinfo(addrinfo_result);
			free(session);
			return false;
		}
		memcpy(senkusha.cloud_launch_spec, session_key.c_str(), session_key.size() + 1);

		uint32_t mtu_in = 0;
		uint32_t mtu_out = 0;
		uint64_t rtt_us = 0;
		chiaki_err = chiaki_senkusha_run(&senkusha, &mtu_in, &mtu_out, &rtt_us, nullptr);

		free(senkusha.cloud_launch_spec);
		senkusha.cloud_launch_spec = nullptr;
		chiaki_senkusha_fini(&senkusha);
		freeaddrinfo(addrinfo_result);
		free(session);

		if(chiaki_err != CHIAKI_ERR_SUCCESS)
		{
			CHIAKI_LOGW(log, "CloudGaikai: ping to %s:%d failed: %d", public_ip.c_str(), port, (int)chiaki_err);
			return false;
		}

		*out_rtt_us = rtt_us;
		// mtu_in/mtu_out are left unset by senkusha_run when skip_mtu_test is
		// true - use the same conservative fixed value the fallback path uses.
		*out_mtu_in = 1200;
		*out_mtu_out = 1200;
		return true;
	}
}

CloudGaikai::CloudGaikai(ChiakiLog *log, std::string duid, std::string service_type, std::string platform,
	std::string npsso, std::string entitlement_id, int resolution_height)
	: log(log), duid(duid), service_type(service_type), platform(platform),
	  npsso(npsso), entitlement_id(entitlement_id), resolution_height(resolution_height)
{
	if(platform == "ps3")
		virt_type = "konan";
	else if(platform == "ps4")
		virt_type = "kratos";
	else
		virt_type = "cronos";

	account_base_url = "https://ca.account.sony.com";
	if(service_type == "pscloud")
	{
		redirect_uri = kPscloudRedirectUri;
		user_agent = kPscloudUserAgent;
		oauth_api_path = "/api/authz/v3";
	}
	else
	{
		redirect_uri = kPsnowRedirectUri;
		user_agent = kPsnowUserAgent;
		oauth_api_path = "/api/v1";
	}
}

CloudGaikai::~CloudGaikai()
{
	if(request_game_spec)
		json_object_put(request_game_spec);
}

void CloudGaikai::BuildRequestGameSpec()
{
	json_object *spec = json_object_new_object();

	json_object_object_add(spec, "entitlementId", json_object_new_string(entitlement_id.c_str()));
	json_object_object_add(spec, "npEnv", json_object_new_string("np"));
	json_object_object_add(spec, "language", json_object_new_string("en-US"));
	json_object_object_add(spec, "cloudEndpoint", json_object_new_string("https://cc.prod.gaikai.com"));
	json_object_object_add(spec, "redirectUri", json_object_new_string(redirect_uri.c_str()));

	// Settings only ever offers 720p or 1080p (see gui.cpp's BuildAccountMenu) -
	// anything else defaults to 1080p rather than silently mismatching what
	// chiaki_connect_video_profile_preset set up for the client-side decoder.
	std::string resolution_setting;
	int client_width, client_height;
	switch(resolution_height)
	{
		case 720: resolution_setting = "720"; client_width = 1280; client_height = 720; break;
		default: resolution_setting = "1080"; client_width = 1920; client_height = 1080; break;
	}
	json_object_object_add(spec, "resolutionSetting", json_object_new_string(resolution_setting.c_str()));
	json_object_object_add(spec, "clientWidth", json_object_new_int(client_width));
	json_object_object_add(spec, "clientHeight", json_object_new_int(client_height));
	json_object_object_add(spec, "adaptiveStreamMode", json_object_new_string("resize"));
	json_object_object_add(spec, "useClientBwLadder", json_object_new_boolean(1));

	json_object_object_add(spec, "audioUploadEnabled", json_object_new_boolean(1));
	json_object_object_add(spec, "audioUploadNumChannels", json_object_new_int(1));
	json_object_object_add(spec, "audioUploadSamplingFrequency", json_object_new_int(48000));

	json_object_object_add(spec, "acceptButton", json_object_new_string("X"));
	json_object_object_add(spec, "encryptionSupported", json_object_new_boolean(1));
	json_object_object_add(spec, "summerTime", json_object_new_int(0));
	json_object_object_add(spec, "timeZone", json_object_new_string(TimezoneString().c_str()));
	json_object_object_add(spec, "httpUserAgent", json_object_new_string(user_agent.c_str()));
	json_object_object_add(spec, "gkCloudAuthCode", json_object_new_string(gk_cloud_auth_code.c_str()));

	json_object_object_add(spec, "accessibilityMarqueeSpeed", json_object_new_int(0));
	json_object_object_add(spec, "accessibilityLargeText", json_object_new_int(0));
	json_object_object_add(spec, "accessibilityBoldText", json_object_new_int(0));
	json_object_object_add(spec, "accessibilityContrast", json_object_new_int(0));
	json_object_object_add(spec, "accessibilityTtsEnable", json_object_new_int(0));
	json_object_object_add(spec, "accessibilityTtsSpeed", json_object_new_int(0));
	json_object_object_add(spec, "accessibilityTtsVolume", json_object_new_int(0));

	json_object_object_add(spec, "partyCapability", json_object_new_boolean(0));
	json_object_object_add(spec, "homesharing", json_object_new_boolean(0));
	json_object_object_add(spec, "isFirstBoot", json_object_new_boolean(0));
	json_object_object_add(spec, "isPlusMember", json_object_new_boolean(1));
	json_object_object_add(spec, "parentalLevel", json_object_new_int(0));
	json_object_object_add(spec, "yuvCoefficient", json_object_new_string(""));

	json_object *capabilities = json_object_new_array();
	json_object_array_add(capabilities, json_object_new_string("cloudDrivenSenkushaTest"));

	if(service_type == "pscloud")
	{
		json_object_object_add(spec, "videoEncoderProfile", json_object_new_string("hw5.0"));

		json_object *controllers = json_object_new_array();
		json_object_array_add(controllers, json_object_new_string("ds4"));
		json_object_array_add(controllers, json_object_new_string("ds5"));
		json_object_array_add(controllers, json_object_new_string("xinput"));
		json_object_object_add(spec, "connectedControllers", controllers);
		json_object *input = json_object_new_object();
		json_object_object_add(input, "controllers", json_object_get(controllers));
		json_object_object_add(spec, "input", input);

		json_object_object_add(spec, "model", json_object_new_string("portal"));
		json_object_object_add(spec, "platform", json_object_new_string("qlite"));
		json_object_object_add(spec, "gaikaiPlayer", json_object_new_string("16.4.0"));
		json_object_object_add(spec, "protocolVersion", json_object_new_int(12));
		json_object_object_add(spec, "ps3AuthCode", json_object_new_string(""));
		json_object_object_add(spec, "streamServerAuthCode", json_object_new_string(stream_server_auth_code.c_str()));

		json_object_array_add(capabilities, json_object_new_string("cronos"));

		json_object *video_stream_settings = json_object_new_object();
		json_object_object_add(video_stream_settings, "clientHeight", json_object_new_int(client_height));
		json_object_object_add(video_stream_settings, "supportedMaxResolution", json_object_new_int(client_height));
		json_object *video_profiles = json_object_new_array();
		json_object_array_add(video_profiles, json_object_new_string("hevc_hw4"));
		json_object_object_add(video_stream_settings, "supportedVideoEncoderProfiles", video_profiles);
		json_object_object_add(video_stream_settings, "supportedDynamicRange", json_object_new_string("sdr"));
		json_object_object_add(video_stream_settings, "preferredMaxResolution", json_object_new_int(client_height));
		json_object_object_add(video_stream_settings, "preferredDynamicRange", json_object_new_string("sdr"));
		json_object_object_add(video_stream_settings, "hqMode", json_object_new_int(1));
		json_object_object_add(spec, "videoStreamSettings", video_stream_settings);

		json_object_object_add(spec, "audioChannels", json_object_new_string("2"));
		json_object_object_add(spec, "audioEncoderProfile", json_object_new_string("default"));
		json_object *audio_stream_settings = json_object_new_object();
		json_object_object_add(audio_stream_settings, "audioEncoderProfile", json_object_new_string("default"));
		json_object_object_add(audio_stream_settings, "maxAudioChannels", json_object_new_string("2"));
		json_object_object_add(audio_stream_settings, "preferredNumberAudioChannels", json_object_new_string("2"));
		json_object_object_add(spec, "audioStreamSettings", audio_stream_settings);
	}
	else
	{
		json_object_object_add(spec, "audioChannels", json_object_new_string("2.1"));
		json_object_object_add(spec, "audioEncoderProfile", json_object_new_string("default"));
		json_object_object_add(spec, "videoEncoderProfile", json_object_new_string("hw4.1"));

		json_object *controllers = json_object_new_array();
		json_object_array_add(controllers, json_object_new_string("xinput"));
		json_object_object_add(spec, "connectedControllers", controllers);
		json_object *input = json_object_new_object();
		json_object_object_add(input, "controllers", json_object_get(controllers));
		json_object_object_add(spec, "input", input);

		json_object_object_add(spec, "model", json_object_new_string("WINDOWS"));
		json_object_object_add(spec, "platform", json_object_new_string("PC"));
		json_object_object_add(spec, "gaikaiPlayer", json_object_new_string("12.5.0"));
		json_object_object_add(spec, "protocolVersion", json_object_new_int(9));
		json_object_object_add(spec, "ps3AuthCode", json_object_new_string(ps3_auth_code.c_str()));
		json_object_object_add(spec, "streamServerAuthCode", json_object_new_string(ps3_auth_code.c_str()));

		json_object_array_add(capabilities, json_object_new_string("kratos"));
	}

	json_object_object_add(spec, "capabilities", capabilities);

	if(request_game_spec)
		json_object_put(request_game_spec);
	request_game_spec = spec;
}

std::string CloudGaikai::PerformOAuthAuthorize(const std::string &url, std::string *out_error)
{
	std::vector<std::string> headers = {
		std::string("User-Agent: ") + user_agent,
		"Accept: */*",
		"Cookie: npsso=" + npsso,
	};

	HttpResponse resp;
	if(!HttpRequest(url, "GET", headers, "", &resp, out_error))
		return "";

	if(resp.status != 302 || resp.redirect_url.empty())
	{
		*out_error = "OAuth authorization failed: expected redirect, got HTTP " + std::to_string(resp.status);
		return "";
	}

	std::string code = ExtractQueryParam(resp.redirect_url, "code");
	if(code.empty())
	{
		*out_error = "OAuth authorization failed: no authorization code in redirect";
		return "";
	}
	return code;
}

bool CloudGaikai::Step0_GetClientIds(std::string *out_error)
{
	std::string url = std::string(kGaikaiBase) + "/client_ids?virtType=" + virt_type;
	std::vector<std::string> headers = { std::string("User-Agent: ") + user_agent, "Accept: */*" };

	HttpResponse resp;
	if(!HttpRequest(url, "GET", headers, "", &resp, out_error))
		return false;

	json_object *root = json_tokener_parse(resp.body.c_str());
	if(!root)
	{
		*out_error = "Invalid JSON from client_ids";
		return false;
	}
	gk_client_id = JsonGetString(root, "gkClientId");
	ps3_gk_client_id = JsonGetString(root, "ps3GkClientId");
	stream_server_client_id = JsonGetString(root, "streamServerClientId");
	json_object_put(root);

	if(gk_client_id.empty())
	{
		*out_error = "No gkClientId in client_ids response";
		return false;
	}
	return true;
}

bool CloudGaikai::Step7_GetConfig(std::string *out_error)
{
	std::string url = std::string(kConfigBase) + "/config";
	std::vector<std::string> headers = {
		"Content-Type: application/json",
		"Accept: */*",
		std::string("User-Agent: ") + user_agent,
	};

	json_object *body = json_object_new_object();
	if(service_type == "pscloud")
	{
		json_object_object_add(body, "product", json_object_new_string("qlite"));
		json_object_object_add(body, "platform", json_object_new_string("qlite"));
	}
	else
	{
		json_object_object_add(body, "product", json_object_new_string("psnow"));
		json_object_object_add(body, "platform", json_object_new_string("PC"));
	}
	json_object_object_add(body, "sessionId", json_object_new_string(""));
	std::string body_str = json_object_to_json_string(body);
	json_object_put(body);

	HttpResponse resp;
	if(!HttpRequest(url, "POST", headers, body_str, &resp, out_error))
		return false;

	json_object *root = json_tokener_parse(resp.body.c_str());
	if(!root)
	{
		*out_error = "Invalid JSON from Gaikai config";
		return false;
	}
	config_key = JsonGetString(root, "configKey");
	json_object_put(root);
	return true;
}

bool CloudGaikai::Step8_StartSession(std::string *out_error)
{
	std::string url = std::string(kGaikaiBase) + "/sessions/start?npEnv=np";
	std::vector<std::string> headers = {
		"Content-Type: application/json",
		"Accept: */*",
		std::string("User-Agent: ") + user_agent,
		"X-Gaikai-Session: " + config_key,
	};

	// Initial session start has no auth codes yet.
	json_object *initial_spec = json_tokener_parse(json_object_to_json_string(request_game_spec));
	json_object_object_add(initial_spec, "gkCloudAuthCode", json_object_new_string(""));
	json_object_object_add(initial_spec, "ps3AuthCode", json_object_new_string(""));
	json_object_object_add(initial_spec, "streamServerAuthCode", json_object_new_string(""));

	json_object *body = json_object_new_object();
	json_object_object_add(body, "requestGameSpecification", initial_spec);
	std::string body_str = json_object_to_json_string(body);
	json_object_put(body); // also frees initial_spec

	HttpResponse resp;
	if(!HttpRequest(url, "POST", headers, body_str, &resp, out_error))
		return false;

	std::string new_key = cloudhttp::ExtractHeaderValue(resp.headers, "X-Gaikai-Session");
	if(!new_key.empty())
		config_key = new_key;

	json_object *root = json_tokener_parse(resp.body.c_str());
	if(!root)
	{
		*out_error = "Invalid JSON from Gaikai session start";
		return false;
	}
	gaikai_session_id = JsonGetString(root, "sessionId");
	json_object_put(root);

	if(gaikai_session_id.empty())
	{
		*out_error = "No sessionId from Gaikai session start";
		return false;
	}
	return true;
}

bool CloudGaikai::Step8a_GetGkAuthCode(std::string *out_error)
{
	std::string url = account_base_url + oauth_api_path + "/oauth/authorize"
		"?response_type=code"
		"&client_id=" + gk_client_id +
		"&redirect_uri=" + UrlEncode(redirect_uri) +
		"&service_entity=" + UrlEncode("urn:service-entity:psn") +
		"&prompt=none"
		"&duid=" + UrlEncode(duid);

	if(service_type == "pscloud")
	{
		url += "&smcid=qlite&applicationId=qlite&mid=qlite"
			"&scope=" + UrlEncode("id_token:psn.basic_claims kamaji:s2s.subscriptionsPremium.get id_token:duid id_token:online_id openid psn:s2s");
	}
	else
	{
		url += "&smcid=" + UrlEncode("pc:psnow") + "&applicationId=psnow&mid=PSNOW"
			"&scope=" + UrlEncode("kamaji:commerce_native versa:user_update_entitlements_first_play kamaji:lists") +
			"&renderMode=mobilePortrait&hidePageElements=forgotPasswordLink&displayFooter=none"
			"&disableLinks=qriocityLink&layout_type=popup&service_logo=ps&tp_psn=true&noEVBlock=true";
	}

	gk_cloud_auth_code = PerformOAuthAuthorize(url, out_error);
	return !gk_cloud_auth_code.empty();
}

bool CloudGaikai::Step8b_GetServerAuthCode(std::string *out_error)
{
	std::string url = account_base_url + oauth_api_path + "/oauth/authorize"
		"?response_type=code"
		"&redirect_uri=" + UrlEncode(redirect_uri) +
		"&service_entity=" + UrlEncode("urn:service-entity:psn") +
		"&prompt=none";

	if(service_type == "pscloud")
	{
		url += "&client_id=" + stream_server_client_id +
			"&smcid=qlite&applicationId=qlite&mid=qlite"
			"&scope=" + UrlEncode("id_token:duid id_token:online_id openid oauth:create_authn_ticket_for_cloud_console_signin") +
			"&duid=" + UrlEncode(duid);
	}
	else
	{
		url += "&client_id=" + ps3_gk_client_id +
			"&smcid=" + UrlEncode("pc:psnow") + "&applicationId=psnow&mid=PSNOW"
			"&scope=" + UrlEncode(platform == "ps3" ? "kamaji:commerce_native" : "sso:none");
		if(platform != "ps3")
			url += "&duid=" + UrlEncode(duid);
		url += "&renderMode=mobilePortrait&hidePageElements=forgotPasswordLink&displayFooter=none"
			"&disableLinks=qriocityLink&layout_type=popup&service_logo=ps&tp_psn=true&noEVBlock=true";
	}

	std::string code = PerformOAuthAuthorize(url, out_error);
	if(code.empty())
		return false;

	if(service_type == "pscloud")
	{
		stream_server_auth_code = code;
		ps3_auth_code.clear();
	}
	else
	{
		ps3_auth_code = code;
		stream_server_auth_code = code;
	}

	json_object_object_add(request_game_spec, "gkCloudAuthCode", json_object_new_string(gk_cloud_auth_code.c_str()));
	json_object_object_add(request_game_spec, "ps3AuthCode", json_object_new_string(ps3_auth_code.c_str()));
	json_object_object_add(request_game_spec, "streamServerAuthCode", json_object_new_string(stream_server_auth_code.c_str()));
	return true;
}

bool CloudGaikai::Step9_AuthorizeSession(std::string *out_error)
{
	std::string url = std::string(kGaikaiBase) + "/sessions/" + gaikai_session_id + "/authorize";
	std::vector<std::string> headers = {
		"Content-Type: application/json",
		"Accept: */*",
		std::string("User-Agent: ") + user_agent,
		"X-Gaikai-SessionId: " + gaikai_session_id,
		"X-Gaikai-Session: " + config_key,
	};

	json_object *body = json_object_new_object();
	json_object_object_add(body, "requestGameSpecification", json_object_get(request_game_spec));
	std::string body_str = json_object_to_json_string(body);
	json_object_put(body);

	HttpResponse resp;
	if(!HttpRequest(url, "POST", headers, body_str, &resp, out_error))
		return false;

	if(resp.status != 200)
	{
		*out_error = "Authorize session failed (HTTP " + std::to_string(resp.status) + ")";
		if(!resp.body.empty())
			*out_error += ": " + resp.body;
		return false;
	}

	std::string new_key = cloudhttp::ExtractHeaderValue(resp.headers, "X-Gaikai-Session");
	if(!new_key.empty())
		config_key = new_key;
	return true;
}

bool CloudGaikai::Step10_LockSession(std::function<void(const std::string &)> progress_cb, std::string *out_error)
{
	for(int attempt = 0; attempt <= kMaxLockSessionRetries; attempt++)
	{
		std::string url = std::string(kGaikaiBase) + "/sessions/" + gaikai_session_id + "/lock?forceLogout=true";
		std::vector<std::string> headers = {
			"Content-Type: application/json",
			"Accept: */*",
			std::string("User-Agent: ") + user_agent,
			"X-Gaikai-SessionId: " + gaikai_session_id,
			"X-Gaikai-Session: " + config_key,
		};

		json_object *body = json_object_new_object();
		json_object_object_add(body, "requestGameSpecification", json_object_get(request_game_spec));
		std::string body_str = json_object_to_json_string(body);
		json_object_put(body);

		HttpResponse resp;
		if(!HttpRequest(url, "POST", headers, body_str, &resp, out_error))
			return false;

		std::string new_key = cloudhttp::ExtractHeaderValue(resp.headers, "X-Gaikai-Session");
		if(!new_key.empty())
			config_key = new_key;

		json_object *root = json_tokener_parse(resp.body.c_str());
		bool lock_acquired = false;
		int poll_frequency = 10;
		if(root)
		{
			json_object *lock_obj = nullptr;
			if(json_object_object_get_ex(root, "lockAcquired", &lock_obj))
				lock_acquired = json_object_get_boolean(lock_obj);
			json_object *poll_obj = nullptr;
			if(json_object_object_get_ex(root, "pollFrequency", &poll_obj))
				poll_frequency = json_object_get_int(poll_obj);
			json_object_put(root);
		}

		if(lock_acquired)
			return true;

		std::string event_name = ParseGaikaiEventName(resp.headers);
		std::string message = event_name.empty()
			? ("Closing old session - attempt " + std::to_string(attempt + 1))
			: ("Closing old session (" + event_name + ") - attempt " + std::to_string(attempt + 1));
		progress_cb(message);

		CHIAKI_LOGI(log, "CloudGaikai: lock not acquired, retrying in %d seconds", poll_frequency);
		std::this_thread::sleep_for(std::chrono::seconds(poll_frequency));
	}

	*out_error = "Could not lock the streaming session after " + std::to_string(kMaxLockSessionRetries) + " attempts";
	return false;
}

bool CloudGaikai::Step11_PickDatacenter(std::string *out_error)
{
	std::string url = std::string(kGaikaiBase) + "/sessions/" + gaikai_session_id + "/datacenters";
	std::vector<std::string> headers = {
		"Content-Type: application/json",
		"Accept: */*",
		std::string("User-Agent: ") + user_agent,
		"X-Gaikai-SessionId: " + gaikai_session_id,
		"X-Gaikai-Session: " + config_key,
	};

	json_object *body = json_object_new_object();
	json_object_object_add(body, "requestGameSpecification", json_object_get(request_game_spec));
	std::string body_str = json_object_to_json_string(body);
	json_object_put(body);

	HttpResponse resp;
	if(!HttpRequest(url, "POST", headers, body_str, &resp, out_error))
		return false;

	std::string new_key = cloudhttp::ExtractHeaderValue(resp.headers, "X-Gaikai-Session");
	if(!new_key.empty())
		config_key = new_key;

	json_object *datacenters = json_tokener_parse(resp.body.c_str());
	if(!datacenters || !json_object_is_type(datacenters, json_type_array) || json_object_array_length(datacenters) == 0)
	{
		*out_error = "No datacenters available for this session";
		if(datacenters)
			json_object_put(datacenters);
		return false;
	}

	// Real RTT-based ping-and-pick-best (PingDatacenter, further up this
	// file) is implemented and reachable from here, but disabled for now:
	// it reliably reproduces a teardown crash in chiaki_takion_close after
	// completing a full, correct ping (RTT measured successfully every
	// time) that resisted extensive on-device bisection - see the
	// switch-build history around commits 974b7e4..942b055 for the full
	// investigation, including two genuine pre-existing thread-lifecycle
	// bugs that were found and fixed along the way but did not resolve it.
	// Falling back to the simple first-listed datacenter with a
	// conservative guessed MTU so streaming actually works while that's
	// unresolved. RTT data already captured for the first datacenter
	// before the crash was a consistently healthy 8-10ms, which doesn't
	// support "wrong datacenter" as the explanation for the original
	// motion-triggered packet loss this was meant to test anyway.
	int datacenter_count = json_object_array_length(datacenters);
	json_object *first = json_object_array_get_idx(datacenters, 0);
	selected_datacenter = JsonGetString(first, "dataCenter");
	selected_public_ip = JsonGetString(first, "publicIp");
	json_object *port_obj = nullptr;
	if(json_object_object_get_ex(first, "port", &port_obj))
		selected_port = json_object_get_int(port_obj);
	selected_rtt_ms = 20;
	selected_mtu_in = 1200;
	selected_mtu_out = 1200;

	CHIAKI_LOGI(log, "CloudGaikai: %d datacenters available, picked '%s' (%s:%d) without RTT ping",
		datacenter_count, selected_datacenter.c_str(), selected_public_ip.c_str(), selected_port);

	json_object_put(datacenters);

	if(selected_datacenter.empty() || selected_port <= 0)
	{
		*out_error = "Selected datacenter is missing required fields";
		return false;
	}
	return true;
}

bool CloudGaikai::Step12_SelectDatacenter(std::string *out_error)
{
	std::string url = std::string(kGaikaiBase) + "/sessions/" + gaikai_session_id + "/datacenters/select";
	std::vector<std::string> headers = {
		"Content-Type: application/json",
		"Accept: */*",
		std::string("User-Agent: ") + user_agent,
		"X-Gaikai-SessionId: " + gaikai_session_id,
		"X-Gaikai-Session: " + config_key,
	};

	json_object *ping_result = json_object_new_object();
	json_object_object_add(ping_result, "dataCenter", json_object_new_string(selected_datacenter.c_str()));
	json_object_object_add(ping_result, "rtt", json_object_new_int(selected_rtt_ms));
	json_object *rtts = json_object_new_array();
	json_object_array_add(rtts, json_object_new_int(selected_rtt_ms));
	json_object_object_add(ping_result, "rtts", rtts);
	json_object_object_add(ping_result, "mtu_in", json_object_new_int(selected_mtu_in));
	json_object_object_add(ping_result, "mtu_out", json_object_new_int(selected_mtu_out));
	json_object_object_add(ping_result, "port", json_object_new_int(selected_port));
	json_object_object_add(ping_result, "publicIp", json_object_new_string(selected_public_ip.c_str()));

	json_object *ping_results = json_object_new_array();
	json_object_array_add(ping_results, ping_result);

	json_object *body = json_object_new_object();
	json_object_object_add(body, "requestGameSpecification", json_object_get(request_game_spec));
	json_object_object_add(body, "pingResults", ping_results);
	std::string body_str = json_object_to_json_string(body);
	json_object_put(body);

	HttpResponse resp;
	if(!HttpRequest(url, "POST", headers, body_str, &resp, out_error))
		return false;

	std::string new_key = cloudhttp::ExtractHeaderValue(resp.headers, "X-Gaikai-Session");
	if(!new_key.empty())
		config_key = new_key;

	if(resp.status != 200)
	{
		*out_error = "Select datacenter failed (HTTP " + std::to_string(resp.status) + ")";
		if(!resp.body.empty())
			*out_error += ": " + resp.body;
		return false;
	}

	// Step 12's response may carry an updated port (nested under "network" on
	// some responses); fall back to the port from step 11 if it's absent or
	// empty, matching upstream's handling of this endpoint's inconsistent body.
	if(!resp.body.empty())
	{
		json_object *selected = json_tokener_parse(resp.body.c_str());
		if(selected)
		{
			json_object *port_obj = nullptr;
			int port_from_response = 0;
			if(json_object_object_get_ex(selected, "port", &port_obj))
				port_from_response = json_object_get_int(port_obj);
			if(port_from_response <= 0)
			{
				json_object *network = nullptr;
				if(json_object_object_get_ex(selected, "network", &network) && json_object_object_get_ex(network, "port", &port_obj))
					port_from_response = json_object_get_int(port_obj);
			}
			if(port_from_response > 0)
				selected_port = port_from_response;
			json_object_put(selected);
		}
	}

	return true;
}

bool CloudGaikai::Step13_AllocateSlot(Result *out_result, std::function<void(const std::string &)> progress_cb, std::string *out_error)
{
	std::chrono::steady_clock::time_point wait_start;
	bool waiting = false;
	int max_wait_seconds = kDefaultAllocationWaitSeconds;
	int retry_count = 0;

	while(true)
	{
		std::string url = std::string(kGaikaiBase) + "/sessions/" + gaikai_session_id + "/allocate";
		std::vector<std::string> headers = {
			"Content-Type: application/json",
			"Accept: */*",
			std::string("User-Agent: ") + user_agent,
			"X-Gaikai-SessionId: " + gaikai_session_id,
			"X-Gaikai-Session: " + config_key,
		};

		json_object *network = json_object_new_object();
		json_object_object_add(network, "bwKbpsSent", json_object_new_int(50000));
		json_object_object_add(network, "bwLoss", json_object_new_double(0.001));
		json_object_object_add(network, "mtu", json_object_new_int(selected_mtu_in));
		json_object_object_add(network, "rtt", json_object_new_int(selected_rtt_ms));
		json_object_object_add(network, "port", json_object_new_int(selected_port));
		json_object_object_add(network, "bwKbpsReceived", json_object_new_int(200000));
		json_object_object_add(network, "bwLossUpstream", json_object_new_int(0));
		json_object_object_add(network, "mtuUpstream", json_object_new_int(selected_mtu_out));

		json_object *body = json_object_new_object();
		json_object_object_add(body, "requestGameSpecification", json_object_get(request_game_spec));
		json_object_object_add(body, "dataCenter", json_object_new_string(selected_datacenter.c_str()));
		json_object_object_add(body, "network", network);
		json_object_object_add(body, "stateExecutionTime", json_object_new_double(5974.7632));
		json_object_object_add(body, "streamTestTime", json_object_new_double(11262.8423));
		std::string body_str = json_object_to_json_string(body);
		json_object_put(body);

		HttpResponse resp;
		if(!HttpRequest(url, "POST", headers, body_str, &resp, out_error))
			return false;

		std::string new_key = cloudhttp::ExtractHeaderValue(resp.headers, "X-Gaikai-Session");
		if(!new_key.empty())
			config_key = new_key;

		json_object *allocation = json_tokener_parse(resp.body.c_str());
		if(!allocation)
		{
			*out_error = "Invalid JSON from allocate response";
			return false;
		}

		json_object *queued_obj = nullptr, *migration_obj = nullptr, *poll_obj = nullptr;
		bool queued = json_object_object_get_ex(allocation, "queued", &queued_obj) && json_object_get_boolean(queued_obj);
		bool data_migration = json_object_object_get_ex(allocation, "dataMigration", &migration_obj) && json_object_get_boolean(migration_obj);
		int poll_frequency = 15;
		if(json_object_object_get_ex(allocation, "pollFrequency", &poll_obj))
			poll_frequency = json_object_get_int(poll_obj);

		if(queued || data_migration)
		{
			if(!waiting)
			{
				waiting = true;
				wait_start = std::chrono::steady_clock::now();
				json_object *estimate_obj = nullptr;
				int wait_estimate = -1;
				if(json_object_object_get_ex(allocation, "waitTimeEstimate", &estimate_obj))
					wait_estimate = json_object_get_int(estimate_obj);
				max_wait_seconds = wait_estimate > 0
					? std::min(wait_estimate * 2, kMaxAllocationWaitSeconds)
					: kDefaultAllocationWaitSeconds;
			}

			int elapsed_seconds = (int)std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - wait_start).count();
			if(elapsed_seconds >= max_wait_seconds)
			{
				*out_error = "Allocation timeout: server did not become ready within " + std::to_string(max_wait_seconds) + " seconds";
				json_object_put(allocation);
				return false;
			}

			int wait_time = std::min(poll_frequency, max_wait_seconds - elapsed_seconds);
			retry_count++;

			if(data_migration)
			{
				json_object *pct_obj = nullptr;
				int pct = json_object_object_get_ex(allocation, "dataMigrationPercentageComplete", &pct_obj) ? json_object_get_int(pct_obj) : 0;
				progress_cb("Migrating data (" + std::to_string(pct) + "%) - attempt " + std::to_string(retry_count));
			}
			else
			{
				json_object *pos_obj = nullptr;
				int queue_position = -1;
				if(json_object_object_get_ex(allocation, "displayQueuePosition", &pos_obj) ||
					json_object_object_get_ex(allocation, "queuePosition", &pos_obj))
					queue_position = json_object_get_int(pos_obj);
				progress_cb(queue_position >= 0
					? ("Allocating streaming slot - queue position " + std::to_string(queue_position) + " - attempt " + std::to_string(retry_count))
					: ("Allocating streaming slot - attempt " + std::to_string(retry_count)));
			}

			json_object_put(allocation);
			std::this_thread::sleep_for(std::chrono::seconds(wait_time));
			continue;
		}

		json_object *launch_slot = nullptr;
		if(!json_object_object_get_ex(allocation, "launchSlot", &launch_slot) || !launch_slot)
		{
			*out_error = "Allocation response is missing launchSlot";
			json_object_put(allocation);
			return false;
		}

		out_result->server_ip = JsonGetString(launch_slot, "publicIp");
		json_object *port_obj = nullptr;
		out_result->server_port = json_object_object_get_ex(launch_slot, "port", &port_obj) ? json_object_get_int(port_obj) : 0;
		std::string private_ip = JsonGetString(launch_slot, "privateIp");
		out_result->handshake_key = JsonGetString(allocation, "handshakeKey");
		out_result->launch_spec = JsonGetString(allocation, "launchSpecification");
		out_result->session_id = JsonGetString(allocation, "sessionId");

		out_result->psn_wrapper_type = 0x01;
		size_t last_dot = private_ip.find_last_of('.');
		if(last_dot != std::string::npos)
		{
			int octet = std::atoi(private_ip.c_str() + last_dot + 1);
			if(octet >= 0 && octet <= 255)
				out_result->psn_wrapper_type = (uint8_t)octet;
		}

		out_result->mtu_in = (uint32_t)selected_mtu_in;
		out_result->mtu_out = (uint32_t)selected_mtu_out;
		out_result->rtt_us = (uint64_t)selected_rtt_ms * 1000;

		json_object_put(allocation);

		if(out_result->server_ip.empty() || out_result->server_port <= 0 || out_result->handshake_key.empty() || out_result->launch_spec.empty())
		{
			*out_error = "Allocation succeeded but response is missing connection details";
			return false;
		}

		CHIAKI_LOGI(log, "CloudGaikai: allocation successful, server=%s:%d session=%s",
			out_result->server_ip.c_str(), out_result->server_port, out_result->session_id.c_str());
		return true;
	}
}

CloudGaikai::Result CloudGaikai::Run(std::function<void(const std::string &)> progress_cb)
{
	Result result;

	if(npsso.empty())
	{
		result.error = "Not signed in to PlayStation";
		return result;
	}

	BuildRequestGameSpec();

	progress_cb("Getting client IDs");
	if(!Step0_GetClientIds(&result.error))
		return result;

	progress_cb("Getting configuration");
	if(!Step7_GetConfig(&result.error))
		return result;

	progress_cb("Starting session");
	if(!Step8_StartSession(&result.error))
		return result;

	progress_cb("Getting tokens");
	if(!Step8a_GetGkAuthCode(&result.error))
		return result;
	if(!Step8b_GetServerAuthCode(&result.error))
		return result;

	progress_cb("Authorizing session");
	if(!Step9_AuthorizeSession(&result.error))
		return result;

	progress_cb("Locking session");
	if(!Step10_LockSession(progress_cb, &result.error))
		return result;

	progress_cb("Selecting a datacenter");
	if(!Step11_PickDatacenter(&result.error))
		return result;
	if(!Step12_SelectDatacenter(&result.error))
		return result;

	progress_cb("Allocating streaming slot");
	if(!Step13_AllocateSlot(&result, progress_cb, &result.error))
		return result;

	result.success = true;
	return result;
}
