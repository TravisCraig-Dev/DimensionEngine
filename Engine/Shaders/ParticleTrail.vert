#version 460
#extension GL_GOOGLE_include_directive : require
#include "Common.glsl"
#include "ParticleCommon.glsl"

// Draws GPU particles' trails: per slot, TRAIL_POINTS camera-facing segments from the particle back through the
// positions the simulation recorded, narrowing and fading toward the oldest (as the CPU trails). Four vertices per
// segment (indexed with the shared quad indices); segments without both ends recorded collapse. Feeds Sprite.frag.

layout(push_constant) uniform Push
{
    uvec2 Particles;
    uvec2 Params;
    uint Texture;
    uint Entity;
} pc;

layout(location = 0) out vec4 vColor;
layout(location = 1) out vec2 vUV;
layout(location = 2) flat out uint vTexture;
layout(location = 3) flat out uint vEntity;

void Discard()
{
    vColor = vec4(0.0);
    vUV = vec2(0.0);
    gl_Position = vec4(0.0, 0.0, 2.0, 1.0); // outside the depth range: clipped
}

void main()
{
    GpuParticleParamsRef P = GpuParticleParamsRef(pc.Params);
    uint segment = uint(gl_VertexIndex) / 4u, corner = uint(gl_VertexIndex) % 4u;
    uint index = segment / TRAIL_POINTS, k = segment % TRAIL_POINTS;
    vTexture = pc.Texture;
    vEntity = pc.Entity;
    if (index >= P.Counts.x)
    {
        Discard();
        return;
    }
    GpuParticle p = GpuParticlesRef(pc.Particles).Items[index];
    GpuTrailRef trail = GpuTrailRef(P.TrailBuffer.xy);
    uint base = index * TRAIL_POINTS;
    vec4 older = trail.Points[base + k];
    if (!ParticleAlive(p) || older.w < 0.0)
    {
        Discard();
        return;
    }
    uint recorded = 0u;
    for (uint i = 0u; i < TRAIL_POINTS; ++i) recorded += trail.Points[base + i].w >= 0.0 ? 1u : 0u;
    vec3 a = (P.SimToWorld * vec4(k == 0u ? p.PosAge.xyz : trail.Points[base + k - 1u].xyz, 1.0)).xyz;
    vec3 b = (P.SimToWorld * vec4(older.xyz, 1.0)).xyz;
    vec3 axis = b - a, mid = (a + b) * 0.5;
    vec3 side = cross(axis, uScene.CameraPosition.xyz - mid);
    float life = clamp(p.PosAge.w / p.VelLife.w, 0.0, 1.0);
    float fade = 1.0 - float(k) / float(max(recorded, 1u));
    float width = p.Misc.x * SampleCurveLut(P, life).x * P.Misc.y * P.Trail.z * fade;
    if (dot(axis, axis) < 1e-10 || dot(side, side) < 1e-14 || width <= 0.0)
    {
        Discard();
        return;
    }
    side = normalize(side) * width;
    vColor = p.Color * SampleColorLut(P, life);
    vColor.a *= fade;
    vec3 c0 = max(vColor.rgb, vec3(0.0)); // authored sRGB to linear, as the CPU path
    vColor.rgb = mix(c0 / 12.92, pow((c0 + 0.055) / 1.055, vec3(2.4)), greaterThan(c0, vec3(0.04045)));
    vec2 q = vec2(corner == 1u || corner == 2u ? 0.5 : -0.5, corner >= 2u ? 0.5 : -0.5);
    vUV = vec2(0.5, q.y < 0.0 ? 1.0 : 0.0); // soft falloff across the width, constant along the length
    gl_Position = uScene.ViewProj * vec4(mid + axis * q.x + side * q.y, 1.0);
}
