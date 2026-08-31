// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include "video_renderer.h"

#include <array>
#include <cstring>

#include <borealis.hpp>
#include <borealis/platforms/switch/switch_video.hpp>

namespace
{
	struct Vertex
	{
		float position[3];
		float uv[2];
	};

	// SharpenParams uniform block (video_nv12.frag): std140 packs
	// "vec2 texelSize; float sharpen;" at byte offsets 0/8/12 - explicit
	// padding to 16 bytes here matches that exactly rather than relying on
	// a packed C++ struct's own (different, tighter) layout rules.
	struct SharpenUniforms
	{
		float texel_size_x;
		float texel_size_y;
		float sharpen;
		float _pad;
	};

	constexpr std::array<DkVtxAttribState, 2> VertexAttribState = {
		DkVtxAttribState{ 0, 0, offsetof(Vertex, position), DkVtxAttribSize_3x32, DkVtxAttribType_Float, 0 },
		DkVtxAttribState{ 0, 0, offsetof(Vertex, uv), DkVtxAttribSize_2x32, DkVtxAttribType_Float, 0 },
	};

	constexpr std::array<DkVtxBufferState, 1> VertexBufferState = {
		DkVtxBufferState{ sizeof(Vertex), 0 },
	};

	// Full-screen quad in NDC, top-left origin UVs (matches deko3d/nanovg's
	// own convention, no Y-flip needed) - same layout xlanor/akira's own
	// deko3d video renderer uses.
	constexpr std::array<Vertex, 4> QuadVertexData = {
		Vertex{ { -1.0f, +1.0f, 0.0f }, { 0.0f, 0.0f } },
		Vertex{ { -1.0f, -1.0f, 0.0f }, { 0.0f, 1.0f } },
		Vertex{ { +1.0f, -1.0f, 0.0f }, { 1.0f, 1.0f } },
		Vertex{ { +1.0f, +1.0f, 0.0f }, { 1.0f, 0.0f } },
	};
}

VideoRenderer::VideoRenderer() {}

VideoRenderer::~VideoRenderer()
{
	Cleanup();
}

