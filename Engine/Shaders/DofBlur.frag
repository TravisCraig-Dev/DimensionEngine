#version 460
#extension GL_EXT_nonuniform_qualifier : require

// Depth of field, pass 2 (half resolution): disc-shaped gather, after Dennis Gustafsson's single-pass bokeh.
// A sample contributes when its own circle of confusion reaches this pixel; samples behind this pixel are limited
// to its blur so sharp foreground objects do not bleed into the background. Alpha returns how much of the
// blurred result composite should use (this pixel's blur, or blurred foreground spreading over it).

layout(set = 0, binding = 0) uniform sampler2D uTextures[];

layout(push_constant) uniform Push
{
    uint Src;
    uint Samples;
    vec2 Texel;   // 1 / half-resolution size
    float Radius; // maximum blur radius in half-resolution pixels
} pc;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

const float GOLDEN_ANGLE = 2.39996323;

void main()
{
    vec4 center = textureLod(uTextures[nonuniformEXT(pc.Src)], vUV, 0.0);
    float centerSize = abs(center.a) * pc.Radius;
    vec3 color = center.rgb;
    float total = 1.0;
    float nearCover = 0.0;
    uint n = max(pc.Samples, 1u);
    for (uint i = 0u; i < n; ++i)
    {
        float r = sqrt((float(i) + 0.5) / float(n)) * pc.Radius;
        float a = float(i) * GOLDEN_ANGLE;
        vec4 s = textureLod(uTextures[nonuniformEXT(pc.Src)], vUV + vec2(cos(a), sin(a)) * r * pc.Texel, 0.0);
        float size = abs(s.a) * pc.Radius;
        if (s.a > center.a) size = min(size, centerSize * 2.0); // behind this pixel
        float m = smoothstep(r - 0.5, r + 0.5, size);
        color += mix(color / total, s.rgb, m);
        total += 1.0;
        if (s.a < 0.0) nearCover = max(nearCover, -s.a * m);
    }
    outColor = vec4(color / total, max(abs(center.a), nearCover));
}
