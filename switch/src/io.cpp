// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include "io.h"
#include "settings.h"

#include <chrono>
#include <thread>

#include <switch.h>
#include <chiaki/thread.h>

// https://github.com/torvalds/linux/blob/41ba50b0572e90ed3d24fe4def54567e9050bc47/drivers/hid/hid-sony.c#L2742
#define DS4_TRACKPAD_MAX_X 1920
#define DS4_TRACKPAD_MAX_Y 942
#define SWITCH_TOUCHSCREEN_MAX_X 1280
#define SWITCH_TOUCHSCREEN_MAX_Y 720

// source:
// gui/src/avopenglwidget.cpp
//
// examples :
// https://www.roxlu.com/2014/039/decoding-h264-and-yuv420p-playback
// https://gist.github.com/roxlu/9329339

// use OpenGl to decode YUV
// the aim is to spare CPU load on nintendo switch

static const char *shader_vert_glsl = R"glsl(
#version 150 core
in vec2 pos_attr;
out vec2 uv_var;
void main()
{
	uv_var = pos_attr;
	gl_Position = vec4(pos_attr * vec2(2.0, -2.0) + vec2(-1.0, 1.0), 0.0, 1.0);
}
)glsl";

// Both fragment shaders take an unsharp-mask term on the luma (Y) plane only,
// before YUV->RGB conversion - sharpening luma alone avoids the color
// fringing a naive RGB-space unsharp mask would introduce. u_sharpen is 0 by
// default (identical output to before sharpening existed); IO::SetSharpenLevel
// maps Settings::GetSharpenLevel() (0..3) to a strength here.
static const char *yuv420p_shader_frag_glsl = R"glsl(
#version 150 core
uniform sampler2D plane1; // Y
uniform sampler2D plane2; // U
uniform sampler2D plane3; // V
uniform vec2 u_texel_size; // 1/width, 1/height of plane1
uniform float u_sharpen;   // 0 = off
in vec2 uv_var;
out vec4 out_color;
void main()
{
	float y_c = texture(plane1, uv_var).r;
	if(u_sharpen > 0.0)
	{
		float y_u = texture(plane1, uv_var - vec2(0.0, u_texel_size.y)).r;
		float y_d = texture(plane1, uv_var + vec2(0.0, u_texel_size.y)).r;
		float y_l = texture(plane1, uv_var - vec2(u_texel_size.x, 0.0)).r;
		float y_r = texture(plane1, uv_var + vec2(u_texel_size.x, 0.0)).r;
		y_c = y_c + u_sharpen * (4.0 * y_c - y_u - y_d - y_l - y_r);
	}
	vec3 yuv = vec3(
		(y_c - (16.0 / 255.0)) / ((235.0 - 16.0) / 255.0),
		(texture(plane2, uv_var).r - (16.0 / 255.0)) / ((240.0 - 16.0) / 255.0) - 0.5,
		(texture(plane3, uv_var).r - (16.0 / 255.0)) / ((240.0 - 16.0) / 255.0) - 0.5);
	vec3 rgb = mat3(
		1.0,		1.0,		1.0,
 		0.0,		-0.18733,	1.85563,
 		1.57480,	-0.46812, 	0.0) * yuv;
	out_color = vec4(rgb, 1.0);
}
)glsl";

static const char *nv12_shader_frag_glsl = R"glsl(
#version 150 core

uniform sampler2D plane1; // Y
uniform sampler2D plane2; // interlaced UV
uniform vec2 u_texel_size; // 1/width, 1/height of plane1
uniform float u_sharpen;   // 0 = off

in vec2 uv_var;

out vec4 out_color;

void main()
{
	float y_c = texture(plane1, uv_var).r;
	if(u_sharpen > 0.0)
	{
		float y_u = texture(plane1, uv_var - vec2(0.0, u_texel_size.y)).r;
		float y_d = texture(plane1, uv_var + vec2(0.0, u_texel_size.y)).r;
		float y_l = texture(plane1, uv_var - vec2(u_texel_size.x, 0.0)).r;
		float y_r = texture(plane1, uv_var + vec2(u_texel_size.x, 0.0)).r;
		y_c = y_c + u_sharpen * (4.0 * y_c - y_u - y_d - y_l - y_r);
	}
	vec3 yuv = vec3(
		(y_c - (16.0 / 255.0)) / ((235.0 - 16.0) / 255.0),
		(texture(plane2, uv_var).r - (16.0 / 255.0)) / ((240.0 - 16.0) / 255.0) - 0.5,
		(texture(plane2, uv_var).g - (16.0 / 255.0)) / ((240.0 - 16.0) / 255.0) - 0.5
	);
	vec3 rgb = mat3(
		1.0,		1.0,		1.0,
 		0.0,		-0.18733,	1.85563,
 		1.57480,	-0.46812, 	0.0) * yuv;
	out_color = vec4(rgb, 1.0);
}
)glsl";

std::atomic<int> current_frame{0};
int next_frame = 0;

bool haptic_lock = false;
int haptic_val = 0;
std::chrono::system_clock::time_point haptic_lock_time;

static const float vert_pos[] = {
	0.0f, 0.0f,
	0.0f, 1.0f,
	1.0f, 0.0f,
	1.0f, 1.0f};

IO *IO::instance = nullptr;
bool enableHWAccl = true;

IO *IO::GetInstance()
{
	if(instance == nullptr)
	{
		instance = new IO;
	}
	return instance;
}

IO::IO()
{
	Settings *settings = Settings::GetInstance();
	this->log = settings->GetLogger();
}

IO::~IO()
{
	//FreeJoystick();
	if(this->sdl_audio_device_id <= 0)
	{
		SDL_CloseAudioDevice(this->sdl_audio_device_id);
	}
	FreeVideo();
}

void IO::SetMesaConfig()
{
	//TRACE("%s", "Mesaconfig");
	//setenv("MESA_GL_VERSION_OVERRIDE", "3.3", 1);
	//setenv("MESA_GLSL_VERSION_OVERRIDE", "330", 1);
	// Uncomment below to disable error checking and save CPU time (useful for production):
	//setenv("MESA_NO_ERROR", "1", 1);
#ifdef DEBUG_OPENGL
	// Uncomment below to enable Mesa logging:
	setenv("EGL_LOG_LEVEL", "debug", 1);
	setenv("MESA_VERBOSE", "all", 1);
	setenv("NOUVEAU_MESA_DEBUG", "1", 1);

	// Uncomment below to enable shader debugging in Nouveau:
	//setenv("NV50_PROG_OPTIMIZE", "0", 1);
	setenv("NV50_PROG_DEBUG", "1", 1);
	//setenv("NV50_PROG_CHIPSET", "0x120", 1);
#endif
}

#ifdef DEBUG_OPENGL
#define D(x)                                        \
	{                                               \
		(x);                                        \
		CheckGLError(__func__, __FILE__, __LINE__); \
	}
void IO::CheckGLError(const char *func, const char *file, int line)
{
	GLenum err;
	while((err = glGetError()) != GL_NO_ERROR)
	{
		CHIAKI_LOGE(this->log, "glGetError: %x function: %s from %s line %d", err, func, file, line);
		//GL_INVALID_VALUE, 0x0501
		// Given when a value parameter is not a legal value for that function. T
		// his is only given for local problems;
		// if the spec allows the value in certain circumstances,
		// where other parameters or state dictate those circumstances,
		// then GL_INVALID_OPERATION is the result instead.
	}
}

#define DS(x)                                             \
	{                                                     \
		DumpShaderError(x, __func__, __FILE__, __LINE__); \
	}
void IO::DumpShaderError(GLuint shader, const char *func, const char *file, int line)
{
	GLchar str[512 + 1];
	GLsizei len = 0;
	glGetShaderInfoLog(shader, 512, &len, str);
	if(len > 512)
		len = 512;
	str[len] = '\0';
	CHIAKI_LOGE(this->log, "glGetShaderInfoLog: %s function: %s from %s line %d", str, func, file, line);
}

#define DP(x)                                              \
	{                                                      \
		DumpProgramError(x, __func__, __FILE__, __LINE__); \
	}
void IO::DumpProgramError(GLuint prog, const char *func, const char *file, int line)
{
	GLchar str[512 + 1];
	GLsizei len = 0;
	glGetProgramInfoLog(prog, 512, &len, str);
	if(len > 512)
		len = 512;
	str[len] = '\0';
	CHIAKI_LOGE(this->log, "glGetProgramInfoLog: %s function: %s from %s line %d", str, func, file, line);
}

#else
// do nothing
#define D(x) \
	{        \
		(x); \
	}
#define DS(x) \
	{         \
	}
#define DP(x) \
	{         \
	}
#endif