bool VideoRenderer::Init(ChiakiLog *log, int frame_width, int frame_height)
{
	this->log = log;

	// Reject degenerate dimensions outright rather than handing deko3d an
	// invalid image to create - an invalid/never-created image silently
	// poisons the whole shared Queue into an unrecoverable error state the
	// instant anything tries to draw with it (confirmed on real hardware
	// earlier in this port: a missing shader file did exactly this, and it
	// showed up as an abort deep inside unrelated UI code, not here).
	if(frame_width <= 0 || frame_height <= 0 || frame_width > 4096 || frame_height > 4096)
	{
		CHIAKI_LOGE(this->log, "VideoRenderer::Init: refusing invalid frame size %dx%d", frame_width, frame_height);
		return false;
	}

	this->vctx = static_cast<brls::SwitchVideoContext *>(brls::Application::getPlatform()->getVideoContext());
	this->device = this->vctx->getDeko3dDevice();
	this->queue = this->vctx->getQueue();

	this->frame_width = frame_width;
	this->frame_height = frame_height;
	// NV12 chroma plane is half resolution in both dimensions, interleaved U/V.
	this->chroma_width = frame_width / 2;
	this->chroma_height = frame_height / 2;

	this->image_pool.emplace(this->device, DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image);
	this->code_pool.emplace(this->device, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_Code | DkMemBlockFlags_GpuCached);
	this->data_pool.emplace(this->device);

	if(!this->vertex_shader.load(*this->code_pool, "romfs:/shaders/video_nv12_vsh.dksh"))
	{
		CHIAKI_LOGE(this->log, "VideoRenderer::Init: failed to load vertex shader");
		return false;
	}
	if(!this->fragment_shader.load(*this->code_pool, "romfs:/shaders/video_nv12_fsh.dksh"))
	{
		CHIAKI_LOGE(this->log, "VideoRenderer::Init: failed to load fragment shader");
		return false;
	}

	dk::ImageLayoutMaker{ this->device }
		.setType(DkImageType_2D)
		.setFormat(DkImageFormat_R8_Unorm)
		.setDimensions(this->frame_width, this->frame_height, 1)
		.setFlags(DkImageFlags_UsageLoadStore | DkImageFlags_Usage2DEngine | DkImageFlags_UsageVideo)
		.initialize(this->luma_layout);
	this->luma_mem = this->image_pool->allocate(this->luma_layout.getSize(), this->luma_layout.getAlignment());
	if(!this->luma_mem)
	{
		CHIAKI_LOGE(this->log, "VideoRenderer::Init: failed to allocate luma image memory");
		return false;
	}
	this->luma_image.initialize(this->luma_layout, this->luma_mem.getMemBlock(), this->luma_mem.getOffset());

	dk::ImageLayoutMaker{ this->device }
		.setType(DkImageType_2D)
		.setFormat(DkImageFormat_RG8_Unorm)
		.setDimensions(this->chroma_width, this->chroma_height, 1)
		.setFlags(DkImageFlags_UsageLoadStore | DkImageFlags_Usage2DEngine | DkImageFlags_UsageVideo)
		.initialize(this->chroma_layout);
	this->chroma_mem = this->image_pool->allocate(this->chroma_layout.getSize(), this->chroma_layout.getAlignment());
	if(!this->chroma_mem)
	{
		CHIAKI_LOGE(this->log, "VideoRenderer::Init: failed to allocate chroma image memory");
		return false;
	}
	this->chroma_image.initialize(this->chroma_layout, this->chroma_mem.getMemBlock(), this->chroma_mem.getOffset());

	// Registers these two images in the same texture descriptor set
	// nanovg's own DkRenderer (and its UI images/font atlas) already uses -
	// SwitchVideoContext::allocateImageIndex()/updateImageDescriptor() are
	// thin wrappers around that same renderer instance's own descriptor
	// management (confirmed by reading switch_video.cpp), which is also
	// exactly how akira's own FSR render targets register themselves. Reuse
	// sampler index 0 below (dkMakeTextureHandle's second argument) rather
	// than creating a new sampler descriptor set: nanovg's DkRenderer
	// constructor already bound one globally at startup, with index 0 being
	// linear-filtered/clamp-to-edge - exactly what smooth video sampling
	// needs, and setting up a second one would be pure duplicated risk.
	this->luma_texture_id = this->vctx->allocateImageIndex();
	this->chroma_texture_id = this->vctx->allocateImageIndex();
	if(this->luma_texture_id < 0 || this->chroma_texture_id < 0)
	{
		CHIAKI_LOGE(this->log, "VideoRenderer::Init: failed to allocate image descriptor indices");
		return false;
	}

	{
		dk::ImageDescriptor luma_desc, chroma_desc;
		luma_desc.initialize(this->luma_image);
		chroma_desc.initialize(this->chroma_image);

		dk::UniqueCmdBuf update_cmdbuf = dk::CmdBufMaker{ this->device }.create();
		CMemPool::Handle update_cmdmem = this->data_pool->allocate(DK_MEMBLOCK_ALIGNMENT);
		update_cmdbuf.addMemory(update_cmdmem.getMemBlock(), update_cmdmem.getOffset(), update_cmdmem.getSize());

		if(!this->vctx->updateImageDescriptor(update_cmdbuf, this->luma_texture_id, luma_desc))
			CHIAKI_LOGE(this->log, "VideoRenderer::Init: failed to bind luma descriptor");
		if(!this->vctx->updateImageDescriptor(update_cmdbuf, this->chroma_texture_id, chroma_desc))
			CHIAKI_LOGE(this->log, "VideoRenderer::Init: failed to bind chroma descriptor");
		this->vctx->invalidateImageDescriptors(update_cmdbuf);

		this->queue.submitCommands(update_cmdbuf.finishList());
		this->queue.waitIdle();
		update_cmdmem.destroy();
	}

	this->vertex_buffer = this->data_pool->allocate(sizeof(QuadVertexData), DK_CMDMEM_ALIGNMENT);
	if(!this->vertex_buffer)
	{
		CHIAKI_LOGE(this->log, "VideoRenderer::Init: failed to allocate vertex buffer");
		return false;
	}
	memcpy(this->vertex_buffer.getCpuAddr(), QuadVertexData.data(), sizeof(QuadVertexData));

	this->uniform_buffer = this->data_pool->allocate(sizeof(SharpenUniforms), DK_UNIFORM_BUF_ALIGNMENT);
	if(!this->uniform_buffer)
	{
		CHIAKI_LOGE(this->log, "VideoRenderer::Init: failed to allocate uniform buffer");
		return false;
	}
	SharpenUniforms initial_uniforms{ 1.0f / (float)this->frame_width, 1.0f / (float)this->frame_height, this->sharpen_amount, 0.0f };
	memcpy(this->uniform_buffer.getCpuAddr(), &initial_uniforms, sizeof(initial_uniforms));

	this->dyn_cmd_buf = dk::CmdBufMaker{ this->device }.create();
	this->dyn_cmd_mem = this->data_pool->allocate(DynCmdSize);
	if(!this->dyn_cmd_mem)
	{
		CHIAKI_LOGE(this->log, "VideoRenderer::Init: failed to allocate command memory");
		return false;
	}
	this->dyn_cmd_buf.addMemory(this->dyn_cmd_mem.getMemBlock(), this->dyn_cmd_mem.getOffset(), this->dyn_cmd_mem.getSize());

	this->initialized = true;
	CHIAKI_LOGI(this->log, "VideoRenderer::Init: initialized for %dx%d (luma_id=%d chroma_id=%d)",
		frame_width, frame_height, this->luma_texture_id, this->chroma_texture_id);
	return true;
}

