// Shared declarations. Must match RenderDevice's pipeline layout and SceneUniforms in SceneRenderer.cpp.
#extension GL_EXT_nonuniform_qualifier : require

layout(set = 0, binding = 0) uniform sampler2D uTextures[];

struct PointLight
{
    vec4 PositionRange;  // xyz position, w range
    vec4 Color;          // rgb color * intensity, w: shadow slot + 1 (0 = no shadows)
    vec4 Direction;      // spot lights: xyz direction, w cos(outer half angle); point lights: w = -2
    vec4 Cone;           // x: cos(inner half angle), y: cookie texture index + 1 (0 = none), z: cookie roll (radians)
};

#define MAX_POINT_LIGHTS 32
#define MAX_POINT_SHADOWS 4
#define MAX_REFLECTION_PROBES 8

struct ReflectionProbe
{
    vec4 CenterIntensity; // xyz capture position, w intensity
    vec4 BoxMin;          // xyz influence box min, w texture index
    vec4 BoxMax;          // xyz influence box max, w mip count
    vec4 Params;          // x 1 = box projection, y blend distance
};

layout(set = 1, binding = 0) uniform SceneData
{
    mat4 ViewProj;
    mat4 View;
    mat4 Proj;
    vec4 CameraPosition;
    vec4 LightDirection; // xyz: direction light travels, w: 1 if enabled
    vec4 LightColor;     // rgb * intensity
    vec4 Ambient;        // rgb * intensity
    ivec4 Counts;        // x: point light count
    vec4 GridParams;     // x: fade distance (0 = none)
    mat4 InvViewProj;
    mat4 LightViewProj[4]; // directional shadow cascades (tiles of a 2x2 atlas)
    vec4 ShadowParams;   // x: cascade count (0 = no shadows), y: atlas texture index, z: depth bias
    vec4 CascadeSplits;  // far view depth of each cascade
    vec4 CascadeTexel;   // world size of a shadow texel per cascade
    vec4 CascadeParams;  // y: blend fraction between cascades, z: 1 = debug colors
    vec4 SkyTop;         // rgb zenith color (linear)
    vec4 SkyHorizon;
    vec4 SkyGround;
    vec4 SkyParams;      // x mode (1 = texture), y texture index, z intensity, w rotation (radians)
    vec4 EnvParams;      // x reflection intensity, y max mip, z 1 = irradiance SH valid, w ambient intensity
    vec4 IrradianceSH[9];
    mat4 PointShadowViewProj[MAX_POINT_SHADOWS * 6]; // per shadowed point light: +X, -X, +Y, -Y, +Z, -Z faces
    vec4 PointShadowParams[MAX_POINT_SHADOWS];       // x near, y far (range), z bias (world units), w atlas index
    mat4 InvProj;
    vec4 AOParams;       // x 1 = SSAO enabled (2 = debug view), y AO texture index, zw 1 / framebuffer size
    vec4 CascadeDepth;   // world depth covered by each cascade's [0,1] depth range
    vec4 SoftShadowParams; // x 1 = PCSS soft shadows, y tan(light angular radius), z max filter radius (tile texels)
    ReflectionProbe Probes[MAX_REFLECTION_PROBES]; // Counts.y in use
    PointLight PointLights[MAX_POINT_LIGHTS];
} uScene;

// Procedural sky gradient, also used as a cheap environment for reflections.
vec3 SkyColor(vec3 dir)
{
    float h = dir.y;
    return h >= 0.0 ? mix(uScene.SkyHorizon.rgb, uScene.SkyTop.rgb, pow(clamp(h, 0.0, 1.0), 0.45))
                    : mix(uScene.SkyHorizon.rgb, uScene.SkyGround.rgb, pow(clamp(-h, 0.0, 1.0), 0.3));
}

// Equirectangular mapping (matches Environment::DirectionToEquirect): u = 0.5 at -Z, v = 0 at +Y.
vec2 DirToEquirect(vec3 d)
{
    float c = cos(uScene.SkyParams.w), s = sin(uScene.SkyParams.w);
    d = vec3(c * d.x + s * d.z, d.y, -s * d.x + c * d.z); // undo the sky rotation around +Y
    return vec2(atan(d.x, -d.z) / 6.28318530718 + 0.5, acos(clamp(d.y, -1.0, 1.0)) / 3.14159265359);
}

