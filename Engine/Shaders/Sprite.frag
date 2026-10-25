#version 460
#extension GL_GOOGLE_include_directive : require
#include "Common.glsl"

layout(location = 0) in vec4 vColor;
layout(location = 1) in vec2 vUV;
layout(location = 2) flat in uint vTexture;
layout(location = 3) flat in uint vEntity;

layout(location = 0) out vec4 outColor;
layout(location = 1) out uint outEntity;

// Text atlases (top bit of the index) store a signed distance in alpha (0.5 on the outline).
float DistanceAlpha(float d)
{
    float w = max(fwidth(d) * 0.75, 1e-4);
    return smoothstep(0.5 - w, 0.5 + w, d);
}

void main()
{
    vec4 t = (vTexture & 0x80000000u) != 0u ? vec4(1.0, 1.0, 1.0, DistanceAlpha(SampleTexture(vTexture & 0x7FFFFFFFu, vUV).a))
                                            : SampleTexture(vTexture, vUV);
    vec4 color = t * vColor;
    if (color.a < 0.01) discard; // keeps fully transparent texels out of picking
    outColor = color;
    outEntity = vEntity;
}
