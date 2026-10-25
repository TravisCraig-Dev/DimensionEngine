#version 460
#extension GL_GOOGLE_include_directive : require
#include "Common.glsl"
#include "MeshCommon.glsl"

layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;
layout(location = 3) in vec4 aTangent; // w = 0: no tangent

layout(location = 0) out vec3 vWorldPos;
layout(location = 1) out vec3 vNormal;
layout(location = 2) out vec2 vUV;
layout(location = 3) flat out uint vEntity;
layout(location = 4) out vec4 vTangent;

invariant gl_Position; // the depth prepass and the main pass must produce identical depths

void main()
{
    vec3 position = aPosition;
    vec3 normal = aNormal;
    vec3 tangent = aTangent.xyz;
    if ((pc.Flags & 1u) != 0u)
    {
        // Linear blend skinning with up to 4 joints per vertex. Instances of a skinned mesh share its weights but
        // each has its own pose.
        JointsRef joints = (pc.Flags & 2u) != 0u ? JointsRef(pc.Instances.Items[gl_InstanceIndex].Data.yz) : pc.Joints;
        SkinVertex s = pc.Skin.Vertices[gl_VertexIndex];
        mat4 skin = joints.Matrices[s.Joints.x] * s.Weights.x + joints.Matrices[s.Joints.y] * s.Weights.y +
                    joints.Matrices[s.Joints.z] * s.Weights.z + joints.Matrices[s.Joints.w] * s.Weights.w;
        position = vec3(skin * vec4(aPosition, 1.0));
        normal = mat3(skin) * aNormal;
        tangent = mat3(skin) * aTangent.xyz;
    }
    mat4 model = pc.Model;
    vEntity = pc.EntityId;
    if ((pc.Flags & 2u) != 0u)
    {
        model = pc.Instances.Items[gl_InstanceIndex].Model;
        vEntity = pc.Instances.Items[gl_InstanceIndex].Data.x;
    }
    vec4 world = model * vec4(position, 1.0);
    vWorldPos = world.xyz;
    vNormal = normalize(transpose(inverse(mat3(model))) * normal);
    // Tangents transform like positions (they lie in the surface); a mirrored model flips the handedness.
    vTangent = aTangent.w == 0.0 ? vec4(0.0) : vec4(mat3(model) * tangent, aTangent.w * sign(determinant(mat3(model))));
    vUV = aUV;
    gl_Position = uScene.ViewProj * world;
}
