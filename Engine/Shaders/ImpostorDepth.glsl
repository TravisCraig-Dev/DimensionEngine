#include "Cutout.glsl"
// Depth-only impostor pixels (DepthImpostor.frag for the depth prepass, ShadowImpostor.frag for shadow maps): the
// cutout of Shadow.frag, each pixel at its texel's depth on the mesh.
layout(location = 0) in vec3 vWorldPos;
layout(location = 2) in vec2 vUV;
layout(location = 3) flat in uint vEntity; // unused; matches Mesh.vert's outputs

void main()
{
    if (LodFadeDiscard(gl_FragCoord.xy)) discard;
    MaterialRef m = pc.Material;
    vec2 uv = vUV * m.TilingOffset.xy + m.TilingOffset.zw;
    if (SampleTexture(m.Maps.x, uv).a * m.BaseColor.a * CutoutAlphaScale(m.Maps.x, uv) < m.Params.z) discard;
#ifdef DIM_SHADOW
    gl_FragDepth = ImpostorDepth(ImpostorPointAlong(m, uv, vWorldPos, ImpostorLightRay()));
#else
    gl_FragDepth = ImpostorDepth(ImpostorPoint(m, uv, vWorldPos));
#endif
}
