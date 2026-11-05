#version 460
#extension GL_GOOGLE_include_directive : require
#include "Common.glsl"
#include "ParticleCommon.glsl"
#include "ParticleLighting.glsl"

// Draws GPU particles as camera-facing quads straight from the simulation buffer: four vertices per slot (indexed
// with the shared quad indices), dead slots collapse to a point. Feeds Sprite.frag.

layout(push_constant) uniform Push
{
    uvec2 Particles;
    uvec2 Params;
    uint Texture;
    uint Entity;
    uvec2 Sorted; // back-to-front slot order (ParticleSort), 0 = draw in slot order
} pc;

layout(buffer_reference, std430, buffer_reference_align = 8) readonly buffer SortedRef { uvec2 Items[]; };

layout(location = 0) out vec4 vColor;
layout(location = 1) out vec2 vUV;
layout(location = 2) flat out uint vTexture;
layout(location = 3) flat out uint vEntity;

void main()
{
    GpuParticleParamsRef P = GpuParticleParamsRef(pc.Params);
    uint index = uint(gl_VertexIndex) / 4u, corner = uint(gl_VertexIndex) % 4u;
    if (pc.Sorted != uvec2(0u)) index = SortedRef(pc.Sorted).Items[index].y;
    vTexture = pc.Texture;
    vEntity = pc.Entity;
    bool valid = index < P.Counts.x;
    GpuParticle p;
    if (valid) p = GpuParticlesRef(pc.Particles).Items[index];
    if (!valid || !ParticleAlive(p))
    {
        vColor = vec4(0.0);
        vUV = vec2(0.0);
        gl_Position = vec4(0.0, 0.0, 2.0, 1.0); // outside the depth range: clipped
        return;
    }
    float life = clamp(p.PosAge.w / p.VelLife.w, 0.0, 1.0);
    vec3 pos = (P.SimToWorld * vec4(p.PosAge.xyz, 1.0)).xyz;
    float size = p.Misc.x * SampleCurveLut(P, life).x * P.Misc.y;
    vColor = p.Color * SampleColorLut(P, life);
    vec3 c0 = max(vColor.rgb, vec3(0.0)); // authored sRGB to linear, as the CPU path
    vColor.rgb = mix(c0 / 12.92, pow((c0 + 0.055) / 1.055, vec3(2.4)), greaterThan(c0, vec3(0.04045)));
    if (P.Lighting.x > 0.5) vColor.rgb *= ParticleLight(pos);
    vec3 right = vec3(uScene.View[0][0], uScene.View[1][0], uScene.View[2][0]);
    vec3 up = vec3(uScene.View[0][1], uScene.View[1][1], uScene.View[2][1]);
    float c = cos(p.Misc.y), s = sin(p.Misc.y);
    vec3 r = (right * c + up * s) * size, u = (up * c - right * s) * size;
    vec2 q = vec2(corner == 1u || corner == 2u ? 0.5 : -0.5, corner >= 2u ? 0.5 : -0.5);
    vec3 world = pos + r * q.x + u * q.y;

    // Flipbook frame (as ParticleSystemComponent::SheetFrame / SheetUV).
    vec2 uvMin = vec2(0.0, 1.0), uvMax = vec2(1.0, 0.0);
    if (P.Sheet.w != 0u)
    {
        uint cols = max(P.Sheet.x, 1u), rows = max(P.Sheet.y, 1u);
        uint frames = P.Sheet.z == 0u ? cols * rows : min(P.Sheet.z, cols * rows);
        float t = fract(min(life, 0.9999) * max(P.Misc.w, 0.0) + p.Misc.w);
        uint frame = min(uint(t * float(frames)), frames - 1u);
        float col = float(frame % cols), row = float(frame / cols);
        uvMin = vec2(col / float(cols), (row + 1.0) / float(rows));
        uvMax = vec2((col + 1.0) / float(cols), row / float(rows));
    }
    vUV = vec2(q.x < 0.0 ? uvMin.x : uvMax.x, q.y < 0.0 ? uvMin.y : uvMax.y);
    gl_Position = uScene.ViewProj * vec4(world, 1.0);
}