bool IO::VideoCB(uint8_t *buf, size_t buf_size, int32_t frames_lost, bool frame_recovered, void *user)
{
	// Called on the Takion network-receive thread (see lib/src/videoreceiver.c's
	// chiaki_video_receiver_flush_frame -> video_sample_cb). buf points into the
	// frame processor's reassembly buffer, which gets reused for the next
	// frame, so it must be copied before handing off - the actual decode now
	// happens on a dedicated thread (see DecodeFrame/VideoDecodeThreadFunc)
	// so this network thread is never blocked by FFmpeg decode time.
	QueuedVideoFrame queued;
	queued.data.assign(buf, buf + buf_size);
	queued.frames_lost = frames_lost;
	queued.frame_recovered = frame_recovered;

	{
		std::lock_guard<std::mutex> lock(this->video_decode_mutex);
		if(this->video_decode_queue.size() >= VIDEO_DECODE_QUEUE_MAX)
		{
			CHIAKI_LOGW(this->log, "Video decode queue full, dropping oldest queued frame");
			this->video_decode_queue.pop_front();
		}
		this->video_decode_queue.push_back(std::move(queued));
	}
	this->video_decode_cv.notify_one();
	return true;
}

void IO::CpuSampleThreadFunc()
{
	// InfoType_IdleTickCount gives idle ticks accumulated per core since
	// boot; armGetSystemTick() is the same underlying ARM generic timer, so
	// (1 - delta_idle/delta_elapsed) over a sampling window is that core's
	// busy fraction. No handle wiring to individual threads needed - this
	// just answers "is the CPU actually maxed, and on which core(s)" before
	// guessing what code is responsible.
	static const int kCoreCount = 4;
	uint64_t prev_idle[kCoreCount] = {0};
	uint64_t prev_tick = armGetSystemTick();
	for(int core = 0; core < kCoreCount; core++)
		svcGetInfo(&prev_idle[core], InfoType_IdleTickCount, INVALID_HANDLE, core);

	// Per-thread ticks (InfoType_ThreadTickCount) alongside per-core idle -
	// once "which core(s) are maxed" wasn't enough to identify the culprit
	// (both software video decode and software AES-GCM were fixed with zero
	// change in the per-core numbers), this answers "which specific
	// registered thread(s) are actually spending that CPU time". Threads
	// register themselves (chiaki_switch_register_thread_for_sampling) as
	// they start, so the list can grow between samples - a thread's first
	// sample after registering will show its ticks since its own start, not
	// since the last 1s window, which is fine for a diagnostic.
	uint64_t prev_thread_ticks[CHIAKI_SWITCH_MAX_SAMPLED_THREADS] = {0};

	while(this->cpu_sample_thread_running)
	{
		std::this_thread::sleep_for(std::chrono::seconds(1));
		if(!this->cpu_sample_thread_running)
			break;

		uint64_t now_tick = armGetSystemTick();
		uint64_t elapsed = now_tick - prev_tick;
		prev_tick = now_tick;

		printf("[CPU PROBE] probe: busy%%");
		double total_pct = 0.0;
		for(int core = 0; core < kCoreCount; core++)
		{
			uint64_t idle = 0;
			svcGetInfo(&idle, InfoType_IdleTickCount, INVALID_HANDLE, core);
			uint64_t idle_delta = idle - prev_idle[core];
			prev_idle[core] = idle;
			double busy_pct = elapsed > 0 ? 100.0 * (1.0 - (double)idle_delta / (double)elapsed) : 0.0;
			printf(" core%d=%.1f", core, busy_pct);
			total_pct += busy_pct;
		}
		// Average across cores - the single number an overlay-style monitor
		// (e.g. sys-clk) typically reports, so this can be compared directly
		// against a user-reported "CPU%" reading rather than per-core numbers.
		printf(" avg=%.1f", total_pct / kCoreCount);
		printf("\n");

		printf("[CPU PROBE THREADS] probe:");
		int thread_count = chiaki_switch_sampled_threads_count;
		if(thread_count > CHIAKI_SWITCH_MAX_SAMPLED_THREADS)
			thread_count = CHIAKI_SWITCH_MAX_SAMPLED_THREADS;
		for(int i = 0; i < thread_count; i++)
		{
			// Reads what each thread last self-reported (chiaki_switch_self_
			// report_ticks, called from within that thread's own loop) rather
			// than querying it directly - InfoType_ThreadTickCount only
			// returns a real value when a thread queries itself; every other
			// combination silently returns 0 with no error, which is why the
			// first version of this sampler read a flat 0.0 for every thread.
			uint64_t ticks = chiaki_switch_sampled_threads[i].self_ticks;
			uint64_t delta = ticks - prev_thread_ticks[i];
			prev_thread_ticks[i] = ticks;
			double busy_pct = elapsed > 0 ? 100.0 * (double)delta / (double)elapsed : 0.0;
			printf(" [%s]=%.1f", chiaki_switch_sampled_threads[i].label, busy_pct);
		}
		printf("\n");
		fflush(stdout);
	}
}

void IO::VideoDecodeThreadFunc()
{
	// This is a raw std::thread, not one of lib/'s chiaki_thread_create'd
	// worker threads, so it never picked up the priority elevation applied
	// there (see lib/src/thread.c) - it ran at the same default priority as
	// Borealis's UI/render thread, with no guarantee of winning a timeslice
	// over it during motion. Decode falling behind under that contention
	// fills this thread's own bounded 8-frame queue (see VideoCB) and starts
	// dropping frames, which reads as exactly the lagging/artifacting this
	// was meant to fix. Same priority as the network worker threads, for the
	// same reason: this thread also must not lose CPU time to UI rendering.
	svcSetThreadPriority(threadGetCurHandle(), 0x2A);
	int cpu_sample_idx = chiaki_switch_register_thread_for_sampling("Video Decode");
	while(true)
	{
		QueuedVideoFrame queued;
		{
			std::unique_lock<std::mutex> lock(this->video_decode_mutex);
			this->video_decode_cv.wait(lock, [this] {
				return !this->video_decode_thread_running || !this->video_decode_queue.empty();
			});
			if(!this->video_decode_thread_running && this->video_decode_queue.empty())
				return;
			queued = std::move(this->video_decode_queue.front());
			this->video_decode_queue.pop_front();
		}
		this->DecodeFrame(queued.data.data(), queued.data.size(), queued.frames_lost, queued.frame_recovered);
		chiaki_switch_self_report_ticks(cpu_sample_idx);
	}
}

bool IO::DecodeFrame(uint8_t *buf, size_t buf_size, int32_t frames_lost, bool frame_recovered)
{
	// callback function to decode video buffer

	AVPacket* packet = av_packet_alloc();
	packet->data = buf;
	packet->size = buf_size;

send_packet:
	// Push
	int r = avcodec_send_packet(this->codec_context, packet);
	if(r != 0)
	{
		if(r == AVERROR(EAGAIN))
		{
			CHIAKI_LOGE(this->log, "AVCodec internal buffer is full removing frames before pushing");
			r = avcodec_receive_frame(this->codec_context, this->tmp_frame);
			// send decoded frame for sdl texture update
			if(r != 0)
			{
				CHIAKI_LOGE(this->log, "Failed to pull frame");
				av_packet_free(&packet);
				return false;
			}
			goto send_packet;
		}
		else
		{
			char errbuf[128];
			av_make_error_string(errbuf, sizeof(errbuf), r);
			CHIAKI_LOGE(this->log, "Failed to push frame: %s", errbuf);
			av_packet_free(&packet);
			return false;
		}
	}

	// Pull
	if (enableHWAccl) {
		r = avcodec_receive_frame(this->codec_context, this->tmp_frame);
		if (r == 0) {
			static bool logged_format_once = false;
			if(!logged_format_once)
			{
				logged_format_once = true;
				CHIAKI_LOGI(this->log, "[HWACCEL PROBE] first decoded frame format=%d (AV_PIX_FMT_NVTEGRA=%d) - %s",
					this->tmp_frame->format, AV_PIX_FMT_NVTEGRA,
					this->tmp_frame->format == AV_PIX_FMT_NVTEGRA ? "hardware decode active" : "still software decode");
			}
			if (av_hwframe_transfer_data(this->frames[next_frame], this->tmp_frame, 0) < 0) {
				CHIAKI_LOGI(this->log, "transfer error");
			}
			if (av_frame_copy_props(this->frames[next_frame], this->tmp_frame) < 0) {
				CHIAKI_LOGI(this->log, "copy error");
			}
		}
	} else {
		r = avcodec_receive_frame(this->codec_context, this->frames[next_frame]);
	}

	if(r != 0) {
		CHIAKI_LOGE(this->log, "Failed to pull frame");
	} else {
		current_frame.store(next_frame, std::memory_order_release);
		next_frame = (next_frame + 1) % MAX_FRAME_COUNT;
		this->decoded_frame_generation.fetch_add(1, std::memory_order_release);

		// Cadence detection for Smooth pacing (see io.h): ~16ms gaps between
		// decoded frames mean a 60fps-like source (present every draw tick),
		// ~33ms mean 30fps-like (present every other tick). A streak of 8
		// debounces one-off scheduling jitter so a single late/early frame
		// doesn't flip the period. Cheap enough to always run, even in
		// Standard mode where nothing reads pacing_source_refresh_period.
		uint64_t decoded_at_ms = SDL_GetTicks64();
		if(this->pacing_last_decode_ms)
		{
			uint64_t gap = decoded_at_ms - this->pacing_last_decode_ms;
			if(gap < 25)
			{
				this->pacing_fast_streak++;
				this->pacing_slow_streak = 0;
				if(this->pacing_fast_streak >= 8)
					this->pacing_source_refresh_period = 1;
			}
			else if(gap < 55)
			{
				this->pacing_slow_streak++;
				this->pacing_fast_streak = 0;
				if(this->pacing_slow_streak >= 8)
					this->pacing_source_refresh_period = 2;
			}
		}
		this->pacing_last_decode_ms = decoded_at_ms;
	}

	av_packet_free(&packet);
	return true;
}

