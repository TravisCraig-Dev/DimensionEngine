#version 460
#extension GL_EXT_nonuniform_qualifier : require

// Bloom downsample (13-tap filter from "Next Generation Post Processing in Call of Duty: Advanced Warfare").
// The first pass also applies a soft brightness threshold and a Karis average against fireflies.

layout(set = 0, binding = 0) uniform sampler2D uTextures[];

layout(push_constant) uniform Push
{
    uint Src;
    uint Prefilter;
    float Threshold;
    float Knee;
    vec2 SrcTexel;
    float Radius;
} pc;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

vec3 S(vec2 offset) { return texture(uTextures[nonuniformEXT(pc.Src)], vUV + offset * pc.SrcTexel).rgb; }
float Luma(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }

vec3 SoftThreshold(vec3 c)
{
    float brightness = max(c.r, max(c.g, c.b));
    float knee = max(pc.Knee * pc.Threshold, 1e-4);
    float soft = clamp(brightness - pc.Threshold + knee, 0.0, 2.0 * knee);
    soft = soft * soft / (4.0 * knee + 1e-5);
    float contribution = max(soft, brightness - pc.Threshold) / max(brightness, 1e-5);
    return c * contribution;
}

void main()
{
    vec3 a = S(vec2(-2, 2)), b = S(vec2(0, 2)), c = S(vec2(2, 2));
    vec3 d = S(vec2(-2, 0)), e = S(vec2(0, 0)), f = S(vec2(2, 0));
    vec3 g = S(vec2(-2, -2)), h = S(vec2(0, -2)), i = S(vec2(2, -2));
    vec3 j = S(vec2(-1, 1)), k = S(vec2(1, 1)), l = S(vec2(-1, -1)), m = S(vec2(1, -1));

    vec3 color;
    if (pc.Prefilter != 0u)
    {
        vec3 g0 = (j + k + l + m) * 0.25;
        vec3 g1 = (a + b + d + e) * 0.25;
        vec3 g2 = (b + c + e + f) * 0.25;
        vec3 g3 = (d + e + g + h) * 0.25;
        vec3 g4 = (e + f + h + i) * 0.25;
        float w0 = 0.5 / (1.0 + Luma(g0));
        float w1 = 0.125 / (1.0 + Luma(g1));
        float w2 = 0.125 / (1.0 + Luma(g2));
        float w3 = 0.125 / (1.0 + Luma(g3));
        float w4 = 0.125 / (1.0 + Luma(g4));
        color = (g0 * w0 + g1 * w1 + g2 * w2 + g3 * w3 + g4 * w4) / (w0 + w1 + w2 + w3 + w4);
        color = SoftThreshold(min(color, vec3(65000.0)));
    }
    else
    {
        color = e * 0.125 + (a + c + g + i) * 0.03125 + (b + d + f + h) * 0.0625 + (j + k + l + m) * 0.125;
    }
    outColor = vec4(max(color, vec3(0.0)), 1.0);
}
