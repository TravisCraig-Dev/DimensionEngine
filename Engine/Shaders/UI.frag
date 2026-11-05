#version 460
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference_uvec2 : require

layout(set = 0, binding = 0) uniform sampler2D uTextures[];

layout(location = 0) in vec4 vColor;
layout(location = 1) in vec2 vUV;
layout(location = 2) flat in uint vTexture;
layout(location = 3) flat in uint vMask;
layout(location = 0) out vec4 outColor;

layout(push_constant) uniform Push { vec2 ScreenSize; uvec2 Masks; } pc;

// UI Masks: rect (pixels), soft edges, and an image whose alpha shapes it.
struct UIMask
{
    vec4 Rect;
    vec2 Softness;
    uint Texture;
    uint Parent; // the mask outside it: index + 1, 0 none
};
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer UIMaskTable { UIMask Masks[]; };

float MaskLevelAlpha(UIMask m, vec2 p)
{
    vec2 inside = min(p - m.Rect.xy, m.Rect.zw - p);
    vec2 fade = clamp(inside / max(m.Softness, vec2(1e-4)), 0.0, 1.0);
    float a = (m.Softness.x > 0.0 ? fade.x : 1.0) * (m.Softness.y > 0.0 ? fade.y : 1.0);
    if (m.Texture != 0u)
    {
        vec2 uv = (p - m.Rect.xy) / max(m.Rect.zw - m.Rect.xy, vec2(1e-4));
        a *= texture(uTextures[nonuniformEXT(m.Texture)], uv).a;
    }
    return a;
}

// Every mask over the quad, innermost to outermost, multiplied.
float MaskAlpha()
{
    float a = 1.0;
    uint index = vMask;
    for (int i = 0; i < 4 && index != 0u; ++i)
    {
        UIMask m = UIMaskTable(pc.Masks).Masks[index - 1u];
        a *= MaskLevelAlpha(m, gl_FragCoord.xy);
        index = m.Parent;
    }
    return a;
}

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
    // Bit 29: an image already in display space (another camera's output), shown as it is.
    uint index = vTexture & 0x1FFFFFFFu;
    vec4 t = texture(uTextures[nonuniformEXT(index)], vUV);
    if ((vTexture & 0x80000000u) != 0u) t = vec4(1.0, 1.0, 1.0, DistanceAlpha((vTexture & 0x40000000u) != 0u ? Median(t.rgb) : t.a));
    vec3 rgb = (vTexture & 0x20000000u) != 0u ? t.rgb : LinearToSRGB(t.rgb);
    outColor = vec4(rgb * vColor.rgb, t.a * vColor.a * MaskAlpha());
}
