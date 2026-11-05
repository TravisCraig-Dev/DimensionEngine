#version 460
#extension GL_GOOGLE_include_directive : require
#include "Common.glsl"
#include "ParticleCommon.glsl"

// Motion vectors of GPU particles, straight from the simulation buffer as ParticleGpu.vert draws them: each corner
// where it is now and where it was a step ago (its velocity times the simulation step). Feeds VelocitySprite.frag.

layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer FrameRef
{
    mat4 ViewProj;     // this frame's, unjittered
    mat4 PrevViewProj; // last frame's, unjittered
};
layout(buffer_reference, std430, buffer_reference_align = 8) readonly buffer SortedRef { uvec2 Items[]; };

layout(push_constant) uniform Push
{
    FrameRef Frame;
    uint Depth; // bindless: the scene depth (VelocitySprite.frag)
    uint Pad;
    uvec2 Particles;
    uvec2 Params;
    uvec2 Sorted; // as ParticleGpu.vert
    uint Texture;
} pc;

layout(location = 0) out vec4 vCurrent;
layout(location = 1) out vec4 vPrevious;
layout(location = 2) out vec2 vUV;
layout(location = 3) flat out uint vTexture;
layout(location = 4) out float vAlpha;

void main()
{
    GpuParticleParamsRef P = GpuParticleParamsRef(pc.Params);
    uint index = uint(gl_VertexIndex) / 4u, corner = uint(gl_VertexIndex) % 4u;
    if (pc.Sorted != uvec2(0u)) index = SortedRef(pc.Sorted).Items[index].y;
    vTexture = pc.Texture;
    bool valid = index < P.Counts.x;
    GpuParticle p;
    if (valid) p = GpuParticlesRef(pc.Particles).Items[index];
    if (!valid || !ParticleAlive(p))
    {
        vCurrent = vPrevious = vec4(0.0, 0.0, 0.0, 1.0);
        vUV = vec2(0.0);
        vAlpha = 0.0;
        gl_Position = vec4(0.0, 0.0, 2.0, 1.0); // clipped
        return;
    }
    float life = clamp(p.PosAge.w / p.VelLife.w, 0.0, 1.0);
    vec3 pos = (P.SimToWorld * vec4(p.PosAge.xyz, 1.0)).xyz;
    vec3 step = mat3(P.SimToWorld) * p.VelLife.xyz * P.Spin.w;
    float size = p.Misc.x * SampleCurveLut(P, life).x * P.Misc.y;
    vAlpha = p.Color.a * SampleColorLut(P, life).a;
    vec3 right = vec3(uScene.View[0][0], uScene.View[1][0], uScene.View[2][0]);
    vec3 up = vec3(uScene.View[0][1], uScene.View[1][1], uScene.View[2][1]);
    float c = cos(p.Misc.y), s = sin(p.Misc.y);
    vec3 r = (right * c + up * s) * size, u = (up * c - right * s) * size;
    vec2 q = vec2(corner == 1u || corner == 2u ? 0.5 : -0.5, corner >= 2u ? 0.5 : -0.5);
    vec3 world = pos + r * q.x + u * q.y;

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
    vCurrent = pc.Frame.ViewProj * vec4(world, 1.0);
    vPrevious = pc.Frame.PrevViewProj * vec4(world - step, 1.0);
    gl_Position = uScene.ViewProj * vec4(world, 1.0);
}
