#version 460
#extension GL_GOOGLE_include_directive : require
#include "Common.glsl"

layout(location = 0) in vec3 vDirection;
layout(location = 0) out vec4 outColor;
layout(location = 1) out uint outEntity; // write-masked

// Volumetric clouds: a layer between two altitudes filled with drifting fractal noise (thinned by Coverage, rounded
// at its base and top), marched along the view ray. Each step is lit by the sun through the cloud above it (a short
// march toward the sun, forward-scattering) and by the sky's light from above.
float Hash3(vec3 p)
{
    p = fract(p * 0.3183099 + 0.1);
    p *= 17.0;
    return fract(p.x * p.y * p.z * (p.x + p.y + p.z));
}
float ValueNoise(vec3 p)
{
    vec3 i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(mix(Hash3(i), Hash3(i + vec3(1, 0, 0)), f.x), mix(Hash3(i + vec3(0, 1, 0)), Hash3(i + vec3(1, 1, 0)), f.x), f.y),
               mix(mix(Hash3(i + vec3(0, 0, 1)), Hash3(i + vec3(1, 0, 1)), f.x), mix(Hash3(i + vec3(0, 1, 1)), Hash3(i + vec3(1, 1, 1)), f.x), f.y), f.z);
}
float CloudDensity(vec3 p, int octaves)
{
    vec4 c = uScene.CloudParams;
    float h = (p.y - c.z) / max(c.w - c.z, 1.0);
    if (h <= 0.0 || h >= 1.0) return 0.0;
    vec3 q = (p - vec3(uScene.Wind.x, 0.0, uScene.Wind.z) * (uScene.Wind.w * 15.0)) / max(uScene.CloudParams2.x, 1.0);
    float n = 0.0, amp = 0.55, total = 0.0;
    for (int o = 0; o < octaves; ++o)
    {
        n += ValueNoise(q) * amp;
        total += amp;
        q = q * 2.03 + vec3(17.1, 3.7, 9.3);
        amp *= 0.5;
    }
    n /= total;
    float shape = smoothstep(0.0, 0.2, h) * smoothstep(1.0, 0.55, h); // flat-ish bases, rounded tops
    return max(n * shape - (1.0 - c.x), 0.0) / max(c.x, 0.05) * c.y;
}
// In-scattered light (rgb) and transmittance (a) of the cloud layer along a view direction.
vec4 Clouds(vec3 dir)
{
    if (uScene.CloudParams2.z < 0.5 || dir.y <= 0.0) return vec4(0.0, 0.0, 0.0, 1.0);
    vec3 cam = uScene.CameraPosition.xyz;
    float bottom = uScene.CloudParams.z, top = uScene.CloudParams.w;
    float t0 = max((bottom - cam.y) / dir.y, 0.0), t1 = (top - cam.y) / dir.y;
    if (t1 <= t0) return vec4(0.0, 0.0, 0.0, 1.0);
    t1 = min(t1, t0 + 8000.0); // long grazing paths near the horizon: march only the first stretch
    int steps = clamp(int(uScene.CloudParams2.y), 8, 128);
    float dt = (t1 - t0) / float(steps);
    float jitter = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
    vec3 toSun = uScene.LightDirection.w > 0.5 ? normalize(-uScene.LightDirection.xyz) : vec3(0.0, 1.0, 0.0);
    vec3 sunColor = uScene.LightDirection.w > 0.5 ? uScene.LightColor.rgb : vec3(0.0);
    float mu = dot(dir, toSun), g = 0.6;
    float phase = (1.0 - g * g) / (4.0 * 3.14159265 * pow(1.0 + g * g - 2.0 * g * mu, 1.5)) * 4.0 + 0.3; // forward glow
    vec3 ambient = uScene.SkyTop.rgb * 0.8 + uScene.Ambient.rgb;
    float transmittance = 1.0;
    vec3 light = vec3(0.0);
    for (int i = 0; i < steps && transmittance > 0.01; ++i)
    {
        vec3 p = cam + dir * (t0 + (float(i) + jitter) * dt);
        float d = CloudDensity(p, 4);
        if (d <= 0.0) continue;
        // Toward the sun: how much cloud the light passes through to get here.
        float toLight = 0.0, ls = (top - bottom) * 0.12;
        for (int k = 1; k <= 4; ++k) toLight += CloudDensity(p + toSun * (ls * float(k)), 2) * ls;
        float h = clamp((p.y - bottom) / max(top - bottom, 1.0), 0.0, 1.0);
        vec3 scatter = sunColor * exp(-toLight) * phase + ambient * mix(0.4, 1.0, h);
        float stepT = exp(-d * dt);
        light += transmittance * (1.0 - stepT) * scatter;
        transmittance *= stepT;
    }
    // Far away the clouds fade into the sky's haze.
    float haze = smoothstep(15000.0, 40000.0, t0);
    return vec4(light * (1.0 - haze), mix(transmittance, 1.0, haze));
}

// Procedural gradient sky with a sun disk following the directional light (or the sky texture), under the clouds.
void main()
{
    vec3 dir = normalize(vDirection);
    vec3 sky;
    if (HasSkyTexture())
        sky = SkyRadiance(dir, 0.0);
    else
    {
        sky = SkyColor(dir);
        if (uScene.LightDirection.w > 0.5)
        {
            vec3 toSun = normalize(-uScene.LightDirection.xyz);
            float c = max(dot(dir, toSun), 0.0);
            sky += uScene.LightColor.rgb * (pow(c, 900.0) * 6.0 + pow(c, 12.0) * 0.08);
        }
    }
    vec4 clouds = Clouds(dir);
    outColor = vec4(sky * clouds.a + clouds.rgb, 1.0);
    outEntity = 0u;
}
