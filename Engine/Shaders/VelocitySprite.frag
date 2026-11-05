#version 460
#extension GL_GOOGLE_include_directive : require
#include "Common.glsl"

// A moving sprite's screen motion where it is mostly opaque (its texture's alpha times its color's) and not hidden
// behind the scene's surfaces (sprites write no depth).

layout(push_constant) uniform Push
{
    uvec2 Frame;
    uint Depth;
    uint Pad;
} pc;

layout(location = 0) in vec4 vCurrent;
layout(location = 1) in vec4 vPrevious;
layout(location = 2) in vec2 vUV;
layout(location = 3) flat in uint vTexture;
layout(location = 4) in float vAlpha;
layout(location = 0) out vec2 outVelocity;

float ViewDepth(float d)
{
    vec4 p = uScene.InvProj * vec4(0.0, 0.0, d, 1.0);
    return -p.z / p.w;
}

void main()
{
    // Text (top bit) is a distance field: inside the outline above 0.5.
    float alpha;
    if ((vTexture & 0x80000000u) != 0u)
    {
        vec4 field = SampleTexture(vTexture & 0x3FFFFFFFu, vUV);
        float d = (vTexture & 0x40000000u) != 0u ? max(min(field.r, field.g), min(max(field.r, field.g), field.b)) : field.a;
        alpha = step(0.5, d);
    }
    else
        alpha = SampleTexture(vTexture & 0x0FFFFFFFu, vUV).a;
    if (alpha * vAlpha < 0.5) discard;
    float scene = texelFetch(uTextures[nonuniformEXT(pc.Depth)], ivec2(gl_FragCoord.xy), 0).r;
    float here = ViewDepth(gl_FragCoord.z), there = ViewDepth(scene);
    if (here > there + 0.01 + 0.01 * abs(there)) discard;
    vec2 current = vCurrent.xy / vCurrent.w, previous = vPrevious.xy / vPrevious.w;
    outVelocity = vec2(current.x - previous.x, previous.y - current.y) * 0.5;
}
