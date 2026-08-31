// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#ifndef CHIAKI_SETTINGS_H
#define CHIAKI_SETTINGS_H

#include <regex>

#include <chiaki/log.h>
#include "host.h"

// mutual host and settings
class Host;

enum class StreamableState
{
	Unknown = 0,
	Streamable = 1,
	NotStreamable = 2
};

class Settings
{
	protected:
		// keep constructor private (sigleton class)
		Settings();
		static Settings * instance;

	private:
		const char * filename = "chiaki.conf";
		ChiakiLog log;
		std::map<std::string, Host> hosts;

		// global_settings from psedo INI file
		ChiakiVideoResolutionPreset global_video_resolution = CHIAKI_VIDEO_RESOLUTION_PRESET_1080p;
		ChiakiVideoFPSPreset global_video_fps = CHIAKI_VIDEO_FPS_PRESET_60;
		std::string global_psn_online_id = "";
		std::string global_psn_account_id = "";

		// PSN cloud gaming account state (account-level, not per-host)
		std::string global_npsso = "";
		std::string global_psn_access_token = "";
		std::string global_psn_id_token = "";
		int64_t global_psn_token_expiry = 0; // unix epoch seconds, 0 = unknown/expired
		std::string global_duid = "";

		// Playback tuning (account-level, applies to every cloud stream of that kind)
		ChiakiVideoResolutionPreset global_ps5_library_resolution = CHIAKI_VIDEO_RESOLUTION_PRESET_1080p;
		ChiakiVideoResolutionPreset global_ps34_catalog_resolution = CHIAKI_VIDEO_RESOLUTION_PRESET_720p;
		int global_ps5_library_bitrate_kbps = 0;  // 0 = use the resolution preset's default
		int global_ps34_catalog_bitrate_kbps = 0; // 0 = use the resolution preset's default
		int global_sharpen_level = 0;       // 0=Off, 1..3=Low/Medium/High
		bool global_video_pacing_smooth = false; // false=Standard, true=Smooth (see io.h)

		// Per-title streamability, keyed by CloudGame::product_id. Learned from
		// real stream-start outcomes (see views/cloud_game_list.cpp) so the
		// catalog tabs can show a tick/cross/question-mark next to each title.
		std::map<std::string, StreamableState> title_streamable;

		typedef enum configurationitem
		{
			UNKNOWN,
			HOST_NAME,
			HOST_ADDR,
			PSN_ONLINE_ID,
			PSN_ACCOUNT_ID,
			RP_KEY,
			RP_KEY_TYPE,
			RP_REGIST_KEY,
			VIDEO_RESOLUTION,
			VIDEO_FPS,
			TARGET,
			NPSSO,
			PSN_ACCESS_TOKEN,
			PSN_ID_TOKEN,
			PSN_TOKEN_EXPIRY,
			DUID,
			PS5_LIBRARY_RESOLUTION,
			PS34_CATALOG_RESOLUTION,
			PS5_LIBRARY_BITRATE_KBPS,
			PS34_CATALOG_BITRATE_KBPS,
			SHARPEN_LEVEL,
			VIDEO_PACING_SMOOTH,
			TITLE_STREAMABLE,
		} ConfigurationItem;

		// dummy parser implementation
		// the aim is not to have bulletproof parser
		// the goal is to read/write inernal flat configuration file
		const std::map<Settings::ConfigurationItem, std::regex> re_map = {
			{HOST_NAME, std::regex("^\\[\\s*(.+)\\s*\\]")},
			{HOST_ADDR, std::regex("^\\s*host_(?:ip|addr)\\s*=\\s*\"?((\\d+\\.\\d+\\.\\d+\\.\\d+)|([A-Za-z0-9-]+(\\.[A-Za-z0-9-]+)+))\"?")},
			{PSN_ONLINE_ID, std::regex("^\\s*psn_online_id\\s*=\\s*\"?([\\w_-]+)\"?")},
			{PSN_ACCOUNT_ID, std::regex("^\\s*psn_account_id\\s*=\\s*\"?([\\w/=+]+)\"?")},
			{RP_KEY, std::regex("^\\s*rp_key\\s*=\\s*\"?([\\w/=+]+)\"?")},
			{RP_KEY_TYPE, std::regex("^\\s*rp_key_type\\s*=\\s*\"?(\\d)\"?")},
			{RP_REGIST_KEY, std::regex("^\\s*rp_regist_key\\s*=\\s*\"?([\\w/=+]+)\"?")},
			{VIDEO_RESOLUTION, std::regex("^\\s*video_resolution\\s*=\\s*\"?(1080p|720p|540p|360p)\"?")},
			{VIDEO_FPS, std::regex("^\\s*video_fps\\s*=\\s*\"?(60|30)\"?")},
			{TARGET, std::regex("^\\s*target\\s*=\\s*\"?(\\d+)\"?")},
			{NPSSO, std::regex("^\\s*npsso\\s*=\\s*\"?([\\w./=+-]+)\"?")},
			{PSN_ACCESS_TOKEN, std::regex("^\\s*psn_access_token\\s*=\\s*\"?([\\w./=+-]+)\"?")},
			{PSN_ID_TOKEN, std::regex("^\\s*psn_id_token\\s*=\\s*\"?([\\w./=+-]+)\"?")},
			{PSN_TOKEN_EXPIRY, std::regex("^\\s*psn_token_expiry\\s*=\\s*\"?(\\d+)\"?")},
			{DUID, std::regex("^\\s*duid\\s*=\\s*\"?([\\w-]+)\"?")},
			{PS5_LIBRARY_RESOLUTION, std::regex("^\\s*ps5_library_resolution\\s*=\\s*\"?(1080p|720p)\"?")},
			{PS34_CATALOG_RESOLUTION, std::regex("^\\s*ps34_catalog_resolution\\s*=\\s*\"?(1080p|720p)\"?")},
			{PS5_LIBRARY_BITRATE_KBPS, std::regex("^\\s*ps5_library_bitrate_kbps\\s*=\\s*\"?(\\d+)\"?")},
			{PS34_CATALOG_BITRATE_KBPS, std::regex("^\\s*ps34_catalog_bitrate_kbps\\s*=\\s*\"?(\\d+)\"?")},
			{SHARPEN_LEVEL, std::regex("^\\s*sharpen_level\\s*=\\s*\"?(\\d)\"?")},
			{VIDEO_PACING_SMOOTH, std::regex("^\\s*video_pacing_smooth\\s*=\\s*\"?(0|1)\"?")},
			{TITLE_STREAMABLE, std::regex("^\\s*title_streamable\\s*=\\s*\"?(.+?)\"?\\s*$")},
		};

