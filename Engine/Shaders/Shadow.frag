#version 460
#extension GL_GOOGLE_include_directive : require
#include "Common.glsl"
#include "MeshCommon.glsl"
#include "Cutout.glsl"

// Depth-only pass. Cutout materials alpha-test so holes let light through.
layout(location = 2) in vec2 vUV;
layout(location = 3) flat in uint vEntity; // unused; matches Mesh.vert's outputs
layout(location = 5) in float vShell;      // fur shells
layout(location = 9) flat in uint vFade;   // LOD cross-fade

void main()
{
    if (LodFadeDiscard(gl_FragCoord.xy, vFade)) discard;
    MaterialRef m = pc.Material;
    if (m.Flags.x == 1u)
    {
        vec2 uv = vUV * m.TilingOffset.xy + m.TilingOffset.zw;
        if (SampleTexture(m.Maps.x, uv).a * m.BaseColor.a * CutoutAlphaScale(m.Maps.x, uv) < m.Params.z) discard;
    }
    if (FurDiscard(m, vUV, vShell)) discard; // fur shells cast and occlude only their strands
}
