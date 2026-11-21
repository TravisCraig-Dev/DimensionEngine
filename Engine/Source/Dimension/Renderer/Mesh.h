#pragma once
#include "Dimension/Renderer/Buffer.h"
#include "Dimension/Animation/Animation.h"
#include "Dimension/Renderer/Material.h"
#include <glm/glm.hpp>
#include <algorithm>
#include <cmath>

namespace Dimension
{
    struct Vertex
    {
        glm::vec3 Position;
        glm::vec3 Normal;
        glm::vec2 UV;
        glm::vec4 Tangent{ 0.0f }; // xyz tangent, w handedness (+-1); all zero = none (shaders derive a frame)
        uint32_t Color = 0xFFFFFFFFu; // vertex color, RGBA8 with sRGB color (multiplies the base color); white = none
    };
    static_assert(sizeof(Vertex) == 52, "Vertex is 13 tightly packed 4-byte values (BlendShapes.comp reads it so)");

    // RGBA8 colors (red in the lowest byte, as images and the vertex Color store them) to and from 0..1 floats.
    inline glm::vec4 UnpackRGBA8(uint32_t c)
    {
        return glm::vec4((float)(c & 0xFF), (float)((c >> 8) & 0xFF), (float)((c >> 16) & 0xFF), (float)(c >> 24)) / 255.0f;
    }
    inline uint32_t PackRGBA8(const glm::vec4& v)
    {
        auto b = [](float x) { return (uint32_t)(std::clamp(x, 0.0f, 1.0f) * 255.0f + 0.5f); };
        return b(v.r) | (b(v.g) << 8) | (b(v.b) << 16) | (b(v.a) << 24);
    }
    inline float SrgbToLinear(float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }
    inline float LinearToSrgb(float c) { return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f; }

    // Per-vertex skinning data (std430-compatible; read by the vertex shader through a device address).
    struct SkinVertex
    {
        glm::uvec4 Joints{ 0 };
        glm::vec4 Weights{ 1.0f, 0.0f, 0.0f, 0.0f };
    };

    struct AABB
    {
        glm::vec3 Min{ 0.0f }, Max{ 0.0f };
        glm::vec3 Center() const { return (Min + Max) * 0.5f; }
        glm::vec3 Extents() const { return (Max - Min) * 0.5f; }
    };

    // A range of the index buffer drawn with one material slot (-1 = the renderer's own material).
    struct SubMesh
    {
        uint32_t IndexOffset = 0;
        uint32_t IndexCount = 0;
        int32_t MaterialSlot = -1;
    };

    // An image referenced by an imported material: an external file, or encoded PNG/JPEG bytes embedded in the model.
    struct ImportedImage
    {
        std::string Name;
        fs::path File;              // absolute path (external image)
        std::vector<uint8_t> Bytes; // embedded image
    };

    // A material read from a model file. Texture handles in Values are unset; images are referenced by index.
    struct ImportedMaterial
    {
        std::string Name;
        Material Values;
        int32_t BaseColorImage = -1, NormalImage = -1, MetallicRoughnessImage = -1, EmissionImage = -1;
    };

    // Per-model import settings (stored in the model's .meta).
    struct MeshImportSettings
    {
        float Scale = 1.0f;          // uniform: vertices, skeleton, bind poses and animated translations
        bool ImportAnimations = true;
        bool ImportMaterials = true; // false: every part uses the Mesh Renderer's material
        std::vector<std::string> ExcludedClips; // animation clips (by name) left out of the import
        // Keyframe reduction of the clips (Animation::ReduceKeys): keys within these errors of the curve are dropped.
        bool ReduceKeyframes = false;
        float RotationError = 0.5f;  // degrees
        float PositionError = 0.001f; // meters (after Scale)
        std::string Key() const;     // distinguishes cached imports with other settings
        bool IsDefault() const { return Scale == 1.0f && ImportAnimations && ImportMaterials && ExcludedClips.empty() && !ReduceKeyframes; }
    };

    // CPU-side mesh as imported from a file (no GPU resources; usable in tools and tests).
    // A blend shape (glTF morph target, Unity's blend shape): per vertex, how far its position and normal move at full
    // weight.
    struct BlendShape
    {
        std::string Name;
        std::vector<glm::vec3> Positions, Normals; // one per vertex
        float DefaultWeight = 0.0f;                // the file's own weight for it (0..1)
    };

    struct MeshData
    {
        std::vector<Vertex> Vertices;
        std::vector<uint32_t> Indices;
        std::vector<SkinVertex> Skin;     // empty for static meshes
        std::vector<BlendShape> BlendShapes;
        Ref<Skeleton> SkeletonData;       // null for static meshes
        std::vector<AnimationClip> Clips;
        std::string Name;
        std::vector<SubMesh> SubMeshes;          // at least one after loading
        std::vector<ImportedMaterial> Materials; // material slots (glTF materials)
        std::vector<ImportedImage> Images;

