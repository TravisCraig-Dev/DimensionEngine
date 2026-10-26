#version 460
#extension GL_EXT_nonuniform_qualifier : require

layout(set = 0, binding = 0) uniform sampler2D uTextures[];

layout(location = 0) in vec4 vColor;
layout(location = 1) in vec2 vUV;
layout(location = 2) flat in uint vTexture;
layout(location = 0) out vec4 outColor;

vec3 LinearToSRGB(vec3 c)
{
    c = clamp(c, 0.0, 1.0);
    return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(vec3(0.0031308), c));
}

// UI is composited in display (sRGB) space; sRGB textures are re-encoded after the hardware decode.
// Text atlases (top bit of the index) store a signed distance: 0.5 on the outline, anti-aliased over about a pixel on
// screen whatever the size. Multi-channel ones (bit 30) keep it as the median of RGB, which keeps corners sharp.
float DistanceAlpha(float d)
{
    float w = max(fwidth(d) * 0.75, 1e-4);
    return smoothstep(0.5 - w, 0.5 + w, d);
}

float Median(vec3 c) { return max(min(c.r, c.g), min(max(c.r, c.g), c.b)); }

void main()
{
    uint index = vTexture & 0x3FFFFFFFu;
    vec4 t = texture(uTextures[nonuniformEXT(index)], vUV);
    if ((vTexture & 0x80000000u) != 0u) t = vec4(1.0, 1.0, 1.0, DistanceAlpha((vTexture & 0x40000000u) != 0u ? Median(t.rgb) : t.a));
    outColor = vec4(LinearToSRGB(t.rgb) * vColor.rgb, t.a * vColor.a);
}
