#version 460
#extension GL_GOOGLE_include_directive : require
// Impostors: Mesh.frag with each pixel moved to the depth its texel had on the mesh (the depth atlas), so it meets the
// ground and other objects as the mesh does.
#define DIM_IMPOSTOR 1
#include "Common.glsl"
#include "MeshCommon.glsl"
#include "MeshSurface.glsl"
