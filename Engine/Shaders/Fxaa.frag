#version 460
#extension GL_EXT_nonuniform_qualifier : require

// FXAA (after Timothy Lottes' FXAA 3, "PC lite" variant) on the tonemapped, sRGB-encoded image.

layout(set = 0, binding = 0) uniform sampler2D uTextures[];

layout(push_constant) uniform Push
{
    uint Src;
    uint Pad;
    vec2 Texel;
} pc;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

const float kReduceMin = 1.0 / 128.0;
const float kReduceMul = 1.0 / 8.0;
const float kSpanMax = 8.0;

vec3 S(vec2 uv) { return texture(uTextures[nonuniformEXT(pc.Src)], uv).rgb; }
float Luma(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }

void main()
{
    vec2 t = pc.Texel;
    vec3 rgbM = S(vUV);
    float lumaNW = Luma(S(vUV + vec2(-1, -1) * t));
    float lumaNE = Luma(S(vUV + vec2(1, -1) * t));
    float lumaSW = Luma(S(vUV + vec2(-1, 1) * t));
    float lumaSE = Luma(S(vUV + vec2(1, 1) * t));
    float lumaM = Luma(rgbM);
    float lumaMin = min(lumaM, min(min(lumaNW, lumaNE), min(lumaSW, lumaSE)));
    float lumaMax = max(lumaM, max(max(lumaNW, lumaNE), max(lumaSW, lumaSE)));

    vec2 dir;
    dir.x = -((lumaNW + lumaNE) - (lumaSW + lumaSE));
    dir.y = ((lumaNW + lumaSW) - (lumaNE + lumaSE));
    float dirReduce = max((lumaNW + lumaNE + lumaSW + lumaSE) * 0.25 * kReduceMul, kReduceMin);
    float rcpDirMin = 1.0 / (min(abs(dir.x), abs(dir.y)) + dirReduce);
    dir = clamp(dir * rcpDirMin, vec2(-kSpanMax), vec2(kSpanMax)) * t;

    vec3 rgbA = 0.5 * (S(vUV + dir * (1.0 / 3.0 - 0.5)) + S(vUV + dir * (2.0 / 3.0 - 0.5)));
    vec3 rgbB = rgbA * 0.5 + 0.25 * (S(vUV - dir * 0.5) + S(vUV + dir * 0.5));
    float lumaB = Luma(rgbB);
    outColor = vec4((lumaB < lumaMin || lumaB > lumaMax) ? rgbA : rgbB, 1.0);
}