// Same mapping without the sky rotation (reflection probes).
vec2 DirToEquirectRaw(vec3 d)
{
    return vec2(atan(d.x, -d.z) / 6.28318530718 + 0.5, acos(clamp(d.y, -1.0, 1.0)) / 3.14159265359);
}

bool HasSkyTexture() { return uScene.SkyParams.x > 0.5; }

// Sky radiance in a direction (textureLod: explicit mips avoid seams at the wrap line).
vec3 SkyRadiance(vec3 dir, float lod)
{
    if (!HasSkyTexture()) return SkyColor(dir);
    return textureLod(uTextures[nonuniformEXT(uint(uScene.SkyParams.y))], DirToEquirect(normalize(dir)), lod).rgb * uScene.SkyParams.z;
}

// Diffuse irradiance from the sky's spherical harmonics (already convolved and divided by pi).
vec3 SkyIrradiance(vec3 n)
{
    vec3 r = uScene.IrradianceSH[0].rgb * 0.282095
           + uScene.IrradianceSH[1].rgb * 0.488603 * n.y
           + uScene.IrradianceSH[2].rgb * 0.488603 * n.z
           + uScene.IrradianceSH[3].rgb * 0.488603 * n.x
           + uScene.IrradianceSH[4].rgb * 1.092548 * n.x * n.y
           + uScene.IrradianceSH[5].rgb * 1.092548 * n.y * n.z
           + uScene.IrradianceSH[6].rgb * 0.315392 * (3.0 * n.z * n.z - 1.0)
           + uScene.IrradianceSH[7].rgb * 1.092548 * n.x * n.z
           + uScene.IrradianceSH[8].rgb * 0.546274 * (n.x * n.x - n.y * n.y);
    return max(r, vec3(0.0));
}

vec4 SampleTexture(uint index, vec2 uv)
{
    return texture(uTextures[nonuniformEXT(index)], uv);
}

// Interleaved gradient noise (Jimenez 2014) over shadow-map coordinates: rotates the soft shadow sample disc.
// Seeded in light space (not screen space) so it stays put as the camera moves; usable from any stage.
float ShadowNoise(vec2 coord) { return fract(52.9829189 * fract(dot(floor(coord), vec2(0.06711056, 0.00583715)))); }

// Vogel disk: sample i of n, evenly spread over the unit disc and rotated by `phi`.
vec2 VogelDisk(int i, int n, float phi)
{
    float r = sqrt((float(i) + 0.5) / float(n));
    float theta = float(i) * 2.39996323 + phi;
    return vec2(cos(theta), sin(theta)) * r;
}

// Shadow factor from one cascade (3x3 PCF, or PCSS when soft shadows are on), or -1 when the point is outside
// that cascade's tile.
float SampleCascade(int c, vec3 worldPos, vec3 normal)
{
    // Normal offset (in this cascade's texels) reduces acne on surfaces at grazing angles.
    vec3 offsetPos = worldPos + normal * uScene.CascadeTexel[c] * 1.5;
    vec4 ls = uScene.LightViewProj[c] * vec4(offsetPos, 1.0);
    vec3 p = ls.xyz / ls.w;
    // Tiles are rendered with the same flipped viewport as the scene: row 0 is NDC y = +1.
    vec2 uv = vec2(p.x * 0.5 + 0.5, 0.5 - p.y * 0.5);
    uint map = uint(uScene.ShadowParams.y);
    vec2 texel = vec2(1.0) / vec2(textureSize(uTextures[nonuniformEXT(map)], 0));
    float margin = texel.x * 4.0; // 2 tile texels: keeps the PCF footprint inside the tile
    if (uv.x <= margin || uv.y <= margin || uv.x >= 1.0 - margin || uv.y >= 1.0 - margin || p.z >= 1.0) return -1.0;
    vec2 tileOrigin = vec2(float(c & 1), float(c >> 1));
    vec2 atlasUV = (tileOrigin + uv) * 0.5;
    float receiver = p.z - uScene.ShadowParams.z;

    if (uScene.SoftShadowParams.x > 0.5)
    {
        // Percentage-closer soft shadows: the penumbra widens with the distance from the blocker to the receiver,
        // like a sun of finite size. Distances are in world units (depth range * [0,1] depth, texel size).
        vec2 tileTexel = texel * 2.0;
        vec2 lo = (tileOrigin + tileTexel) * 0.5, hi = (tileOrigin + 1.0 - tileTexel) * 0.5; // stay inside the tile
        float range = uScene.CascadeDepth[c];
        float tanAngle = uScene.SoftShadowParams.y;
        float maxRadius = uScene.SoftShadowParams.z;
        float phi = ShadowNoise(atlasUV / texel * 4.0) * 6.28318530718;

        // 1. Blocker search over the area that could shade this point.
        float searchTexels = clamp(tanAngle * min(p.z * range, 20.0) / uScene.CascadeTexel[c], 1.5, maxRadius);
        float blockerSum = 0.0, blockers = 0.0;
        for (int i = 0; i < 16; ++i)
        {
            vec2 suv = clamp(atlasUV + VogelDisk(i, 16, phi) * searchTexels * texel, lo, hi);
            float d = texture(uTextures[nonuniformEXT(map)], suv).r;
            if (d < receiver) { blockerSum += d; blockers += 1.0; }
        }
        if (blockers == 0.0) return 1.0;
        float blocker = blockerSum / blockers;

        // 2. Penumbra width from the average blocker depth, then a PCF filter of that size.
        float penumbraWorld = (receiver - blocker) * range * tanAngle;
        float filterTexels = clamp(penumbraWorld / uScene.CascadeTexel[c], 1.0, maxRadius);
        float lit = 0.0;
        for (int i = 0; i < 24; ++i)
        {
            vec2 suv = clamp(atlasUV + VogelDisk(i, 24, phi) * filterTexels * texel, lo, hi);
            lit += receiver <= texture(uTextures[nonuniformEXT(map)], suv).r ? 1.0 : 0.0;
        }
        return lit / 24.0;
    }

    float lit = 0.0;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x)
        {
            float d = texture(uTextures[nonuniformEXT(map)], atlasUV + vec2(x, y) * texel).r;
            lit += receiver <= d ? 1.0 : 0.0;
        }
    return lit / 9.0;
}

