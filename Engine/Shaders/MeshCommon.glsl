// Per-draw data for mesh shaders. Must match MeshPush/MaterialGPU in SceneRenderer.cpp.
#extension GL_EXT_buffer_reference : require

layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer MaterialRef
{
    vec4 BaseColor;     // linear rgb, alpha
    vec4 Emission;      // linear rgb * strength
    vec4 Params;        // x metallic, y roughness, z alpha cutoff, w normal strength
    vec4 TilingOffset;  // xy tiling, zw offset
    uvec4 Maps;         // bindless: base, normal, metallic-roughness, emission
    uvec4 Flags;        // x blend mode (0 opaque, 1 cutout, 2 transparent), y has normal map
};

struct SkinVertex
{
    uvec4 Joints;
    vec4 Weights;
};
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer SkinRef { SkinVertex Vertices[]; };
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer JointsRef { mat4 Matrices[]; };

// GPU instancing: one entry per instance, indexed by gl_InstanceIndex.
struct MeshInstance
{
    mat4 Model;
    uvec4 Data; // x: entity id
};
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer InstancesRef { MeshInstance Items[]; };

layout(push_constant) uniform Push
{
    mat4 Model;
    MaterialRef Material;
    SkinRef Skin;       // valid when Flags & 1
    JointsRef Joints;   // skinning matrices (mesh space)
    InstancesRef Instances; // valid when Flags & 2 (Model and EntityId then come from here)
    uint EntityId;
    uint Flags;         // bit 0: skinned, bit 1: instanced
    uint Probe;         // reflection probe slot + 1 (0 = sky)
} pc;
