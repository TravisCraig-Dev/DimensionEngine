#version 460
#extension GL_GOOGLE_include_directive : require
#include "Common.glsl"

// Camera motion blur: each pixel's world position (from depth) is projected with last frame's view, and the image is
// averaged along the path it moved on screen (centered on the pixel), as long as the shutter was open (Intensity), up
// to MaxLength of the screen.

layout(push_constant) uniform Push
{
    mat4 Reproject;    // this frame's clip space to last frame's (last ViewProj * inverse ViewProj)
    uint Hdr;          // bindless: this frame's HDR color
    uint Depth;        // bindless: this frame's depth
    float Intensity;   // fraction of the motion blurred (the shutter)
    float MaxLength;   // longest blur, as a fraction of the screen
} pc;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

void main()
{
    float depth = textureLod(uTextures[nonuniformEXT(pc.Depth)], vUV, 0.0).r;
    vec4 previous = pc.Reproject * vec4(vUV.x * 2.0 - 1.0, 1.0 - vUV.y * 2.0, depth, 1.0);
    vec2 previousUV = vec2(previous.x / previous.w * 0.5 + 0.5, 0.5 - previous.y / previous.w * 0.5);
    vec2 motion = (vUV - previousUV) * pc.Intensity;
    float len = length(motion);
    if (len > pc.MaxLength) motion *= pc.MaxLength / len;
    vec4 center = textureLod(uTextures[nonuniformEXT(pc.Hdr)], vUV, 0.0);
    if (len < 0.0005)
    {
        outColor = center;
        return;
    }
    const int kSamples = 10;
    vec3 sum = vec3(0.0);
    for (int i = 0; i < kSamples; ++i)
    {
        float t = (float(i) + 0.5) / float(kSamples) - 0.5;
        sum += textureLod(uTextures[nonuniformEXT(pc.Hdr)], clamp(vUV + motion * t, vec2(0.0), vec2(1.0)), 0.0).rgb;
    }
    outColor = vec4(sum / float(kSamples), center.a);
}