int ShadowCascadeIndex(vec3 worldPos)
{
    int count = int(uScene.ShadowParams.x);
    float depth = -(uScene.View * vec4(worldPos, 1.0)).z;
    for (int c = 0; c < count; ++c)
        if (depth <= uScene.CascadeSplits[c]) return c;
    return -1;
}

// Returns 1 when lit, 0 when fully in shadow. Blends into the next cascade near each split and fades out at the shadow distance.
float DirectionalShadow(vec3 worldPos, vec3 normal)
{
    int count = int(uScene.ShadowParams.x);
    if (count == 0) return 1.0;
    float depth = -(uScene.View * vec4(worldPos, 1.0)).z;
    for (int c = 0; c < count; ++c)
    {
        if (depth > uScene.CascadeSplits[c]) continue;
        float s = SampleCascade(c, worldPos, normal);
        if (s < 0.0) continue; // outside this tile: fall back to the next cascade
        float start = c == 0 ? 0.0 : uScene.CascadeSplits[c - 1];
        float end = uScene.CascadeSplits[c];
        float blendStart = end - (end - start) * uScene.CascadeParams.y;
        if (depth > blendStart)
        {
            float f = (depth - blendStart) / max(end - blendStart, 1e-4);
            float next = c + 1 < count ? SampleCascade(c + 1, worldPos, normal) : 1.0;
            if (next >= 0.0) s = mix(s, next, f);
        }
        return s;
    }
    return 1.0;
}

// Debug tint per cascade (red, green, blue, yellow).
vec3 CascadeDebugColor(vec3 worldPos)
{
    int c = ShadowCascadeIndex(worldPos);
    if (c == 0) return vec3(1.0, 0.45, 0.45);
    if (c == 1) return vec3(0.45, 1.0, 0.45);
    if (c == 2) return vec3(0.45, 0.6, 1.0);
    if (c == 3) return vec3(1.0, 1.0, 0.4);
    return vec3(1.0);
}

// Reflection probes. Weight of a probe at a point: 0 outside its box, rising to 1 at Blend Distance inside its walls.
float ProbeWeight(ReflectionProbe p, vec3 pos)
{
    vec3 inside = min(pos - p.BoxMin.xyz, p.BoxMax.xyz - pos);
    float d = min(min(inside.x, inside.y), inside.z);
    if (d < 0.0) return 0.0;
    return p.Params.y > 0.0 ? clamp(d / p.Params.y, 0.0, 1.0) : 1.0;
}

