#version 460
#extension GL_GOOGLE_include_directive : require
#include "Common.glsl"
#include "MeshCommon.glsl"

layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;

layout(location = 0) out vec3 vWorldPos;
layout(location = 1) out vec3 vNormal;
layout(location = 2) out vec2 vUV;

invariant gl_Position; // the depth prepass and the main pass must produce identical depths

void main()
{
    vec3 position = aPosition;
    vec3 normal = aNormal;
    if ((pc.Flags & 1u) != 0u)
    {
        // Linear blend skinning with up to 4 joints per vertex.
        SkinVertex s = pc.Skin.Vertices[gl_VertexIndex];
        mat4 skin = pc.Joints.Matrices[s.Joints.x] * s.Weights.x + pc.Joints.Matrices[s.Joints.y] * s.Weights.y +
                    pc.Joints.Matrices[s.Joints.z] * s.Weights.z + pc.Joints.Matrices[s.Joints.w] * s.Weights.w;
        position = vec3(skin * vec4(aPosition, 1.0));
        normal = mat3(skin) * aNormal;
    }
    vec4 world = pc.Model * vec4(position, 1.0);
    vWorldPos = world.xyz;
    vNormal = normalize(transpose(inverse(mat3(pc.Model))) * normal);
    vUV = aUV;
    gl_Position = uScene.ViewProj * world;
}
