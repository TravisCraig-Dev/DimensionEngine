// The mesh fragment shader's body, shared by Mesh.frag (the main pass) and MeshOit.frag (order-independent
// transparency, with DIM_OIT defined).
#include "Clouds.glsl"

// Clustered point and spot lights (see LightClusters).
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer LightListRef { PointLight Lights[]; };
layout(buffer_reference, std430, buffer_reference_align = 8) readonly buffer ClusterCellsRef { uvec2 Cells[]; };
layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer ClusterIndexRef { uint Indices[]; };
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer ProbeSHRef { vec4 Coefficients[]; };

// Light probe volumes: irradiance at `pos` from the 8 probes of its grid cell (SH blended trilinearly), and how much it
// counts (1 inside, fading out over Blend Distance toward the box's walls). The first volume containing the point wins.
float ProbeVolumeIrradiance(vec3 pos, vec3 n, out vec3 irradiance)
{
    irradiance = vec3(0.0);
    uint count = uScene.ProbeVolumeBuffer.z;
    if (count == 0u) return 0.0;
    ProbeSHRef probes = ProbeSHRef(uScene.ProbeVolumeBuffer.xy);
    for (uint v = 0u; v < count; ++v)
    {
        vec4 lo = uScene.ProbeVolumes[v * 3u], hi = uScene.ProbeVolumes[v * 3u + 1u], info = uScene.ProbeVolumes[v * 3u + 2u];
        vec3 local = (pos - lo.xyz) / max(hi.xyz - lo.xyz, vec3(1e-4));
        if (any(lessThan(local, vec3(0.0))) || any(greaterThan(local, vec3(1.0)))) continue;
        vec3 inside = min(pos - lo.xyz, hi.xyz - pos);
        float weight = info.z > 0.0 ? clamp(min(inside.x, min(inside.y, inside.z)) / info.z, 0.0, 1.0) : 1.0;
        ivec3 res = ivec3(int(lo.w), int(hi.w), int(info.x));
        vec3 g = local * vec3(max(res - 1, ivec3(0)));
        ivec3 i0 = clamp(ivec3(floor(g)), ivec3(0), max(res - 2, ivec3(0)));
        vec3 f = clamp(g - vec3(i0), 0.0, 1.0);
        vec3 sh[9];
        for (int k = 0; k < 9; ++k) sh[k] = vec3(0.0);
        for (int c = 0; c < 8; ++c)
        {
            ivec3 o = ivec3(c & 1, (c >> 1) & 1, (c >> 2) & 1);
            ivec3 p = min(i0 + o, res - 1);
            float w = (o.x == 1 ? f.x : 1.0 - f.x) * (o.y == 1 ? f.y : 1.0 - f.y) * (o.z == 1 ? f.z : 1.0 - f.z);
            uint index = uint(info.y) + uint((p.z * res.y + p.y) * res.x + p.x);
            for (int k = 0; k < 9; ++k) sh[k] += probes.Coefficients[index * 9u + uint(k)].rgb * w;
        }
        irradiance = EvaluateSH(sh, n) * info.w;
        return weight;
    }
    return 0.0;
}

// A terrain layer's texel, read from above (world x/z) or, triplanar, from the three axes weighted by the normal.
vec4 TerrainSample(uint map, vec3 world, float tiling, bool triplanar, vec3 weights)
{
    vec4 top = SampleTexture(map, world.xz * tiling);
    if (!triplanar) return top;
    vec4 c = top * weights.y;
    if (weights.x > 0.01) c += SampleTexture(map, world.zy * tiling) * weights.x;
    if (weights.z > 0.01) c += SampleTexture(map, world.xy * tiling) * weights.z;
    return c;
}