        static bool LoadFromFile(const fs::path& path, MeshData& out, const MeshImportSettings& settings = {});
        // The vertices with blend shapes applied (`weights` 0..1 by shape index, `count` of them; the rest count 0):
        // positions and normals move, tangents follow the normals. Writes each of Vertices.size() vertices once (so
        // `out` may be mapped GPU memory) and returns their bounds.
        AABB ApplyBlendShapes(const float* weights, size_t count, Vertex* out) const;
        int FindBlendShape(const std::string& name) const; // -1 = none
        // Applies import settings to freshly imported data.
        void ApplyImportSettings(const MeshImportSettings& settings);
        void ReduceClipKeys(const MeshImportSettings& settings); // (keyframe reduction, when on)
        static MeshData FromArrays(std::vector<Vertex> vertices, std::vector<uint32_t> indices, const std::string& name);
        // Fills in tangents for vertices that have none, from the UV layout (as MikkTSpace does: per-corner tangents
        // weighted by corner angle, orthogonalized against the normal). Where triangles with mirrored UVs share a
        // vertex, it is split so each side keeps its own handedness. Triangles without UV area add nothing.
        void GenerateTangents();
        // Built-in primitives (CPU data; Mesh::Create* uploads them).
        static MeshData Cube();
        static MeshData Sphere(uint32_t segments = 48, uint32_t rings = 24);
        static MeshData Plane();
        static MeshData Quad();
        static MeshData Cylinder(uint32_t segments = 48);
    };

    // Immutable GPU mesh (single vertex/index buffer). Imported files are merged into one mesh.
    // Skinned meshes additionally carry a skeleton, animation clips and a per-vertex skin buffer.
    class Mesh
    {
    public:
        Mesh(std::vector<Vertex> vertices, std::vector<uint32_t> indices, const std::string& name);
        explicit Mesh(MeshData&& data);

        static Ref<Mesh> LoadFromFile(const fs::path& path);
        static Ref<Mesh> CreateCube();
        static Ref<Mesh> CreateSphere(uint32_t segments = 48, uint32_t rings = 24);
        static Ref<Mesh> CreatePlane(); // 10x10 on XZ, like Unity's Plane
        static Ref<Mesh> CreateQuad();  // 1x1 on XY facing +Z
        static Ref<Mesh> CreateCylinder(uint32_t segments = 48);

        void Bind(VkCommandBuffer cmd) const;
        // Rewrites vertices [first, first + count), recorded into `cmd` outside render passes: draws submitted
        // earlier finish reading them first, and later ones see the new ones (an edited terrain's changed patch).
        void UpdateVertices(VkCommandBuffer cmd, uint32_t first, const Vertex* vertices, uint32_t count);
        uint32_t GetIndexCount() const { return m_IndexCount; }
        uint32_t GetVertexCount() const { return m_VertexCount; }
        const AABB& GetBounds() const { return m_Bounds; }
        const std::string& GetName() const { return m_Name; }
        const std::vector<SubMesh>& GetSubMeshes() const { return m_SubMeshes; }

        bool IsSkinned() const { return m_Skeleton != nullptr; }
        uint32_t GetBlendShapeCount() const { return m_BlendShapeCount; }
        // GPU blend shapes: the vertex buffer's address, the shapes' deltas (two vec4 per vertex per shape: position,
        // normal; shape by shape), and how far each shape moves any vertex along each axis at full weight (bounds).
        VkDeviceAddress GetVertexAddress() const { return m_VertexAddress; }
        VkDeviceAddress GetBlendShapeAddress() const { return m_BlendAddress; }
        const std::vector<glm::vec3>& GetBlendShapeReach() const { return m_BlendReach; }
        const Skeleton* GetSkeleton() const { return m_Skeleton.get(); }
        const std::vector<AnimationClip>& GetClips() const { return m_Clips; }
        VkDeviceAddress GetSkinAddress() const { return m_SkinAddress; }

    private:
        void Upload(const std::vector<Vertex>& vertices, const std::vector<uint32_t>& indices);

        Buffer m_VertexBuffer, m_IndexBuffer, m_SkinBuffer, m_BlendBuffer;
        uint32_t m_BlendShapeCount = 0;
        VkDeviceAddress m_VertexAddress = 0, m_BlendAddress = 0;
        std::vector<glm::vec3> m_BlendReach;
        VkDeviceAddress m_SkinAddress = 0;
        uint32_t m_IndexCount = 0, m_VertexCount = 0;
        AABB m_Bounds;
        std::string m_Name;
        Ref<Skeleton> m_Skeleton;
        std::vector<AnimationClip> m_Clips;
        std::vector<SubMesh> m_SubMeshes;
    };
}
