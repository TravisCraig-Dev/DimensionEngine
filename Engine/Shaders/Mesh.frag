#version 460
#extension GL_GOOGLE_include_directive : require
#include "Common.glsl"
#include "MeshCommon.glsl"

// Clustered point and spot lights (see LightClusters).
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer LightListRef { PointLight Lights[]; };
layout(buffer_reference, std430, buffer_reference_align = 8) readonly buffer ClusterCellsRef { uvec2 Cells[]; };
layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer ClusterIndexRef { uint Indices[]; };

layout(location = 0) in vec3 vWorldPos;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in vec2 vUV;
layout(location = 3) flat in uint vEntity;
layout(location = 4) in vec4 vTangent;

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
        vec3 t = SampleTexture(m.Maps.y, uv).xyz * 2.0 - 1.0; // glTF/OpenGL normal maps: +Y points up the image
        t.xy *= m.Params.w;
        if (abs(vTangent.w) > 0.5 && dot(vTangent.xyz, vTangent.xyz) > 1e-8)
        {
            // Vertex tangents (authored or generated; Gram-Schmidt against the interpolated normal). As in glTF,
            // B = cross(N, T) * w points up the image.
            vec3 T = normalize(vTangent.xyz - N * dot(N, vTangent.xyz));
            vec3 B = cross(N, T) * (vTangent.w < 0.0 ? -1.0 : 1.0);
            N = normalize(mat3(T, B, N) * t);
        }
        else
        {
            t.y = -t.y; // the derived frame's B follows V, which runs down the image
            N = normalize(CotangentFrame(N, vWorldPos, uv) * t);
        }
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
    vec3 env;
    vec3 ambient = uScene.Ambient.rgb;
    if (HasSkyTexture())
    {
        // Image-based lighting: the GGX-prefiltered sky (one roughness per mip), SH irradiance for diffuse.
        if (uScene.SkyFilter.x > 0.5)
            env = SampleEquirect(uint(uScene.SkyFilter.x), DirToEquirect(R), roughness * uScene.SkyFilter.y) * uScene.EnvParams.x;
        else
            env = SkyRadiance(R, roughness * max(uScene.EnvParams.y - 1.0, 0.0)) / max(uScene.SkyParams.z, 1e-4) * uScene.EnvParams.x;
        if (uScene.EnvParams.z > 0.5) ambient = SkyIrradiance(N) * uScene.EnvParams.w;
    }
    else
        env = mix(SkyColor(R), SkyColor(vec3(0.0, 1.0, 0.0)) * 0.5 + uScene.SkyHorizon.rgb * 0.5, roughness);
    // Reflection probes, blended per pixel: smallest boxes first, each fading out over its Blend Distance toward its
    // walls; whatever weight is left over goes to the next probe and finally to the sky.
    {
        vec3 probeSum = vec3(0.0);
        float remaining = 1.0;
        for (int i = 0; i < uScene.Counts.y && remaining > 1e-3; ++i)
        {
            ReflectionProbe probe = uScene.Probes[i];
            float w = ProbeWeight(probe, vWorldPos) * remaining;
            if (w <= 0.0) continue;
            probeSum += SampleProbe(probe, vWorldPos, R, roughness) * w;
            remaining -= w;
        }
        env = probeSum + env * remaining;
    }
    // Screen-space ambient occlusion only darkens indirect light (opaque surfaces are in the depth prepass).
    if (uScene.AOParams.x > 0.5 && m.Flags.x != 2u)
    {
        float ao = textureLod(uTextures[nonuniformEXT(uint(uScene.AOParams.y))], gl_FragCoord.xy * uScene.AOParams.zw, 0.0).r;
        if (uScene.AOParams.x > 1.5)
        {
            outColor = vec4(vec3(ao), 1.0);
            outEntity = vEntity;
            return;
        }
        ambient *= ao;
        env *= mix(ao, 1.0, 0.25);
    }
    vec3 color = albedo * (1.0 - metallic) * ambient;
    color += env * EnvBRDFApprox(F0, roughness, NdotV); // the sky is the environment, at full strength

    if (uScene.LightDirection.w > 0.5)
        color += DirectLight(N, V, normalize(-uScene.LightDirection.xyz), uScene.LightColor.rgb, albedo, metallic, roughness) *
                 DirectionalShadow(vWorldPos, normalize(vNormal));

    // The point and spot lights of this pixel's cluster.
    uvec2 cell = uvec2(0u);
    if (uScene.Counts.x > 0)
    {
        vec4 clip = uScene.ViewProj * vec4(vWorldPos, 1.0);
        vec2 ndc = clip.xy / clip.w;
        ivec2 tile = clamp(ivec2((ndc * 0.5 + 0.5) * vec2(CLUSTER_TILES_X, CLUSTER_TILES_Y)), ivec2(0), ivec2(CLUSTER_TILES_X - 1, CLUSTER_TILES_Y - 1));
        float depth = -(uScene.View * vec4(vWorldPos, 1.0)).z;
        float n = uScene.ClusterParams.x, f = uScene.ClusterParams.y;
        float s = uScene.ClusterParams.z > 0.5 ? (depth - n) / (f - n) : log(max(depth, n) / n) / uScene.ClusterParams.w;
        int slice = clamp(int(s * float(CLUSTER_SLICES)), 0, CLUSTER_SLICES - 1);
        cell = ClusterCellsRef(uScene.LightBuffers.zw).Cells[(slice * CLUSTER_TILES_Y + tile.y) * CLUSTER_TILES_X + tile.x];
    }
    LightListRef lights = LightListRef(uScene.LightBuffers.xy);
    ClusterIndexRef lightIndices = ClusterIndexRef(uScene.LightIndexBuffer.xy);
    for (uint k = 0u; k < cell.y; ++k)
    {
        PointLight light = lights.Lights[lightIndices.Indices[cell.x + k]];
        vec3 toLight = light.PositionRange.xyz - vWorldPos;
        float dist = length(toLight);
        float range = light.PositionRange.w;
        float falloff = clamp(1.0 - pow(dist / range, 4.0), 0.0, 1.0);
        float attenuation = falloff * falloff / (dist * dist + 1.0);
        vec4 spot = light.Direction;
        vec3 lightColor = light.Color.rgb;
        if (spot.w > -1.5) // spot light cone
        {
            attenuation *= smoothstep(spot.w, light.Cone.x, dot(-toLight / max(dist, 1e-4), spot.xyz));
            uint cookie = uint(light.Cone.y + 0.5);
            if (cookie > 0u && attenuation > 0.0)
            {
                vec2 cuv = SpotCookieUV(spot.xyz, spot.w, light.Cone.z, -toLight);
                lightColor *= textureLod(uTextures[nonuniformEXT(cookie - 1u)], cuv, 0.0).rgb;
            }
        }
        else if (spot.x > 0.5 && attenuation > 0.0) // point light cookie
            lightColor *= SampleEquirect(uint(spot.x + 0.5) - 1u, PointCookieUV(light.Cone, -toLight), 0.0);
        if (attenuation <= 0.0) continue;
        uint shadowSlot = uint(light.Color.w + 0.5);
        if (shadowSlot > 0u)
            attenuation *= PointShadow(shadowSlot - 1u, light.PositionRange.xyz, spot, vWorldPos, normalize(vNormal));
        color += DirectLight(N, V, toLight / max(dist, 1e-4), lightColor * attenuation, albedo, metallic, roughness);
    }
    color += m.Emission.rgb * SampleTexture(m.Maps.w, uv).rgb;
    if (uScene.CascadeParams.z > 0.5) color *= CascadeDebugColor(vWorldPos);

    outColor = vec4(color, m.Flags.x == 2u ? base.a : 1.0);
    outEntity = vEntity;
}
