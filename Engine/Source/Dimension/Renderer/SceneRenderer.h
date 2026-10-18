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
        };

        void DrawMeshes(VkCommandBuffer cmd, Scene& scene, const glm::mat4& viewProj, bool shadowPass);
        // Renders the directional shadow map; returns the light view-projection.
        void RenderShadowMap(VkCommandBuffer cmd, Scene& scene, const CameraData& camera, const glm::vec3& lightDir,
                             float distance, int cascadeCount, const void* uniforms, size_t uniformSize, ShadowCascades& out);
        void CollectSprites(Scene& scene, const CameraData& camera);
        void CollectEditorIcons(Scene& scene, const CameraData& camera);
        void CollectText(Scene& scene, const CameraData& camera);
        void CollectTilemaps(Scene& scene, const CameraData& camera);
        void CollectParticles(Scene& scene, const CameraData& camera);
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
