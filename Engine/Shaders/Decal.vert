#version 460
#extension GL_GOOGLE_include_directive : require
#include "Common.glsl"

// A decal's box (the unit cube scaled by its size), drawn to find the pixels it may cover; Decal.frag does the rest.

layout(location = 0) in vec3 aPosition;

layout(push_constant) uniform Push
{
    mat4 Model;     // decal box: local [-0.5, 0.5]^3 -> world
    vec4 Color;     // linear, a: opacity
    uint Texture;
    uint Depth;
    uint Blend;     // 0 alpha, 1 multiply, 2 additive
    float AngleFade; // cos of the surface angle where it starts fading out
} pc;

void main()
{
    gl_Position = uScene.ViewProj * (pc.Model * vec4(aPosition, 1.0));
}
