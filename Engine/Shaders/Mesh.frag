#version 460
#extension GL_GOOGLE_include_directive : require
#include "Common.glsl"
#include "MeshCommon.glsl"

layout(location = 0) in vec3 vWorldPos;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in vec2 vUV;

layout(location = 0) out vec4 outColor;
layout(location = 1) out uint outEntity;

const float PI = 3.14159265359;

// Tangent frame from screen-space derivatives (no vertex tangents needed).
mat3 CotangentFrame(vec3 N, vec3 p, vec2 uv)
{
    vec3 dp1 = dFdx(p), dp2 = dFdy(p);
    vec2 duv1 = dFdx(uv), duv2 = dFdy(uv);
    vec3 dp2perp = cross(dp2, N), dp1perp = cross(N, dp1);
    vec3 T = dp2perp * duv1.x + dp1perp * duv2.x;
    vec3 B = dp2perp * duv1.y + dp1perp * duv2.y;
    float invmax = inversesqrt(max(max(dot(T, T), dot(B, B)), 1e-12));
    return mat3(T * invmax, B * invmax, N);
}

float DistributionGGX(float NdotH, float a)
{
    float a2 = a * a;
    float d = NdotH * NdotH * (a2 - 1.0) + 1.0;
    return a2 / (PI * d * d);
}

float GeometrySmith(float NdotV, float NdotL, float roughness)
{
    float k = (roughness + 1.0) * (roughness + 1.0) / 8.0;
    return (NdotV / (NdotV * (1.0 - k) + k)) * (NdotL / (NdotL * (1.0 - k) + k));
}

vec3 FresnelSchlick(float cosTheta, vec3 F0) { return F0 + (1.0 - F0) * pow(1.0 - cosTheta, 5.0); }

// Karis' analytic approximation of the split-sum environment BRDF.
vec3 EnvBRDFApprox(vec3 F0, float roughness, float NdotV)
{
    const vec4 c0 = vec4(-1.0, -0.0275, -0.572, 0.022);
    const vec4 c1 = vec4(1.0, 0.0425, 1.04, -0.04);
    vec4 r = roughness * c0 + c1;
    float a004 = min(r.x * r.x, exp2(-9.28 * NdotV)) * r.x + r.y;
    vec2 AB = vec2(-1.04, 1.04) * a004 + r.zw;
    return F0 * AB.x + AB.y;
}

// Light intensities are scaled by PI so a white light at intensity 1 lights a white diffuse surface to 1.
vec3 DirectLight(vec3 N, vec3 V, vec3 L, vec3 radiance, vec3 albedo, float metallic, float roughness)
{
    float NdotL = max(dot(N, L), 0.0);
    if (NdotL <= 0.0) return vec3(0.0);
    vec3 H = normalize(V + L);
    float NdotV = max(dot(N, V), 1e-4);
    float NdotH = max(dot(N, H), 0.0);
    vec3 F0 = mix(vec3(0.04), albedo, metallic);
    vec3 F = FresnelSchlick(max(dot(H, V), 0.0), F0);
    float a = roughness * roughness;
    vec3 specular = DistributionGGX(NdotH, a) * GeometrySmith(NdotV, NdotL, roughness) * F / max(4.0 * NdotV * NdotL, 1e-4);
    vec3 kd = (1.0 - F) * (1.0 - metallic);
    return (kd * albedo / PI + specular) * radiance * PI * NdotL;
}

void main()
{
    MaterialRef m = pc.Material;
    vec2 uv = vUV * m.TilingOffset.xy + m.TilingOffset.zw;
    vec4 base = SampleTexture(m.Maps.x, uv) * m.BaseColor;
    if (m.Flags.x == 1u && base.a < m.Params.z) discard;

    vec3 N = normalize(vNormal);
    if (!gl_FrontFacing) N = -N;
    if (m.Flags.y != 0u)
    {
        vec3 t = SampleTexture(m.Maps.y, uv).xyz * 2.0 - 1.0;
        t.y = -t.y; // glTF/OpenGL normal maps: +Y points up the image, our V runs downwards
        t.xy *= m.Params.w;
        N = normalize(CotangentFrame(N, vWorldPos, uv) * t);
    }
    vec4 mr = SampleTexture(m.Maps.z, uv);
    float roughness = clamp(m.Params.y * mr.g, 0.02, 1.0);
    float metallic = clamp(m.Params.x * mr.b, 0.0, 1.0);
    vec3 albedo = base.rgb;
    vec3 V = normalize(uScene.CameraPosition.xyz - vWorldPos);
    float NdotV = max(dot(N, V), 1e-4);

    // Ambient: flat diffuse term plus sky reflections (rougher = blurrier, approximated by blending to the horizon tone).
    vec3 F0 = mix(vec3(0.04), albedo, metallic);
    vec3 R = reflect(-V, N);
    vec3 env = mix(SkyColor(R), SkyColor(vec3(0.0, 1.0, 0.0)) * 0.5 + uScene.SkyHorizon.rgb * 0.5, roughness);
    vec3 color = albedo * (1.0 - metallic) * uScene.Ambient.rgb;
    color += env * EnvBRDFApprox(F0, roughness, NdotV); // the sky is the environment, at full strength

    if (uScene.LightDirection.w > 0.5)
        color += DirectLight(N, V, normalize(-uScene.LightDirection.xyz), uScene.LightColor.rgb, albedo, metallic, roughness) *
                 DirectionalShadow(vWorldPos, normalize(vNormal));

    for (int i = 0; i < uScene.Counts.x; ++i)
    {
        vec3 toLight = uScene.PointLights[i].PositionRange.xyz - vWorldPos;
        float dist = length(toLight);
        float range = uScene.PointLights[i].PositionRange.w;
        float falloff = clamp(1.0 - pow(dist / range, 4.0), 0.0, 1.0);
        float attenuation = falloff * falloff / (dist * dist + 1.0);
        color += DirectLight(N, V, toLight / max(dist, 1e-4), uScene.PointLights[i].Color.rgb * attenuation, albedo, metallic, roughness);
    }
    color += m.Emission.rgb * SampleTexture(m.Maps.w, uv).rgb;

    outColor = vec4(color, m.Flags.x == 2u ? base.a : 1.0);
    outEntity = pc.EntityId;
}
