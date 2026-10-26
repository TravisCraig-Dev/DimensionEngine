// Volumetric clouds' density (shared by Sky.frag, which draws them, and MeshSurface.glsl, whose sunlight they shade):
// a layer between two altitudes filled with drifting fractal noise, thinned by Coverage, rounded at its base and top.
// Needs Common.glsl (uScene).

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

// How much sunlight reaches a point through the cloud layer (1 = clear): a few samples where the sun's ray crosses it.
float CloudShadow(vec3 p)
{
    if (uScene.CloudParams2.z < 0.5 || uScene.CloudParams2.w < 0.5 || uScene.LightDirection.w < 0.5) return 1.0;
    vec3 toSun = normalize(-uScene.LightDirection.xyz);
    if (toSun.y <= 0.02) return 1.0;
    float bottom = uScene.CloudParams.z, top = uScene.CloudParams.w;
    float t0 = max((bottom - p.y) / toSun.y, 0.0), t1 = (top - p.y) / toSun.y;
    if (t1 <= t0) return 1.0;
    float dt = (t1 - t0) / 6.0, depth = 0.0;
    for (int i = 0; i < 6; ++i) depth += CloudDensity(p + toSun * (t0 + (float(i) + 0.5) * dt), 3) * dt;
    return exp(-depth);
}
