#version 460
#extension GL_GOOGLE_include_directive : require
#include "Common.glsl"

layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec4 aColor;

layout(location = 0) out vec4 vColor;
layout(location = 1) out vec3 vWorldPos;

void main()
{
    vColor = aColor;
    vWorldPos = aPosition;
    gl_Position = uScene.ViewProj * vec4(aPosition, 1.0);
}
