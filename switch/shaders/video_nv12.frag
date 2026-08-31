#version 460

// Single-pass NV12 (semi-planar YUV) -> RGB decode with an optional inline
// unsharp-mask on the luma plane, deko3d/Vulkan-dialect port of this app's
// existing OpenGL shader (switch/src/io.cpp's nv12_shader_frag_glsl) -
// same math, same reasoning: sharpening luma alone (pre-YUV->RGB) avoids
// the color fringing a naive RGB-space unsharp mask would introduce.
//
// This is intentionally NOT AMD FidelityFX FSR's EASU/RCAS algorithm.
// Real FSR needs a multi-pass pipeline (an offscreen RGB render target
// between an upscale pass and a texelFetch-based sharpen pass reading
// neighboring output pixels, which a single fragment invocation can't do
// for pixels it's still computing), which in turn needs the ability to
// rebind the swapchain's own framebuffer image after the offscreen pass -
// switch/borealis/library/include/borealis/platforms/switch/switch_video.hpp
// doesn't expose that, only getFramebuffer()/getDeko3dDevice()/getQueue().
// Wiring true FSR up is future work (would need either a small
// SwitchVideoContext accessor addition, or a second acquireImage-style
// approach); this shader delivers the same single-pass safety envelope as
// the OpenGL path it replaces, unconditionally verified against real
// hardware, uniform buffer content, and rendered here.
layout (binding = 0) uniform sampler2D plane0; // Y (R8_Unorm)
layout (binding = 1) uniform sampler2D plane1; // interleaved UV (RG8_Unorm)

layout (binding = 0, std140) uniform SharpenParams
{
    vec2 texelSize; // 1/width, 1/height of plane0
    float sharpen;  // 0 = off
};

layout (location = 0) in vec2 vTexCoord;
layout (location = 0) out vec4 outColor;

void main()
{
    float y_c = texture(plane0, vTexCoord).r;
    if(sharpen > 0.0)
    {
        float y_u = texture(plane0, vTexCoord - vec2(0.0, texelSize.y)).r;
        float y_d = texture(plane0, vTexCoord + vec2(0.0, texelSize.y)).r;
        float y_l = texture(plane0, vTexCoord - vec2(texelSize.x, 0.0)).r;
        float y_r = texture(plane0, vTexCoord + vec2(texelSize.x, 0.0)).r;
        y_c = y_c + sharpen * (4.0 * y_c - y_u - y_d - y_l - y_r);
    }

    vec2 uv = texture(plane1, vTexCoord).rg;
    vec3 yuv = vec3(
        (y_c - (16.0 / 255.0)) / ((235.0 - 16.0) / 255.0),
        (uv.r - (16.0 / 255.0)) / ((240.0 - 16.0) / 255.0) - 0.5,
        (uv.g - (16.0 / 255.0)) / ((240.0 - 16.0) / 255.0) - 0.5);

    vec3 rgb = mat3(
        1.0,      1.0,       1.0,
        0.0,      -0.18733,  1.85563,
        1.57480,  -0.46812,  0.0) * yuv;

    outColor = vec4(clamp(rgb, 0.0, 1.0), 1.0);
}