// The surroundings captured at the probe, optionally box-projected so reflections line up with the walls of the
// influence box instead of being "at infinity". Rougher surfaces read blurrier (GGX-prefiltered) mips.
vec3 SampleProbe(ReflectionProbe probe, vec3 pos, vec3 R, float roughness)
{
    vec3 dir = R;
    if (probe.Params.x > 0.5)
    {
        vec3 safe = mix(R, vec3(1e-4), lessThan(abs(R), vec3(1e-4)));
        vec3 t1 = (probe.BoxMax.xyz - pos) / safe, t2 = (probe.BoxMin.xyz - pos) / safe;
        vec3 far = max(t1, t2);
        float t = min(min(far.x, far.y), far.z);
        if (t > 0.0) dir = pos + R * t - probe.CenterIntensity.xyz;
    }
    float lod = roughness * max(probe.BoxMax.w - 1.0, 0.0);
    return textureLod(uTextures[nonuniformEXT(uint(probe.BoxMin.w))], DirToEquirectRaw(normalize(dir)), lod).rgb * probe.CenterIntensity.w;
}

// Spot light cookie coordinates (mirrors SpotCookieUV in SceneRenderer.cpp).
vec2 SpotCookieUV(vec3 dir, float cosOuter, float roll, vec3 toPoint)
{
    vec3 d = normalize(dir), p = normalize(toPoint);
    vec3 ref = abs(d.y) > 0.99 ? vec3(0, 0, 1) : vec3(0, 1, 0);
    vec3 right = normalize(cross(d, ref)), up = cross(right, d);
    float tanOuter = sqrt(max(1.0 - cosOuter * cosOuter, 1e-6)) / max(cosOuter, 1e-4);
    vec2 q = vec2(dot(p, right), dot(p, up)) / (max(dot(p, d), 1e-4) * tanOuter);
    float c = cos(roll), s = sin(roll);
    q = vec2(c * q.x + s * q.y, -s * q.x + c * q.y);
    return vec2(q.x * 0.5 + 0.5, 0.5 - q.y * 0.5);
}

// Point/spot light shadow from the shadow atlas (rows = shadowed lights, columns = cube faces; spot lights use
// column 0 only). Returns 1 when lit. Compares linear distances along the face axis, so the bias is in world units.
float PointShadow(uint slot, vec3 lightPos, vec4 spotDirection, vec3 worldPos, vec3 normal)
{
    vec4 params = uScene.PointShadowParams[slot];
    vec3 toFrag = worldPos - lightPos;
    vec3 a = abs(toFrag);
    // Normal offset scaled by distance (texel footprint grows with distance).
    float dist = max(a.x, max(a.y, a.z));
    vec3 offsetPos = worldPos + normal * (0.01 + dist * 0.004);
    toFrag = offsetPos - lightPos;
    a = abs(toFrag);
    int face;
    float axisDist;
    if (spotDirection.w > -1.5) { face = 0; axisDist = dot(toFrag, spotDirection.xyz); if (axisDist <= 0.0) return 1.0; }
    else if (a.x >= a.y && a.x >= a.z) { face = toFrag.x > 0.0 ? 0 : 1; axisDist = a.x; }
    else if (a.y >= a.z)          { face = toFrag.y > 0.0 ? 2 : 3; axisDist = a.y; }
    else                          { face = toFrag.z > 0.0 ? 4 : 5; axisDist = a.z; }
    if (axisDist >= params.y) return 1.0;

    vec4 clip = uScene.PointShadowViewProj[slot * 6u + uint(face)] * vec4(offsetPos, 1.0);
    vec2 uv = vec2(clip.x / clip.w * 0.5 + 0.5, 0.5 - clip.y / clip.w * 0.5);
    uint map = uint(params.w);
    vec2 atlasSize = vec2(textureSize(uTextures[nonuniformEXT(map)], 0));
    vec2 tiles = vec2(6.0, float(MAX_POINT_SHADOWS));
    vec2 texel = 1.0 / atlasSize;
    vec2 tileTexel = texel * tiles; // one texel in tile uv
    uv = clamp(uv, tileTexel * 1.5, 1.0 - tileTexel * 1.5); // keep the PCF footprint inside the face
    vec2 atlasUV = (vec2(float(face), float(slot)) + uv) / tiles;

    float n = params.x, f = params.y;
    float lit = 0.0;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x)
        {
            float d = texture(uTextures[nonuniformEXT(map)], atlasUV + vec2(x, y) * texel).r;
            float occluder = n * f / (f - d * (f - n)); // depth [0,1] -> linear distance along the face axis
            lit += axisDist - params.z <= occluder ? 1.0 : 0.0;
        }
    return lit / 9.0;
}
