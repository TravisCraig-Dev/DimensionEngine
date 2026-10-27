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
        // Scene view search: while Highlighting, meshes of objects not in Highlight are drawn dimmed.
        bool Highlighting = false;
        std::vector<Entity> Highlight;
    };

    // Directional shadow cascades for one frame.
    struct ShadowCascades
    {
        static constexpr int kMax = 4;
        int Count = 0;
        glm::mat4 ViewProj[kMax]{};
        float Splits[kMax]{}; // far view depth of each cascade
        float Texel[kMax]{};  // world size of one shadow texel
        float DepthRange[kMax]{}; // world depth spanned by the cascade's [0,1] depth
    };

    // Cascade far distances between nearDepth and farDepth (view depth), blending logarithmic and
    // uniform splits by lambda (0 = uniform, 1 = logarithmic). The last split is always farDepth.
    std::array<float, ShadowCascades::kMax> ComputeCascadeSplits(float nearDepth, float farDepth, int count, float lambda);

    // View-projection of cube face `face` (+X, -X, +Y, -Y, +Z, -Z) for a point light shadow (90 degree FOV).
    glm::mat4 PointShadowFaceViewProj(const glm::vec3& position, int face, float nearPlane, float farPlane);
    // Spot light cookies: the basis the shader projects with (right/up perpendicular to the direction), and the cookie
    // texture coordinate of a point (toPoint = point - light position; `roll` turns the cookie around the axis).
    // Mirrors SpotCookieUV in Common.glsl. Points on the outer cone's rim land on the texture's inscribed circle.
    void SpotCookieBasis(const glm::vec3& direction, glm::vec3& right, glm::vec3& up);
    glm::vec2 SpotCookieUV(const glm::vec3& direction, float cosOuter, float roll, const glm::vec3& toPoint);
    // Point light cookies are equirectangular images around the light, in its local space (u = 0.5 at its forward,
    // -Z; v = 0 at its up), so they turn with it. Mirrors PointCookieUV in Common.glsl (which gets the rotation as
    // a quaternion).
    glm::vec2 PointCookieUV(const glm::mat3& lightRotation, const glm::vec3& toPoint);
    // View-projection for a spot light shadow covering its outer cone.
    glm::mat4 SpotShadowViewProj(const glm::vec3& position, const glm::vec3& direction, float spotAngleDegrees, float nearPlane, float farPlane);

    // Clustered lighting: the view is split into a grid of tiles (in normalized device coordinates) times depth slices
    // (logarithmic, linear for orthographic views); every cluster lists the point and spot lights whose range sphere
    // reaches it, so a pixel only shades the lights near it.
    struct LightClusters
    {
        static constexpr int kTilesX = 16, kTilesY = 9, kSlices = 24, kCount = kTilesX * kTilesY * kSlices;
        static constexpr uint32_t kMaxLights = 1024, kMaxPerCluster = 256;
        std::vector<glm::uvec2> Cells; // per cluster: offset into Indices, light count
        std::vector<uint32_t> Indices;
        float Near = 0.1f, Far = 100.0f;
        bool Orthographic = false;

        static int CellIndex(int x, int y, int slice) { return (slice * kTilesY + y) * kTilesX + x; }
        int SliceOf(float viewDepth) const;
        // Near, far and orthographic from the projection (Build calls it; GPU culling needs only this).
        void SetPlanes(const glm::mat4& projection);
        // Lights as (world position, range) spheres; near/far come from the projection. Without culling every cluster
        // lists every light (for comparisons).
        void Build(const std::vector<glm::vec4>& spheres, const glm::mat4& view, const glm::mat4& projection, bool cull = true);
    };

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
        uint32_t Lights = 0;          // point and spot lights in view's clusters
        uint32_t ClusterLightRefs = 0; // light entries over all clusters
        uint32_t GpuParticleCapacity = 0; // particle slots drawn by GPU particle systems
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
        // Slider joint: its axis through the anchor, the travel range between the limits with end ticks.
        void DrawSliderGizmo(const glm::vec3& anchor, glm::vec3 axis, bool limits, float min, float max);
        void DrawWireSphere(const glm::mat4& transform, const glm::vec3& center, float radius, const glm::vec4& color);

        const RenderStats& GetStats() const { return m_Stats; }

        // GPU instancing: identical opaque meshes (same mesh, sub-mesh, material and pipeline) are drawn with one
        // instanced call. On by default; turning it off is useful for comparisons.
        static void SetInstancingEnabled(bool enabled);
        static void SetLightClusteringEnabled(bool enabled); // false: every light in every cluster (comparisons)
        // Light culling runs in a compute shader by default; false builds the clusters on the CPU (comparisons).
        static void SetGpuLightCullingEnabled(bool enabled);
        static bool IsGpuLightCullingEnabled();
        static bool IsInstancingEnabled();
        // Re-captures every reflection probe on the next frame (all renderers).
        static void RequestProbeBake();
        static constexpr uint32_t kMaxReflectionProbes = 8;
        static constexpr uint32_t kMaxProbeVolumes = 4; // light probe volumes shading a view (MAX_PROBE_VOLUMES in Common.glsl)
        static constexpr uint32_t kMaxGrassBenders = 16; // bodies bending grass per view (MAX_GRASS_BENDERS in Common.glsl)

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
        bool m_FrameRefracts = false; // a transparent material this frame refracts
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
            VkBuffer Vertices = VK_NULL_HANDLE; // this frame's vertices instead of the mesh's (simulated cloth)
            VkDeviceSize VertexOffset = 0;
            VkBuffer Indices = VK_NULL_HANDLE;  // and triangles instead of the mesh's (torn cloth)
            VkDeviceSize IndexOffset = 0;
            uint32_t IndexCount = 0;
            bool AlwaysVisible;        // skinned: animated bounds are unknown
            bool CastShadows;          // the renderer's flag
            uint32_t Motion = 0;       // grass tufts: half floats (sway, bending), 0 = rigid
            uint32_t Fade = 0;         // LOD cross-fade: coverage (1-254) | side << 8, 0 = none (see LODGroups::Frame)
            uint32_t FirstPart, PartCount;
        };
        std::vector<RenderItem> m_RenderItems;
        std::vector<RenderPart> m_RenderParts;
        std::vector<uint8_t> m_BatchInstanceable;       // per batch: false for skinned parts (one draw each)
        std::vector<uint32_t> m_BatchStart, m_BatchFill; // per-pass scratch for the counting sort
        LightClusters m_Clusters; // this view's light clusters (rebuilt every Render)
        // GPU light culling output: per cluster (offset, count) and a fixed-capacity light list per cluster.
        Buffer m_GpuClusterCells, m_GpuClusterIndices;
        VkDeviceAddress m_GpuClusterCellsAddress = 0, m_GpuClusterIndicesAddress = 0;
        bool CullLightsOnGpu(VkCommandBuffer cmd, const void* uniforms, size_t size);
        std::vector<uint32_t> m_VisibleParts, m_SortedParts;
        void BuildRenderList(Scene& scene, const CameraData& camera, const SceneRenderOptions& options);

        // Reflection probes: GPU images per probe entity, captured on demand and listed for this frame's uniforms.
        struct ProbeResources
        {
            uint32_t Resolution = 0;
            Image Faces[6];
            uint32_t FaceBindless[6] = {};
            Image Base;     // equirectangular conversion target (mip 0)
            Image Equirect; // box-filtered mip chain: the prefilter's source
            Image Filtered; // GGX-prefiltered per mip (mip m = roughness m / (mips - 1)), sampled by meshes
            uint32_t SourceBindless = 0;
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
            float BlendDistance;
        };
        std::unordered_map<uint64_t, std::unique_ptr<ProbeResources>> m_Probes;
        std::vector<FrameProbe> m_FrameProbes;
        Scope<SceneRenderTarget> m_ProbeTarget;
        bool m_Capturing = false;
        bool m_Oit = false; // this frame draws transparent meshes order-independently
        uint32_t m_Samples = 1; // MSAA samples of the view being rendered
        // A main-pass pipeline (MainPipelines::Kind) for m_Samples.
        const Pipeline& MainPipeline(int kind) const;
        static void MultisampleInto(VkRenderingAttachmentInfo& att, const Image& multisampled, const Image& resolved, VkResolveModeFlagBits mode);
        void UpdateReflectionProbes(VkCommandBuffer cmd, Scene& scene);
        // Light probe volumes: one probe captured per frame while baking, read back a few frames later.
        void UpdateLightProbes(VkCommandBuffer cmd, Scene& scene);
        struct Readback
        {
            Buffer Data;
            bool Busy = false;
            uint64_t Frame = 0;
            const Scene* Owner = nullptr;
            uint32_t EntityIndex = 0, EntityGeneration = 0;
            int Probe = 0;
            uint32_t Width = 0, Height = 0;
        };
        std::vector<Readback> m_LightProbeReadbacks;
        std::unique_ptr<ProbeResources> m_LightProbeCapture;
        void BakeProbe(VkCommandBuffer cmd, Scene& scene, ProbeResources& gpu, const glm::vec3& position, uint32_t resolution);

        // GGX-prefiltered copy of the HDR sky texture for reflections (mip m = roughness m / (mips - 1)), rebuilt
        // when the sky texture changes.
        struct FilteredSky
        {
            std::weak_ptr<Texture2D> Source;
            Image Filtered;
            uint32_t Bindless = 0;
            ~FilteredSky();
        };
        FilteredSky m_FilteredSky;
        const FilteredSky* FilterSky(VkCommandBuffer cmd, const Ref<Texture2D>& sky);

        enum class MeshPass { Main, Shadow, Depth, Transparent }; // Transparent: the order-independent transparency pass
        void DrawMeshes(VkCommandBuffer cmd, const glm::mat4& viewProj, MeshPass pass);
        // Depth prepass, half-resolution SSAO and its blur (leaves the depth attachment ready to load).
        void RenderDepthOfField(VkCommandBuffer cmd, SceneRenderTarget& target, const CameraData& camera, const SceneSettings& settings);
        void RenderReflections(VkCommandBuffer cmd, SceneRenderTarget& target, const SceneSettings& settings);
        // A full-screen triangle into `dst` (from and back to shader-read layout).
        void FullscreenPass(VkCommandBuffer cmd, const Image& dst, bool load, const Pipeline& pipeline, const void* push, uint32_t pushSize);
        struct DecalDraw
        {
            glm::mat4 Model;
            glm::vec4 Color;
            uint32_t Texture, Blend;
            float AngleFade;
            int32_t Order;
        };
        std::vector<DecalDraw> m_Decals;
        void CollectDecals(Scene& scene);
        void DrawDecals(VkCommandBuffer cmd, SceneRenderTarget& target);
        // GPU particle systems: compute simulation before the main pass, then camera-facing quads in it.
        struct GpuParticleDraw
        {
            VkDeviceAddress Particles, Params;
            uint32_t Capacity, Texture, Entity;
            bool Additive;
            int32_t Order;
            VkDeviceAddress Sorted = 0; // back-to-front slot order, 0 = unsorted
            bool Trails = false;        // also draw the slots' trail ribbons
        };
        std::vector<GpuParticleDraw> m_GpuParticleDraws;
        void SimulateGpuParticles(VkCommandBuffer cmd, Scene& scene, SceneRenderTarget& target, const glm::mat4& view);
        // GPU particle world collision reads the depth this view stored last frame (with its view-projection).
        bool m_ParticleDepthWanted = false, m_ParticleDepthKept = false;
        glm::mat4 m_ParticleDepthViewProj{ 1.0f };
        glm::uvec2 m_ParticleDepthSize{ 0 };
        void DrawGpuParticles(VkCommandBuffer cmd);
        void RenderIndirect(VkCommandBuffer cmd, SceneRenderTarget& target, const SceneSettings& settings, const glm::mat4& viewProj); // screen-space GI
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
