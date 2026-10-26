#version 460
#extension GL_GOOGLE_include_directive : require
#include "Common.glsl"

// Screen-space global illumination: from each pixel, a few cosine-weighted rays march through the depth buffer, and
// where one meets a surface on screen, that surface's lit color is light bouncing onto the pixel. The average times
// the pixel's diffuse color (from the surface buffer) is added onto the HDR image by SsrApply.frag (with its blur).
// Rays turn per pixel and per frame, so TAA averages the noise away.

layout(set = 0, binding = 0) uniform usampler2D uUintTextures[]; // the same bindless array, for the uint surface buffer

layout(push_constant) uniform Push
{
    uint Hdr;          // bindless: scene color (this frame, direct and ambient light)
    uint Depth;        // bindless: scene depth (nearest)
    uint Surface;      // bindless: surface buffer (normal, ..., diffuse color)
    uint Rays;
    vec2 Texel;        // 1 / framebuffer size
    float MaxDistance; // view-space ray length
    float Thickness;   // how far behind a depth sample a ray still counts as hitting it
    float Intensity;
    uint Steps;
    float Scale;       // full-resolution pixels per traced pixel
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
    uvec4 surface = texelFetch(uUintTextures[nonuniformEXT(pc.Surface)], ivec2(gl_FragCoord.xy * pc.Scale), 0);
    vec3 diffuse = unpackUnorm4x8(surface.z).rgb;
    if (surface.x == 0u || dot(diffuse, diffuse) <= 0.0) return; // sky, sprites, transparent or black surfaces
    float depth = Depth(vUV);
    if (depth >= 1.0) return;

    vec3 P = ViewPosition(vUV, depth);
    vec3 N = normalize(mat3(uScene.View) * OctDecode(unpackHalf2x16(surface.x)));
    vec3 T = normalize(cross(N, abs(N.y) < 0.9 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0)));
    vec3 B = cross(N, T);
    float jitter = fract(uScene.Wind.w * 7.31);
    float noise = Hash(gl_FragCoord.xy + jitter * 61.7), noise2 = Hash(gl_FragCoord.yx * 1.37 + 11.3 + jitter * 37.1);
    uint rays = clamp(pc.Rays, 1u, 16u), steps = clamp(pc.Steps, 4u, 64u);
    float stepLength = pc.MaxDistance / float(steps);
    vec3 sum = vec3(0.0);
    for (uint r = 0u; r < rays; ++r)
    {
        // Cosine-weighted direction (a different one per ray, pixel and frame).
        float u1 = fract(noise + float(r) * 0.618034), u2 = fract(noise2 + float(r) * 0.754878);
        float phi = 6.28318530718 * u1, sinTheta = sqrt(u2), cosTheta = sqrt(1.0 - u2);
        vec3 dir = T * (cos(phi) * sinTheta) + B * (sin(phi) * sinTheta) + N * cosTheta;
        vec3 origin = P + N * (0.02 * -P.z * 0.05 + 0.01); // off the surface, more so far away
        for (uint s = 1u; s <= steps; ++s)
        {
            vec3 Q = origin + dir * (stepLength * (float(s) - 0.5 + noise));
            vec2 uv = ProjectToUV(Q);
            if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0))) || Q.z >= 0.0) break; // off screen
            float d = Depth(uv);
            if (d >= 1.0) continue;
            float sceneZ = ViewPosition(uv, d).z;
            float behind = sceneZ - Q.z; // > 0: the ray went behind what the camera sees there
            if (behind > 0.0 && behind < pc.Thickness)
            {
                sum += textureLod(uTextures[nonuniformEXT(pc.Hdr)], uv, 0.0).rgb;
                break;
            }
        }
    }
    // Lambert with cosine-weighted rays: the mean incoming light times the diffuse color.
    outColor = vec4(min(sum / float(rays), vec3(64.0)) * diffuse * pc.Intensity, 1.0);
}
