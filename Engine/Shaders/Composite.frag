#version 460
#extension GL_EXT_nonuniform_qualifier : require

layout(set = 0, binding = 0) uniform sampler2D uTextures[];

layout(push_constant) uniform Push
{
    uint HdrTexture;
    float Exposure;
    uint Tonemap;        // 0 none, 1 ACES
    uint BloomTexture;   // bindless index + 1, 0 = no bloom
    float BloomIntensity;
    float Vignette;      // 0..1 darkening at the corners
    uint DofTexture;     // bindless index + 1 of the blurred half-resolution image (alpha = blend), 0 = off
    uint AlphaFromHdr;   // 1: alpha from the HDR image (a Depth Only camera: where it drew), else 1
    float Saturation;    // color grading (Post Process Volumes): 1 = as is
    float Contrast;
    float FilterR, FilterG, FilterB;
} pc;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

vec3 ACESFilm(vec3 x)
{
    const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
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
    vec3 mapped = pc.Tonemap == 1u ? ACESFilm(hdr) : hdr;
    // Color grading: filter, saturation, then contrast around mid grey.
    mapped *= vec3(pc.FilterR, pc.FilterG, pc.FilterB);
    float luma = dot(mapped, vec3(0.2126, 0.7152, 0.0722));
    mapped = max(mix(vec3(luma), mapped, pc.Saturation), vec3(0.0));
    mapped = max((mapped - 0.18) * pc.Contrast + 0.18, vec3(0.0));
    if (pc.Vignette > 0.0)
    {
        float d = length(vUV - 0.5) * 1.41421;
        mapped *= 1.0 - pc.Vignette * smoothstep(0.4, 1.1, d);
    }
    outColor = vec4(LinearToSRGB(mapped), pc.AlphaFromHdr != 0u ? clamp(hdrSample.a, 0.0, 1.0) : 1.0);
}
