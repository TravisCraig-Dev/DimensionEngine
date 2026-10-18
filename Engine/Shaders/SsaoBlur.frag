#version 460
#extension GL_GOOGLE_include_directive : require
#include "Common.glsl"

// 4x4 depth-aware blur of the raw SSAO (removes the per-pixel kernel rotation without bleeding across edges).

layout(push_constant) uniform Push
{
    uint AO;
    uint Depth;
    vec2 AOTexel;
} pc;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outAO;

float LinearDepth(vec2 uv)
{
    float d = textureLod(uTextures[nonuniformEXT(pc.Depth)], uv, 0.0).r;
    vec4 p = uScene.InvProj * vec4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, d, 1.0);
    return -p.z / p.w;
}

void main()
{
    float center = LinearDepth(vUV);
    float sum = 0.0, weight = 0.0;
    for (int y = -2; y < 2; ++y)
        for (int x = -2; x < 2; ++x)
        {
            vec2 uv = vUV + (vec2(x, y) + 0.5) * pc.AOTexel;
            float w = exp(-abs(LinearDepth(uv) - center) / max(center * 0.05, 1e-3));
            sum += textureLod(uTextures[nonuniformEXT(pc.AO)], uv, 0.0).r * w;
            weight += w;
        }
    outAO = vec4(weight > 0.0 ? sum / weight : 1.0);
}