void IO::InitAudioCB(unsigned int channels, unsigned int rate)
{
	SDL_AudioSpec want, have, test;
	SDL_memset(&want, 0, sizeof(want));

	//source
	//[I] Audio Header:
	//[I]   channels = 2
	//[I]   bits = 16
	//[I]   rate = 48000
	//[I]   frame size = 480
	//[I]   unknown = 1
	want.freq = rate;
	want.format = AUDIO_S16SYS;
	// 2 == stereo
	want.channels = channels;
	want.samples = 1024;
	want.callback = NULL;

	if(this->sdl_audio_device_id <= 0)
	{
		// the chiaki session might be called many times
		// open the audio device only once
		this->sdl_audio_device_id = SDL_OpenAudioDevice(NULL, 0, &want, NULL, 0);
	}

	if(this->sdl_audio_device_id <= 0)
	{
		CHIAKI_LOGE(this->log, "SDL_OpenAudioDevice failed: %s\n", SDL_GetError());
	}
	else
	{
		SDL_PauseAudioDevice(this->sdl_audio_device_id, 0);
	}
}

void IO::AudioCB(int16_t *buf, size_t samples_count)
{
	for(int x = 0; x < samples_count * 2; x++)
	{
		// boost audio volume
		int sample = buf[x] * 1.80;
		// Hard clipping (audio compression)
		// truncate value that overflow/underflow int16
		if(sample > INT16_MAX)
		{
			buf[x] = INT16_MAX;
			CHIAKI_LOGD(this->log, "Audio Hard clipping INT16_MAX < %d", sample);
		}
		else if(sample < INT16_MIN)
		{
			buf[x] = INT16_MIN;
			CHIAKI_LOGD(this->log, "Audio Hard clipping INT16_MIN > %d", sample);
		}
		else
			buf[x] = (int16_t)sample;
	}

	int audio_queued_size = SDL_GetQueuedAudioSize(this->sdl_audio_device_id);
	// Hard-clears the whole queue (an audible pop every time it fires) once
	// backlog gets excessive, to bound worst-case audio delay during a real
	// stall/catch-up. The original 16000-byte threshold left only ~15ms of
	// margin over the ~13000 bytes (~68ms) this queue normally sits at
	// (48kHz/16-bit/stereo), so on Cloud Play - where measured RTT swings
	// from ~10ms to 200ms+ well within normal operation - routine jitter
	// alone crossed it constantly, producing frequent audible crackles that
	// had nothing to do with actual network loss. 96000 bytes (~500ms) still
	// bounds runaway latency during a genuine stall, but gives enough
	// headroom that normal jitter on this link doesn't trigger it.
	if(audio_queued_size > 96000)
	{
		CHIAKI_LOGW(this->log, "Triggering SDL_ClearQueuedAudio with queue size = %d", audio_queued_size);
		SDL_ClearQueuedAudio(this->sdl_audio_device_id);
	}

	int success = SDL_QueueAudio(this->sdl_audio_device_id, buf, sizeof(int16_t) * samples_count * 2);
	if(success != 0)
		CHIAKI_LOGE(this->log, "SDL_QueueAudio failed: %s\n", SDL_GetError());

	// check haptic
	if (haptic_lock) {
		CleanUpHaptic();
	}
}

bool IO::InitVideo(int video_width, int video_height, int screen_width, int screen_height)
{
	CHIAKI_LOGI(this->log, "load InitVideo");
	// Whichever thread calls InitVideo (in practice, Borealis's main/render
	// thread) - registered here since that thread has no dedicated entry
	// point of its own to hook into. Self-reported from MainLoop() below,
	// which is this thread's own natural per-frame work-item boundary.
	this->cpu_sample_main_idx = chiaki_switch_register_thread_for_sampling("Main/Render (InitVideo caller)");
	this->video_width = video_width;
	this->video_height = video_height;

	this->screen_width = screen_width;
	this->screen_height = screen_height;
	this->frames = (AVFrame**)malloc(MAX_FRAME_COUNT * sizeof(AVFrame*));

	for (int i = 0; i < MAX_FRAME_COUNT; i++) {
			frames[i] = av_frame_alloc();
			if (frames[i] == NULL) {
					CHIAKI_LOGE(this->log, "FFmpeg: Couldn't allocate frame");
					return -1;
			}
			frames[i]->format = AV_PIX_FMT_NV12;
			frames[i]->width  = video_width;
			frames[i]->height = video_height;

			int err = av_frame_get_buffer(frames[i], 256);
			if (err < 0) {
					CHIAKI_LOGE(this->log, "FFmpeg: Couldn't allocate frame buffer:");
					return -1;
			}
			for (int j = 0; j < MAX_NV12_PLANE_COUNT; j++) {
				uintptr_t ptr = (uintptr_t)frames[i]->data[j];
				// store origin address for releasing
				origin_ptr[i][j] = ptr;
				uintptr_t dst = (((ptr)+(256)-1)&~((256)-1));
				uintptr_t gap = dst - ptr;
				frames[i]->data[j] += gap;
			}
			CHIAKI_LOGE(this->log, "FFmpeg: allocated address: %d %d, linesize 0: %d", (uintptr_t)frames[i]->data[0], (uintptr_t)frames[i]->data[1], frames[i]->linesize[0]);
	}
  this->tmp_frame = av_frame_alloc();

	if(!InitOpenGl())
	{
		throw Exception("Failed to initiate OpenGl");
	}

	this->video_decode_thread_running = true;
	this->video_decode_thread = std::thread(&IO::VideoDecodeThreadFunc, this);

	// Keep diagnostic sampling compiled in but disabled in normal builds. Its
	// once-per-second stdout traffic is useful while profiling, not streaming.
	this->cpu_sample_thread_running = false;

	return true;
}

bool IO::FreeVideo()
{
	bool ret = true;

	if(this->cpu_sample_thread.joinable())
	{
		this->cpu_sample_thread_running = false;
		this->cpu_sample_thread.join();
	}

	if(this->video_decode_thread.joinable())
	{
		{
			std::lock_guard<std::mutex> lock(this->video_decode_mutex);
			this->video_decode_thread_running = false;
		}
		this->video_decode_cv.notify_one();
		this->video_decode_thread.join();
	}
	this->video_decode_queue.clear();

	if (this->hw_device_ctx) {
			av_buffer_unref(&this->hw_device_ctx);
	}

	if (this->frames != NULL) {
		for (int i = 0; i < MAX_FRAME_COUNT; i++) {
			if(this->frames[i]) {
				for (int j = 0; j < MAX_NV12_PLANE_COUNT; j++) {
					// resume origin pointer address
					this->frames[i]->data[j] = (uint8_t*) origin_ptr[i][j];
				}
				av_frame_free(&this->frames[i]);
			}
		}
		free(this->frames); // allocted via malloc
		this->frames = nullptr;
	}

	if(this->tmp_frame)
		av_frame_free(&this->tmp_frame);

	// avcodec_alloc_context3(codec);
	if(this->codec_context)
	{
		avcodec_free_context(&this->codec_context);
	}

	this->isFirst = true;
	current_frame.store(0);
	next_frame = 0;
	this->decoded_frame_generation.store(0);
	this->rendered_frame_generation = 0;

	return ret;
}

