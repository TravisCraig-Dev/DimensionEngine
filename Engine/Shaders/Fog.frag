#version 460
#extension GL_GOOGLE_include_directive : require
#include "Common.glsl"

// Volumetric fog: marches each view ray through height fog up to the visible surface (or Max Distance for the sky),
// adding in-scattered light (the fog's ambient color, and sunlight where the shadow map says the sun reaches, shaped by
// a Henyey-Greenstein phase function: light shafts) and dimming what lies behind. Output is premultiplied:
// result = scene * (1 - alpha) + rgb.

layout(push_constant) uniform Push
{
    vec4 Color;       // rgb: ambient light in the fog (linear), a: sunlight scattering strength
    uint Depth;       // bindless depth
    uint Steps;
    float Density;    // extinction per meter at the base height
    float HeightFalloff; // how fast it thins above the base height (per meter)
    float BaseHeight;
    float MaxDistance;
    float Anisotropy; // -1..1: forward scattering (> 0) brightens looking toward the sun
    float Noise;      // per-frame offset of the step jitter
} pc;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

float Hash(vec2 p) { return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715)))); }

float DensityAt(vec3 p) { return pc.Density * exp(-max(p.y - pc.BaseHeight, 0.0) * pc.HeightFalloff); }

float SunVisibility(vec3 p)
{
    int count = int(uScene.ShadowParams.x);
    if (count == 0) return 1.0;
    float depth = -(uScene.View * vec4(p, 1.0)).z;
    for (int c = 0; c < count; ++c)
    {
        if (depth > uScene.CascadeSplits[c]) continue;
        float s = SampleCascadePoint(c, p); // one reading: the jittered steps (and TAA) smooth the shafts
        if (s >= 0.0) return s;
    }
    return 1.0;
}

void main()
{
    float depth = textureLod(uTextures[nonuniformEXT(pc.Depth)], vUV, 0.0).r;
    vec4 far = uScene.InvViewProj * vec4(vUV.x * 2.0 - 1.0, 1.0 - vUV.y * 2.0, depth >= 1.0 ? 1.0 : depth, 1.0);
    vec3 end = far.xyz / far.w;
    vec3 start = uScene.CameraPosition.xyz;
    vec3 ray = end - start;
    float len = length(ray);
    vec3 dir = ray / max(len, 1e-4);
    float dist = depth >= 1.0 ? pc.MaxDistance : min(len, pc.MaxDistance);

    bool sun = uScene.LightDirection.w > 0.5 && pc.Color.a > 0.0;
    float g = clamp(pc.Anisotropy, -0.95, 0.95);
    float cosTheta = dot(normalize(uScene.LightDirection.xyz), -dir);
    float phase = (1.0 - g * g) / (4.0 * 3.14159265 * pow(1.0 + g * g - 2.0 * g * cosTheta, 1.5));
    vec3 sunLight = uScene.LightColor.rgb * pc.Color.a * phase * 4.0 * 3.14159265; // phase 1 when isotropic

    uint steps = clamp(pc.Steps, 4u, 128u);
    float dt = dist / float(steps);
    float jitter = fract(Hash(gl_FragCoord.xy) + pc.Noise);
    float transmittance = 1.0;
    vec3 light = vec3(0.0);
    for (uint i = 0u; i < steps; ++i)
    {
        vec3 p = start + dir * ((float(i) + jitter) * dt);
        float sigma = DensityAt(p);
        if (sigma <= 1e-6) continue;
        vec3 inscatter = pc.Color.rgb;
        if (sun) inscatter += sunLight * SunVisibility(p);
        // Energy-conserving integration of a homogeneous step.
        float stepT = exp(-sigma * dt);
        light += transmittance * inscatter * (1.0 - stepT);
        transmittance *= stepT;
        if (transmittance < 0.003) break;
    }
    outColor = vec4(light, 1.0 - transmittance);
}
