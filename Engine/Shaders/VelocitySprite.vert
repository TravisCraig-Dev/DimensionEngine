#version 460
#extension GL_GOOGLE_include_directive : require
#include "Common.glsl"
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference_uvec2 : require

// Motion vectors of moving sprites and particles: each corner where it is now and where it was last frame, both
// projected without the TAA jitter.

layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec3 aPrevious;
layout(location = 2) in vec2 aUV;
layout(location = 3) in uint aTexture;
layout(location = 4) in float aAlpha;

layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer FrameRef
{
    mat4 ViewProj;     // this frame's, unjittered
    mat4 PrevViewProj; // last frame's, unjittered
};

layout(push_constant) uniform Push
{
    FrameRef Frame;
    uint Depth; // bindless: the scene depth
    uint Pad;
} pc;

layout(location = 0) out vec4 vCurrent;
layout(location = 1) out vec4 vPrevious;
layout(location = 2) out vec2 vUV;
layout(location = 3) flat out uint vTexture;
layout(location = 4) out float vAlpha;

void main()
{
    vCurrent = pc.Frame.ViewProj * vec4(aPosition, 1.0);
    vPrevious = pc.Frame.PrevViewProj * vec4(aPrevious, 1.0);
    vUV = aUV;
    vTexture = aTexture;
    vAlpha = aAlpha;
    gl_Position = uScene.ViewProj * vec4(aPosition, 1.0);
}