bool IO::ReadGameTouchScreen(ChiakiControllerState *chiaki_state, std::map<uint32_t, int8_t> *finger_id_touch_id)
{
#ifdef __SWITCH__
	HidTouchScreenState sw_state = {0};

	bool ret = false;
	hidGetTouchScreenStates(&sw_state, 1);
	// scale switch screen to the PS trackpad
	chiaki_state->buttons &= ~CHIAKI_CONTROLLER_BUTTON_TOUCHPAD; // touchscreen release

	// un-touch all old touches
	for(auto it = finger_id_touch_id->begin(); it != finger_id_touch_id->end();)
	{
		auto cur = it;
		it++;
		for(int i = 0; i < sw_state.count; i++)
		{
			if(sw_state.touches[i].finger_id == cur->first)
				goto cont;
		}
		if(cur->second >= 0)
			chiaki_controller_state_stop_touch(chiaki_state, (uint8_t)cur->second);
		finger_id_touch_id->erase(cur);
cont:
		continue;
	}


	// touch or update all current touches
	for(int i = 0; i < sw_state.count; i++)
	{
		uint16_t x = sw_state.touches[i].x * ((float)DS4_TRACKPAD_MAX_X / (float)SWITCH_TOUCHSCREEN_MAX_X);
		uint16_t y = sw_state.touches[i].y * ((float)DS4_TRACKPAD_MAX_Y / (float)SWITCH_TOUCHSCREEN_MAX_Y);
		// use nintendo switch border's 5% to trigger the touchpad button
		if(x <= (DS4_TRACKPAD_MAX_X * 0.05) || x >= (DS4_TRACKPAD_MAX_X * 0.95) || y <= (DS4_TRACKPAD_MAX_Y * 0.05) || y >= (DS4_TRACKPAD_MAX_Y * 0.95))
			chiaki_state->buttons |= CHIAKI_CONTROLLER_BUTTON_TOUCHPAD; // touchscreen

		auto it = finger_id_touch_id->find(sw_state.touches[i].finger_id);
		if(it == finger_id_touch_id->end())
		{
			// new touch
			(*finger_id_touch_id)[sw_state.touches[i].finger_id] =
				chiaki_controller_state_start_touch(chiaki_state, x, y);
		}
		else if(it->second >= 0)
			chiaki_controller_state_set_touch_pos(chiaki_state, (uint8_t)it->second, x, y);
		// it->second < 0 ==> touch ignored because there were already too many multi-touches
		ret = true;
	}
	return ret;
#else
	return false;
#endif
}

void IO::SetRumble(uint8_t left, uint8_t right)
{
#ifdef __SWITCH__
	Result rc = 0;
	HidVibrationValue vibration_values[] = {
		{
			.amp_low = 0.0f,
			.freq_low = 160.0f,
			.amp_high = 0.0f,
			.freq_high = 200.0f,
		},
		{
			.amp_low = 0.0f,
			.freq_low = 160.0f,
			.amp_high = 0.0f,
			.freq_high = 200.0f,
		}};

	int target_device = padIsHandheld(&pad) ? 0 : 1;
	if(left > 160) left = 160;
	if(left > 0)
	{
		// SDL_HapticRumblePlay(this->sdl_haptic_ptr[0], left / 100, 5000);
		float l = (float)left / 255.0;
		vibration_values[0].amp_low = l;
		vibration_values[0].freq_low *= l;
		vibration_values[0].amp_high = l;
		vibration_values[0].freq_high *= l;
	}

	if(right > 160) right = 160;
	if(right > 0)
	{
		// SDL_HapticRumblePlay(this->sdl_haptic_ptr[1], right / 100, 5000);
		float r = (float)right / 255.0;
		vibration_values[1].amp_low = r;
		vibration_values[1].freq_low *= r;
		vibration_values[1].amp_high = r;
		vibration_values[1].freq_high *= r;
	}

	rc = hidSendVibrationValues(this->vibration_handles[target_device], vibration_values, 2);
	if(R_FAILED(rc))
		CHIAKI_LOGE(this->log, "hidSendVibrationValues() returned: 0x%x", rc);

#endif
}

void IO::HapticCB(uint8_t *buf, size_t buf_size) {
		int16_t amplitudel = 0, amplituder = 0;
		int32_t suml = 0, sumr = 0;
		const size_t sample_size = 2 * sizeof(int16_t); // stereo samples

		size_t buf_count = buf_size / sample_size;
		for (size_t i = 0; i < buf_count; i++){
			size_t cur = i * sample_size;

			memcpy(&amplitudel, buf + cur, sizeof(int16_t));
			memcpy(&amplituder, buf + cur + sizeof(int16_t), sizeof(int16_t));
			suml += amplitudel;
			sumr += amplituder;
		}
		uint16_t left = 0, right = 0;
		left = suml / buf_count;
		right = sumr / buf_count;
		SetHapticRumble(left, right);
		if ((left != 0 || right != 0) && !haptic_lock) {
			haptic_lock = true;
		}
}

void IO::SetHapticRumble(uint8_t left, uint8_t right)
{
	uint8_t val = left > right ? left : right;
	haptic_val = val;
	haptic_lock_time = std::chrono::high_resolution_clock::now(); 
	
#ifdef __SWITCH__
	Result rc = 0;
	HidVibrationValue vibration_values[] = {
		{
			.amp_low = 0.0f,
			.freq_low = 160.0f,
			.amp_high = 0.0f,
			.freq_high = 200.0f,
		},
		{
			.amp_low = 0.0f,
			.freq_low = 160.0f,
			.amp_high = 0.0f,
			.freq_high = 200.0f,
		}};

	int target_device = padIsHandheld(&pad) ? 0 : 1;
	for (int i = 0; i < 2; i++) {
		float index = (float)val / (float)HapticBase;
		vibration_values[i].amp_low = index;
		vibration_values[i].amp_high = index;
		if (val != 0) {
			vibration_values[i].freq_low *= index;
			vibration_values[i].freq_high *= index;
		}
	}
	// CHIAKI_LOGW(this->log, "haptic rumble param: %f %f %f %f",
	// 	vibration_values[0].amp_low, vibration_values[0].amp_high,
	// 	vibration_values[0].freq_low, vibration_values[0].freq_high);
	
	rc = hidSendVibrationValues(this->vibration_handles[target_device], vibration_values, 2);
#endif
}

void IO::CleanUpHaptic() {
	std::chrono::system_clock::time_point now = std::chrono::high_resolution_clock::now();
	auto dur = now - haptic_lock_time;
	auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(dur).count();
	if (haptic_val == 0) {
		haptic_lock = false;
	} else if (ms > 30) {
		SetHapticRumble(0, 0);
		haptic_lock = false;
	}
}

bool IO::ReadGameSixAxis(ChiakiControllerState *state)
{
#ifdef __SWITCH__
	// Read from the correct sixaxis handle depending on the current input style
	HidSixAxisSensorState sixaxis = {0};
	uint64_t style_set = padGetStyleSet(&pad);
	if(style_set & HidNpadStyleTag_NpadHandheld)
		hidGetSixAxisSensorStates(this->sixaxis_handles[0], &sixaxis, 1);
	else if(style_set & HidNpadStyleTag_NpadFullKey)
		hidGetSixAxisSensorStates(this->sixaxis_handles[1], &sixaxis, 1);
	else if(style_set & HidNpadStyleTag_NpadJoyDual)
	{
		// For JoyDual, read from either the Left or Right Joy-Con depending on which is/are connected
		u64 attrib = padGetAttributes(&pad);
		if(attrib & HidNpadAttribute_IsLeftConnected)
			hidGetSixAxisSensorStates(this->sixaxis_handles[2], &sixaxis, 1);
		else if(attrib & HidNpadAttribute_IsRightConnected)
			hidGetSixAxisSensorStates(this->sixaxis_handles[3], &sixaxis, 1);
	}

	state->gyro_x = sixaxis.angular_velocity.x * 2.0f * M_PI;
	state->gyro_y = sixaxis.angular_velocity.z * 2.0f * M_PI;
	state->gyro_z = -sixaxis.angular_velocity.y * 2.0f * M_PI;
	state->accel_x = -sixaxis.acceleration.x;
	state->accel_y = -sixaxis.acceleration.z;
	state->accel_z = sixaxis.acceleration.y;

	// https://d3cw3dd2w32x2b.cloudfront.net/wp-content/uploads/2015/01/matrix-to-quat.pdf
	float (*dm)[3] = sixaxis.direction.direction;
	float m[3][3] = {
		{ dm[0][0], dm[2][0], dm[1][0] },
		{ dm[0][2], dm[2][2], dm[1][2] },
		{ dm[0][1], dm[2][1], dm[1][1] }
	};
	std::array<float, 4> q;
	float t;
	if(m[2][2] < 0)
	{
		if (m[0][0] > m[1][1])
		{
			t = 1 + m[0][0] - m[1][1] - m[2][2];
			q = { t, m[0][1] + m[1][0], m[2][0] + m[0][2], m[1][2] - m[2][1] };
		}
		else
		{
			t = 1 - m[0][0] + m[1][1] -m[2][2];
			q = { m[0][1] + m[1][0], t, m[1][2] + m[2][1], m[2][0] - m[0][2] };
		}
	}
	else
	{
		if(m[0][0] < -m[1][1])
		{
			t = 1 - m[0][0] - m[1][1] + m[2][2];
			q = { m[2][0] + m[0][2], m[1][2] + m[2][1], t, m[0][1] - m[1][0] };
		}
		else
		{
			t = 1 + m[0][0] + m[1][1] + m[2][2];
			q = { m[1][2] - m[2][1], m[2][0] - m[0][2], m[0][1] - m[1][0], t };
		}
	}
	float fac = 0.5f / sqrt(t);
	state->orient_x = q[0] * fac;
	state->orient_y = q[1] * fac;
	state->orient_z = -q[2] * fac;
	state->orient_w = q[3] * fac;
	return true;
#else
	return false;
#endif
}

