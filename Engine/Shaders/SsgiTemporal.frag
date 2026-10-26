#version 460
#extension GL_GOOGLE_include_directive : require
#include "Common.glsl"

// Screen-space GI's temporal filter: this frame's half-resolution trace, upsampled, blended with last frame's result
// reprojected to where the surface was then (from the depth buffer). The history is clamped to the trace's colors
// around the pixel, so light that moved does not smear.

layout(push_constant) uniform Push
{
    mat4 PrevViewProj;
    uint Current;   // bindless: this frame's trace (half resolution)
    uint History;   // bindless: last frame's filtered result
    uint Depth;
    uint Valid;     // 1 = the history holds a previous frame
    vec2 Texel;     // 1 / the trace's size
    float Blend;    // weight of the history
} pc;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

void main()
{
    vec3 current = textureLod(uTextures[nonuniformEXT(pc.Current)], vUV, 0.0).rgb;
    float depth = textureLod(uTextures[nonuniformEXT(pc.Depth)], vUV, 0.0).r;
    if (pc.Valid == 0u || depth >= 1.0)
    {
        outColor = vec4(current, 1.0);
        return;
    }
    // The neighbourhood's range in this frame's trace (a 3x3 of trace texels).
    vec3 lo = current, hi = current;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x)
        {
            vec3 c = textureLod(uTextures[nonuniformEXT(pc.Current)], vUV + vec2(x, y) * pc.Texel, 0.0).rgb;
            lo = min(lo, c);
            hi = max(hi, c);
        }
    vec4 world = uScene.InvViewProj * vec4(vUV.x * 2.0 - 1.0, 1.0 - vUV.y * 2.0, depth, 1.0);
    vec4 prev = pc.PrevViewProj * vec4(world.xyz / world.w, 1.0);
    vec2 prevUV = vec2(prev.x / prev.w * 0.5 + 0.5, 0.5 - prev.y / prev.w * 0.5);
    if (prev.w <= 0.0 || any(lessThan(prevUV, vec2(0.0))) || any(greaterThan(prevUV, vec2(1.0))))
    {
        outColor = vec4(current, 1.0);
        return;
    }
    vec3 history = clamp(textureLod(uTextures[nonuniformEXT(pc.History)], prevUV, 0.0).rgb, lo, hi);
    outColor = vec4(mix(current, history, pc.Blend), 1.0);
}
