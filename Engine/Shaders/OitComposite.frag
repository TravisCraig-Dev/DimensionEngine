#version 460
#extension GL_GOOGLE_include_directive : require
#include "Common.glsl"

// Order-independent transparency, resolved over the scene: the weighted average color of the transparent layers,
// covering the scene by 1 - their transmittance (premultiplied: result = scene * reveal + average * (1 - reveal)).

layout(push_constant) uniform Push
{
    uint Accum;  // bindless: weighted color and alpha sums
    uint Reveal; // bindless: product of (1 - alpha)
} pc;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

void main()
{
    ivec2 p = ivec2(gl_FragCoord.xy);
    float reveal = texelFetch(uTextures[nonuniformEXT(pc.Reveal)], p, 0).r;
    vec4 accum = texelFetch(uTextures[nonuniformEXT(pc.Accum)], p, 0);
    if (reveal >= 0.9999) discard; // nothing transparent here
    vec3 average = accum.rgb / max(accum.a, 1e-5);
    outColor = vec4(average * (1.0 - reveal), 1.0 - reveal);
}
