#version 460
#extension GL_GOOGLE_include_directive : require
#include "Common.glsl"

// Editor overlay lines drawn after tonemapping, directly in display (sRGB) space.
layout(location = 0) in vec4 vColor;
layout(location = 1) in vec3 vWorldPos;

layout(location = 0) out vec4 outColor;

void main()
{
    outColor = vColor;
}
