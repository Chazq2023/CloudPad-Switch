// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#ifndef CHIAKI_IO_H
#define CHIAKI_IO_H

#include <SDL2/SDL.h>
#include <cstdint>
#include <functional>

#include <glad.h> // glad library (OpenGL loader)

#include <chiaki/session.h>

/*
https://github.com/devkitPro/switch-glad/blob/master/include/glad/glad.h
https://glad.dav1d.de/#profile=core&language=c&specification=gl&api=gl%3D4.3&extensions=GL_EXT_texture_compression_s3tc&extensions=GL_EXT_texture_filter_anisotropic

Language/Generator: C/C++
Specification: gl
APIs: gl=4.3
Profile: core
Extensions:
GL_EXT_texture_compression_s3tc,
GL_EXT_texture_filter_anisotropic
Loader: False
Local files: False
Omit khrplatform: False
Reproducible: False
*/

#ifdef __SWITCH__
#include <switch.h>
#else
#include <iostream>
#endif

#include <mutex>
#include <map>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#include <chiaki/controller.h>
#include <chiaki/log.h>

#include "exception.h"

#define PLANES_COUNT 3
#define SDL_JOYSTICK_COUNT 2

class IO
{
	protected:
		IO();
		static IO * instance;
	private:
		ChiakiLog *log;
		int video_width;
		int video_height;
		bool quit = false;
		static const int MAX_FRAME_COUNT = 3;
		static const int MAX_NV12_PLANE_COUNT = 2;
		GLint m_texture_uniform[MAX_NV12_PLANE_COUNT];
		// opengl reader writer
		std::mutex mtx;
		// default nintendo switch res
    AVBufferRef *hw_device_ctx = nullptr;
		int screen_width = 1280;
		int screen_height = 720;
		const AVCodec *codec;
		AVCodecContext *codec_context;
		AVFrame **frames;
		uintptr_t origin_ptr[MAX_FRAME_COUNT][MAX_NV12_PLANE_COUNT];
		AVFrame *tmp_frame;
		SDL_AudioDeviceID sdl_audio_device_id = 0;
		SDL_Event sdl_event;
		SDL_Joystick *sdl_joystick_ptr[SDL_JOYSTICK_COUNT] = {0};
		SDL_Haptic *sdl_haptic_ptr[2];
#ifdef __SWITCH__
		PadState pad;
		HidSixAxisSensorHandle sixaxis_handles[4];
		HidVibrationDeviceHandle vibration_handles[2][2];
#endif
		GLuint vao;
		GLuint vbo;
		GLuint tex[PLANES_COUNT];
		GLuint pbo[PLANES_COUNT];
		GLuint vert;
		GLuint frag;
		GLuint prog;
		GLint sharpen_uniform = -1;
		GLint texel_size_uniform = -1;
		float sharpen_amount = 0.0f; // 0=Off, matches shader's u_sharpen

		// "Standard" vs "Smooth" video pacing (see IO::SetVideoPacing). Standard
		// re-uploads and presents the newest decoded frame every draw tick
		// (lowest latency, arrival jitter shows as motion hitches). Smooth
		// detects the source's 30/60fps cadence from decode arrival spacing and
		// only re-uploads on the matching tick, holding the previous texture
		// content (a plain skip - no re-upload call - since the GPU texture
		// already retains its last-uploaded pixels) in between, trading a bit
		// of latency for steadier motion. Adapted from the Steady/Smooth
		// pacing modes in rmrf404/green-nx's Switch deko3d renderer, simplified
		// to fit this app's texture-upload-per-draw pipeline instead of its
		// AVFrame-queue-based one.
		bool video_pacing_smooth = false;
		uint32_t pacing_source_refresh_period = 1; // 1=60fps-like, 2=30fps-like
		uint32_t pacing_fast_streak = 0;
		uint32_t pacing_slow_streak = 0;
		uint64_t pacing_last_decode_ms = 0;
		uint32_t pacing_phase = 0;
		bool InitOpenGl();
		bool InitOpenGlTextures();
		bool InitOpenGlTX1Textures();
		bool InitOpenGlShader();
		void OpenGlDraw();
#ifdef DEBUG_OPENGL
		void CheckGLError(const char *func, const char *file, int line);
		void DumpShaderError(GLuint prog, const char *func, const char *file, int line);
		void DumpProgramError(GLuint prog, const char *func, const char *file, int line);
#endif
		GLuint CreateAndCompileShader(GLenum type, const char *source);
		void SetOpenGlYUVPixels(AVFrame *frame);
		void SetOpenGlNV12Pixels(AVFrame *frame);
		bool ReadGameKeys(SDL_Event *event, ChiakiControllerState *state);
		bool ReadGameTouchScreen(ChiakiControllerState *state, std::map<uint32_t, int8_t> *finger_id_touch_id);
		bool ReadGameSixAxis(ChiakiControllerState *state);
	public:
		// singleton configuration
		IO(const IO&) = delete;
		void operator=(const IO&) = delete;
		static IO * GetInstance();
		int HapticBase = 400;

		~IO();
		bool isFirst = true;
		void SetMesaConfig();
		bool VideoCB(uint8_t *buf, size_t buf_size, int32_t frames_lost, bool frame_recovered, void *user);
		void InitAudioCB(unsigned int channels, unsigned int rate);
		void AudioCB(int16_t *buf, size_t samples_count);
		bool InitVideo(int video_width, int video_height, int screen_width, int screen_height);
		bool InitAVCodec(bool is_PS5);
		bool FreeVideo();
		bool InitController();
		bool FreeController();
		bool MainLoop();
		void UpdateControllerState(ChiakiControllerState *state, std::map<uint32_t, int8_t> *finger_id_touch_id);
		void SetRumble(uint8_t left, uint8_t right);
		void SetHapticRumble(uint8_t left, uint8_t right);
		void HapticCB(uint8_t *buf, size_t buf_size);
		void CleanUpHaptic();

		// level: 0=Off, 1..3=Low/Medium/High (see Settings::GetSharpenLevel).
		void SetSharpenLevel(int level);
		// See video_pacing_smooth above.
		void SetVideoPacing(bool smooth);
};

#endif //CHIAKI_IO_H
