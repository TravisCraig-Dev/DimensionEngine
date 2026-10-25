#pragma once
#include "Dimension/Renderer/Buffer.h"
#include "Dimension/Animation/Animation.h"
#include "Dimension/Renderer/Material.h"
#include <glm/glm.hpp>

namespace Dimension
{
    struct Vertex
    {
        glm::vec3 Position;
        glm::vec3 Normal;
        glm::vec2 UV;
        glm::vec4 Tangent{ 0.0f }; // xyz tangent, w handedness (+-1); all zero = none (shaders derive a frame)
    };

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

    // CPU-side mesh as imported from a file (no GPU resources; usable in tools and tests).
    struct MeshData
    {
        std::vector<Vertex> Vertices;
        std::vector<uint32_t> Indices;
        std::vector<SkinVertex> Skin;     // empty for static meshes
        Ref<Skeleton> SkeletonData;       // null for static meshes
        std::vector<AnimationClip> Clips;
        std::string Name;
        std::vector<SubMesh> SubMeshes;          // at least one after loading
        std::vector<ImportedMaterial> Materials; // material slots (glTF materials)
        std::vector<ImportedImage> Images;

        static bool LoadFromFile(const fs::path& path, MeshData& out);
        static MeshData FromArrays(std::vector<Vertex> vertices, std::vector<uint32_t> indices, const std::string& name);
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
        uint32_t GetIndexCount() const { return m_IndexCount; }
        uint32_t GetVertexCount() const { return m_VertexCount; }
        const AABB& GetBounds() const { return m_Bounds; }
        const std::string& GetName() const { return m_Name; }
        const std::vector<SubMesh>& GetSubMeshes() const { return m_SubMeshes; }

        bool IsSkinned() const { return m_Skeleton != nullptr; }
        const Skeleton* GetSkeleton() const { return m_Skeleton.get(); }
        const std::vector<AnimationClip>& GetClips() const { return m_Clips; }
        VkDeviceAddress GetSkinAddress() const { return m_SkinAddress; }

    private:
        void Upload(const std::vector<Vertex>& vertices, const std::vector<uint32_t>& indices);

        Buffer m_VertexBuffer, m_IndexBuffer, m_SkinBuffer;
        VkDeviceAddress m_SkinAddress = 0;
        uint32_t m_IndexCount = 0, m_VertexCount = 0;
        AABB m_Bounds;
        std::string m_Name;
        Ref<Skeleton> m_Skeleton;
        std::vector<AnimationClip> m_Clips;
        std::vector<SubMesh> m_SubMeshes;
    };
}