bool IO::ReadGameKeys(SDL_Event *event, ChiakiControllerState *state)
{
	// return true if an event changed (gamepad input)

	// TODO
	// share vs PS button
	bool ret = true;
	switch(event->type)
	{
		case SDL_JOYAXISMOTION:
			// printf("SDL_JOYAXISMOTION jaxis %d axis %d value %d\n",
			// event->jaxis.which, event->jaxis.axis, event->jaxis.value);
			if(event->jaxis.which == 0)
			{
				// left joystick
				if(event->jaxis.axis == 0)
					// Left-right movement
					state->left_x = event->jaxis.value;
				else if(event->jaxis.axis == 1)
					// Up-Down movement
					state->left_y = event->jaxis.value;
				else if(event->jaxis.axis == 2)
					// Left-right movement
					state->right_x = event->jaxis.value;
				else if(event->jaxis.axis == 3)
					// Up-Down movement
					state->right_y = event->jaxis.value;
				else
					ret = false;
			}
			else if(event->jaxis.which == 1)
			{
				// right joystick
				if(event->jaxis.axis == 0)
					// Left-right movement
					state->right_x = event->jaxis.value;
				else if(event->jaxis.axis == 1)
					// Up-Down movement
					state->right_y = event->jaxis.value;
				else
					ret = false;
			}
			else
				ret = false;
			break;
		case SDL_JOYBUTTONDOWN:
			// printf("Joystick %d button %d DOWN\n",
			// 	event->jbutton.which, event->jbutton.button);
			switch(event->jbutton.button)
			{
				case 0:
					state->buttons |= CHIAKI_CONTROLLER_BUTTON_MOON;
					break; // KEY_A
				case 1:
					state->buttons |= CHIAKI_CONTROLLER_BUTTON_CROSS;
					break; // KEY_B
				case 2:
					state->buttons |= CHIAKI_CONTROLLER_BUTTON_PYRAMID;
					break; // KEY_X
				case 3:
					state->buttons |= CHIAKI_CONTROLLER_BUTTON_BOX;
					break; // KEY_Y
				case 12:
					state->buttons |= CHIAKI_CONTROLLER_BUTTON_DPAD_LEFT;
					break; // KEY_DLEFT
				case 14:
					state->buttons |= CHIAKI_CONTROLLER_BUTTON_DPAD_RIGHT;
					break; // KEY_DRIGHT
				case 13:
					state->buttons |= CHIAKI_CONTROLLER_BUTTON_DPAD_UP;
					break; // KEY_DUP
				case 15:
					state->buttons |= CHIAKI_CONTROLLER_BUTTON_DPAD_DOWN;
					break; // KEY_DDOWN
				case 6:
					state->buttons |= CHIAKI_CONTROLLER_BUTTON_L1;
					break; // KEY_L
				case 7:
					state->buttons |= CHIAKI_CONTROLLER_BUTTON_R1;
					break; // KEY_R
				case 8:
					state->l2_state = 0xff;
					break; // KEY_ZL
				case 9:
					state->r2_state = 0xff;
					break; // KEY_ZR
				case 4:
					state->buttons |= CHIAKI_CONTROLLER_BUTTON_L3;
					break; // KEY_LSTICK
				case 5:
					state->buttons |= CHIAKI_CONTROLLER_BUTTON_R3;
					break; // KEY_RSTICK
				case 10:
					state->buttons |= CHIAKI_CONTROLLER_BUTTON_OPTIONS;
					break; // KEY_PLUS
				case 11:
					state->buttons |= CHIAKI_CONTROLLER_BUTTON_SHARE;
					break; // KEY_MINUS
				default:
					ret = false;
			}
			break;
		case SDL_JOYBUTTONUP:
			// printf("Joystick %d button %d UP\n",
			// 	event->jbutton.which, event->jbutton.button);
			switch(event->jbutton.button)
			{
				case 0:
					state->buttons &= ~CHIAKI_CONTROLLER_BUTTON_MOON;
					break; // KEY_A
				case 1:
					state->buttons &= ~CHIAKI_CONTROLLER_BUTTON_CROSS;
					break; // KEY_B
				case 2:
					state->buttons &= ~CHIAKI_CONTROLLER_BUTTON_PYRAMID;
					break; // KEY_X
				case 3:
					state->buttons &= ~CHIAKI_CONTROLLER_BUTTON_BOX;
					break; // KEY_Y
				case 12:
					state->buttons ^= CHIAKI_CONTROLLER_BUTTON_DPAD_LEFT;
					break; // KEY_DLEFT
				case 14:
					state->buttons ^= CHIAKI_CONTROLLER_BUTTON_DPAD_RIGHT;
					break; // KEY_DRIGHT
				case 13:
					state->buttons ^= CHIAKI_CONTROLLER_BUTTON_DPAD_UP;
					break; // KEY_DUP
				case 15:
					state->buttons ^= CHIAKI_CONTROLLER_BUTTON_DPAD_DOWN;
					break; // KEY_DDOWN
				case 6:
					state->buttons ^= CHIAKI_CONTROLLER_BUTTON_L1;
					break; // KEY_L
				case 7:
					state->buttons ^= CHIAKI_CONTROLLER_BUTTON_R1;
					break; // KEY_R
				case 8:
					state->l2_state = 0x00;
					break; // KEY_ZL
				case 9:
					state->r2_state = 0x00;
					break; // KEY_ZR
				case 4:
					state->buttons ^= CHIAKI_CONTROLLER_BUTTON_L3;
					break; // KEY_LSTICK
				case 5:
					state->buttons ^= CHIAKI_CONTROLLER_BUTTON_R3;
					break; // KEY_RSTICK
				case 10:
					state->buttons &= ~CHIAKI_CONTROLLER_BUTTON_OPTIONS;
					break; // KEY_PLUS
				case 11:
					state->buttons &= ~CHIAKI_CONTROLLER_BUTTON_SHARE;
					break; // KEY_MINUS
				default:
					ret = false;
			}
			break;
		default:
			ret = false;
	}

	return ret;
}

bool IO::InitAVCodec(bool is_PS5)
{
	CHIAKI_LOGI(this->log, "loading AVCodec");
	// set libav video context
	if (is_PS5) {
		this->codec = avcodec_find_decoder_by_name("hevc");
	} else {
		this->codec = avcodec_find_decoder_by_name("h264");
	}
	if(!this->codec)
		throw Exception("H265 Codec not available");
	CHIAKI_LOGI(this->log, "get codec %s", this->codec->name);

	this->codec_context = avcodec_alloc_context3(codec);
	if(!this->codec_context)
		throw Exception("Failed to alloc codec context");

	if (enableHWAccl) {
		this->codec_context->flags |= AV_CODEC_FLAG_LOW_DELAY;
		if(is_PS5)
		{
			this->codec_context->skip_loop_filter = AVDISCARD_ALL;
			this->codec_context->flags2 |= AV_CODEC_FLAG2_FAST;
		}
		else
		{
			// H.264 block damage is much more visible with deblocking disabled,
			// especially after a failed FEC recovery. NVDEC performs the filter
			// in hardware, so retain quality/error resilience without a CPU cost.
			this->codec_context->skip_loop_filter = AVDISCARD_DEFAULT;
			this->codec_context->err_recognition = AV_EF_CAREFUL;
		}

		// Both of these must happen BEFORE avcodec_open2(), not after: the
		// decoder decides whether it can attach a hwaccel (ff_hevc_nvtegra_
		// hwaccel / ff_h264_nvtegra_hwaccel - both genuinely compiled into
		// this toolchain's libavcodec.a) during its own init, which runs
		// inside avcodec_open2() itself. hw_device_ctx used to be attached
		// AFTER avcodec_open2() had already returned, and get_format was
		// never set at all - between the two, the decoder had no way to
		// ever select AV_PIX_FMT_NVTEGRA, so despite hw_device_ctx being
		// created "successfully", every frame was silently decoded in
		// software the entire time (auto-threaded across every core - the
		// real explanation for ~100% CPU on 3/4 cores continuously, not just
		// during motion). get_format is the standard FFmpeg mechanism a
		// decoder uses to ask which of several offered pixel formats to
		// decode into; returning AV_PIX_FMT_NVTEGRA when it's offered is
		// what actually makes the codec pick up the linked-in hwaccel.
		if(av_hwdevice_ctx_create(&this->hw_device_ctx, AV_HWDEVICE_TYPE_NVTEGRA, NULL, NULL, 0) < 0) {
			throw Exception("Failed to enable hardware encoding");
		}
		this->codec_context->hw_device_ctx = av_buffer_ref(this->hw_device_ctx);
		this->codec_context->get_format = [](AVCodecContext *ctx, const enum AVPixelFormat *pix_fmts) -> AVPixelFormat {
			for(const enum AVPixelFormat *p = pix_fmts; *p != AV_PIX_FMT_NONE; p++)
			{
				if(*p == AV_PIX_FMT_NVTEGRA)
					return *p;
			}
			return avcodec_default_get_format(ctx, pix_fmts);
		};
	} else {
		// use rock88's mooxlight-nx optimization
		// https://github.com/rock88/moonlight-nx/blob/698d138b9fdd4e483c998254484ccfb4ec829e95/src/streaming/ffmpeg/FFmpegVideoDecoder.cpp#L63
		// this->codec_context->skip_loop_filter = AVDISCARD_ALL;
		this->codec_context->flags |= AV_CODEC_FLAG_LOW_DELAY;
		this->codec_context->flags2 |= AV_CODEC_FLAG2_FAST;
		// this->codec_context->flags2 |= AV_CODEC_FLAG2_CHUNKS;
		this->codec_context->thread_type = FF_THREAD_SLICE;
		this->codec_context->thread_count = 4;
	}

	if(avcodec_open2(this->codec_context, this->codec, nullptr) < 0)
	{
		avcodec_free_context(&this->codec_context);
		throw Exception("Failed to open codec context");
	}
	return true;
}

