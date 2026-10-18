// Shared declarations. Must match RenderDevice's pipeline layout and SceneUniforms in SceneRenderer.cpp.
#extension GL_EXT_nonuniform_qualifier : require

layout(set = 0, binding = 0) uniform sampler2D uTextures[];

struct PointLight
{
    vec4 PositionRange;  // xyz position, w range
    vec4 Color;          // rgb color * intensity
};

#define MAX_POINT_LIGHTS 32

layout(set = 1, binding = 0) uniform SceneData
{
    mat4 ViewProj;
    mat4 View;
    mat4 Proj;
    vec4 CameraPosition;
    vec4 LightDirection; // xyz: direction light travels, w: 1 if enabled
    vec4 LightColor;     // rgb * intensity
    vec4 Ambient;        // rgb * intensity
    ivec4 Counts;        // x: point light count
    vec4 GridParams;     // x: fade distance (0 = none)
    mat4 InvViewProj;
    mat4 LightViewProj;  // directional shadow map projection
    vec4 ShadowParams;   // x: enabled, y: shadow map texture index, z: depth bias, w: texel size
    vec4 SkyTop;         // rgb zenith color (linear)
    vec4 SkyHorizon;
    vec4 SkyGround;
    PointLight PointLights[MAX_POINT_LIGHTS];
} uScene;

// Procedural sky gradient, also used as a cheap environment for reflections.
vec3 SkyColor(vec3 dir)
{
    float h = dir.y;
    return h >= 0.0 ? mix(uScene.SkyHorizon.rgb, uScene.SkyTop.rgb, pow(clamp(h, 0.0, 1.0), 0.45))
                    : mix(uScene.SkyHorizon.rgb, uScene.SkyGround.rgb, pow(clamp(-h, 0.0, 1.0), 0.3));
}

vec4 SampleTexture(uint index, vec2 uv)
{
    return texture(uTextures[nonuniformEXT(index)], uv);
}

// Returns 1 when lit, 0 when fully in shadow (3x3 PCF on the directional shadow map).
float DirectionalShadow(vec3 worldPos, vec3 normal)
{
    if (uScene.ShadowParams.x < 0.5) return 1.0;
    // Normal offset reduces acne on surfaces at grazing angles.
    vec3 offsetPos = worldPos + normal * uScene.ShadowParams.w * 1.5;
    vec4 ls = uScene.LightViewProj * vec4(offsetPos, 1.0);
    vec3 p = ls.xyz / ls.w;
    // The shadow map is rendered with the same flipped viewport as the scene: row 0 is NDC y = +1.
    vec2 uv = vec2(p.x * 0.5 + 0.5, 0.5 - p.y * 0.5);
    if (uv.x <= 0.0 || uv.x >= 1.0 || uv.y <= 0.0 || uv.y >= 1.0 || p.z >= 1.0) return 1.0;
    uint map = uint(uScene.ShadowParams.y);
    vec2 texel = vec2(1.0) / vec2(textureSize(uTextures[nonuniformEXT(map)], 0));
    float lit = 0.0;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x)
        {
            float d = texture(uTextures[nonuniformEXT(map)], uv + vec2(x, y) * texel).r;
            lit += (p.z - uScene.ShadowParams.z) <= d ? 1.0 : 0.0;
        }
    return lit / 9.0;
}
