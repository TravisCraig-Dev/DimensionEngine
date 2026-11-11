#version 460
#extension GL_GOOGLE_include_directive : require
#include "Common.glsl"
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference_uvec2 : require
#include "Sway.glsl"

// Motion vectors of a moving object: each vertex where it is now and where it was last frame (its last world matrix
// and, when skinned, its last pose; simulated vertices where they were), both projected without the TAA jitter.
// Velocity.frag keeps the pixels where this surface is the one the main pass saw.

layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec3 aNormal;

struct SkinVertex
{
    uvec4 Joints;
    vec4 Weights;
};
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer SkinRef { SkinVertex Vertices[]; };
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer JointsRef { mat4 Matrices[]; };

struct VelocityObject
{
    mat4 Model;
    mat4 PrevModel;
    uvec4 Joints; // xy: this frame's skinning matrices, zw: last frame's (0 = not skinned)
    uvec4 Extra;  // xy: last frame's simulated vertex positions (cloth, soft bodies, ropes: packed xyz), 0 = none;
                  // z: grass sway or fur shell (half floats, as Mesh.vert's Motion), 0 = none; w: fur (droop, wind)
};
layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer PositionsRef { float Values[]; };
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer ObjectsRef { VelocityObject Items[]; };
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer FrameRef
{
    mat4 ViewProj;     // this frame's, unjittered
    mat4 PrevViewProj; // last frame's, unjittered
    vec4 Params;       // x: the wind's clock last frame (grass)
};

layout(push_constant) uniform Push
{
    ObjectsRef Objects; // indexed by gl_InstanceIndex (the draw's first instance)
    SkinRef Skin;       // the mesh's skin weights (skinned objects)
    FrameRef Frame;
    uint Depth;         // bindless: the scene depth
    uint Flags;         // 1: a water surface (Velocity.frag)
} pc;

layout(location = 0) out vec4 vCurrent;
layout(location = 1) out vec4 vPrevious;

mat4 SkinMatrix(uvec2 address, SkinVertex s)
{
    JointsRef joints = JointsRef(address);
    return joints.Matrices[s.Joints.x] * s.Weights.x + joints.Matrices[s.Joints.y] * s.Weights.y +
           joints.Matrices[s.Joints.z] * s.Weights.z + joints.Matrices[s.Joints.w] * s.Weights.w;
}

void main()
{
    VelocityObject o = pc.Objects.Items[gl_InstanceIndex];
    vec4 position = vec4(aPosition, 1.0), previous = position;
    vec3 normal = aNormal, prevNormal = aNormal;
    if (o.Joints.x != 0u || o.Joints.y != 0u)
    {
        SkinVertex s = pc.Skin.Vertices[gl_VertexIndex];
        mat4 now = SkinMatrix(o.Joints.xy, s), then = SkinMatrix(o.Joints.zw, s);
        position = now * position;
        previous = then * previous;
        normal = mat3(now) * aNormal;
        prevNormal = mat3(then) * aNormal;
    }
    if (o.Extra.x != 0u || o.Extra.y != 0u)
    {
        PositionsRef last = PositionsRef(o.Extra.xy);
        uint i = uint(gl_VertexIndex) * 3u;
        previous = vec4(last.Values[i], last.Values[i + 1u], last.Values[i + 2u], 1.0);
    }
    vec4 world = o.Model * position, before = o.PrevModel * previous;
    vec2 mv = unpackHalf2x16(o.Extra.z);
    if (o.Extra.z != 0u && mv.y < 0.0)
    {
        // A fur shell: pushed out along the normal as Mesh.vert does, now and at last frame's pose and wind time.
        vec2 fur = unpackHalf2x16(o.Extra.w);
        vec3 n = normalize(transpose(inverse(mat3(o.Model))) * normal);
        vec3 pn = normalize(transpose(inverse(mat3(o.PrevModel))) * prevNormal);
        world.xyz = FurMotion(world.xyz, n, mv.x, -mv.y, fur.x, fur.y, uScene.Wind.w);
        before.xyz = FurMotion(before.xyz, pn, mv.x, -mv.y, fur.x, fur.y, pc.Frame.Params.x);
    }
    else if (o.Extra.z != 0u)
    {
        // Grass in the wind: where the blade is now and where it was at last frame's wind time.
        world.xyz = GrassMotion(world.xyz, o.Model[3].xyz, mv, uScene.Wind.w);
        before.xyz = GrassMotion(before.xyz, o.PrevModel[3].xyz, mv, pc.Frame.Params.x);
    }
    vCurrent = pc.Frame.ViewProj * world;
    vPrevious = pc.Frame.PrevViewProj * before;
    gl_Position = uScene.ViewProj * world; // jittered as the main pass drew it
}