bool IO::InitOpenGl()
{
	CHIAKI_LOGI(this->log, "loading OpenGL");
	isFirst = true;

	if(!InitOpenGlShader())
		return false;
	
	if (enableHWAccl) {
		if(!InitOpenGlTX1Textures()) {
			return false;
		}
	} else {
		if(!InitOpenGlTextures()) {
			return false;
		}
	}


	return true;
}

bool IO::InitOpenGlTextures()
{
	CHIAKI_LOGV(this->log, "loading OpenGL textrures");

	D(glGenTextures(PLANES_COUNT, this->tex));
	D(glGenBuffers(PLANES_COUNT, this->pbo));
	uint8_t uv_default[] = {0x7f, 0x7f};
	for(int i = 0; i < PLANES_COUNT; i++)
	{
		D(glBindTexture(GL_TEXTURE_2D, this->tex[i]));
		D(glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR));
		D(glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR));
		D(glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE));
		D(glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE));
		D(glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, 1, 1, 0, GL_RED, GL_UNSIGNED_BYTE, i > 0 ? uv_default : nullptr));
	}

	D(glUseProgram(this->prog));
	// bind only as many planes as we need
	const char *plane_names[] = {"plane1", "plane2", "plane3"};
	for(int i = 0; i < PLANES_COUNT; i++)
		D(glUniform1i(glGetUniformLocation(this->prog, plane_names[i]), i));

	D(glGenVertexArrays(1, &this->vao));
	D(glBindVertexArray(this->vao));

	D(glGenBuffers(1, &this->vbo));
	D(glBindBuffer(GL_ARRAY_BUFFER, this->vbo));
	D(glBufferData(GL_ARRAY_BUFFER, sizeof(vert_pos), vert_pos, GL_STATIC_DRAW));

	D(glBindBuffer(GL_ARRAY_BUFFER, this->vbo));
	D(glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr));
	D(glEnableVertexAttribArray(0));

	D(glCullFace(GL_BACK));
	D(glEnable(GL_CULL_FACE));
	D(glClearColor(0.5, 0.5, 0.5, 1.0));
	return true;
}

bool IO::InitOpenGlTX1Textures()
{
	CHIAKI_LOGV(this->log, "loading OpenGL TX1 textrures");

	int planes[][5] = {
		// { width_divide, height_divider, data_per_pixel }
			{ 1, 1, 1, GL_R8, GL_RED },
			{ 2, 2, 2, GL_RG8, GL_RG }
	};

	D(glGenTextures(2, this->tex));
	D(glGenBuffers(2, this->pbo));
	uint8_t uv_default[] = {0x7f, 0x7f};
	for(int i = 0; i < MAX_NV12_PLANE_COUNT; i++)
	{
		D(glBindTexture(GL_TEXTURE_2D, this->tex[i]));
		D(glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR));
		D(glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR));
		D(glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE));
		D(glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE));
		D(glTexImage2D(GL_TEXTURE_2D, 0, planes[i][3], 1, 1, 0, planes[i][4], GL_UNSIGNED_BYTE, i > 0 ? uv_default : nullptr));
	}

	D(glUseProgram(this->prog));
	// bind only as many planes as we need
	const char *plane_names[] = {"plane1", "plane2", "plane3"};
	for(int i = 0; i < MAX_NV12_PLANE_COUNT; i++) {
		m_texture_uniform[i] = glGetUniformLocation(this->prog, plane_names[i]);
		D(glUniform1i(m_texture_uniform[i], i));
	}

	D(glGenVertexArrays(1, &this->vao));
	D(glBindVertexArray(this->vao));

	D(glGenBuffers(1, &this->vbo));
	D(glBindBuffer(GL_ARRAY_BUFFER, this->vbo));
	D(glBufferData(GL_ARRAY_BUFFER, sizeof(vert_pos), vert_pos, GL_STATIC_DRAW));

	D(glBindBuffer(GL_ARRAY_BUFFER, this->vbo));
	D(glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr));
	D(glEnableVertexAttribArray(0));

	D(glCullFace(GL_BACK));
	D(glEnable(GL_CULL_FACE));
	return true;
}

GLuint IO::CreateAndCompileShader(GLenum type, const char *source)
{
	GLint success;
	GLchar msg[512];

	GLuint handle;
	D(handle = glCreateShader(type));
	if(!handle)
	{
		CHIAKI_LOGE(this->log, "%u: cannot create shader", type);
		DP(this->prog);
	}

	D(glShaderSource(handle, 1, &source, nullptr));
	D(glCompileShader(handle));
	D(glGetShaderiv(handle, GL_COMPILE_STATUS, &success));

	if(!success)
	{
		D(glGetShaderInfoLog(handle, sizeof(msg), nullptr, msg));
		CHIAKI_LOGE(this->log, "%u: %s\n", type, msg);
		D(glDeleteShader(handle));
	}

	return handle;
}

bool IO::InitOpenGlShader()
{
	CHIAKI_LOGV(this->log, "loading OpenGl Shaders");

	D(this->vert = CreateAndCompileShader(GL_VERTEX_SHADER, shader_vert_glsl));
	if (enableHWAccl) {
		D(this->frag = CreateAndCompileShader(GL_FRAGMENT_SHADER, nv12_shader_frag_glsl));
	} else {
		D(this->frag = CreateAndCompileShader(GL_FRAGMENT_SHADER, yuv420p_shader_frag_glsl));
	}

	D(this->prog = glCreateProgram());

	D(glAttachShader(this->prog, this->vert));
	D(glAttachShader(this->prog, this->frag));
	D(glBindAttribLocation(this->prog, 0, "pos_attr"));
	D(glLinkProgram(this->prog));

	GLint success;
	D(glGetProgramiv(this->prog, GL_LINK_STATUS, &success));
	if(!success)
	{
		char buf[512];
		glGetProgramInfoLog(this->prog, sizeof(buf), nullptr, buf);
		CHIAKI_LOGE(this->log, "OpenGL link error: %s", buf);
		return false;
	}

	D(glDeleteShader(this->vert));
	D(glDeleteShader(this->frag));

	D(this->sharpen_uniform = glGetUniformLocation(this->prog, "u_sharpen"));
	D(this->texel_size_uniform = glGetUniformLocation(this->prog, "u_texel_size"));

	return true;
}

