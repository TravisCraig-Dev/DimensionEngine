// Fragment stages only (textureQueryLod). Include after Common.glsl.
// Cutout (alpha-tested) textures thin out in their smaller mips, where averaged alpha falls below the cutoff: foliage
// seen from afar would dissolve into its twigs. Scaling alpha up with the mip level being sampled keeps its coverage.
float CutoutAlphaScale(uint index, vec2 uv)
{
    float lod = textureQueryLod(uTextures[nonuniformEXT(index)], uv).y;
    return 1.0 + max(lod, 0.0) * 0.25;
}