		ConfigurationItem ParseLine(std::string * line, std::string * value);
		size_t GetB64encodeSize(size_t);

	public:
		// singleton configuration
		Settings(const Settings&) = delete;
		void operator=(const Settings&) = delete;
		static Settings * GetInstance();

		ChiakiLog * GetLogger();
		std::map<std::string, Host> * GetHostsMap();
		Host * GetOrCreateHost(std::string * host_name);

		void ParseFile();
		int WriteFile();

		std::string ResolutionPresetToString(ChiakiVideoResolutionPreset resolution);
		int ResolutionPresetToInt(ChiakiVideoResolutionPreset resolution);
		ChiakiVideoResolutionPreset StringToResolutionPreset(std::string value);

		std::string FPSPresetToString(ChiakiVideoFPSPreset fps);
		int FPSPresetToInt(ChiakiVideoFPSPreset fps);
		ChiakiVideoFPSPreset StringToFPSPreset(std::string value);

		std::string GetHostName(Host * host);
		std::string GetHostAddr(Host * host);
		void SetHostAddr(Host * host, std::string host_addr);

		std::string GetPSNOnlineID(Host * host);
		void SetPSNOnlineID(Host * host, std::string psn_online_id);

		std::string GetPSNAccountID(Host * host);
		void SetPSNAccountID(Host * host, std::string psn_account_id);

		ChiakiVideoResolutionPreset GetVideoResolution(Host * host);
		void SetVideoResolution(Host * host, ChiakiVideoResolutionPreset value);
		void SetVideoResolution(Host * host, std::string value);

		ChiakiVideoFPSPreset GetVideoFPS(Host * host);
		void SetVideoFPS(Host * host, ChiakiVideoFPSPreset value);
		void SetVideoFPS(Host * host, std::string value);

		ChiakiTarget GetChiakiTarget(Host * host);
		bool SetChiakiTarget(Host * host, ChiakiTarget target);
		bool SetChiakiTarget(Host * host, std::string value);

		std::string GetHostRPKey(Host * host);
		bool SetHostRPKey(Host * host, std::string rp_key_b64);

		std::string GetHostRPRegistKey(Host * host);
		bool SetHostRPRegistKey(Host * host, std::string rp_regist_key_b64);

		int GetHostRPKeyType(Host * host);
		bool SetHostRPKeyType(Host * host, std::string value);

		// PSN cloud gaming account state
		std::string GetNPSSO();
		void SetNPSSO(std::string npsso);

		std::string GetPSNAccessToken();
		void SetPSNAccessToken(std::string token);

		std::string GetPSNIdToken();
		void SetPSNIdToken(std::string token);

		int64_t GetPSNTokenExpiry();
		void SetPSNTokenExpiry(int64_t expiry_unix_time);
		void SetPSNTokenExpiry(std::string expiry_unix_time);

		// Stable per-device identifier sent to PSN cloud gaming APIs. Generated
		// once via chiaki_holepunch_generate_client_device_uid and persisted.
		std::string GetOrCreateDUID();

		bool IsCloudLoggedIn();
		void ClearCloudLogin();

		// Playback tuning
		ChiakiVideoResolutionPreset GetPs5LibraryResolution();
		void SetPs5LibraryResolution(ChiakiVideoResolutionPreset value);
		void SetPs5LibraryResolution(std::string value);

		ChiakiVideoResolutionPreset GetPs34CatalogResolution();
		void SetPs34CatalogResolution(ChiakiVideoResolutionPreset value);
		void SetPs34CatalogResolution(std::string value);

		int GetPs5LibraryBitrateKbps(); // 0 = use the resolution preset's default bitrate
		void SetPs5LibraryBitrateKbps(int kbps);
		void SetPs5LibraryBitrateKbps(std::string kbps);

		int GetPs34CatalogBitrateKbps(); // 0 = use the resolution preset's default bitrate
		void SetPs34CatalogBitrateKbps(int kbps);
		void SetPs34CatalogBitrateKbps(std::string kbps);

		int GetSharpenLevel(); // 0=Off, 1..3=Low/Medium/High
		void SetSharpenLevel(int level);
		void SetSharpenLevel(std::string level);

		bool GetVideoPacingSmooth();
		void SetVideoPacingSmooth(bool smooth);
		void SetVideoPacingSmooth(std::string value);

		// Per-title streamability (see title_streamable above)
		StreamableState GetTitleStreamable(std::string product_id);
		void SetTitleStreamable(std::string product_id, StreamableState state);
};

#endif // CHIAKI_SETTINGS_H
