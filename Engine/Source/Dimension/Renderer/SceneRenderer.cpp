#include "Dimension/Renderer/SceneRenderer.h"
#include "Dimension/Renderer/Texture.h"
#include "Dimension/Renderer/Material.h"
#include "Dimension/Asset/AssetManager.h"
#include "Dimension/Scene/Scene.h"
#include "Dimension/Core/Log.h"
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cstdint>
#include <cstring>

#include "shaders/Mesh.vert.h"
#include "shaders/Mesh.frag.h"
#include "shaders/Sprite.vert.h"
#include "shaders/Sprite.frag.h"
#include "shaders/Line.vert.h"
#include "shaders/Line.frag.h"
#include "shaders/Overlay.frag.h"
#include "shaders/Shadow.frag.h"
#include "shaders/Sky.vert.h"
#include "shaders/Sky.frag.h"
#include "shaders/UI.vert.h"
#include "shaders/UI.frag.h"
#include "Dimension/UI/UI.h"
#include "Dimension/Renderer/Font.h"
#include "Dimension/Particles/ParticleSystem.h"
#include "shaders/Composite.vert.h"
#include "shaders/Composite.frag.h"
#include "shaders/BloomDown.frag.h"
#include "shaders/BloomUp.frag.h"
#include "shaders/Fxaa.frag.h"

namespace Dimension
{
    namespace
    {
        constexpr uint32_t kMaxPointLights = 32;
        constexpr uint32_t kMaxQuadsPerDraw = 65536;

        struct PointLightGPU
        {
            glm::vec4 PositionRange;
            glm::vec4 Color;     // w: shadow slot + 1
            glm::vec4 Direction; // spot: xyz direction, w cos(outer half angle); point: w = -2
            glm::vec4 Cone;      // x: cos(inner half angle)
        };

        // std140 mirror of SceneData in Common.glsl.
        struct SceneUniforms
        {
            glm::mat4 ViewProj;
            glm::mat4 View;
            glm::mat4 Proj;
            glm::vec4 CameraPosition;
            glm::vec4 LightDirection;
            glm::vec4 LightColor;
            glm::vec4 Ambient;
            glm::ivec4 Counts;
            glm::vec4 GridParams;
            glm::mat4 InvViewProj;
            glm::mat4 LightViewProj[ShadowCascades::kMax];
            glm::vec4 ShadowParams;  // x: cascade count, y: atlas index, z: bias
            glm::vec4 CascadeSplits;
            glm::vec4 CascadeTexel;
            glm::vec4 CascadeParams; // y: blend fraction, z: debug colors
            glm::vec4 SkyTop, SkyHorizon, SkyGround;
            glm::vec4 SkyParams;  // x mode (1 = texture), y texture index, z intensity, w rotation (radians)
            glm::vec4 EnvParams;  // x reflection intensity, y max mip, z 1 = irradiance SH valid, w ambient intensity
            glm::vec4 IrradianceSH[9];
            glm::mat4 PointShadowViewProj[SceneRenderer::kMaxPointShadows * 6];
            glm::vec4 PointShadowParams[SceneRenderer::kMaxPointShadows]; // near, far, bias, atlas index
            PointLightGPU PointLights[kMaxPointLights];
        };
        static_assert(sizeof(SceneUniforms) <= RenderDevice::kUniformBlockSize);

        struct MeshPush
        {
            glm::mat4 Model;
            VkDeviceAddress Material;
            VkDeviceAddress Skin;
            VkDeviceAddress Joints;
            uint32_t EntityId;
            uint32_t Flags; // bit 0: skinned
        };

        // std430 mirror of MaterialRef in MeshCommon.glsl.
        struct MaterialGPU
        {
            glm::vec4 BaseColor;
            glm::vec4 Emission;
            glm::vec4 Params;
            glm::vec4 TilingOffset;
            glm::uvec4 Maps;
            glm::uvec4 Flags;
        };
        static_assert(sizeof(MeshPush) <= kPushConstantSize);

        struct CompositePush
        {
            uint32_t HdrTexture;
            float Exposure;
            uint32_t Tonemap;
            uint32_t BloomTexture; // index + 1, 0 = none
            float BloomIntensity;
            float Vignette;
        };

        struct BloomPush
        {
            uint32_t Src;
            uint32_t Prefilter;
            float Threshold;
            float Knee;
            glm::vec2 SrcTexel;
            float Radius;
        };

        struct FxaaPush
        {
            uint32_t Src;
            uint32_t Pad;
            glm::vec2 Texel;
        };

        // Frustum planes (Gribb/Hartmann) for a GL-style clip space with depth in [0, 1].
        struct Frustum
        {
            glm::vec4 Planes[6];
            explicit Frustum(const glm::mat4& m)
            {
                glm::mat4 t = glm::transpose(m);
                Planes[0] = t[3] + t[0];
                Planes[1] = t[3] - t[0];
                Planes[2] = t[3] + t[1];
                Planes[3] = t[3] - t[1];
                Planes[4] = t[2];
                Planes[5] = t[3] - t[2];
                for (auto& p : Planes) p /= glm::length(glm::vec3(p));
            }
            // Conservative test of a local-space AABB under a world transform.
            bool Intersects(const glm::mat4& world, const AABB& box) const
            {
                glm::vec3 c = glm::vec3(world * glm::vec4(box.Center(), 1.0f));
                glm::vec3 e = box.Extents();
                glm::mat3 m(world);
                glm::vec3 we = glm::abs(m[0]) * e.x + glm::abs(m[1]) * e.y + glm::abs(m[2]) * e.z;
                for (auto& p : Planes)
                    if (glm::dot(glm::vec3(p), c) + p.w + glm::dot(glm::abs(glm::vec3(p)), we) < 0.0f) return false;
                return true;
            }
        };

        float SrgbToLinear(float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }
        glm::vec3 ToLinear(const glm::vec3& c) { return { SrgbToLinear(c.r), SrgbToLinear(c.g), SrgbToLinear(c.b) }; }
        glm::vec4 ToLinear(const glm::vec4& c) { return glm::vec4(ToLinear(glm::vec3(c)), c.a); }

        // --- Procedural editor icons (64x64, white on transparent; tinted when drawn)
        enum IconId { IconCamera, IconSun, IconBulb, IconSpeaker, IconCount };

        float Coverage(float sdf) { return std::clamp(0.5f - sdf, 0.0f, 1.0f); } // 1px AA

        float SdBox(glm::vec2 p, glm::vec2 c, glm::vec2 h)
        {
            glm::vec2 d = glm::abs(p - c) - h;
            return glm::length(glm::max(d, glm::vec2(0.0f))) + std::min(std::max(d.x, d.y), 0.0f);
        }
        float SdSegment(glm::vec2 p, glm::vec2 a, glm::vec2 b, float r)
        {
            glm::vec2 pa = p - a, ba = b - a;
            float h = std::clamp(glm::dot(pa, ba) / glm::dot(ba, ba), 0.0f, 1.0f);
            return glm::length(pa - ba * h) - r;
        }

        std::vector<uint32_t> MakeIcon(IconId id)
        {
            std::vector<uint32_t> pixels(64 * 64);
            for (int y = 0; y < 64; ++y)
                for (int x = 0; x < 64; ++x)
                {
                    glm::vec2 p(x + 0.5f, y + 0.5f);
                    float d = 1e9f;
                    switch (id)
                    {
                    case IconCamera:
                        d = std::min(SdBox(p, { 26, 32 }, { 18, 13 }) - 3.0f,
                                     std::max(SdBox(p, { 52, 32 }, { 8, 12 }), -(p.x - 46.0f) * 0.7f + std::abs(p.y - 32) - 13.0f));
                        break;
                    case IconSun:
                    {
                        d = glm::length(p - glm::vec2(32)) - 12.0f;
                        for (int i = 0; i < 8; ++i)
                        {
                            float a = i * 0.785398f;
                            glm::vec2 dir(std::cos(a), std::sin(a));
                            d = std::min(d, SdSegment(p, glm::vec2(32) + dir * 19.0f, glm::vec2(32) + dir * 28.0f, 2.5f));
                        }
                        break;
                    }
                    case IconBulb:
                        d = std::min(glm::length(p - glm::vec2(32, 26)) - 16.0f, SdBox(p, { 32, 49 }, { 7, 7 }) - 1.0f);
                        break;
                    case IconSpeaker:
                        d = std::min(SdBox(p, { 18, 32 }, { 7, 9 }),
                                     std::max(SdBox(p, { 34, 32 }, { 10, 22 }), std::abs(p.y - 32) - (p.x - 22.0f) * 1.1f - 9.0f));
                        d = std::min(d, std::abs(glm::length(p - glm::vec2(40, 32)) - 18.0f) - 2.0f + std::max(0.0f, 46.0f - p.x));
                        break;
                    default: break;
                    }
                    // Dark outline so icons stay readable on light backgrounds.
                    float fill = Coverage(d);
                    float outline = Coverage(d - 2.0f);
                    float lum = fill;
                    float alpha = outline;
                    uint8_t c = (uint8_t)(std::clamp(lum, 0.0f, 1.0f) * 255);
                    uint8_t a = (uint8_t)(std::clamp(alpha, 0.0f, 1.0f) * 230);
                    pixels[y * 64 + x] = c | (c << 8) | (c << 16) | ((uint32_t)a << 24);
                }
            return pixels;
        }
    }

    struct Renderer::Resources
    {
        Pipeline Mesh, MeshDoubleSided, MeshTransparent, Shadow, Sky, UI, SpriteAdditive, Sprite, Line, LineOverlay, Composite;
        Pipeline BloomDown, BloomUp, Fxaa;
        Buffer QuadIndices;
        Ref<Texture2D> Icons[IconCount];
    };

    static Renderer::Resources* s_Resources = nullptr;