// A terrain layer's normal map bent onto the surface (world space). From above: east along the image, north (-Z) up it.
// Triplanar: each axis's reading added to the surface normal in that plane (Golus's UDN blend), weighted.
vec3 TerrainNormal(uint map, vec3 world, float tiling, float strength, bool triplanar, vec3 weights, vec3 N)
{
    vec3 tY = SampleTexture(map, world.xz * tiling).xyz * 2.0 - 1.0;
    tY.xy *= strength;
    vec3 T = normalize(vec3(1.0, 0.0, 0.0) - N * N.x);
    vec3 fromTop = mat3(T, cross(N, T), N) * tY;
    if (!triplanar) return fromTop;
    vec3 n = fromTop * weights.y;
    if (weights.x > 0.01)
    {
        vec2 t = (SampleTexture(map, world.zy * tiling).xy * 2.0 - 1.0) * strength;
        n += normalize(vec3(N.x, t.y + N.y, t.x + N.z)) * weights.x;
    }
    if (weights.z > 0.01)
    {
        vec2 t = (SampleTexture(map, world.xy * tiling).xy * 2.0 - 1.0) * strength;
        n += normalize(vec3(t.x + N.x, t.y + N.y, N.z)) * weights.z;
    }
    return n;
}

layout(location = 0) in vec3 vWorldPos;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in vec2 vUV;
layout(location = 3) flat in uint vEntity;
layout(location = 4) in vec4 vTangent;
layout(location = 5) in float vShell; // fur shells: height 0-1
layout(location = 6) in float vFoam;  // water (Flags.w = 2): foam 0-1

#ifdef DIM_OIT
// Weighted blended order-independent transparency (McGuire and Bavoil 2013): the shaded color, weighted by its alpha
// and a falloff with distance, is summed into one target and the transmittance (1 - alpha) multiplied into another;
// OitComposite.frag divides the sums. The main pass's outputs are plain variables here.
layout(location = 0) out vec4 outAccum;
layout(location = 1) out float outReveal;
vec4 outColor;
uint outEntity;
uvec4 outSurface;
#else
layout(location = 0) out vec4 outColor;
layout(location = 1) out uint outEntity;
// For screen-space reflections and GI: x the world normal (octahedral, half floats), y the reflection weight (the
// environment BRDF times its occlusion, rgb) and roughness (a), z the diffuse color (rgb), unorm8. 0 = none.
layout(location = 2) out uvec4 outSurface;
#endif

const float PI = 3.14159265359;

// Tangent frame from screen-space derivatives (no vertex tangents needed).
mat3 CotangentFrame(vec3 N, vec3 p, vec2 uv)
{
    vec3 dp1 = dFdx(p), dp2 = dFdy(p);
    vec2 duv1 = dFdx(uv), duv2 = dFdy(uv);
    vec3 dp2perp = cross(dp2, N), dp1perp = cross(N, dp1);
    vec3 T = dp2perp * duv1.x + dp1perp * duv2.x;
    vec3 B = dp2perp * duv1.y + dp1perp * duv2.y;
    float invmax = inversesqrt(max(max(dot(T, T), dot(B, B)), 1e-12));
    return mat3(T * invmax, B * invmax, N);
}

float DistributionGGX(float NdotH, float a)
{
    float a2 = a * a;
    float d = NdotH * NdotH * (a2 - 1.0) + 1.0;
    return a2 / (PI * d * d);
}

float GeometrySmith(float NdotV, float NdotL, float roughness)
{
    float k = (roughness + 1.0) * (roughness + 1.0) / 8.0;
    return (NdotV / (NdotV * (1.0 - k) + k)) * (NdotL / (NdotL * (1.0 - k) + k));
}

vec3 FresnelSchlick(float cosTheta, vec3 F0) { return F0 + (1.0 - F0) * pow(1.0 - cosTheta, 5.0); }

// Karis' analytic approximation of the split-sum environment BRDF.
vec3 EnvBRDFApprox(vec3 F0, float roughness, float NdotV)
{
    const vec4 c0 = vec4(-1.0, -0.0275, -0.572, 0.022);
    const vec4 c1 = vec4(1.0, 0.0425, 1.04, -0.04);
    vec4 r = roughness * c0 + c1;
    float a004 = min(r.x * r.x, exp2(-9.28 * NdotV)) * r.x + r.y;
    vec2 AB = vec2(-1.04, 1.04) * a004 + r.zw;
    return F0 * AB.x + AB.y;
}

