// Per-draw data for mesh shaders. Must match MeshPush/MaterialGPU in SceneRenderer.cpp.
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference_uvec2 : require

layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer MaterialRef
{
    vec4 BaseColor;     // linear rgb, alpha
    vec4 Emission;      // linear rgb * strength, w refraction (transparent)
    vec4 Params;        // x metallic, y roughness, z alpha cutoff, w normal strength
    vec4 TilingOffset;  // xy tiling, zw offset
    uvec4 Maps;         // bindless: base, normal, metallic-roughness, emission
    uvec4 Flags;        // x blend mode (0 opaque, 1 cutout, 2 transparent), y has normal map, z 1 = terrain, w 1 = fur shell
    vec4 Layers[2];     // fur: [0] density, thickness, droop, wind response; [1] tip color (rgb), root shade
    uvec4 Maps2;        // terrain: xy its layers' address (TerrainLayersRef), z their count, w height blend (float bits)
};

// A terrain's surface layers (TerrainLayerGPU in SceneRenderer.cpp).
struct TerrainLayer
{
    vec4 Color;  // linear rgb, w texture repeats per meter
    uvec4 Maps;  // bindless: x texture, y splat map, w normal map (~0 = none); z its channel in the splat map
    vec4 Params; // x normal strength, y 1 = triplanar
};
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer TerrainLayersRef { TerrainLayer Layers[]; };

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
    uint Flags;         // bit 0: skinned, bit 1: instanced, bits 8-15: LOD cross-fade coverage (0 = none), bit 16: its side
    uint Motion;        // grass tufts: half floats (sway, bending); fur shells: (length, -height 0-1); 0 = neither
} pc;

// LOD cross-fade: two levels share the pixels by a 4x4 ordered dither, one keeping those under the coverage, the
// other the rest (called with gl_FragCoord.xy).
bool LodFadeDiscard(vec2 fragCoord)
{
    uint coverage = (pc.Flags >> 8) & 255u;
    if (coverage == 0u) return false;
    const float bayer[16] = float[16](0.0, 8.0, 2.0, 10.0, 12.0, 4.0, 14.0, 6.0, 3.0, 11.0, 1.0, 9.0, 15.0, 7.0, 13.0, 5.0);
    ivec2 p = ivec2(fragCoord) & 3;
    bool under = (bayer[p.y * 4 + p.x] + 0.5) / 16.0 < float(coverage) / 255.0;
    return ((pc.Flags >> 16) & 1u) == 0u ? !under : under;
}

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

// Impostors: the point a pixel of the quad stands for, moved along the view ray by the depth its texel had when the
// mesh was captured (the surface atlas's red channel: how far behind the impostor's center plane, in radii; Layers[0].x
// is the radius in the world).
vec3 ImpostorPointAlong(MaterialRef m, vec2 uv, vec3 world, vec3 ray)
{
    return world + ray * (SampleTexture(m.Maps.z, uv).r * m.Layers[0].x);
}
vec3 ImpostorPoint(MaterialRef m, vec2 uv, vec3 world)
{
    vec3 ray = uScene.Proj[3][3] > 0.5 ? -vec3(uScene.View[0][2], uScene.View[1][2], uScene.View[2][2]) : normalize(world - uScene.CameraPosition.xyz);
    return ImpostorPointAlong(m, uv, world, ray);
}
// In a shadow map (only ViewProj is the light's): the direction away from the light, for an orthographic (sun) view;
// none for a point light's perspective faces.
vec3 ImpostorLightRay()
{
    mat4 vp = uScene.ViewProj;
    if (abs(vp[0][3]) + abs(vp[1][3]) + abs(vp[2][3]) > 1e-6) return vec3(0.0);
    vec3 x = vec3(vp[0][0], vp[1][0], vp[2][0]), y = vec3(vp[0][1], vp[1][1], vp[2][1]), z = vec3(vp[0][2], vp[1][2], vp[2][2]);
    vec3 d = normalize(cross(x, y));
    return dot(z, d) < 0.0 ? -d : d;
}
float ImpostorDepth(vec3 world)
{
    vec4 clip = uScene.ViewProj * vec4(world, 1.0);
    return clamp(clip.z / clip.w, 0.0, 1.0);
}
