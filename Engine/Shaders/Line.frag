#version 460
#extension GL_GOOGLE_include_directive : require
#include "Common.glsl"

layout(location = 0) in vec4 vColor;
layout(location = 1) in vec3 vWorldPos;

layout(location = 0) out vec4 outColor;
layout(location = 1) out uint outEntity; // write-masked off in the pipeline

void main()
{
    vec4 color = vColor;
    float fade = uScene.GridParams.x;
    if (fade > 0.0)
    {
        float d = distance(vWorldPos.xz, uScene.CameraPosition.xz);
        color.a *= 1.0 - smoothstep(fade * 0.5, fade, d);
    }
    if (color.a <= 0.001) discard;
    outColor = color;
    outEntity = 0u;
}