inline void IO::SetOpenGlYUVPixels(AVFrame *frame)
{
	D(glUseProgram(this->prog));
	if(this->texel_size_uniform >= 0)
		D(glUniform2f(this->texel_size_uniform, 1.0f / frame->width, 1.0f / frame->height));
	if(this->sharpen_uniform >= 0)
		D(glUniform1f(this->sharpen_uniform, this->sharpen_amount));

	int planes[][3] = {
		// { width_divide, height_divider, data_per_pixel }
		{1, 1, 1}, // Y
		{2, 2, 1}, // U
		{2, 2, 1}  // V
	};

	for(int i = 0; i < PLANES_COUNT; i++)
	{
		int width = frame->width / planes[i][0];
		int height = frame->height / planes[i][1];
		int size = width * height * planes[i][2];
		uint8_t *buf;

		D(glBindBuffer(GL_PIXEL_UNPACK_BUFFER, this->pbo[i]));
		D(glBufferData(GL_PIXEL_UNPACK_BUFFER, size, nullptr, GL_STREAM_DRAW));
		D(buf = reinterpret_cast<uint8_t *>(glMapBufferRange(GL_PIXEL_UNPACK_BUFFER, 0, size, GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT)));
		if(!buf)
		{
			GLint data;
			D(glGetBufferParameteriv(GL_PIXEL_UNPACK_BUFFER, GL_BUFFER_SIZE, &data));
			CHIAKI_LOGE(this->log, "AVOpenGLFrame failed to map PBO");
			CHIAKI_LOGE(this->log, "Info buf == %p. size %d frame %d * %d, divs %d, %d, pbo %d GL_BUFFER_SIZE %x",
				buf, size, frame->width, frame->height, planes[i][0], planes[i][1], pbo[i], data);
			continue;
		}

		if(frame->linesize[i] == width)
		{
			// Y
			memcpy(buf, frame->data[i], size);
		}
		else
		{
			// UV
			for(int l = 0; l < height; l++)
				memcpy(buf + width * l * planes[i][2],
					frame->data[i] + frame->linesize[i] * l,
					width * planes[i][2]);
		}
		D(glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER));
		D(glBindTexture(GL_TEXTURE_2D, tex[i]));
		D(glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, width, height, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr));
		glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
	}
	glFinish();
}
inline void IO::SetOpenGlNV12Pixels(AVFrame *frame)
{
	D(glUseProgram(this->prog));
	if(this->texel_size_uniform >= 0)
		D(glUniform2f(this->texel_size_uniform, 1.0f / frame->width, 1.0f / frame->height));
	if(this->sharpen_uniform >= 0)
		D(glUniform1f(this->sharpen_uniform, this->sharpen_amount));

	int planes[][5] = {
		// { width_divide, height_divider, data_per_pixel }
			{ 1, 1, 1, GL_R8, GL_RED },
			{ 2, 2, 2, GL_RG8, GL_RG }
	};

	for (int i = 0; i < MAX_NV12_PLANE_COUNT; i++) {
		glActiveTexture(GL_TEXTURE0 + i);
		int real_width = frame->linesize[i] / planes[i][0];
		int width = frame->width / planes[i][0];
		int height = frame->height / planes[i][1];
		D(glBindTexture(GL_TEXTURE_2D, tex[i]));
		glPixelStorei(GL_UNPACK_ROW_LENGTH, real_width);
		if (isFirst) {
			CHIAKI_LOGI(this->log, "[NV12 PROBE] plane=%d frame->width=%d frame->height=%d frame->linesize=%d real_width=%d tex_width=%d tex_height=%d video_width=%d video_height=%d screen_width=%d screen_height=%d",
				i, frame->width, frame->height, frame->linesize[i], real_width, width, height, this->video_width, this->video_height, this->screen_width, this->screen_height);
			D(glTexImage2D(GL_TEXTURE_2D, 0, planes[i][3], width, height, 0, planes[i][4], GL_UNSIGNED_BYTE, frame->data[i]));
		} else {
			glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width,
											height, planes[i][4], GL_UNSIGNED_BYTE, frame->data[i]);
		}
		glUniform1i(m_texture_uniform[i], i);
		D(glBindTexture(GL_TEXTURE_2D, 0));
	}

	isFirst = false;
}

inline void IO::OpenGlDraw()
{
	glClear(GL_COLOR_BUFFER_BIT);

	// Upload only when the decode thread has published a new frame. The GPU
	// texture retains the previous image between arrivals, so drawing it again
	// is sufficient and avoids repeatedly transferring the same 1080p NV12
	// planes. Smooth pacing additionally waits for the detected source cadence.
	uint64_t decoded_generation = this->decoded_frame_generation.load(std::memory_order_acquire);
	bool due = decoded_generation != this->rendered_frame_generation;
	if(this->video_pacing_smooth)
	{
		this->pacing_phase++;
		bool cadence_due = this->pacing_phase >= this->pacing_source_refresh_period;
		due = due && cadence_due;
		if(cadence_due)
			this->pacing_phase = 0;
	}

	if(due)
	{
		int frame_index = current_frame.load(std::memory_order_acquire);
		if (enableHWAccl) {
			SetOpenGlNV12Pixels(this->frames[frame_index]);
		} else {
			// send to OpenGl
			SetOpenGlYUVPixels(this->frames[frame_index]);
		}
		this->rendered_frame_generation = decoded_generation;
	}

	//avcodec_flush_buffers(this->codec_context);
	D(glBindVertexArray(this->vao));

	for(int i = 0; i < PLANES_COUNT; i++)
	{
		D(glActiveTexture(GL_TEXTURE0 + i));
		D(glBindTexture(GL_TEXTURE_2D, this->tex[i]));
	}

	D(glDrawArrays(GL_TRIANGLE_STRIP, 0, 4));
	D(glBindVertexArray(0));
	// Borealis calls glfwSwapBuffers() immediately after this frame. Forcing a
	// full finish here made the CPU busy-wait for the GPU before that swap.
	D(glFlush());
}

bool IO::InitController()
{
	// https://github.com/switchbrew/switch-examples/blob/master/graphics/sdl2/sdl2-simple/source/main.cpp#L57
	// open CONTROLLER_PLAYER_1 and CONTROLLER_PLAYER_2
	// when railed, both joycons are mapped to joystick #0,
	// else joycons are individually mapped to joystick #0, joystick #1, ...
	for(int i = 0; i < SDL_JOYSTICK_COUNT; i++)
	{
		this->sdl_joystick_ptr[i] = SDL_JoystickOpen(i);
		if(sdl_joystick_ptr[i] == nullptr)
		{
			CHIAKI_LOGE(this->log, "SDL_JoystickOpen: %s\n", SDL_GetError());
			return false;
		}
		// this->sdl_haptic_ptr[i] = SDL_HapticOpenFromJoystick(sdl_joystick_ptr[i]);
		// if(sdl_haptic_ptr[i] == nullptr)
		// {
		// 	CHIAKI_LOGE(this->log, "SDL_HapticRumbleInit: %s\n", SDL_GetError());
		// } else {
		// 	SDL_HapticRumbleInit(this->sdl_haptic_ptr[i]);
		// }
	}
#ifdef __SWITCH__
Result rc = 0;
	// Configure our supported input layout: a single player with standard controller styles
	padConfigureInput(1, HidNpadStyleSet_NpadStandard);

	// Initialize the default gamepad (which reads handheld mode inputs as well as the first connected controller)
	padInitializeDefault(&this->pad);
	// touchpad
	hidInitializeTouchScreen();
	// It's necessary to initialize these separately as they all have different handle values
	hidGetSixAxisSensorHandles(&this->sixaxis_handles[0], 1, HidNpadIdType_Handheld, HidNpadStyleTag_NpadHandheld);
	hidGetSixAxisSensorHandles(&this->sixaxis_handles[1], 1, HidNpadIdType_No1, HidNpadStyleTag_NpadFullKey);
	hidGetSixAxisSensorHandles(&this->sixaxis_handles[2], 2, HidNpadIdType_No1, HidNpadStyleTag_NpadJoyDual);
	hidStartSixAxisSensor(this->sixaxis_handles[0]);
	hidStartSixAxisSensor(this->sixaxis_handles[1]);
	hidStartSixAxisSensor(this->sixaxis_handles[2]);
	hidStartSixAxisSensor(this->sixaxis_handles[3]);

    rc = hidInitializeVibrationDevices(this->vibration_handles[0], 2, HidNpadIdType_Handheld, HidNpadStyleTag_NpadHandheld);
	if(R_FAILED(rc))
		CHIAKI_LOGE(this->log, "hidInitializeVibrationDevices() HidNpadIdType_Handheld returned: 0x%x", rc);

    rc = hidInitializeVibrationDevices(this->vibration_handles[1], 2, HidNpadIdType_No1, HidNpadStyleTag_NpadJoyDual);
	if(R_FAILED(rc))
		CHIAKI_LOGE(this->log, "hidInitializeVibrationDevices() HidNpadIdType_No1 returned: 0x%x", rc);

#endif
	return true;
}

bool IO::FreeController()
{
	for(int i = 0; i < SDL_JOYSTICK_COUNT; i++)
	{
		SDL_JoystickClose(this->sdl_joystick_ptr[i]);
		// SDL_HapticClose(this->sdl_haptic_ptr[i]);
	}
#ifdef __SWITCH__
	hidStopSixAxisSensor(this->sixaxis_handles[0]);
	hidStopSixAxisSensor(this->sixaxis_handles[1]);
	hidStopSixAxisSensor(this->sixaxis_handles[2]);
	hidStopSixAxisSensor(this->sixaxis_handles[3]);
#endif
	return true;
}

#ifdef __SWITCH__
void IO::SetPS3Stream(bool is_ps3)
{
	this->ps3_stream = is_ps3;
	this->minus_held_frames = 0;
	this->minus_combo_triggered = false;
	this->synthetic_touch_mode = SyntheticTouchMode::NONE;
	this->synthetic_touch_id = -1;
}

