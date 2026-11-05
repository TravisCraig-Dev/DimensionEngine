#version 460
#extension GL_GOOGLE_include_directive : require
#include "Common.glsl"

// The camera's part of the velocity buffer: each pixel's point (from the depth buffer) projected with last frame's
// view. Moving objects then draw their own motion over it (Velocity.vert).

layout(push_constant) uniform Push
{
    mat4 Reproject; // this frame's unjittered clip space to last frame's
    uint Depth;     // bindless: this frame's depth
    uint Pad;
} pc;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec2 outVelocity;

void main()
{
    float depth = textureLod(uTextures[nonuniformEXT(pc.Depth)], vUV, 0.0).r;
    vec4 previous = pc.Reproject * vec4(vUV.x * 2.0 - 1.0, 1.0 - vUV.y * 2.0, depth, 1.0);
    vec2 previousUV = vec2(previous.x / previous.w * 0.5 + 0.5, 0.5 - previous.y / previous.w * 0.5);
    outVelocity = vUV - previousUV;
}
