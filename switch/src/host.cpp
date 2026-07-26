// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include <cstring>

#include "host.h"
#include "io.h"

static void InitAudioCB(unsigned int channels, unsigned int rate, void *user)
{
	IO *io = (IO *)user;
	io->InitAudioCB(channels, rate);
}

static bool VideoCB(uint8_t *buf, size_t buf_size, int32_t frames_lost, bool frame_recovered, void *user)
{
	IO *io = (IO *)user;
	return io->VideoCB(buf, buf_size, frames_lost, frame_recovered, user);
}

static void AudioCB(int16_t *buf, size_t samples_count, void *user)
{
	IO *io = (IO *)user;
	io->AudioCB(buf, samples_count);
}

static void HapticsFrameCb(uint8_t *buf, size_t buf_size, void *user)
{
	IO *io = (IO *)user;
	io->HapticCB(buf, buf_size);
}

static void EventCB(ChiakiEvent *event, void *user)
{
	Host *host = (Host *)user;
	host->ConnectionEventCB(event);
}

Host::Host(std::string host_name)
	: host_name(host_name)
{
	this->settings = Settings::GetInstance();
	this->log = settings->GetLogger();
}

Host::~Host()
{
}

int Host::InitSession(IO *user)
{
	chiaki_connect_video_profile_preset(&(this->video_profile),
		this->video_resolution, this->video_fps);
	// Build chiaki ps4 stream session
	chiaki_opus_decoder_init(&(this->opus_decoder), this->log);
	ChiakiAudioSink audio_sink;
	ChiakiAudioSink haptics_sink;
	haptics_sink.user = user;
	haptics_sink.frame_cb = HapticsFrameCb;
	ChiakiConnectInfo chiaki_connect_info = {};

	chiaki_connect_info.video_profile = this->video_profile;
	chiaki_connect_info.video_profile_auto_downgrade = true;
	if (this->IsPS5()) {
		chiaki_connect_info.video_profile.codec = CHIAKI_CODEC_H265;
	}
	if (this->haptic > 0) {
		chiaki_connect_info.enable_dualsense = true;
		if (this->haptic == 1) {
			user->HapticBase = 580;
		} else {
			user->HapticBase = 400;
		}
	}

	chiaki_connect_info.ps5 = this->IsPS5();

	if(this->cloud_mode)
	{
		chiaki_connect_info.host = this->cloud_server_addr.c_str();
		chiaki_connect_info.service_type = this->cloud_service_type;
		chiaki_connect_info.cloud_launch_spec = this->cloud_launch_spec.c_str();
		chiaki_connect_info.cloud_handshake_key = this->cloud_handshake_key_b64.c_str();
		chiaki_connect_info.cloud_session_id = this->cloud_session_id.c_str();
		chiaki_connect_info.cloud_port = (uint16_t)this->cloud_server_port;
		chiaki_connect_info.cloud_psn_wrapper_type = this->cloud_psn_wrapper_type;
		chiaki_connect_info.cloud_mtu_in = this->cloud_mtu_in;
		chiaki_connect_info.cloud_mtu_out = this->cloud_mtu_out;
		chiaki_connect_info.cloud_rtt_us = this->cloud_rtt_us;
	}
	else
	{
		chiaki_connect_info.host = this->host_addr.c_str();
		chiaki_connect_info.service_type = CHIAKI_SERVICE_TYPE_REMOTE_PLAY;
		memcpy(chiaki_connect_info.regist_key, this->rp_regist_key, sizeof(chiaki_connect_info.regist_key));
		memcpy(chiaki_connect_info.morning, this->rp_key, sizeof(chiaki_connect_info.morning));
	}

	if(!user->InitAVCodec(this->IsPS5()))
	{
		throw Exception("Failed to initiate libav codec");
	}
	if(!user->InitVideo(chiaki_connect_info.video_profile.width, chiaki_connect_info.video_profile.height, 1280, 720))
	{
		throw Exception("Failed to initiate video");
	}

	ChiakiErrorCode err = chiaki_session_init(&(this->session), &chiaki_connect_info, this->log);
	if(err != CHIAKI_ERR_SUCCESS)
		throw Exception(chiaki_error_string(err));
	this->session_init = true;
	// audio setting_cb and frame_cb
	chiaki_opus_decoder_set_cb(&this->opus_decoder, InitAudioCB, AudioCB, user);
	chiaki_opus_decoder_get_sink(&this->opus_decoder, &audio_sink);
	chiaki_session_set_audio_sink(&this->session, &audio_sink);
	chiaki_session_set_haptics_sink(&this->session, &haptics_sink);
	chiaki_session_set_video_sample_cb(&this->session, VideoCB, user);
	chiaki_session_set_event_cb(&this->session, EventCB, this);

	// init controller states
	chiaki_controller_state_set_idle(&this->controller_state);

	return 0;
}

