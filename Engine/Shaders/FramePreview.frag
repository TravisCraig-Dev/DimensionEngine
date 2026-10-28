#version 460
#extension GL_GOOGLE_include_directive : require
#include "Common.glsl"

// Frame debugger: a render target drawn into a small picture for the editor. Depth (shadow maps) as grey, near
// bright; HDR color tonemapped. Written sRGB-encoded, as the scene's output.

layout(push_constant) uniform Push
{
    uint Source; // bindless
    uint Mode;   // 0 depth, 1 HDR color
} pc;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

vec3 ToSrgb(vec3 c) { return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(vec3(0.0031308), c)); }

void main()
{
    vec4 c = texture(uTextures[nonuniformEXT(pc.Source)], vUV);
    vec3 shown = pc.Mode == 0u ? vec3(pow(clamp(1.0 - c.r, 0.0, 1.0), 0.6)) : ToSrgb(c.rgb / (1.0 + c.rgb));
    outColor = vec4(shown, 1.0);
}