    void Renderer::Init()
    {
        s_Resources = new Resources();
        auto& r = *s_Resources;
        using F = SceneRenderTarget;

        GraphicsPipelineDesc mesh;
        mesh.VertexSpirv = kMesh_vert;
        mesh.FragmentSpirv = kMesh_frag;
        mesh.VertexStride = sizeof(Vertex);
        mesh.Attributes = { { 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, Position) },
                            { 1, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, Normal) },
                            { 2, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, UV) } };
        mesh.CullMode = VK_CULL_MODE_BACK_BIT;
        mesh.DepthTest = mesh.DepthWrite = true;
        mesh.ColorAttachments = { { F::kHdrFormat }, { F::kIdFormat } };
        mesh.DepthFormat = F::kDepthFormat;
        mesh.DebugName = "MeshPipeline";
        r.Mesh = Pipeline(mesh);
        GraphicsPipelineDesc meshTwoSided = mesh;
        meshTwoSided.CullMode = VK_CULL_MODE_NONE;
        meshTwoSided.DebugName = "MeshDoubleSidedPipeline";
        r.MeshDoubleSided = Pipeline(meshTwoSided);
        GraphicsPipelineDesc meshTransparent = meshTwoSided;
        meshTransparent.DepthWrite = false;
        meshTransparent.ColorAttachments = { { F::kHdrFormat, BlendMode::Alpha }, { F::kIdFormat } };
        meshTransparent.DebugName = "MeshTransparentPipeline";
        r.MeshTransparent = Pipeline(meshTransparent);

        GraphicsPipelineDesc shadow = mesh;
        shadow.FragmentSpirv = kShadow_frag;
        shadow.CullMode = VK_CULL_MODE_NONE; // single-sided geometry (planes, quads) must still cast
        shadow.ColorAttachments = {};
        shadow.DepthBias = true;
        shadow.DebugName = "ShadowPipeline";
        r.Shadow = Pipeline(shadow);

        GraphicsPipelineDesc sky;
        sky.VertexSpirv = kSky_vert;
        sky.FragmentSpirv = kSky_frag;
        sky.ColorAttachments = { { F::kHdrFormat }, { F::kIdFormat, BlendMode::Opaque, false } };
        sky.DepthFormat = F::kDepthFormat;
        sky.DebugName = "SkyPipeline";
        r.Sky = Pipeline(sky);

        struct SV { glm::vec3 P; glm::vec4 C; glm::vec2 UV; uint32_t T; uint32_t E; };
        GraphicsPipelineDesc sprite;
        sprite.VertexSpirv = kSprite_vert;
        sprite.FragmentSpirv = kSprite_frag;
        sprite.VertexStride = sizeof(SV);
        sprite.Attributes = { { 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(SV, P) },
                              { 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(SV, C) },
                              { 2, VK_FORMAT_R32G32_SFLOAT, offsetof(SV, UV) },
                              { 3, VK_FORMAT_R32_UINT, offsetof(SV, T) },
                              { 4, VK_FORMAT_R32_UINT, offsetof(SV, E) } };
        sprite.DepthTest = true;
        sprite.DepthWrite = false;
        sprite.ColorAttachments = { { F::kHdrFormat, BlendMode::Alpha }, { F::kIdFormat } };
        sprite.DepthFormat = F::kDepthFormat;
        sprite.DebugName = "SpritePipeline";
        r.Sprite = Pipeline(sprite);
        GraphicsPipelineDesc additive = sprite;
        additive.ColorAttachments = { { F::kHdrFormat, BlendMode::Additive }, { F::kIdFormat } };
        additive.DebugName = "SpriteAdditivePipeline";
        r.SpriteAdditive = Pipeline(additive);

        GraphicsPipelineDesc ui = sprite;
        ui.VertexSpirv = kUI_vert;
        ui.FragmentSpirv = kUI_frag;
        ui.DepthTest = false;
        ui.ColorAttachments = { { F::kOutputFormat, BlendMode::Alpha } };
        ui.DepthFormat = VK_FORMAT_UNDEFINED;
        ui.DebugName = "UIPipeline";
        r.UI = Pipeline(ui);

        struct LV { glm::vec3 P; glm::vec4 C; };
        GraphicsPipelineDesc line;
        line.VertexSpirv = kLine_vert;
        line.FragmentSpirv = kLine_frag;
        line.VertexStride = sizeof(LV);
        line.Attributes = { { 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(LV, P) }, { 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(LV, C) } };
        line.Topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
        line.DepthTest = true;
        line.ColorAttachments = { { F::kHdrFormat, BlendMode::Alpha }, { F::kIdFormat, BlendMode::Opaque, false } };
        line.DepthFormat = F::kDepthFormat;
        line.DebugName = "LinePipeline";
        r.Line = Pipeline(line);
        GraphicsPipelineDesc overlay = line;
        overlay.FragmentSpirv = kOverlay_frag;
        overlay.DepthTest = false;
        overlay.ColorAttachments = { { F::kOutputFormat, BlendMode::Alpha } };
        overlay.DepthFormat = VK_FORMAT_UNDEFINED;
        overlay.DebugName = "LineOverlayPipeline";
        r.LineOverlay = Pipeline(overlay);

        GraphicsPipelineDesc composite;
        composite.VertexSpirv = kComposite_vert;
        composite.FragmentSpirv = kComposite_frag;
        composite.ColorAttachments = { { F::kOutputFormat } };
        composite.DebugName = "CompositePipeline";
        r.Composite = Pipeline(composite);

        GraphicsPipelineDesc post = composite;
        post.FragmentSpirv = kBloomDown_frag;
        post.ColorAttachments = { { F::kHdrFormat } };
        post.DebugName = "BloomDownPipeline";
        r.BloomDown = Pipeline(post);
        post.FragmentSpirv = kBloomUp_frag;
        post.ColorAttachments = { { F::kHdrFormat, BlendMode::Additive } };
        post.DebugName = "BloomUpPipeline";
        r.BloomUp = Pipeline(post);
        post.FragmentSpirv = kFxaa_frag;
        post.ColorAttachments = { { F::kOutputFormat } };
        post.DebugName = "FxaaPipeline";
        r.Fxaa = Pipeline(post);

        std::vector<uint32_t> indices(kMaxQuadsPerDraw * 6);
        for (uint32_t q = 0; q < kMaxQuadsPerDraw; ++q)
        {
            uint32_t v = q * 4;
            uint32_t* i = &indices[q * 6];
            i[0] = v; i[1] = v + 1; i[2] = v + 2; i[3] = v; i[4] = v + 2; i[5] = v + 3;
        }
        r.QuadIndices = Buffer::CreateWithData(indices.data(), indices.size() * sizeof(uint32_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT, "QuadIndices");

        TextureSettings iconSettings;
        iconSettings.SRGB = false;
        for (int i = 0; i < IconCount; ++i)
        {
            auto pixels = MakeIcon((IconId)i);
            r.Icons[i] = MakeRef<Texture2D>(pixels.data(), 64, 64, iconSettings, "EditorIcon");
        }
    }

    void Renderer::Shutdown()
    {
        delete s_Resources;
        s_Resources = nullptr;
    }

    Renderer::Resources& Renderer::Get() { return *s_Resources; }

    // ---- Camera ----------------------------------------------------------------------

    CameraData CameraData::FromEntity(Scene& scene, Entity camera, float aspect)
    {
        CameraData data;
        auto& reg = scene.GetRegistry();
        auto& cam = reg.Get<CameraComponent>(camera);
        auto& t = reg.Get<TransformComponent>(camera);
        glm::mat4 m = t.World;
        for (int i = 0; i < 3; ++i) m[i] = glm::vec4(glm::normalize(glm::vec3(m[i])), 0.0f); // strip scale
        data.View = glm::inverse(m);
        data.Projection = cam.GetProjection(aspect);
        data.Position = t.GetWorldPosition();
        data.ClearColor = cam.ClearColor;
        data.Orthographic = cam.Projection == ProjectionType::Orthographic;
        data.Skybox = cam.Clear == ClearMode::Skybox;
        return data;
    }

    // ---- Debug lines ------------------------------------------------------------------

    void SceneRenderer::DrawLine(const glm::vec3& a, const glm::vec3& b, const glm::vec4& color)
    {
        m_Lines.push_back({ a, color });
        m_Lines.push_back({ b, color });
    }

    void SceneRenderer::DrawWireBox(const glm::mat4& m, const glm::vec3& c, const glm::vec3& h, const glm::vec4& color)
    {
        glm::vec3 corners[8];
        for (int i = 0; i < 8; ++i)
        {
            glm::vec3 s((i & 1) ? 1.0f : -1.0f, (i & 2) ? 1.0f : -1.0f, (i & 4) ? 1.0f : -1.0f);
            corners[i] = glm::vec3(m * glm::vec4(c + s * h, 1.0f));
        }
        const int edges[12][2] = { { 0, 1 }, { 2, 3 }, { 4, 5 }, { 6, 7 }, { 0, 2 }, { 1, 3 }, { 4, 6 }, { 5, 7 }, { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 } };
        for (auto& e : edges) DrawLine(corners[e[0]], corners[e[1]], color);
    }

    void SceneRenderer::DrawWireCircle(const glm::mat4& m, const glm::vec3& c, float r, const glm::vec4& color, int axis)
    {
        constexpr int kSegments = 48;
        glm::vec3 prev;
        for (int i = 0; i <= kSegments; ++i)
        {
            float a = glm::two_pi<float>() * i / kSegments;
            glm::vec3 p = axis == 2 ? glm::vec3(std::cos(a), std::sin(a), 0) : axis == 1 ? glm::vec3(std::cos(a), 0, std::sin(a)) : glm::vec3(0, std::cos(a), std::sin(a));
            glm::vec3 w = glm::vec3(m * glm::vec4(c + p * r, 1.0f));
            if (i > 0) DrawLine(prev, w, color);
            prev = w;
        }
    }

    void SceneRenderer::DrawWireSphere(const glm::mat4& m, const glm::vec3& c, float r, const glm::vec4& color)
    {
        for (int axis = 0; axis < 3; ++axis) DrawWireCircle(m, c, r, color, axis);
    }

    // ---- Rendering ------------------------------------------------------------------------

    void SceneRenderer::Render(VkCommandBuffer cmd, Scene& scene, const CameraData& camera, SceneRenderTarget& target,
                               const SceneRenderOptions& options)
    {
        auto& device = RenderDevice::Get();
        auto& res = Renderer::Get();
        m_Stats = {};
        auto& settings = scene.GetSettings();
        auto& reg = scene.GetRegistry();

        // Lights
        SceneUniforms u{};
        u.View = camera.View;
        u.Proj = camera.Projection;
        u.ViewProj = camera.Projection * camera.View;
        u.CameraPosition = glm::vec4(camera.Position, 1.0f);
        u.Ambient = glm::vec4(ToLinear(settings.AmbientColor) * settings.AmbientIntensity, settings.AmbientIntensity);
        u.InvViewProj = glm::inverse(u.ViewProj);
        u.SkyTop = glm::vec4(ToLinear(settings.SkyTop), 1.0f);
        u.SkyHorizon = glm::vec4(ToLinear(settings.SkyHorizon), 1.0f);
        u.SkyGround = glm::vec4(ToLinear(settings.SkyGround), 1.0f);
        if (settings.SkyMode == 1)
            if (auto sky = AssetManager::GetTexture(settings.SkyTexture))
            {
                u.SkyParams = glm::vec4(1.0f, (float)sky->GetBindlessIndex(), settings.SkyIntensity, glm::radians(settings.SkyRotation));
                u.EnvParams = glm::vec4(settings.ReflectionIntensity * settings.SkyIntensity, (float)(sky->GetMipLevels() - 1), 0.0f,
                                        settings.AmbientIntensity * settings.SkyIntensity);
                if (const auto& sh = sky->GetIrradianceSH())
                {
                    u.EnvParams.z = 1.0f;
                    for (int i = 0; i < 9; ++i) u.IrradianceSH[i] = glm::vec4((*sh)[i], 0.0f);
                }
            }
        bool castShadows = false, debugCascades = false;
        float shadowDistance = 40.0f, shadowBias = 0.0015f;
        int cascadeCount = 4;
        for (auto [e, light, t] : reg.ViewOf<DirectionalLightComponent, TransformComponent>())
        {
            if (!t.ActiveInHierarchy) continue;
            u.LightDirection = glm::vec4(t.Forward(), 1.0f);
            u.LightColor = glm::vec4(ToLinear(light.Color) * light.Intensity, 1.0f);
            castShadows = light.CastShadows;
            shadowDistance = light.ShadowDistance;
            shadowBias = light.ShadowBias;
            cascadeCount = std::clamp(light.ShadowCascades, 1, ShadowCascades::kMax);
            debugCascades = light.DebugCascades;
            break;
        }
        uint32_t pointShadows = 0;
        uint32_t shadowFaces[kMaxPointShadows] = {};
        const float shadowNear = 0.05f;
        for (auto [e, light, t] : reg.ViewOf<PointLightComponent, TransformComponent>())
        {
            if (!t.ActiveInHierarchy || u.Counts.x >= (int)kMaxPointLights) continue;
            glm::vec3 pos = t.GetWorldPosition();
            float slot = 0.0f; // shadow slot + 1
            if (light.CastShadows && pointShadows < kMaxPointShadows && light.Range > 0.1f)
            {
                for (int f = 0; f < 6; ++f)
                    u.PointShadowViewProj[pointShadows * 6 + f] = PointShadowFaceViewProj(pos, f, shadowNear, light.Range);
                u.PointShadowParams[pointShadows] = glm::vec4(shadowNear, light.Range, light.ShadowBias, 0.0f);
                shadowFaces[pointShadows] = 6;
                slot = (float)(++pointShadows);
            }
            u.PointLights[u.Counts.x++] = { glm::vec4(pos, light.Range), glm::vec4(ToLinear(light.Color) * light.Intensity, slot),
                                            glm::vec4(0.0f, 0.0f, 0.0f, -2.0f), glm::vec4(0.0f) };
        }
        for (auto [e, light, t] : reg.ViewOf<SpotLightComponent, TransformComponent>())
        {
            if (!t.ActiveInHierarchy || u.Counts.x >= (int)kMaxPointLights) continue;
            glm::vec3 pos = t.GetWorldPosition(), dir = t.Forward();
            float outer = glm::radians(std::clamp(light.SpotAngle, 1.0f, 179.0f)) * 0.5f;
            float inner = glm::radians(std::clamp(light.InnerSpotAngle, 0.0f, light.SpotAngle)) * 0.5f;
            float cosOuter = std::cos(outer), cosInner = std::max(std::cos(inner), cosOuter + 1e-4f);
            float slot = 0.0f;
            if (light.CastShadows && pointShadows < kMaxPointShadows && light.Range > 0.1f)
            {
                u.PointShadowViewProj[pointShadows * 6] = SpotShadowViewProj(pos, dir, light.SpotAngle, shadowNear, light.Range);
                u.PointShadowParams[pointShadows] = glm::vec4(shadowNear, light.Range, light.ShadowBias, 0.0f);
                shadowFaces[pointShadows] = 1;
                slot = (float)(++pointShadows);
            }
            u.PointLights[u.Counts.x++] = { glm::vec4(pos, light.Range), glm::vec4(ToLinear(light.Color) * light.Intensity, slot),
                                            glm::vec4(dir, cosOuter), glm::vec4(cosInner, 0.0f, 0.0f, 0.0f) };
        }

        // Prepare CPU-side draw lists
        CollectSprites(scene, camera);
        CollectTilemaps(scene, camera);
        CollectText(scene, camera);
        CollectParticles(scene, camera);
        if (options.EditorOverlays)
        {
            CollectEditorIcons(scene, camera);
            BuildEditorOverlays(scene, options);
            if (options.Grid) BuildGrid(camera, options.Grid2D, m_GridLines);
        }

        if (pointShadows > 0)
        {
            RenderPointShadows(cmd, scene, u.PointShadowViewProj, shadowFaces, pointShadows, &u, sizeof(u));
            for (uint32_t i = 0; i < pointShadows; ++i) u.PointShadowParams[i].w = (float)m_PointShadowBindless;
        }
        if (castShadows)
        {
            ShadowCascades cascades;
            RenderShadowMap(cmd, scene, camera, glm::vec3(u.LightDirection), shadowDistance, cascadeCount, &u, sizeof(u), cascades);
            for (int i = 0; i < cascades.Count; ++i)
            {
                u.LightViewProj[i] = cascades.ViewProj[i];
                u.CascadeSplits[i] = cascades.Splits[i];
                u.CascadeTexel[i] = cascades.Texel[i];
            }
            u.ShadowParams = glm::vec4((float)cascades.Count, (float)m_ShadowBindless, shadowBias, 0.0f);
            u.CascadeParams = glm::vec4(0.0f, 0.15f, debugCascades ? 1.0f : 0.0f, 0.0f);
        }

        VkExtent2D extent{ target.GetWidth(), target.GetHeight() };
        Vk::TransitionImage(cmd, target.GetHdr().GetHandle(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        Vk::TransitionImage(cmd, target.GetIds().GetHandle(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        Vk::TransitionImage(cmd, target.GetDepth().GetHandle(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                            VK_IMAGE_ASPECT_DEPTH_BIT);

        glm::vec4 clear = ToLinear(camera.ClearColor);
        VkRenderingAttachmentInfo colors[2]{};
        colors[0] = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
        colors[0].imageView = target.GetHdr().GetView();
        colors[0].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colors[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        colors[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        colors[0].clearValue.color = { { clear.r, clear.g, clear.b, 1.0f } };
        colors[1] = colors[0];
        colors[1].imageView = target.GetIds().GetView();
        colors[1].clearValue.color.uint32[0] = 0;
        colors[1].clearValue.color.uint32[1] = 0;
        colors[1].clearValue.color.uint32[2] = 0;
        colors[1].clearValue.color.uint32[3] = 0;
        VkRenderingAttachmentInfo depth{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
        depth.imageView = target.GetDepth().GetView();
        depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depth.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depth.clearValue.depthStencil = { 1.0f, 0 };

        VkRenderingInfo rendering{ VK_STRUCTURE_TYPE_RENDERING_INFO };
        rendering.renderArea = { { 0, 0 }, extent };
        rendering.layerCount = 1;
        rendering.colorAttachmentCount = 2;
        rendering.pColorAttachments = colors;
        rendering.pDepthAttachment = &depth;
        vkCmdBeginRendering(cmd, &rendering);

        // Negative-height viewport gives a Y-up clip space, matching glm's (OpenGL-style) projections.
        VkViewport viewport{ 0.0f, (float)extent.height, (float)extent.width, -(float)extent.height, 0.0f, 1.0f };
        VkRect2D scissor{ { 0, 0 }, extent };
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &scissor);
        vkCmdSetLineWidth(cmd, 1.0f);

        device.BindBindless(cmd);
        device.BindUniforms(cmd, &u, sizeof(u));

        if (camera.Skybox)
        {
            res.Sky.Bind(cmd);
            vkCmdDraw(cmd, 3, 1, 0, 0);
            m_Stats.DrawCalls++;
        }
        DrawMeshes(cmd, scene, u.ViewProj, false);

        if (!m_GridLines.empty())
        {
            SceneUniforms gridU = u;
            gridU.GridParams.x = (options.Grid2D || camera.Orthographic) ? 0.0f : 60.0f;
            device.BindUniforms(cmd, &gridU, sizeof(gridU));
            FlushLines(cmd, m_GridLines, res.Line);
            device.BindUniforms(cmd, &u, sizeof(u));
        }

        DrawSprites(cmd);

        vkCmdEndRendering(cmd);
        target.RecordPickCopy(cmd);

        // Post processing: bloom (HDR) -> composite (exposure, tonemap, vignette) -> optional FXAA -> overlays and UI.
        Vk::TransitionImage(cmd, target.GetHdr().GetHandle(), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        device.BindBindless(cmd);
        bool bloom = settings.Bloom && settings.BloomIntensity > 0.0f && target.GetBloomLevels() > 0;
        if (bloom) RenderBloom(cmd, target, settings);

        VkViewport flat{ 0.0f, 0.0f, (float)extent.width, (float)extent.height, 0.0f, 1.0f };
        CompositePush push{ target.GetHdrBindless(), settings.Exposure, settings.Tonemapping ? 1u : 0u,
                            bloom ? target.GetBloomBindless(0) + 1 : 0u,
                            settings.BloomIntensity / (float)std::max(1u, target.GetBloomLevels()), settings.Vignette };
        auto beginPass = [&](const Image& image) {
            Vk::TransitionImage(cmd, image.GetHandle(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
            VkRenderingAttachmentInfo att{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
            att.imageView = image.GetView();
            att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            att.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            VkRenderingInfo info{ VK_STRUCTURE_TYPE_RENDERING_INFO };
            info.renderArea = { { 0, 0 }, extent };
            info.layerCount = 1;
            info.colorAttachmentCount = 1;
            info.pColorAttachments = &att;
            vkCmdBeginRendering(cmd, &info);
            vkCmdSetViewport(cmd, 0, 1, &flat);
            vkCmdSetScissor(cmd, 0, 1, &scissor);
        };
        auto composite = [&] {
            res.Composite.Bind(cmd);
            vkCmdPushConstants(cmd, device.GetPipelineLayout(), VK_SHADER_STAGE_ALL, 0, sizeof(push), &push);
            vkCmdDraw(cmd, 3, 1, 0, 0);
        };
        if (settings.FXAA)
        {
            beginPass(target.GetLdr());
            composite();
            vkCmdEndRendering(cmd);
            Vk::TransitionImage(cmd, target.GetLdr().GetHandle(), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            m_Stats.DrawCalls++;
            beginPass(target.GetOutput());
            res.Fxaa.Bind(cmd);
            FxaaPush fxaa{ target.GetLdrBindless(), 0u, glm::vec2(1.0f / extent.width, 1.0f / extent.height) };
            vkCmdPushConstants(cmd, device.GetPipelineLayout(), VK_SHADER_STAGE_ALL, 0, sizeof(fxaa), &fxaa);
            vkCmdDraw(cmd, 3, 1, 0, 0);
        }
        else
        {
            beginPass(target.GetOutput());
            composite();
        }
        if (!m_Lines.empty())
        {
            // Overlays go on top of the tonemapped image so their colors stay exact.
            vkCmdSetViewport(cmd, 0, 1, &viewport);
            device.BindUniforms(cmd, &u, sizeof(u));
            FlushLines(cmd, m_Lines, res.LineOverlay);
        }
        if (options.RenderUI)
        {
            vkCmdSetViewport(cmd, 0, 1, &flat);
            DrawUI(cmd, scene, extent);
        }
        vkCmdEndRendering(cmd);
        Vk::TransitionImage(cmd, target.GetOutput().GetHandle(), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        m_Stats.DrawCalls++;

        m_Sprites.clear();
        m_Lines.clear();
        m_GridLines.clear();
    }

    namespace
    {
        // Material for a renderer: the assigned asset, or one built from the component's simple fields.
        const Material& ResolveMaterial(const MeshRendererComponent& mr, Material& scratch)
        {
            if (auto m = AssetManager::GetMaterial(mr.Material)) return *m;
            scratch = Material();
            scratch.BaseColor = mr.Color;
            scratch.BaseColorMap = mr.Texture;
            scratch.Tiling = mr.Tiling;
            scratch.Roughness = std::clamp(1.0f - mr.Smoothness, 0.02f, 1.0f);
            scratch.EmissionColor = glm::vec3(mr.Color);
            scratch.EmissionStrength = mr.Emission;
            scratch.CastShadows = mr.CastShadows;
            return scratch;
        }

        uint32_t TextureIndex(AssetHandle h, uint32_t fallback)
        {
            auto tex = AssetManager::GetTexture(h);
            return tex ? tex->GetBindlessIndex() : fallback;
        }

        VkDeviceAddress UploadMaterial(const Material& m)
        {
            TransientAllocation a = RenderDevice::Get().AllocateTransient(sizeof(MaterialGPU), 16);
            if (!a) return 0;
            uint32_t white = AssetManager::GetWhiteTexture()->GetBindlessIndex();
            auto* g = (MaterialGPU*)a.Data;
            g->BaseColor = ToLinear(m.BaseColor);
            g->Emission = glm::vec4(ToLinear(m.EmissionColor) * m.EmissionStrength, 0.0f);
            g->Params = glm::vec4(m.Metallic, m.Roughness, m.AlphaCutoff, m.NormalStrength);
            g->TilingOffset = glm::vec4(m.Tiling, m.Offset);
            uint32_t normal = TextureIndex(m.NormalMap, white);
            g->Maps = glm::uvec4(TextureIndex(m.BaseColorMap, white), normal, TextureIndex(m.MetallicRoughnessMap, white),
                                 TextureIndex(m.EmissionMap, white));
            g->Flags = glm::uvec4((uint32_t)m.BlendMode, m.NormalMap.IsValid() && normal != white ? 1u : 0u, 0u, 0u);
            return a.Address;
        }
    }

    SceneRenderer::~SceneRenderer()
    {
        if (m_ShadowMap) RenderDevice::Get().UnregisterTexture(m_ShadowBindless);
        if (m_PointShadowMap) RenderDevice::Get().UnregisterTexture(m_PointShadowBindless);
    }

    glm::mat4 PointShadowFaceViewProj(const glm::vec3& position, int face, float nearPlane, float farPlane)
    {
        static const glm::vec3 dirs[6] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
        static const glm::vec3 ups[6] = { { 0, 1, 0 }, { 0, 1, 0 }, { 0, 0, 1 }, { 0, 0, 1 }, { 0, 1, 0 }, { 0, 1, 0 } };
        glm::mat4 view = glm::lookAt(position, position + dirs[face], ups[face]);
        glm::mat4 proj = glm::perspective(glm::radians(90.0f), 1.0f, nearPlane, farPlane);
        return proj * view;
    }

    glm::mat4 SpotShadowViewProj(const glm::vec3& position, const glm::vec3& direction, float spotAngleDegrees, float nearPlane, float farPlane)
    {
        glm::vec3 dir = glm::normalize(direction);
        glm::vec3 up = std::abs(dir.y) > 0.99f ? glm::vec3(0, 0, 1) : glm::vec3(0, 1, 0);
        float fov = glm::radians(std::clamp(spotAngleDegrees + 4.0f, 2.0f, 170.0f)); // small margin for PCF at the rim
        return glm::perspective(fov, 1.0f, nearPlane, farPlane) * glm::lookAt(position, position + dir, up);
    }

    void SceneRenderer::RenderPointShadows(VkCommandBuffer cmd, Scene& scene, const glm::mat4* faceViewProj, const uint32_t* faceCounts,
                                           uint32_t lightCount, const void* uniforms, size_t uniformSize)
    {
        auto& device = RenderDevice::Get();
        const uint32_t width = kPointShadowTile * 6, height = kPointShadowTile * kMaxPointShadows;
        if (!m_PointShadowMap)
        {
            m_PointShadowMap = MakeScope<Image>(ImageDesc{ width, height, VK_FORMAT_D32_SFLOAT,
                                                           VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, 1, "PointShadowAtlas" });
            m_PointShadowBindless = device.RegisterTexture(m_PointShadowMap->GetView(), device.GetNearestClampSampler());
            VkImage img = m_PointShadowMap->GetHandle();
            device.ImmediateSubmit([&](VkCommandBuffer c) {
                Vk::TransitionImage(c, img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);
            });
        }

        VkImage img = m_PointShadowMap->GetHandle();
        Vk::TransitionImage(cmd, img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);
        VkRenderingAttachmentInfo depth{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
        depth.imageView = m_PointShadowMap->GetView();
        depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        depth.clearValue.depthStencil = { 1.0f, 0 };
        VkRenderingInfo info{ VK_STRUCTURE_TYPE_RENDERING_INFO };
        info.renderArea = { { 0, 0 }, { width, height } };
        info.layerCount = 1;
        info.pDepthAttachment = &depth;
        vkCmdBeginRendering(cmd, &info);
        vkCmdSetLineWidth(cmd, 1.0f);
        device.BindBindless(cmd);
        std::vector<uint8_t> faceUniforms((const uint8_t*)uniforms, (const uint8_t*)uniforms + uniformSize);
        for (uint32_t light = 0; light < lightCount; ++light)
            for (uint32_t face = 0; face < faceCounts[light]; ++face)
            {
                float x = (float)(face * kPointShadowTile), y = (float)(light * kPointShadowTile);
                // Same flipped viewport as the scene: tile row 0 is NDC y = +1.
                VkViewport viewport{ x, y + kPointShadowTile, (float)kPointShadowTile, -(float)kPointShadowTile, 0.0f, 1.0f };
                VkRect2D scissor{ { (int32_t)x, (int32_t)y }, { kPointShadowTile, kPointShadowTile } };
                vkCmdSetViewport(cmd, 0, 1, &viewport);
                vkCmdSetScissor(cmd, 0, 1, &scissor);
                const glm::mat4& viewProj = faceViewProj[light * 6 + face];
                std::memcpy(faceUniforms.data(), &viewProj, sizeof(glm::mat4)); // ViewProj is the first member
                device.BindUniforms(cmd, faceUniforms.data(), faceUniforms.size());
                DrawMeshes(cmd, scene, viewProj, true);
            }
        vkCmdEndRendering(cmd);
        Vk::TransitionImage(cmd, img, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);
    }

    void SceneRenderer::DrawMeshes(VkCommandBuffer cmd, Scene& scene, const glm::mat4& viewProj, bool shadowPass)
    {
        auto& device = RenderDevice::Get();
        auto& res = Renderer::Get();
        Frustum frustum(viewProj);
        Material scratch;

        struct Draw
        {
            Mesh* MeshPtr;
            uint32_t FirstIndex = 0, IndexCount = 0;
            MeshPush Push;
            const Pipeline* Pipe;
            float Depth;
        };
        std::vector<Draw> transparent;
        const Pipeline* bound = nullptr;
        auto submit = [&](const Draw& d) {
            if (d.Pipe != bound)
            {
                d.Pipe->Bind(cmd);
                bound = d.Pipe;
            }
            vkCmdPushConstants(cmd, device.GetPipelineLayout(), VK_SHADER_STAGE_ALL, 0, sizeof(MeshPush), &d.Push);
            d.MeshPtr->Bind(cmd);
            vkCmdDrawIndexed(cmd, d.IndexCount, 1, d.FirstIndex, 0, 0);
            m_Stats.DrawCalls++;
            if (shadowPass) m_Stats.ShadowCasters++;
            else
            {
                m_Stats.Meshes++;
                m_Stats.Triangles += d.IndexCount / 3;
            }
        };

        glm::vec3 camPos = glm::vec3(glm::inverse(viewProj) * glm::vec4(0, 0, 0, 1)); // only used for sorting
        auto& reg = scene.GetRegistry();
        for (auto [e, mr, t] : reg.ViewOf<MeshRendererComponent, TransformComponent>())
        {
            if (!t.ActiveInHierarchy) continue;
            Ref<Mesh> mesh = AssetManager::GetMesh(mr.Mesh);
            if (!mesh) continue;
            // Skinned meshes use the Animator's matrices; without an Animator they render in bind pose.
            const AnimatorComponent* animator = mesh->IsSkinned() ? reg.TryGet<AnimatorComponent>(e) : nullptr;
            bool skinned = animator && animator->SkinMatrices.size() == mesh->GetSkeleton()->Joints.size() && mesh->GetSkinAddress();
            if (shadowPass && !mr.CastShadows) continue;
            if (!skinned && !frustum.Intersects(t.World, mesh->GetBounds())) // animated bounds are unknown: never cull
            {
                if (!shadowPass) m_Stats.Culled++;
                continue;
            }

            // Joint matrices are shared by all sub-meshes of the entity.
            VkDeviceAddress jointsAddress = 0;
            if (skinned)
            {
                size_t bytes = animator->SkinMatrices.size() * sizeof(glm::mat4);
                TransientAllocation joints = device.AllocateTransient(bytes, 16);
                if (joints)
                {
                    std::memcpy(joints.Data, animator->SkinMatrices.data(), bytes);
                    jointsAddress = joints.Address;
                }
            }

            // Material per sub-mesh: the renderer's Material overrides all; otherwise the model's material for the
            // slot; otherwise the renderer's simple color/texture fields.
            Ref<Material> overrideMat = AssetManager::GetMaterial(mr.Material);
            const auto& slots = AssetManager::GetMeshMaterials(mr.Mesh);
            for (const SubMesh& sub : mesh->GetSubMeshes())
            {
                const Material* matPtr = overrideMat.get();
                Ref<Material> slotMat;
                if (!matPtr && sub.MaterialSlot >= 0 && sub.MaterialSlot < (int32_t)slots.size())
                {
                    slotMat = AssetManager::GetMaterial(slots[sub.MaterialSlot]);
                    matPtr = slotMat.get();
                }
                if (!matPtr) matPtr = &ResolveMaterial(mr, scratch);
                const Material& mat = *matPtr;
                bool isTransparent = mat.BlendMode == MaterialBlendMode::Transparent;
                if (shadowPass && (!mat.CastShadows || !mr.CastShadows || isTransparent)) continue;

                Draw d;
                d.MeshPtr = mesh.get();
                d.FirstIndex = sub.IndexOffset;
                d.IndexCount = sub.IndexCount;
                d.Push.Model = t.World;
                d.Push.Material = UploadMaterial(mat);
                d.Push.EntityId = e.Index + 1;
                d.Push.Skin = jointsAddress ? mesh->GetSkinAddress() : 0;
                d.Push.Joints = jointsAddress;
                d.Push.Flags = jointsAddress ? 1u : 0u;
                if (!d.Push.Material) continue;
                d.Pipe = shadowPass ? &res.Shadow : isTransparent ? &res.MeshTransparent : mat.DoubleSided ? &res.MeshDoubleSided : &res.Mesh;
                if (isTransparent)
                {
                    d.Depth = glm::distance(glm::vec3(t.World * glm::vec4(mesh->GetBounds().Center(), 1.0f)), camPos);
                    transparent.push_back(d);
                }
                else
                    submit(d);
            }
        }
        // Transparent meshes after opaque ones, far to near.
        std::sort(transparent.begin(), transparent.end(), [](const Draw& a, const Draw& b) { return a.Depth > b.Depth; });
        for (auto& d : transparent) submit(d);
    }

    std::array<float, ShadowCascades::kMax> ComputeCascadeSplits(float nearDepth, float farDepth, int count, float lambda)
    {
        std::array<float, ShadowCascades::kMax> splits{};
        count = std::clamp(count, 1, ShadowCascades::kMax);
        nearDepth = std::max(nearDepth, 1e-3f);
        farDepth = std::max(farDepth, nearDepth + 1e-3f);
        for (int i = 0; i < count; ++i)
        {
            float f = (float)(i + 1) / count;
            float logSplit = nearDepth * std::pow(farDepth / nearDepth, f);
            float uniformSplit = nearDepth + (farDepth - nearDepth) * f;
            splits[i] = glm::mix(uniformSplit, logSplit, lambda);
        }
        splits[count - 1] = farDepth;
        for (int i = count; i < ShadowCascades::kMax; ++i) splits[i] = farDepth;
        return splits;
    }

    void SceneRenderer::RenderShadowMap(VkCommandBuffer cmd, Scene& scene, const CameraData& camera, const glm::vec3& lightDir,
                                        float distance, int cascadeCount, const void* uniforms, size_t uniformSize, ShadowCascades& out)
    {
        auto& device = RenderDevice::Get();
        if (!m_ShadowMap)
        {
            m_ShadowMap = MakeScope<Image>(ImageDesc{ kShadowMapSize, kShadowMapSize, VK_FORMAT_D32_SFLOAT,
                                                      VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, 1, "ShadowAtlas" });
            m_ShadowBindless = device.RegisterTexture(m_ShadowMap->GetView(), device.GetNearestClampSampler());
            VkImage img = m_ShadowMap->GetHandle();
            device.ImmediateSubmit([&](VkCommandBuffer c) {
                Vk::TransitionImage(c, img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);
            });
        }

        // Camera frustum corner rays (near plane -> far plane) and their view depths.
        glm::mat4 invCam = glm::inverse(camera.Projection * camera.View);
        glm::vec3 nearP[4], farP[4];
        for (int i = 0; i < 4; ++i)
        {
            glm::vec2 ndc((i & 1) ? 1.0f : -1.0f, (i & 2) ? 1.0f : -1.0f);
            glm::vec4 n = invCam * glm::vec4(ndc, 0.0f, 1.0f), f = invCam * glm::vec4(ndc, 1.0f, 1.0f);
            nearP[i] = glm::vec3(n) / n.w;
            farP[i] = glm::vec3(f) / f.w;
        }
        auto viewDepth = [&](const glm::vec3& p) { return -(camera.View * glm::vec4(p, 1.0f)).z; };
        float nearDepth = viewDepth(nearP[0]), farDepth = viewDepth(farP[0]);
        float shadowFar = std::min(distance, farDepth);
        auto splits = ComputeCascadeSplits(std::max(nearDepth, 0.01f), shadowFar, cascadeCount, 0.75f);
        auto pointAt = [&](int ray, float depth) {
            float t = (depth - nearDepth) / std::max(farDepth - nearDepth, 1e-5f);
            return nearP[ray] + (farP[ray] - nearP[ray]) * t;
        };

        glm::vec3 dir = glm::normalize(lightDir);
        glm::vec3 up = std::abs(dir.y) > 0.99f ? glm::vec3(0, 0, 1) : glm::vec3(0, 1, 0);
        glm::mat4 lightRot = glm::lookAt(glm::vec3(0.0f), dir, up);
        const float backoff = 100.0f; // include casters outside the camera frustum, between it and the light

        out.Count = std::clamp(cascadeCount, 1, ShadowCascades::kMax);
        for (int c = 0; c < out.Count; ++c)
        {
            // Bounding sphere of this slice of the frustum.
            float d0 = c == 0 ? nearDepth : splits[c - 1], d1 = splits[c];
            glm::vec3 corners[8], center(0.0f);
            for (int i = 0; i < 4; ++i)
            {
                corners[i * 2] = pointAt(i, d0);
                corners[i * 2 + 1] = pointAt(i, d1);
            }
            for (auto& p : corners) center += p / 8.0f;
            float radius = 0.0f;
            for (auto& p : corners) radius = std::max(radius, glm::distance(p, center));
            radius = std::ceil(radius * 16.0f) / 16.0f; // quantized size reduces shimmering

            // Snap the center to whole shadow texels in light space so shadows do not crawl as the camera moves.
            float unitsPerTexel = radius * 2.0f / kShadowTileSize;
            glm::vec3 ls = glm::vec3(lightRot * glm::vec4(center, 1.0f));
            ls.x = std::floor(ls.x / unitsPerTexel) * unitsPerTexel;
            ls.y = std::floor(ls.y / unitsPerTexel) * unitsPerTexel;
            center = glm::vec3(glm::inverse(lightRot) * glm::vec4(ls, 1.0f));

            glm::mat4 view = glm::lookAt(center - dir * (radius + backoff), center, up);
            glm::mat4 proj = glm::ortho(-radius, radius, -radius, radius, 0.0f, radius * 2.0f + backoff);
            out.ViewProj[c] = proj * view;
            out.Splits[c] = d1;
            out.Texel[c] = unitsPerTexel;
        }

        // Depth-only passes into the atlas tiles, reusing the mesh vertex shader with the light as the camera.
        VkImage img = m_ShadowMap->GetHandle();
        Vk::TransitionImage(cmd, img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);
        VkRenderingAttachmentInfo depth{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
        depth.imageView = m_ShadowMap->GetView();
        depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        depth.clearValue.depthStencil = { 1.0f, 0 };
        VkRenderingInfo info{ VK_STRUCTURE_TYPE_RENDERING_INFO };
        info.renderArea = { { 0, 0 }, { kShadowMapSize, kShadowMapSize } };
        info.layerCount = 1;
        info.pDepthAttachment = &depth;
        vkCmdBeginRendering(cmd, &info);
        vkCmdSetLineWidth(cmd, 1.0f);
        device.BindBindless(cmd);
        std::vector<uint8_t> shadowUniforms((const uint8_t*)uniforms, (const uint8_t*)uniforms + uniformSize);
        for (int c = 0; c < out.Count; ++c)
        {
            float x = (float)((c & 1) * kShadowTileSize), y = (float)((c >> 1) * kShadowTileSize);
            VkViewport viewport{ x, y + kShadowTileSize, (float)kShadowTileSize, -(float)kShadowTileSize, 0.0f, 1.0f };
            VkRect2D scissor{ { (int32_t)x, (int32_t)y }, { kShadowTileSize, kShadowTileSize } };
            vkCmdSetViewport(cmd, 0, 1, &viewport);
            vkCmdSetScissor(cmd, 0, 1, &scissor);
            std::memcpy(shadowUniforms.data(), &out.ViewProj[c], sizeof(glm::mat4)); // ViewProj is the first member
            device.BindUniforms(cmd, shadowUniforms.data(), shadowUniforms.size());
            DrawMeshes(cmd, scene, out.ViewProj[c], true);
        }
        vkCmdEndRendering(cmd);
        Vk::TransitionImage(cmd, img, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);
    }

    void SceneRenderer::CollectSprites(Scene& scene, const CameraData& camera)
    {
        uint32_t white = AssetManager::GetWhiteTexture()->GetBindlessIndex();
        for (auto [e, sr, t] : scene.GetRegistry().ViewOf<SpriteRendererComponent, TransformComponent>())
        {
            if (!t.ActiveInHierarchy) continue;
            Ref<Texture2D> tex = AssetManager::GetTexture(sr.Texture);
            SpriteDraw d;
            d.Transform = t.World;
            int cols = std::max(1, sr.SheetColumns), rows = std::max(1, sr.SheetRows);
            if (tex && tex->GetHeight() > 0) // keep the (cell) aspect ratio, 1 unit tall
            {
                float aspect = ((float)tex->GetWidth() / cols) / ((float)tex->GetHeight() / rows);
                d.Transform = d.Transform * glm::scale(glm::mat4(1.0f), glm::vec3(aspect, 1.0f, 1.0f));
            }
            d.Color = ToLinear(sr.Color);
            glm::vec2 cellMin(0.0f), cellMax(sr.Tiling);
            if (cols > 1 || rows > 1)
            {
                int frame = std::clamp(sr.Frame, 0, cols * rows - 1);
                glm::vec2 cell(1.0f / cols, 1.0f / rows);
                cellMin = glm::vec2((float)(frame % cols), (float)(frame / cols)) * cell;
                cellMax = cellMin + cell;
            }
            // Quad corner order: bottom-left uses UVMin; image rows run top-down.
            d.UVMin = { sr.FlipX ? cellMax.x : cellMin.x, sr.FlipY ? cellMin.y : cellMax.y };
            d.UVMax = { sr.FlipX ? cellMin.x : cellMax.x, sr.FlipY ? cellMax.y : cellMin.y };
            d.Texture = tex ? tex->GetBindlessIndex() : white;
            d.Entity = e.Index + 1;
            d.Order = sr.SortingOrder;
            d.Depth = (camera.View * t.World[3]).z; // more negative = farther
            m_Sprites.push_back(d);
        }
    }

    void SceneRenderer::CollectTilemaps(Scene& scene, const CameraData& camera)
    {
        uint32_t white = AssetManager::GetWhiteTexture()->GetBindlessIndex();
        for (auto [e, tm, t] : scene.GetRegistry().ViewOf<TilemapComponent, TransformComponent>())
        {
            if (!t.ActiveInHierarchy) continue;
            const auto& cells = tm.Cells();
            if (cells.empty()) continue;
            Ref<Texture2D> tex = AssetManager::GetTexture(tm.Tileset);
            int cols = std::max(1, tm.Columns), rows = std::max(1, tm.Rows);
            glm::vec2 cell(1.0f / cols, 1.0f / rows);
            // Half-texel inset keeps neighbouring tiles from bleeding in with filtering.
            glm::vec2 inset = tex ? glm::vec2(0.5f / tex->GetWidth(), 0.5f / tex->GetHeight()) : glm::vec2(0.0f);
            glm::vec4 color = ToLinear(tm.Color);
            float cs = tm.CellSize, depth = (camera.View * t.World[3]).z;
            glm::mat4 cellScale = glm::scale(glm::mat4(1.0f), glm::vec3(cs, cs, 1.0f));
            for (auto& [key, tile] : cells)
            {
                int32_t x = TilemapComponent::KeyX(key), y = TilemapComponent::KeyY(key);
                int frame = std::clamp(tile, 0, cols * rows - 1);
                glm::vec2 uvMin = glm::vec2((float)(frame % cols), (float)(frame / cols)) * cell + inset;
                glm::vec2 uvMax = uvMin + cell - 2.0f * inset;
                SpriteDraw d;
                d.Transform = t.World * glm::translate(glm::mat4(1.0f), glm::vec3((x + 0.5f) * cs, (y + 0.5f) * cs, 0.0f)) * cellScale;
                d.Color = color;
                d.UVMin = { uvMin.x, uvMax.y }; // bottom-left corner; image rows run top-down
                d.UVMax = { uvMax.x, uvMin.y };
                d.Texture = tex ? tex->GetBindlessIndex() : white;
                d.Entity = e.Index + 1;
                d.Order = tm.SortingOrder;
                d.Depth = depth;
                m_Sprites.push_back(d);
            }
        }
    }

    void SceneRenderer::CollectText(Scene& scene, const CameraData& camera)
    {
        for (auto [e, text, t] : scene.GetRegistry().ViewOf<TextRendererComponent, TransformComponent>())
        {
            if (!t.ActiveInHierarchy || text.Text.empty()) continue;
            auto font = AssetManager::GetFont(text.Font);
            if (!font) continue;
            glm::vec2 block;
            auto quads = font->Layout(text.Text, Font::kBakeSize, 0.0f, text.Align, 0.0f, block);
            float s = text.FontSize / font->GetLineHeight();
            glm::vec4 color = ToLinear(text.Color);
            uint32_t atlas = font->GetAtlas()->GetBindlessIndex();
            float depth = (camera.View * t.World[3]).z;
            for (auto& q : quads)
            {
                // Layout is y-down pixels; center the block on the entity and flip to y-up world units.
                glm::vec2 c = ((q.Min + q.Max) * 0.5f - block * 0.5f) * s;
                glm::vec2 size = (q.Max - q.Min) * s;
                SpriteDraw d;
                d.Transform = t.World * glm::translate(glm::mat4(1.0f), glm::vec3(c.x, -c.y, 0.0f)) *
                              glm::scale(glm::mat4(1.0f), glm::vec3(size, 1.0f));
                d.Color = color;
                d.UVMin = { q.UVMin.x, q.UVMax.y };
                d.UVMax = { q.UVMax.x, q.UVMin.y };
                d.Texture = atlas;
                d.Entity = e.Index + 1;
                d.Order = text.SortingOrder;
                d.Depth = depth;
                m_Sprites.push_back(d);
            }
        }
    }

    void SceneRenderer::CollectParticles(Scene& scene, const CameraData& camera)
    {
        glm::mat4 invView = glm::inverse(camera.View);
        glm::vec3 right = glm::vec3(invView[0]), up = glm::vec3(invView[1]), forward = glm::cross(right, up);
        for (auto [e, ps, t] : scene.GetRegistry().ViewOf<ParticleSystemComponent, TransformComponent>())
        {
            if (!t.ActiveInHierarchy || ps.Particles.empty()) continue;
            auto tex = ps.Texture.IsValid() ? AssetManager::GetTexture(ps.Texture) : nullptr;
            uint32_t texture = (tex ? tex : AssetManager::GetParticleTexture())->GetBindlessIndex();
            bool local = ps.SimulationSpace == ParticleSpace::Local;
            float worldScale = local ? glm::length(glm::vec3(t.World[0])) : 1.0f;
            uint32_t trailTexture = AssetManager::GetParticleTexture()->GetBindlessIndex();
            for (const auto& p : ps.Particles)
            {
                float life = std::clamp(p.Age / p.Lifetime, 0.0f, 1.0f);
                glm::vec3 pos = local ? glm::vec3(t.World * glm::vec4(p.Position, 1.0f)) : p.Position;
                float size = p.Size * ps.SizeOverLifetime.Evaluate(life) * worldScale;
                glm::vec4 color = p.Color * ps.ColorOverLifetime.Evaluate(life);
                if (ps.Trails && p.TrailCount > 0)
                {
                    // Ribbon of camera-facing segments from the particle back through its history, tapering and fading.
                    glm::vec3 prev = pos;
                    int n = p.TrailCount;
                    for (int k = 0; k < n; ++k)
                    {
                        glm::vec3 q = local ? glm::vec3(t.World * glm::vec4(p.Trail[k], 1.0f)) : p.Trail[k];
                        glm::vec3 axis = q - prev;
                        if (glm::dot(axis, axis) > 1e-8f)
                        {
                            glm::vec3 mid = (q + prev) * 0.5f;
                            glm::vec3 side = glm::cross(axis, camera.Position - mid);
                            float fade = 1.0f - (float)k / n;
                            float width = size * ps.TrailWidth * fade;
                            if (glm::dot(side, side) > 1e-10f && width > 0.0f)
                            {
                                side = glm::normalize(side) * width;
                                SpriteDraw seg;
                                seg.Transform = glm::mat4(glm::vec4(axis, 0), glm::vec4(side, 0), glm::vec4(forward, 0), glm::vec4(mid, 1));
                                seg.Color = ToLinear(color * glm::vec4(1.0f, 1.0f, 1.0f, fade));
                                seg.UVMin = { 0.5f, 1.0f }; // soft falloff across the width, constant along the length
                                seg.UVMax = { 0.5f, 0.0f };
                                seg.Texture = trailTexture;
                                seg.Entity = e.Index + 1;
                                seg.Order = ps.SortingOrder;
                                seg.Depth = (camera.View * glm::vec4(mid, 1.0f)).z;
                                seg.Additive = ps.Blend == ParticleBlend::Additive;
                                m_Sprites.push_back(seg);
                            }
                        }
                        prev = q;
                    }
                }
                float c = std::cos(p.Rotation), s = std::sin(p.Rotation);
                glm::vec3 r = (right * c + up * s) * size, u = (up * c - right * s) * size;
                SpriteDraw d;
                d.Transform = glm::mat4(glm::vec4(r, 0), glm::vec4(u, 0), glm::vec4(forward, 0), glm::vec4(pos, 1));
                d.Color = ToLinear(color);
                d.UVMin = { 0, 1 };
                d.UVMax = { 1, 0 };
                d.Texture = texture;
                d.Entity = e.Index + 1;
                d.Order = ps.SortingOrder;
                d.Depth = (camera.View * glm::vec4(pos, 1.0f)).z;
                d.Additive = ps.Blend == ParticleBlend::Additive;
                m_Sprites.push_back(d);
            }
        }
    }

    void SceneRenderer::CollectEditorIcons(Scene& scene, const CameraData& camera)
    {
        auto& res = Renderer::Get();
        auto& reg = scene.GetRegistry();
        glm::mat4 invView = glm::inverse(camera.View);
        glm::vec3 right = glm::vec3(invView[0]), up = glm::vec3(invView[1]);
        float tanHalf = 1.0f / camera.Projection[1][1];

        auto addIcon = [&](Entity e, IconId icon, const glm::vec4& tint) {
            auto& t = reg.Get<TransformComponent>(e);
            glm::vec3 p = t.GetWorldPosition();
            float scale = camera.Orthographic ? tanHalf * 0.12f : glm::distance(p, camera.Position) * tanHalf * 0.12f;
            SpriteDraw d;
            d.Transform = glm::mat4(glm::vec4(right * scale, 0), glm::vec4(up * scale, 0), glm::vec4(glm::cross(right, up), 0), glm::vec4(p, 1));
            d.Color = tint;
            d.UVMin = { 0, 1 };
            d.UVMax = { 1, 0 };
            d.Texture = res.Icons[icon]->GetBindlessIndex();
            d.Entity = e.Index + 1;
            d.Order = INT32_MAX;
            d.Depth = (camera.View * glm::vec4(p, 1)).z;
            m_Sprites.push_back(d);
        };
        for (auto [e, c] : reg.ViewOf<CameraComponent>()) addIcon(e, IconCamera, { 0.9f, 0.9f, 0.9f, 1 });
        for (auto [e, l] : reg.ViewOf<DirectionalLightComponent>()) addIcon(e, IconSun, { 1.0f, 0.85f, 0.3f, 1 });
        for (auto [e, l] : reg.ViewOf<PointLightComponent>()) addIcon(e, IconBulb, glm::vec4(ToLinear(l.Color), 1));
        for (auto [e, l] : reg.ViewOf<SpotLightComponent>()) addIcon(e, IconBulb, glm::vec4(ToLinear(l.Color), 1));
        for (auto [e, a] : reg.ViewOf<AudioSourceComponent>()) addIcon(e, IconSpeaker, { 0.6f, 0.8f, 1.0f, 1 });
    }

    void SceneRenderer::DrawSprites(VkCommandBuffer cmd)
    {
        if (m_Sprites.empty()) return;
        auto& res = Renderer::Get();
        std::stable_sort(m_Sprites.begin(), m_Sprites.end(), [](const SpriteDraw& a, const SpriteDraw& b) {
            if (a.Order != b.Order) return a.Order < b.Order;
            return a.Depth < b.Depth; // back to front
        });

        size_t count = m_Sprites.size();
        TransientAllocation vb = RenderDevice::Get().AllocateTransient(count * 4 * sizeof(SpriteVertex));
        if (!vb) return;
        auto* v = (SpriteVertex*)vb.Data;
        const glm::vec4 corners[4] = { { -0.5f, -0.5f, 0, 1 }, { 0.5f, -0.5f, 0, 1 }, { 0.5f, 0.5f, 0, 1 }, { -0.5f, 0.5f, 0, 1 } };
        for (auto& s : m_Sprites)
        {
            glm::vec2 uvs[4] = { { s.UVMin.x, s.UVMin.y }, { s.UVMax.x, s.UVMin.y }, { s.UVMax.x, s.UVMax.y }, { s.UVMin.x, s.UVMax.y } };
            for (int i = 0; i < 4; ++i)
                *v++ = { glm::vec3(s.Transform * corners[i]), s.Color, uvs[i], s.Texture, s.Entity };
        }

        VkBuffer buffer = vb.Buffer;
        vkCmdBindVertexBuffers(cmd, 0, 1, &buffer, &vb.Offset);
        vkCmdBindIndexBuffer(cmd, res.QuadIndices.GetHandle(), 0, VK_INDEX_TYPE_UINT32);
        // Draw in sorted order, switching pipelines only where the blend mode changes.
        const Pipeline* bound = nullptr;
        for (size_t runStart = 0; runStart < count;)
        {
            bool additive = m_Sprites[runStart].Additive;
            size_t runEnd = runStart;
            while (runEnd < count && m_Sprites[runEnd].Additive == additive && runEnd - runStart < kMaxQuadsPerDraw) ++runEnd;
            const Pipeline* pipeline = additive ? &res.SpriteAdditive : &res.Sprite;
            if (pipeline != bound)
            {
                pipeline->Bind(cmd);
                bound = pipeline;
            }
            vkCmdDrawIndexed(cmd, (uint32_t)(runEnd - runStart) * 6, 1, 0, (int32_t)(runStart * 4), 0);
            m_Stats.DrawCalls++;
            runStart = runEnd;
        }
        m_Stats.Sprites += (uint32_t)count;
        m_Stats.Triangles += (uint32_t)count * 2;
    }

    void SceneRenderer::DrawUI(VkCommandBuffer cmd, Scene& scene, VkExtent2D extent)
    {
        std::vector<UI::DrawCommand> commands;
        glm::vec2 screen((float)extent.width, (float)extent.height);
        UI::BuildDrawList(scene, screen, commands);
        if (commands.empty()) return;

        TransientAllocation vb = RenderDevice::Get().AllocateTransient(commands.size() * 4 * sizeof(SpriteVertex));
        if (!vb) return;
        auto* v = (SpriteVertex*)vb.Data;
        for (auto& c : commands)
        {
            *v++ = { glm::vec3(c.Min.x, c.Min.y, 0), c.Color, { c.UVMin.x, c.UVMin.y }, c.Texture, 0 };
            *v++ = { glm::vec3(c.Max.x, c.Min.y, 0), c.Color, { c.UVMax.x, c.UVMin.y }, c.Texture, 0 };
            *v++ = { glm::vec3(c.Max.x, c.Max.y, 0), c.Color, { c.UVMax.x, c.UVMax.y }, c.Texture, 0 };
            *v++ = { glm::vec3(c.Min.x, c.Max.y, 0), c.Color, { c.UVMin.x, c.UVMax.y }, c.Texture, 0 };
        }
        auto& res = Renderer::Get();
        res.UI.Bind(cmd);
        vkCmdPushConstants(cmd, RenderDevice::Get().GetPipelineLayout(), VK_SHADER_STAGE_ALL, 0, sizeof(glm::vec2), &screen);
        VkBuffer buffer = vb.Buffer;
        vkCmdBindVertexBuffers(cmd, 0, 1, &buffer, &vb.Offset);
        vkCmdBindIndexBuffer(cmd, res.QuadIndices.GetHandle(), 0, VK_INDEX_TYPE_UINT32);
        size_t count = commands.size();
        for (size_t first = 0; first < count; first += kMaxQuadsPerDraw)
        {
            uint32_t n = (uint32_t)std::min<size_t>(kMaxQuadsPerDraw, count - first);
            vkCmdDrawIndexed(cmd, n * 6, 1, 0, (int32_t)(first * 4), 0);
            m_Stats.DrawCalls++;
        }
    }

    void SceneRenderer::RenderBloom(VkCommandBuffer cmd, SceneRenderTarget& target, const SceneSettings& settings)
    {
        auto& device = RenderDevice::Get();
        auto& res = Renderer::Get();
        auto pass = [&](const Image& dst, bool load, const Pipeline& pipeline, const BloomPush& push) {
            Vk::TransitionImage(cmd, dst.GetHandle(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
            VkRenderingAttachmentInfo att{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
            att.imageView = dst.GetView();
            att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            att.loadOp = load ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            VkRenderingInfo info{ VK_STRUCTURE_TYPE_RENDERING_INFO };
            info.renderArea = { { 0, 0 }, dst.GetExtent() };
            info.layerCount = 1;
            info.colorAttachmentCount = 1;
            info.pColorAttachments = &att;
            vkCmdBeginRendering(cmd, &info);
            VkViewport viewport{ 0.0f, 0.0f, (float)dst.GetWidth(), (float)dst.GetHeight(), 0.0f, 1.0f };
            VkRect2D scissor{ { 0, 0 }, dst.GetExtent() };
            vkCmdSetViewport(cmd, 0, 1, &viewport);
            vkCmdSetScissor(cmd, 0, 1, &scissor);
            pipeline.Bind(cmd);
            vkCmdPushConstants(cmd, device.GetPipelineLayout(), VK_SHADER_STAGE_ALL, 0, sizeof(push), &push);
            vkCmdDraw(cmd, 3, 1, 0, 0);
            vkCmdEndRendering(cmd);
            Vk::TransitionImage(cmd, dst.GetHandle(), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            m_Stats.DrawCalls++;
        };

        uint32_t levels = target.GetBloomLevels();
        // Downsample: HDR -> level 0 (thresholded) -> level 1 -> ...
        for (uint32_t i = 0; i < levels; ++i)
        {
            const Image& src = i == 0 ? target.GetHdr() : target.GetBloom(i - 1);
            BloomPush push{ i == 0 ? target.GetHdrBindless() : target.GetBloomBindless(i - 1), i == 0 ? 1u : 0u,
                            settings.BloomThreshold, settings.BloomKnee,
                            glm::vec2(1.0f / src.GetWidth(), 1.0f / src.GetHeight()), settings.BloomRadius };
            pass(target.GetBloom(i), false, res.BloomDown, push);
        }
        // Upsample: add each smaller level onto the next larger one.
        for (int i = (int)levels - 2; i >= 0; --i)
        {
            const Image& src = target.GetBloom(i + 1);
            BloomPush push{ target.GetBloomBindless(i + 1), 0u, 0.0f, 0.0f,
                            glm::vec2(1.0f / src.GetWidth(), 1.0f / src.GetHeight()), settings.BloomRadius };
            pass(target.GetBloom(i), true, res.BloomUp, push);
        }
    }

    void SceneRenderer::FlushLines(VkCommandBuffer cmd, std::vector<LineVertex>& lines, const Pipeline& pipeline)
    {
        if (lines.empty()) return;
        TransientAllocation vb = RenderDevice::Get().AllocateTransient(lines.size() * sizeof(LineVertex));
        if (!vb) return;
        std::memcpy(vb.Data, lines.data(), lines.size() * sizeof(LineVertex));
        pipeline.Bind(cmd);
        VkBuffer buffer = vb.Buffer;
        vkCmdBindVertexBuffers(cmd, 0, 1, &buffer, &vb.Offset);
        vkCmdDraw(cmd, (uint32_t)lines.size(), 1, 0, 0);
        m_Stats.DrawCalls++;
    }

    void SceneRenderer::BuildGrid(const CameraData& camera, bool grid2D, std::vector<LineVertex>& out)
    {
        // Grid spacing adapts to zoom so lines stay readable.
        float height = grid2D ? 1.0f / camera.Projection[1][1] : std::abs(camera.Position.y);
        float spacing = std::pow(10.0f, std::floor(std::log10(std::max(height, 1.0f) * 0.5f)));
        spacing = std::max(spacing, 1.0f);
        int half = 60;
        glm::vec2 center = grid2D ? glm::vec2(camera.Position.x, camera.Position.y) : glm::vec2(camera.Position.x, camera.Position.z);
        center = glm::floor(center / spacing) * spacing;
        const glm::vec4 minor{ 0.5f, 0.5f, 0.5f, 0.25f }, major{ 0.6f, 0.6f, 0.6f, 0.5f };

        auto point = [&](float a, float b) { return grid2D ? glm::vec3(a, b, 0.0f) : glm::vec3(a, 0.0f, b); };
        for (int i = -half; i <= half; ++i)
        {
            float o = i * spacing;
            float x = center.x + o, y = center.y + o;
            bool majorX = std::fmod(std::abs(x), spacing * 10.0f) < 0.001f, majorY = std::fmod(std::abs(y), spacing * 10.0f) < 0.001f;
            glm::vec4 cx = std::abs(x) < 0.001f ? glm::vec4(0.3f, 0.45f, 0.9f, 0.8f) : majorX ? major : minor; // line along second axis at x
            glm::vec4 cy = std::abs(y) < 0.001f ? glm::vec4(0.9f, 0.3f, 0.3f, 0.8f) : majorY ? major : minor;
            if (grid2D) cx = std::abs(x) < 0.001f ? glm::vec4(0.3f, 0.85f, 0.3f, 0.8f) : cx;
            out.push_back({ point(x, center.y - half * spacing), cx });
            out.push_back({ point(x, center.y + half * spacing), cx });
            out.push_back({ point(center.x - half * spacing, y), cy });
            out.push_back({ point(center.x + half * spacing, y), cy });
        }
    }

    void SceneRenderer::BuildEditorOverlays(Scene& scene, const SceneRenderOptions& options)
    {
        for (Entity e : options.Selection)
            if (scene.IsValid(e) && e != options.Selected) BuildEntityOverlay(scene, e);
        if (scene.IsValid(options.Selected)) BuildEntityOverlay(scene, options.Selected);
    }

    void SceneRenderer::BuildEntityOverlay(Scene& scene, Entity sel)
    {
        auto& reg = scene.GetRegistry();
        auto& t = reg.Get<TransformComponent>(sel);
        const glm::vec4 orange{ 1.0f, 0.6f, 0.1f, 1.0f }, green{ 0.4f, 1.0f, 0.4f, 1.0f }, yellow{ 1.0f, 0.9f, 0.4f, 0.8f };

        if (auto* mr = reg.TryGet<MeshRendererComponent>(sel))
            if (auto mesh = AssetManager::GetMesh(mr->Mesh))
                DrawWireBox(t.World, mesh->GetBounds().Center(), mesh->GetBounds().Extents(), orange);
        if (auto* sr = reg.TryGet<SpriteRendererComponent>(sel))
        {
            float aspect = 1.0f;
            if (auto tex = AssetManager::GetTexture(sr->Texture))
                aspect = ((float)tex->GetWidth() / std::max(1, sr->SheetColumns)) / ((float)tex->GetHeight() / std::max(1, sr->SheetRows));
            DrawWireBox(t.World, glm::vec3(0), glm::vec3(0.5f * aspect, 0.5f, 0.0f), orange);
        }
        if (auto* cam = reg.TryGet<CameraComponent>(sel))
        {
            CameraData cd = CameraData::FromEntity(scene, sel, 16.0f / 9.0f);
            CameraComponent clamped = *cam;
            clamped.FarClip = std::min(cam->FarClip, cam->NearClip + 50.0f);
            glm::mat4 inv = glm::inverse(clamped.GetProjection(16.0f / 9.0f) * cd.View);
            glm::vec3 c[8];
            for (int i = 0; i < 8; ++i)
            {
                glm::vec4 p = inv * glm::vec4((i & 1) ? 1 : -1, (i & 2) ? 1 : -1, (i & 4) ? 1 : 0, 1);
                c[i] = glm::vec3(p) / p.w;
            }
            const int edges[12][2] = { { 0, 1 }, { 2, 3 }, { 4, 5 }, { 6, 7 }, { 0, 2 }, { 1, 3 }, { 4, 6 }, { 5, 7 }, { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 } };
            for (auto& e : edges) DrawLine(c[e[0]], c[e[1]], { 0.9f, 0.9f, 0.9f, 0.8f });
        }
        if (reg.Has<DirectionalLightComponent>(sel))
        {
            glm::vec3 p = t.GetWorldPosition(), f = t.Forward(), r = t.Right(), u = t.Up();
            for (int i = 0; i < 8; ++i)
            {
                float a = glm::two_pi<float>() * i / 8;
                glm::vec3 o = (r * std::cos(a) + u * std::sin(a)) * 0.4f;
                DrawLine(p + o, p + o + f * 2.0f, yellow);
            }
        }
        if (auto* pl = reg.TryGet<PointLightComponent>(sel))
            DrawWireSphere(glm::translate(glm::mat4(1.0f), t.GetWorldPosition()), glm::vec3(0), pl->Range, yellow);
        if (auto* joint = reg.TryGet<JointComponent>(sel))
        {
            const glm::vec4 cyan{ 0.3f, 0.9f, 1.0f, 1.0f };
            glm::vec3 anchor = glm::vec3(t.World * glm::vec4(joint->Anchor, 1.0f));
            DrawWireSphere(glm::translate(glm::mat4(1.0f), anchor), glm::vec3(0), 0.08f, cyan);
            glm::vec3 other = anchor;
            if (scene.IsValid(joint->ConnectedBody))
            {
                const auto& ot = reg.Get<TransformComponent>(joint->ConnectedBody);
                other = joint->Type == JointType::Distance ? glm::vec3(ot.World * glm::vec4(joint->ConnectedAnchor, 1.0f)) : ot.GetWorldPosition();
            }
            else if (joint->Type == JointType::Distance)
                other = joint->ConnectedAnchor;
            DrawLine(t.GetWorldPosition(), anchor, cyan);
            DrawLine(anchor, other, cyan);
            if (joint->Type == JointType::Hinge)
            {
                glm::vec3 axis = glm::mat3(t.World) * joint->Axis;
                if (glm::length(axis) > 1e-6f) axis = glm::normalize(axis) * 0.6f;
                DrawLine(anchor - axis, anchor + axis, { 1.0f, 0.6f, 0.2f, 1.0f });
            }
        }
        if (auto* joint = reg.TryGet<Joint2DComponent>(sel))
        {
            const glm::vec4 cyan{ 0.3f, 0.9f, 1.0f, 1.0f };
            glm::vec3 anchor = glm::vec3(t.World * glm::vec4(joint->Anchor, 0.0f, 1.0f));
            DrawWireCircle(glm::translate(glm::mat4(1.0f), anchor), glm::vec3(0), 0.08f, cyan, 2);
            glm::vec3 other = anchor;
            if (scene.IsValid(joint->ConnectedBody))
            {
                const auto& ot = reg.Get<TransformComponent>(joint->ConnectedBody);
                other = joint->Type == Joint2DType::Distance ? glm::vec3(ot.World * glm::vec4(joint->ConnectedAnchor, 0.0f, 1.0f)) : ot.GetWorldPosition();
            }
            else if (joint->Type == Joint2DType::Distance)
                other = glm::vec3(joint->ConnectedAnchor, 0.0f);
            DrawLine(t.GetWorldPosition(), anchor, cyan);
            DrawLine(anchor, other, cyan);
        }
        if (auto* cc = reg.TryGet<CharacterControllerComponent>(sel))
        {
            const glm::vec4 green{ 0.4f, 1.0f, 0.4f, 1.0f };
            glm::vec3 center = t.GetWorldPosition() + cc->Center;
            float r = cc->Radius, half = std::max(0.0f, cc->Height * 0.5f - r);
            glm::mat4 top = glm::translate(glm::mat4(1.0f), center + glm::vec3(0, half, 0));
            glm::mat4 bottom = glm::translate(glm::mat4(1.0f), center - glm::vec3(0, half, 0));
            DrawWireCircle(top, glm::vec3(0), r, green, 1);
            DrawWireCircle(bottom, glm::vec3(0), r, green, 1);
            DrawWireCircle(top, glm::vec3(0), r, green, 0);
            DrawWireCircle(top, glm::vec3(0), r, green, 2);
            DrawWireCircle(bottom, glm::vec3(0), r, green, 0);
            DrawWireCircle(bottom, glm::vec3(0), r, green, 2);
            for (glm::vec3 d : { glm::vec3(r, 0, 0), glm::vec3(-r, 0, 0), glm::vec3(0, 0, r), glm::vec3(0, 0, -r) })
                DrawLine(center + glm::vec3(0, half, 0) + d, center - glm::vec3(0, half, 0) + d, green);
        }
        if (auto* mc = reg.TryGet<MeshColliderComponent>(sel))
        {
            auto* mr = reg.TryGet<MeshRendererComponent>(sel);
            AssetHandle h = mc->Mesh.IsValid() ? mc->Mesh : mr ? mr->Mesh : AssetHandle::None();
            if (auto mesh = AssetManager::GetMesh(h))
                DrawWireBox(t.World, mesh->GetBounds().Center(), mesh->GetBounds().Extents(), { 0.4f, 1.0f, 0.4f, 1.0f });
        }
        if (auto* sl = reg.TryGet<SpotLightComponent>(sel))
        {
            // Cone: apex at the light, rim circle at Range along the forward axis.
            glm::vec3 p = t.GetWorldPosition(), f = t.Forward(), r = glm::normalize(t.Right()), up = glm::normalize(t.Up());
            float half = glm::radians(std::clamp(sl->SpotAngle, 1.0f, 179.0f)) * 0.5f;
            float dist = sl->Range * std::cos(half), radius = sl->Range * std::sin(half);
            glm::vec3 c = p + f * dist;
            glm::vec3 prev{};
            for (int i = 0; i <= 32; ++i)
            {
                float a = glm::two_pi<float>() * i / 32;
                glm::vec3 q = c + (r * std::cos(a) + up * std::sin(a)) * radius;
                if (i > 0) DrawLine(prev, q, yellow);
                if (i % 8 == 0 && i < 32) DrawLine(p, q, yellow);
                prev = q;
            }
        }
        if (auto* box = reg.TryGet<BoxCollider2DComponent>(sel))
            DrawWireBox(t.World, glm::vec3(box->Offset, 0), glm::vec3(box->Size * 0.5f, 0), green);
        if (auto* circle = reg.TryGet<CircleCollider2DComponent>(sel))
        {
            float s = std::max(std::abs(t.Scale.x), std::abs(t.Scale.y));
            glm::mat4 m = glm::translate(glm::mat4(1.0f), t.GetWorldPosition()) * glm::rotate(glm::mat4(1.0f), t.Rotation.z, { 0, 0, 1 });
            DrawWireCircle(m, glm::vec3(circle->Offset * glm::vec2(t.Scale), 0), circle->Radius * s, green, 2);
        }
        if (auto* box = reg.TryGet<BoxColliderComponent>(sel)) DrawWireBox(t.World, box->Offset, box->Size * 0.5f, green);
        if (auto* sphere = reg.TryGet<SphereColliderComponent>(sel))
        {
            float s = std::max({ std::abs(t.Scale.x), std::abs(t.Scale.y), std::abs(t.Scale.z) });
            glm::mat4 m = glm::translate(glm::mat4(1.0f), t.GetWorldPosition()) * glm::toMat4(t.GetRotationQuat());
            DrawWireSphere(m, sphere->Offset * t.Scale, sphere->Radius * s, green);
        }
        if (auto* cap = reg.TryGet<CapsuleColliderComponent>(sel))
        {
            float r = cap->Radius * std::max(std::abs(t.Scale.x), std::abs(t.Scale.z));
            float half = std::max(0.0f, cap->Height * 0.5f * std::abs(t.Scale.y) - r);
            glm::mat4 m = glm::translate(glm::mat4(1.0f), t.GetWorldPosition()) * glm::toMat4(t.GetRotationQuat()) *
                          glm::translate(glm::mat4(1.0f), cap->Offset * t.Scale);
            for (float y : { -half, half })
            {
                DrawWireCircle(m, { 0, y, 0 }, r, green, 1);
                DrawWireCircle(m, { 0, y, 0 }, r, green, 0);
                DrawWireCircle(m, { 0, y, 0 }, r, green, 2);
            }
            for (glm::vec3 d : { glm::vec3(r, 0, 0), glm::vec3(-r, 0, 0), glm::vec3(0, 0, r), glm::vec3(0, 0, -r) })
                DrawLine(glm::vec3(m * glm::vec4(d + glm::vec3(0, -half, 0), 1)), glm::vec3(m * glm::vec4(d + glm::vec3(0, half, 0), 1)), green);
        }
        if (auto* ps = reg.TryGet<ParticleSystemComponent>(sel))
        {
            const glm::vec4 cyan{ 0.4f, 0.85f, 1.0f, 0.9f };
            glm::mat4 m = t.World;
            switch (ps->Shape)
            {
            case ParticleShape::Cone:
            {
                float top = ps->ShapeRadius + std::tan(glm::radians(ps->ShapeAngle)) * 1.0f;
                DrawWireCircle(m, glm::vec3(0), ps->ShapeRadius, cyan, 1);
                DrawWireCircle(m, glm::vec3(0, 1, 0), top, cyan, 1);
                for (int i = 0; i < 4; ++i)
                {
                    float a = glm::half_pi<float>() * i;
                    glm::vec3 d(std::cos(a), 0, std::sin(a));
                    DrawLine(glm::vec3(m * glm::vec4(d * ps->ShapeRadius, 1)), glm::vec3(m * glm::vec4(d * top + glm::vec3(0, 1, 0), 1)), cyan);
                }
                break;
            }
            case ParticleShape::Sphere: DrawWireSphere(m, glm::vec3(0), ps->ShapeRadius, cyan); break;
            case ParticleShape::Box: DrawWireBox(m, glm::vec3(0), ps->BoxSize * 0.5f, cyan); break;
            case ParticleShape::Point: DrawWireSphere(m, glm::vec3(0), 0.05f, cyan); break;
            }
        }
        if (auto* src = reg.TryGet<AudioSourceComponent>(sel); src && src->Spatial)
        {
            glm::mat4 m = glm::translate(glm::mat4(1.0f), t.GetWorldPosition());
            DrawWireSphere(m, glm::vec3(0), src->MinDistance, { 0.6f, 0.8f, 1.0f, 0.8f });
            DrawWireSphere(m, glm::vec3(0), src->MaxDistance, { 0.4f, 0.6f, 1.0f, 0.5f });
        }
    }
}
