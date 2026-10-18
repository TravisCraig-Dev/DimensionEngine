#version 460
#extension GL_GOOGLE_include_directive : require
#include "Common.glsl"

layout(location = 0) in vec4 vColor;
layout(location = 1) in vec2 vUV;
layout(location = 2) flat in uint vTexture;
layout(location = 3) flat in uint vEntity;

layout(location = 0) out vec4 outColor;
layout(location = 1) out uint outEntity;

void main()
{
    vec4 color = SampleTexture(vTexture, vUV) * vColor;
    if (color.a < 0.01) discard; // keeps fully transparent texels out of picking
    outColor = color;
    outEntity = vEntity;
}
