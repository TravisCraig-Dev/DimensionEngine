#version 460
#extension GL_GOOGLE_include_directive : require
#include "Common.glsl"

// Adds the screen-space reflections (Ssr.frag's output) onto the HDR image (additive blending), through a small
// blur that hides the per-pixel jitter of the ray march.

layout(push_constant) uniform Push
{
    uint Reflections;
    uint Pad;
    vec2 Texel; // 1 / framebuffer size
} pc;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

void main()
{
    // Bilinear taps between the pixel and its neighbours: a 4x4 tent in 5 fetches.
    vec3 sum = textureLod(uTextures[nonuniformEXT(pc.Reflections)], vUV, 0.0).rgb * 0.36;
    const vec2 offsets[4] = vec2[](vec2(-0.75, -0.75), vec2(0.75, -0.75), vec2(-0.75, 0.75), vec2(0.75, 0.75));
    for (int i = 0; i < 4; ++i) sum += textureLod(uTextures[nonuniformEXT(pc.Reflections)], vUV + offsets[i] * pc.Texel, 0.0).rgb * 0.16;
    outColor = vec4(sum, 1.0);
}
