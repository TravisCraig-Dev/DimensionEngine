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
} pc;

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

void main()
{
    vec4 hdrSample = texture(uTextures[nonuniformEXT(pc.HdrTexture)], vUV);
    vec3 hdr = hdrSample.rgb;
    if (pc.DofTexture != 0u)
    {
        vec4 blurred = texture(uTextures[nonuniformEXT(pc.DofTexture - 1u)], vUV);
        hdr = mix(hdr, blurred.rgb, smoothstep(0.05, 0.35, blurred.a));
    }
    if (pc.BloomTexture != 0u)
        hdr += texture(uTextures[nonuniformEXT(pc.BloomTexture - 1u)], vUV).rgb * pc.BloomIntensity;
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
    outColor = vec4(LinearToSRGB(mapped), pc.AlphaFromHdr != 0u ? clamp(hdrSample.a, 0.0, 1.0) : 1.0);
}
