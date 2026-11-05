#version 460
#extension GL_EXT_nonuniform_qualifier : require

layout(set = 0, binding = 0) uniform sampler2D uTextures[];

layout(push_constant) uniform Push
{
    uint HdrTexture;
    float Exposure;
    uint Tonemap;        // 0 none, 1 ACES, 2 Neutral, 3 AgX
    uint BloomTexture;   // bindless index + 1, 0 = no bloom
    float BloomIntensity;
    float Vignette;      // 0..1 darkening at the corners
    uint DofTexture;     // bindless index + 1 of the blurred half-resolution image (alpha = blend), 0 = off
    uint AlphaFromHdr;   // 1: alpha from the HDR image (a Depth Only camera: where it drew), else 1
    float Saturation;    // color grading (Post Process Volumes): 1 = as is
    float Contrast;
    float FilterR, FilterG, FilterB;
    float LiftR, LiftG, LiftB, GammaR, GammaG, GammaB, GainR, GainG, GainB;
    float ChromaticAberration; // 0-1
    float FilmGrain;           // 0-1
    float LensDistortion;      // > 0 barrel, < 0 pincushion
    uint Seed;                 // per frame
    uint LookupTexture;        // bindless index + 1 of a color lookup strip (N slices of N x N), 0 = none
    uint LookupSize;           // N
    float LookupContribution;
    uint DirtTexture;          // lens dirt: bindless index + 1, 0 = none (lit by the bloom)
    float DirtIntensity;
} pc;

