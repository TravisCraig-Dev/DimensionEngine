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

// Grass tufts: the wind sways them (stronger higher up, in gusts that travel across the field, with a flutter) and
// bodies standing in them push them away. `root` is the tuft's base, `params` its sway and bending. Each point
// moves sideways by an amount growing with its height above the base and sinks so the blade keeps its length.
vec3 GrassMotion(vec3 world, vec3 root, vec2 params)
{
    float h = max(world.y - root.y, 0.0);
    if (h <= 0.0) return world;
    vec2 offset = vec2(0.0);
    float time = uScene.Wind.w;
    vec2 wind = uScene.Wind.xz;
    float speed = length(wind);
    if (speed > 1e-3 && params.x > 0.0)
    {
        vec2 dir = wind / speed;
        float wave = dot(root.xz, dir) * 0.25 - time * (1.0 + speed * 0.3);
        float gust = 0.6 + 0.4 * sin(wave) + uScene.WindParams.x * 0.5 * max(sin(wave * 0.31 + 1.7), 0.0);
        float flutter = sin(time * 7.0 + root.x * 3.1 + root.z * 2.3) * 0.12;
        offset += (wind * gust + vec2(-dir.y, dir.x) * speed * flutter) * (0.12 * params.x * h * h);
    }
    int benders = int(uScene.WindParams.y);
    for (int i = 0; i < benders && params.y > 0.0; ++i)
    {
        vec4 b = uScene.GrassBenders[i];
        vec2 away = root.xz - b.xz;
        float d = length(away);
        // Nearest point of the blade's axis to the sphere: the push fades with that distance.
        float axisY = clamp(b.y, root.y, root.y + h);
        float gap = length(vec3(d, axisY - b.y, 0.0)) - b.w;
        float push = 1.0 - smoothstep(0.0, b.w * 0.8 + 0.1, gap);
        offset += (d > 1e-4 ? away / d : vec2(1.0, 0.0)) * push * params.y * h * 1.4;
    }
    float len = length(offset);
    if (len > h * 0.9) offset *= h * 0.9 / len;
    len = min(len, h * 0.9);
    return vec3(world.x + offset.x, world.y - (h - sqrt(h * h - len * len)), world.z + offset.y);
}

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
    uint motion = pc.Motion;
    if ((pc.Flags & 2u) != 0u)
    {
        model = pc.Instances.Items[gl_InstanceIndex].Model;
        vEntity = pc.Instances.Items[gl_InstanceIndex].Data.x;
        motion = pc.Instances.Items[gl_InstanceIndex].Data.w;
    }
    vec4 world = model * vec4(position, 1.0);
    if (motion != 0u) world.xyz = GrassMotion(world.xyz, model[3].xyz, unpackHalf2x16(motion));
    vWorldPos = world.xyz;
    vNormal = normalize(transpose(inverse(mat3(model))) * normal);
    // Tangents transform like positions (they lie in the surface); a mirrored model flips the handedness.
    vTangent = aTangent.w == 0.0 ? vec4(0.0) : vec4(mat3(model) * tangent, aTangent.w * sign(determinant(mat3(model))));
    vUV = aUV;
    gl_Position = uScene.ViewProj * world;
}