void IO::StartSyntheticTouch(ChiakiControllerState *state, SyntheticTouchMode mode,
	uint16_t start_x, uint16_t start_y, uint16_t end_x, uint16_t end_y)
{
	if(this->synthetic_touch_id >= 0)
		chiaki_controller_state_stop_touch(state, (uint8_t)this->synthetic_touch_id);

	this->synthetic_touch_mode = mode;
	this->synthetic_touch_frame = 0;
	this->synthetic_touch_start_x = start_x;
	this->synthetic_touch_start_y = start_y;
	this->synthetic_touch_end_x = end_x;
	this->synthetic_touch_end_y = end_y;
	this->synthetic_touch_id = chiaki_controller_state_start_touch(state, start_x, start_y);
	if(this->synthetic_touch_id < 0)
		this->synthetic_touch_mode = SyntheticTouchMode::NONE;
}

void IO::UpdateSyntheticTouch(ChiakiControllerState *state)
{
	if(this->synthetic_touch_mode == SyntheticTouchMode::NONE || this->synthetic_touch_id < 0)
		return;

	this->synthetic_touch_frame++;
	if(this->synthetic_touch_mode == SyntheticTouchMode::TAP || this->synthetic_touch_mode == SyntheticTouchMode::HOLD)
		state->buttons |= CHIAKI_CONTROLLER_BUTTON_TOUCHPAD;

	if(this->synthetic_touch_mode == SyntheticTouchMode::SWIPE && this->synthetic_touch_frame >= 4)
		chiaki_controller_state_set_touch_pos(state, (uint8_t)this->synthetic_touch_id,
			this->synthetic_touch_end_x, this->synthetic_touch_end_y);

	bool finished = (this->synthetic_touch_mode == SyntheticTouchMode::TAP && this->synthetic_touch_frame >= 5)
		|| (this->synthetic_touch_mode == SyntheticTouchMode::SWIPE && this->synthetic_touch_frame >= 9);
	if(finished)
	{
		chiaki_controller_state_stop_touch(state, (uint8_t)this->synthetic_touch_id);
		state->buttons &= ~CHIAKI_CONTROLLER_BUTTON_TOUCHPAD;
		this->synthetic_touch_id = -1;
		this->synthetic_touch_mode = SyntheticTouchMode::NONE;
	}
}

void IO::ApplyCloudPadControllerMapping(ChiakiControllerState *state, u64 held, u64 down, u64 up)
{
	const bool minus_held = (held & HidNpadButton_Minus) != 0;
	const bool home_held = minus_held && (held & HidNpadButton_Plus);

	// PS3 has no touchpad: Minus is Select/Share. Minus + Plus remains PS Home
	// so cloud PS3 sessions can open the XMB menu.
	if(this->ps3_stream)
	{
		if(home_held)
		{
			state->buttons &= ~(CHIAKI_CONTROLLER_BUTTON_SHARE | CHIAKI_CONTROLLER_BUTTON_OPTIONS);
			state->buttons |= CHIAKI_CONTROLLER_BUTTON_PS;
		}
		else
		{
			state->buttons &= ~CHIAKI_CONTROLLER_BUTTON_PS;
			if(minus_held)
				state->buttons |= CHIAKI_CONTROLLER_BUTTON_SHARE;
		}
		return;
	}

	// PS4/PS5 use Minus as CloudPad's Select modifier. A quick press clicks
	// the touchpad; holding it alone becomes a sustained touchpad click.
	state->buttons &= ~CHIAKI_CONTROLLER_BUTTON_SHARE;
	if(down & HidNpadButton_Minus)
	{
		this->minus_held_frames = 0;
		this->minus_combo_triggered = false;
	}

	if(minus_held)
	{
		this->minus_held_frames++;
		uint32_t suppress = 0;
		if(held & HidNpadButton_L) suppress |= CHIAKI_CONTROLLER_BUTTON_L1;
		if(held & HidNpadButton_R) suppress |= CHIAKI_CONTROLLER_BUTTON_R1;
		if(held & HidNpadButton_X) suppress |= CHIAKI_CONTROLLER_BUTTON_PYRAMID;
		if(held & HidNpadButton_Y) suppress |= CHIAKI_CONTROLLER_BUTTON_BOX;
		if(held & HidNpadButton_A) suppress |= CHIAKI_CONTROLLER_BUTTON_MOON;
		if(held & HidNpadButton_B) suppress |= CHIAKI_CONTROLLER_BUTTON_CROSS;
		state->buttons &= ~suppress;

		auto start_combo = [this, state](SyntheticTouchMode mode,
			uint16_t sx, uint16_t sy, uint16_t ex, uint16_t ey) {
			this->minus_combo_triggered = true;
			this->StartSyntheticTouch(state, mode, sx, sy, ex, ey);
		};
		if(down & HidNpadButton_L) start_combo(SyntheticTouchMode::TAP, 480, 471, 480, 471);
		else if(down & HidNpadButton_R) start_combo(SyntheticTouchMode::TAP, 1440, 471, 1440, 471);
		else if(down & HidNpadButton_X) start_combo(SyntheticTouchMode::SWIPE, 960, 471, 960, 120);
		else if(down & HidNpadButton_Y) start_combo(SyntheticTouchMode::SWIPE, 960, 471, 250, 471);
		else if(down & HidNpadButton_A) start_combo(SyntheticTouchMode::SWIPE, 960, 471, 1670, 471);
		else if(down & HidNpadButton_B) start_combo(SyntheticTouchMode::SWIPE, 960, 471, 960, 820);
		else if(down & HidNpadButton_Plus)
			this->minus_combo_triggered = true;

		if(home_held)
		{
			state->buttons &= ~CHIAKI_CONTROLLER_BUTTON_OPTIONS;
			state->buttons |= CHIAKI_CONTROLLER_BUTTON_PS;
		}
		else
			state->buttons &= ~CHIAKI_CONTROLLER_BUTTON_PS;

		// Roughly 300 ms at 60 FPS separates a click from click-and-hold.
		if(!this->minus_combo_triggered && this->minus_held_frames == 18)
			this->StartSyntheticTouch(state, SyntheticTouchMode::HOLD, 960, 471, 960, 471);
	}

	if(up & HidNpadButton_Minus)
	{
		if(this->synthetic_touch_mode == SyntheticTouchMode::HOLD && this->synthetic_touch_id >= 0)
		{
			chiaki_controller_state_stop_touch(state, (uint8_t)this->synthetic_touch_id);
			state->buttons &= ~CHIAKI_CONTROLLER_BUTTON_TOUCHPAD;
			this->synthetic_touch_id = -1;
			this->synthetic_touch_mode = SyntheticTouchMode::NONE;
		}
		else if(!this->minus_combo_triggered && this->minus_held_frames < 18)
			this->StartSyntheticTouch(state, SyntheticTouchMode::TAP, 960, 471, 960, 471);

		this->minus_held_frames = 0;
		this->minus_combo_triggered = false;
		state->buttons &= ~CHIAKI_CONTROLLER_BUTTON_PS;
	}

	UpdateSyntheticTouch(state);
}
#else
void IO::SetPS3Stream(bool is_ps3)
{
	(void)is_ps3;
}
#endif

void IO::UpdateControllerState(ChiakiControllerState *state, std::map<uint32_t, int8_t> *finger_id_touch_id)
{
#ifdef __SWITCH__
	padUpdate(&this->pad);
	u64 held = padGetButtons(&this->pad);
	u64 down = padGetButtonsDown(&this->pad);
	u64 up = padGetButtonsUp(&this->pad);
	// ZL+ZR+Plus: exit the stream cleanly back to the app's own main menu,
	// without needing to wait for a server-side disconnect. Held-together
	// combo (not a single button) so it can't be triggered accidentally
	// during normal play.
	{
		if((held & HidNpadButton_ZL) && (held & HidNpadButton_ZR) && (held & HidNpadButton_Plus))
			this->exit_stream_requested.store(true);
	}
#endif
	// handle SDL events
	while(SDL_PollEvent(&this->sdl_event))
	{
		this->ReadGameKeys(&this->sdl_event, state);
		switch(this->sdl_event.type)
		{
			case SDL_QUIT:
				this->quit = true;
		}
	}

	ReadGameTouchScreen(state, finger_id_touch_id);
	ReadGameSixAxis(state);
#ifdef __SWITCH__
	ApplyCloudPadControllerMapping(state, held, down, up);
#endif
}

bool IO::MainLoop()
{
	D(glUseProgram(this->prog));

	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	OpenGlDraw();
	chiaki_switch_self_report_ticks(this->cpu_sample_main_idx);

	return !this->quit;
}

void IO::SetSharpenLevel(int level)
{
	// 0.15 per level keeps High (0.45) visibly sharper without haloing badly
	// on the Switch's own 720p/1080p panel scaling.
	this->sharpen_amount = 0.15f * (float)level;
}

void IO::SetVideoPacing(bool smooth)
{
	this->video_pacing_smooth = smooth;
	this->pacing_phase = 0;
	this->pacing_source_refresh_period = 1;
	this->pacing_fast_streak = 0;
	this->pacing_slow_streak = 0;
	this->pacing_last_decode_ms = 0;
}
