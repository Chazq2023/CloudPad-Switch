// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#ifndef CHIAKI_HOST_H
#define CHIAKI_HOST_H

#include <netinet/in.h>
#include <map>
#include <string>

#include <chiaki/controller.h>
#include <chiaki/discovery.h>
#include <chiaki/log.h>
#include <chiaki/opusdecoder.h>
#include <chiaki/regist.h>

#include "exception.h"
#include "io.h"
#include "settings.h"

static void InitAudioCB(int16_t *buf, size_t buf_size, void *user);
static void HapticsFrameCb(unsigned int channels, unsigned int rate, void *user);
static bool VideoCB(uint8_t *buf, size_t buf_size, void *user);
static void AudioCB(int16_t *buf, size_t samples_count, void *user);
static void EventCB(ChiakiEvent *event, void *user);

class Settings;

class Host
{
	private:
		ChiakiLog *log = nullptr;
		Settings *settings = nullptr;
		//video config
		ChiakiVideoResolutionPreset video_resolution = CHIAKI_VIDEO_RESOLUTION_PRESET_720p;
		ChiakiVideoFPSPreset video_fps = CHIAKI_VIDEO_FPS_PRESET_60;
		int haptic = 0; 
		std::string host_type;
		// user info
		std::string psn_online_id = "";
		std::string psn_account_id = "";
		std::function<void()> chiaki_event_connected_cb = nullptr;
		std::function<void(bool)> chiaki_even_login_pin_request_cb = nullptr;
		std::function<void(uint8_t, uint8_t)> chiaki_event_rumble_cb = nullptr;
		std::function<void(ChiakiQuitEvent *)> chiaki_event_quit_cb = nullptr;
		std::function<void(ChiakiControllerState *, std::map<uint32_t, int8_t> *)> io_read_controller_cb = nullptr;

		// internal state
		bool discovered = false;
		bool registered = false;
		// rp_key_data is true when rp_key, rp_regist_key, rp_key_type
		bool rp_key_data = false;

		std::string host_name;
		// sony's host_id == mac addr without colon
		std::string host_id;
		std::string host_addr;
		std::string ap_ssid;
		std::string ap_bssid;
		std::string ap_key;
		std::string ap_name;
		std::string server_nickname;
		ChiakiTarget target = CHIAKI_TARGET_PS4_UNKNOWN;
		ChiakiDiscoveryHostState state = CHIAKI_DISCOVERY_HOST_STATE_UNKNOWN;
		ChiakiControllerState controller_state = {0};
		std::map<uint32_t, int8_t> finger_id_touch_id;

		// mac address = 48 bits
		uint8_t server_mac[6] = {0};
		char rp_regist_key[CHIAKI_SESSION_AUTH_SIZE] = {0};
		uint32_t rp_key_type = 0;
		uint8_t rp_key[0x10] = {0};
		// manage stream session
		bool session_init = false;
		ChiakiSession session;
		ChiakiOpusDecoder opus_decoder;
		ChiakiConnectVideoProfile video_profile;

		// cloud (PSNOW/PSCLOUD) session state, set by SetCloudConnectInfo instead
		// of the local discovery/registration fields above. cloud_launch_spec in
		// particular must stay alive for the whole session (chiaki_session_init
		// stores the pointer as-is, it does not copy it).
		bool cloud_mode = false;
		ChiakiServiceType cloud_service_type = CHIAKI_SERVICE_TYPE_REMOTE_PLAY;
		std::string cloud_server_addr;
		int cloud_server_port = 0;
		std::string cloud_launch_spec;
		std::string cloud_handshake_key_b64;
		std::string cloud_session_id;
		uint8_t cloud_psn_wrapper_type = 0;
		uint32_t cloud_mtu_in = 0;
		uint32_t cloud_mtu_out = 0;
		uint64_t cloud_rtt_us = 0;
		friend class Settings;

	public:
		Host(std::string host_name);
		~Host();
		int InitSession(IO *);
		int FiniSession();
		// platform must be "ps3", "ps4", or "ps5" - selects the RP protocol
		// variant (PS4-style vs PS5-style) the same way local ps5 sessions do.
		void SetCloudConnectInfo(ChiakiServiceType service_type, std::string platform,
			std::string server_addr, int server_port, std::string launch_spec,
			std::string handshake_key_b64, std::string session_id, uint8_t psn_wrapper_type,
			uint32_t mtu_in, uint32_t mtu_out, uint64_t rtt_us);
		void StopSession();
		void StartSession();
		void SendFeedbackState();
		void ConnectionEventCB(ChiakiEvent *);
		bool GetVideoResolution(int *ret_width, int *ret_height);
		std::string GetHostName();
		std::string GetHostAddr();
		ChiakiTarget GetChiakiTarget();
		void SetChiakiTarget(ChiakiTarget target);
		void SetHostAddr(std::string host_addr);
		void SetEventConnectedCallback(std::function<void()> chiaki_event_connected_cb);
		void SetEventLoginPinRequestCallback(std::function<void(bool)> chiaki_even_login_pin_request_cb);
		void SetEventRumbleCallback(std::function<void(uint8_t, uint8_t)> chiaki_event_rumble_cb);
		void SetEventQuitCallback(std::function<void(ChiakiQuitEvent *)> chiaki_event_quit_cb);
		void SetReadControllerCallback(std::function<void(ChiakiControllerState *, std::map<uint32_t, int8_t> *)> io_read_controller_cb);
		bool IsPS5();
		void PushHapticsFrame(uint8_t *buf, size_t buf_size);
};

#endif
