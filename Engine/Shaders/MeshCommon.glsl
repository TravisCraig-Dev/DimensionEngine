// Per-draw data for mesh shaders. Must match MeshPush/MaterialGPU in SceneRenderer.cpp.
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference_uvec2 : require

layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer MaterialRef
{
    vec4 BaseColor;     // linear rgb, alpha
    vec4 Emission;      // linear rgb * strength, w refraction (transparent)
    vec4 Params;        // x metallic, y roughness, z alpha cutoff, w normal strength
    vec4 TilingOffset;  // xy tiling, zw offset
    uvec4 Maps;         // bindless: base, normal, metallic-roughness, emission (terrain: splat, grass, rock, snow)
    uvec4 Flags;        // x blend mode (0 opaque, 1 cutout, 2 transparent), y has normal map, z 1 = terrain, w 1 = fur shell
    vec4 Layers[6];     // terrain: grass, rock, snow, dirt, sand and moss tint (linear rgb), w texture repeats per meter
                        // fur: [0] density, thickness, droop, wind response; [1] tip color (rgb), root shade
    uvec4 Maps2;        // terrain: second splat map, dirt, sand, moss
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
    uvec4 Data; // x: entity id, yz: skinning matrices address (skinned instances), w: grass motion (see Push)
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
    uint Motion;        // grass tufts: half floats (sway, bending); fur shells: (length, -height 0-1); 0 = neither
} pc;

// Fur: whether a shell at `shell` (0 root - 1 tip) has no strand at this UV. Strands stand on a grid of Density cells
// per UV unit, each jittered, of a random height, tapering from Thickness at the root to nothing at its tip.
uint FurHash(uvec2 v)
{
    uint h = v.x * 0x8da6b343u ^ v.y * 0xd8163841u;
    h ^= h >> 15;
    h *= 0x2c1b3c6du;
    h ^= h >> 12;
    h *= 0x297a2d39u;
    return h ^ (h >> 15);
}
bool FurDiscard(MaterialRef m, vec2 uv, float shell)
{
    if (m.Flags.w != 1u) return false;
    vec2 cell = uv * m.Layers[0].x;
    uvec2 id = uvec2(ivec2(floor(cell)) + 32768);
    uint h = FurHash(id);
    float height = 0.55 + 0.45 * float(h & 1023u) / 1023.0;
    if (shell > height) return true;
    vec2 center = vec2(0.5) + (vec2(float((h >> 10) & 1023u), float((h >> 20) & 1023u)) / 1023.0 - 0.5) * 0.4;
    float radius = 0.5 * m.Layers[0].y * (1.0 - shell / height);
    return length(fract(cell) - center) > radius;
}
