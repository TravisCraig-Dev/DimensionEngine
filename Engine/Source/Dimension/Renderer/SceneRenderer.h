#pragma once
#include "Dimension/Renderer/RenderTarget.h"
#include "Dimension/Renderer/Pipeline.h"
#include "Dimension/Renderer/Mesh.h"
#include "Dimension/Renderer/Image.h"
#include "Dimension/ECS/Entity.h"
#include <glm/glm.hpp>
#include <array>

namespace Dimension
{
    class Scene;
    class Texture2D;
    struct SceneSettings;

    struct CameraData
    {
        glm::mat4 View{ 1.0f };
        glm::mat4 Projection{ 1.0f };
        glm::vec3 Position{ 0.0f };
        glm::vec4 ClearColor{ 0.1f, 0.1f, 0.1f, 1.0f }; // sRGB
        bool Orthographic = false;
        bool Skybox = false;

        // Builds camera data from a scene entity with a CameraComponent.
        static CameraData FromEntity(Scene& scene, Entity camera, float aspect);
    };

    struct SceneRenderOptions
    {
        bool EditorOverlays = false; // grid, icons, selection outlines, collider wireframes
        bool Grid = true;
        bool Grid2D = false;         // XY grid instead of XZ
        bool RenderUI = false;       // draw screen-space canvases (Game view / Player)
        bool DefaultLight = false;   // studio sun when the scene has no directional light (Prefab Mode)
        bool ProbeCapture = false;   // internal: HDR scene only, no post or probes (reflection probe faces)
        Entity Selected = Entity::Null();          // primary selection (camera frustum, light gizmos)
        std::vector<Entity> Selection;             // all selected entities (outlines)
    };

    // Directional shadow cascades for one frame.
    struct ShadowCascades
    {
        static constexpr int kMax = 4;
        int Count = 0;
        glm::mat4 ViewProj[kMax]{};
        float Splits[kMax]{}; // far view depth of each cascade
        float Texel[kMax]{};  // world size of one shadow texel
    };

    // Cascade far distances between nearDepth and farDepth (view depth), blending logarithmic and
    // uniform splits by lambda (0 = uniform, 1 = logarithmic). The last split is always farDepth.
    std::array<float, ShadowCascades::kMax> ComputeCascadeSplits(float nearDepth, float farDepth, int count, float lambda);

    // View-projection of cube face `face` (+X, -X, +Y, -Y, +Z, -Z) for a point light shadow (90 degree FOV).
    glm::mat4 PointShadowFaceViewProj(const glm::vec3& position, int face, float nearPlane, float farPlane);
    // View-projection for a spot light shadow covering its outer cone.
    glm::mat4 SpotShadowViewProj(const glm::vec3& position, const glm::vec3& direction, float spotAngleDegrees, float nearPlane, float farPlane);

    struct RenderStats
    {
        uint32_t DrawCalls = 0;
        uint32_t Triangles = 0;
        uint32_t Meshes = 0;
        uint32_t Sprites = 0;
        uint32_t Culled = 0;
        uint32_t ShadowCasters = 0;
        uint32_t InstancedMeshes = 0; // meshes drawn as part of an instanced draw (main pass)
        uint32_t RenderListItems = 0; // mesh renderers resolved this frame (once, shared by all passes)
        float CpuMs = 0.0f;           // CPU time spent recording this view
        float ListMs = 0.0f;          // part of CpuMs: building the render list
    };

    // Shared GPU resources for all scene renderers (pipelines, index buffers, icons).
    // Created by the Application after the RenderDevice.
    class Renderer
    {
    public:
        static void Init();
        static void Shutdown();

        struct Resources;
        static Resources& Get();
    };

    // Records the commands to draw a scene from one camera into a SceneRenderTarget.
    // Also offers an immediate-mode debug line API, flushed on the next Render.
    class SceneRenderer
    {
    public:
        SceneRenderer() = default;
        ~SceneRenderer();
        SceneRenderer(const SceneRenderer&) = delete;
        SceneRenderer& operator=(const SceneRenderer&) = delete;

