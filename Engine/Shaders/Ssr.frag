#version 460
#extension GL_GOOGLE_include_directive : require
#include "Common.glsl"

// Screen-space reflections: marches the reflected view ray through the depth buffer and, where it hits something on
// screen, outputs how much that hit's color should replace the environment reflection the surface was shaded with
// (weight * (hit - environment)). SsrApply.frag adds the result onto the HDR image.

layout(set = 0, binding = 0) uniform usampler2D uUintTextures[]; // the same bindless array, for the uint surface buffer

layout(push_constant) uniform Push
{
    uint Hdr;          // bindless: scene color (this frame, before reflections)
    uint Depth;        // bindless: scene depth (nearest)
    uint Surface;      // bindless: surface buffer (normal, reflection weight, roughness)
    uint Steps;        // march steps
    vec2 Texel;        // 1 / framebuffer size
    float MaxDistance; // view-space ray length
    float Thickness;   // how far behind a depth sample a ray still counts as hitting it
    float MaxRoughness; // reflections fade out toward this roughness
    float Intensity;
} pc;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

float Depth(vec2 uv) { return textureLod(uTextures[nonuniformEXT(pc.Depth)], uv, 0.0).r; }

vec3 ViewPosition(vec2 uv, float depth)
{
    // Rendered with a flipped viewport: texture row 0 is NDC y = +1.
    vec4 p = uScene.InvProj * vec4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, depth, 1.0);
    return p.xyz / p.w;
}

vec2 ProjectToUV(vec3 viewPos)
{
    vec4 clip = uScene.Proj * vec4(viewPos, 1.0);
    return vec2(clip.x / clip.w * 0.5 + 0.5, 0.5 - clip.y / clip.w * 0.5);
}

float Hash(vec2 p) { return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715)))); } // interleaved gradient noise

void main()
{
    outColor = vec4(0.0);
    uvec2 surface = texelFetch(uUintTextures[nonuniformEXT(pc.Surface)], ivec2(gl_FragCoord.xy), 0).xy; // (z: diffuse color, for GI)
    if (surface.y == 0u) return; // sky, sprites, transparent or unlit pixels
    vec4 weightRough = unpackUnorm4x8(surface.y);
    float roughness = weightRough.a;
    if (roughness >= pc.MaxRoughness) return;
    float depth = Depth(vUV);
    if (depth >= 1.0) return;

    vec3 P = ViewPosition(vUV, depth);
    vec3 Nw = OctDecode(unpackHalf2x16(surface.x));
    vec3 N = normalize(mat3(uScene.View) * Nw);
    vec3 V = normalize(P);
    vec3 R = normalize(reflect(V, N));

    // March with steps that grow with distance (detail near the surface, reach far away), jittered per pixel.
    uint steps = clamp(pc.Steps, 8u, 256u);
    float jitter = Hash(gl_FragCoord.xy);
    float prevT = 0.0;
    vec2 hitUV = vec2(-1.0);
    float hitT = 0.0;
    vec3 start = P + N * (0.01 * max(1.0, -P.z * 0.05)); // off the surface, against self-hits
    for (uint i = 0u; i < steps; ++i)
    {
        float f = (float(i) + jitter) / float(steps);
        float t = pc.MaxDistance * f * f;
        vec3 Q = start + R * t;
        if (Q.z > -0.01) break; // behind the camera
        vec2 uv = ProjectToUV(Q);
        if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) break;
        float sceneZ = ViewPosition(uv, Depth(uv)).z;
        float behind = sceneZ - Q.z; // > 0: the ray is behind the visible surface
        float thickness = pc.Thickness + (t - prevT);
        if (behind > 0.0 && behind < thickness)
        {
            // Refine between the last two samples.
            float lo = prevT, hi = t;
            for (int k = 0; k < 6; ++k)
            {
                float mid = 0.5 * (lo + hi);
                vec3 M = start + R * mid;
                vec2 muv = ProjectToUV(M);
                if (ViewPosition(muv, Depth(muv)).z - M.z > 0.0) hi = mid;
                else lo = mid;
            }
            // Only surfaces facing the ray reflect into it: a hit on one facing away (the surface the ray left, at
            // grazing angles, or the back of something it passed) is skipped and the march goes on.
            vec2 candidate = ProjectToUV(start + R * hi);
            uvec2 hitSurface = texelFetch(uUintTextures[nonuniformEXT(pc.Surface)], ivec2(candidate / pc.Texel), 0).xy;
            if (hitSurface.y != 0u && dot(normalize(mat3(uScene.View) * OctDecode(unpackHalf2x16(hitSurface.x))), R) > 0.0)
            {
                prevT = t;
                continue;
            }
            hitT = hi;
            hitUV = candidate;
            break;
        }
        prevT = t;
    }
    if (hitUV.x < 0.0) return;

    // Confidence: fade near the screen edges, toward the end of the ray, for rays coming back at the camera, and with
    // roughness (the hit is sampled sharp).
    vec2 edge = min(hitUV, 1.0 - hitUV);
    float fade = smoothstep(0.0, 0.08, min(edge.x, edge.y));
    fade *= 1.0 - smoothstep(0.7, 1.0, hitT / pc.MaxDistance);
    fade *= 1.0 - smoothstep(0.4, 0.9, dot(R, -V) * 0.5 + 0.5);
    fade *= 1.0 - smoothstep(pc.MaxRoughness * 0.5, pc.MaxRoughness, roughness);
    if (fade <= 0.0) return;

    vec3 hit = textureLod(uTextures[nonuniformEXT(pc.Hdr)], hitUV, 0.0).rgb;
    vec3 worldPos = (inverse(uScene.View) * vec4(P, 1.0)).xyz;
    vec3 Rw = normalize(transpose(mat3(uScene.View)) * R);
    vec3 env = EnvironmentReflection(worldPos, Rw, roughness);
    outColor = vec4(weightRough.rgb * (hit - env) * fade * pc.Intensity, 1.0);
}
