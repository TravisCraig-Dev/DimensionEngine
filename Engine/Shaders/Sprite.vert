#version 460
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference_uvec2 : require
#include "Common.glsl"
#include "ParticleLighting.glsl"

layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec4 aColor;
layout(location = 2) in vec2 aUV;
layout(location = 3) in uint aTexture;
layout(location = 4) in uint aEntity;

layout(location = 0) out vec4 vColor;
layout(location = 1) out vec2 vUV;
layout(location = 2) flat out uint vTexture;
layout(location = 3) flat out uint vEntity;

void main()
{
    vColor = aColor;
    vUV = aUV;
    vTexture = aTexture & ~0x20000000u;
    // Bit 29: a lit particle, shaded by the scene's lights where it is.
    if ((aTexture & 0x20000000u) != 0u) vColor.rgb *= ParticleLight(aPosition);
    vEntity = aEntity;
    gl_Position = uScene.ViewProj * vec4(aPosition, 1.0);
}
