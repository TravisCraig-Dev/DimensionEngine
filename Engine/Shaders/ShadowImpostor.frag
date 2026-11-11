#version 460
#extension GL_GOOGLE_include_directive : require
#include "Common.glsl"
#include "MeshCommon.glsl"

// Shadow maps for impostors' sun-facing cutouts: each pixel at its texel's depth on the mesh along the light, so an
// impostor shadows itself and the ground at its base as its mesh does.
#define DIM_SHADOW 1
#include "ImpostorDepth.glsl"
