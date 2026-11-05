#version 460
#extension GL_GOOGLE_include_directive : require
#include "Common.glsl"
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference_uvec2 : require

// A moving object's screen motion, written only where its surface is the one in the depth buffer (within a small
// distance, so cut-out holes and whatever hides it keep the camera's motion).

layout(push_constant) uniform Push
{
    uvec2 Objects;
    uvec2 Skin;
    uvec2 Frame;
    uint Depth;
    uint Pad;
} pc;

layout(location = 0) in vec4 vCurrent;
layout(location = 1) in vec4 vPrevious;
layout(location = 0) out vec2 outVelocity;

float ViewDepth(float d)
{
    vec4 p = uScene.InvProj * vec4(0.0, 0.0, d, 1.0);
    return -p.z / p.w;
}

void main()
{
    float scene = texelFetch(uTextures[nonuniformEXT(pc.Depth)], ivec2(gl_FragCoord.xy), 0).r;
    float here = ViewDepth(gl_FragCoord.z), there = ViewDepth(scene);
    if (abs(here - there) > 0.01 + 0.01 * abs(there)) discard;
    vec2 current = vCurrent.xy / vCurrent.w, previous = vPrevious.xy / vPrevious.w;
    // NDC to UV (y down), this frame minus last.
    outVelocity = vec2(current.x - previous.x, previous.y - current.y) * 0.5;
}
