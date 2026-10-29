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
        // In the grid's plane: XZ, XY or YZ (GridParams.y).
        int plane = int(uScene.GridParams.y + 0.5);
        vec3 p = vWorldPos, c = uScene.CameraPosition.xyz;
        float d = plane == 1 ? distance(p.xy, c.xy) : plane == 2 ? distance(p.zy, c.zy) : distance(p.xz, c.xz);
        color.a *= 1.0 - smoothstep(fade * 0.5, fade, d);
    }
    if (color.a <= 0.001) discard;
    outColor = color;
    outEntity = 0u;
}
