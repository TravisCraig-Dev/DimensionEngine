// Lit particles (Particle System > Lit): the light reaching a particle at a world position, evaluated per vertex. A
// particle faces the camera, so its light wraps around it (half of what comes from behind still reaches it): the
// ambient (or the sky texture's light from above), the sun with its cascaded shadow, and the point and spot lights of
// the position's cluster (without their shadows or cookies). Multiplies the particle's color, like a mesh's albedo.
// Needs Common.glsl.

layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer ParticleLightListRef { PointLight Lights[]; };
layout(buffer_reference, std430, buffer_reference_align = 8) readonly buffer ParticleClusterCellsRef { uvec2 Cells[]; };
layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer ParticleClusterIndexRef { uint Indices[]; };

vec3 ParticleLight(vec3 pos)
{
    vec3 light = uScene.Ambient.rgb;
    if (HasSkyTexture() && uScene.EnvParams.z > 0.5) light = SkyIrradiance(vec3(0.0, 1.0, 0.0)) * uScene.EnvParams.w;
    vec3 N = normalize(uScene.CameraPosition.xyz - pos + vec3(0.0, 1e-4, 0.0));
    if (uScene.LightDirection.w > 0.5)
    {
        vec3 L = normalize(-uScene.LightDirection.xyz);
        float wrap = dot(N, L) * 0.5 + 0.5;
        light += uScene.LightColor.rgb * wrap * DirectionalShadow(pos, L);
    }
    if (uScene.Counts.x > 0)
    {
        vec4 clip = uScene.ViewProj * vec4(pos, 1.0);
        vec2 ndc = clip.xy / max(clip.w, 1e-4);
        ivec2 tile = clamp(ivec2((ndc * 0.5 + 0.5) * vec2(CLUSTER_TILES_X, CLUSTER_TILES_Y)), ivec2(0), ivec2(CLUSTER_TILES_X - 1, CLUSTER_TILES_Y - 1));
        float depth = -(uScene.View * vec4(pos, 1.0)).z;
        float n = uScene.ClusterParams.x, f = uScene.ClusterParams.y;
        float s = uScene.ClusterParams.z > 0.5 ? (depth - n) / (f - n) : log(max(depth, n) / n) / uScene.ClusterParams.w;
        int slice = clamp(int(s * float(CLUSTER_SLICES)), 0, CLUSTER_SLICES - 1);
        uvec2 cell = ParticleClusterCellsRef(uScene.LightBuffers.zw).Cells[(slice * CLUSTER_TILES_Y + tile.y) * CLUSTER_TILES_X + tile.x];
        ParticleLightListRef lights = ParticleLightListRef(uScene.LightBuffers.xy);
        ParticleClusterIndexRef indices = ParticleClusterIndexRef(uScene.LightIndexBuffer.xy);
        for (uint k = 0u; k < cell.y; ++k)
        {
            PointLight l = lights.Lights[indices.Indices[cell.x + k]];
            vec3 toLight = l.PositionRange.xyz - pos;
            float dist = length(toLight);
            float falloff = clamp(1.0 - pow(dist / l.PositionRange.w, 4.0), 0.0, 1.0);
            float attenuation = falloff * falloff / (dist * dist + 1.0);
            if (l.Direction.w > -1.5) attenuation *= smoothstep(l.Direction.w, l.Cone.x, dot(-toLight / max(dist, 1e-4), l.Direction.xyz));
            float wrap = dot(N, toLight / max(dist, 1e-4)) * 0.5 + 0.5;
            light += l.Color.rgb * attenuation * wrap;
        }
    }
    return light;
}