// A rain-spattered puddle's surface normal: in a grid of half-meter cells, a drop lands somewhere in each now and then
// and a ring spreads out from it and fades; neighbouring cells' rings overlap. `rain` sets how often drops land.
vec3 PuddleNormal(vec2 p, float rain)
{
    vec2 slope = vec2(0.0);
    if (rain > 0.0)
    {
        const float cell = 0.5;
        vec2 base = floor(p / cell);
        float time = uScene.Wind.w;
        for (int y = -1; y <= 1; ++y)
            for (int x = -1; x <= 1; ++x)
            {
                vec2 c = base + vec2(x, y);
                float h = Hash3(vec3(c, 3.7));
                float period = mix(1.6, 0.6, rain) * (0.7 + 0.6 * h);
                float t = fract(time / period + h * 13.1);
                vec2 center = (c + vec2(Hash3(vec3(c, 9.1)), Hash3(vec3(c, 1.3)))) * cell;
                vec2 d = p - center;
                float dist = length(d);
                float r = t * cell * 0.9;
                float ring = sin((dist - r) * 60.0) * exp(-abs(dist - r) * 40.0) * (1.0 - t);
                if (dist > 1e-4) slope += d / dist * ring;
            }
    }
    return normalize(vec3(-slope.x * 0.25, 1.0, -slope.y * 0.25));
}

// Caustics: the web of bright lines that waves focus sunlight into, as the edges of drifting Voronoi cells (1 on an
// edge, 0 inside a cell).
float CausticLayer(vec2 p, float t)
{
    vec2 cell = floor(p), f = fract(p);
    float d1 = 8.0, d2 = 8.0;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x)
        {
            vec2 o = vec2(x, y), c = cell + o;
            vec2 h = vec2(Hash3(vec3(c, 4.1)), Hash3(vec3(c, 8.3)));
            vec2 d = o + 0.5 + 0.4 * sin(t * 0.9 + 6.2831853 * h) - f;
            float dd = dot(d, d);
            if (dd < d1) { d2 = d1; d1 = dd; }
            else if (dd < d2) d2 = dd;
        }
    float edge = sqrt(d2) - sqrt(d1);
    return pow(1.0 - smoothstep(0.0, 0.16, edge), 3.0);
}

// The sunlight reaching a point under the shown Water Volume's surface: dimmed by the water above it and gathered into
// caustics (two layers drifting at different scales). 1 out of the water.
float UnderwaterSun(vec3 p)
{
    if (uScene.WaterMin.w < 0.5 || any(lessThan(p, uScene.WaterMin.xyz)) || p.x > uScene.WaterMax.x || p.z > uScene.WaterMax.z) return 1.0;
    float depth = uScene.WaterMax.y - p.y;
    if (depth <= 0.0) return 1.0;
    float sun = exp(-depth * uScene.WaterFog.a * 0.5);
    float strength = uScene.WaterMax.w * smoothstep(0.0, 0.3, depth) * exp(-depth * 0.08);
    if (strength > 0.0)
    {
        float t = uScene.Wind.w;
        vec2 q = p.xz / uScene.WaterParams.x;
        q += 0.25 * sin(q.yx * 2.3 + t * 0.7); // bends the cells' edges into curves
        float c = CausticLayer(q, t) + 0.7 * CausticLayer(q * 1.37 + 3.1, t * 1.3 + 1.7);
        c *= 0.6 + 0.8 * ValueNoise(vec3(q * 0.35, t * 0.2)); // brighter and dimmer stretches
        sun *= max(1.0 + (0.5 + 2.4 * c - 1.0) * strength, 0.0);
    }
    return sun;
}

// Light intensities are scaled by PI so a white light at intensity 1 lights a white diffuse surface to 1.
vec3 DirectLight(vec3 N, vec3 V, vec3 L, vec3 radiance, vec3 albedo, float metallic, float roughness)
{
    float NdotL = max(dot(N, L), 0.0);
    if (NdotL <= 0.0) return vec3(0.0);
    vec3 H = normalize(V + L);
    float NdotV = max(dot(N, V), 1e-4);
    float NdotH = max(dot(N, H), 0.0);
    vec3 F0 = mix(vec3(0.04), albedo, metallic);
    vec3 F = FresnelSchlick(max(dot(H, V), 0.0), F0);
    float a = roughness * roughness;
    vec3 specular = DistributionGGX(NdotH, a) * GeometrySmith(NdotV, NdotL, roughness) * F / max(4.0 * NdotV * NdotL, 1e-4);
    vec3 kd = (1.0 - F) * (1.0 - metallic);
    return (kd * albedo / PI + specular) * radiance * PI * NdotL;
}

