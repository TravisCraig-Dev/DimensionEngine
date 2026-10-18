#version 460
#extension GL_GOOGLE_include_directive : require
#include "Common.glsl"

// Screen-space ambient occlusion (hemisphere sampling around the view-space position reconstructed from depth).

layout(push_constant) uniform Push
{
    uint Depth;      // bindless index of the scene depth
    uint Samples;
    vec2 DepthTexel; // 1 / depth size
    float Radius;    // world units
    float Power;
    float Bias;
    float Pad;
} pc;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outAO;

// Snapped to depth texel centers: half-resolution pixels sit exactly between full-resolution texels.
vec2 Snap(vec2 uv) { return (floor(uv / pc.DepthTexel) + 0.5) * pc.DepthTexel; }
float Depth(vec2 uv) { return textureLod(uTextures[nonuniformEXT(pc.Depth)], uv, 0.0).r; }

vec3 ViewPosition(vec2 uv, float depth)
{
    // Rendered with a flipped viewport: texture row 0 is NDC y = +1.
    vec4 p = uScene.InvProj * vec4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, depth, 1.0);
    return p.xyz / p.w;
}

vec3 ViewPosition(vec2 uv) { return ViewPosition(uv, Depth(uv)); }

float Hash(vec2 p) { return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715)))); } // interleaved gradient noise

void main()
{
    vec2 uv = Snap(vUV);
    float depth = Depth(uv);
    if (depth >= 1.0)
    {
        outAO = vec4(1.0);
        return;
    }
    vec3 P = ViewPosition(uv, depth);

    // Normal from neighbouring depths, using the smaller difference on each axis to avoid smearing across edges.
    vec3 r = ViewPosition(uv + vec2(pc.DepthTexel.x, 0.0)) - P, l = P - ViewPosition(uv - vec2(pc.DepthTexel.x, 0.0));
    vec3 d = ViewPosition(uv + vec2(0.0, pc.DepthTexel.y)) - P, u = P - ViewPosition(uv - vec2(0.0, pc.DepthTexel.y));
    vec3 dx = abs(r.z) < abs(l.z) ? r : l;
    vec3 dy = abs(d.z) < abs(u.z) ? d : u;
    vec3 N = normalize(cross(dx, dy));
    if (dot(N, -P) < 0.0) N = -N;

    // Per-pixel random rotation of the kernel (removed by the blur).
    float angle = Hash(gl_FragCoord.xy) * 6.28318530718;
    vec3 rnd = vec3(cos(angle), sin(angle), 0.0);
    vec3 T = normalize(rnd - N * dot(rnd, N));
    if (any(isnan(T))) T = normalize(cross(N, vec3(0.0, 1.0, 0.0)));
    mat3 TBN = mat3(T, cross(N, T), N);

    float bias = pc.Bias * max(1.0, -P.z * 0.1); // depth precision falls off with distance
    float occlusion = 0.0;
    uint count = max(pc.Samples, 1u);
    for (uint i = 0u; i < count; ++i)
    {
        // Fibonacci hemisphere, denser near the center; stays ~25 degrees above the tangent plane so flat surfaces
        // do not occlude themselves at grazing angles.
        float f = (float(i) + 0.5) / float(count);
        float z = sqrt(1.0 - f * 0.82);
        float phi = float(i) * 2.39996323;
        float s = sqrt(f * 0.82);
        vec3 k = vec3(cos(phi) * s, sin(phi) * s, z);
        float scale = mix(0.1, 1.0, f * f);
        vec3 S = P + TBN * k * pc.Radius * scale;

        vec4 clip = uScene.Proj * vec4(S, 1.0);
        vec2 suv = vec2(clip.x / clip.w * 0.5 + 0.5, 0.5 - clip.y / clip.w * 0.5);
        if (any(lessThan(suv, vec2(0.0))) || any(greaterThan(suv, vec2(1.0)))) continue;
        float sceneZ = ViewPosition(Snap(suv)).z;
        float range = smoothstep(0.0, 1.0, pc.Radius / max(abs(P.z - sceneZ), 1e-4));
        occlusion += (sceneZ >= S.z + bias ? 1.0 : 0.0) * range;
    }
    float ao = 1.0 - occlusion / float(count);
    outAO = vec4(pow(clamp(ao, 0.0, 1.0), pc.Power));
}