float Hash(vec2 p)
{
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

vec3 ACESFilm(vec3 x)
{
    const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

// Khronos PBR Neutral: hues and saturation kept, highlights rolled off to white.
vec3 NeutralTonemap(vec3 color)
{
    const float startCompression = 0.8 - 0.04, desaturation = 0.15;
    float x = min(color.r, min(color.g, color.b));
    float offset = x < 0.08 ? x - 6.25 * x * x : 0.04;
    color -= offset;
    float peak = max(color.r, max(color.g, color.b));
    if (peak < startCompression) return color;
    float d = 1.0 - startCompression;
    float newPeak = 1.0 - d * d / (peak + d - startCompression);
    color *= newPeak / peak;
    float g = 1.0 - 1.0 / (desaturation * (peak - newPeak) + 1.0);
    return mix(color, vec3(newPeak), g);
}

// AgX (minimal fit): soft, film-like highlights that desaturate gracefully.
vec3 AgxContrast(vec3 x)
{
    vec3 x2 = x * x, x4 = x2 * x2;
    return 15.5 * x4 * x2 - 40.14 * x4 * x + 31.96 * x4 - 6.868 * x2 * x + 0.4298 * x2 + 0.1191 * x - 0.00232;
}
vec3 AgxTonemap(vec3 color)
{
    const mat3 inset = mat3(0.842479062253094, 0.0423282422610123, 0.0423756549057051,
                            0.0784335999999992, 0.878468636469772, 0.0784336,
                            0.0792237451477643, 0.0791661274605434, 0.879142973793104);
    const mat3 outset = mat3(1.19687900512017, -0.0528968517574562, -0.0529716355144438,
                             -0.0980208811401368, 1.15190312990417, -0.0980434501171241,
                             -0.0990297440797205, -0.0989611768448433, 1.15107367264116);
    const float minEv = -12.47393, maxEv = 4.026069;
    color = inset * max(color, vec3(1e-10));
    color = clamp((log2(color) - minEv) / (maxEv - minEv), 0.0, 1.0);
    color = AgxContrast(color);
    color = outset * color;
    // The fit's output is display-encoded: back to linear for the rest of the chain.
    return pow(max(color, vec3(0.0)), vec3(2.2));
}

vec3 LinearToSRGB(vec3 c)
{
    c = clamp(c, 0.0, 1.0);
    return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(vec3(0.0031308), c));
}

// A display-space color through the lookup strip: red across a slice, green down it, blue picks (and blends) the
// slices. The strip is an sRGB texture (decoded when sampled), so its values are encoded again.
vec3 ApplyLookup(vec3 c)
{
    float n = float(pc.LookupSize);
    c = clamp(c, 0.0, 1.0);
    float slice = c.b * (n - 1.0);
    float s0 = floor(slice), s1 = min(s0 + 1.0, n - 1.0), f = slice - s0;
    vec2 inSlice = vec2(c.r * (n - 1.0) + 0.5, c.g * (n - 1.0) + 0.5);
    vec2 size = vec2(n * n, n);
    uint lut = pc.LookupTexture - 1u;
    // Level 0 explicitly: the coordinates jump with the colors, so implicit derivatives would pick blurry mips.
    vec3 a = textureLod(uTextures[nonuniformEXT(lut)], (vec2(s0 * n, 0.0) + inSlice) / size, 0.0).rgb;
    vec3 b = textureLod(uTextures[nonuniformEXT(lut)], (vec2(s1 * n, 0.0) + inSlice) / size, 0.0).rgb;
    return LinearToSRGB(mix(a, b, f));
}

void main()
{
    // Lens distortion: the image bulges (barrel) or pinches, scaled so the corners stay near the frame.
    vec2 uv = vUV;
    if (pc.LensDistortion != 0.0)
    {
        vec2 c = uv - 0.5;
        float k = pc.LensDistortion * 0.5;
        uv = 0.5 + c * (1.0 - k * dot(c, c)) / (1.0 - k * 0.5);
    }
    vec4 hdrSample = texture(uTextures[nonuniformEXT(pc.HdrTexture)], uv);
    vec3 hdr = hdrSample.rgb;
    // Chromatic aberration: red and blue pulled apart toward the edges.
    if (pc.ChromaticAberration > 0.0)
    {
        vec2 shift = (uv - 0.5) * pc.ChromaticAberration * 0.02;
        hdr.r = texture(uTextures[nonuniformEXT(pc.HdrTexture)], uv + shift).r;
        hdr.b = texture(uTextures[nonuniformEXT(pc.HdrTexture)], uv - shift).b;
    }
    if (pc.DofTexture != 0u)
    {
        vec4 blurred = texture(uTextures[nonuniformEXT(pc.DofTexture - 1u)], uv);
        hdr = mix(hdr, blurred.rgb, smoothstep(0.05, 0.35, blurred.a));
    }
    if (pc.BloomTexture != 0u)
    {
        vec3 bloom = texture(uTextures[nonuniformEXT(pc.BloomTexture - 1u)], uv).rgb * pc.BloomIntensity;
        hdr += bloom;
        // Smudges on the lens light up where the bloom is.
        if (pc.DirtTexture != 0u) hdr += bloom * texture(uTextures[nonuniformEXT(pc.DirtTexture - 1u)], vUV).rgb * pc.DirtIntensity;
    }
    hdr *= pc.Exposure;
    vec3 mapped = pc.Tonemap == 1u ? ACESFilm(hdr) : pc.Tonemap == 2u ? NeutralTonemap(hdr) : pc.Tonemap == 3u ? AgxTonemap(hdr) : hdr;
    // Color grading: filter, saturation, then contrast around mid grey.
    mapped *= vec3(pc.FilterR, pc.FilterG, pc.FilterB);
    float luma = dot(mapped, vec3(0.2126, 0.7152, 0.0722));
    mapped = max(mix(vec3(luma), mapped, pc.Saturation), vec3(0.0));
    mapped = max((mapped - 0.18) * pc.Contrast + 0.18, vec3(0.0));
    // Lift (shadows), gain (highlights), gamma (midtones).
    mapped = vec3(pc.GainR, pc.GainG, pc.GainB) * (mapped + vec3(pc.LiftR, pc.LiftG, pc.LiftB) * (1.0 - clamp(mapped, 0.0, 1.0)));
    mapped = pow(max(mapped, vec3(0.0)), 1.0 / vec3(pc.GammaR, pc.GammaG, pc.GammaB));
    if (pc.Vignette > 0.0)
    {
        float d = length(vUV - 0.5) * 1.41421;
        mapped *= 1.0 - pc.Vignette * smoothstep(0.4, 1.1, d);
    }
    vec3 display = LinearToSRGB(mapped);
    if (pc.LookupTexture != 0u) display = mix(display, ApplyLookup(display), pc.LookupContribution);
    // Film grain in display space, strongest in the midtones.
    if (pc.FilmGrain > 0.0)
    {
        float n = Hash(gl_FragCoord.xy + float(pc.Seed % 1024u) * 17.31) - 0.5;
        float l = dot(display, vec3(0.2126, 0.7152, 0.0722));
        display = clamp(display + n * pc.FilmGrain * 0.12 * (1.0 - abs(l * 2.0 - 1.0) * 0.6), 0.0, 1.0);
    }
    outColor = vec4(display, pc.AlphaFromHdr != 0u ? clamp(hdrSample.a, 0.0, 1.0) : 1.0);
}
