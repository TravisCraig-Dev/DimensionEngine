#version 460
#extension GL_GOOGLE_include_directive : require
#include "Common.glsl"

// Projected decal: the scene point under the pixel (from the depth buffer) in the decal box's space; inside the box
// its X and Y pick the texel (projected along the box's -Z), and the texture is blended onto the scene color.
// Surfaces turned away from the projection direction fade out.

layout(push_constant) uniform Push
{
    mat4 Model;
    vec4 Color;
    uint Texture;
    uint Depth;
    uint Blend;
    float AngleFade;
} pc;

layout(location = 0) out vec4 outColor;

vec3 WorldAt(vec2 uv)
{
    float d = textureLod(uTextures[nonuniformEXT(pc.Depth)], uv, 0.0).r;
    vec4 p = uScene.InvViewProj * vec4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, d, 1.0);
    return p.xyz / p.w;
}

void main()
{
    vec2 size = vec2(textureSize(uTextures[nonuniformEXT(pc.Depth)], 0));
    vec2 uv = gl_FragCoord.xy / size;
    float d = textureLod(uTextures[nonuniformEXT(pc.Depth)], uv, 0.0).r;
    vec3 world = WorldAt(uv);
    // Surface normal from neighbouring depths (before any discard, so the derivatives are valid).
    vec3 n = normalize(cross(dFdx(world), dFdy(world)));
    if (d >= 1.0) discard; // sky
    vec3 local = (inverse(pc.Model) * vec4(world, 1.0)).xyz;
    if (any(greaterThan(abs(local), vec3(0.5)))) discard;

    // Fade where the surface turns away from the projector (its -Z axis).
    vec3 forward = normalize(mat3(pc.Model) * vec3(0.0, 0.0, -1.0));
    float facing = abs(dot(n, forward));
    float fade = smoothstep(pc.AngleFade * 0.5, max(pc.AngleFade, 1e-3), facing);
    fade *= smoothstep(0.5, 0.42, abs(local.z)); // soft ends along the projection

    vec4 tex = SampleTexture(pc.Texture, vec2(local.x + 0.5, 0.5 - local.y)) * pc.Color;
    float a = tex.a * fade;
    if (a <= 0.002) discard;
    if (pc.Blend == 1u) outColor = vec4(mix(vec3(1.0), tex.rgb, a), 1.0); // multiply: darkens (dirt, scorch marks)
    else outColor = vec4(tex.rgb, a);                                       // alpha or additive
}
