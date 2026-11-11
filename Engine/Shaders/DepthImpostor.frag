#version 460
#extension GL_GOOGLE_include_directive : require
#include "Common.glsl"
#include "MeshCommon.glsl"

// Depth prepass for impostors: each pixel at its texel's depth on the mesh (as MeshImpostor.frag puts it).
#include "ImpostorDepth.glsl"
