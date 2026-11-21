#version 460
#extension GL_GOOGLE_include_directive : require
#include "Common.glsl"
#include "MeshCommon.glsl"

layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;
layout(location = 3) in vec4 aTangent; // w = 0: no tangent
layout(location = 4) in vec4 aColor;   // vertex color (sRGB bytes; white = none)

layout(location = 0) out vec3 vWorldPos;
layout(location = 1) out vec3 vNormal;
layout(location = 2) out vec2 vUV;
layout(location = 3) flat out uint vEntity;
layout(location = 4) out vec4 vTangent;
layout(location = 5) out float vShell; // fur shells: height 0-1 (0 for everything else)
layout(location = 6) out float vFoam;  // water: foam 0-1, carried as the normal's length above 1
layout(location = 7) flat out vec4 vTint; // instances with their own color (Flags bit 17: Data.yz, half floats; linear)
layout(location = 8) out vec4 vColor;     // the vertex color, linear
layout(location = 9) flat out uint vFade;  // LOD cross-fade (see LodFadeDiscard): the draw's, or the instance's

invariant gl_Position; // the depth prepass and the main pass must produce identical depths

#include "Sway.glsl"

void main()
{
    vec3 position = aPosition;
    vec3 normal = aNormal;
    vFoam = clamp(length(aNormal) - 1.0, 0.0, 1.0);
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
    vFade = (pc.Flags >> 8) & 511u;
    uint motion = pc.Motion;
    if ((pc.Flags & 2u) != 0u)
    {
        model = pc.Instances.Items[gl_InstanceIndex].Model;
        uint id = pc.Instances.Items[gl_InstanceIndex].Data.x;
        vEntity = id & 0x7FFFFFu;
        vFade = id >> 23;
        motion = pc.Instances.Items[gl_InstanceIndex].Data.w;
    }
    vTint = vec4(1.0);
    if ((pc.Flags & 0x20002u) == 0x20002u)
    {
        uvec4 data = pc.Instances.Items[gl_InstanceIndex].Data;
        vTint = vec4(unpackHalf2x16(data.y), unpackHalf2x16(data.z));
    }
    vec4 world = model * vec4(position, 1.0);
    vNormal = normalize(transpose(inverse(mat3(model))) * normal);
    vShell = 0.0;
    if (motion != 0u)
    {
        vec2 mv = unpackHalf2x16(motion);
        if (mv.y < 0.0)
        {
            // A fur shell: out along the normal, the tips hanging down and swaying in the wind.
            vec4 fur = pc.Material.Layers[0];
            float h = -mv.y, len = mv.x;
            vShell = h;
            world.xyz = FurMotion(world.xyz, vNormal, len, h, fur.z, fur.w, uScene.Wind.w);
        }
        else
            world.xyz = GrassMotion(world.xyz, model[3].xyz, mv, uScene.Wind.w);
    }
    vWorldPos = world.xyz;
    // Tangents transform like positions (they lie in the surface); a mirrored model flips the handedness.
    vTangent = aTangent.w == 0.0 ? vec4(0.0) : vec4(mat3(model) * tangent, aTangent.w * sign(determinant(mat3(model))));
    vUV = aUV;
    vColor = vec4(mix(aColor.rgb / 12.92, pow((aColor.rgb + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), aColor.rgb)), aColor.a);
    gl_Position = uScene.ViewProj * world;
}