void Host::SetCloudConnectInfo(ChiakiServiceType service_type, std::string platform,
	std::string server_addr, int server_port, std::string launch_spec,
	std::string handshake_key_b64, std::string session_id, uint8_t psn_wrapper_type,
	uint32_t mtu_in, uint32_t mtu_out, uint64_t rtt_us)
{
	this->cloud_mode = true;
	this->cloud_service_type = service_type;
	this->cloud_server_addr = server_addr;
	this->cloud_server_port = server_port;
	this->cloud_launch_spec = launch_spec;
	this->cloud_handshake_key_b64 = handshake_key_b64;
	this->cloud_session_id = session_id;
	this->cloud_psn_wrapper_type = psn_wrapper_type;
	this->cloud_mtu_in = mtu_in;
	this->cloud_mtu_out = mtu_out;
	this->cloud_rtt_us = rtt_us;

	// Drives IsPS5() (and so the H265/ps5 branches InitSession already has for
	// local sessions) the same way a locally discovered PS5 host would.
	this->target = (platform == "ps5") ? CHIAKI_TARGET_PS5_1 : CHIAKI_TARGET_PS4_10;
}

int Host::FiniSession()
{
	if(this->session_init)
	{
		this->session_init = false;
		chiaki_session_join(&this->session);
		chiaki_session_fini(&this->session);
		chiaki_opus_decoder_fini(&this->opus_decoder);
	}
	return 0;
}

void Host::StopSession()
{
	chiaki_session_stop(&this->session);
}

void Host::StartSession()
{
	ChiakiErrorCode err = chiaki_session_start(&this->session);
	if(err != CHIAKI_ERR_SUCCESS)
	{
		chiaki_session_fini(&this->session);
		throw Exception("Chiaki Session Start failed");
	}
}

void Host::SendFeedbackState()
{
	// send controller/joystick key
	if(this->io_read_controller_cb != nullptr)
		this->io_read_controller_cb(&this->controller_state, &finger_id_touch_id);

	chiaki_session_set_controller_state(&this->session, &this->controller_state);
}

void Host::ConnectionEventCB(ChiakiEvent *event)
{
	switch(event->type)
	{
		case CHIAKI_EVENT_CONNECTED:
			CHIAKI_LOGI(this->log, "EventCB CHIAKI_EVENT_CONNECTED");
			if(this->chiaki_event_connected_cb != nullptr)
				this->chiaki_event_connected_cb();
			break;
		case CHIAKI_EVENT_LOGIN_PIN_REQUEST:
			CHIAKI_LOGI(this->log, "EventCB CHIAKI_EVENT_LOGIN_PIN_REQUEST");
			if(this->chiaki_even_login_pin_request_cb != nullptr)
				this->chiaki_even_login_pin_request_cb(event->login_pin_request.pin_incorrect);
			break;
		case CHIAKI_EVENT_RUMBLE:
			CHIAKI_LOGD(this->log, "EventCB CHIAKI_EVENT_RUMBLE");
			if(this->chiaki_event_rumble_cb != nullptr)
				this->chiaki_event_rumble_cb(event->rumble.left, event->rumble.right);
			break;
		case CHIAKI_EVENT_QUIT:
			CHIAKI_LOGI(this->log, "EventCB CHIAKI_EVENT_QUIT");
			if(this->chiaki_event_quit_cb != nullptr)
				this->chiaki_event_quit_cb(&event->quit);
			break;
	}
}

bool Host::GetVideoResolution(int *ret_width, int *ret_height)
{
	switch(this->video_resolution)
	{
		case CHIAKI_VIDEO_RESOLUTION_PRESET_360p:
			*ret_width = 640;
			*ret_height = 360;
			break;
		case CHIAKI_VIDEO_RESOLUTION_PRESET_540p:
			*ret_width = 950;
			*ret_height = 540;
			break;
		case CHIAKI_VIDEO_RESOLUTION_PRESET_720p:
			*ret_width = 1280;
			*ret_height = 720;
			break;
		case CHIAKI_VIDEO_RESOLUTION_PRESET_1080p:
			*ret_width = 1920;
			*ret_height = 1080;
			break;
		default:
			return false;
	}
	return true;
}

std::string Host::GetHostName()
{
	return this->host_name;
}

std::string Host::GetHostAddr()
{
	return this->host_addr;
}

ChiakiTarget Host::GetChiakiTarget()
{
	return this->target;
}

void Host::SetChiakiTarget(ChiakiTarget target)
{
	this->target = target;
}

void Host::SetHostAddr(std::string host_addr)
{
	this->host_addr = host_addr;
}

void Host::SetEventConnectedCallback(std::function<void()> chiaki_event_connected_cb)
{
	this->chiaki_event_connected_cb = chiaki_event_connected_cb;
}

void Host::SetEventLoginPinRequestCallback(std::function<void(bool)> chiaki_even_login_pin_request_cb)
{
	this->chiaki_even_login_pin_request_cb = chiaki_even_login_pin_request_cb;
}

void Host::SetEventQuitCallback(std::function<void(ChiakiQuitEvent *)> chiaki_event_quit_cb)
{
	this->chiaki_event_quit_cb = chiaki_event_quit_cb;
}

void Host::SetEventRumbleCallback(std::function<void(uint8_t, uint8_t)> chiaki_event_rumble_cb)
{
	this->chiaki_event_rumble_cb = chiaki_event_rumble_cb;
}

void Host::SetReadControllerCallback(std::function<void(ChiakiControllerState *, std::map<uint32_t, int8_t> *)> io_read_controller_cb)
{
	this->io_read_controller_cb = io_read_controller_cb;
}

bool Host::IsPS5()
{
	if(this->target >= CHIAKI_TARGET_PS5_UNKNOWN)
		return true;
	else
		return false;
}
