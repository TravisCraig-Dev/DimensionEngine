#version 460
#extension GL_EXT_nonuniform_qualifier : require

// Bloom upsample: 9-tap tent filter of the smaller level, added (blend) onto the larger level.

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

vec3 S(vec2 offset) { return texture(uTextures[nonuniformEXT(pc.Src)], vUV + offset * pc.SrcTexel * pc.Radius).rgb; }

void main()
{
    vec3 color = S(vec2(0, 0)) * 4.0;
    color += (S(vec2(-1, 0)) + S(vec2(1, 0)) + S(vec2(0, -1)) + S(vec2(0, 1))) * 2.0;
    color += S(vec2(-1, -1)) + S(vec2(1, -1)) + S(vec2(-1, 1)) + S(vec2(1, 1));
    outColor = vec4(color / 16.0, 1.0);
}
