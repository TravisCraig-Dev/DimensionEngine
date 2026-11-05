#version 460
#extension GL_GOOGLE_include_directive : require
#include "Common.glsl"
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference_uvec2 : require

// Motion vectors of a moving object: each vertex where it is now and where it was last frame (its last world matrix
// and, when skinned, its last pose), both projected without the TAA jitter. Velocity.frag keeps the pixels where this
// surface is the one the main pass saw.

layout(location = 0) in vec3 aPosition;

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
};
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer ObjectsRef { VelocityObject Items[]; };
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer FrameRef
{
    mat4 ViewProj;     // this frame's, unjittered
    mat4 PrevViewProj; // last frame's, unjittered
};

layout(push_constant) uniform Push
{
    ObjectsRef Objects; // indexed by gl_InstanceIndex (the draw's first instance)
    SkinRef Skin;       // the mesh's skin weights (skinned objects)
    FrameRef Frame;
    uint Depth;         // bindless: the scene depth
    uint Pad;
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
    if (o.Joints.x != 0u || o.Joints.y != 0u)
    {
        SkinVertex s = pc.Skin.Vertices[gl_VertexIndex];
        position = SkinMatrix(o.Joints.xy, s) * position;
        previous = SkinMatrix(o.Joints.zw, s) * previous;
    }
    vec4 world = o.Model * position;
    vCurrent = pc.Frame.ViewProj * world;
    vPrevious = pc.Frame.PrevViewProj * (o.PrevModel * previous);
    gl_Position = uScene.ViewProj * world; // jittered as the main pass drew it
}