void main()
{
    if (LodFadeDiscard(gl_FragCoord.xy)) discard;
    MaterialRef m = pc.Material;
    vec2 uv = vUV * m.TilingOffset.xy + m.TilingOffset.zw;
    vec4 base;
    vec3 terrainNormal = vec3(0.0); // terrain: its layers' normals blended (world space)
    if (m.Flags.z == 1u)
    {
        // Terrain: its layers blended by their splat map weights (4 a map), tiled in world space (layers without
        // weight are skipped). With Height Blend, each layer's texture brightness is its height, and where layers
        // meet the highest wins: weights become how far each stands above the top less a band that narrows as
        // Height Blend grows.
        TerrainLayersRef layers = TerrainLayersRef(m.Maps2.xy);
        vec3 world = vWorldPos;
        vec3 surfaceN = normalize(vNormal);
        vec3 axes = pow(abs(surfaceN), vec3(4.0));
        axes /= max(axes.x + axes.y + axes.z, 1e-4);
        uint count = min(m.Maps2.z, 16u);
        float heightBlend = uintBitsToFloat(m.Maps2.w);
        float weights[16];
        vec3 colors[16];
        float heights[16];
        vec4 w = vec4(0.0);
        float top = -1e9, plainTotal = 0.0;
        for (uint k = 0u; k < count; ++k)
        {
            TerrainLayer layer = layers.Layers[k];
            if (layer.Maps.z == 0u) w = SampleTexture(layer.Maps.y, vUV);
            weights[k] = w[layer.Maps.z];
            colors[k] = vec3(0.0);
            heights[k] = -1e9;
            if (weights[k] <= 0.0) continue;
            vec3 texel = TerrainSample(layer.Maps.x, world, layer.Color.w, layer.Params.y > 0.5, axes).rgb;
            colors[k] = texel * layer.Color.rgb;
            heights[k] = weights[k] + dot(texel, vec3(0.299, 0.587, 0.114)) * heightBlend;
            top = max(top, heights[k]);
            plainTotal += weights[k];
        }
        float band = mix(0.6, 0.05, heightBlend), total = 0.0;
        vec3 c = vec3(0.0), n = vec3(0.0);
        for (uint k = 0u; k < count; ++k)
        {
            if (weights[k] <= 0.0) continue;
            float byHeight = max(heights[k] - (top - band), 0.0);
            float weight = mix(weights[k] / max(plainTotal, 1e-4), byHeight, heightBlend > 0.0 ? 1.0 : 0.0);
            if (weight <= 0.0) continue;
            c += colors[k] * weight;
            TerrainLayer layer = layers.Layers[k];
            vec3 t = layer.Maps.w != 0xFFFFFFFFu ? TerrainNormal(layer.Maps.w, world, layer.Color.w, layer.Params.x, layer.Params.y > 0.5, axes, surfaceN) : surfaceN;
            n += normalize(t) * weight;
            total += weight;
        }
        base = vec4(c / max(total, 1e-4), 1.0) * m.BaseColor;
        terrainNormal = n / max(total, 1e-4);
    }
    else
        base = SampleTexture(m.Maps.x, uv) * m.BaseColor;
    if (m.Flags.x == 1u && base.a < m.Params.z) discard;
    if (m.Flags.w == 1u)
    {
        // A fur shell: only the strands, darker at the roots and tinted toward the tips.
        if (FurDiscard(m, vUV, vShell)) discard;
        base.rgb *= mix(m.Layers[1].w, 1.0, vShell) * mix(vec3(1.0), m.Layers[1].rgb, vShell);
    }

    vec3 N = normalize(vNormal);
    if (!gl_FrontFacing) N = -N;
    if (m.Flags.z == 1u && dot(terrainNormal, terrainNormal) > 1e-8) N = normalize(terrainNormal); // terrains are seen from above
    if (m.Flags.y != 0u)
    {
        vec3 t = SampleTexture(m.Maps.y, uv).xyz * 2.0 - 1.0; // glTF/OpenGL normal maps: +Y points up the image
        t.xy *= m.Params.w;
        if (abs(vTangent.w) > 0.5 && dot(vTangent.xyz, vTangent.xyz) > 1e-8)
        {
            // Vertex tangents (authored or generated; Gram-Schmidt against the interpolated normal). As in glTF,
            // B = cross(N, T) * w points up the image.
            vec3 T = normalize(vTangent.xyz - N * dot(N, vTangent.xyz));
            vec3 B = cross(N, T) * (vTangent.w < 0.0 ? -1.0 : 1.0);
            N = normalize(mat3(T, B, N) * t);
        }
        else
        {
            t.y = -t.y; // the derived frame's B follows V, which runs down the image
            N = normalize(CotangentFrame(N, vWorldPos, uv) * t);
        }
    }
    vec4 mr = m.Flags.z == 1u ? vec4(1.0) : SampleTexture(m.Maps.z, uv);
    float roughness = clamp(m.Params.y * mr.g, 0.02, 1.0);
    float metallic = clamp(m.Params.x * mr.b, 0.0, 1.0);
    vec3 albedo = base.rgb;
    float foam = m.Flags.w == 2u ? vFoam : 0.0;
    if (m.Flags.w == 2u && uScene.Refraction.w > 0.5 && m.Layers[0].x > 0.0)
    {
        // Contact foam: a band where the opaque scene (a shore, rocks, a hull, a wading leg) meets the surface, from
        // the depth drawn before the water: how far the view goes on through the water before it hits something,
        // foaming within a quarter of Shore Foam (the shallows' wider foam comes from rays while playing).
        uint depthTexture = uint(uScene.Refraction.w) - 1u;
        ivec2 size = textureSize(uTextures[nonuniformEXT(depthTexture)], 0);
        float d = texelFetch(uTextures[nonuniformEXT(depthTexture)], ivec2(gl_FragCoord.xy), 0).r;
        if (d < 1.0)
        {
            vec2 suv = gl_FragCoord.xy / vec2(size);
            vec4 p = uScene.InvViewProj * vec4(suv.x * 2.0 - 1.0, 1.0 - suv.y * 2.0, d, 1.0);
            vec3 under = p.xyz / p.w;
            float gap = distance(vWorldPos, under);
            foam = max(foam, 1.0 - smoothstep(0.0, m.Layers[0].x * 0.25, gap));
        }
    }
    if (foam > 0.002)
    {
        // Water foam: white froth broken into drifting patches, opaque and rough where it is thick.
        vec2 drift = uScene.Wind.xz * uScene.Wind.w * 0.05;
        float pattern = ValueNoise(vec3((vWorldPos.xz + drift) * 0.9, uScene.Wind.w * 0.15)) * 0.65 +
                        ValueNoise(vec3((vWorldPos.xz - drift) * 3.1, uScene.Wind.w * 0.4)) * 0.35;
        float froth = smoothstep(0.25, 0.75, foam * (0.55 + pattern));
        albedo = mix(albedo, vec3(0.92, 0.95, 0.97), froth);
        base.a = mix(base.a, 1.0, froth);
        roughness = mix(roughness, 0.8, froth);
    }
    // Weather: snow settles on upward-facing surfaces (patchy at its edge), and wet surfaces darken and turn glossy,
    // the more so the flatter they are (water pools there).
    if (m.Flags.x != 2u)
    {
        float cover = uScene.Weather.y;
        if (cover > 0.0)
        {
            float patchiness = ValueNoise(vWorldPos * 1.3) * 0.25 + ValueNoise(vWorldPos * 5.1) * 0.1;
            float snow = smoothstep(0.0, 0.15, N.y + patchiness - 1.0 + cover * 1.2) * smoothstep(0.0, 0.05, cover);
            albedo = mix(albedo, vec3(0.88, 0.9, 0.94), snow);
            roughness = mix(roughness, 0.7, snow);
            metallic = mix(metallic, 0.0, snow);
        }
        float wet = uScene.Weather.x * (0.6 + 0.4 * clamp(N.y, 0.0, 1.0));
        if (wet > 0.0)
        {
            albedo *= mix(1.0, 0.5, wet * (1.0 - metallic));
            roughness = mix(roughness, 0.06, wet);
        }
        // Puddles: very wet, flat ground collects water in patches (more as it gets wetter): dark, mirror-smooth and,
        // while it rains, rippling.
        float puddle = uScene.Weather.x > 0.3 && cover <= 0.0
                     ? smoothstep(0.5, 0.56, ValueNoise(vWorldPos * 0.3 + 7.0) * 0.7 + ValueNoise(vWorldPos * 1.3) * 0.3 + (uScene.Weather.x - 0.8) * 0.4) *
                       smoothstep(0.88, 0.97, N.y)
                     : 0.0;
        if (puddle > 0.0)
        {
            albedo *= mix(1.0, 0.35, puddle);
            roughness = mix(roughness, 0.02, puddle);
            N = normalize(mix(N, PuddleNormal(vWorldPos.xz, uScene.Weather.z), puddle));
        }
    }
    vec3 V = normalize(uScene.CameraPosition.xyz - vWorldPos);
    float NdotV = max(dot(N, V), 1e-4);

    // Ambient: flat diffuse term plus sky reflections (rougher = blurrier, approximated by blending to the horizon tone).
    vec3 F0 = mix(vec3(0.04), albedo, metallic);
    vec3 R = reflect(-V, N);
#ifndef DIM_OIT
    // The Scene view's Unlit draw mode: the surface's own color (and emission), no lights.
    if (uScene.CascadeParams.w > 0.5)
    {
        vec3 own = albedo;
        if (m.Flags.z != 1u) own += m.Emission.rgb * SampleTexture(m.Maps.w, uv).rgb;
        outColor = vec4(own, m.Flags.x == 2u ? base.a : 1.0);
        outEntity = vEntity;
        outSurface = uvec4(0u);
        return;
    }
#endif
    // Image-based lighting: the environment for specular, SH irradiance of a sky texture for diffuse.
    vec3 env = EnvironmentReflection(vWorldPos, R, roughness);
    vec3 ambient = uScene.Ambient.rgb;
    if (HasSkyTexture() && uScene.EnvParams.z > 0.5) ambient = SkyIrradiance(N) * uScene.EnvParams.w;
    {
        vec3 probeLight;
        float probeWeight = ProbeVolumeIrradiance(vWorldPos, N, probeLight);
        if (probeWeight > 0.0) ambient = mix(ambient, probeLight, probeWeight);
    }
    float envOcclusion = 1.0;
    // Screen-space ambient occlusion only darkens indirect light (opaque surfaces are in the depth prepass).
    if (uScene.AOParams.x > 0.5 && m.Flags.x != 2u)
    {
        float ao = textureLod(uTextures[nonuniformEXT(uint(uScene.AOParams.y))], gl_FragCoord.xy * uScene.AOParams.zw, 0.0).r;
        if (uScene.AOParams.x > 1.5)
        {
            outColor = vec4(vec3(ao), 1.0);
            outEntity = vEntity;
            outSurface = uvec4(0u);
            return;
        }
        ambient *= ao;
        envOcclusion = mix(ao, 1.0, 0.25);
        env *= envOcclusion;
    }
    vec3 color = albedo * (1.0 - metallic) * ambient;
    vec3 envBRDF = EnvBRDFApprox(F0, roughness, NdotV);
    color += env * envBRDF; // the sky is the environment, at full strength
    outSurface = uvec4(packHalf2x16(OctEncode(N)), packUnorm4x8(vec4(clamp(envBRDF * envOcclusion, 0.0, 1.0), roughness)),
                       packUnorm4x8(vec4(clamp(albedo * (1.0 - metallic), 0.0, 1.0), 1.0)), 0u);

    if (uScene.LightDirection.w > 0.5)
        color += DirectLight(N, V, normalize(-uScene.LightDirection.xyz), uScene.LightColor.rgb, albedo, metallic, roughness) *
                 DirectionalShadow(vWorldPos, normalize(vNormal)) * CloudShadow(vWorldPos) * (m.Flags.x != 2u ? UnderwaterSun(vWorldPos) : 1.0);

    // The point and spot lights of this pixel's cluster.
    uvec2 cell = uvec2(0u);
    if (uScene.Counts.x > 0)
    {
        vec4 clip = uScene.ViewProj * vec4(vWorldPos, 1.0);
        vec2 ndc = clip.xy / clip.w;
        ivec2 tile = clamp(ivec2((ndc * 0.5 + 0.5) * vec2(CLUSTER_TILES_X, CLUSTER_TILES_Y)), ivec2(0), ivec2(CLUSTER_TILES_X - 1, CLUSTER_TILES_Y - 1));
        float depth = -(uScene.View * vec4(vWorldPos, 1.0)).z;
        float n = uScene.ClusterParams.x, f = uScene.ClusterParams.y;
        float s = uScene.ClusterParams.z > 0.5 ? (depth - n) / (f - n) : log(max(depth, n) / n) / uScene.ClusterParams.w;
        int slice = clamp(int(s * float(CLUSTER_SLICES)), 0, CLUSTER_SLICES - 1);
        cell = ClusterCellsRef(uScene.LightBuffers.zw).Cells[(slice * CLUSTER_TILES_Y + tile.y) * CLUSTER_TILES_X + tile.x];
    }
    LightListRef lights = LightListRef(uScene.LightBuffers.xy);
    ClusterIndexRef lightIndices = ClusterIndexRef(uScene.LightIndexBuffer.xy);
    for (uint k = 0u; k < cell.y; ++k)
    {
        PointLight light = lights.Lights[lightIndices.Indices[cell.x + k]];
        vec3 toLight = light.PositionRange.xyz - vWorldPos;
        float dist = length(toLight);
        float range = light.PositionRange.w;
        float falloff = clamp(1.0 - pow(dist / range, 4.0), 0.0, 1.0);
        float attenuation = falloff * falloff / (dist * dist + 1.0);
        vec4 spot = light.Direction;
        vec3 lightColor = light.Color.rgb;
        if (spot.w > -1.5) // spot light cone
        {
            attenuation *= smoothstep(spot.w, light.Cone.x, dot(-toLight / max(dist, 1e-4), spot.xyz));
            uint cookie = uint(light.Cone.y + 0.5);
            if (cookie > 0u && attenuation > 0.0)
            {
                vec2 cuv = SpotCookieUV(spot.xyz, spot.w, light.Cone.z, -toLight);
                lightColor *= textureLod(uTextures[nonuniformEXT(cookie - 1u)], cuv, 0.0).rgb;
            }
        }
        else if (spot.x > 0.5 && attenuation > 0.0) // point light cookie
            lightColor *= SampleEquirect(uint(spot.x + 0.5) - 1u, PointCookieUV(light.Cone, -toLight), 0.0);
        if (attenuation <= 0.0) continue;
        uint shadowSlot = uint(light.Color.w + 0.5);
        if (shadowSlot > 0u)
            attenuation *= PointShadow(shadowSlot - 1u, light.PositionRange.xyz, spot, vWorldPos, normalize(vNormal));
        color += DirectLight(N, V, toLight / max(dist, 1e-4), lightColor * attenuation, albedo, metallic, roughness);
    }
    if (m.Flags.z != 1u) color += m.Emission.rgb * SampleTexture(m.Maps.w, uv).rgb;
    if (uScene.CascadeParams.z > 0.5) color *= CascadeDebugColor(vWorldPos);

    outColor = vec4(color, m.Flags.x == 2u ? base.a : 1.0);
    outEntity = vEntity;
#ifdef DIM_OIT
    // Refraction: the opaque scene behind, looked up where the surface's normal bends the view (more up close), tinted
    // by the surface's color and covered by it as much as it is opaque. The result hides what is behind.
    if (m.Emission.w > 0.0 && uScene.Refraction.x > 0.5)
    {
        vec3 viewN = mat3(uScene.View) * N;
        float depth = abs((uScene.View * vec4(vWorldPos, 1.0)).z);
        vec2 bend = vec2(viewN.x, -viewN.y) * m.Emission.w * 0.06 / max(depth * 0.15, 1.0);
        vec2 suv = clamp(gl_FragCoord.xy * uScene.Refraction.yz - bend, vec2(0.001), vec2(0.999));
        vec3 behind = textureLod(uTextures[nonuniformEXT(uint(uScene.Refraction.x) - 1u)], suv, 0.0).rgb;
        behind *= mix(vec3(1.0), albedo, 0.5);
        outColor = vec4(mix(behind, color, clamp(base.a, 0.0, 1.0)), 1.0);
        color = outColor.rgb;
    }
    float alpha = clamp(outColor.a, 0.0, 1.0);
    float z = abs((uScene.View * vec4(vWorldPos, 1.0)).z);
    float weight = clamp(10.0 / (1e-5 + pow(z / 5.0, 2.0) + pow(z / 200.0, 6.0)), 1e-2, 300.0); // fits half floats
    outAccum = vec4(min(color, vec3(200.0)) * alpha, alpha) * (alpha * weight);
    outReveal = alpha;
#endif
}
