#version 460
#extension GL_EXT_nonuniform_qualifier : require

// Depth of field, pass 1 (half resolution): HDR color plus the signed circle of confusion in alpha
// (-1 = fully blurred in front of the focus, 0 = sharp, +1 = fully blurred behind it).

layout(set = 0, binding = 0) uniform sampler2D uTextures[];

layout(push_constant) uniform Push
{
    uint Hdr;
    uint Depth;
    float ProjZ;     // projection[2][2]
    float ProjW;     // projection[3][2]: view distance = ProjW / (depth + ProjZ)
    float Focus;     // focus distance (world units)
    float Range;     // distance from the focus at which blur is at its maximum
    vec2 FullTexel;  // 1 / full-resolution size
} pc;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

float CoC(vec2 uv)
{
    float d = textureLod(uTextures[nonuniformEXT(pc.Depth)], uv, 0.0).r;
    if (d >= 1.0) return 1.0; // sky: as far as it gets
    float dist = pc.ProjW / (d + pc.ProjZ);
    return clamp((dist - pc.Focus) / max(pc.Range, 1e-3), -1.0, 1.0);
}

void main()
{
    // This half-resolution pixel covers 2x2 full-resolution texels: bilinear color, and the CoC that blurs most
    // (so sharp silhouettes do not leak into blurred neighbours).
    vec3 color = texture(uTextures[nonuniformEXT(pc.Hdr)], vUV).rgb;
    vec2 o = pc.FullTexel * 0.5;
    float c0 = CoC(vUV + vec2(-o.x, -o.y)), c1 = CoC(vUV + vec2(o.x, -o.y));
    float c2 = CoC(vUV + vec2(-o.x, o.y)), c3 = CoC(vUV + vec2(o.x, o.y));
    float coc = c0;
    if (abs(c1) > abs(coc)) coc = c1;
    if (abs(c2) > abs(coc)) coc = c2;
    if (abs(c3) > abs(coc)) coc = c3;
    outColor = vec4(min(color, vec3(65000.0)), coc);
}