void VideoRenderer::Cleanup()
{
	if(!this->initialized)
		return;

	// Make sure nothing we're about to free is still in flight.
	this->queue.waitIdle();

	if(this->luma_texture_id >= 0)
		this->vctx->freeImageIndex(this->luma_texture_id);
	if(this->chroma_texture_id >= 0)
		this->vctx->freeImageIndex(this->chroma_texture_id);
	this->luma_texture_id = -1;
	this->chroma_texture_id = -1;

	this->luma_image = dk::Image{};
	this->chroma_image = dk::Image{};
	this->luma_mem.destroy();
	this->chroma_mem.destroy();
	this->vertex_buffer.destroy();
	this->uniform_buffer.destroy();
	this->dyn_cmd_mem.destroy();
	this->dyn_cmd_buf = dk::UniqueCmdBuf{};

	this->vertex_shader.destroy();
	this->fragment_shader.destroy();

	this->image_pool.reset();
	this->code_pool.reset();
	this->data_pool.reset();

	this->initialized = false;
}

bool VideoRenderer::UploadPlane(dk::Image &image, int width, int height, int bytes_per_pixel,
	const uint8_t *src, int src_linesize)
{
	// Mirrors nanovg's own dk_renderer.cpp UpdateImage() helper exactly
	// (tightly-packed linear staging buffer -> copyBufferToImage -> submit
	// + waitIdle) - that pattern is already proven correct on real hardware
	// for every texture update in this app's UI, so this reuses it rather
	// than inventing a different one. The waitIdle() per plane update is a
	// known latency/CPU cost worth revisiting (a double-buffered staging
	// scheme could avoid blocking the render thread on the GPU copy every
	// frame), left as-is for correctness first given this couldn't be
	// verified on hardware this session.
	const size_t plane_size = (size_t)width * height * bytes_per_pixel;
	CMemPool::Handle staging = this->data_pool->allocate((uint32_t)plane_size, DK_IMAGE_LINEAR_STRIDE_ALIGNMENT);
	if(!staging)
	{
		CHIAKI_LOGE(this->log, "VideoRenderer::UploadPlane: failed to allocate staging buffer");
		return false;
	}

	uint8_t *dst = (uint8_t *)staging.getCpuAddr();
	int row_bytes = width * bytes_per_pixel;
	if(src_linesize == row_bytes)
	{
		memcpy(dst, src, plane_size);
	}
	else
	{
		for(int row = 0; row < height; row++)
			memcpy(dst + (size_t)row_bytes * row, src + (size_t)src_linesize * row, row_bytes);
	}

	dk::UniqueCmdBuf cmdbuf = dk::CmdBufMaker{ this->device }.create();
	CMemPool::Handle cmdmem = this->data_pool->allocate(DK_MEMBLOCK_ALIGNMENT);
	cmdbuf.addMemory(cmdmem.getMemBlock(), cmdmem.getOffset(), cmdmem.getSize());

	dk::ImageView view{ image };
	cmdbuf.copyBufferToImage({ staging.getGpuAddr() }, view, { 0, 0, 0, (uint32_t)width, (uint32_t)height, 1 });

	this->queue.submitCommands(cmdbuf.finishList());
	this->queue.waitIdle();

	cmdmem.destroy();
	staging.destroy();
	return true;
}

