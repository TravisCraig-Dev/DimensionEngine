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
void main()
{
    vec4 t = texture(uTextures[nonuniformEXT(vTexture)], vUV);
    outColor = vec4(LinearToSRGB(t.rgb) * vColor.rgb, t.a * vColor.a);
}
