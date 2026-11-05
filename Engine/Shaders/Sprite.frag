#version 460
#extension GL_GOOGLE_include_directive : require
#include "Common.glsl"

layout(location = 0) in vec4 vColor;
layout(location = 1) in vec2 vUV;
layout(location = 2) flat in uint vTexture;
layout(location = 3) flat in uint vEntity;

layout(location = 0) out vec4 outColor;
layout(location = 1) out uint outEntity;

// Text atlases (top bit of the index) store a signed distance (0.5 on the outline): in alpha, or as the median of RGB
// for multi-channel ones (bit 30).
float DistanceAlpha(float d)
{
    float w = max(fwidth(d) * 0.75, 1e-4);
    return smoothstep(0.5 - w, 0.5 + w, d);
}

float Median(vec3 c) { return max(min(c.r, c.g), min(max(c.r, c.g), c.b)); }

void main()
{
    vec4 t;
    if ((vTexture & 0x80000000u) != 0u)
    {
        vec4 field = SampleTexture(vTexture & 0x3FFFFFFFu, vUV);
        t = vec4(1.0, 1.0, 1.0, DistanceAlpha((vTexture & 0x40000000u) != 0u ? Median(field.rgb) : field.a));
    }
    else
    {
        t = SampleTexture(vTexture & 0x0FFFFFFFu, vUV);
        // Bit 28: an image of display values (a World Space canvas's UI): to linear like the rest of the scene.
        if ((vTexture & 0x10000000u) != 0u)
        {
            vec3 c = clamp(t.rgb, 0.0, 1.0);
            t.rgb = mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), greaterThan(c, vec3(0.04045)));
        }
    }
    vec4 color = t * vColor;
    if (color.a < 0.01) discard; // keeps fully transparent texels out of picking
    outColor = color;
    outEntity = vEntity;
}
