#version 460
#extension GL_EXT_nonuniform_qualifier : require

// Prefilters an equirectangular environment for one roughness (one mip of the result): the GGX lobe around each
// direction, with N = V = R (Karis' split-sum approximation). Filtered importance sampling: each sample reads the
// source mip whose texels match the sample's solid angle, so 64 samples give a smooth result.

layout(set = 0, binding = 0) uniform sampler2D uTextures[];

layout(push_constant) uniform Push
{
    uint Source;     // bindless index of the source (box-filtered mip chain)
    float Roughness; // perceptual roughness of this mip
    float MaxLod;    // source mip count - 1
    float MinLod;    // lowest source mip to read (a source larger than this mip)
} pc;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

const float PI = 3.14159265359;
const uint SAMPLES = 64u;

vec3 DirFromUV(vec2 uv)
{
    float theta = (uv.x - 0.5) * 2.0 * PI, phi = uv.y * PI;
    return vec3(sin(theta) * sin(phi), cos(phi), -cos(theta) * sin(phi));
}

vec2 UVFromDir(vec3 d)
{
    return vec2(atan(d.x, -d.z) / (2.0 * PI) + 0.5, acos(clamp(d.y, -1.0, 1.0)) / PI);
}

vec2 Hammersley(uint i, uint n)
{
    uint bits = bitfieldReverse(i);
    return vec2(float(i) / float(n), float(bits) * 2.3283064365386963e-10);
}

void main()
{
    vec3 N = DirFromUV(vUV);
    float a = max(pc.Roughness * pc.Roughness, 1e-4), a2 = a * a;
    vec3 up = abs(N.y) < 0.999 ? vec3(0, 1, 0) : vec3(1, 0, 0);
    vec3 tx = normalize(cross(up, N)), ty = cross(N, tx);
    vec2 size = vec2(textureSize(uTextures[nonuniformEXT(pc.Source)], 0));

    vec3 sum = vec3(0.0);
    float weight = 0.0;
    for (uint i = 0u; i < SAMPLES; ++i)
    {
        // GGX-distributed half vector.
        vec2 xi = Hammersley(i, SAMPLES);
        float phi = 2.0 * PI * xi.x;
        float cosTheta = sqrt((1.0 - xi.y) / (1.0 + (a2 - 1.0) * xi.y));
        float sinTheta = sqrt(1.0 - cosTheta * cosTheta);
        vec3 H = normalize(tx * (sinTheta * cos(phi)) + ty * (sinTheta * sin(phi)) + N * cosTheta);
        vec3 L = 2.0 * dot(N, H) * H - N;
        float NdotL = dot(N, L);
        if (NdotL <= 0.0) continue;

        // pdf of L = D * NdotH / (4 VdotH) = D / 4 with N = V; compare its solid angle with a source texel's.
        float NdotH = cosTheta;
        float d = NdotH * NdotH * (a2 - 1.0) + 1.0;
        float D = a2 / (PI * d * d);
        float pdf = D * 0.25;
        float sinPolar = sqrt(max(1.0 - L.y * L.y, 1e-4)); // equirect texels shrink toward the poles
        float texelSolidAngle = (2.0 * PI / size.x) * (PI / size.y) * sinPolar;
        float sampleSolidAngle = 1.0 / (float(SAMPLES) * pdf + 1e-4);
        float lod = clamp(max(0.5 * log2(sampleSolidAngle / texelSolidAngle) + 1.0, pc.MinLod), 0.0, pc.MaxLod);

        vec2 uv = UVFromDir(L);
        float rows = float(textureSize(uTextures[nonuniformEXT(pc.Source)], int(ceil(lod))).y);
        uv.y = clamp(uv.y, 0.5 / rows, 1.0 - 0.5 / rows); // the sampler repeats: keep the poles apart
        sum += textureLod(uTextures[nonuniformEXT(pc.Source)], uv, lod).rgb * NdotL;
        weight += NdotL;
    }
    outColor = vec4(sum / max(weight, 1e-4), 1.0);
}