void VideoRenderer::UpdateFrame(AVFrame *frame)
{
	if(!this->initialized || frame == nullptr)
		return;

	if(frame->width != this->frame_width || frame->height != this->frame_height)
	{
		// A mid-stream resolution change would need textures recreated at
		// the new size - not handled here, matches the OpenGL path this
		// replaces (which also assumed a fixed size for the whole session).
		CHIAKI_LOGE(this->log, "VideoRenderer::UpdateFrame: frame size %dx%d != initialized %dx%d, dropping frame",
			frame->width, frame->height, this->frame_width, this->frame_height);
		return;
	}

	UploadPlane(this->luma_image, this->frame_width, this->frame_height, 1, frame->data[0], frame->linesize[0]);
	UploadPlane(this->chroma_image, this->chroma_width, this->chroma_height, 2, frame->data[1], frame->linesize[1]);
}

void VideoRenderer::Draw()
{
	if(!this->initialized)
		return;

	SharpenUniforms uniforms{ 1.0f / (float)this->frame_width, 1.0f / (float)this->frame_height, this->sharpen_amount, 0.0f };
	memcpy(this->uniform_buffer.getCpuAddr(), &uniforms, sizeof(uniforms));

	this->dyn_cmd_buf.clear();
	this->dyn_cmd_buf.addMemory(this->dyn_cmd_mem.getMemBlock(), this->dyn_cmd_mem.getOffset(), this->dyn_cmd_mem.getSize());

	this->dyn_cmd_buf.bindRasterizerState(dk::RasterizerState{}.setCullMode(DkFace_None));
	this->dyn_cmd_buf.bindDepthStencilState(dk::DepthStencilState{}
		.setDepthTestEnable(false)
		.setDepthWriteEnable(false)
		.setStencilTestEnable(false));
	this->dyn_cmd_buf.bindColorState(dk::ColorState{});
	this->dyn_cmd_buf.bindColorWriteState(dk::ColorWriteState{});

	this->dyn_cmd_buf.bindShaders(DkStageFlag_GraphicsMask, { this->vertex_shader, this->fragment_shader });
	this->dyn_cmd_buf.bindTextures(DkStage_Fragment, 0, dkMakeTextureHandle(this->luma_texture_id, 0));
	this->dyn_cmd_buf.bindTextures(DkStage_Fragment, 1, dkMakeTextureHandle(this->chroma_texture_id, 0));
	this->dyn_cmd_buf.bindUniformBuffer(DkStage_Fragment, 0, this->uniform_buffer.getGpuAddr(), this->uniform_buffer.getSize());

	this->dyn_cmd_buf.bindVtxBuffer(0, this->vertex_buffer.getGpuAddr(), this->vertex_buffer.getSize());
	this->dyn_cmd_buf.bindVtxAttribState(VertexAttribState);
	this->dyn_cmd_buf.bindVtxBufferState(VertexBufferState);

	this->dyn_cmd_buf.draw(DkPrimitive_Quads, QuadVertexData.size(), 1, 0, 0);

	this->queue.submitCommands(this->dyn_cmd_buf.finishList());
}

void VideoRenderer::SetSharpenLevel(int level)
{
	// 0.15 per level, matching the tuning already validated for the OpenGL
	// unsharp-mask this shader ports (see IO::SetSharpenLevel).
	this->sharpen_amount = 0.15f * (float)level;
}
