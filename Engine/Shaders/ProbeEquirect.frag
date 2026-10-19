#version 460
#extension GL_EXT_nonuniform_qualifier : require

// Reflection probe: resamples the six captured faces into one equirectangular image (same layout as HDR skies,
// without the sky rotation). Each face was rendered looking along Forward[f] with up Up[f].

layout(set = 0, binding = 0) uniform sampler2D uTextures[];

layout(push_constant) uniform Push
{
    uint Faces[6]; // bindless indices: +X, -X, +Y, -Y, +Z, -Z
} pc;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

const vec3 FORWARD[6] = vec3[](vec3(1, 0, 0), vec3(-1, 0, 0), vec3(0, 1, 0), vec3(0, -1, 0), vec3(0, 0, 1), vec3(0, 0, -1));
const vec3 UP[6] = vec3[](vec3(0, 1, 0), vec3(0, 1, 0), vec3(0, 0, 1), vec3(0, 0, 1), vec3(0, 1, 0), vec3(0, 1, 0));

void main()
{
    // Inverse of DirToEquirect: u = atan(x, -z) / 2pi + 0.5, v = acos(y) / pi.
    float theta = (vUV.x - 0.5) * 6.28318530718, phi = vUV.y * 3.14159265359;
    vec3 d = vec3(sin(theta) * sin(phi), cos(phi), -cos(theta) * sin(phi));

    vec3 a = abs(d);
    int face = a.x >= a.y && a.x >= a.z ? (d.x > 0.0 ? 0 : 1) : a.y >= a.z ? (d.y > 0.0 ? 2 : 3) : (d.z > 0.0 ? 4 : 5);
    vec3 f = FORWARD[face], up = UP[face], right = normalize(cross(f, up));
    float depth = dot(d, f);
    vec2 ndc = vec2(dot(d, right), dot(d, up)) / depth; // 90 degree frustum: [-1, 1]
    vec2 uv = vec2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5); // the face image's top row is +up
    outColor = vec4(textureLod(uTextures[nonuniformEXT(pc.Faces[face])], uv, 0.0).rgb, 1.0);
}
