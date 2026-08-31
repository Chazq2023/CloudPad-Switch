// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#ifndef CHIAKI_VIDEO_RENDERER_H
#define CHIAKI_VIDEO_RENDERER_H

#include <deko3d.hpp>
#include <optional>

#include <nanovg/framework/CMemPool.h>
#include <nanovg/framework/CShader.h>

#include <chiaki/log.h>

extern "C"
{
#include <libavcodec/avcodec.h>
}

namespace brls { class SwitchVideoContext; }

// Renders decoded NV12 video frames via deko3d. Deliberately shares the
// single dk::Device/dk::Queue Borealis's own SwitchVideoContext already
// owns (getDeko3dDevice()/getQueue()) rather than creating its own -
// deko3d expects one device/queue for the whole app, and this renderer's
// commands, nanovg's own UI draw commands, and any other queue user all
// submit to that one shared queue within the same Application::frame()
// beginFrame()/endFrame() window. This class must never call beginFrame()/
// endFrame()/create a swapchain itself - Draw() just records and submits a
// command list against whatever render target SwitchVideoContext::
// beginFrame() already bound for the current frame, exactly like nanovg's
// own DkRenderer::Flush() already does successfully every frame (verified:
// switch/borealis/library/lib/core/application.cpp's Application::frame()
// calls beginFrame() before the view tree draws, endFrame() after).
class VideoRenderer
{
	public:
		VideoRenderer();
		~VideoRenderer();

		VideoRenderer(const VideoRenderer&) = delete;
		void operator=(const VideoRenderer&) = delete;

		bool Init(ChiakiLog *log, int frame_width, int frame_height);
		void Cleanup();

		// Copies frame->data[0]/[1] (NV12 luma/interleaved-chroma planes) into
		// the GPU texture. CPU-copy (matches what the OpenGL path this
		// replaces already effectively did via glTexSubImage2D on the
		// NVTEGRA-decoded frame's CPU-mapped data), not the true zero-copy
		// NVTEGRA GPU-memory import akira's video_decoder.cpp does via
		// av_nvtegra_frame_get_fbuf_map/av_nvtegra_map_get_handle - deferred
		// as a follow-up optimization, see the .cpp for why.
		void UpdateFrame(AVFrame *frame);

		// Records and submits the draw command list to the shared queue.
		// Must be called from within an already-active beginFrame()/
		// endFrame() window (see class comment) - never calls either itself.
		void Draw();

		// level: 0=Off, 1..3=Low/Medium/High. See switch/shaders/video_nv12.frag.
		void SetSharpenLevel(int level);

	private:
		ChiakiLog *log = nullptr;
		bool initialized = false;

		brls::SwitchVideoContext *vctx = nullptr;
		dk::Device device;
		dk::Queue queue;

		std::optional<CMemPool> image_pool;
		std::optional<CMemPool> code_pool;
		std::optional<CMemPool> data_pool;

		CShader vertex_shader;
		CShader fragment_shader;

		int frame_width = 0;
		int frame_height = 0;
		int chroma_width = 0;
		int chroma_height = 0;

		dk::ImageLayout luma_layout;
		dk::ImageLayout chroma_layout;
		dk::Image luma_image;
		dk::Image chroma_image;
		CMemPool::Handle luma_mem;
		CMemPool::Handle chroma_mem;
		int luma_texture_id = -1;
		int chroma_texture_id = -1;

		CMemPool::Handle vertex_buffer;
		CMemPool::Handle uniform_buffer;

		// Reused every Draw() call rather than kept as a single static list
		// recorded once: other queue users (nanovg's own UI, e.g. a
		// stream-menu overlay) may rebind different shaders/textures/vertex
		// state on this same shared queue between our draw calls, so state
		// is rebound in full every frame rather than assumed to persist -
		// same approach nanovg's own DkRenderer::Flush() takes.
		dk::UniqueCmdBuf dyn_cmd_buf;
		CMemPool::Handle dyn_cmd_mem;
		static constexpr uint32_t DynCmdSize = 0x8000;

		float sharpen_amount = 0.0f;

		bool UploadPlane(dk::Image &image, int width, int height, int bytes_per_pixel,
			const uint8_t *src, int src_linesize);
};

#endif // CHIAKI_VIDEO_RENDERER_H