        void Render(VkCommandBuffer cmd, Scene& scene, const CameraData& camera, SceneRenderTarget& target,
                    const SceneRenderOptions& options = {});

        void DrawLine(const glm::vec3& a, const glm::vec3& b, const glm::vec4& color);
        void DrawWireBox(const glm::mat4& transform, const glm::vec3& center, const glm::vec3& halfExtents, const glm::vec4& color);
        void DrawWireCircle(const glm::mat4& transform, const glm::vec3& center, float radius, const glm::vec4& color, int axis = 2);
        void DrawWireSphere(const glm::mat4& transform, const glm::vec3& center, float radius, const glm::vec4& color);

        const RenderStats& GetStats() const { return m_Stats; }

        // GPU instancing: identical opaque meshes (same mesh, sub-mesh, material and pipeline) are drawn with one
        // instanced call. On by default; turning it off is useful for comparisons.
        static void SetInstancingEnabled(bool enabled);
        static bool IsInstancingEnabled();
        // Re-captures every reflection probe on the next frame (all renderers).
        static void RequestProbeBake();
        static constexpr uint32_t kMaxReflectionProbes = 8;

    private:
        struct LineVertex
        {
            glm::vec3 Position;
            glm::vec4 Color;
        };
        struct SpriteVertex
        {
            glm::vec3 Position;
            glm::vec4 Color;
            glm::vec2 UV;
            uint32_t Texture;
            uint32_t Entity;
        };
        struct SpriteDraw
        {
            glm::mat4 Transform;
            glm::vec4 Color;
            glm::vec2 UVMin, UVMax;
            uint32_t Texture;
            uint32_t Entity;
            int32_t Order;
            float Depth;
            bool Additive = false;
            // Explicit world-space corners (ribbons); Transform/UVMin/UVMax are ignored when set.
            bool HasCorners = false;
            glm::vec3 P[4];
            glm::vec4 C[4];
            glm::vec2 UV[4];
        };

        // Mesh renderers resolved once per frame (mesh, world bounds, uploaded materials, skinning) and then only
        // culled and batched by each pass (main, depth prepass, every shadow map).
        struct RenderPart
        {
            uint32_t FirstIndex, IndexCount;
            VkDeviceAddress Material;
            bool Transparent, DoubleSided, CastShadows; // CastShadows: the material's flag
            uint32_t Item;  // owning RenderItem
            uint32_t Batch; // parts with the same batch share mesh, range, material and culling: one instanced draw
        };
        struct RenderItem
        {
            Ref<Mesh> MeshRef;
            glm::mat4 World;
            glm::vec3 Center, Extents; // world-space bounds
            uint32_t EntityId;
            uint32_t Probe = 0;        // reflection probe slot + 1
            VkDeviceAddress Skin = 0, Joints = 0;
            bool AlwaysVisible;        // skinned: animated bounds are unknown
            bool CastShadows;          // the renderer's flag
            uint32_t FirstPart, PartCount;
        };
        std::vector<RenderItem> m_RenderItems;
        std::vector<RenderPart> m_RenderParts;
        std::vector<uint8_t> m_BatchInstanceable;       // per batch: false for skinned parts (one draw each)
        std::vector<uint32_t> m_BatchStart, m_BatchFill; // per-pass scratch for the counting sort
        std::vector<uint32_t> m_VisibleParts, m_SortedParts;
        void BuildRenderList(Scene& scene);

