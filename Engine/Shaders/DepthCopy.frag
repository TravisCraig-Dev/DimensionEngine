#version 460
#extension GL_GOOGLE_include_directive : require
#include "Common.glsl"

// The opaque depth (from the depth prepass) copied into a color image that surfaces drawn later can read while the
// depth buffer itself is attached (water: shore foam where the ground is close under the surface).

layout(push_constant) uniform Push
{
    uint Depth; // bindless: the scene depth
} pc;

layout(location = 0) out float outDepth;

void main()
{
    outDepth = texelFetch(uTextures[nonuniformEXT(pc.Depth)], ivec2(gl_FragCoord.xy), 0).r;
}