        // Reflection probes: GPU images per probe entity, captured on demand and listed for this frame's uniforms.
        struct ProbeResources
        {
            uint32_t Resolution = 0;
            Image Faces[6];
            uint32_t FaceBindless[6] = {};
            Image Base;     // equirectangular conversion target (mip 0)
            Image Equirect; // with mips, sampled by meshes
            uint32_t Bindless = 0;
            uint64_t Key = 0, Generation = ~0ull;
            bool Baked = false;
            ~ProbeResources();
        };
        struct FrameProbe
        {
            glm::vec3 Center, Min, Max;
            float Intensity;
            uint32_t Texture, Mips;
            bool BoxProjection;
        };
        std::unordered_map<uint64_t, std::unique_ptr<ProbeResources>> m_Probes;
        std::vector<FrameProbe> m_FrameProbes;
        Scope<SceneRenderTarget> m_ProbeTarget;
        bool m_Capturing = false;
        void UpdateReflectionProbes(VkCommandBuffer cmd, Scene& scene);
        void BakeProbe(VkCommandBuffer cmd, Scene& scene, ProbeResources& gpu, const glm::vec3& position, uint32_t resolution);

        enum class MeshPass { Main, Shadow, Depth };
        void DrawMeshes(VkCommandBuffer cmd, const glm::mat4& viewProj, MeshPass pass);
        // Depth prepass, half-resolution SSAO and its blur (leaves the depth attachment ready to load).
        void RenderDepthOfField(VkCommandBuffer cmd, SceneRenderTarget& target, const CameraData& camera, const SceneSettings& settings);
        void RenderAmbientOcclusion(VkCommandBuffer cmd, Scene& scene, SceneRenderTarget& target, const glm::mat4& viewProj,
                                    const SceneSettings& settings);
        // Renders the directional shadow map; returns the light view-projection.
        void RenderShadowMap(VkCommandBuffer cmd, Scene& scene, const CameraData& camera, const glm::vec3& lightDir,
                             float distance, int cascadeCount, const void* uniforms, size_t uniformSize, ShadowCascades& out);
        void CollectSprites(Scene& scene, const CameraData& camera);
        void CollectEditorIcons(Scene& scene, const CameraData& camera);
        void CollectText(Scene& scene, const CameraData& camera);
        void CollectTilemaps(Scene& scene, const CameraData& camera);
        void CollectParticles(Scene& scene, const CameraData& camera);
        void CollectLines(Scene& scene, const CameraData& camera);
        void DrawSprites(VkCommandBuffer cmd);
        void BuildGrid(const CameraData& camera, bool grid2D, std::vector<LineVertex>& out);
        void BuildEditorOverlays(Scene& scene, const SceneRenderOptions& options);
        void BuildEntityOverlay(Scene& scene, Entity e);
        void FlushLines(VkCommandBuffer cmd, std::vector<LineVertex>& lines, const Pipeline& pipeline);
        void DrawUI(VkCommandBuffer cmd, Scene& scene, VkExtent2D extent);
        void RenderBloom(VkCommandBuffer cmd, SceneRenderTarget& target, const SceneSettings& settings);
        // Renders the cube faces of each shadowed point light into the point shadow atlas.
        // faceCounts[i] is 6 for point lights and 1 for spot lights.
        void RenderPointShadows(VkCommandBuffer cmd, Scene& scene, const glm::mat4* faceViewProj, const uint32_t* faceCounts,
                                uint32_t lightCount, const void* uniforms, size_t uniformSize);

        static constexpr uint32_t kShadowTileSize = 2048;               // one cascade
        static constexpr uint32_t kShadowMapSize = kShadowTileSize * 2; // 2x2 atlas
        Scope<Image> m_ShadowMap;
        uint32_t m_ShadowBindless = 0;

        // Point-light shadows: 6 faces (columns) x kMaxPointShadows lights (rows).
    public:
        static constexpr uint32_t kMaxPointShadows = 4;
        static constexpr uint32_t kPointShadowTile = 512;
    private:
        Scope<Image> m_PointShadowMap;
        uint32_t m_PointShadowBindless = 0;

        std::vector<SpriteDraw> m_Sprites;
        std::vector<LineVertex> m_Lines;     // depth-tested overlay lines
        std::vector<LineVertex> m_GridLines;
        RenderStats m_Stats;
    };
}
