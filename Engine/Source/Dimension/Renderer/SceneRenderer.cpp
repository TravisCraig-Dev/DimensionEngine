#include "Dimension/Renderer/Quality.h"
#include "Dimension/Renderer/SceneRenderer.h"
#include "Dimension/Renderer/PostProcess.h"
#include "Dimension/Renderer/Texture.h"
#include "Dimension/Renderer/Material.h"
#include "Dimension/Scene/LODGroup.h"
#include "Dimension/Scene/SceneSerializer.h"
#include <glm/gtx/matrix_decompose.hpp>
#include "Dimension/Core/Profiler.h"
#include "Dimension/Navigation/NavMesh.h"
#include "Dimension/Asset/AssetManager.h"
#include "Dimension/Physics/ConvexDecomposition.h"
#include "Dimension/Physics/Cloth.h"
#include "Dimension/Physics/Water.h"
#include "Dimension/Terrain/Terrain.h"
#include "Dimension/Scene/PathPlacer.h"
#include "Dimension/Physics/Rope.h"
#include "Dimension/Scene/Scene.h"
#include "Dimension/Core/Log.h"
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/packing.hpp>
#include "Dimension/Renderer/Environment.h"
#include <algorithm>
#include <cstdint>
#include <chrono>
#include <cstring>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

#include "shaders/Mesh.vert.h"
#include "shaders/Mesh.frag.h"
#include "shaders/MeshWire.frag.h"
#include "shaders/MeshWireOverlay.frag.h"
#include "shaders/MeshOverdraw.frag.h"
#include "shaders/MeshOit.frag.h"
#include "shaders/MeshImpostor.frag.h"
#include "shaders/DepthImpostor.frag.h"
#include "shaders/ShadowImpostor.frag.h"
#include "shaders/OitComposite.frag.h"
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
#include "shaders/FramePreview.frag.h"
#include "Dimension/ImGui/ImGuiLayer.h"
#include "Dimension/Renderer/SpriteSlicing.h"
#include "shaders/BloomDown.frag.h"
#include "shaders/BloomUp.frag.h"
#include "shaders/Fxaa.frag.h"
#include "shaders/Ssao.frag.h"
#include "shaders/SsaoBlur.frag.h"
#include "shaders/DofPrefilter.frag.h"
#include "shaders/DofBlur.frag.h"
#include "shaders/ProbeEquirect.frag.h"
#include "shaders/ProbePrefilter.frag.h"
#include "shaders/LightCull.comp.h"
#include "shaders/BlendShapes.comp.h"
#include "shaders/DepthReduce.comp.h"
#include "shaders/LuminanceReduce.comp.h"
#include "shaders/ParticleSim.comp.h"
#include "shaders/VfxSim.comp.h"
#include "shaders/ParticleSortKeys.comp.h"
#include "shaders/ParticleSort.comp.h"
#include "shaders/ParticleGpu.vert.h"
#include "shaders/ParticleTrail.vert.h"
#include "shaders/Ssgi.frag.h"
#include "shaders/SsgiTemporal.frag.h"
#include "shaders/Ssr.frag.h"
#include "shaders/SsrApply.frag.h"
#include "shaders/Taa.frag.h"
#include "shaders/MotionBlur.frag.h"
#include "shaders/DepthCopy.frag.h"
#include "shaders/Velocity.vert.h"
#include "shaders/Velocity.frag.h"
#include "shaders/VelocityCamera.frag.h"
#include "shaders/VelocityTiles.frag.h"
#include "shaders/VelocitySprite.vert.h"
#include "shaders/VelocitySprite.frag.h"
#include "shaders/VelocityParticleGpu.vert.h"
#include "shaders/Fog.frag.h"
#include "shaders/Underwater.frag.h"
#include "shaders/Decal.vert.h"
#include "shaders/Decal.frag.h"

namespace Dimension
{
    namespace
    {
        constexpr uint32_t kMaxPointLights = LightClusters::kMaxLights;
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
            glm::mat4 InvProj;
            glm::vec4 AOParams; // x enabled, y AO texture index, zw 1 / framebuffer size
            glm::vec4 CascadeDepth;     // world depth range of each cascade
            glm::vec4 SoftShadowParams; // x PCSS on, y tan(light angular radius), z max filter radius in texels
            glm::vec4 SkyFilter;        // x GGX-prefiltered sky texture index (0 = none), y its mip count - 1
            glm::uvec4 LightBuffers;     // xy: PointLightGPU array, zw: cluster cells (buffer addresses)
            glm::uvec4 LightIndexBuffer; // xy: cluster light indices
            glm::vec4 ClusterParams;     // x near, y far, z 1 = orthographic, w log(far / near)
            struct ProbeGPU
            {
                glm::vec4 CenterIntensity, BoxMin, BoxMax, Params; // see ReflectionProbe in Common.glsl
            } Probes[SceneRenderer::kMaxReflectionProbes];
            glm::uvec4 ProbeVolumeBuffer;
            glm::vec4 ProbeVolumes[SceneRenderer::kMaxProbeVolumes * 3];
            glm::vec4 Wind;       // xyz wind (m/s), w seconds since start (grass motion)
            glm::vec4 WindParams; // x gusts, y grass bender count
            glm::vec4 GrassBenders[SceneRenderer::kMaxGrassBenders]; // xyz sphere center, w radius
            glm::vec4 CloudParams;  // x coverage, y density, z base, w top
            glm::vec4 CloudParams2; // x feature size, y steps, z 1 = on, w 1 = shadows
            glm::vec4 Weather;      // x wetness, y snow cover, z rain, w lightning flash
            glm::vec4 WaterMin, WaterMax, WaterFog, WaterParams;
            glm::vec4 Refraction; // x: opaque scene color texture + 1 while transparent meshes draw (0 = none), yz: 1 / size
        };

        glm::uvec2 SplitAddress(VkDeviceAddress a) { return { (uint32_t)a, (uint32_t)(a >> 32) }; }
        // On a sprite vertex's texture index: the scene's lights shade it (lit CPU particles; see Sprite.vert).
        constexpr uint32_t kLitSpriteBit = 0x20000000u;
        static_assert(sizeof(SceneUniforms) <= RenderDevice::kUniformBlockSize);

        struct MeshPush
        {
            glm::mat4 Model;
            VkDeviceAddress Material;
            VkDeviceAddress Skin;
            VkDeviceAddress Joints;
            VkDeviceAddress Instances;
            uint32_t EntityId;
            uint32_t Flags;  // bit 0: skinned, bit 1: instanced, bits 8-15: LOD cross-fade coverage, bit 16: its side
            uint32_t Motion; // grass tufts: half floats (sway, bending); 0 = rigid
        };

        // std430 mirror of VelocityObject in Velocity.vert.
        struct VelocityGPU
        {
            glm::mat4 Model, PrevModel;
            glm::uvec4 Joints; // xy this frame's skinning matrices, zw last frame's
            glm::uvec4 Extra;  // xy last frame's simulated vertex positions (packed xyz); z grass/fur Motion; w FurWind
        };
        // Vertex of the sprites' motion vector pass (VelocitySprite.vert).
        struct VelocitySpriteVertex
        {
            glm::vec3 Position, Previous;
            glm::vec2 UV;
            uint32_t Texture;
            float Alpha;
        };

        // std430 mirror of MeshInstance in MeshCommon.glsl.
        struct InstanceGPU
        {
            glm::mat4 Model;
            glm::uvec4 Data; // x: entity id (low 23 bits) and LOD cross-fade (top 9), yz: skinning matrices address, w: grass motion (see MeshPush)
        };
        constexpr uint32_t kInstanceIdLimit = 1u << 23; // items with larger ids draw on their own


        bool s_InstancingEnabled = true;
        bool s_ClusterCulling = true;
        bool s_GpuLightCulling = true;
        bool s_GpuBlendShapes = true;
        bool s_OcclusionCulling = true;
        uint64_t s_ProbeBakeGeneration = 0;
        constexpr uint32_t kMaxInstancesPerDraw = 16384;

        // std430 mirror of MaterialRef in MeshCommon.glsl.
        struct MaterialGPU
        {
            glm::vec4 BaseColor;
            glm::vec4 Emission;
            glm::vec4 Params;
            glm::vec4 TilingOffset;
            glm::uvec4 Maps;
            glm::uvec4 Flags;
            glm::vec4 Layers[2]; // fur shell values
            glm::uvec4 Maps2;    // terrain: xy its layers' address (TerrainLayerGPU), z their count
        };
        // std430 mirror of TerrainLayer in MeshCommon.glsl: a terrain layer for the shader.
        struct TerrainLayerGPU
        {
            glm::vec4 Color;  // linear rgb, w texture repeats per meter
            glm::uvec4 Maps;  // bindless: x texture, y splat map, w normal map (~0 = none); z its channel in the splat map
            glm::vec4 Params; // x normal strength, y 1 = triplanar
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
            uint32_t DofTexture; // index + 1, 0 = none
            uint32_t AlphaFromHdr; // 1: alpha is the HDR image's (Depth Only cameras), else opaque
            float Saturation, Contrast;
            float FilterR, FilterG, FilterB; // color filter and white balance (linear)
            float LiftR, LiftG, LiftB, GammaR, GammaG, GammaB, GainR, GainG, GainB;
            float ChromaticAberration, FilmGrain, LensDistortion;
            uint32_t Seed; // changes every frame (grain)
            uint32_t LookupTexture; // bindless index + 1, 0 = none
            uint32_t LookupSize;    // slices (= slice side)
            float LookupContribution;
            uint32_t DirtTexture;   // lens dirt: bindless index + 1, 0 = none
            float DirtIntensity;
        };

        struct DofPrefilterPush
        {
            uint32_t Hdr, Depth;
            float ProjZ, ProjW, Focus, Range;
            glm::vec2 FullTexel;
        };

        struct DofBlurPush
        {
            uint32_t Src, Samples;
            glm::vec2 Texel;
            float Radius;
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

        struct SsaoPush
        {
            uint32_t Depth;
            uint32_t Samples;
            glm::vec2 DepthTexel;
            float Radius;
            float Power;
            float Bias;
            float Pad;
        };

        struct SsaoBlurPush
        {
            uint32_t AO;
            uint32_t Depth;
            glm::vec2 AOTexel;
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
            // World-space box given by its center and half extents.
            bool IntersectsWorld(const glm::vec3& c, const glm::vec3& we) const
            {
                for (auto& p : Planes)
                    if (glm::dot(glm::vec3(p), c) + p.w + glm::dot(glm::abs(glm::vec3(p)), we) < 0.0f) return false;
                return true;
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

    // The pipelines that draw into the scene's main pass; MSAA needs a copy of each built for its sample count.
    struct MainPipelines
    {
        enum Kind { Mesh, MeshDoubleSided, MeshTransparent, DepthPrepass, DepthPrepassDoubleSided, Sky, Sprite, SpriteAdditive, Line,
                    GpuParticles, GpuParticlesAdditive, GpuTrails, GpuTrailsAdditive, MeshWire, MeshWireOverlay, MeshOverdraw, MeshImpostor,
                    DepthPrepassImpostor, MeshPrepassed, MeshDoubleSidedPrepassed, Count };
        Pipeline Pipelines[Count];
        const Pipeline& operator[](Kind k) const { return Pipelines[k]; }
    };

    struct Renderer::Resources
    {
        Pipeline MeshPrepassed, MeshDoubleSidedPrepassed; // after the depth prepass: testing its depth, not writing it
        Pipeline Mesh, MeshDoubleSided, MeshTransparent, Shadow, Sky, UI, SpriteAdditive, Sprite, Line, LineOverlay, Composite;
        Pipeline MeshWire, MeshWireOverlay, MeshOverdraw; // Scene view draw modes
        Pipeline MeshImpostor, DepthPrepassImpostor, ShadowImpostor; // impostors: pixels at their depth on the mesh
        // Descriptions of the main-pass pipelines (single-sampled above) and their multisampled builds, made on demand.
        GraphicsPipelineDesc MainDescs[MainPipelines::Count];
        std::map<uint32_t, std::unique_ptr<MainPipelines>> Multisampled;
        Pipeline BloomDown, BloomUp, Fxaa, DepthPrepass, DepthPrepassDoubleSided, Ssao, SsaoBlur, DofPrefilter, DofBlur, ProbeEquirect, ProbePrefilter;
        Pipeline LightCull, ParticleSim, VfxSim, ParticleSortKeys, ParticleSort, DepthReduce, LuminanceReduce, BlendShapes; // compute
        Pipeline GpuParticles, GpuParticlesAdditive, GpuTrails, GpuTrailsAdditive;
        // GPU particle systems' simulation buffers, per (scene, entity); dropped when unused for a while.
        struct GpuParticleState
        {
            Buffer Particles;
            VkDeviceAddress Address = 0;
            Buffer Trails; // recorded trail points (systems with trails)
            VkDeviceAddress TrailAddress = 0;
            Buffer Hits[kFramesInFlight + 1]; // collision decal hits, read back once their frame is done
            VkDeviceAddress HitsAddress[kFramesInFlight + 1] = {};
            uint64_t HitsFrame[kFramesInFlight + 1] = {}, HitsHandled = ~0ull;
            uint32_t Capacity = 0, Head = 0, Generation = 0;
            uint64_t LastFrame = 0;
            Buffer Sort; // (key, slot) pairs for back-to-front drawing
            VkDeviceAddress SortAddress = 0;
            uint32_t SortPadded = 0;
            struct EventBuffer { Buffer Data; VkDeviceAddress Address = 0; };
            std::vector<EventBuffer> VfxEvents; // a Visual Effect's Trigger On Die slots (their child effects read them)
            Buffer VfxHead; // a child effect's ring head (kept on the GPU)
            Buffer VfxInstances; // a mesh output's instances (one per slot)
            VkDeviceAddress VfxInstancesAddress = 0;
            VkDeviceAddress VfxHeadAddress = 0;
        };
        std::unordered_map<uint64_t, GpuParticleState> GpuParticleStates;
        // The terrain's height grid for GPU particle collision (made again when the terrain changes).
        struct
        {
            uint64_t Key = 0;
            Buffer Heights;
            VkDeviceAddress Address = 0;
            uint32_t Samples = 0;
        } ParticleTerrain;
        // Mesh-shaped GPU emitters' spawn points (ParticleSystems::MeshSurfaceSamples), uploaded once per mesh.
        struct MeshSampleBuffer { Buffer Data; VkDeviceAddress Address = 0; const std::vector<glm::vec4>* Source = nullptr; };
        std::unordered_map<uint64_t, MeshSampleBuffer> MeshSampleBuffers;
        Pipeline Ssr, SsrApply, Ssgi, SsgiTemporal, Taa, Fog, Underwater, MotionBlur;
        Pipeline DepthCopy;
        Pipeline Velocity, VelocityCamera, VelocityTiles, VelocitySprite, VelocityParticleGpu; // motion vectors
        Pipeline MeshOit, OitComposite; // order-independent transparency
        Pipeline FramePreview;          // the frame debugger's target pictures
        Pipeline Decals[3]; // by DecalBlend
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
                            { 2, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, UV) },
                            { 3, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Vertex, Tangent) },
                            { 4, VK_FORMAT_R8G8B8A8_UNORM, offsetof(Vertex, Color) } };
        mesh.CullMode = VK_CULL_MODE_BACK_BIT;
        mesh.DepthTest = mesh.DepthWrite = true;
        // HDR color, entity ids and the surface buffer for screen-space reflections (opaque meshes only write it).
        mesh.ColorAttachments = { { F::kHdrFormat }, { F::kIdFormat }, { F::kSurfaceFormat } };
        mesh.DepthFormat = F::kDepthFormat;
        mesh.DebugName = "MeshPipeline";
        r.Mesh = Pipeline(mesh);
        r.MainDescs[MainPipelines::Mesh] = mesh;
        GraphicsPipelineDesc meshTwoSided = mesh;
        meshTwoSided.CullMode = VK_CULL_MODE_NONE;
        meshTwoSided.DebugName = "MeshDoubleSidedPipeline";
        r.MeshDoubleSided = Pipeline(meshTwoSided);
        r.MainDescs[MainPipelines::MeshDoubleSided] = meshTwoSided;
        // Opaque parts whose depth the prepass already wrote: the shader can discard (cutouts), and a pipeline that
        // writes depth from such a shader tests it only after shading, so every hidden fragment would be shaded. Without
        // the write the test comes first and only the visible surface is shaded (the depths match: Mesh.vert is invariant).
        GraphicsPipelineDesc meshPrepassed = mesh;
        meshPrepassed.DepthWrite = false;
        meshPrepassed.DebugName = "MeshPrepassedPipeline";
        r.MeshPrepassed = Pipeline(meshPrepassed);
        r.MainDescs[MainPipelines::MeshPrepassed] = meshPrepassed;
        meshPrepassed.CullMode = VK_CULL_MODE_NONE;
        meshPrepassed.DebugName = "MeshDoubleSidedPrepassedPipeline";
        r.MeshDoubleSidedPrepassed = Pipeline(meshPrepassed);
        r.MainDescs[MainPipelines::MeshDoubleSidedPrepassed] = meshPrepassed;
        GraphicsPipelineDesc meshImpostor = meshTwoSided;
        meshImpostor.FragmentSpirv = kMeshImpostor_frag;
        meshImpostor.DebugName = "MeshImpostorPipeline";
        r.MeshImpostor = Pipeline(meshImpostor);
        r.MainDescs[MainPipelines::MeshImpostor] = meshImpostor;
        GraphicsPipelineDesc meshTransparent = meshTwoSided;
        meshTransparent.DepthWrite = false;
        meshTransparent.ColorAttachments = { { F::kHdrFormat, BlendMode::Alpha }, { F::kIdFormat }, { F::kSurfaceFormat, BlendMode::Opaque, false } };
        meshTransparent.DebugName = "MeshTransparentPipeline";
        r.MeshTransparent = Pipeline(meshTransparent);
        r.MainDescs[MainPipelines::MeshTransparent] = meshTransparent;
        // Scene view draw modes: edges without depth testing (Wireframe), edges pulled toward the camera over the shaded
        // surfaces (Shaded Wireframe), and an additive layer per mesh without depth (Overdraw).
        GraphicsPipelineDesc wire = meshTwoSided;
        wire.FragmentSpirv = kMeshWire_frag;
        wire.PolygonMode = VK_POLYGON_MODE_LINE;
        wire.DepthTest = wire.DepthWrite = false;
        wire.DebugName = "MeshWirePipeline";
        r.MeshWire = Pipeline(wire);
        r.MainDescs[MainPipelines::MeshWire] = wire;
        GraphicsPipelineDesc wireOverlay = wire;
        wireOverlay.FragmentSpirv = kMeshWireOverlay_frag;
        wireOverlay.DepthTest = true; // its shader pulls the edges' depth a little toward the camera
        wireOverlay.ColorAttachments = { { F::kHdrFormat }, { F::kIdFormat, BlendMode::Opaque, false }, { F::kSurfaceFormat, BlendMode::Opaque, false } };
        wireOverlay.DebugName = "MeshWireOverlayPipeline";
        r.MeshWireOverlay = Pipeline(wireOverlay);
        r.MainDescs[MainPipelines::MeshWireOverlay] = wireOverlay;
        GraphicsPipelineDesc overdraw = meshTwoSided;
        overdraw.FragmentSpirv = kMeshOverdraw_frag;
        overdraw.DepthTest = overdraw.DepthWrite = false;
        overdraw.ColorAttachments = { { F::kHdrFormat, BlendMode::Additive }, { F::kIdFormat, BlendMode::Opaque, false },
                                      { F::kSurfaceFormat, BlendMode::Opaque, false } };
        overdraw.DebugName = "MeshOverdrawPipeline";
        r.MeshOverdraw = Pipeline(overdraw);
        r.MainDescs[MainPipelines::MeshOverdraw] = overdraw;
        GraphicsPipelineDesc meshOit = meshTransparent;
        meshOit.FragmentSpirv = kMeshOit_frag;
        meshOit.ColorAttachments = { { F::kOitAccumFormat, BlendMode::Accumulate }, { F::kOitRevealFormat, BlendMode::Revealage } };
        meshOit.DebugName = "MeshOitPipeline";
        r.MeshOit = Pipeline(meshOit);

        GraphicsPipelineDesc shadow = mesh;
        shadow.FragmentSpirv = kShadow_frag;
        shadow.CullMode = VK_CULL_MODE_NONE; // single-sided geometry (planes, quads) must still cast
        shadow.ColorAttachments = {};
        shadow.DepthBias = true;
        shadow.DebugName = "ShadowPipeline";
        r.Shadow = Pipeline(shadow);
        GraphicsPipelineDesc shadowImpostor = shadow;
        shadowImpostor.FragmentSpirv = kShadowImpostor_frag;
        shadowImpostor.DebugName = "ShadowImpostorPipeline";
        r.ShadowImpostor = Pipeline(shadowImpostor);
        // Depth prepass for SSAO: same culling as the main pass so it never hides what the main pass would draw.
        GraphicsPipelineDesc prepass = shadow;
        prepass.CullMode = VK_CULL_MODE_BACK_BIT;
        prepass.DepthBias = false;
        prepass.DebugName = "DepthPrepassPipeline";
        r.DepthPrepass = Pipeline(prepass);
        r.MainDescs[MainPipelines::DepthPrepass] = prepass;
        prepass.CullMode = VK_CULL_MODE_NONE;
        prepass.DebugName = "DepthPrepassDoubleSidedPipeline";
        r.DepthPrepassDoubleSided = Pipeline(prepass);
        r.MainDescs[MainPipelines::DepthPrepassDoubleSided] = prepass;
        prepass.FragmentSpirv = kDepthImpostor_frag;
        prepass.DebugName = "DepthPrepassImpostorPipeline";
        r.DepthPrepassImpostor = Pipeline(prepass);
        r.MainDescs[MainPipelines::DepthPrepassImpostor] = prepass;

        GraphicsPipelineDesc sky;
        sky.VertexSpirv = kSky_vert;
        sky.FragmentSpirv = kSky_frag;
        sky.ColorAttachments = { { F::kHdrFormat }, { F::kIdFormat, BlendMode::Opaque, false }, { F::kSurfaceFormat, BlendMode::Opaque, false } };
        sky.DepthFormat = F::kDepthFormat;
        sky.DebugName = "SkyPipeline";
        r.Sky = Pipeline(sky);
        r.MainDescs[MainPipelines::Sky] = sky;

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
        sprite.ColorAttachments = { { F::kHdrFormat, BlendMode::Alpha }, { F::kIdFormat }, { F::kSurfaceFormat, BlendMode::Opaque, false } };
        sprite.DepthFormat = F::kDepthFormat;
        sprite.DebugName = "SpritePipeline";
        r.Sprite = Pipeline(sprite);
        r.MainDescs[MainPipelines::Sprite] = sprite;
        GraphicsPipelineDesc additive = sprite;
        additive.ColorAttachments = { { F::kHdrFormat, BlendMode::Additive }, { F::kIdFormat }, { F::kSurfaceFormat, BlendMode::Opaque, false } };
        additive.DebugName = "SpriteAdditivePipeline";
        r.SpriteAdditive = Pipeline(additive);
        r.MainDescs[MainPipelines::SpriteAdditive] = additive;
        GraphicsPipelineDesc gpuParticles = sprite;
        gpuParticles.VertexSpirv = kParticleGpu_vert;
        gpuParticles.VertexStride = 0;
        gpuParticles.Attributes = {};
        gpuParticles.DebugName = "GpuParticlesPipeline";
        r.GpuParticles = Pipeline(gpuParticles);
        r.MainDescs[MainPipelines::GpuParticles] = gpuParticles;
        gpuParticles.ColorAttachments = additive.ColorAttachments;
        gpuParticles.DebugName = "GpuParticlesAdditivePipeline";
        r.GpuParticlesAdditive = Pipeline(gpuParticles);
        r.MainDescs[MainPipelines::GpuParticlesAdditive] = gpuParticles;
        GraphicsPipelineDesc gpuTrails = gpuParticles;
        gpuTrails.VertexSpirv = kParticleTrail_vert;
        gpuTrails.ColorAttachments = sprite.ColorAttachments;
        gpuTrails.DebugName = "GpuTrailsPipeline";
        r.GpuTrails = Pipeline(gpuTrails);
        r.MainDescs[MainPipelines::GpuTrails] = gpuTrails;
        gpuTrails.ColorAttachments = additive.ColorAttachments;
        gpuTrails.DebugName = "GpuTrailsAdditivePipeline";
        r.GpuTrailsAdditive = Pipeline(gpuTrails);
        r.MainDescs[MainPipelines::GpuTrailsAdditive] = gpuTrails;

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
        line.ColorAttachments = { { F::kHdrFormat, BlendMode::Alpha }, { F::kIdFormat, BlendMode::Opaque, false },
                                  { F::kSurfaceFormat, BlendMode::Opaque, false } };
        line.DepthFormat = F::kDepthFormat;
        line.DebugName = "LinePipeline";
        r.Line = Pipeline(line);
        r.MainDescs[MainPipelines::Line] = line;
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
        post.FragmentSpirv = kProbeEquirect_frag;
        post.ColorAttachments = { { F::kHdrFormat } };
        post.DebugName = "ProbeEquirectPipeline";
        r.ProbeEquirect = Pipeline(post);
        post.FragmentSpirv = kProbePrefilter_frag;
        post.DebugName = "ProbePrefilterPipeline";
        r.ProbePrefilter = Pipeline(post);
        post.FragmentSpirv = kDofPrefilter_frag;
        post.ColorAttachments = { { F::kHdrFormat } };
        post.DebugName = "DofPrefilterPipeline";
        r.DofPrefilter = Pipeline(post);
        post.FragmentSpirv = kDofBlur_frag;
        post.DebugName = "DofBlurPipeline";
        r.DofBlur = Pipeline(post);
        post.FragmentSpirv = kSsao_frag;
        post.ColorAttachments = { { F::kAOFormat } };
        post.DebugName = "SsaoPipeline";
        r.Ssao = Pipeline(post);
        post.FragmentSpirv = kSsaoBlur_frag;
        post.DebugName = "SsaoBlurPipeline";
        r.SsaoBlur = Pipeline(post);
        post.FragmentSpirv = kSsr_frag;
        post.ColorAttachments = { { F::kHdrFormat } };
        post.DebugName = "SsrPipeline";
        r.Ssr = Pipeline(post);
        post.FragmentSpirv = kSsgi_frag;
        post.DebugName = "SsgiPipeline";
        r.Ssgi = Pipeline(post);
        post.FragmentSpirv = kSsgiTemporal_frag;
        post.DebugName = "SsgiTemporalPipeline";
        r.SsgiTemporal = Pipeline(post);
        post.FragmentSpirv = kSsrApply_frag;
        post.ColorAttachments = { { F::kHdrFormat, BlendMode::Additive } };
        post.DebugName = "SsrApplyPipeline";
        r.SsrApply = Pipeline(post);
        post.FragmentSpirv = kTaa_frag;
        post.ColorAttachments = { { F::kHdrFormat } };
        post.DebugName = "TaaPipeline";
        r.Taa = Pipeline(post);
        post.FragmentSpirv = kMotionBlur_frag;
        post.DebugName = "MotionBlurPipeline";
        r.MotionBlur = Pipeline(post);
        post.ColorAttachments = { { VK_FORMAT_R32_SFLOAT } };
        post.FragmentSpirv = kDepthCopy_frag;
        post.DebugName = "DepthCopyPipeline";
        r.DepthCopy = Pipeline(post);
        post.ColorAttachments = { { F::kVelocityFormat } };
        post.FragmentSpirv = kVelocityCamera_frag;
        post.DebugName = "VelocityCameraPipeline";
        r.VelocityCamera = Pipeline(post);
        post.FragmentSpirv = kVelocityTiles_frag;
        post.DebugName = "VelocityTilesPipeline";
        r.VelocityTiles = Pipeline(post);
        {
            GraphicsPipelineDesc velocity;
            velocity.VertexSpirv = kVelocity_vert;
            velocity.FragmentSpirv = kVelocity_frag;
            velocity.VertexStride = sizeof(Vertex);
            velocity.Attributes = { { 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, Position) },
                                    { 1, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, Normal) } };
            velocity.ColorAttachments = { { F::kVelocityFormat } };
            velocity.DebugName = "VelocityPipeline";
            r.Velocity = Pipeline(velocity);
            using SV = VelocitySpriteVertex;
            velocity.VertexSpirv = kVelocitySprite_vert;
            velocity.FragmentSpirv = kVelocitySprite_frag;
            velocity.VertexStride = sizeof(SV);
            velocity.Attributes = { { 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(SV, Position) },
                                    { 1, VK_FORMAT_R32G32B32_SFLOAT, offsetof(SV, Previous) },
                                    { 2, VK_FORMAT_R32G32_SFLOAT, offsetof(SV, UV) },
                                    { 3, VK_FORMAT_R32_UINT, offsetof(SV, Texture) },
                                    { 4, VK_FORMAT_R32_SFLOAT, offsetof(SV, Alpha) } };
            velocity.DebugName = "VelocitySpritePipeline";
            r.VelocitySprite = Pipeline(velocity);
            velocity.VertexSpirv = kVelocityParticleGpu_vert;
            velocity.VertexStride = 0;
            velocity.Attributes = {};
            velocity.DebugName = "VelocityParticleGpuPipeline";
            r.VelocityParticleGpu = Pipeline(velocity);
        }
        post.FragmentSpirv = kFog_frag;
        post.ColorAttachments = { { F::kHdrFormat, BlendMode::Premultiplied } };
        post.DebugName = "FogPipeline";
        r.Fog = Pipeline(post);
        post.FragmentSpirv = kUnderwater_frag;
        post.DebugName = "UnderwaterPipeline";
        r.Underwater = Pipeline(post);
        post.FragmentSpirv = kOitComposite_frag;
        post.DebugName = "OitCompositePipeline";
        r.OitComposite = Pipeline(post);
        post.FragmentSpirv = kFramePreview_frag;
        post.ColorAttachments = { { SceneRenderTarget::kOutputFormat } };
        post.DebugName = "FramePreviewPipeline";
        r.FramePreview = Pipeline(post);
        {
            GraphicsPipelineDesc decal;
            decal.VertexSpirv = kDecal_vert;
            decal.FragmentSpirv = kDecal_frag;
            decal.VertexStride = sizeof(Vertex);
            decal.Attributes = { { 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, Position) } };
            const BlendMode blends[3] = { BlendMode::Alpha, BlendMode::Multiply, BlendMode::Additive };
            const char* names[3] = { "DecalAlphaPipeline", "DecalMultiplyPipeline", "DecalAdditivePipeline" };
            for (int i = 0; i < 3; ++i)
            {
                decal.ColorAttachments = { { F::kHdrFormat, blends[i] } };
                decal.DebugName = names[i];
                r.Decals[i] = Pipeline(decal);
            }
        }
        r.LightCull = Pipeline::Compute(kLightCull_comp, "LightCullPipeline");
        r.BlendShapes = Pipeline::Compute(kBlendShapes_comp, "BlendShapesPipeline");
        r.DepthReduce = Pipeline::Compute(kDepthReduce_comp, "DepthReducePipeline");
        r.LuminanceReduce = Pipeline::Compute(kLuminanceReduce_comp, "LuminanceReducePipeline");
        r.ParticleSim = Pipeline::Compute(kParticleSim_comp, "ParticleSimPipeline");
        r.VfxSim = Pipeline::Compute(kVfxSim_comp, "VfxSimPipeline");
        r.ParticleSortKeys = Pipeline::Compute(kParticleSortKeys_comp, "ParticleSortKeysPipeline");
        r.ParticleSort = Pipeline::Compute(kParticleSort_comp, "ParticleSortPipeline");

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
        data.CullingMask = (uint32_t)cam.CullingMask;
        data.Transparent = cam.Clear == ClearMode::DepthOnly;
        if (data.Transparent) data.ClearColor = glm::vec4(0.0f);
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

    void SceneRenderer::DrawSliderGizmo(const glm::vec3& anchor, glm::vec3 axis, bool limits, float min, float max)
    {
        if (glm::length(axis) < 1e-6f) return;
        axis = glm::normalize(axis);
        const glm::vec4 orange{ 1.0f, 0.6f, 0.2f, 1.0f }, dim{ 1.0f, 0.6f, 0.2f, 0.35f };
        if (!limits)
        {
            DrawLine(anchor - axis * 1.5f, anchor + axis * 1.5f, dim);
            return;
        }
        glm::vec3 a = anchor + axis * std::min(min, 0.0f), b = anchor + axis * std::max(max, 0.0f);
        DrawLine(a, b, orange);
        glm::vec3 side = glm::normalize(glm::cross(axis, std::abs(axis.y) < 0.9f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0))) * 0.12f;
        DrawLine(a - side, a + side, orange);
        DrawLine(b - side, b + side, orange);
    }

    void SceneRenderer::DrawWireSphere(const glm::mat4& m, const glm::vec3& c, float r, const glm::vec4& color)
    {
        for (int axis = 0; axis < 3; ++axis) DrawWireCircle(m, c, r, color, axis);
    }

    // ---- Rendering ------------------------------------------------------------------------

    namespace
    {
        float Halton(uint32_t index, uint32_t base)
        {
            float f = 1.0f, r = 0.0f;
            for (uint32_t i = index; i > 0; i /= base)
            {
                f /= (float)base;
                r += f * (float)(i % base);
            }
            return r;
        }
    }

    bool SceneRenderer::LayerCulled(Scene& scene, Entity e) const
    {
        if (m_CullingMask == ~0u) return false;
        auto* tag = scene.GetRegistry().TryGet<TagComponent>(e);
        int layer = tag ? tag->Layer : 0;
        return layer >= 0 && layer < 32 && !(m_CullingMask & (1u << layer));
    }

    void SceneRenderer::Render(VkCommandBuffer cmd, Scene& scene, const CameraData& view, SceneRenderTarget& target,
                               const SceneRenderOptions& options)
    {
        m_CullingMask = view.CullingMask;
        // Reflection probes capture the scene with nested Render calls, so they run before this view collects anything
        // (and light probe volumes bake through the same captures).
        if (!options.ProbeCapture)
        {
            UpdateReflectionProbes(cmd, scene);
            UpdateLightProbes(cmd, scene);
            PrepareImpostors(cmd, scene); // (also nested Render calls)
        }
        // The Scene view's draw mode and visibility (probe captures always see the scene as it is).
        m_DrawMode = options.ProbeCapture ? SceneDrawMode::Shaded : options.DrawMode;
        m_HiddenIds.clear();
        m_UnpickableIds.clear();
        if (!options.ProbeCapture)
        {
            for (Entity e : options.Hidden) m_HiddenIds.insert(e.Index + 1);
            for (Entity e : options.Unpickable)
                if (std::find(options.Selection.begin(), options.Selection.end(), e) == options.Selection.end()) m_UnpickableIds.insert(e.Index + 1);
        }
        // Frame debugger: this view's draws from here (the nested captures above are not part of its frame).
        m_FrameCapturing = !options.ProbeCapture && options.FrameDebugStop > -2;
        if (m_FrameCapturing)
        {
            m_FrameEvents.clear();
            m_FrameMaterials.clear();
            m_FrameStop = options.FrameDebugStop;
        }
        // Temporal anti-aliasing renders the game view with a sub-pixel offset that cycles through a Halton sequence;
        // the Scene view and probe faces are never jittered.
        CameraData camera = view;
        glm::mat4 unjitteredViewProj = view.Projection * view.View;
        bool taa = Quality::Apply(scene.GetSettings()).TAA && !options.ProbeCapture && !options.EditorOverlays;
        // A cut to another camera: the previous frames do not belong to this view (they would smear into it).
        if (!options.ProbeCapture && target.Temporal.CameraCuts != scene.GetCameraCuts())
        {
            target.Temporal.CameraCuts = scene.GetCameraCuts();
            target.Temporal.Valid = false;
            target.Gi.Valid = false;
            target.Motion.Valid = false;
        }
        if (taa)
        {
            uint32_t i = ++target.Temporal.Frame % 8 + 1;
            glm::vec2 jitter((Halton(i, 2) - 0.5f) * 2.0f / (float)target.GetWidth(), (Halton(i, 3) - 0.5f) * 2.0f / (float)target.GetHeight());
            camera.Projection = glm::translate(glm::mat4(1.0f), glm::vec3(jitter, 0.0f)) * camera.Projection;
        }
        else
            target.Temporal.Valid = false;
        // MSAA for this view (probe faces stay single-sampled).
        target.SetSamples(options.ProbeCapture ? 1u : (uint32_t)std::clamp(Quality::Apply(scene.GetSettings()).MSAA, 1, 8));
        m_Samples = target.GetSamples();
        m_Capturing = options.ProbeCapture;
        auto cpuStart = std::chrono::steady_clock::now();
        auto& device = RenderDevice::Get();
        auto& res = Renderer::Get();
        m_Stats = {};
        const SceneSettings settings = Quality::Apply(scene.GetSettings()); // (the quality level's caps)
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
                if (const FilteredSky* filtered = FilterSky(cmd, sky))
                    u.SkyFilter = glm::vec4((float)filtered->Bindless, (float)(filtered->Filtered.GetMipLevels() - 1), 0.0f, 0.0f);
                if (const auto& sh = sky->GetIrradianceSH())
                {
                    u.EnvParams.z = 1.0f;
                    for (int i = 0; i < 9; ++i) u.IrradianceSH[i] = glm::vec4((*sh)[i], 0.0f);
                }
            }
        bool castShadows = false, debugCascades = false, softShadows = false;
        float shadowDistance = 40.0f, shadowBias = 0.0015f, lightAngle = 1.0f;
        int cascadeCount = 4;
        for (auto [e, light, t] : reg.ViewOf<DirectionalLightComponent, TransformComponent>())
        {
            if (!t.ActiveInHierarchy) continue;
            u.LightDirection = glm::vec4(t.Forward(), 1.0f);
            u.LightColor = glm::vec4(ToLinear(light.Color) * light.Intensity, 1.0f);
            castShadows = light.CastShadows;
            shadowDistance = light.ShadowDistance * std::clamp(Quality::Active().ShadowDistance, 0.1f, 1.0f);
            shadowBias = light.ShadowBias;
            cascadeCount = std::clamp(light.ShadowCascades, 1, ShadowCascades::kMax);
            debugCascades = light.DebugCascades;
            softShadows = light.SoftShadows;
            lightAngle = light.LightAngle;
            break;
        }
        if (options.DefaultLight && u.LightDirection.w == 0.0f)
        {
            // Neutral studio light for scenes without a sun (Prefab Mode).
            u.LightDirection = glm::vec4(glm::normalize(glm::vec3(-0.45f, -0.8f, -0.4f)), 1.0f);
            u.LightColor = glm::vec4(glm::vec3(2.2f), 1.0f);
            castShadows = true;
        }
        std::vector<PointLightGPU> lights;
        uint32_t pointShadows = 0;
        uint32_t shadowFaces[kMaxPointShadows] = {};
        const float shadowNear = 0.05f;
        for (auto [e, light, t] : reg.ViewOf<PointLightComponent, TransformComponent>())
        {
            if (!t.ActiveInHierarchy || lights.size() >= kMaxPointLights) continue;
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
            // Cookie: bindless index + 1 in Direction.x, and the world-to-light rotation as a quaternion in Cone.
            float cookie = 0.0f;
            glm::quat toLight(1.0f, 0.0f, 0.0f, 0.0f);
            if (light.Cookie)
                if (auto tex = AssetManager::GetTexture(light.Cookie))
                {
                    cookie = (float)tex->GetBindlessIndex() + 1.0f;
                    toLight = glm::quat_cast(glm::transpose(glm::mat3(t.Right(), t.Up(), -t.Forward())));
                }
            lights.push_back({ glm::vec4(pos, light.Range), glm::vec4(ToLinear(light.Color) * light.Intensity, slot),
                               glm::vec4(cookie, 0.0f, 0.0f, -2.0f), glm::vec4(toLight.x, toLight.y, toLight.z, toLight.w) });
        }
        for (auto [e, light, t] : reg.ViewOf<SpotLightComponent, TransformComponent>())
        {
            if (!t.ActiveInHierarchy || lights.size() >= kMaxPointLights) continue;
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
            // Cookie: bindless index + 1, and the light's roll around its axis relative to the basis the shader builds.
            float cookie = 0.0f, roll = 0.0f;
            if (light.Cookie)
                if (auto tex = AssetManager::GetTexture(light.Cookie))
                {
                    cookie = (float)tex->GetBindlessIndex() + 1.0f;
                    glm::vec3 right0, up0;
                    SpotCookieBasis(dir, right0, up0);
                    glm::vec3 right = t.Right();
                    roll = std::atan2(glm::dot(right, up0), glm::dot(right, right0));
                }
            lights.push_back({ glm::vec4(pos, light.Range), glm::vec4(ToLinear(light.Color) * light.Intensity, slot),
                               glm::vec4(dir, cosOuter), glm::vec4(cosInner, cookie, roll, 0.0f) });
        }
        // Particle lights: unshadowed point lights on the particles that carry one.
        for (auto [e, ps, t] : reg.ViewOf<ParticleSystemComponent, TransformComponent>())
        {
            if (!t.ActiveInHierarchy || !ps.Lights || ps.SimulateOnGpu || ps.MaxLights <= 0 || ps.LightIntensity <= 0.0f) continue;
            bool local = ps.SimulationSpace == ParticleSpace::Local;
            float worldScale = local ? glm::length(glm::vec3(t.World[0])) : 1.0f;
            glm::vec3 tint = ToLinear(glm::vec3(ps.LightColor));
            int count = 0;
            for (const auto& p : ps.Particles)
            {
                if (count >= ps.MaxLights || lights.size() >= kMaxPointLights) break;
                if (!ps.CarriesLight(p)) continue;
                float life = std::clamp(p.Age / std::max(p.Lifetime, 1e-4f), 0.0f, 1.0f);
                glm::vec4 color = p.Color * ps.ColorOverLifetime.Evaluate(life);
                float strength = ps.LightIntensity * color.a * ps.LightColor.a;
                if (strength <= 1e-3f) continue;
                float range = ps.LightRange * (ps.SizeAffectsRange ? p.Size * ps.SizeOverLifetime.Evaluate(life) * worldScale : 1.0f);
                if (range <= 0.01f) continue;
                glm::vec3 rgb = tint * (ps.UseParticleColor ? ToLinear(glm::vec3(color)) : glm::vec3(1.0f)) * strength;
                glm::vec3 pos = local ? glm::vec3(t.World * glm::vec4(p.Position, 1.0f)) : p.Position;
                lights.push_back({ glm::vec4(pos, range), glm::vec4(rgb, 0.0f), glm::vec4(0.0f, 0.0f, 0.0f, -2.0f), glm::vec4(0.0f, 0.0f, 0.0f, 1.0f) });
                ++count;
            }
        }

        // Clustered lights: the light array, each cluster's (offset, count) and the light index list. A compute
        // shader fills the clusters by default (into this view's buffers); otherwise they are built here and go in
        // the transient ring.
        u.InvProj = glm::inverse(u.Proj);
        // Baked light probe volumes: their SH in the transient ring, their boxes in the uniforms.
        {
            std::vector<glm::vec4> sh;
            uint32_t volumes = 0;
            for (auto [e, lpv, t] : reg.ViewOf<LightProbeVolumeComponent, TransformComponent>())
            {
                if (!t.ActiveInHierarchy || !lpv.IsBaked() || volumes >= kMaxProbeVolumes) continue;
                glm::vec3 center = t.GetWorldPosition(), half = glm::abs(lpv.Size) * 0.5f;
                u.ProbeVolumes[volumes * 3] = glm::vec4(center - half, (float)std::max(lpv.ProbesX, 1));
                u.ProbeVolumes[volumes * 3 + 1] = glm::vec4(center + half, (float)std::max(lpv.ProbesY, 1));
                u.ProbeVolumes[volumes * 3 + 2] = glm::vec4((float)std::max(lpv.ProbesZ, 1), (float)(sh.size() / 9), std::max(lpv.BlendDistance, 0.0f), lpv.Intensity);
                for (const glm::vec3& c : lpv.Baked) sh.push_back(glm::vec4(c, 0.0f));
                ++volumes;
            }
            if (volumes > 0)
                if (TransientAllocation block = device.AllocateTransient(sh.size() * sizeof(glm::vec4), 16))
                {
                    std::memcpy(block.Data, sh.data(), sh.size() * sizeof(glm::vec4));
                    glm::uvec2 a = SplitAddress(block.Address);
                    u.ProbeVolumeBuffer = glm::uvec4(a, volumes, 0u);
                }
        }
        // Wind and the bodies that push grass aside: characters and dynamic rigid bodies near the camera, each as a
        // sphere at its lowest part (a capsule's bottom cap, a sphere, a box's inscribed sphere at its bottom).
        {
            static const auto start = std::chrono::steady_clock::now();
            u.Wind = glm::vec4(settings.Wind, std::chrono::duration<float>(std::chrono::steady_clock::now() - start).count());
            struct Bender
            {
                glm::vec4 Sphere;
                float Distance;
            };
            std::vector<Bender> benders;
            auto add = [&](const TransformComponent& t, glm::vec3 offset, float radius, float halfHeight) {
                glm::vec3 scale(glm::length(glm::vec3(t.World[0])), glm::length(glm::vec3(t.World[1])), glm::length(glm::vec3(t.World[2])));
                radius *= std::max(std::abs(scale.x), std::abs(scale.z));
                glm::vec3 center = glm::vec3(t.World * glm::vec4(offset, 1.0f)) - glm::vec3(0.0f, std::max(halfHeight * std::abs(scale.y) - radius, 0.0f), 0.0f);
                if (radius > 0.0f) benders.push_back({ glm::vec4(center, radius), glm::distance(center, camera.Position) });
            };
            for (auto [e, cc, t] : reg.ViewOf<CharacterControllerComponent, TransformComponent>())
                if (t.ActiveInHierarchy) add(t, cc.Center, cc.Radius, cc.Height * 0.5f);
            for (auto [e, rb, t] : reg.ViewOf<RigidbodyComponent, TransformComponent>())
            {
                if (!t.ActiveInHierarchy || rb.Type != BodyType::Dynamic || reg.Has<CharacterControllerComponent>(e)) continue;
                if (auto* s = reg.TryGet<SphereColliderComponent>(e)) add(t, s->Offset, s->Radius, 0.0f);
                else if (auto* c = reg.TryGet<CapsuleColliderComponent>(e)) add(t, c->Offset, c->Radius, c->Height * 0.5f);
                else if (auto* b = reg.TryGet<BoxColliderComponent>(e)) add(t, b->Offset, std::min(b->Size.x, b->Size.z) * 0.5f, b->Size.y * 0.5f);
            }
            if (benders.size() > kMaxGrassBenders)
                std::partial_sort(benders.begin(), benders.begin() + kMaxGrassBenders, benders.end(), [](const Bender& a, const Bender& b) { return a.Distance < b.Distance; });
            size_t count = std::min<size_t>(benders.size(), kMaxGrassBenders);
            for (size_t i = 0; i < count; ++i) u.GrassBenders[i] = benders[i].Sphere;
            u.WindParams = glm::vec4(std::max(settings.WindGusts, 0.0f), (float)count, 0.0f, 0.0f);
        }
        if (settings.Clouds)
        {
            float bottom = settings.CloudBase, top = std::max(settings.CloudTop, settings.CloudBase + 1.0f);
            u.CloudParams = glm::vec4(std::clamp(settings.CloudCoverage, 0.0f, 1.0f), std::max(settings.CloudDensity, 0.0f), bottom, top);
            u.CloudParams2 = glm::vec4(std::max(settings.CloudScale, 1.0f), (float)std::clamp(settings.CloudSteps, 8, 128), 1.0f, settings.CloudShadows ? 1.0f : 0.0f);
        }
        // Weather: the first active Weather component's wet and snowy surfaces.
        for (auto [we, w, wt] : reg.ViewOf<WeatherComponent, TransformComponent>())
            if (wt.ActiveInHierarchy)
            {
                u.Weather = glm::vec4(std::clamp(w.Wetness, 0.0f, 1.0f), std::clamp(w.SnowCover, 0.0f, 1.0f), std::clamp(w.Rain, 0.0f, 1.0f), w.Flash);
                // A lightning flash lights everything from the sky.
                u.Ambient += glm::vec4(glm::vec3(0.55f, 0.6f, 0.85f) * (w.Flash * 2.5f), 0.0f);
                break;
            }
        // Water: the Water Volume the camera is over (or in), else the nearest one, for caustics and underwater murk.
        // Its surface is the top face, or the waves over the camera while it is over the water.
        bool underwaterFog = false;
        {
            float bestScore = 1e30f;
            for (auto [we, water, wt] : reg.ViewOf<WaterVolumeComponent, TransformComponent>())
            {
                if (!wt.ActiveInHierarchy) continue;
                glm::vec3 center = wt.GetWorldPosition(), scale = glm::max(glm::abs(wt.Scale), glm::vec3(1e-4f));
                glm::vec3 lo, hi;
                Water::Bounds(water, center, scale, lo, hi);
                glm::vec3 nearest = glm::clamp(camera.Position, lo, hi);
                bool over = camera.Position.x >= lo.x && camera.Position.x <= hi.x && camera.Position.z >= lo.z && camera.Position.z <= hi.z;
                float score = over ? 0.0f : glm::distance(nearest, camera.Position) + 1.0f;
                if (score >= bestScore) continue;
                bestScore = score;
                if (over && Water::HasWaves(water))
                {
                    glm::vec2 local((camera.Position.x - center.x) / scale.x, (camera.Position.z - center.z) / scale.z);
                    hi.y = center.y + Water::Sample(water, local, scene.GetDisplayTime()).Height * scale.y;
                }
                u.WaterMin = glm::vec4(lo, 1.0f);
                u.WaterMax = glm::vec4(hi, std::max(water.Caustics, 0.0f));
                u.WaterFog = glm::vec4(ToLinear(glm::vec3(water.UnderwaterColor)), std::max(water.UnderwaterFog, 0.0f));
                u.WaterParams = glm::vec4(std::max(water.CausticsScale, 0.1f), 0.0f, 0.0f, 0.0f);
            }
            underwaterFog = u.WaterMin.w > 0.0f && u.WaterFog.a > 0.0f && !camera.Orthographic && !options.ProbeCapture;
        }
        bool gpuCulled = false;
        if (!lights.empty() && s_GpuLightCulling && s_ClusterCulling)
        {
            TransientAllocation lightBlock = device.AllocateTransient(lights.size() * sizeof(PointLightGPU), 16);
            if (lightBlock)
            {
                std::memcpy(lightBlock.Data, lights.data(), lights.size() * sizeof(PointLightGPU));
                m_Clusters.SetPlanes(camera.Projection);
                glm::uvec2 a = SplitAddress(lightBlock.Address);
                u.LightBuffers = glm::uvec4(a, 0u, 0u);
                u.Counts.x = (int)lights.size();
                u.ClusterParams = glm::vec4(m_Clusters.Near, m_Clusters.Far, m_Clusters.Orthographic ? 1.0f : 0.0f, std::log(m_Clusters.Far / m_Clusters.Near));
                gpuCulled = CullLightsOnGpu(cmd, &u, sizeof(u)); // sets the cluster buffer addresses in u first
                if (gpuCulled)
                {
                    u.LightBuffers = glm::uvec4(a, SplitAddress(m_GpuClusterCellsAddress));
                    u.LightIndexBuffer = glm::uvec4(SplitAddress(m_GpuClusterIndicesAddress), 0u, 0u);
                    m_Stats.Lights = (uint32_t)lights.size();
                    m_Stats.ClusterLightRefs = 0; // not read back
                }
                else
                    u.Counts.x = 0;
            }
        }
        if (!lights.empty() && !gpuCulled)
        {
            std::vector<glm::vec4> spheres(lights.size());
            for (size_t i = 0; i < lights.size(); ++i) spheres[i] = lights[i].PositionRange;
            m_Clusters.Build(spheres, camera.View, camera.Projection, s_ClusterCulling);
            TransientAllocation lightBlock = device.AllocateTransient(lights.size() * sizeof(PointLightGPU), 16);
            TransientAllocation cellBlock = device.AllocateTransient(m_Clusters.Cells.size() * sizeof(glm::uvec2), 16);
            TransientAllocation indexBlock = device.AllocateTransient(std::max<size_t>(m_Clusters.Indices.size(), 1) * sizeof(uint32_t), 16);
            if (lightBlock && cellBlock && indexBlock)
            {
                std::memcpy(lightBlock.Data, lights.data(), lights.size() * sizeof(PointLightGPU));
                std::memcpy(cellBlock.Data, m_Clusters.Cells.data(), m_Clusters.Cells.size() * sizeof(glm::uvec2));
                if (!m_Clusters.Indices.empty()) std::memcpy(indexBlock.Data, m_Clusters.Indices.data(), m_Clusters.Indices.size() * sizeof(uint32_t));
                glm::uvec2 a = SplitAddress(lightBlock.Address), b = SplitAddress(cellBlock.Address), c = SplitAddress(indexBlock.Address);
                u.LightBuffers = glm::uvec4(a, b);
                u.LightIndexBuffer = glm::uvec4(c, 0u, 0u);
                u.ClusterParams = glm::vec4(m_Clusters.Near, m_Clusters.Far, m_Clusters.Orthographic ? 1.0f : 0.0f, std::log(m_Clusters.Far / m_Clusters.Near));
                u.Counts.x = (int)lights.size();
                m_Stats.Lights = (uint32_t)lights.size();
                m_Stats.ClusterLightRefs = (uint32_t)m_Clusters.Indices.size();
            }
        }

        // GPU particles: simulate (once per frame, whichever view renders first) and list their draws.
        SimulateGpuParticles(cmd, scene, target, camera.View);

        // World Space canvases into their images (shown as quads by CollectSprites).
        if (!options.ProbeCapture) RenderWorldCanvases(cmd, scene);
        // Prepare CPU-side draw lists (the camera's layers: nested probe captures above drew everything)
        m_CullingMask = view.CullingMask;
        CollectSprites(scene, camera);
        CollectTilemaps(scene, camera);
        CollectText(scene, camera);
        CollectParticles(scene, camera);
        CollectLines(scene, camera);
        {
            DIM_PROFILE_SCOPE("Render List");
            BuildRenderList(scene, camera, options); // shared by the shadow, depth and main passes below
        }
        Terrain::FlushUploads(cmd); // terrains edited in place (craters): their changed patches, before any pass draws
        if (!m_BlendJobs.empty())
        {
            // Blend shapes into this list's vertex streams, before any pass draws them.
            Renderer::Get().BlendShapes.Bind(cmd);
            device.BindBindless(cmd, VK_PIPELINE_BIND_POINT_COMPUTE);
            for (const BlendJob& job : m_BlendJobs)
            {
                struct { glm::uvec2 Base, Delta, Out, List; uint32_t VertexCount, ShapeCount; } push{
                    SplitAddress(job.Base), SplitAddress(job.Deltas), SplitAddress(job.Out), SplitAddress(job.List), job.VertexCount, job.ShapeCount };
                vkCmdPushConstants(cmd, device.GetPipelineLayout(), VK_SHADER_STAGE_ALL, 0, sizeof(push), &push);
                vkCmdDispatch(cmd, (job.VertexCount + 63) / 64, 1, 1);
            }
            Vk::MemoryBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT,
                              VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT);
            m_BlendJobs.clear();
        }
        // Occlusion culling: render items hidden behind what an earlier frame drew.
        bool occlusion = s_OcclusionCulling && scene.GetSettings().OcclusionCulling && !options.ProbeCapture;
        m_ItemOccluded.assign(m_RenderItems.size(), 0);
        if (occlusion)
        {
            ChooseOccluder(camera);
            if (m_Occluder.IsValid())
                for (size_t i = 0; i < m_RenderItems.size(); ++i)
                    if (!m_RenderItems[i].AlwaysVisible) m_ItemOccluded[i] = m_Occluder.Hides(m_RenderItems[i].Center, m_RenderItems[i].Extents);
        }
        if (options.EditorOverlays)
        {
            CollectEditorIcons(scene, camera);
            BuildEditorOverlays(scene, options);
            if (options.Grid)
            {
                GridSettings grid = options.GridConfig;
                if (options.Grid2D) grid.Plane = GridPlane::XY, grid.Offset = 0.0f;
                m_GridSpacing = BuildGrid(camera, grid, m_GridLines);
                m_GridPlane = grid.Plane;
            }
        }

        int shadowTimer = device.BeginGpuTimer(cmd, "Shadows");
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
                u.CascadeDepth[i] = cascades.DepthRange[i];
            }
            u.ShadowParams = glm::vec4((float)cascades.Count, (float)m_ShadowBindless, shadowBias, 0.0f);
            u.CascadeParams = glm::vec4(0.0f, 0.15f, debugCascades ? 1.0f : 0.0f, 0.0f);
            if (softShadows)
                u.SoftShadowParams = glm::vec4(1.0f, std::tan(glm::radians(std::clamp(lightAngle, 0.01f, 20.0f)) * 0.5f), 24.0f, 0.0f);
        }

        VkExtent2D extent{ target.GetWidth(), target.GetHeight() };
        Vk::TransitionImage(cmd, target.GetHdr().GetHandle(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        Vk::TransitionImage(cmd, target.GetIds().GetHandle(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        Vk::TransitionImage(cmd, target.GetDepth().GetHandle(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                            VK_IMAGE_ASPECT_DEPTH_BIT);
        // Screen-space reflections and GI need a perspective view too; without them the surface buffer is not stored.
        bool ssr = settings.SSR && settings.SSRIntensity > 0.0f && !camera.Orthographic && !options.ProbeCapture;
        bool ssgi = settings.SSGI && settings.SSGIIntensity > 0.0f && !camera.Orthographic && !options.ProbeCapture;
        bool surface = ssr || ssgi;
        Vk::TransitionImage(cmd, target.GetSurface().GetHandle(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        if (m_Samples > 1)
        {
            Vk::TransitionImage(cmd, target.GetSurfaceMS().GetHandle(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
            Vk::TransitionImage(cmd, target.GetHdrMS().GetHandle(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
            Vk::TransitionImage(cmd, target.GetIdsMS().GetHandle(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
            Vk::TransitionImage(cmd, target.GetDepthMS().GetHandle(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                                VK_IMAGE_ASPECT_DEPTH_BIT);
        }

        // SSAO needs a perspective view of opaque geometry; orthographic (2D) cameras skip it.
        bool ssao = settings.SSAO && !camera.Orthographic && !options.ProbeCapture;
        bool fog = settings.Fog && settings.FogDensity > 0.0f && !camera.Orthographic && !options.ProbeCapture;
        CollectDecals(scene);
        if (!m_Capturing)
            for (size_t i = 0; i < m_FrameProbes.size(); ++i)
            {
                const FrameProbe& p = m_FrameProbes[i];
                u.Probes[i] = { glm::vec4(p.Center, p.Intensity), glm::vec4(p.Min, (float)p.Texture), glm::vec4(p.Max, (float)p.Mips),
                                glm::vec4(p.BoxProjection ? 1.0f : 0.0f, p.BlendDistance, 0.0f, 0.0f) };
                u.Counts.y = (int)i + 1;
            }
        device.EndGpuTimer(cmd, shadowTimer);
        // Water surfaces read the opaque depth under them (shore foam): the depth prepass, copied.
        bool waterDepth = m_FrameHasWater && !camera.Orthographic && !options.ProbeCapture;
        u.Refraction.w = waterDepth ? (float)target.GetOpaqueDepthBindless() + 1.0f : 0.0f;
        m_DepthPrepassed = ssao || waterDepth;
        if (ssao || waterDepth)
        {
            if (ssao) u.AOParams = glm::vec4(settings.SSAODebug ? 2.0f : 1.0f, (float)target.GetAOBindless(1), 1.0f / extent.width, 1.0f / extent.height);
            device.BindBindless(cmd);
            device.BindUniforms(cmd, &u, sizeof(u));
            int aoTimer = device.BeginGpuTimer(cmd, ssao ? "Depth Prepass and SSAO" : "Depth Prepass");
            RenderAmbientOcclusion(cmd, scene, target, u.ViewProj, settings, ssao, waterDepth);
            device.EndGpuTimer(cmd, aoTimer);
        }
        int opaqueTimer = device.BeginGpuTimer(cmd, "Opaque");

        glm::vec4 clear = ToLinear(camera.ClearColor);
        VkRenderingAttachmentInfo colors[3]{};
        colors[0] = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
        colors[0].imageView = target.GetHdr().GetView();
        colors[0].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colors[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        colors[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        colors[0].clearValue.color = { { clear.r, clear.g, clear.b, options.ProbeCapture || camera.Transparent ? clear.a : 1.0f } }; // captures and Depth Only: alpha = covered
        colors[1] = colors[0];
        colors[1].imageView = target.GetIds().GetView();
        colors[1].clearValue.color.uint32[0] = 0;
        colors[1].clearValue.color.uint32[1] = 0;
        colors[1].clearValue.color.uint32[2] = 0;
        colors[1].clearValue.color.uint32[3] = 0;
        colors[2] = colors[1];
        colors[2].imageView = target.GetSurface().GetView();
        if (!surface) colors[2].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        VkRenderingAttachmentInfo depth{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
        depth.imageView = target.GetDepth().GetView();
        depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        depth.loadOp = ssao || waterDepth ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR; // reuse the prepass depth
        // Depth of field focuses for the game camera: the editor's Scene view (overlays on) stays sharp.
        bool dof = settings.DepthOfField && !camera.Orthographic && settings.BokehSize > 0.0f && !options.EditorOverlays && !options.ProbeCapture;
        // Passes after this one read the depth: depth of field, SSR, decals, fog and TAA (and next frame's GPU particle
        // collision).
        // Post Process Volumes around this camera blend over the scene's settings (not in probe captures or the Scene view).
        PostProcessValues post;
        post.Exposure = settings.Exposure;
        post.BloomIntensity = settings.BloomIntensity;
        post.Vignette = settings.Vignette;
        if (!options.ProbeCapture && !options.EditorOverlays) post = PostProcess::Evaluate(scene, camera.Position, camera.CullingMask);
        // Motion blur and TAA read the velocity buffer (game views).
        bool motionBlur = post.MotionBlur > 0.0f && Quality::Active().MotionBlur && !options.ProbeCapture && !options.EditorOverlays; // (volumes too)
        bool velocity = taa || motionBlur;
        bool readDepth = dof || surface || fog || underwaterFog || velocity || !m_Decals.empty() || m_ParticleDepthWanted || occlusion;
        m_ParticleDepthKept = readDepth && !camera.Orthographic;
        m_ParticleDepthViewProj = camera.Projection * camera.View;
        m_ParticleDepthSize = glm::uvec2(target.GetWidth(), target.GetHeight());
        // Order-independent transparency (game views): transparent meshes leave the main pass for their own.
        // Refracting materials need it (they read the opaque image while the transparent meshes draw on their own).
        m_Oit = (settings.OIT || m_FrameRefracts) && !options.ProbeCapture && !options.EditorOverlays &&
                std::any_of(m_RenderParts.begin(), m_RenderParts.end(), [](const RenderPart& p) { return p.Transparent; });
        depth.storeOp = readDepth || m_Oit ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depth.clearValue.depthStencil = { 1.0f, 0 };
        if (m_Samples > 1)
        {
            // Draw into the multisampled images and resolve at the end: color averaged, ids and depth from sample 0.
            MultisampleInto(colors[0], target.GetHdrMS(), target.GetHdr(), VK_RESOLVE_MODE_AVERAGE_BIT);
            MultisampleInto(colors[1], target.GetIdsMS(), target.GetIds(), VK_RESOLVE_MODE_SAMPLE_ZERO_BIT);
            MultisampleInto(colors[2], target.GetSurfaceMS(), target.GetSurface(), surface ? VK_RESOLVE_MODE_SAMPLE_ZERO_BIT : VK_RESOLVE_MODE_NONE);
            if (!surface) colors[2].resolveImageView = VK_NULL_HANDLE;
            MultisampleInto(depth, target.GetDepthMS(), target.GetDepth(), VK_RESOLVE_MODE_SAMPLE_ZERO_BIT);
        }

        VkRenderingInfo rendering{ VK_STRUCTURE_TYPE_RENDERING_INFO };
        rendering.renderArea = { { 0, 0 }, extent };
        rendering.layerCount = 1;
        rendering.colorAttachmentCount = 3;
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
        u.CascadeParams.w = m_DrawMode == SceneDrawMode::Unlit ? 1.0f : 0.0f; // Unlit: the surfaces' own colors
        if (options.ProbeCapture && options.CaptureChannel > 0) u.CascadeParams.w = (float)options.CaptureChannel; // impostor atlases
        device.BindUniforms(cmd, &u, sizeof(u));

        bool flatModes = m_DrawMode == SceneDrawMode::Wireframe || m_DrawMode == SceneDrawMode::Overdraw; // no sky behind them
        if (camera.Skybox && !flatModes && DebugDraw("Opaque", "HDR Color", "Sky", 0, 1, 1))
        {
            MainPipeline(MainPipelines::Sky).Bind(cmd);
            vkCmdDraw(cmd, 3, 1, 0, 0);
            m_Stats.DrawCalls++;
        }
        DrawMeshes(cmd, u.ViewProj, MeshPass::Main);

        if (!m_GridLines.empty())
        {
            SceneUniforms gridU = u;
            // Fading out with distance in the grid's plane (none in 2D and orthographic views).
            gridU.GridParams.x = (options.Grid2D || camera.Orthographic) ? 0.0f : 60.0f * m_GridSpacing;
            gridU.GridParams.y = (float)(int)m_GridPlane;
            device.BindUniforms(cmd, &gridU, sizeof(gridU));
            FlushLines(cmd, m_GridLines, MainPipeline(MainPipelines::Line));
            device.BindUniforms(cmd, &u, sizeof(u));
        }

        DrawSprites(cmd);
        DrawGpuParticles(cmd);

        vkCmdEndRendering(cmd);
        device.EndGpuTimer(cmd, opaqueTimer);
        target.RecordPickCopy(cmd);
        if (options.ProbeCapture)
        {
            // The caller copies the HDR image into a probe face.
            Vk::TransitionImage(cmd, target.GetHdr().GetHandle(), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            m_Sprites.clear();
            m_Lines.clear();
            m_GridLines.clear();
            m_RenderItems.clear();
            m_Capturing = false;
            return;
        }

        // Post processing: bloom (HDR) -> composite (exposure, tonemap, vignette) -> optional FXAA -> overlays and UI.
        Vk::TransitionImage(cmd, target.GetHdr().GetHandle(), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        device.BindBindless(cmd);
        if (readDepth)
        {
            Vk::TransitionImage(cmd, target.GetDepth().GetHandle(), VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                VK_IMAGE_ASPECT_DEPTH_BIT);
            if (occlusion)
            {
                ReduceDepth(cmd, target, camera);
                device.BindBindless(cmd);
            }
            device.BindUniforms(cmd, &u, sizeof(u));
        }
        if (velocity) RenderVelocity(cmd, target, u.ViewProj, unjitteredViewProj, u.Wind);
        else target.Motion.Valid = false;
        // HDR effects that need the finished opaque image: decals onto surfaces, reflections, fog, then TAA over all.
        if (!m_Decals.empty()) DrawDecals(cmd, target);
        if (surface)
            Vk::TransitionImage(cmd, target.GetSurface().GetHandle(), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        if (ssgi) RenderIndirect(cmd, target, settings, u.ViewProj);
        else target.Gi.Valid = false;
        if (ssr) RenderReflections(cmd, target, settings);
        int postTimer = device.BeginGpuTimer(cmd, "Effects and Post Processing");
        if (m_Oit)
        {
            int oitTimer = device.BeginGpuTimer(cmd, "Transparent (OIT)");
            // Transparent meshes sum into the OIT targets, tested against the opaque depth, then resolve over the scene.
            VkImage depthImage = target.GetDepth().GetHandle();
            VkImageLayout depthLayout = readDepth ? VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
            if (readDepth)
                Vk::TransitionImage(cmd, depthImage, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, depthLayout, VK_IMAGE_ASPECT_DEPTH_BIT);
            Vk::TransitionImage(cmd, target.GetOitAccum().GetHandle(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
            Vk::TransitionImage(cmd, target.GetOitReveal().GetHandle(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
            VkRenderingAttachmentInfo oit[2]{};
            for (int i = 0; i < 2; ++i)
            {
                oit[i] = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
                oit[i].imageView = i == 0 ? target.GetOitAccum().GetView() : target.GetOitReveal().GetView();
                oit[i].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                oit[i].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
                oit[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                float c = i == 0 ? 0.0f : 1.0f; // no color yet, nothing in the way
                oit[i].clearValue.color = { { c, c, c, c } };
            }
            VkRenderingAttachmentInfo oitDepth{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
            oitDepth.imageView = target.GetDepth().GetView();
            oitDepth.imageLayout = depthLayout;
            oitDepth.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            oitDepth.storeOp = VK_ATTACHMENT_STORE_OP_NONE;
            VkRenderingInfo info{ VK_STRUCTURE_TYPE_RENDERING_INFO };
            info.renderArea = { { 0, 0 }, extent };
            info.layerCount = 1;
            info.colorAttachmentCount = 2;
            info.pColorAttachments = oit;
            info.pDepthAttachment = &oitDepth;
            vkCmdBeginRendering(cmd, &info);
            vkCmdSetViewport(cmd, 0, 1, &viewport);
            vkCmdSetScissor(cmd, 0, 1, &scissor);
            device.BindBindless(cmd);
            // Refracting materials look up the finished opaque image behind them.
            SceneUniforms transparentU = u;
            transparentU.Refraction = glm::vec4((float)target.GetHdrBindless() + 1.0f, 1.0f / (float)extent.width, 1.0f / (float)extent.height, u.Refraction.w);
            device.BindUniforms(cmd, &transparentU, sizeof(transparentU));
            DrawMeshes(cmd, u.ViewProj, MeshPass::Transparent);
            vkCmdEndRendering(cmd);
            Vk::TransitionImage(cmd, target.GetOitAccum().GetHandle(), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            Vk::TransitionImage(cmd, target.GetOitReveal().GetHandle(), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            if (readDepth)
                Vk::TransitionImage(cmd, depthImage, depthLayout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);
            struct { uint32_t Accum, Reveal; } push{ target.GetOitAccumBindless(), target.GetOitRevealBindless() };
            FullscreenPass(cmd, target.GetHdr(), true, res.OitComposite, &push, sizeof(push));
            device.EndGpuTimer(cmd, oitTimer);
        }
        if (fog)
        {
            struct
            {
                glm::vec4 Color;
                uint32_t Depth, Steps;
                float Density, HeightFalloff, BaseHeight, MaxDistance, Anisotropy, Noise;
            } push{ glm::vec4(ToLinear(settings.FogColor) * settings.FogIntensity, std::max(settings.FogSunScattering, 0.0f)), target.GetDepthBindless(),
                    (uint32_t)std::clamp(settings.FogSteps, 4, 128), settings.FogDensity, std::max(settings.FogHeightFalloff, 0.0f), settings.FogHeight,
                    std::max(settings.FogMaxDistance, 1.0f), settings.FogAnisotropy, taa ? Halton(target.Temporal.Frame % 16 + 1, 5) : 0.0f };
            FullscreenPass(cmd, target.GetHdr(), true, res.Fog, &push, sizeof(push));
        }
        if (underwaterFog)
        {
            uint32_t depthTexture = target.GetDepthBindless();
            FullscreenPass(cmd, target.GetHdr(), true, res.Underwater, &depthTexture, sizeof(depthTexture));
        }
        if (taa)
        {
            // Blend into the history image not read this frame, then copy the result back as this frame's color.
            uint32_t read = target.Temporal.Written, write = 1 - read;
            struct
            {
                glm::mat4 PrevViewProj;
                uint32_t Current, History, Depth, HasHistory;
                glm::vec2 Texel;
                float Feedback;
                uint32_t Velocity;
            } push{ target.Temporal.PrevViewProj, target.GetHdrBindless(), target.GetTaaHistoryBindless(read), target.GetDepthBindless(),
                    target.Temporal.Valid ? 1u : 0u, glm::vec2(1.0f / extent.width, 1.0f / extent.height),
                    std::clamp(settings.TAAFeedback, 0.01f, 1.0f), target.GetVelocityBindless() + 1 };
            FullscreenPass(cmd, target.GetTaaHistory(write), false, res.Taa, &push, sizeof(push));
            VkImage hdr = target.GetHdr().GetHandle(), result = target.GetTaaHistory(write).GetHandle();
            Vk::TransitionImage(cmd, result, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            Vk::TransitionImage(cmd, hdr, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            VkImageCopy copy{};
            copy.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            copy.dstSubresource = copy.srcSubresource;
            copy.extent = { extent.width, extent.height, 1 };
            vkCmdCopyImage(cmd, result, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, hdr, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
            Vk::TransitionImage(cmd, result, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            Vk::TransitionImage(cmd, hdr, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            target.Temporal.Written = write;
            target.Temporal.Valid = true;
            target.Temporal.PrevViewProj = unjitteredViewProj;
        }
        // Motion blur: the velocity buffer's tiles, then the image blurred into the scratch image and copied back.
        if (motionBlur)
        {
            {
                struct { uint32_t Velocity, Tile; } tiles{ target.GetVelocityBindless(), target.GetVelocityTileSize() };
                FullscreenPass(cmd, target.GetVelocityTiles(), false, res.VelocityTiles, &tiles, sizeof(tiles));
                struct
                {
                    uint32_t Hdr, Depth, Velocity, Tiles;
                    float Intensity, MaxLength;
                    uint32_t Frame, Pad;
                } push{ target.GetHdrBindless(), target.GetDepthBindless(), target.GetVelocityBindless(), target.GetVelocityTilesBindless(),
                        std::clamp(post.MotionBlur, 0.0f, 1.0f), 0.06f, (uint32_t)RenderDevice::Get().Frame().FrameNumber, 0u };
                FullscreenPass(cmd, target.GetScratch(), false, res.MotionBlur, &push, sizeof(push));
                VkImage hdr = target.GetHdr().GetHandle(), result = target.GetScratch().GetHandle();
                Vk::TransitionImage(cmd, result, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
                Vk::TransitionImage(cmd, hdr, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                VkImageCopy copy{};
                copy.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
                copy.dstSubresource = copy.srcSubresource;
                copy.extent = { extent.width, extent.height, 1 };
                vkCmdCopyImage(cmd, result, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, hdr, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
                Vk::TransitionImage(cmd, result, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                Vk::TransitionImage(cmd, hdr, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            }
        }
        // Auto exposure: this frame's brightness is measured (read a few frames later) and the exposure adapts.
        float autoExposure = 1.0f;
        if (settings.AutoExposure && !options.ProbeCapture && !camera.Transparent)
        {
            autoExposure = UpdateAutoExposure(settings);
            ReduceLuminance(cmd, target);
        }
        else
            m_AutoExposureStarted = false;
        bool bloom = settings.Bloom && settings.BloomIntensity > 0.0f && target.GetBloomLevels() > 0;
        if (bloom) RenderBloom(cmd, target, settings);
        if (dof) RenderDepthOfField(cmd, target, camera, settings);

        VkViewport flat{ 0.0f, 0.0f, (float)extent.width, (float)extent.height, 0.0f, 1.0f };
        glm::vec3 filter = ToLinear(post.ColorFilter) * PostProcess::WhiteBalance(post.Temperature, post.Tint);
        glm::vec3 gamma = glm::max(post.Gamma, glm::vec3(0.01f));
        uint32_t tonemap = settings.Tonemapping ? 1u + (uint32_t)std::clamp(settings.Tonemapper, 0, 2) : 0u;
        CompositePush push{ target.GetHdrBindless(), post.Exposure * autoExposure, tonemap,
                            bloom ? target.GetBloomBindless(0) + 1 : 0u,
                            post.BloomIntensity / (float)std::max(1u, target.GetBloomLevels()), post.Vignette,
                            dof ? target.GetDofBindless(1) + 1 : 0u, camera.Transparent ? 1u : 0u,
                            post.Saturation, post.Contrast, filter.r, filter.g, filter.b,
                            post.Lift.r, post.Lift.g, post.Lift.b, gamma.r, gamma.g, gamma.b, post.Gain.r, post.Gain.g, post.Gain.b,
                            options.EditorOverlays ? 0.0f : post.ChromaticAberration, options.EditorOverlays ? 0.0f : post.FilmGrain,
                            options.EditorOverlays ? 0.0f : post.LensDistortion, (uint32_t)RenderDevice::Get().Frame().FrameNumber, 0u, 0u, 0.0f, 0u, 0.0f };
        if (bloom && settings.LensDirt.IsValid() && settings.LensDirtIntensity > 0.0f && !options.EditorOverlays)
            if (Ref<Texture2D> dirt = AssetManager::GetTexture(settings.LensDirt))
            {
                push.DirtTexture = dirt->GetBindlessIndex() + 1;
                push.DirtIntensity = settings.LensDirtIntensity;
            }
        if (post.LookupTexture && post.LookupContribution > 0.0f)
            if (Ref<Texture2D> lut = AssetManager::GetTexture(AssetHandle(post.LookupTexture)); lut && lut->GetHeight() > 1 && lut->GetWidth() == lut->GetHeight() * lut->GetHeight())
            {
                push.LookupTexture = lut->GetBindlessIndex() + 1;
                push.LookupSize = lut->GetHeight();
                push.LookupContribution = std::clamp(post.LookupContribution, 0.0f, 1.0f);
            }
        static_assert(sizeof(CompositePush) <= kPushConstantSize, "composite push constants too large");
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
        if (settings.FXAA && !camera.Transparent) // FXAA's output is opaque
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
        if (!options.Underlays.empty())
        {
            vkCmdSetViewport(cmd, 0, 1, &flat);
            DrawQuads(cmd, options.Underlays, extent);
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
        device.EndGpuTimer(cmd, postTimer);
        m_Stats.DrawCalls++;
        if (m_FrameCapturing) RenderFramePreview(cmd, target);

        m_Sprites.clear();
        m_Lines.clear();
        m_GridLines.clear();
        m_RenderItems.clear(); // drop mesh references until the next frame
        m_Stats.CpuMs = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - cpuStart).count();
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

        // Material content as a hash-map key (no allocations; MaterialGPU has no padding).
        struct MaterialKey
        {
            MaterialGPU Data;
            bool operator==(const MaterialKey& o) const { return std::memcmp(&Data, &o.Data, sizeof(Data)) == 0; }
        };
        struct MaterialKeyHash
        {
            size_t operator()(const MaterialKey& k) const
            {
                const auto* words = reinterpret_cast<const uint32_t*>(&k.Data);
                uint64_t h = 1469598103934665603ull; // FNV-1a over 32-bit words
                for (size_t i = 0; i < sizeof(MaterialGPU) / 4; ++i) h = (h ^ words[i]) * 1099511628211ull;
                return (size_t)h;
            }
        };
        static_assert(sizeof(MaterialGPU) % 4 == 0);

        MaterialGPU BuildMaterialGPU(const Material& m)
        {
            uint32_t white = AssetManager::GetWhiteTexture()->GetBindlessIndex();
            MaterialGPU gpu{};
            auto* g = &gpu;
            g->BaseColor = ToLinear(m.BaseColor);
            g->Emission = glm::vec4(ToLinear(m.EmissionColor) * m.EmissionStrength, m.BlendMode == MaterialBlendMode::Transparent ? std::max(m.Refraction, 0.0f) : 0.0f);
            g->Params = glm::vec4(m.Metallic, m.Roughness, m.AlphaCutoff, m.NormalStrength);
            g->TilingOffset = glm::vec4(m.Tiling, m.Offset);
            uint32_t normal = TextureIndex(m.NormalMap, white);
            g->Maps = glm::uvec4(TextureIndex(m.BaseColorMap, white), normal, TextureIndex(m.MetallicRoughnessMap, white),
                                 TextureIndex(m.EmissionMap, white));
            g->Flags = glm::uvec4((uint32_t)m.BlendMode, m.NormalMap.IsValid() && normal != white ? 1u : 0u, 0u, 0u);
            // Occlusion (plain materials: terrain and water use Maps2 themselves): z its texture + 1, w the strength.
            if (m.OcclusionMap.IsValid())
            {
                uint32_t occlusion = TextureIndex(m.OcclusionMap, white);
                if (occlusion != white)
                {
                    float strength = std::clamp(m.OcclusionStrength, 0.0f, 1.0f);
                    uint32_t bits;
                    std::memcpy(&bits, &strength, 4);
                    g->Maps2 = glm::uvec4(0u, 0u, occlusion + 1u, bits);
                }
            }
            return gpu;
        }
    }

    void SceneRenderer::SetInstancingEnabled(bool enabled) { s_InstancingEnabled = enabled; }
    void SceneRenderer::SetLightClusteringEnabled(bool enabled) { s_ClusterCulling = enabled; }
    void SceneRenderer::SetGpuLightCullingEnabled(bool enabled) { s_GpuLightCulling = enabled; }
    void SceneRenderer::SetGpuBlendShapesEnabled(bool enabled) { s_GpuBlendShapes = enabled; }
    void SceneRenderer::SetOcclusionCullingEnabled(bool enabled) { s_OcclusionCulling = enabled; }

    void SceneRenderer::ChooseOccluder(const CameraData& camera)
    {
        // The newest finished grid: as it is while the camera stays nearly where it was, else reprojected into this
        // view (within a few meters and a wide turn; farther, nothing is culled this frame).
        m_Occluder.Width = m_Occluder.Height = 0;
        uint64_t completed = RenderDevice::Get().GetCompletedFrameNumber();
        const OcclusionSlot* best = nullptr;
        for (const OcclusionSlot& s : m_OcclusionSlots)
            if (s.Used && s.Frame <= completed && (!best || s.Frame > best->Frame)) best = &s;
        if (!best) return;
        glm::vec3 forward = glm::normalize(-glm::vec3(glm::inverse(camera.View)[2]));
        float moved = glm::distance(camera.Position, best->Position), turned = glm::dot(forward, best->Forward);
        if (moved > 5.0f || turned < std::cos(glm::radians(60.0f))) return;
        m_Occluder.Width = best->Width;
        m_Occluder.Height = best->Height;
        m_Occluder.ViewProj = best->ViewProj;
        const auto* cells = static_cast<const float*>(best->Data.GetMapped());
        m_Occluder.Farthest.assign(cells, cells + (size_t)best->Width * best->Height);
        bool still = moved <= 0.01f && turned >= std::cos(glm::radians(0.1f)) && std::abs(camera.Projection[0][0] - best->Projection[0][0]) <= 1e-4f &&
                     std::abs(camera.Projection[1][1] - best->Projection[1][1]) <= 1e-4f;
        if (!still) m_Occluder = m_Occluder.Reprojected(camera.Projection * camera.View);
    }

    void SceneRenderer::ReduceDepth(VkCommandBuffer cmd, SceneRenderTarget& target, const CameraData& camera)
    {
        auto& device = RenderDevice::Get();
        uint64_t frame = device.Frame().FrameNumber;
        OcclusionSlot& slot = m_OcclusionSlots[frame % 3];
        uint32_t width = target.GetWidth(), height = target.GetHeight();
        uint32_t cell = std::max(8u, (width + 191) / 192);
        uint32_t gw = (width + cell - 1) / cell, gh = (height + cell - 1) / cell;
        if (!slot.Data.IsValid() || slot.Width * slot.Height < gw * gh)
        {
            slot.Data = Buffer((VkDeviceSize)gw * gh * sizeof(float), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                               MemoryUsage::GpuToCpu, "OcclusionDepth");
            VkBufferDeviceAddressInfo info{ VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO };
            info.buffer = slot.Data.GetHandle();
            slot.Address = vkGetBufferDeviceAddress(device.GetDevice(), &info);
        }
        slot.Width = gw;
        slot.Height = gh;
        slot.Frame = frame;
        slot.Used = true;
        slot.ViewProj = camera.Projection * camera.View;
        slot.Projection = camera.Projection;
        slot.Position = camera.Position;
        slot.Forward = glm::normalize(-glm::vec3(glm::inverse(camera.View)[2]));
        struct
        {
            glm::uvec2 Out;
            uint32_t Depth, CellSize;
            glm::uvec2 Grid, Size;
        } push{ { (uint32_t)slot.Address, (uint32_t)(slot.Address >> 32) }, target.GetDepthBindless(), cell, { gw, gh }, { width, height } };
        Vk::MemoryBarrier(cmd, VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                          VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
        Renderer::Get().DepthReduce.Bind(cmd);
        device.BindBindless(cmd, VK_PIPELINE_BIND_POINT_COMPUTE);
        vkCmdPushConstants(cmd, device.GetPipelineLayout(), VK_SHADER_STAGE_ALL, 0, sizeof(push), &push);
        vkCmdDispatch(cmd, (gw + 7) / 8, (gh + 7) / 8, 1);
        Vk::MemoryBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
    }
    void SceneRenderer::ReduceLuminance(VkCommandBuffer cmd, SceneRenderTarget& target)
    {
        auto& device = RenderDevice::Get();
        uint64_t frame = device.Frame().FrameNumber;
        LuminanceSlot& slot = m_LuminanceSlots[frame % 3];
        uint32_t width = target.GetWidth(), height = target.GetHeight();
        uint32_t cell = std::max(8u, (std::max(width, height) + 15) / 16); // about 16 x 9 cells
        uint32_t gw = (width + cell - 1) / cell, gh = (height + cell - 1) / cell;
        if (!slot.Data.IsValid() || slot.Cells < gw * gh)
        {
            slot.Data = Buffer((VkDeviceSize)gw * gh * sizeof(float), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                               MemoryUsage::GpuToCpu, "Luminance");
            VkBufferDeviceAddressInfo info{ VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO };
            info.buffer = slot.Data.GetHandle();
            slot.Address = vkGetBufferDeviceAddress(device.GetDevice(), &info);
        }
        slot.Cells = gw * gh;
        slot.Frame = frame;
        slot.Used = true;
        slot.Read = false;
        struct
        {
            glm::uvec2 Out;
            uint32_t Hdr, CellSize;
            glm::uvec2 Grid, Size;
        } push{ { (uint32_t)slot.Address, (uint32_t)(slot.Address >> 32) }, target.GetHdrBindless(), cell, { gw, gh }, { width, height } };
        Vk::MemoryBarrier(cmd, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                          VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT,
                          VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
        Renderer::Get().LuminanceReduce.Bind(cmd);
        device.BindBindless(cmd, VK_PIPELINE_BIND_POINT_COMPUTE);
        vkCmdPushConstants(cmd, device.GetPipelineLayout(), VK_SHADER_STAGE_ALL, 0, sizeof(push), &push);
        vkCmdDispatch(cmd, (gw + 7) / 8, (gh + 7) / 8, 1);
        Vk::MemoryBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
    }

    float SceneRenderer::UpdateAutoExposure(const SceneSettings& settings)
    {
        double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        float dt = m_AutoExposureStarted ? (float)std::clamp(now - m_AutoExposureClock, 0.0, 0.25) : 0.0f;
        m_AutoExposureClock = now;
        // The newest finished measurement (it saw the image as exposed then: undo that exposure).
        uint64_t completed = RenderDevice::Get().GetCompletedFrameNumber();
        LuminanceSlot* best = nullptr;
        for (auto& s : m_LuminanceSlots)
            if (s.Used && !s.Read && s.Frame <= completed && (!best || s.Frame > best->Frame)) best = &s;
        if (best)
        {
            best->Read = true;
            const float* cells = static_cast<const float*>(best->Data.GetMapped());
            double sum = 0.0;
            for (uint32_t i = 0; i < best->Cells; ++i) sum += cells[i];
            float avgLog = best->Cells ? (float)(sum / best->Cells) : 0.0f;
            // HDR values are before exposure, so the measurement does not depend on the adaptation.
            float targetEv = std::log2(0.18f) - avgLog + settings.ExposureCompensation;
            targetEv = std::clamp(targetEv, settings.AutoExposureMinEv, settings.AutoExposureMaxEv);
            if (!m_AutoExposureStarted) m_AutoExposureEv = targetEv;
            else
            {
                // Eyes adapt to the dark more slowly than to light.
                float speed = std::max(settings.AdaptationSpeed, 0.0f) * (targetEv > m_AutoExposureEv ? 0.6f : 1.0f);
                m_AutoExposureEv += (targetEv - m_AutoExposureEv) * (1.0f - std::exp(-dt * speed));
            }
            m_AutoExposureStarted = true;
        }
        return std::exp2(m_AutoExposureEv);
    }

    bool SceneRenderer::IsGpuLightCullingEnabled() { return s_GpuLightCulling; }

    bool SceneRenderer::CullLightsOnGpu(VkCommandBuffer cmd, const void* uniforms, size_t size)
    {
        auto& device = RenderDevice::Get();
        if (!m_GpuClusterCells.IsValid())
        {
            auto make = [&](VkDeviceSize bytes, const char* name, VkDeviceAddress& address) {
                Buffer b(bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, MemoryUsage::GpuOnly, name);
                VkBufferDeviceAddressInfo info{ VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO };
                info.buffer = b.GetHandle();
                address = vkGetBufferDeviceAddress(device.GetDevice(), &info);
                return b;
            };
            m_GpuClusterCells = make(LightClusters::kCount * sizeof(glm::uvec2), "GpuClusterCells", m_GpuClusterCellsAddress);
            m_GpuClusterIndices = make((VkDeviceSize)LightClusters::kCount * LightClusters::kMaxPerCluster * sizeof(uint32_t), "GpuClusterIndices",
                                       m_GpuClusterIndicesAddress);
        }
        SceneUniforms u = *static_cast<const SceneUniforms*>(uniforms);
        DIM_ASSERT(size == sizeof(u), "Scene uniforms size mismatch");
        u.LightBuffers.z = (uint32_t)m_GpuClusterCellsAddress;
        u.LightBuffers.w = (uint32_t)(m_GpuClusterCellsAddress >> 32);
        u.LightIndexBuffer = glm::uvec4((uint32_t)m_GpuClusterIndicesAddress, (uint32_t)(m_GpuClusterIndicesAddress >> 32), 0u, 0u);

        // The previous view's shading (earlier in this or an in-flight frame, same queue) must be done reading.
        Vk::MemoryBarrier(cmd, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT,
                          VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT);
        auto& res = Renderer::Get();
        res.LightCull.Bind(cmd);
        device.BindBindless(cmd, VK_PIPELINE_BIND_POINT_COMPUTE);
        device.BindUniforms(cmd, &u, sizeof(u), VK_PIPELINE_BIND_POINT_COMPUTE);
        uint32_t maxPerCluster = LightClusters::kMaxPerCluster;
        vkCmdPushConstants(cmd, device.GetPipelineLayout(), VK_SHADER_STAGE_ALL, 0, sizeof(maxPerCluster), &maxPerCluster);
        vkCmdDispatch(cmd, (LightClusters::kCount + 63) / 64, 1, 1);
        Vk::MemoryBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
                          VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
        return true;
    }
    void SceneRenderer::RequestProbeBake() { ++s_ProbeBakeGeneration; }

    SceneRenderer::ProbeResources::~ProbeResources()
    {
        auto& device = RenderDevice::Get();
        for (uint32_t index : FaceBindless)
            if (index) device.UnregisterTexture(index);
        if (Bindless) device.UnregisterTexture(Bindless);
        if (SourceBindless) device.UnregisterTexture(SourceBindless);
    }
    bool SceneRenderer::IsInstancingEnabled() { return s_InstancingEnabled; }

    SceneRenderer::FilteredSky::~FilteredSky()
    {
        if (Bindless) RenderDevice::Get().UnregisterTexture(Bindless);
    }

    namespace
    {
        // Prefiltered environments stop at 16 x 8: smaller mips cannot hold even the widest GGX lobe's shape, and
        // fewer mips leave more resolution per roughness step.
        uint32_t FilteredMipCount(uint32_t width)
        {
            uint32_t full = (uint32_t)std::floor(std::log2((float)std::max(width, 1u))) + 1;
            return full > 4 ? full - 4 : 1;
        }

        // Renders mips [firstMip, mips) of an equirectangular image as the GGX-prefiltered environment of `source`
        // (a bindless equirect with a mip chain): mip m is blurred for roughness m / (mips - 1). Mip 0 reads the source
        // at least at minLod0 so a larger source does not alias. Leaves those mips in SHADER_READ_ONLY_OPTIMAL.
        void PrefilterEquirectMips(VkCommandBuffer cmd, Image& dst, uint32_t firstMip, uint32_t source, uint32_t sourceMips, float minLod0)
        {
            auto& device = RenderDevice::Get();
            uint32_t mips = dst.GetMipLevels(), width = dst.GetWidth(), height = dst.GetHeight();
            if (firstMip >= mips) return;
            Vk::TransitionImage(cmd, dst.GetHandle(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT,
                                firstMip, mips - firstMip);
            struct PrefilterPush { uint32_t Source; float Roughness; float MaxLod; float MinLod; } push{ source, 0.0f, (float)(sourceMips - 1), 0.0f };
            device.BindBindless(cmd);
            Renderer::Get().ProbePrefilter.Bind(cmd);
            for (uint32_t mip = firstMip; mip < mips; ++mip)
            {
                uint32_t mw = std::max(1u, width >> mip), mh = std::max(1u, height >> mip);
                VkRenderingAttachmentInfo att{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
                att.imageView = dst.GetMipView(mip);
                att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                att.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
                att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                VkRenderingInfo info{ VK_STRUCTURE_TYPE_RENDERING_INFO };
                info.renderArea = { { 0, 0 }, { mw, mh } };
                info.layerCount = 1;
                info.colorAttachmentCount = 1;
                info.pColorAttachments = &att;
                vkCmdBeginRendering(cmd, &info);
                VkViewport viewport{ 0.0f, 0.0f, (float)mw, (float)mh, 0.0f, 1.0f };
                VkRect2D scissor{ { 0, 0 }, { mw, mh } };
                vkCmdSetViewport(cmd, 0, 1, &viewport);
                vkCmdSetScissor(cmd, 0, 1, &scissor);
                push.Roughness = mips > 1 ? (float)mip / (float)(mips - 1) : 0.0f;
                push.MinLod = mip == 0 ? minLod0 : 0.0f;
                vkCmdPushConstants(cmd, device.GetPipelineLayout(), VK_SHADER_STAGE_ALL, 0, sizeof(push), &push);
                vkCmdDraw(cmd, 3, 1, 0, 0);
                vkCmdEndRendering(cmd);
            }
            Vk::TransitionImage(cmd, dst.GetHandle(), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                VK_IMAGE_ASPECT_COLOR_BIT, firstMip, mips - firstMip);
        }
    }

    const SceneRenderer::FilteredSky* SceneRenderer::FilterSky(VkCommandBuffer cmd, const Ref<Texture2D>& sky)
    {
        if (!sky || !sky->IsHDR()) return nullptr;
        if (m_FilteredSky.Source.lock() == sky && m_FilteredSky.Bindless) return &m_FilteredSky;
        auto& device = RenderDevice::Get();
        if (m_FilteredSky.Bindless) device.UnregisterTexture(m_FilteredSky.Bindless);
        // Reflections never need more than 1024 x 512; rough mips are tiny anyway.
        uint32_t width = std::max(std::min(sky->GetWidth(), 1024u), 8u);
        uint32_t height = std::max(1u, width / 2);
        m_FilteredSky.Filtered = Image({ width, height, SceneRenderTarget::kHdrFormat,
                                         VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, FilteredMipCount(width), "SkyFiltered" });
        m_FilteredSky.Bindless = device.RegisterTexture(m_FilteredSky.Filtered.GetView(), device.GetLinearSampler());
        m_FilteredSky.Source = sky;
        float minLod0 = std::max(0.0f, std::log2((float)sky->GetWidth() / (float)width));
        PrefilterEquirectMips(cmd, m_FilteredSky.Filtered, 0, sky->GetBindlessIndex(), sky->GetMipLevels(), minLod0);
        return &m_FilteredSky;
    }

    SceneRenderer::~SceneRenderer()
    {
        if (m_ShadowMap) RenderDevice::Get().UnregisterTexture(m_ShadowBindless);
        if (m_PointShadowMap) RenderDevice::Get().UnregisterTexture(m_PointShadowBindless);
        for (auto& [key, impostor] : m_Impostors)
        {
            if (impostor.Bindless) RenderDevice::Get().UnregisterTexture(impostor.Bindless);
            if (impostor.NormalBindless) RenderDevice::Get().UnregisterTexture(impostor.NormalBindless);
            if (impostor.SurfaceBindless) RenderDevice::Get().UnregisterTexture(impostor.SurfaceBindless);
        }
        for (auto& [key, canvas] : m_WorldCanvases)
            if (canvas.Bindless) RenderDevice::Get().UnregisterTexture(canvas.Bindless);
    }

    namespace
    {
        constexpr uint32_t kImpostorTile = 256, kImpostorSides = 8, kImpostorRows = 2;
        // The rows of sides: seen a little from above, and from high up (picked by how steeply they are looked at).
        constexpr float kImpostorElevations[kImpostorRows] = { 15.0f, 55.0f };
        uint32_t ImpostorRow(float elevationDegrees) { return elevationDegrees > 35.0f ? 1u : 0u; }

        uint64_t ImpostorKey(Entity e) { return ((uint64_t)e.Index << 32) | e.Generation; }

        // What the impostor pictures, as a number: the meshes, materials and places of LOD 0 in the group's space (not the
        // lights: impostors are lit live; not the group's own place or turn: it is drawn turned to the camera, showing
        // the side the camera sees).
        uint64_t ImpostorSignature(Scene& scene, Entity group)
        {
            auto& reg = scene.GetRegistry();
            uint64_t h = 1469598103934665603ull;
            auto mix = [&](const void* data, size_t bytes) {
                const auto* p = (const uint8_t*)data;
                for (size_t i = 0; i < bytes; ++i) h = (h ^ p[i]) * 1099511628211ull;
            };
            std::function<void(Entity)> walk = [&](Entity e) {
                const auto& t = reg.Get<TransformComponent>(e);
                mix(&t.Position, sizeof(glm::vec3));
                mix(&t.Rotation, sizeof(glm::vec3));
                mix(&t.Scale, sizeof(glm::vec3));
                if (const auto* mr = reg.TryGet<MeshRendererComponent>(e))
                {
                    mix(&mr->Mesh, sizeof(mr->Mesh));
                    mix(&mr->Material, sizeof(mr->Material));
                    mix(&mr->Color, sizeof(mr->Color));
                    mix(&mr->Tint, sizeof(mr->Tint));
                    for (const auto& m : mr->Materials) mix(&m, sizeof(m));
                }
                for (Entity c : scene.GetChildren(e)) walk(c);
            };
            const auto* lod = reg.TryGet<LODGroupComponent>(group);
            if (lod && lod->GeneratedLevels > 0 && reg.Has<MeshRendererComponent>(group))
            {
                const auto& mr = reg.Get<MeshRendererComponent>(group);
                mix(&mr.Mesh, sizeof(mr.Mesh));
                mix(&mr.Material, sizeof(mr.Material));
                mix(&mr.Color, sizeof(mr.Color));
                mix(&mr.Tint, sizeof(mr.Tint));
            }
            else if (!scene.GetChildren(group).empty())
                walk(scene.GetChildren(group)[0]);
            return h;
        }
    }

    void SceneRenderer::PrepareImpostors(VkCommandBuffer cmd, Scene& scene)
    {
        auto& device = RenderDevice::Get();
        auto& reg = scene.GetRegistry();
        uint64_t frame = device.Frame().FrameNumber;
        bool captured = false; // one capture a frame
        std::vector<Entity> groups;
        for (auto [e, lod, t] : reg.ViewOf<LODGroupComponent, TransformComponent>())
            if (lod.Impostor && t.ActiveInHierarchy) groups.push_back(e);
        for (Entity g : groups)
        {
            uint64_t signature = ImpostorSignature(scene, g);
            uint64_t key = signature ^ ((uint64_t)(uintptr_t)&scene * 0x9e3779b97f4a7c15ull);
            m_ImpostorOf[ImpostorKey(g)] = key;
            Impostor& impostor = m_Impostors[key];
            impostor.LastUsed = frame;
            if (impostor.Owner == &scene && impostor.Signature == signature) continue;
            if (captured) continue;
            CaptureImpostor(cmd, scene, g, impostor);
            impostor.Signature = signature;
            impostor.Owner = &scene;
            captured = true;
        }
        // Impostors no longer shown for a while go.
        for (auto it = m_Impostors.begin(); it != m_Impostors.end();)
            if (frame - it->second.LastUsed > 600)
            {
                if (it->second.Bindless) device.UnregisterTexture(it->second.Bindless);
                if (it->second.NormalBindless) device.UnregisterTexture(it->second.NormalBindless);
                if (it->second.SurfaceBindless) device.UnregisterTexture(it->second.SurfaceBindless);
                it = m_Impostors.erase(it);
            }
            else
                ++it;
        std::erase_if(m_ImpostorOf, [this](const auto& entry) { return !m_Impostors.contains(entry.second); });
    }

    void SceneRenderer::CaptureImpostor(VkCommandBuffer cmd, Scene& scene, Entity group, Impostor& impostor)
    {
        auto& device = RenderDevice::Get();
        auto& reg = scene.GetRegistry();
        // A scene of LOD 0 alone (in the group's space), pictured as its surface colors and normals (lit live when drawn).
        Scene stage("Impostor");
        stage.GetSettings() = scene.GetSettings();
        auto& sreg = stage.GetRegistry();
        Entity model;
        const auto* lod = reg.TryGet<LODGroupComponent>(group);
        if (lod && lod->GeneratedLevels > 0 && reg.Has<MeshRendererComponent>(group))
        {
            model = stage.CreateEntity("Model");
            sreg.Emplace<MeshRendererComponent>(model) = reg.Get<MeshRendererComponent>(group);
        }
        else if (!scene.GetChildren(group).empty())
            model = SceneSerializer::DeserializeEntityTree(stage, SceneSerializer::SerializeEntityTree(scene, scene.GetChildren(group)[0]), Entity::Null());
        std::vector<Entity> nested;
        for (auto [e, l] : sreg.ViewOf<LODGroupComponent>()) nested.push_back(e);
        for (Entity e : nested) sreg.Remove<LODGroupComponent>(e);
        stage.UpdateTransforms();
        glm::vec3 lo, hi;
        impostor.Radius = 0.0f;
        if (model.IsNull() || !LODGroups::Bounds(stage, model, lo, hi)) return;
        impostor.Center = (lo + hi) * 0.5f;
        impostor.Radius = std::max(glm::length(hi - lo) * 0.5f, 1e-3f);

        // The atlas: a tile per side, a row per elevation, then its mips (down to a texel a tile).
        uint32_t width = kImpostorTile * kImpostorSides, height = kImpostorTile * kImpostorRows;
        uint32_t mips = (uint32_t)std::floor(std::log2((float)kImpostorTile)) + 1;
        if (!impostor.Atlas.IsValid())
        {
            impostor.Atlas = Image(ImageDesc{ width, height, SceneRenderTarget::kHdrFormat,
                                              VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, mips, "ImpostorAtlas" });
            impostor.Bindless = device.RegisterTexture(impostor.Atlas.GetView(), device.GetLinearSampler());
        }
        if (!impostor.Normals.IsValid())
        {
            impostor.Normals = Image(ImageDesc{ width, height, SceneRenderTarget::kHdrFormat,
                                                VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, mips, "ImpostorNormals" });
            impostor.NormalBindless = device.RegisterTexture(impostor.Normals.GetView(), device.GetLinearSampler());
        }
        if (!impostor.Surface.IsValid())
        {
            impostor.Surface = Image(ImageDesc{ width, height, SceneRenderTarget::kHdrFormat,
                                                VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, mips, "ImpostorSurface" });
            impostor.SurfaceBindless = device.RegisterTexture(impostor.Surface.GetView(), device.GetLinearSampler());
        }
        if (!m_ImpostorTarget) m_ImpostorTarget = MakeScope<SceneRenderTarget>(kImpostorTile, kImpostorTile);
        VkImage atlases[3] = { impostor.Atlas.GetHandle(), impostor.Normals.GetHandle(), impostor.Surface.GetHandle() };
        for (VkImage a : atlases) Vk::TransitionImage(cmd, a, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        RenderStats outer = m_Stats;
        SceneRenderOptions capture;
        capture.ProbeCapture = true;
        float r = impostor.Radius;
        for (uint32_t pass = 0; pass < kImpostorSides * kImpostorRows * 3; ++pass)
        {
            // Each side three times: its surface colors, its normals (cleared to "facing the viewer" so the mips' edges
            // do not lean; 0.7354 is the sRGB value of linear 0.5), and its roughness and metallic.
            uint32_t side = pass / 3 % kImpostorSides, row = pass / 3 / kImpostorSides, channel = pass % 3;
            bool normals = channel == 1;
            VkImage atlas = atlases[channel];
            const int channels[3] = { 3, 2, 4 };
            capture.CaptureChannel = channels[channel];
            // Seen a little from above, from all round: side k from the direction yaw = k * 45 degrees (+Z at 0).
            float yaw = glm::two_pi<float>() * side / kImpostorSides, elevation = glm::radians(kImpostorElevations[row]);
            glm::vec3 from(std::sin(yaw) * std::cos(elevation), std::sin(elevation), std::cos(yaw) * std::cos(elevation));
            CameraData cam;
            cam.Position = impostor.Center + from * (r * 3.0f);
            cam.View = glm::lookAt(cam.Position, impostor.Center, glm::vec3(0.0f, 1.0f, 0.0f));
            cam.Projection = glm::ortho(-r, r, -r, r, 0.01f, r * 6.0f);
            cam.Orthographic = true;
            cam.Skybox = false;
            // The surface atlas's depth (red) is cleared to a radius behind the center: the edges' texels blend toward
            // the back, so a cutout never reaches toward the viewer (or the sun) past its picture.
            cam.ClearColor = normals ? glm::vec4(0.7354f, 0.7354f, 1.0f, 0.0f) : channel == 2 ? glm::vec4(1.0f, 1.0f, 0.0f, 0.0f) // rough at the edges
                                                                                       : glm::vec4(0.0f); // alpha 0 where nothing is
            Render(cmd, stage, cam, *m_ImpostorTarget, capture);
            VkImageCopy copy{};
            copy.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            copy.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            copy.dstOffset = { (int32_t)(side * kImpostorTile), (int32_t)(row * kImpostorTile), 0 };
            copy.extent = { kImpostorTile, kImpostorTile, 1 };
            vkCmdCopyImage(cmd, m_ImpostorTarget->GetHdr().GetHandle(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, atlas, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
            Vk::TransitionImage(cmd, m_ImpostorTarget->GetHdr().GetHandle(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
        m_Stats = outer;
        for (VkImage atlas : atlases)
        {
            int32_t w = (int32_t)width, h = (int32_t)height;
            for (uint32_t mip = 1; mip < mips; ++mip)
            {
                Vk::TransitionImage(cmd, atlas, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT, mip - 1, 1);
                int32_t nw = std::max(1, w / 2), nh = std::max(1, h / 2);
                VkImageBlit blit{};
                blit.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, mip - 1, 0, 1 };
                blit.srcOffsets[1] = { w, h, 1 };
                blit.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, mip, 0, 1 };
                blit.dstOffsets[1] = { nw, nh, 1 };
                vkCmdBlitImage(cmd, atlas, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, atlas, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
                Vk::TransitionImage(cmd, atlas, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT, mip - 1, 1);
                w = nw;
                h = nh;
            }
            Vk::TransitionImage(cmd, atlas, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT, mips - 1, 1);
        }
    }

    glm::mat4 PointShadowFaceViewProj(const glm::vec3& position, int face, float nearPlane, float farPlane)
    {
        static const glm::vec3 dirs[6] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
        static const glm::vec3 ups[6] = { { 0, 1, 0 }, { 0, 1, 0 }, { 0, 0, 1 }, { 0, 0, 1 }, { 0, 1, 0 }, { 0, 1, 0 } };
        glm::mat4 view = glm::lookAt(position, position + dirs[face], ups[face]);
        glm::mat4 proj = glm::perspective(glm::radians(90.0f), 1.0f, nearPlane, farPlane);
        return proj * view;
    }

    void SpotCookieBasis(const glm::vec3& direction, glm::vec3& right, glm::vec3& up)
    {
        glm::vec3 d = glm::normalize(direction);
        glm::vec3 ref = std::abs(d.y) > 0.99f ? glm::vec3(0, 0, 1) : glm::vec3(0, 1, 0);
        right = glm::normalize(glm::cross(d, ref));
        up = glm::cross(right, d);
    }

    glm::vec2 SpotCookieUV(const glm::vec3& direction, float cosOuter, float roll, const glm::vec3& toPoint)
    {
        glm::vec3 right, up, d = glm::normalize(direction), p = glm::normalize(toPoint);
        SpotCookieBasis(d, right, up);
        float tanOuter = std::sqrt(std::max(1.0f - cosOuter * cosOuter, 1e-6f)) / std::max(cosOuter, 1e-4f);
        glm::vec2 q = glm::vec2(glm::dot(p, right), glm::dot(p, up)) / (std::max(glm::dot(p, d), 1e-4f) * tanOuter);
        float c = std::cos(roll), s = std::sin(roll);
        q = glm::vec2(c * q.x + s * q.y, -s * q.x + c * q.y); // into the light's own (rolled) axes
        return glm::vec2(q.x * 0.5f + 0.5f, 0.5f - q.y * 0.5f);
    }

    glm::vec2 PointCookieUV(const glm::mat3& lightRotation, const glm::vec3& toPoint)
    {
        return Environment::DirectionToEquirect(glm::transpose(lightRotation) * toPoint);
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
        m_ShadowTargetName = "Point Shadow Map";
        struct Restore
        {
            const char*& Name;
            ~Restore() { Name = "Shadow Map"; }
        } restore{ m_ShadowTargetName };
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
                DrawMeshes(cmd, viewProj, MeshPass::Shadow);
            }
        vkCmdEndRendering(cmd);
        Vk::TransitionImage(cmd, img, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);
    }

    uint64_t SceneRenderer::TerrainKey(const TerrainComponent& terrain)
    {
        uint64_t frame = RenderDevice::Get().Frame().FrameNumber;
        if (frame != m_TerrainKeysFrame)
        {
            m_TerrainKeys.clear();
            m_TerrainKeysFrame = frame;
        }
        auto [it, added] = m_TerrainKeys.try_emplace(&terrain, 0);
        if (added) it->second = Terrain::SettingsKey(terrain);
        return it->second;
    }

    void SceneRenderer::BuildRenderList(Scene& scene, const CameraData& camera, const SceneRenderOptions& options)
    {
        auto& device = RenderDevice::Get();
        auto& reg = scene.GetRegistry();
        m_RenderItems.clear();
        m_RenderParts.clear();
        m_SimPositions.clear();
        m_BlendJobs.clear();
        m_OceanFrame = {};
        m_FrameRefracts = false;
        m_FrameHasWater = false;
        Material scratch;

        // Materials are uploaded once per distinct content, so objects sharing a material (or the same simple
        // Mesh Renderer settings) share an address, which is what lets them batch.
        auto listStart = std::chrono::steady_clock::now();
        m_BatchInstanceable.clear();
        struct BatchKey
        {
            Mesh* MeshPtr;
            uint32_t FirstIndex, IndexCount;
            VkDeviceAddress Material;
            bool DoubleSided;
            uint32_t Probe;
            bool operator==(const BatchKey&) const = default;
        };
        struct BatchKeyHash
        {
            size_t operator()(const BatchKey& k) const
            {
                size_t h = std::hash<const void*>()(k.MeshPtr);
                for (uint64_t v : { (uint64_t)k.FirstIndex, (uint64_t)k.IndexCount, (uint64_t)k.Material, (uint64_t)k.DoubleSided, (uint64_t)k.Probe })
                    h = (h ^ std::hash<uint64_t>()(v)) * 1099511628211ull;
                return h;
            }
        };
        std::unordered_map<BatchKey, uint32_t, BatchKeyHash> batches;
        auto batchOf = [&](const BatchKey& key, bool instanceable) -> uint32_t {
            if (instanceable)
                if (auto it = batches.find(key); it != batches.end()) return it->second;
            uint32_t id = (uint32_t)m_BatchInstanceable.size();
            m_BatchInstanceable.push_back(instanceable ? 1 : 0);
            if (instanceable) batches.emplace(key, id);
            return id;
        };

        std::unordered_map<MaterialKey, VkDeviceAddress, MaterialKeyHash> materialCache;
        uint32_t whiteIndex = AssetManager::GetWhiteTexture()->GetBindlessIndex();
        auto uploadMaterialGPU = [&](const MaterialGPU& gpu) -> VkDeviceAddress {
            MaterialKey key{ gpu };
            if (auto it = materialCache.find(key); it != materialCache.end()) return it->second;
            TransientAllocation a = device.AllocateTransient(sizeof(MaterialGPU), 16);
            if (!a) return 0;
            std::memcpy(a.Data, &key.Data, sizeof(MaterialGPU));
            materialCache.emplace(key, a.Address);
            if (m_FrameCapturing)
            {
                // The frame debugger shows these values with the draws that use them.
                FrameDebugEvent& m = m_FrameMaterials[a.Address];
                m.HasMaterial = true;
                m.BaseColor = gpu.BaseColor;
                m.Emission = glm::vec4(glm::vec3(gpu.Emission), 1.0f);
                m.Metallic = gpu.Params.x;
                m.Roughness = gpu.Params.y;
                m.AlphaCutoff = gpu.Params.z;
                m.NormalStrength = gpu.Params.w;
                m.BlendMode = gpu.Flags.x;
                bool terrain = gpu.Flags.z == 1u;
                m.BaseMap = !terrain && gpu.Maps.x != whiteIndex;
                m.NormalMap = !terrain && gpu.Flags.y != 0u;
                m.Kind = terrain ? "Terrain" : gpu.Flags.w == 1u ? "Fur Shell" : gpu.Flags.w == 2u ? "Water" : "Mesh";
            }
            return a.Address;
        };
        // Scene view search: everything but the matches drawn dark (their colors, emission and terrain layers). Greyed
        // objects (Prefab Mode's context) are drawn pale and colorless.
        std::vector<uint8_t> lit, greyed;
        bool dimming = options.EditorOverlays && options.Highlighting;
        if (dimming)
            for (Entity h : options.Highlight)
            {
                if (lit.size() <= h.Index) lit.resize(h.Index + 1, 0);
                lit[h.Index] = 1;
            }
        for (Entity g : options.Greyed)
        {
            if (greyed.size() <= g.Index) greyed.resize(g.Index + 1, 0);
            greyed[g.Index] = 1;
        }
        // 0: as it is, 1: dark (search), 2: greyed.
        auto dimmed = [&](Entity e) -> int {
            if (e.Index < greyed.size() && greyed[e.Index]) return 2;
            return dimming && (e.Index >= lit.size() || !lit[e.Index]) ? 1 : 0;
        };
        auto dim = [whiteIndex](MaterialGPU g, int mode) {
            if (mode == 1) g.BaseColor = glm::vec4(glm::vec3(g.BaseColor) * 0.15f + glm::vec3(0.02f), g.BaseColor.a);
            if (mode == 2)
            {
                // Flat grey: the color map goes too (terrains keep their layer maps, greyed by their colors).
                float luma = glm::dot(glm::vec3(g.BaseColor), glm::vec3(0.3f, 0.59f, 0.11f));
                g.BaseColor = glm::vec4(glm::vec3(luma * 0.25f + 0.35f), g.BaseColor.a);
                if (g.Flags.z != 1u) g.Maps.x = whiteIndex;
            }
            if (mode != 0) g.Emission = glm::vec4(0.0f, 0.0f, 0.0f, g.Emission.w);
            return g;
        };
        auto uploadMaterial = [&](const Material& m, int dark) { return uploadMaterialGPU(dim(BuildMaterialGPU(m), dark)); };
        // LOD groups pick their child for this camera (so every pass, shadows too, draws the same one).
        LODGroups::Frame lods;
        LODGroups::Evaluate(scene, camera.Position, camera.Projection, camera.Orthographic, lods);
        std::vector<uint32_t> partSubMesh; // per part of the current item: its sub-mesh

        auto culled = [&](Entity e) { return LayerCulled(scene, e); };
        // Water: the shallows foam where the ground is near the surface, not around floating things (dynamic bodies),
        // whose boxes go to the water shader (filled once every item is known).
        constexpr uint32_t kMaxFloating = 64;
        bool anyWater = reg.ViewOf<WaterVolumeComponent>().SizeHint() > 0;
        TransientAllocation floatingBlock;
        auto floats = [&](Entity e) {
            for (int depth = 0; !e.IsNull() && depth < 64; ++depth, e = scene.GetParent(e))
                if (const auto* rb = reg.TryGet<RigidbodyComponent>(e)) return rb->Type == BodyType::Dynamic;
            return false;
        };
        for (auto [e, mr, t] : reg.ViewOf<MeshRendererComponent, TransformComponent>())
        {
            if (!t.ActiveInHierarchy || (e.Index < lods.Hidden.size() && lods.Hidden[e.Index]) || culled(e)) continue;
            int dark = dimmed(e);
            // LOD groups: a fading level is dithered; a group with generated levels draws its own mesh simplified.
            uint32_t lodFade = 0;
            if (auto it = lods.Fade.find(e.Index); it != lods.Fade.end()) lodFade = it->second;
            const LODGroups::Frame::Own* own = nullptr;
            float reduction = 0.5f;
            if (auto it = lods.Generated.find(e.Index); it != lods.Generated.end())
            {
                own = &it->second;
                lodFade = own->Fade;
                reduction = reg.Get<LODGroupComponent>(e).Reduction;
            }
            // Cloth draws its own grid, with this frame's simulated vertices when playing.
            const ClothComponent* cloth = reg.TryGet<ClothComponent>(e);
            const SoftBodyComponent* soft = cloth ? nullptr : reg.TryGet<SoftBodyComponent>(e);
            const TerrainComponent* terrain = reg.TryGet<TerrainComponent>(e);
            const Terrain::Runtime* terrainRt = terrain ? Terrain::GetRuntime(*terrain, TerrainKey(*terrain)) : nullptr;
            // A Water Volume draws its surface grid, moved by the waves.
            const WaterVolumeComponent* water = cloth || soft ? nullptr : reg.TryGet<WaterVolumeComponent>(e);
            // A rope draws a tube along its points (simulated, or its starting shape in the editor).
            const RopeComponent* rope = cloth || soft || water ? nullptr : reg.TryGet<RopeComponent>(e);
            const WaterVolumeComponent* ocean = water && water->Ocean ? water : nullptr;
            Ref<Mesh> mesh = rope ? Rope::TubeMesh(*rope) : cloth ? Cloth::GridMesh(*cloth) : ocean ? Water::OceanMesh() : water ? Water::SurfaceMesh(*water) : terrainRt ? terrainRt->Surface
                                                                                                                   : AssetManager::GetMesh(own ? LODGroups::GeneratedMesh(mr.Mesh, own->Level, reduction, false) : mr.Mesh);
            if (!mesh || (cloth && cloth->TornEdges > 0 && cloth->TornIndices.empty())) continue; // torn to nothing
            RenderItem item;
            item.World = t.World;
            item.Floating = anyWater && floats(e);
            AABB bounds = mesh->GetBounds();
            if (ocean)
            {
                // The ocean's rings around the camera (in the volume's space), out to the camera's far plane.
                glm::mat4 toLocal = glm::inverse(t.World);
                glm::vec3 eye = glm::vec3(toLocal * glm::vec4(camera.Position, 1.0f));
                float farPlane = camera.Orthographic ? 2000.0f : camera.Projection[3][2] / (camera.Projection[2][2] + 1.0f);
                float scale = std::max(glm::length(glm::vec3(t.World[0])), glm::length(glm::vec3(t.World[2])));
                glm::vec2 center = glm::floor(glm::vec2(eye.x, eye.z) * 0.5f) * 2.0f; // in steps, so the rings do not slide along
                float radius = std::max(farPlane, 1.0f) * 0.98f / std::max(scale, 1e-4f);
                Water::OceanVertices(*ocean, center, radius, scene.GetDisplayTime(), m_OceanVertices);
                m_OceanFrame = { ocean, center, radius, scene.GetDisplayTime() };
                item.Ocean = true;
                if (TransientAllocation block = device.AllocateTransient(m_OceanVertices.size() * sizeof(Vertex), 16))
                {
                    std::memcpy(block.Data, m_OceanVertices.data(), m_OceanVertices.size() * sizeof(Vertex));
                    item.Vertices = block.Buffer;
                    item.VertexOffset = block.Offset;
                }
                else
                    continue;
                bounds.Min = glm::vec3(center.x - 1e6f, -1e3f, center.y - 1e6f);
                bounds.Max = glm::vec3(center.x + 1e6f, 1e3f, center.y + 1e6f);
            }
            if (rope)
            {
                std::vector<glm::vec3> points = rope->SimulatedPoints;
                if ((int)points.size() != Rope::PointCount(*rope))
                {
                    glm::mat4 toLocal = glm::inverse(t.World);
                    bool attached = !rope->AttachTo.IsNull() && scene.IsValid(rope->AttachTo) && rope->AttachTo != e;
                    glm::vec3 end = attached ? glm::vec3(toLocal * (reg.Get<TransformComponent>(rope->AttachTo).World * glm::vec4(rope->AttachOffset, 1.0f))) : glm::vec3(0.0f);
                    points = Rope::RestPoints(*rope, glm::vec3(0.0f), glm::vec3(0.0f, -1.0f, 0.0f), attached ? &end : nullptr);
                }
                std::vector<Vertex> tube;
                Rope::BuildTube(*rope, points, tube);
                if (TransientAllocation block = tube.size() == mesh->GetVertexCount() ? device.AllocateTransient(tube.size() * sizeof(Vertex), 16) : TransientAllocation{})
                {
                    std::memcpy(block.Data, tube.data(), tube.size() * sizeof(Vertex));
                    item.Vertices = block.Buffer;
                    item.VertexOffset = block.Offset;
                    item.SimPositions = (int32_t)m_SimPositions.size();
                    auto& positions = m_SimPositions.emplace_back(tube.size());
                    for (size_t i = 0; i < tube.size(); ++i) positions[i] = tube[i].Position;
                    bounds.Min = bounds.Max = tube[0].Position;
                    for (const Vertex& v : tube)
                    {
                        bounds.Min = glm::min(bounds.Min, v.Position);
                        bounds.Max = glm::max(bounds.Max, v.Position);
                    }
                }
            }
            // Simulated vertices (cloth, soft bodies, dents) replace the mesh's, on top of its UVs and tangents.
            const DeformableComponent* dented = cloth || soft || water || rope || terrainRt ? nullptr : reg.TryGet<DeformableComponent>(e);
            if (dented && dented->DentedPositions.empty()) dented = nullptr;
            const std::vector<glm::vec3>* simPositions = cloth ? &cloth->SimulatedPositions : soft ? &soft->SimulatedPositions : water && !ocean ? &water->SimulatedPositions
                                                       : dented ? &dented->DentedPositions : nullptr;
            const std::vector<glm::vec3>* simNormals = cloth ? &cloth->SimulatedNormals : soft ? &soft->SimulatedNormals : water ? &water->SimulatedNormals
                                                     : dented ? &dented->DentedNormals : nullptr;
            std::shared_ptr<const MeshData> softData = soft || dented ? AssetManager::GetMeshData(mr.Mesh) : nullptr;
            const std::vector<Vertex>* base = cloth ? &Cloth::GridVertices(*cloth) : softData ? &softData->Vertices : water ? &Water::SurfaceVertices(*water) : nullptr;
            if (simPositions && base && !simPositions->empty() && simPositions->size() == mesh->GetVertexCount() && base->size() == simPositions->size())
            {
                if (TransientAllocation block = device.AllocateTransient(base->size() * sizeof(Vertex), 16))
                {
                    // Each vertex is put together here and written once: the block is mapped GPU memory, slow to
                    // read back (the bounds come from the simulated positions).
                    auto* out = (Vertex*)block.Data;
                    bounds.Min = bounds.Max = (*simPositions)[0];
                    for (size_t i = 0; i < base->size(); ++i)
                    {
                        Vertex v = (*base)[i];
                        v.Position = (*simPositions)[i];
                        v.Normal = (*simNormals)[i];
                        out[i] = v;
                        bounds.Min = glm::min(bounds.Min, v.Position);
                        bounds.Max = glm::max(bounds.Max, v.Position);
                    }
                    item.Vertices = block.Buffer;
                    item.VertexOffset = block.Offset;
                    item.SimPositions = (int32_t)m_SimPositions.size();
                    m_SimPositions.push_back(*simPositions);
                }
                // Torn cloth: only the triangles still attached.
                if (cloth && !cloth->TornIndices.empty() && item.Vertices)
                    if (TransientAllocation block = device.AllocateTransient(cloth->TornIndices.size() * sizeof(uint32_t), 16))
                    {
                        std::memcpy(block.Data, cloth->TornIndices.data(), cloth->TornIndices.size() * sizeof(uint32_t));
                        item.Indices = block.Buffer;
                        item.IndexOffset = block.Offset;
                        item.IndexCount = (uint32_t)cloth->TornIndices.size();
                    }
            }
            // Blend shapes: the model's vertices moved by the weighted shapes (skinning still applies on top).
            if (!item.Vertices && mesh->GetBlendShapeCount() > 0)
                if (auto shaped = AssetManager::GetMeshData(mr.Mesh); shaped && shaped->Vertices.size() == mesh->GetVertexCount())
                {
                    m_BlendWeights.resize(shaped->BlendShapes.size());
                    bool any = false;
                    for (size_t s = 0; s < m_BlendWeights.size(); ++s)
                    {
                        m_BlendWeights[s] = s < mr.BlendShapeWeights.size() ? mr.BlendShapeWeights[s] * 0.01f : shaped->BlendShapes[s].DefaultWeight;
                        any |= m_BlendWeights[s] != 0.0f;
                    }
                    if (any && s_GpuBlendShapes && mesh->IsSkinned() && mesh->GetBlendShapeAddress())
                    {
                        // On the GPU (a compute pass before the passes): the active shapes as (index, weight) pairs.
                        uint32_t active = 0;
                        for (float w : m_BlendWeights) active += w != 0.0f;
                        TransientAllocation out = device.AllocateTransient(shaped->Vertices.size() * sizeof(Vertex), 16);
                        TransientAllocation list = device.AllocateTransient(active * sizeof(glm::uvec2), 16);
                        if (out && list)
                        {
                            auto* pairs = (glm::uvec2*)list.Data;
                            uint32_t k = 0;
                            glm::vec3 reach(0.0f);
                            for (size_t s = 0; s < m_BlendWeights.size(); ++s)
                                if (m_BlendWeights[s] != 0.0f)
                                {
                                    float w = m_BlendWeights[s];
                                    uint32_t bits;
                                    std::memcpy(&bits, &w, sizeof(bits));
                                    pairs[k++] = glm::uvec2((uint32_t)s, bits);
                                    if (s < mesh->GetBlendShapeReach().size()) reach += mesh->GetBlendShapeReach()[s] * std::abs(w);
                                }
                            m_BlendJobs.push_back({ mesh->GetVertexAddress(), mesh->GetBlendShapeAddress(), out.Address, list.Address, mesh->GetVertexCount(), active });
                            item.Vertices = out.Buffer;
                            item.VertexOffset = out.Offset;
                            bounds.Min -= reach; // (as far as the shapes could move it)
                            bounds.Max += reach;
                        }
                    }
                    else if (any)
                        if (TransientAllocation block = device.AllocateTransient(shaped->Vertices.size() * sizeof(Vertex), 16))
                        {
                            AABB moved;
                            if (mesh->IsSkinned())
                                moved = shaped->ApplyBlendShapes(m_BlendWeights.data(), m_BlendWeights.size(), (Vertex*)block.Data);
                            else
                            {
                                // Unskinned: its moved positions also give its motion vectors (as simulated vertices do).
                                m_BlendVertices.resize(shaped->Vertices.size());
                                moved = shaped->ApplyBlendShapes(m_BlendWeights.data(), m_BlendWeights.size(), m_BlendVertices.data());
                                std::memcpy(block.Data, m_BlendVertices.data(), m_BlendVertices.size() * sizeof(Vertex));
                                item.SimPositions = (int32_t)m_SimPositions.size();
                                auto& positions = m_SimPositions.emplace_back(m_BlendVertices.size());
                                for (size_t i = 0; i < m_BlendVertices.size(); ++i) positions[i] = m_BlendVertices[i].Position;
                            }
                            item.Vertices = block.Buffer;
                            item.VertexOffset = block.Offset;
                            bounds.Min = glm::min(bounds.Min, moved.Min); // (the bind pose's too: skinning keeps its own bounds)
                            bounds.Max = glm::max(bounds.Max, moved.Max);
                        }
                }
            glm::mat3 m(t.World);
            glm::vec3 ext = bounds.Extents();
            item.Center = glm::vec3(t.World * glm::vec4(bounds.Center(), 1.0f));
            item.Extents = glm::abs(m[0]) * ext.x + glm::abs(m[1]) * ext.y + glm::abs(m[2]) * ext.z;
            item.EntityId = e.Index + 1;
            item.CastShadows = mr.CastShadows;
            item.Fade = lodFade;
            item.MotionVectors = (uint8_t)mr.MotionVectors;
            partSubMesh.clear();
            // Reflection probes are chosen and blended per pixel in the shader (item.Probe stays 0).

            // Skinned meshes use the Animator's matrices (uploaded once, shared by every pass); without an
            // Animator they render in bind pose.
            const AnimatorComponent* animator = mesh->IsSkinned() ? reg.TryGet<AnimatorComponent>(e) : nullptr;
            bool skinned = animator && animator->SkinMatrices.size() == mesh->GetSkeleton()->Joints.size() && mesh->GetSkinAddress();
            item.AlwaysVisible = skinned || ocean;
            if (skinned)
            {
                size_t bytes = animator->SkinMatrices.size() * sizeof(glm::mat4);
                if (TransientAllocation joints = device.AllocateTransient(bytes, 16))
                {
                    std::memcpy(joints.Data, animator->SkinMatrices.data(), bytes);
                    item.Joints = joints.Address;
                    item.Skin = mesh->GetSkinAddress();
                    item.JointMatrices = &animator->SkinMatrices;
                }
            }

            // Material per sub-mesh: the renderer's per-slot override (Materials[slot], where sub-meshes without a
            // model slot use their own index); else the renderer's Material for all parts; else the model's material
            // for the slot; else the renderer's simple color/texture fields.
            item.FirstPart = (uint32_t)m_RenderParts.size();
            Ref<Material> overrideMat = AssetManager::GetMaterial(mr.Material);
            const auto& slots = AssetManager::GetMeshMaterials(mr.Mesh);
            const auto& subMeshes = mesh->GetSubMeshes();
            // Fur: each part also gets a shell material (its own, with the strand settings), drawn below.
            const FurComponent* fur = terrainRt || item.Vertices ? nullptr : reg.TryGet<FurComponent>(e);
            if (fur && (fur->Length <= 0.0f || fur->Shells <= 0)) fur = nullptr;
            std::vector<VkDeviceAddress> furMaterials;
            for (size_t si = 0; si < subMeshes.size(); ++si)
            {
                const SubMesh& sub = subMeshes[si];
                const Material* matPtr = nullptr;
                Ref<Material> slotMat;
                size_t slotIndex = sub.MaterialSlot >= 0 ? (size_t)sub.MaterialSlot : si;
                if (slotIndex < mr.Materials.size() && mr.Materials[slotIndex])
                {
                    slotMat = AssetManager::GetMaterial(mr.Materials[slotIndex]);
                    matPtr = slotMat.get();
                }
                if (!matPtr) matPtr = overrideMat.get();
                if (!matPtr && sub.MaterialSlot >= 0 && sub.MaterialSlot < (int32_t)slots.size())
                {
                    slotMat = AssetManager::GetMaterial(slots[sub.MaterialSlot]);
                    matPtr = slotMat.get();
                }
                if (!matPtr) matPtr = &ResolveMaterial(mr, scratch);
                // The renderer's Tint on top of whichever material it draws (this object only).
                Material tinted;
                if (mr.Tint != glm::vec4(1.0f))
                {
                    tinted = *matPtr;
                    tinted.BaseColor *= mr.Tint;
                    matPtr = &tinted;
                }
                const Material& mat = *matPtr;
                VkDeviceAddress address;
                if (terrainRt)
                {
                    // Terrain: its layers (each its texture, color and splat map channel) in place of the material's maps
                    // (the material's other values stay).
                    MaterialGPU gpu = BuildMaterialGPU(mat);
                    uint32_t white = AssetManager::GetWhiteTexture()->GetBindlessIndex();
                    gpu.BaseColor = glm::vec4(1.0f);
                    gpu.Flags = glm::uvec4(0u, 0u, 1u, 0u);
                    int layers = std::min(Terrain::LayerCount(*terrain), (int)terrainRt->Splats.size() * 4);
                    TransientAllocation la = device.AllocateTransient(sizeof(TerrainLayerGPU) * (size_t)std::max(layers, 1), 16);
                    if (!la) continue;
                    auto* out = static_cast<TerrainLayerGPU*>(la.Data);
                    for (int k = 0; k < layers; ++k)
                    {
                        TerrainLayer fallback;
                        const TerrainLayer& layer = k < (int)terrain->Layers.size() ? terrain->Layers[k] : fallback;
                        glm::vec3 color = glm::vec3(ToLinear(layer.Color)) * (dark == 1 ? 0.15f : dark == 2 ? 0.4f : 1.0f);
                        out[k].Color = glm::vec4(color, std::max(layer.Tiling, 1e-4f));
                        out[k].Maps = glm::uvec4(TextureIndex(layer.Texture, white), terrainRt->Splats[k / 4]->GetBindlessIndex(), (uint32_t)(k % 4),
                                                 TextureIndex(layer.NormalMap, ~0u));
                        out[k].Params = glm::vec4(layer.NormalStrength, layer.Triplanar ? 1.0f : 0.0f, 0.0f, 0.0f);
                    }
                    float heightBlend = std::clamp(terrain->HeightBlend, 0.0f, 1.0f);
                    uint32_t blendBits;
                    std::memcpy(&blendBits, &heightBlend, 4);
                    gpu.Maps2 = glm::uvec4((uint32_t)la.Address, (uint32_t)(la.Address >> 32), (uint32_t)layers, blendBits);
                    address = uploadMaterialGPU(dim(gpu, dark));
                }
                else if (water)
                {
                    // Water: the shader draws foam where the vertex normals say (Flags.w = 2).
                    MaterialGPU gpu = BuildMaterialGPU(mat);
                    gpu.Flags.w = 2u;
                    gpu.Layers[0] = glm::vec4(std::max(water->ShoreFoam, 0.0f), 0.0f, 0.0f, 0.0f); // shore foam reach (m)
                    if (!floatingBlock) floatingBlock = device.AllocateTransient(sizeof(glm::uvec4) + kMaxFloating * sizeof(glm::mat4), 16);
                    if (floatingBlock)
                    {
                        std::memset(floatingBlock.Data, 0, sizeof(glm::uvec4)); // none until filled
                        gpu.Maps2 = glm::uvec4((uint32_t)floatingBlock.Address, (uint32_t)(floatingBlock.Address >> 32), 0u, 0u);
                    }
                    address = uploadMaterialGPU(dim(gpu, dark));
                    m_FrameHasWater = true;
                }
                else
                    address = uploadMaterial(mat, dark);
                if (!address) continue;
                if (fur)
                {
                    MaterialGPU shell = BuildMaterialGPU(mat);
                    shell.Flags.w = 1u;
                    shell.Layers[0] = glm::vec4(std::max(fur->Density, 1.0f), std::clamp(fur->Thickness, 0.01f, 1.0f), std::max(fur->Droop, 0.0f), fur->WindResponse);
                    shell.Layers[1] = glm::vec4(glm::vec3(fur->TipColor), std::clamp(fur->RootShade, 0.0f, 1.0f));
                    furMaterials.push_back(uploadMaterialGPU(dim(shell, dark)));
                }
                bool transparent = mat.BlendMode == MaterialBlendMode::Transparent;
                m_FrameRefracts |= transparent && mat.Refraction > 0.0f;
                bool doubleSided = mat.DoubleSided || cloth; // cloth is seen from both sides
                // Every part can batch: skinned instances carry their own skinning matrices, and transparent parts
                // merge with same-batch neighbours in the back-to-front order. Parts with their own vertices cannot.
                uint32_t batch = batchOf({ mesh.get(), sub.IndexOffset, sub.IndexCount, address, doubleSided, item.Probe }, !item.Vertices && item.EntityId < kInstanceIdLimit);
                partSubMesh.push_back((uint32_t)si);
                if (item.Indices) // its own triangles: one part over all of them
                    m_RenderParts.push_back({ 0u, item.IndexCount, address, transparent, doubleSided, mat.CastShadows, (uint32_t)m_RenderItems.size(), batch });
                else
                    m_RenderParts.push_back({ sub.IndexOffset, sub.IndexCount, address, transparent, doubleSided, mat.CastShadows,
                                              (uint32_t)m_RenderItems.size(), batch });
                if (item.Indices) break;
            }
            if (water)
                for (size_t p = item.FirstPart; p < m_RenderParts.size(); ++p) m_RenderParts[p].Water = true;
            item.PartCount = (uint32_t)m_RenderParts.size() - item.FirstPart;
            item.MeshRef = std::move(mesh);
            if (terrainRt && !terrainRt->Chunks.empty() && item.PartCount == 1)
            {
                // Terrain: an item per chunk (each culled by every pass on its own bounds), drawn at the coarsest level
                // whose error looks no bigger than kTerrainErrorPixels on a 1080-line view from this camera. Shadows
                // draw the same levels.
                constexpr float kTerrainErrorPixels = 2.0f;
                RenderPart part = m_RenderParts[item.FirstPart];
                m_RenderParts.resize(item.FirstPart);
                float heightScale = glm::length(glm::vec3(t.World[1]));
                float pixelsPerUnit = 540.0f * std::abs(camera.Projection[1][1]); // at a distance of 1 (perspective)
                for (const Terrain::Chunk& chunk : terrainRt->Chunks)
                {
                    if (chunk.Count[0] == 0) continue; // all holes
                    glm::vec3 half = (chunk.Max - chunk.Min) * 0.5f;
                    RenderItem piece = item;
                    piece.Center = glm::vec3(t.World * glm::vec4(chunk.Min + half, 1.0f));
                    piece.Extents = glm::abs(m[0]) * half.x + glm::abs(m[1]) * half.y + glm::abs(m[2]) * half.z;
                    float distance = camera.Orthographic ? 1.0f : std::max(glm::length(glm::max(glm::abs(camera.Position - piece.Center) - piece.Extents, glm::vec3(0.0f))), 1e-3f);
                    int level = 0;
                    while (level + 1 < chunk.Levels && chunk.Error[level + 1] * heightScale * pixelsPerUnit / distance <= kTerrainErrorPixels) ++level;
                    piece.FirstPart = (uint32_t)m_RenderParts.size();
                    piece.PartCount = 1;
                    RenderPart p = part;
                    p.FirstIndex = chunk.First[level];
                    p.IndexCount = chunk.Count[level];
                    p.Item = (uint32_t)m_RenderItems.size();
                    p.Batch = batchOf({ piece.MeshRef.get(), p.FirstIndex, p.IndexCount, p.Material, p.DoubleSided, piece.Probe }, true);
                    m_RenderParts.push_back(p);
                    m_RenderItems.push_back(std::move(piece));
                }
                continue;
            }
            m_RenderItems.push_back(std::move(item));
            size_t mainItem = m_RenderItems.size() - 1;
            if (fur && furMaterials.size() == m_RenderItems.back().PartCount)
            {
                // The shells: copies of the item, each pushed out to its height (they batch into instanced draws).
                int shells = std::clamp(fur->Shells, 1, 64);
                for (int k = 1; k <= shells; ++k)
                {
                    RenderItem shell = m_RenderItems.back();
                    shell.Motion = glm::packHalf2x16(glm::vec2(fur->Length, -(float)k / shells));
                    shell.FurWind = glm::packHalf2x16(glm::vec2(std::max(fur->Droop, 0.0f), fur->WindResponse));
                    shell.Extents += glm::vec3(fur->Length * 2.0f);
                    shell.FirstPart = (uint32_t)m_RenderParts.size();
                    for (uint32_t p = 0; p < shell.PartCount; ++p)
                    {
                        RenderPart part = m_RenderParts[m_RenderItems.back().FirstPart + p];
                        part.Material = furMaterials[p];
                        part.Transparent = false;
                        part.Item = (uint32_t)m_RenderItems.size();
                        part.Batch = batchOf({ shell.MeshRef.get(), part.FirstIndex, part.IndexCount, part.Material, part.DoubleSided, shell.Probe }, true);
                        m_RenderParts.push_back(part);
                    }
                    m_RenderItems.push_back(std::move(shell));
                }
            }
            // The next generated level, sharing the pixels while they cross-fade (dithered the other way).
            if (own && own->Next >= 0 && !m_RenderItems[mainItem].Vertices && !m_RenderItems[mainItem].Indices)
                if (Ref<Mesh> nextMesh = AssetManager::GetMesh(LODGroups::GeneratedMesh(mr.Mesh, own->Next, reduction, false)))
                {
                    RenderItem next = m_RenderItems[mainItem];
                    next.Fade = own->NextFade;
                    next.FirstPart = (uint32_t)m_RenderParts.size();
                    const auto& nextSubs = nextMesh->GetSubMeshes();
                    for (uint32_t p = 0; p < next.PartCount && p < partSubMesh.size(); ++p)
                    {
                        if (partSubMesh[p] >= nextSubs.size()) continue;
                        RenderPart part = m_RenderParts[m_RenderItems[mainItem].FirstPart + p];
                        part.FirstIndex = nextSubs[partSubMesh[p]].IndexOffset;
                        part.IndexCount = nextSubs[partSubMesh[p]].IndexCount;
                        part.Item = (uint32_t)m_RenderItems.size();
                        part.Batch = batchOf({ nextMesh.get(), part.FirstIndex, part.IndexCount, part.Material, part.DoubleSided, next.Probe }, next.EntityId < kInstanceIdLimit);
                        m_RenderParts.push_back(part);
                    }
                    next.PartCount = (uint32_t)m_RenderParts.size() - next.FirstPart;
                    next.MeshRef = std::move(nextMesh);
                    m_RenderItems.push_back(std::move(next));
                }
        }

        // The floating things' boxes for the water shader: the nearest to the camera first.
        if (floatingBlock)
        {
            std::vector<std::pair<float, uint32_t>> floating;
            for (uint32_t i = 0; i < (uint32_t)m_RenderItems.size(); ++i)
                if (m_RenderItems[i].Floating && !m_RenderItems[i].Vertices && m_RenderItems[i].MeshRef)
                    floating.push_back({ glm::distance(m_RenderItems[i].Center, camera.Position), i });
            size_t count = std::min<size_t>(floating.size(), kMaxFloating);
            std::partial_sort(floating.begin(), floating.begin() + count, floating.end());
            auto* boxes = (glm::mat4*)((char*)floatingBlock.Data + sizeof(glm::uvec4));
            for (size_t k = 0; k < count; ++k)
            {
                // World points into the mesh's box, grown a little (so its surface counts as inside), from -1 to 1.
                const RenderItem& item = m_RenderItems[floating[k].second];
                AABB local = item.MeshRef->GetBounds();
                glm::vec3 half = glm::max(local.Extents() * 1.05f + 0.05f, glm::vec3(1e-3f));
                boxes[k] = glm::inverse(item.World * glm::translate(glm::mat4(1.0f), local.Center()) * glm::scale(glm::mat4(1.0f), half));
            }
            glm::uvec4 header((uint32_t)count, 0u, 0u, 0u);
            std::memcpy(floatingBlock.Data, &header, sizeof(header));
        }

        // Impostors: a quad turned to the camera (about the vertical), showing the atlas tile of the side nearest the
        // camera's, cut out where nothing was and lit live: the tile's normals (in that side's view, which is the
        // quad's tangent frame) are its normal map.
        // Their shadows: a second quad, turned to the sun and showing the side nearest it, drawn only into shadow maps.
        bool impostorShadows = false;
        glm::vec3 toSun(0.0f, 1.0f, 0.0f);
        if (!lods.Impostors.empty())
            for (auto [e, light, t] : reg.ViewOf<DirectionalLightComponent, TransformComponent>())
                if (t.ActiveInHierarchy)
                {
                    impostorShadows = light.CastShadows;
                    toSun = -t.Forward();
                    break;
                }
        if (!lods.Impostors.empty())
            if (Ref<Mesh> quad = AssetManager::GetMesh(AssetManager::kQuad))
                for (const auto& [g, fade] : lods.Impostors)
                {
                    auto of = m_ImpostorOf.find(ImpostorKey(g));
                    auto it = of == m_ImpostorOf.end() ? m_Impostors.end() : m_Impostors.find(of->second);
                    if (it == m_Impostors.end() || !it->second.Bindless || !it->second.NormalBindless || !it->second.SurfaceBindless || it->second.Radius <= 0.0f ||
                        it->second.Owner != &scene)
                        continue;
                    const Impostor& impostor = it->second;
                    const auto& t = reg.Get<TransformComponent>(g);
                    float s = std::max({ glm::length(glm::vec3(t.World[0])), glm::length(glm::vec3(t.World[1])), glm::length(glm::vec3(t.World[2])) });
                    glm::vec3 center = glm::vec3(t.World * glm::vec4(impostor.Center, 1.0f));
                    float radius = impostor.Radius * s;
                    glm::vec3 toCamera = camera.Position - center;
                    float yaw = std::atan2(toCamera.x, toCamera.z);
                    // Tilted up toward a camera looking down on it (the picture from the row nearest its elevation).
                    float elevation = std::atan2(toCamera.y, std::max(glm::length(glm::vec2(toCamera.x, toCamera.z)), 1e-4f));
                    elevation = std::clamp(elevation, 0.0f, glm::radians(80.0f));
                    uint32_t row = ImpostorRow(glm::degrees(elevation));
                    glm::vec3 forward = glm::vec3(t.World[2]);
                    float local = yaw - std::atan2(forward.x, forward.z);
                    int side = ((int)std::lround(local / (glm::two_pi<float>() / kImpostorSides)) % (int)kImpostorSides + (int)kImpostorSides) % (int)kImpostorSides;
                    RenderItem item;
                    item.World = glm::translate(glm::mat4(1.0f), center) * glm::rotate(glm::mat4(1.0f), yaw, glm::vec3(0.0f, 1.0f, 0.0f)) *
                                 glm::rotate(glm::mat4(1.0f), -elevation, glm::vec3(1.0f, 0.0f, 0.0f)) *
                                 glm::scale(glm::mat4(1.0f), glm::vec3(radius * 2.0f, radius * 2.0f, 1.0f));
                    item.Center = center;
                    item.Extents = glm::vec3(radius);
                    item.EntityId = g.Index + 1;
                    item.AlwaysVisible = false;
                    item.CastShadows = false;
                    item.Fade = fade;
                    uint32_t white = AssetManager::GetWhiteTexture()->GetBindlessIndex();
                    MaterialGPU gpu{};
                    gpu.BaseColor = glm::vec4(1.0f);
                    gpu.Emission = glm::vec4(0.0f);
                    gpu.Params = glm::vec4(1.0f, 1.0f, 0.5f, 1.0f); // metallic and roughness from the surface atlas
                    gpu.TilingOffset = glm::vec4(1.0f / kImpostorSides, 1.0f / kImpostorRows, (float)side / kImpostorSides, (float)row / kImpostorRows);
                    gpu.Maps = glm::uvec4(impostor.Bindless, impostor.NormalBindless, impostor.SurfaceBindless, white);
                    gpu.Flags = glm::uvec4(1u, 1u, 0u, 0u); // cutout, normal map
                    gpu.Layers[0] = glm::vec4(radius, 0.0f, 0.0f, 0.0f); // the depth atlas's unit (MeshImpostor.frag)
                    VkDeviceAddress address = uploadMaterialGPU(dim(gpu, dimmed(g)));
                    if (!address) continue;
                    const SubMesh& sub = quad->GetSubMeshes()[0];
                    item.FirstPart = (uint32_t)m_RenderParts.size();
                    uint32_t batch = batchOf({ quad.get(), sub.IndexOffset, sub.IndexCount, address, true, 0u }, true);
                    m_RenderParts.push_back({ sub.IndexOffset, sub.IndexCount, address, false, true, false, (uint32_t)m_RenderItems.size(), batch });
                    m_RenderParts.back().Impostor = true;
                    item.PartCount = 1;
                    item.MeshRef = quad;
                    RenderItem shadow = item;
                    m_RenderItems.push_back(std::move(item));
                    if (!impostorShadows) continue;
                    float sunYaw = std::atan2(toSun.x, toSun.z); // (0 with the sun straight overhead: any side does)
                    float sunElevation = std::clamp(std::atan2(toSun.y, glm::length(glm::vec2(toSun.x, toSun.z))), 0.0f, glm::radians(80.0f));
                    uint32_t sunRow = ImpostorRow(glm::degrees(sunElevation));
                    float sunLocal = sunYaw - std::atan2(forward.x, forward.z);
                    int sunSide = ((int)std::lround(sunLocal / (glm::two_pi<float>() / kImpostorSides)) % (int)kImpostorSides + (int)kImpostorSides) %
                                  (int)kImpostorSides;
                    shadow.World = glm::translate(glm::mat4(1.0f), center) * glm::rotate(glm::mat4(1.0f), sunYaw, glm::vec3(0.0f, 1.0f, 0.0f)) *
                                   glm::rotate(glm::mat4(1.0f), -sunElevation, glm::vec3(1.0f, 0.0f, 0.0f)) *
                                   glm::scale(glm::mat4(1.0f), glm::vec3(radius * 2.0f, radius * 2.0f, 1.0f));
                    shadow.CastShadows = true;
                    shadow.ShadowOnly = true;
                    gpu.TilingOffset.z = (float)sunSide / kImpostorSides;
                    gpu.TilingOffset.w = (float)sunRow / kImpostorRows;
                    VkDeviceAddress shadowAddress = uploadMaterialGPU(dim(gpu, dimmed(g)));
                    if (!shadowAddress) continue;
                    shadow.FirstPart = (uint32_t)m_RenderParts.size();
                    uint32_t shadowBatch = batchOf({ quad.get(), sub.IndexOffset, sub.IndexCount, shadowAddress, true, 0u }, true);
                    m_RenderParts.push_back({ sub.IndexOffset, sub.IndexCount, shadowAddress, false, true, true, (uint32_t)m_RenderItems.size(), shadowBatch });
                    m_RenderParts.back().Impostor = true;
                    m_RenderItems.push_back(std::move(shadow));
                }

        // Terrain details: an instance of the tree or grass mesh wherever one was scattered (they batch into
        // instanced draws; picking one selects the terrain).
        for (auto [e, terrain, t] : reg.ViewOf<TerrainComponent, TransformComponent>())
        {
            if (!t.ActiveInHierarchy) continue;
            int dark = dimmed(e);
            const Terrain::DetailInstances& details = Terrain::GetDetails(terrain, TerrainKey(terrain));
            const auto* terrainRenderer = reg.TryGet<MeshRendererComponent>(e);
            for (int kind = 0; kind < 2; ++kind)
            {
                const std::vector<glm::mat4>& list = kind == 0 ? details.Trees : details.Grass;
                AssetHandle meshHandle = kind == 0 ? terrain.TreeMesh : terrain.GrassMesh;
                Ref<Mesh> mesh = list.empty() ? nullptr : AssetManager::GetMesh(meshHandle);
                if (!mesh) continue;
                Ref<Material> overrideMat = AssetManager::GetMaterial(kind == 0 ? terrain.TreeMaterial : terrain.GrassMaterial);
                const auto& slots = AssetManager::GetMeshMaterials(meshHandle);
                // Grass tufts sway in the wind and bend away from bodies (in the vertex shader).
                uint32_t motion = kind == 1 && (terrain.GrassSway > 0.0f || terrain.GrassBending > 0.0f)
                                      ? glm::packHalf2x16(glm::vec2(terrain.GrassSway, terrain.GrassBending)) : 0u;
                struct DetailPart
                {
                    uint32_t FirstIndex, IndexCount;
                    VkDeviceAddress Address;
                    bool Transparent, DoubleSided, CastShadows;
                    uint32_t Batch;
                };
                std::vector<DetailPart> parts;
                static const Material kDefaultMaterial;
                for (const SubMesh& sub : mesh->GetSubMeshes())
                {
                    Ref<Material> slotMat;
                    const Material* mat = overrideMat.get();
                    if (!mat && sub.MaterialSlot >= 0 && sub.MaterialSlot < (int32_t)slots.size())
                    {
                        slotMat = AssetManager::GetMaterial(slots[sub.MaterialSlot]);
                        mat = slotMat.get();
                    }
                    if (!mat) mat = &kDefaultMaterial;
                    m_FrameRefracts |= mat->BlendMode == MaterialBlendMode::Transparent && mat->Refraction > 0.0f;
                    VkDeviceAddress address = uploadMaterial(*mat, dark);
                    if (!address) continue;
                    uint32_t batch = batchOf({ mesh.get(), sub.IndexOffset, sub.IndexCount, address, mat->DoubleSided, 0u }, true);
                    parts.push_back({ sub.IndexOffset, sub.IndexCount, address, mat->BlendMode == MaterialBlendMode::Transparent, mat->DoubleSided,
                                      mat->CastShadows, batch });
                }
                AABB bounds = mesh->GetBounds();
                glm::vec3 ext = bounds.Extents();
                for (const glm::mat4& local : list)
                {
                    RenderItem item;
                    item.World = t.World * local;
                    glm::mat3 m(item.World);
                    item.Center = glm::vec3(item.World * glm::vec4(bounds.Center(), 1.0f));
                    item.Extents = glm::abs(m[0]) * ext.x + glm::abs(m[1]) * ext.y + glm::abs(m[2]) * ext.z;
                    item.EntityId = e.Index + 1;
                    item.AlwaysVisible = false;
                    item.CastShadows = terrainRenderer ? terrainRenderer->CastShadows : true;
                    item.Motion = motion;
                    if (motion) item.Extents += glm::vec3(1.0f, 0.0f, 1.0f); // swaying tips reach past the mesh's bounds
                    item.FirstPart = (uint32_t)m_RenderParts.size();
                    for (const DetailPart& p : parts)
                        m_RenderParts.push_back({ p.FirstIndex, p.IndexCount, p.Address, p.Transparent, p.DoubleSided, p.CastShadows, (uint32_t)m_RenderItems.size(), p.Batch });
                    item.PartCount = (uint32_t)parts.size();
                    item.MeshRef = mesh;
                    m_RenderItems.push_back(std::move(item));
                }
            }
        }
        m_Stats.RenderListItems = (uint32_t)m_RenderItems.size();
        // Visual Effects drawn as meshes: a part per submesh, each drawn once for all the effect's slots.
        m_VfxMeshParts.clear();
        for (const VfxMeshOutput& out : m_VfxMeshOutputs)
        {
            static const Material kDefaultMaterial;
            Ref<Material> overrideMat = AssetManager::GetMaterial(out.Material);
            const auto& slots = AssetManager::GetMeshMaterials(out.MeshHandle);
            for (const SubMesh& sub : out.MeshRef->GetSubMeshes())
            {
                Ref<Material> slotMat;
                const Material* mat = overrideMat.get();
                if (!mat && sub.MaterialSlot >= 0 && sub.MaterialSlot < (int32_t)slots.size())
                {
                    slotMat = AssetManager::GetMaterial(slots[sub.MaterialSlot]);
                    mat = slotMat.get();
                }
                if (!mat) mat = &kDefaultMaterial;
                if (VkDeviceAddress address = uploadMaterial(*mat, 0))
                    m_VfxMeshParts.push_back({ out.MeshRef, sub.IndexOffset, sub.IndexCount, address, mat->DoubleSided, mat->CastShadows, out.Instances,
                                               out.Capacity, out.EntityId });
            }
        }
        m_Stats.ListMs = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - listStart).count();
    }

    bool SceneRenderer::DebugDraw(const char* pass, const char* target, const std::string& what, uint32_t entity, uint32_t instances, uint32_t triangles,
                                  uint64_t material)
    {
        if (!m_FrameCapturing) return true;
        int index = (int)m_FrameEvents.size();
        FrameDebugEvent ev;
        if (auto it = m_FrameMaterials.find(material); material && it != m_FrameMaterials.end()) ev = it->second;
        ev.Pass = pass;
        ev.Target = target;
        ev.What = what;
        ev.EntityId = entity;
        ev.Instances = instances;
        ev.Triangles = triangles;
        m_FrameEvents.push_back(std::move(ev));
        return m_FrameStop < 0 || index <= m_FrameStop;
    }

    void SceneRenderer::RenderFramePreview(VkCommandBuffer cmd, SceneRenderTarget& target)
    {
        // The target of the last draw kept: a shadow map (depth) or the HDR image (before tonemapping).
        m_FramePreviewShown = false;
        if (m_FrameEvents.empty()) return;
        const FrameDebugEvent& last = m_FrameEvents[m_FrameStop < 0 ? m_FrameEvents.size() - 1 : std::min<size_t>(m_FrameStop, m_FrameEvents.size() - 1)];
        uint32_t source = 0, mode = 0;
        glm::uvec2 size{ 0 };
        if (last.Target == "Shadow Map" && m_ShadowMap)
        {
            source = m_ShadowBindless;
            size = { ShadowMapSize(), ShadowMapSize() };
        }
        else if (last.Target == "Point Shadow Map" && m_PointShadowMap)
        {
            source = m_PointShadowBindless;
            size = { m_PointShadowMap->GetWidth(), m_PointShadowMap->GetHeight() };
        }
        else if (last.Target == "HDR Color" || last.Target == "OIT Accumulation")
        {
            source = target.GetHdrBindless();
            mode = 1;
            size = { target.GetHdr().GetWidth(), target.GetHdr().GetHeight() };
        }
        if (!source) return;
        if (!m_FramePreview)
        {
            m_FramePreview = MakeScope<Image>(ImageDesc{ 512, 512, SceneRenderTarget::kOutputFormat, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, 1,
                                                         "FramePreview" });
            Vk::TransitionImage(cmd, m_FramePreview->GetHandle(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            m_FramePreviewImGui = ImGuiLayer::AddTexture(RenderDevice::Get().GetClampSampler(), m_FramePreview->GetView());
        }
        struct { uint32_t Source, Mode; } push{ source, mode };
        RenderDevice::Get().BindBindless(cmd);
        FullscreenPass(cmd, *m_FramePreview, false, Renderer::Get().FramePreview, &push, sizeof(push));
        m_Stats.DrawCalls--; // not the scene's
        m_FramePreviewShown = true;
        m_FramePreviewSize = size;
    }

    void SceneRenderer::DrawMeshes(VkCommandBuffer cmd, const glm::mat4& viewProj, MeshPass pass)
    {
        bool shadowPass = pass == MeshPass::Shadow, depthPass = pass == MeshPass::Depth, oitPass = pass == MeshPass::Transparent;
        auto& device = RenderDevice::Get();
        auto& res = Renderer::Get();
        Frustum frustum(viewProj);

        struct Draw
        {
            Mesh* MeshPtr;
            uint32_t FirstIndex = 0, IndexCount = 0;
            MeshPush Push;
            const Pipeline* Pipe;
            float Depth;
            uint32_t Part = 0; // transparent draws: the part, for merging same-batch neighbours
            VkBuffer Vertices = VK_NULL_HANDLE; // vertices other than the mesh's (cloth)
            VkDeviceSize VertexOffset = 0;
            VkBuffer Indices = VK_NULL_HANDLE;  // and indices (torn cloth)
            VkDeviceSize IndexOffset = 0;
        };
        std::vector<Draw> transparent;
        const Pipeline* bound = nullptr;
        Mesh* boundMesh = nullptr;
        const char* passName = shadowPass ? "Shadows" : depthPass ? "Depth Prepass" : oitPass ? "Transparent (OIT)" : "Opaque";
        const char* targetName = shadowPass ? m_ShadowTargetName : depthPass ? "Depth" : oitPass ? "OIT Accumulation" : "HDR Color";
        auto submit = [&](const Draw& d, uint32_t instances) {
            if (!DebugDraw(passName, targetName, d.MeshPtr->GetName(), d.Push.EntityId, instances, d.IndexCount / 3 * instances, d.Push.Material)) return;
            if (d.Pipe != bound)
            {
                d.Pipe->Bind(cmd);
                bound = d.Pipe;
            }
            vkCmdPushConstants(cmd, device.GetPipelineLayout(), VK_SHADER_STAGE_ALL, 0, sizeof(MeshPush), &d.Push);
            if (d.MeshPtr != boundMesh)
            {
                d.MeshPtr->Bind(cmd);
                boundMesh = d.MeshPtr;
            }
            if (d.Vertices)
            {
                vkCmdBindVertexBuffers(cmd, 0, 1, &d.Vertices, &d.VertexOffset);
                boundMesh = nullptr; // the next draw binds its mesh's vertices again
            }
            if (d.Indices) vkCmdBindIndexBuffer(cmd, d.Indices, d.IndexOffset, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(cmd, d.IndexCount, instances, d.FirstIndex, 0, 0);
            m_Stats.DrawCalls++;
            if (shadowPass) m_Stats.ShadowCasters += instances;
            else if (!depthPass)
            {
                m_Stats.Meshes += instances;
                m_Stats.Triangles += d.IndexCount / 3 * instances;
                if (instances > 1) m_Stats.InstancedMeshes += instances;
            }
        };

        // Unpickable objects (Scene visibility) draw without their id.
        auto idOf = [&](const RenderItem& item) { return m_UnpickableIds.empty() || !m_UnpickableIds.contains(item.EntityId) ? item.EntityId : 0u; };
        auto InstanceOf = [&](const RenderItem& item) {
            // The id's top 9 bits carry the LOD cross-fade (as MeshPush.Flags bits 8-16), so fading copies batch too.
            return InstanceGPU{ item.World, glm::uvec4(idOf(item) | (item.Fade << 23), (uint32_t)item.Joints, (uint32_t)(item.Joints >> 32), item.Motion) };
        };
        // The Scene view's draw modes swap the main pass's mesh pipelines.
        bool mainPass = pass == MeshPass::Main || pass == MeshPass::Transparent;
        const Pipeline* modePipe = !mainPass ? nullptr
                                 : m_DrawMode == SceneDrawMode::Wireframe ? &MainPipeline(MainPipelines::MeshWire)
                                 : m_DrawMode == SceneDrawMode::Overdraw  ? &MainPipeline(MainPipelines::MeshOverdraw)
                                                                          : nullptr;
        std::vector<std::pair<Draw, uint32_t>> wireOverlay; // Shaded Wireframe: the opaque draws again, as edges
        bool overlayEdges = pass == MeshPass::Main && m_DrawMode == SceneDrawMode::ShadedWireframe;
        bool prepassed = pass == MeshPass::Main && m_DepthPrepassed; // (the opaque parts' depth is in place)
        glm::vec3 camPos = glm::vec3(glm::inverse(viewProj) * glm::vec4(0, 0, 0, 1)); // only used for sorting
        auto makeDraw = [&](const RenderPart& part) {
            const RenderItem& item = m_RenderItems[part.Item];
            Draw d;
            d.MeshPtr = item.MeshRef.get();
            d.FirstIndex = part.FirstIndex;
            d.IndexCount = part.IndexCount;
            d.Push.Model = item.World;
            d.Push.Material = part.Material;
            d.Push.EntityId = idOf(item);
            d.Push.Skin = item.Joints ? item.Skin : 0;
            d.Push.Joints = item.Joints;
            d.Push.Instances = 0;
            d.Push.Flags = (item.Joints ? 1u : 0u) | (item.Fade << 8);
            d.Push.Motion = item.Motion;
            d.Vertices = item.Vertices;
            d.VertexOffset = item.VertexOffset;
            d.Indices = item.Indices;
            d.IndexOffset = item.IndexOffset;
            d.Pipe = shadowPass  ? (part.Impostor ? &res.ShadowImpostor : &res.Shadow)
                   : oitPass     ? &res.MeshOit
                   : depthPass   ? &MainPipeline(part.Impostor ? MainPipelines::DepthPrepassImpostor
                                                   : part.DoubleSided ? MainPipelines::DepthPrepassDoubleSided : MainPipelines::DepthPrepass)
                   : modePipe    ? modePipe
                   : &MainPipeline(part.Transparent ? MainPipelines::MeshTransparent : part.Impostor ? MainPipelines::MeshImpostor
                                   : prepassed && !part.Water ? (part.DoubleSided ? MainPipelines::MeshDoubleSidedPrepassed : MainPipelines::MeshPrepassed)
                                   : part.DoubleSided ? MainPipelines::MeshDoubleSided : MainPipelines::Mesh);
            return d;
        };

        // Visible opaque parts, bucketed by batch with a counting sort (no comparisons, no per-draw structs).
        size_t batchCount = m_BatchInstanceable.size();
        m_VisibleParts.clear();
        m_BatchStart.assign(batchCount + 1, 0);
        for (const RenderItem& item : m_RenderItems)
        {
            if (!m_HiddenIds.empty() && m_HiddenIds.contains(item.EntityId)) continue; // hidden in the Scene view
            if (shadowPass && !item.CastShadows) continue;
            if (!shadowPass && item.ShadowOnly) continue;
            // Hidden behind others (occlusion culling): not drawn in the view, but it still casts its shadow.
            if (!shadowPass && m_ItemOccluded.size() == m_RenderItems.size() && m_ItemOccluded[&item - m_RenderItems.data()])
            {
                if (pass == MeshPass::Main) m_Stats.Occluded++;
                continue;
            }
            if (!item.AlwaysVisible && !frustum.IntersectsWorld(item.Center, item.Extents))
            {
                if (pass == MeshPass::Main) m_Stats.Culled++;
                continue;
            }
            for (uint32_t p = item.FirstPart; p < item.FirstPart + item.PartCount; ++p)
            {
                const RenderPart& part = m_RenderParts[p];
                if (shadowPass && (!part.CastShadows || part.Transparent)) continue;
                if (depthPass && part.Water) continue; // it reads the depth under it
                if (part.Transparent)
                {
                    if (depthPass || (pass == MeshPass::Main && m_Oit)) continue; // OIT draws them in their own pass
                    Draw d = makeDraw(part);
                    d.Depth = glm::distance(item.Center, camPos);
                    d.Part = p;
                    transparent.push_back(d);
                    continue;
                }
                if (oitPass) continue; // opaque parts were drawn in the main pass
                m_VisibleParts.push_back(p);
                m_BatchStart[part.Batch + 1]++;
            }
        }
        for (size_t b = 0; b < batchCount; ++b) m_BatchStart[b + 1] += m_BatchStart[b];
        m_BatchFill.assign(m_BatchStart.begin(), m_BatchStart.end() - 1);
        m_SortedParts.resize(m_VisibleParts.size());
        for (uint32_t p : m_VisibleParts) m_SortedParts[m_BatchFill[m_RenderParts[p].Batch]++] = p;

        // One transient block holds the instance data of every instanced draw in this pass.
        size_t instancedTotal = 0;
        if (s_InstancingEnabled)
            for (size_t b = 0; b < batchCount; ++b)
                if (uint32_t n = m_BatchStart[b + 1] - m_BatchStart[b]; n > 1 && m_BatchInstanceable[b]) instancedTotal += n;
        TransientAllocation instanceBlock = instancedTotal ? device.AllocateTransient(instancedTotal * sizeof(InstanceGPU), 16) : TransientAllocation{};
        auto* instanceData = instanceBlock ? (InstanceGPU*)instanceBlock.Data : nullptr;
        size_t instanceCursor = 0;

        for (size_t b = 0; b < batchCount; ++b)
        {
            uint32_t first = m_BatchStart[b], count = m_BatchStart[b + 1] - first;
            if (count == 0) continue;
            if (count > 1 && m_BatchInstanceable[b] && instanceData)
            {
                for (uint32_t done = 0; done < count;)
                {
                    uint32_t n = std::min(count - done, kMaxInstancesPerDraw);
                    InstanceGPU* items = instanceData + instanceCursor;
                    for (uint32_t k = 0; k < n; ++k) items[k] = InstanceOf(m_RenderItems[m_RenderParts[m_SortedParts[first + done + k]].Item]);
                    Draw batch = makeDraw(m_RenderParts[m_SortedParts[first + done]]);
                    batch.Push.Instances = instanceBlock.Address + instanceCursor * sizeof(InstanceGPU);
                    batch.Push.Flags |= 2u;
                    submit(batch, n);
                    if (overlayEdges) wireOverlay.push_back({ batch, n });
                    instanceCursor += n;
                    done += n;
                }
            }
            else
                for (uint32_t k = 0; k < count; ++k)
                {
                    Draw d = makeDraw(m_RenderParts[m_SortedParts[first + k]]);
                    submit(d, 1);
                    if (overlayEdges) wireOverlay.push_back({ d, 1u });
                }
        }
        // Visual Effects drawn as meshes: every slot an instance (dead ones collapse), written by VfxSim.comp.
        if (!oitPass)
            for (const VfxMeshPart& v : m_VfxMeshParts)
            {
                if (shadowPass && !v.CastShadows) continue;
                if (!m_HiddenIds.empty() && m_HiddenIds.contains(v.EntityId)) continue;
                for (uint32_t done = 0; done < v.Capacity; done += kMaxInstancesPerDraw)
                {
                    Draw d;
                    d.MeshPtr = v.MeshRef.get();
                    d.FirstIndex = v.FirstIndex;
                    d.IndexCount = v.IndexCount;
                    d.Push.Model = glm::mat4(1.0f);
                    d.Push.Material = v.Material;
                    d.Push.EntityId = v.EntityId;
                    d.Push.Skin = 0;
                    d.Push.Joints = 0;
                    d.Push.Instances = v.Instances + (VkDeviceAddress)done * sizeof(InstanceGPU);
                    d.Push.Flags = 2u | (1u << 17); // (instanced, each with its particle's color)
                    d.Push.Motion = 0;
                    d.Pipe = shadowPass ? &res.Shadow
                           : depthPass  ? &MainPipeline(v.DoubleSided ? MainPipelines::DepthPrepassDoubleSided : MainPipelines::DepthPrepass)
                           : modePipe   ? modePipe
                                        : &MainPipeline(prepassed ? (v.DoubleSided ? MainPipelines::MeshDoubleSidedPrepassed : MainPipelines::MeshPrepassed)
                                                                  : v.DoubleSided ? MainPipelines::MeshDoubleSided : MainPipelines::Mesh);
                    submit(d, std::min(kMaxInstancesPerDraw, v.Capacity - done));
                }
            }
        // Shaded Wireframe: the opaque meshes' edges over them (not counted in the stats).
        if (!wireOverlay.empty())
        {
            const Pipeline& edges = MainPipeline(MainPipelines::MeshWireOverlay);
            edges.Bind(cmd);
            bound = &edges;
            for (auto& [d, instances] : wireOverlay)
            {
                vkCmdPushConstants(cmd, device.GetPipelineLayout(), VK_SHADER_STAGE_ALL, 0, sizeof(MeshPush), &d.Push);
                if (d.MeshPtr != boundMesh)
                {
                    d.MeshPtr->Bind(cmd);
                    boundMesh = d.MeshPtr;
                }
                if (d.Vertices)
                {
                    vkCmdBindVertexBuffers(cmd, 0, 1, &d.Vertices, &d.VertexOffset);
                    boundMesh = nullptr;
                }
                if (d.Indices) vkCmdBindIndexBuffer(cmd, d.Indices, d.IndexOffset, VK_INDEX_TYPE_UINT32);
                vkCmdDrawIndexed(cmd, d.IndexCount, instances, d.FirstIndex, 0, 0);
            }
        }

        // Transparent meshes after opaque ones, far to near. Runs of neighbours from the same batch become one
        // instanced draw: instances blend in instance order, so the back-to-front order is kept exactly.
        std::sort(transparent.begin(), transparent.end(), [](const Draw& a, const Draw& b) { return a.Depth > b.Depth; });
        for (size_t i = 0; i < transparent.size();)
        {
            uint32_t batch = m_RenderParts[transparent[i].Part].Batch;
            size_t run = 1;
            if (s_InstancingEnabled && m_BatchInstanceable[batch])
                while (i + run < transparent.size() && run < kMaxInstancesPerDraw && m_RenderParts[transparent[i + run].Part].Batch == batch) ++run;
            TransientAllocation block = run > 1 ? device.AllocateTransient(run * sizeof(InstanceGPU), 16) : TransientAllocation{};
            if (!block)
            {
                submit(transparent[i], 1);
                ++i;
                continue;
            }
            auto* items = (InstanceGPU*)block.Data;
            for (size_t k = 0; k < run; ++k) items[k] = InstanceOf(m_RenderItems[m_RenderParts[transparent[i + k].Part].Item]);
            Draw d = transparent[i];
            d.Push.Instances = block.Address;
            d.Push.Flags |= 2u;
            submit(d, (uint32_t)run);
            i += run;
        }
    }

    const Pipeline& SceneRenderer::MainPipeline(int kind) const
    {
        auto& r = Renderer::Get();
        if (m_Samples <= 1)
            switch (kind)
            {
            case MainPipelines::Mesh: return r.Mesh;
            case MainPipelines::MeshDoubleSided: return r.MeshDoubleSided;
            case MainPipelines::MeshTransparent: return r.MeshTransparent;
            case MainPipelines::DepthPrepass: return r.DepthPrepass;
            case MainPipelines::DepthPrepassDoubleSided: return r.DepthPrepassDoubleSided;
            case MainPipelines::Sky: return r.Sky;
            case MainPipelines::Sprite: return r.Sprite;
            case MainPipelines::SpriteAdditive: return r.SpriteAdditive;
            case MainPipelines::GpuParticles: return r.GpuParticles;
            case MainPipelines::GpuParticlesAdditive: return r.GpuParticlesAdditive;
            case MainPipelines::GpuTrails: return r.GpuTrails;
            case MainPipelines::GpuTrailsAdditive: return r.GpuTrailsAdditive;
            case MainPipelines::MeshWire: return r.MeshWire;
            case MainPipelines::MeshWireOverlay: return r.MeshWireOverlay;
            case MainPipelines::MeshOverdraw: return r.MeshOverdraw;
            case MainPipelines::MeshImpostor: return r.MeshImpostor;
            case MainPipelines::DepthPrepassImpostor: return r.DepthPrepassImpostor;
            case MainPipelines::MeshPrepassed: return r.MeshPrepassed;
            case MainPipelines::MeshDoubleSidedPrepassed: return r.MeshDoubleSidedPrepassed;
            default: return r.Line;
            }
        auto& set = r.Multisampled[m_Samples];
        if (!set)
        {
            set = std::make_unique<MainPipelines>();
            for (int k = 0; k < MainPipelines::Count; ++k)
            {
                GraphicsPipelineDesc desc = r.MainDescs[k];
                desc.Samples = (VkSampleCountFlagBits)m_Samples;
                set->Pipelines[k] = Pipeline(desc);
            }
        }
        return set->Pipelines[kind];
    }

    void SceneRenderer::MultisampleInto(VkRenderingAttachmentInfo& att, const Image& multisampled, const Image& resolved, VkResolveModeFlagBits mode)
    {
        att.imageView = multisampled.GetView();
        att.resolveMode = mode;
        att.resolveImageView = resolved.GetView();
        att.resolveImageLayout = att.imageLayout;
        if (att.storeOp == VK_ATTACHMENT_STORE_OP_STORE) att.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE; // the resolve keeps the result
    }

    int LightClusters::SliceOf(float d) const
    {
        float s = Orthographic ? (d - Near) / (Far - Near) : std::log(std::max(d, Near) / Near) / std::log(Far / Near);
        return std::clamp((int)(s * kSlices), 0, kSlices - 1);
    }

    void LightClusters::SetPlanes(const glm::mat4& proj)
    {
        // Near and far planes from the projection (right-handed, depth 0..1).
        Orthographic = proj[3][3] == 1.0f;
        float n = proj[3][2] / proj[2][2];
        float f = Orthographic ? n - 1.0f / proj[2][2] : proj[3][2] / (proj[2][2] + 1.0f);
        Near = std::max(n, Orthographic ? -1e6f : 0.01f);
        Far = std::clamp(f, Near + 0.01f, Near + 10000.0f);
        if (!std::isfinite(Near) || !std::isfinite(Far)) { Near = 0.1f; Far = 1000.0f; }
    }

    void LightClusters::Build(const std::vector<glm::vec4>& spheres, const glm::mat4& view, const glm::mat4& proj, bool cull)
    {
        SetPlanes(proj);

        // Each light's range of clusters: depth slices from its sphere, tiles from the screen bounds of its box.
        struct Range { int X0, X1, Y0, Y1, S0, S1; };
        std::vector<Range> ranges(spheres.size(), Range{ 0, -1, 0, -1, 0, -1 });
        for (size_t i = 0; i < spheres.size(); ++i)
        {
            glm::vec3 c = glm::vec3(view * glm::vec4(glm::vec3(spheres[i]), 1.0f));
            float r = std::max(spheres[i].w, 0.0f);
            float dMin = -c.z - r, dMax = -c.z + r;
            Range& rg = ranges[i];
            if (!cull)
            {
                rg = { 0, kTilesX - 1, 0, kTilesY - 1, 0, kSlices - 1 };
                continue;
            }
            if (dMax < Near || dMin > Far) continue;
            rg.S0 = SliceOf(std::max(dMin, Near));
            rg.S1 = SliceOf(std::min(dMax, Far));
            glm::vec2 lo(-1.0f), hi(1.0f);
            if (Orthographic || dMin > Near)
            {
                lo = glm::vec2(1e9f);
                hi = glm::vec2(-1e9f);
                for (int k = 0; k < 8; ++k)
                {
                    glm::vec3 corner = c + glm::vec3(k & 1 ? r : -r, k & 2 ? r : -r, k & 4 ? r : -r);
                    glm::vec4 clip = proj * glm::vec4(corner, 1.0f);
                    glm::vec2 ndc = glm::vec2(clip) / std::max(clip.w, 1e-6f);
                    lo = glm::min(lo, ndc);
                    hi = glm::max(hi, ndc);
                }
            }
            auto tile = [](float ndc, int count) { return (int)std::floor((ndc * 0.5f + 0.5f) * (float)count); };
            rg.X0 = std::max(tile(lo.x, kTilesX), 0);
            rg.X1 = std::min(tile(hi.x, kTilesX), kTilesX - 1);
            rg.Y0 = std::max(tile(lo.y, kTilesY), 0);
            rg.Y1 = std::min(tile(hi.y, kTilesY), kTilesY - 1);
        }
        // Count, then fill (lights keep their order inside each cluster).
        Cells.assign(kCount, glm::uvec2(0u));
        for (const Range& rg : ranges)
            for (int s = rg.S0; s <= rg.S1; ++s)
                for (int y = rg.Y0; y <= rg.Y1; ++y)
                    for (int x = rg.X0; x <= rg.X1; ++x)
                        if (Cells[CellIndex(x, y, s)].y < kMaxPerCluster) ++Cells[CellIndex(x, y, s)].y;
        uint32_t offset = 0;
        for (auto& cell : Cells)
        {
            cell.x = offset;
            offset += cell.y;
            cell.y = 0;
        }
        Indices.assign(offset, 0u);
        for (size_t i = 0; i < ranges.size(); ++i)
        {
            const Range& rg = ranges[i];
            for (int s = rg.S0; s <= rg.S1; ++s)
                for (int y = rg.Y0; y <= rg.Y1; ++y)
                    for (int x = rg.X0; x <= rg.X1; ++x)
                    {
                        glm::uvec2& cell = Cells[CellIndex(x, y, s)];
                        if (cell.y < kMaxPerCluster) Indices[cell.x + cell.y++] = (uint32_t)i;
                    }
        }
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
        // The quality level's resolution: a different one makes the atlas again (the old one goes once the GPU is done).
        uint32_t tile = 256;
        while (tile < 4096 && tile < (uint32_t)Quality::Active().ShadowResolution) tile *= 2;
        if (m_ShadowMap && tile != m_ShadowTileSize)
        {
            std::shared_ptr<Image> old(m_ShadowMap.release());
            uint32_t oldIndex = m_ShadowBindless;
            device.DeferRelease([old, oldIndex] { RenderDevice::Get().UnregisterTexture(oldIndex); });
        }
        m_ShadowTileSize = tile;
        if (!m_ShadowMap)
        {
            m_ShadowMap = MakeScope<Image>(ImageDesc{ ShadowMapSize(), ShadowMapSize(), VK_FORMAT_D32_SFLOAT,
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
            float unitsPerTexel = radius * 2.0f / m_ShadowTileSize;
            glm::vec3 ls = glm::vec3(lightRot * glm::vec4(center, 1.0f));
            ls.x = std::floor(ls.x / unitsPerTexel) * unitsPerTexel;
            ls.y = std::floor(ls.y / unitsPerTexel) * unitsPerTexel;
            center = glm::vec3(glm::inverse(lightRot) * glm::vec4(ls, 1.0f));

            glm::mat4 view = glm::lookAt(center - dir * (radius + backoff), center, up);
            glm::mat4 proj = glm::ortho(-radius, radius, -radius, radius, 0.0f, radius * 2.0f + backoff);
            out.ViewProj[c] = proj * view;
            out.Splits[c] = d1;
            out.Texel[c] = unitsPerTexel;
            out.DepthRange[c] = radius * 2.0f + backoff;
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
        info.renderArea = { { 0, 0 }, { ShadowMapSize(), ShadowMapSize() } };
        info.layerCount = 1;
        info.pDepthAttachment = &depth;
        vkCmdBeginRendering(cmd, &info);
        vkCmdSetLineWidth(cmd, 1.0f);
        device.BindBindless(cmd);
        std::vector<uint8_t> shadowUniforms((const uint8_t*)uniforms, (const uint8_t*)uniforms + uniformSize);
        for (int c = 0; c < out.Count; ++c)
        {
            float x = (float)((c & 1) * m_ShadowTileSize), y = (float)((c >> 1) * m_ShadowTileSize);
            VkViewport viewport{ x, y + m_ShadowTileSize, (float)m_ShadowTileSize, -(float)m_ShadowTileSize, 0.0f, 1.0f };
            VkRect2D scissor{ { (int32_t)x, (int32_t)y }, { m_ShadowTileSize, m_ShadowTileSize } };
            vkCmdSetViewport(cmd, 0, 1, &viewport);
            vkCmdSetScissor(cmd, 0, 1, &scissor);
            std::memcpy(shadowUniforms.data(), &out.ViewProj[c], sizeof(glm::mat4)); // ViewProj is the first member
            device.BindUniforms(cmd, shadowUniforms.data(), shadowUniforms.size());
            DrawMeshes(cmd, out.ViewProj[c], MeshPass::Shadow);
        }
        vkCmdEndRendering(cmd);
        Vk::TransitionImage(cmd, img, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);
    }

    void SceneRenderer::CollectSprites(Scene& scene, const CameraData& camera)
    {
        uint32_t white = AssetManager::GetWhiteTexture()->GetBindlessIndex();
        // World Space canvases: their UI images as panels on their objects (the images hold display values: bit 28).
        for (auto [e, canvas, t] : scene.GetRegistry().ViewOf<CanvasComponent, TransformComponent>())
        {
            if (!t.ActiveInHierarchy || canvas.RenderMode != CanvasRenderMode::WorldSpace || LayerCulled(scene, e)) continue;
            uint64_t key = (uint64_t)(uintptr_t)&scene * 1000003ull ^ (((uint64_t)e.Generation << 32) | e.Index);
            auto it = m_WorldCanvases.find(key);
            if (it == m_WorldCanvases.end() || !it->second.Initialized) continue;
            SpriteDraw d{};
            glm::vec2 size = canvas.WorldSize();
            d.Transform = t.World * glm::scale(glm::mat4(1.0f), glm::vec3(size, 1.0f));
            d.Color = glm::vec4(1.0f);
            d.UVMin = { 0.0f, 1.0f };
            d.UVMax = { 1.0f, 0.0f };
            d.Texture = it->second.Bindless | 0x10000000u;
            d.Entity = e.Index + 1;
            d.Order = canvas.SortingOrder;
            d.Depth = (camera.View * t.World[3]).z;
            m_Sprites.push_back(d);
        }
        for (auto [e, sr, t] : scene.GetRegistry().ViewOf<SpriteRendererComponent, TransformComponent>())
        {
            if (!t.ActiveInHierarchy) continue;
            if (LayerCulled(scene, e)) continue;
            Ref<Texture2D> tex = AssetManager::GetTexture(sr.Texture);
            SpriteDraw d;
            d.Transform = t.World;
            int cols = std::max(1, sr.SheetColumns), rows = std::max(1, sr.SheetRows);
            // A named sprite (sliced in the Sprite Editor): just its rectangle, sized to it.
            const SpriteRect* named = tex && !sr.SpriteName.empty() ? AssetManager::FindSprite(sr.Texture, sr.SpriteName) : nullptr;
            if (named && tex->GetWidth() > 0 && tex->GetHeight() > 0)
                d.Transform = d.Transform * glm::scale(glm::mat4(1.0f), glm::vec3((float)named->Rect.z / (float)named->Rect.w, 1.0f, 1.0f));
            else if (tex && tex->GetHeight() > 0) // keep the (cell) aspect ratio, 1 unit tall
            {
                float aspect = ((float)tex->GetWidth() / cols) / ((float)tex->GetHeight() / rows);
                d.Transform = d.Transform * glm::scale(glm::mat4(1.0f), glm::vec3(aspect, 1.0f, 1.0f));
            }
            d.Color = ToLinear(sr.Color);
            glm::vec2 cellMin(0.0f), cellMax(sr.Tiling);
            if (named && tex->GetWidth() > 0 && tex->GetHeight() > 0)
            {
                glm::vec2 size((float)tex->GetWidth(), (float)tex->GetHeight());
                cellMin = glm::vec2(named->Rect.x, named->Rect.y) / size;
                cellMax = glm::vec2(named->Rect.x + named->Rect.z, named->Rect.y + named->Rect.w) / size;
            }
            else if (cols > 1 || rows > 1)
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
            // Packed into a sprite atlas: drawn from its page, the UVs moved into the sprite's region (not when it
            // repeats: Tiling beyond 1 needs the texture of its own).
            glm::vec4 region;
            if ((sr.Tiling.x <= 1.0f && sr.Tiling.y <= 1.0f) || named)
                if (Ref<Texture2D> page = AssetManager::FindInAtlas(sr.Texture, region))
                {
                    glm::vec2 lo(region.x, region.y), hi(region.z, region.w);
                    d.UVMin = lo + d.UVMin * (hi - lo);
                    d.UVMax = lo + d.UVMax * (hi - lo);
                    d.Texture = page->GetBindlessIndex();
                }
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
            if (LayerCulled(scene, e)) continue;
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
            if (LayerCulled(scene, e)) continue;
            auto font = AssetManager::GetFont(text.Font);
            if (!font) continue;
            glm::vec2 block;
            float s = text.FontSize / font->GetLineHeight();
            // Layout at bake size: <size=N> (same units as Font Size) is N / s pixels.
            auto quads = font->Layout(text.Text, Font::kBakeSize, 0.0f, text.Align, 0.0f, block, text.RichText, 1.0f / s);
            glm::vec4 color = ToLinear(text.Color);
            uint32_t atlas = font->GetAtlasIndex(), white = AssetManager::GetWhiteTexture()->GetBindlessIndex();
            float depth = (camera.View * t.World[3]).z;
            for (auto& q : quads)
            {
                // Layout is y-down pixels; center the block on the entity and flip to y-up world units.
                glm::vec2 c = ((q.Min + q.Max) * 0.5f - block * 0.5f) * s;
                glm::vec2 size = (q.Max - q.Min) * s;
                glm::mat4 shear(1.0f);
                shear[1][0] = q.Skew; // italic: x leans with height (sheared after scaling, so the slope is exact)
                SpriteDraw d;
                // The layout slants around the quad's bottom edge and the sprite around its center: move half the lean.
                d.Transform = t.World * glm::translate(glm::mat4(1.0f), glm::vec3(c.x + q.Skew * size.y * 0.5f, -c.y, 0.0f)) * shear *
                              glm::scale(glm::mat4(1.0f), glm::vec3(size, 1.0f));
                d.Color = q.HasColor ? glm::vec4(glm::vec3(ToLinear(q.Color)), q.Color.a * color.a) : color;
                d.UVMin = { q.UVMin.x, q.UVMax.y };
                d.UVMax = { q.UVMax.x, q.UVMin.y };
                d.Texture = q.Solid ? white : q.Atlas ? q.Atlas : atlas;
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
        float dt = scene.GetDeltaTime(); // how far particles moved this frame (motion vectors)
        for (auto [e, ps, t] : scene.GetRegistry().ViewOf<ParticleSystemComponent, TransformComponent>())
        {
            if (!t.ActiveInHierarchy || ps.Particles.empty()) continue;
            if (LayerCulled(scene, e)) continue;
            auto tex = ps.Texture.IsValid() ? AssetManager::GetTexture(ps.Texture) : nullptr;
            uint32_t texture = (tex ? tex : AssetManager::GetParticleTexture())->GetBindlessIndex();
            if (ps.Lit) texture |= kLitSpriteBit; // Sprite.vert lights it
            bool local = ps.SimulationSpace == ParticleSpace::Local;
            float worldScale = local ? glm::length(glm::vec3(t.World[0])) : 1.0f;
            uint32_t trailTexture = AssetManager::GetParticleTexture()->GetBindlessIndex();
            // Points and directions to world space in plain floats (thousands of particles a frame, Debug builds too).
            const float* w = &t.World[0][0];
            auto toWorld = [w](const glm::vec3& v, float one) {
                return glm::vec3(w[0] * v.x + w[4] * v.y + w[8] * v.z + w[12] * one, w[1] * v.x + w[5] * v.y + w[9] * v.z + w[13] * one,
                                 w[2] * v.x + w[6] * v.y + w[10] * v.z + w[14] * one);
            };
            const float* view = &camera.View[0][0];
            for (const auto& p : ps.Particles)
            {
                float life = std::clamp(p.Age / p.Lifetime, 0.0f, 1.0f);
                glm::vec3 pos = local ? toWorld(p.Position, 1.0f) : p.Position;
                float size = p.Size * ps.SizeOverLifetime.Evaluate(life) * worldScale;
                glm::vec4 color = p.Color * ps.ColorOverLifetime.Evaluate(life);
                if (ps.Trails && p.TrailCount > 0)
                {
                    // Ribbon of camera-facing segments from the particle back through its history, tapering and fading.
                    glm::vec3 prev = pos;
                    int n = p.TrailCount;
                    glm::vec3 moved(0.0f); // the head segment's particle end moved with it (the history points stay put)
                    if (n > 0)
                    {
                        glm::vec3 vel = local ? toWorld(p.Velocity, 0.0f) : p.Velocity;
                        moved = glm::vec3(vel.x * dt, vel.y * dt, vel.z * dt);
                    }
                    for (int k = 0; k < n; ++k)
                    {
                        glm::vec3 q = local ? toWorld(p.Trail[k], 1.0f) : p.Trail[k];
                        float ax = q.x - prev.x, ay = q.y - prev.y, az = q.z - prev.z;
                        if (ax * ax + ay * ay + az * az > 1e-8f)
                        {
                            float mx = (q.x + prev.x) * 0.5f, my = (q.y + prev.y) * 0.5f, mz = (q.z + prev.z) * 0.5f;
                            float tx = camera.Position.x - mx, ty = camera.Position.y - my, tz = camera.Position.z - mz;
                            float sx = ay * tz - az * ty, sy = az * tx - ax * tz, sz = ax * ty - ay * tx; // cross(axis, to camera)
                            float fade = 1.0f - (float)k / n;
                            float width = size * ps.TrailWidth * fade;
                            float side2 = sx * sx + sy * sy + sz * sz;
                            if (side2 > 1e-10f && width > 0.0f)
                            {
                                float scale = width / std::sqrt(side2);
                                sx *= scale, sy *= scale, sz *= scale;
                                SpriteDraw& seg = m_Sprites.emplace_back();
                                float* m = &seg.Transform[0][0];
                                m[0] = ax, m[1] = ay, m[2] = az, m[3] = 0.0f;
                                m[4] = sx, m[5] = sy, m[6] = sz, m[7] = 0.0f;
                                m[8] = forward.x, m[9] = forward.y, m[10] = forward.z, m[11] = 0.0f;
                                m[12] = mx, m[13] = my, m[14] = mz, m[15] = 1.0f;
                                seg.Color = ToLinear(glm::vec4(color.r, color.g, color.b, color.a * fade));
                                seg.UVMin = { 0.5f, 1.0f }; // soft falloff across the width, constant along the length
                                seg.UVMax = { 0.5f, 0.0f };
                                seg.Texture = trailTexture;
                                seg.Entity = e.Index + 1;
                                seg.Order = ps.SortingOrder;
                                seg.Depth = view[2] * mx + view[6] * my + view[10] * mz + view[14];
                                seg.Additive = ps.Blend == ParticleBlend::Additive;
                                seg.MotionKind = 2;
                                if (k == 0)
                                {
                                    // The segment at the particle: its corners at -x (the particle's end) moved with it.
                                    const float cx[4] = { -0.5f, 0.5f, 0.5f, -0.5f }, cy[4] = { -0.5f, -0.5f, 0.5f, 0.5f };
                                    for (int i = 0; i < 4; ++i)
                                    {
                                        glm::vec3 corner(ax * cx[i] + sx * cy[i] + mx, ay * cx[i] + sy * cy[i] + my, az * cx[i] + sz * cy[i] + mz);
                                        seg.Before[i] = cx[i] < 0.0f ? glm::vec3(corner.x - moved.x, corner.y - moved.y, corner.z - moved.z) : corner;
                                    }
                                    seg.MotionKind = moved == glm::vec3(0.0f) ? 2 : 3;
                                }
                            }
                        }
                        prev = q;
                    }
                }
                float c = std::cos(p.Rotation), s = std::sin(p.Rotation);
                SpriteDraw& d = m_Sprites.emplace_back();
                float* m = &d.Transform[0][0];
                m[0] = (right.x * c + up.x * s) * size, m[1] = (right.y * c + up.y * s) * size, m[2] = (right.z * c + up.z * s) * size, m[3] = 0.0f;
                m[4] = (up.x * c - right.x * s) * size, m[5] = (up.y * c - right.y * s) * size, m[6] = (up.z * c - right.z * s) * size, m[7] = 0.0f;
                m[8] = forward.x, m[9] = forward.y, m[10] = forward.z, m[11] = 0.0f;
                m[12] = pos.x, m[13] = pos.y, m[14] = pos.z, m[15] = 1.0f;
                d.Color = ToLinear(color);
                ps.SheetUV(ps.SheetFrame(p), d.UVMin, d.UVMax);
                d.Texture = texture;
                d.Entity = e.Index + 1;
                d.Order = ps.SortingOrder;
                d.Depth = view[2] * pos.x + view[6] * pos.y + view[10] * pos.z + view[14];
                d.Additive = ps.Blend == ParticleBlend::Additive;
                d.MotionKind = 1;
                glm::vec3 velocity = local ? toWorld(p.Velocity, 0.0f) : p.Velocity;
                d.Displacement = glm::vec3(velocity.x * dt, velocity.y * dt, velocity.z * dt);
            }
        }
    }

    void SceneRenderer::CollectLines(Scene& scene, const CameraData& camera)
    {
        auto& reg = scene.GetRegistry();
        RibbonParams params;
        params.Orthographic = camera.Orthographic;
        params.CameraPosition = camera.Position;
        params.CameraForward = -glm::vec3(glm::inverse(camera.View)[2]);
        uint32_t white = AssetManager::GetWhiteTexture()->GetBindlessIndex();
        auto textureIndex = [&](AssetHandle handle) {
            auto tex = handle.IsValid() ? AssetManager::GetTexture(handle) : nullptr;
            return tex ? tex->GetBindlessIndex() : white;
        };
        std::vector<RibbonPoint> points;
        // Motion vectors: `object` (local-space lines) moves the corners with that world matrix; `before` (a trail
        // whose head moved) gives every corner's last position; neither: still.
        auto emit = [&](const std::vector<RibbonQuad>& quads, uint32_t texture, Entity e, int32_t order, bool additive,
                        const glm::mat4* object = nullptr, const std::vector<RibbonQuad>* before = nullptr) {
            for (size_t k = 0; k < quads.size(); ++k)
            {
                const RibbonQuad& q = quads[k];
                SpriteDraw d;
                d.HasCorners = true;
                d.MotionKind = object ? 0 : before ? 3 : 2;
                if (object) d.Transform = *object;
                for (int i = 0; i < 4; ++i)
                {
                    d.P[i] = q.P[i];
                    d.C[i] = q.C[i];
                    d.UV[i] = q.UV[i];
                    if (before) d.Before[i] = (*before)[k].P[i];
                }
                d.Texture = texture;
                d.Entity = e.Index + 1;
                d.Order = order;
                d.Depth = (camera.View * glm::vec4((q.P[0] + q.P[2]) * 0.5f, 1.0f)).z;
                d.Additive = additive;
                m_Sprites.push_back(d);
            }
        };

        for (auto [e, line, t] : reg.ViewOf<LineRendererComponent, TransformComponent>())
        {
            if (!t.ActiveInHierarchy || line.Positions.size() < 2) continue;
            if (LayerCulled(scene, e)) continue;
            size_t n = line.Positions.size();
            float scale = line.UseWorldSpace ? 1.0f : glm::length(glm::vec3(t.World[0]));
            points.clear();
            for (size_t i = 0; i < n; ++i)
            {
                float f = (float)i / (float)(n - 1);
                glm::vec3 p = line.UseWorldSpace ? line.Positions[i] : glm::vec3(t.World * glm::vec4(line.Positions[i], 1.0f));
                points.push_back({ p, line.Width * scale * line.WidthCurve.Evaluate(f), ToLinear(line.ColorGradient.Evaluate(f)) });
            }
            params.FaceCamera = line.Alignment == LineAlignment::View;
            params.Normal = glm::normalize(glm::vec3(t.World[2]));
            params.Loop = line.Loop;
            params.TextureMode = line.TextureMode;
            params.CornerVertices = line.CornerVertices;
            params.CapVertices = line.EndCapVertices;
            emit(BuildRibbon(points, params), textureIndex(line.Texture), e, line.SortingOrder, line.Blend == LineBlend::Additive,
                 line.UseWorldSpace ? nullptr : &t.World);
        }

        float now = scene.GetTime();
        for (auto [e, trail, t] : reg.ViewOf<TrailRendererComponent, TransformComponent>())
        {
            if (trail.Points.empty()) continue;
            if (LayerCulled(scene, e)) continue;
            float lifetime = std::max(trail.Time, 1e-3f);
            auto add = [&](const glm::vec3& p, float age) {
                float f = std::clamp(age / lifetime, 0.0f, 1.0f);
                points.push_back({ p, trail.Width * trail.WidthCurve.Evaluate(f), ToLinear(trail.ColorGradient.Evaluate(f)) });
            };
            // Head first, so normalized position 0 is the newest point.
            points.clear();
            bool head = trail.Emitting && t.ActiveInHierarchy;
            if (head) add(t.GetWorldPosition(), 0.0f);
            for (auto it = trail.Points.rbegin(); it != trail.Points.rend(); ++it) add(it->Position, now - it->Time);
            params.FaceCamera = true;
            params.Loop = false;
            params.TextureMode = trail.TextureMode;
            params.CornerVertices = trail.CornerVertices;
            params.CapVertices = trail.EndCapVertices;
            std::vector<RibbonQuad> quads = BuildRibbon(points, params), before;
            // The head moved since the last frame (the recorded points stay put): the ribbon as it was, with the head
            // where it was, gives each corner's motion.
            if (head && trail.HasHead && trail.PreviousHead != points[0].Position)
            {
                points[0].Position = trail.PreviousHead;
                before = BuildRibbon(points, params);
            }
            emit(quads, textureIndex(trail.Texture), e, trail.SortingOrder, trail.Blend == LineBlend::Additive, nullptr,
                 before.size() == quads.size() && !before.empty() ? &before : nullptr);
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
        // Scene visibility: hidden objects' sprites and icons go; unpickable ones draw without their id.
        if (!m_HiddenIds.empty()) std::erase_if(m_Sprites, [&](const SpriteDraw& d) { return m_HiddenIds.contains(d.Entity); });
        if (!m_UnpickableIds.empty())
            for (SpriteDraw& d : m_Sprites)
                if (m_UnpickableIds.contains(d.Entity)) d.Entity = 0;
        if (m_Sprites.empty()) return;
        auto& res = Renderer::Get();
        {
            std::unordered_map<uint32_t, uint32_t> occurrence;
            for (SpriteDraw& d : m_Sprites)
                if (d.MotionKind == 0) d.Occurrence = occurrence[d.Entity]++;
        }
        {
            // Sorted by order, then back to front, keeping the collected order for ties (a stable sort). The small keys
            // are sorted and the draws moved once: sorting the large draws themselves was slow (particles).
            struct Key { int32_t Order; float Depth; uint32_t Index; };
            std::vector<Key> keys(m_Sprites.size());
            for (uint32_t i = 0; i < (uint32_t)m_Sprites.size(); ++i) keys[i] = { m_Sprites[i].Order, std::isnan(m_Sprites[i].Depth) ? 0.0f : m_Sprites[i].Depth, i }; // (NaN breaks the ordering)
            std::sort(keys.begin(), keys.end(), [](const Key& a, const Key& b) {
                if (a.Order != b.Order) return a.Order < b.Order;
                if (a.Depth != b.Depth) return a.Depth < b.Depth;
                return a.Index < b.Index;
            });
            bool sorted = true;
            for (uint32_t i = 0; i < (uint32_t)keys.size() && sorted; ++i) sorted = keys[i].Index == i;
            if (!sorted)
            {
                std::vector<SpriteDraw> ordered;
                ordered.reserve(m_Sprites.size());
                for (const Key& k : keys) ordered.push_back(std::move(m_Sprites[k.Index]));
                m_Sprites.swap(ordered);
            }
        }

        size_t count = m_Sprites.size();
        TransientAllocation vb = RenderDevice::Get().AllocateTransient(count * 4 * sizeof(SpriteVertex));
        if (!vb) return;
        auto* v = (SpriteVertex*)vb.Data;
        const float cornerX[4] = { -0.5f, 0.5f, 0.5f, -0.5f }, cornerY[4] = { -0.5f, -0.5f, 0.5f, 0.5f };
        for (auto& s : m_Sprites)
        {
            if (s.HasCorners)
            {
                for (int i = 0; i < 4; ++i) *v++ = { s.P[i], s.C[i], s.UV[i], s.Texture, s.Entity };
                continue;
            }
            glm::vec2 uvs[4] = { { s.UVMin.x, s.UVMin.y }, { s.UVMax.x, s.UVMin.y }, { s.UVMax.x, s.UVMax.y }, { s.UVMin.x, s.UVMax.y } };
            // Transform * (x, y, 0, 1), in plain floats (thousands of particles a frame, Debug builds too).
            const float* m = &s.Transform[0][0];
            for (int i = 0; i < 4; ++i)
            {
                float x = cornerX[i], y = cornerY[i];
                SpriteVertex& out = *v++;
                out.Position.x = m[0] * x + m[4] * y + m[12];
                out.Position.y = m[1] * x + m[5] * y + m[13];
                out.Position.z = m[2] * x + m[6] * y + m[14];
                out.Color = s.Color;
                out.UV = uvs[i];
                out.Texture = s.Texture;
                out.Entity = s.Entity;
            }
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
            const Pipeline* pipeline = &MainPipeline(additive ? MainPipelines::SpriteAdditive : MainPipelines::Sprite);
            if (!DebugDraw("Opaque", "HDR Color", "Sprites", m_Sprites[runStart].Entity, 1, (uint32_t)(runEnd - runStart) * 2))
            {
                runStart = runEnd;
                continue;
            }
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

    void SceneRenderer::RenderUIOnly(VkCommandBuffer cmd, Scene& scene, SceneRenderTarget& target)
    {
        RenderUIInto(cmd, scene, target.GetOutput(), { target.GetWidth(), target.GetHeight() }, Entity::Null(), false);
    }

    void SceneRenderer::RenderWorldCanvases(VkCommandBuffer cmd, Scene& scene)
    {
        auto& device = RenderDevice::Get();
        uint64_t frame = device.Frame().FrameNumber;
        for (auto [e, canvas, t] : scene.GetRegistry().ViewOf<CanvasComponent, TransformComponent>())
        {
            if (!t.ActiveInHierarchy || canvas.RenderMode != CanvasRenderMode::WorldSpace) continue;
            glm::uvec2 size = glm::clamp(glm::uvec2(glm::round(canvas.PixelSize)), glm::uvec2(1), glm::uvec2(4096));
            uint64_t key = (uint64_t)(uintptr_t)&scene * 1000003ull ^ (((uint64_t)e.Generation << 32) | e.Index);
            auto& img = m_WorldCanvases[key];
            if (img.Size != size || !img.Bindless)
            {
                if (img.Bindless) device.UnregisterTexture(img.Bindless);
                img.Img = Image({ size.x, size.y, SceneRenderTarget::kOutputFormat, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, 1,
                                  "WorldCanvas" });
                img.Bindless = device.RegisterTexture(img.Img.GetView(), device.GetLinearSampler());
                img.Size = size;
                img.Initialized = false;
            }
            img.Frame = frame;
            RenderUIInto(cmd, scene, img.Img, { size.x, size.y }, e, !img.Initialized);
            img.Initialized = true;
        }
        for (auto it = m_WorldCanvases.begin(); it != m_WorldCanvases.end();)
        {
            if (it->second.Frame + 8 < frame)
            {
                if (it->second.Bindless) device.UnregisterTexture(it->second.Bindless);
                it = m_WorldCanvases.erase(it);
            }
            else
                ++it;
        }
    }

    void SceneRenderer::RenderUIInto(VkCommandBuffer cmd, Scene& scene, const Image& out, VkExtent2D extent, Entity canvas, bool firstUse)
    {
        auto& device = RenderDevice::Get();
        Vk::TransitionImage(cmd, out.GetHandle(), firstUse ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        VkRenderingAttachmentInfo att{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
        att.imageView = out.GetView();
        att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att.clearValue.color = { { 0.0f, 0.0f, 0.0f, 0.0f } };
        VkRenderingInfo info{ VK_STRUCTURE_TYPE_RENDERING_INFO };
        info.renderArea = { { 0, 0 }, extent };
        info.layerCount = 1;
        info.colorAttachmentCount = 1;
        info.pColorAttachments = &att;
        vkCmdBeginRendering(cmd, &info);
        VkViewport flat{ 0.0f, 0.0f, (float)extent.width, (float)extent.height, 0.0f, 1.0f };
        VkRect2D scissor{ { 0, 0 }, extent };
        vkCmdSetViewport(cmd, 0, 1, &flat);
        vkCmdSetScissor(cmd, 0, 1, &scissor);
        device.BindBindless(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS);
        {
            std::vector<UI::DrawCommand> commands;
            UI::BuildDrawList(scene, { (float)extent.width, (float)extent.height }, commands, canvas);
            DrawQuads(cmd, commands, extent);
        }
        vkCmdEndRendering(cmd);
        Vk::TransitionImage(cmd, out.GetHandle(), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }

    void SceneRenderer::DrawUI(VkCommandBuffer cmd, Scene& scene, VkExtent2D extent)
    {
        std::vector<UI::DrawCommand> commands;
        // Laid out at the view's size (as input sees it), drawn into the target at whatever render scale it has: the
        // UI keeps its place and size on screen, only softer at a lower scale.
        glm::vec2 screen((float)extent.width, (float)extent.height), view = scene.GetUIScreenSize();
        if (view.x >= 1.0f && view.y >= 1.0f && std::abs(view.x / view.y - screen.x / screen.y) < 0.02f) screen = view;
        UI::BuildDrawList(scene, screen, commands);
        DrawQuads(cmd, commands, extent, screen);
    }

    void SceneRenderer::DrawQuads(VkCommandBuffer cmd, const std::vector<UI::DrawCommand>& commands, VkExtent2D extent, glm::vec2 logical)
    {
        if (commands.empty()) return;
        glm::vec2 screen = logical.x >= 1.0f && logical.y >= 1.0f ? logical : glm::vec2((float)extent.width, (float)extent.height);

        TransientAllocation vb = RenderDevice::Get().AllocateTransient(commands.size() * 4 * sizeof(SpriteVertex));
        if (!vb) return;
        auto* v = (SpriteVertex*)vb.Data;
        // UI Masks (soft edges, image shapes): a table the fragment shader reads; each quad's vertices carry the index + 1
        // of its innermost mask, whose Parent (index + 1, 0 none) links to the one outside it.
        struct GpuMask
        {
            glm::vec4 Rect;
            glm::vec2 Softness;
            uint32_t Texture, Parent;
        };
        std::vector<GpuMask> masks;
        const UI::MaskInfo* lastMask = nullptr;
        uint32_t lastIndex = 0;
        for (auto& c : commands)
        {
            uint32_t m = 0;
            if (c.Mask.Active)
            {
                if (!lastMask || !(*lastMask == c.Mask))
                {
                    lastIndex = (uint32_t)masks.size() + 1;
                    for (int i = 0; i < c.Mask.Count; ++i)
                    {
                        const auto& l = c.Mask.Levels[i];
                        uint32_t parent = i + 1 < c.Mask.Count ? (uint32_t)masks.size() + 2 : 0;
                        masks.push_back({ l.Rect, l.Softness, l.Texture, parent });
                    }
                }
                lastMask = &c.Mask;
                m = lastIndex;
            }
            float slant = c.Slant * (c.Max.y - c.Min.y); // italic: the top edge leans right
            *v++ = { glm::vec3(c.Min.x + slant, c.Min.y, 0), c.Color, { c.UVMin.x, c.UVMin.y }, c.Texture, m };
            *v++ = { glm::vec3(c.Max.x + slant, c.Min.y, 0), c.Color, { c.UVMax.x, c.UVMin.y }, c.Texture, m };
            *v++ = { glm::vec3(c.Max.x, c.Max.y, 0), c.Color, { c.UVMax.x, c.UVMax.y }, c.Texture, m };
            *v++ = { glm::vec3(c.Min.x, c.Max.y, 0), c.Color, { c.UVMin.x, c.UVMax.y }, c.Texture, m };
        }
        struct UIPush
        {
            glm::vec2 Screen;
            glm::uvec2 Masks;
            glm::vec2 FragScale; // target pixels to UI pixels (masks test fragment positions)
        } push{ screen, glm::uvec2(0), screen / glm::vec2((float)extent.width, (float)extent.height) };
        if (!masks.empty())
            if (TransientAllocation mb = RenderDevice::Get().AllocateTransient(masks.size() * sizeof(GpuMask)))
            {
                std::memcpy(mb.Data, masks.data(), masks.size() * sizeof(GpuMask));
                push.Masks = { (uint32_t)mb.Address, (uint32_t)(mb.Address >> 32) };
            }
        auto& res = Renderer::Get();
        res.UI.Bind(cmd);
        vkCmdPushConstants(cmd, RenderDevice::Get().GetPipelineLayout(), VK_SHADER_STAGE_ALL, 0, sizeof(push), &push);
        VkBuffer buffer = vb.Buffer;
        vkCmdBindVertexBuffers(cmd, 0, 1, &buffer, &vb.Offset);
        vkCmdBindIndexBuffer(cmd, res.QuadIndices.GetHandle(), 0, VK_INDEX_TYPE_UINT32);
        size_t count = commands.size();
        for (size_t first = 0; first < count; first += kMaxQuadsPerDraw)
        {
            uint32_t n = (uint32_t)std::min<size_t>(kMaxQuadsPerDraw, count - first);
            if (!DebugDraw("UI", "Screen", "UI", 0, 1, n * 2)) continue;
            vkCmdDrawIndexed(cmd, n * 6, 1, 0, (int32_t)(first * 4), 0);
            m_Stats.DrawCalls++;
        }
    }

    namespace
    {
        // std430 mirror of GpuParticle and GpuParticleParamsRef in ParticleCommon.glsl.
        constexpr int kParticleLut = 32;
        struct GpuParticleParams
        {
            glm::mat4 Emitter, SimToWorld, WorldToSim;
            glm::vec4 Lifetime, SizeRot, Spin, ColorA, ColorB, Shape, Box, Gravity, Force, Noise, Collision, Misc;
            glm::uvec4 Counts, Sheet;
            glm::uvec4 SegmentCounts[ParticleSystemComponent::kMaxGpuEmissions / 4];
            glm::vec4 SegmentOrigins[ParticleSystemComponent::kMaxGpuEmissions];
            glm::vec4 ColorLut[kParticleLut], CurveLut[kParticleLut];
            glm::mat4 DepthViewProj, DepthInvViewProj;
            glm::uvec4 DepthInfo;
            glm::vec4 DepthParams;
            glm::vec4 Trail;
            glm::uvec4 TrailBuffer;
            glm::uvec4 ColliderCount;
            glm::vec4 Colliders[32 * 4]; // PARTICLE_COLLIDERS
            glm::uvec4 MeshSamples;
            glm::uvec4 DecalBuffer;
            glm::mat4 TerrainToLocal, TerrainToWorld;
            glm::uvec4 TerrainInfo;
            glm::vec4 TerrainSize;
            glm::vec4 Lighting; // x 1 = lit
        };
        // Collision decal hits read back per GPU system: a ring of buffers, one per frame until the GPU is done with it.
        constexpr uint32_t kHitRing = kFramesInFlight + 1, kMaxHitsPerFrame = 128;
        constexpr VkDeviceSize kGpuParticleSize = 64;
        constexpr uint32_t kMaxGpuParticles = 1000000;
        constexpr uint32_t kGpuTrailPoints = 8;              // TRAIL_POINTS in ParticleCommon.glsl
        constexpr uint32_t kMaxGpuTrailParticles = 262144;   // trails' history: 128 bytes per particle (32 MB here)
    }

    void SceneRenderer::SimulateGpuParticles(VkCommandBuffer cmd, Scene& scene, SceneRenderTarget& target, const glm::mat4& view)
    {
        m_GpuParticleDraws.clear();
        auto& device = RenderDevice::Get();
        auto& res = Renderer::Get();
        uint64_t frame = device.Frame().FrameNumber;
        bool first = true;
        // The depth this view kept from its last frame (for world collision), if it is still the same size.
        bool depthValid = m_ParticleDepthKept && m_ParticleDepthSize == glm::uvec2(target.GetWidth(), target.GetHeight());
        uint32_t depthBindless = target.GetDepthBindless();
        m_ParticleDepthWanted = false;
        // The scene's (first active) terrain: its heights on the GPU, for World collision to follow its shape anywhere.
        const TerrainComponent* collideTerrain = nullptr;
        glm::mat4 collideTerrainWorld(1.0f);
        for (auto [te, tc, tt] : scene.GetRegistry().ViewOf<TerrainComponent, TransformComponent>())
            if (tt.ActiveInHierarchy)
            {
                collideTerrain = &tc;
                collideTerrainWorld = tt.World;
                break;
            }
        if (collideTerrain)
        {
            auto& pt = res.ParticleTerrain;
            uint64_t key = TerrainKey(*collideTerrain) ^ (collideTerrain->EditHeights.empty() ? 0ull : (uint64_t)(uintptr_t)collideTerrain->EditHeights.data());
            if (key != pt.Key || !pt.Heights.IsValid())
            {
                std::vector<float> heights = Terrain::GetHeights(*collideTerrain);
                uint32_t n = (uint32_t)Terrain::SampleCount(*collideTerrain);
                if (heights.size() == (size_t)n * n)
                {
                    pt.Heights = Buffer(heights.size() * sizeof(float), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                        MemoryUsage::CpuToGpu, "ParticleTerrainHeights");
                    std::memcpy(pt.Heights.GetMapped(), heights.data(), heights.size() * sizeof(float));
                    VkBufferDeviceAddressInfo info{ VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO };
                    info.buffer = pt.Heights.GetHandle();
                    pt.Address = vkGetBufferDeviceAddress(device.GetDevice(), &info);
                    pt.Samples = n;
                    pt.Key = key;
                }
                else
                    collideTerrain = nullptr;
            }
        }
        auto beginCompute = [&] {
            if (!first) return;
            // Earlier draws (this or an in-flight frame) must be done reading the particle and sort buffers.
            Vk::MemoryBarrier(cmd, VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                              VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT,
                              VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_CLEAR_BIT,
                              VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT);
            device.BindBindless(cmd, VK_PIPELINE_BIND_POINT_COMPUTE);
            first = false;
        };
        // Back to front for this view: keys from view depth, then a bitonic sort over a power-of-two list.
        auto sortParticles = [&](auto& st, uint32_t capacity, VkDeviceAddress params) {
            beginCompute();
            uint32_t padded = 64;
            while (padded < capacity) padded <<= 1;
            if (!st.Sort.IsValid() || st.SortPadded != padded)
            {
                st.Sort = Buffer((VkDeviceSize)padded * 8, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                 MemoryUsage::GpuOnly, "GpuParticleSort");
                VkBufferDeviceAddressInfo info{ VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO };
                info.buffer = st.Sort.GetHandle();
                st.SortAddress = vkGetBufferDeviceAddress(device.GetDevice(), &info);
                st.SortPadded = padded;
            }
            auto computeToCompute = [&] {
                Vk::MemoryBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_SHADER_READ_BIT,
                                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);
            };
            computeToCompute(); // the simulation wrote the particles
            res.ParticleSortKeys.Bind(cmd);
            struct { glm::mat4 View; glm::uvec2 Particles, Params, Sort; uint32_t Count, Padded; } keys{
                view, SplitAddress(st.Address), SplitAddress(params), SplitAddress(st.SortAddress), capacity, padded };
            vkCmdPushConstants(cmd, device.GetPipelineLayout(), VK_SHADER_STAGE_ALL, 0, sizeof(keys), &keys);
            vkCmdDispatch(cmd, padded / 64, 1, 1);
            res.ParticleSort.Bind(cmd);
            for (uint32_t k = 2; k <= padded; k <<= 1)
                for (uint32_t j = k >> 1; j > 0; j >>= 1)
                {
                    computeToCompute();
                    struct { glm::uvec2 Sort; uint32_t J, K, Padded; } step{ SplitAddress(st.SortAddress), j, k, padded };
                    vkCmdPushConstants(cmd, device.GetPipelineLayout(), VK_SHADER_STAGE_ALL, 0, sizeof(step), &step);
                    vkCmdDispatch(cmd, padded / 64, 1, 1);
                }
        };
        // The 32 colliders nearest a point, in world space (GpuParticleParams' Colliders).
        auto fillColliders = [&](GpuParticleParams& p, const glm::vec3& emitter) {
            struct Shape { float Distance; glm::vec4 Rows[4]; };
            std::vector<Shape> shapes;
            auto axes = [](const glm::mat4& m, glm::vec3 axis[3], glm::vec3& scale) {
                for (int k = 0; k < 3; ++k)
                {
                    scale[k] = glm::length(glm::vec3(m[k]));
                    axis[k] = scale[k] > 1e-8f ? glm::vec3(m[k]) / scale[k] : glm::vec3(0.0f);
                }
            };
            auto& reg = scene.GetRegistry();
            auto add = [&](const glm::vec3& center, const glm::vec4 r1, const glm::vec4 r2, const glm::vec4 r3, float type) {
                shapes.push_back({ glm::distance(center, emitter), { glm::vec4(center, type), r1, r2, r3 } });
            };
            for (auto [ce, sc, ct] : reg.ViewOf<SphereColliderComponent, TransformComponent>())
            {
                if (!ct.ActiveInHierarchy || sc.IsTrigger) continue;
                glm::vec3 axis[3], scale;
                axes(ct.World, axis, scale);
                add(glm::vec3(ct.World * glm::vec4(sc.Offset, 1.0f)), glm::vec4(0, 0, 0, sc.Radius * std::max({ scale.x, scale.y, scale.z })), {}, {}, 0.0f);
            }
            for (auto [ce, bc, ct] : reg.ViewOf<BoxColliderComponent, TransformComponent>())
            {
                if (!ct.ActiveInHierarchy || bc.IsTrigger) continue;
                glm::vec3 axis[3], scale;
                axes(ct.World, axis, scale);
                glm::vec3 half = glm::abs(bc.Size) * 0.5f * scale;
                add(glm::vec3(ct.World * glm::vec4(bc.Offset, 1.0f)), glm::vec4(axis[0], half.x), glm::vec4(axis[1], half.y), glm::vec4(axis[2], half.z), 1.0f);
            }
            auto capsule = [&](const TransformComponent& ct, const glm::vec3& offset, float radius, float height) {
                glm::vec3 axis[3], scale;
                axes(ct.World, axis, scale);
                float r = radius * std::max(scale.x, scale.z);
                add(glm::vec3(ct.World * glm::vec4(offset, 1.0f)), glm::vec4(0, 0, 0, r), glm::vec4(axis[1], std::max(height * 0.5f * scale.y - r, 0.0f)), {}, 2.0f);
            };
            for (auto [ce, cc, ct] : reg.ViewOf<CapsuleColliderComponent, TransformComponent>())
                if (ct.ActiveInHierarchy && !cc.IsTrigger) capsule(ct, cc.Offset, cc.Radius, cc.Height);
            for (auto [ce, ch, ct] : reg.ViewOf<CharacterControllerComponent, TransformComponent>())
                if (ct.ActiveInHierarchy) capsule(ct, ch.Center, ch.Radius, ch.Height);
            if (shapes.size() > 32)
                std::partial_sort(shapes.begin(), shapes.begin() + 32, shapes.end(), [](const Shape& a, const Shape& b) { return a.Distance < b.Distance; });
            uint32_t count = (uint32_t)std::min<size_t>(shapes.size(), 32);
            for (uint32_t k = 0; k < count; ++k)
                for (int row = 0; row < 4; ++row) p.Colliders[k * 4 + row] = shapes[k].Rows[row];
            p.ColliderCount = glm::uvec4(count, 0u, 0u, 0u);
        };
        // One GPU system: its buffers, this frame's parameters, simulation (and sorting) and its draw. `salt` tells apart
        // several systems on one object (a Weather's rain and snow).
        auto simulateOne = [&](Entity e, ParticleSystemComponent& ps, const glm::mat4& emitterWorld, uint64_t salt) {
            uint32_t capacity = (uint32_t)std::clamp(ps.MaxParticles, 1, (int32_t)kMaxGpuParticles);
            uint64_t key = (uint64_t)(uintptr_t)&scene * 1000003ull ^ ((uint64_t)e.Index << 1) ^ salt;
            auto& st = res.GpuParticleStates[key];
            st.LastFrame = frame;
            bool clear = false;
            if (!st.Particles.IsValid() || st.Capacity != capacity)
            {
                st.Particles = Buffer(capacity * kGpuParticleSize,
                                      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                      MemoryUsage::GpuOnly, "GpuParticles");
                VkBufferDeviceAddressInfo info{ VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO };
                info.buffer = st.Particles.GetHandle();
                st.Address = vkGetBufferDeviceAddress(device.GetDevice(), &info);
                st.Capacity = capacity;
                clear = true;
            }
            // Trails keep a history per slot (made with the system's first frame with trails, so it starts cleared).
            bool trails = ps.Trails && capacity <= kMaxGpuTrailParticles;
            if (trails && (!st.Trails.IsValid() || st.Trails.GetSize() != (VkDeviceSize)capacity * kGpuTrailPoints * 16))
            {
                st.Trails = Buffer((VkDeviceSize)capacity * kGpuTrailPoints * 16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                   MemoryUsage::GpuOnly, "GpuParticleTrails");
                VkBufferDeviceAddressInfo info{ VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO };
                info.buffer = st.Trails.GetHandle();
                st.TrailAddress = vkGetBufferDeviceAddress(device.GetDevice(), &info);
                clear = true; // existing particles have no history
            }
            if (st.Generation != ps.GpuGeneration) clear = true;
            if (clear)
            {
                st.Generation = ps.GpuGeneration;
                st.Head = 0;
            }

            // Parameters for this frame (simulation and drawing).
            bool worldSpace = ps.SimulationSpace == ParticleSpace::World;
            glm::mat4 world = emitterWorld, invWorld = glm::inverse(world);
            GpuParticleParams p{};
            p.Emitter = world;
            p.SimToWorld = worldSpace ? glm::mat4(1.0f) : world;
            p.WorldToSim = worldSpace ? glm::mat4(1.0f) : invWorld;
            auto toSim = [&](const glm::vec3& v, ParticleSpace space) {
                if (space == ps.SimulationSpace) return v;
                return space == ParticleSpace::Local ? glm::vec3(world * glm::vec4(v, 0.0f)) : glm::vec3(invWorld * glm::vec4(v, 0.0f));
            };
            float dt = std::min(ps.GpuPendingDt, 0.25f);
            ps.GpuPendingDt = 0.0f;
            p.Lifetime = glm::vec4(ps.StartLifetime, ps.StartSpeed);
            p.SizeRot = glm::vec4(ps.StartSize, glm::radians(ps.StartRotation));
            p.Spin = glm::vec4(glm::radians(ps.AngularVelocity), ps.Drag, dt);
            p.ColorA = ps.StartColorA;
            p.ColorB = ps.StartColorB;
            p.Shape = glm::vec4((float)ps.Shape, glm::radians(ps.ShapeAngle), ps.ShapeRadius, worldSpace ? 1.0f : 0.0f);
            p.Box = glm::vec4(ps.BoxSize, ps.NoiseTime * ps.NoiseScrollSpeed);
            p.Gravity = glm::vec4(toSim(glm::vec3(0.0f, -9.81f * ps.GravityModifier, 0.0f), ParticleSpace::World), ps.ForceOverLifetime ? 1.0f : 0.0f);
            p.Force = glm::vec4(toSim(ps.Force, ps.ForceSpace), 0.0f);
            p.Noise = glm::vec4(ps.NoiseStrength, ps.NoiseFrequency, ps.Noise ? 1.0f : 0.0f, 0.0f);
            p.Lighting = glm::vec4(ps.Lit ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f);
            // World collision runs against the depth buffer this view rendered last frame.
            bool depthCollision = ps.Collision == ParticleCollision::World;
            if (depthCollision)
            {
                m_ParticleDepthWanted = true;
                p.DepthViewProj = m_ParticleDepthViewProj;
                p.DepthInvViewProj = glm::inverse(m_ParticleDepthViewProj);
                p.DepthInfo = glm::uvec4(depthBindless, depthValid ? 1u : 0u, 0u, 0u);
                p.DepthParams = glm::vec4(0.6f + ps.CollisionRadius, ps.CollisionRadius, 0.0f, 0.0f);
                if (collideTerrain)
                {
                    p.TerrainToWorld = collideTerrainWorld;
                    p.TerrainToLocal = glm::inverse(collideTerrainWorld);
                    p.TerrainInfo = glm::uvec4(SplitAddress(res.ParticleTerrain.Address), res.ParticleTerrain.Samples, 1u);
                    p.TerrainSize = glm::vec4(collideTerrain->Size, 0.0f, 0.0f);
                }
            }
            p.Collision = glm::vec4(ps.Collision == ParticleCollision::Plane ? 1.0f : depthCollision ? 2.0f : 0.0f, ps.CollisionPlaneHeight + ps.CollisionRadius, ps.Bounce,
                                    ps.CollisionDampen);
            p.Misc = glm::vec4(ps.LifetimeLoss, worldSpace ? 1.0f : glm::length(glm::vec3(world[0])), ps.SheetRandomStart ? 1.0f : 0.0f,
                               std::max(ps.SheetCycles, 0.0f));
            if (ps.Shape == ParticleShape::Mesh)
                if (const auto* samples = ParticleSystems::MeshSurfaceSamples(ps.EmitMesh))
                {
                    auto& buffer = res.MeshSampleBuffers[(uint64_t)ps.EmitMesh];
                    if (buffer.Source != samples || !buffer.Data.IsValid())
                    {
                        buffer.Data = Buffer(samples->size() * sizeof(glm::vec4), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                             MemoryUsage::CpuToGpu, "ParticleMeshSamples");
                        std::memcpy(buffer.Data.GetMapped(), samples->data(), samples->size() * sizeof(glm::vec4));
                        VkBufferDeviceAddressInfo info{ VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO };
                        info.buffer = buffer.Data.GetHandle();
                        buffer.Address = vkGetBufferDeviceAddress(device.GetDevice(), &info);
                        buffer.Source = samples;
                    }
                    p.MeshSamples = glm::uvec4(SplitAddress(buffer.Address), (uint32_t)(samples->size() / 2), 0u);
                }
            if (ps.CollideWithColliders)
            {
                fillColliders(p, glm::vec3(world[3])); // (the 32 nearest the emitter)
                p.Collision.z = ps.Bounce;
                p.Collision.w = ps.CollisionDampen;
                p.DepthParams.y = ps.CollisionRadius;
            }
            if (trails)
            {
                p.Trail = glm::vec4(1.0f, std::max(ps.TrailLifetime, 1e-3f) / kGpuTrailPoints, ps.TrailWidth, 0.0f);
                glm::uvec2 a = SplitAddress(st.TrailAddress);
                p.TrailBuffer = glm::uvec4(a, 0u, 0u);
            }
            uint32_t total = 0;
            for (size_t i = 0; i < ps.GpuEmissions.size() && i < ParticleSystemComponent::kMaxGpuEmissions; ++i)
            {
                uint32_t n = std::min<uint32_t>((uint32_t)std::max(ps.GpuEmissions[i].Count, 0), capacity - total);
                p.SegmentCounts[i / 4][i % 4] = n;
                p.SegmentOrigins[i] = glm::vec4(ps.GpuEmissions[i].Origin, ps.GpuEmissions[i].AtOrigin ? 1.0f : 0.0f);
                total += n;
            }
            ps.GpuEmissions.clear();
            p.Counts = glm::uvec4(capacity, st.Head, total, ++ps.GpuSeed * 2654435761u ^ (e.Index + 1) * 40503u);
            p.Sheet = glm::uvec4((uint32_t)std::max(ps.SheetColumns, 1), (uint32_t)std::max(ps.SheetRows, 1), (uint32_t)std::max(ps.SheetFrames, 0),
                                 ps.TextureSheet ? 1u : 0u);
            for (int i = 0; i < kParticleLut; ++i)
            {
                float x = (float)i / (kParticleLut - 1);
                p.ColorLut[i] = ps.ColorOverLifetime.Evaluate(x);
                p.CurveLut[i] = glm::vec4(ps.SizeOverLifetime.Evaluate(x), ps.SpeedOverLifetime.Evaluate(x), ps.ForceCurve.Evaluate(x), 0.0f);
            }
            // Collision decals: last time around this ring slot's hits (that frame is finished) become decals, and
            // the slot records this frame's.
            bool dispatching = clear || dt > 0.0f || total > 0;
            bool decals = ps.CollisionDecals && ps.MaxDecals > 0 && (ps.Collision != ParticleCollision::None || ps.CollideWithColliders);
            uint32_t hitSlot = (uint32_t)(frame % kHitRing);
            bool clearHits = false;
            if (decals && st.HitsHandled != frame)
            {
                st.HitsHandled = frame;
                Buffer& hits = st.Hits[hitSlot];
                if (hits.IsValid() && st.HitsFrame[hitSlot] != 0 && st.HitsFrame[hitSlot] <= device.GetCompletedFrameNumber())
                {
                    const auto* words = static_cast<const uint32_t*>(hits.GetMapped());
                    uint32_t count = std::min(words[0], kMaxHitsPerFrame);
                    const auto* items = reinterpret_cast<const glm::vec4*>(words + 4);
                    for (uint32_t k = 0; k < count; ++k)
                    {
                        uint32_t h = (uint32_t)(frame * 2654435761u) ^ (k * 40503u);
                        h = (h ^ (h >> 15)) * 0x2c1b3c6du;
                        float r0 = (h >> 8 & 1023) / 1023.0f, r1 = (h >> 18 & 1023) / 1023.0f;
                        if ((int32_t)ps.Decals.size() >= ps.MaxDecals) ps.Decals.erase(ps.Decals.begin());
                        ps.Decals.push_back({ glm::vec3(items[k * 3]), glm::vec3(items[k * 3 + 1]), items[k * 3 + 2],
                                              std::max(ps.DecalSize, 0.001f) * glm::mix(0.8f, 1.2f, r0), 0.0f, r1 * glm::two_pi<float>() });
                    }
                }
                st.HitsFrame[hitSlot] = 0;
                if (dispatching)
                {
                    if (!hits.IsValid())
                    {
                        hits = Buffer(16 + (VkDeviceSize)kMaxHitsPerFrame * 48,
                                      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                      MemoryUsage::GpuToCpu, "GpuParticleHits");
                        VkBufferDeviceAddressInfo info{ VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO };
                        info.buffer = hits.GetHandle();
                        st.HitsAddress[hitSlot] = vkGetBufferDeviceAddress(device.GetDevice(), &info);
                    }
                    st.HitsFrame[hitSlot] = frame;
                    clearHits = true;
                    glm::uvec2 a = SplitAddress(st.HitsAddress[hitSlot]);
                    p.DecalBuffer = glm::uvec4(a, kMaxHitsPerFrame, 1u);
                }
            }
            TransientAllocation params = device.AllocateTransient(sizeof(p), 16);
            if (!params) return;
            std::memcpy(params.Data, &p, sizeof(p));

            bool sort = ps.GpuSorting && ps.Blend == ParticleBlend::Alpha;
            if (clear || dt > 0.0f || total > 0)
            {
                beginCompute();
                res.ParticleSim.Bind(cmd);
                if (clear)
                {
                    // Zero = every slot dead (age 0 is not below lifetime 0).
                    vkCmdFillBuffer(cmd, st.Particles.GetHandle(), 0, VK_WHOLE_SIZE, 0);
                    Vk::MemoryBarrier(cmd, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                      VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);
                }
                if (clearHits)
                {
                    vkCmdFillBuffer(cmd, st.Hits[hitSlot].GetHandle(), 0, 16, 0);
                    Vk::MemoryBarrier(cmd, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                      VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);
                }
                struct { glm::uvec2 Particles, Params; } push{ SplitAddress(st.Address), SplitAddress(params.Address) };
                vkCmdPushConstants(cmd, device.GetPipelineLayout(), VK_SHADER_STAGE_ALL, 0, sizeof(push), &push);
                vkCmdDispatch(cmd, (capacity + 63) / 64, 1, 1);
                st.Head = (st.Head + total) % capacity;
            }
            if (sort) sortParticles(st, capacity, params.Address);

            auto tex = ps.Texture.IsValid() ? AssetManager::GetTexture(ps.Texture) : nullptr;
            m_GpuParticleDraws.push_back({ st.Address, params.Address, capacity, (tex ? tex : AssetManager::GetParticleTexture())->GetBindlessIndex(),
                                           e.Index + 1, ps.Blend == ParticleBlend::Additive, ps.SortingOrder, sort ? st.SortAddress : 0, trails });
        };
        for (auto [e, ps, t] : scene.GetRegistry().ViewOf<ParticleSystemComponent, TransformComponent>())
            if (ps.SimulateOnGpu && t.ActiveInHierarchy) simulateOne(e, ps, t.World, 0);
        for (auto [e, w, t] : scene.GetRegistry().ViewOf<WeatherComponent, TransformComponent>())
        {
            if (!t.ActiveInHierarchy) continue;
            if (w.RainParticles.IsAlive()) simulateOne(e, w.RainParticles, w.RainWorld, 1ull << 40);
            if (w.SnowParticles.IsAlive()) simulateOne(e, w.SnowParticles, w.SnowWorld, 2ull << 40);
        }
        // Visual Effects: their graph's programs run by VfxSim.comp, drawn as GPU particles. A graph's Trigger On Die
        // blocks start child effects: the parent writes events as its particles die, then each child spawns from its
        // events on the GPU (the count never comes back to the CPU), and a last pass moves the child's ring on.
        struct VfxParams
        {
            glm::mat4 EmitterToSim;
            glm::vec4 Time;
            glm::uvec4 Counts, Program, Code, Tables;
            glm::mat4 SimToWorld;
            glm::uvec4 Events[2];
            glm::uvec4 Source, Head;
            glm::vec4 Trail;
            glm::uvec4 TrailBuffer;
            glm::uvec4 MeshOut;
            glm::uvec4 Collide;
        };
        m_VfxMeshOutputs.clear();
        auto makeBuffer = [&](Buffer& buffer, VkDeviceAddress& address, VkDeviceSize size, MemoryUsage usage, const char* name) {
            buffer = Buffer(size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, usage, name);
            VkBufferDeviceAddressInfo info{ VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO };
            info.buffer = buffer.GetHandle();
            address = vkGetBufferDeviceAddress(device.GetDevice(), &info);
        };
        constexpr VkDeviceSize kEventBytes = 16 + (VkDeviceSize)VfxProgram::kMaxEvents * 48;
        auto toCompute = [&] {
            Vk::MemoryBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT,
                              VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);
        };
        for (auto [e, vfx, t] : scene.GetRegistry().ViewOf<VisualEffectComponent, TransformComponent>())
        {
            if (!t.ActiveInHierarchy) continue;
            auto graph = AssetManager::GetVisualEffect(vfx.Asset);
            auto program = graph ? VisualEffects::ProgramOf(vfx.Asset) : nullptr;
            if (!graph || !program) continue;
            float dt = std::min(vfx.PendingDt, 0.25f);
            vfx.PendingDt = 0.0f;
            uint64_t parentKey = (uint64_t)(uintptr_t)&scene * 1000003ull ^ ((uint64_t)e.Index << 1) ^ (3ull << 40);
            uint32_t seed = ++vfx.Seed * 2654435761u ^ (e.Index + 1) * 40503u;
            // One effect's simulation, sort and draw. `child` >= 0: a child spawning from `events` (slot `child`).
            auto simulate = [&](const VisualEffectAsset& g, const VfxProgram& prog, uint64_t key, int child, VkDeviceAddress childEvents,
                                VkDeviceAddress eventSlots[VfxProgram::kMaxChildren]) {
                uint32_t capacity = (uint32_t)std::clamp(g.Capacity, 1, (int32_t)kMaxGpuParticles);
                auto& st = res.GpuParticleStates[key];
                st.LastFrame = frame;
                bool clear = false;
                if (!st.Particles.IsValid() || st.Capacity != capacity)
                {
                    makeBuffer(st.Particles, st.Address, capacity * kGpuParticleSize, MemoryUsage::GpuOnly, "VfxParticles");
                    st.Capacity = capacity;
                    clear = true;
                }
                if (child >= 0 && !st.VfxHead.IsValid()) // (a child's ring head lives on the GPU)
                {
                    makeBuffer(st.VfxHead, st.VfxHeadAddress, 16, MemoryUsage::GpuOnly, "VfxChildHead");
                    clear = true;
                }
                bool trails = g.Trails && capacity <= kMaxGpuTrailParticles;
                Ref<Mesh> outMesh = g.MeshOutput && g.Mesh.IsValid() ? AssetManager::GetMesh(g.Mesh) : nullptr;
                if (outMesh && (!st.VfxInstances.IsValid() || st.VfxInstances.GetSize() != (VkDeviceSize)capacity * 80))
                {
                    makeBuffer(st.VfxInstances, st.VfxInstancesAddress, (VkDeviceSize)capacity * 80, MemoryUsage::GpuOnly, "VfxMeshInstances");
                    clear = true;
                }
                if (trails && (!st.Trails.IsValid() || st.Trails.GetSize() != (VkDeviceSize)capacity * kGpuTrailPoints * 16))
                {
                    makeBuffer(st.Trails, st.TrailAddress, (VkDeviceSize)capacity * kGpuTrailPoints * 16, MemoryUsage::GpuOnly, "VfxTrails");
                    clear = true; // (the particles out have no history)
                }
                if (st.Generation != vfx.Generation) clear = true;
                if (clear)
                {
                    st.Generation = vfx.Generation;
                    st.Head = 0;
                }
                bool worldSpace = child >= 0 || g.Space == ParticleSpace::World; // (children have no object: world space)
                uint32_t spawn = 0;
                if (child < 0)
                {
                    spawn = (uint32_t)std::min<int64_t>(std::max(vfx.PendingSpawn, 0), capacity);
                    vfx.PendingSpawn = 0;
                }
                // The alive count (the parent's): last time around this ring slot's, and a fresh counter.
                uint32_t slot = (uint32_t)(frame % kHitRing);
                bool dispatch = clear || dt > 0.0f || spawn > 0 || child >= 0;
                if (child < 0)
                {
                    Buffer& counter = st.Hits[slot];
                    if (counter.IsValid() && st.HitsFrame[slot] != 0 && st.HitsFrame[slot] <= device.GetCompletedFrameNumber())
                        vfx.AliveCount = *static_cast<const uint32_t*>(counter.GetMapped());
                    if (!counter.IsValid()) makeBuffer(counter, st.HitsAddress[slot], 16, MemoryUsage::GpuToCpu, "VfxAliveCount");
                    st.HitsFrame[slot] = dispatch ? frame : 0;
                }
                // The programs, their constants (exposed properties first: this object's values) and tables.
                std::vector<glm::vec4> constants = prog.Constants;
                for (size_t k = 0; k < g.Properties.size() && k < constants.size(); ++k)
                    if (glm::vec4 value; child < 0 && vfx.GetProperty(g.Properties[k].Name, value)) constants[k] = VisualEffects::PropertyConstant(g.Properties[k], value);
                size_t codeBytes = prog.Code.size() * sizeof(glm::uvec4), constBytes = constants.size() * sizeof(glm::vec4), lutBytes = prog.Luts.size() * sizeof(glm::vec4);
                size_t constOffset = std::max<size_t>(codeBytes, 16);
                TransientAllocation tables = device.AllocateTransient(constOffset + constBytes + lutBytes, 16);
                TransientAllocation simParams = device.AllocateTransient(sizeof(VfxParams), 16);
                TransientAllocation params = device.AllocateTransient(sizeof(GpuParticleParams), 16);
                if (!tables || !simParams || !params) return;
                auto* bytes = static_cast<uint8_t*>(tables.Data);
                if (codeBytes) std::memcpy(bytes, prog.Code.data(), codeBytes);
                std::memcpy(bytes + constOffset, constants.data(), constBytes);
                std::memcpy(bytes + constOffset + constBytes, prog.Luts.data(), lutBytes);
                glm::mat4 simToWorld = worldSpace ? glm::mat4(1.0f) : t.World;
                VfxParams vp{};
                vp.EmitterToSim = child >= 0 ? glm::mat4(1.0f) : worldSpace ? t.World : glm::mat4(1.0f);
                vp.Time = glm::vec4(vfx.Time, dt, 0.0f, 0.0f);
                vp.Counts = glm::uvec4(capacity, st.Head, spawn, seed ^ (uint32_t)(child + 1) * 668265263u);
                vp.Program = glm::uvec4(prog.InitStart, prog.InitCount, prog.UpdateStart, prog.UpdateCount);
                vp.Code = glm::uvec4(SplitAddress(tables.Address), SplitAddress(tables.Address + constOffset));
                vp.Tables = glm::uvec4(SplitAddress(tables.Address + constOffset + constBytes),
                                       child < 0 && dispatch ? SplitAddress(st.HitsAddress[slot]) : glm::uvec2(0u));
                vp.SimToWorld = simToWorld;
                if (eventSlots)
                    for (int k = 0; k < VfxProgram::kMaxChildren; ++k)
                    {
                        glm::uvec2 a = SplitAddress(eventSlots[k]);
                        vp.Events[k / 2][(k % 2) * 2] = a.x;
                        vp.Events[k / 2][(k % 2) * 2 + 1] = a.y;
                    }
                vp.Source = glm::uvec4(child >= 0 ? 1u : 0u, SplitAddress(childEvents), (uint32_t)VfxProgram::kMaxEvents);
                vp.Head = glm::uvec4(SplitAddress(child >= 0 ? st.VfxHeadAddress : 0), 0u, 0u);
                if (outMesh) vp.MeshOut = glm::uvec4(SplitAddress(st.VfxInstancesAddress), e.Index + 1, 0u);
                if (trails)
                {
                    vp.Trail = glm::vec4(1.0f, std::max(g.TrailLifetime, 1e-3f) / kGpuTrailPoints, 0.0f, 0.0f);
                    vp.TrailBuffer = glm::uvec4(SplitAddress(st.TrailAddress), 0u, 0u);
                }
                std::memcpy(simParams.Data, &vp, sizeof(vp));
                // Drawing as a GPU Particle System: the particles' own colors and sizes (flat tables), the effect's space.
                GpuParticleParams p{};
                p.Emitter = t.World;
                p.SimToWorld = simToWorld;
                p.WorldToSim = glm::inverse(simToWorld);
                p.Misc = glm::vec4(0.0f, worldSpace ? 1.0f : glm::length(glm::vec3(t.World[0])), 0.0f, 0.0f);
                p.Counts = glm::uvec4(capacity, st.Head, 0u, 0u);
                p.Lighting = glm::vec4(g.Lit ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f);
                for (int i = 0; i < kParticleLut; ++i) p.ColorLut[i] = p.CurveLut[i] = glm::vec4(1.0f);
                // Collide With Scene: the GPU particles' World collision (last frame's depth, the terrain, the colliders
                // near the effect), with the block's values. VfxSim.comp reads these parameters for it.
                if (const VfxNode* collide = VisualEffects::SceneCollision(g))
                {
                    auto in = [&](int k, float fallback) { return k < (int)collide->Inputs.size() ? collide->Inputs[k].x : fallback; };
                    float radius = std::max(in(2, 0.05f), 0.0f);
                    m_ParticleDepthWanted = true;
                    p.DepthViewProj = m_ParticleDepthViewProj;
                    p.DepthInvViewProj = glm::inverse(m_ParticleDepthViewProj);
                    p.DepthInfo = glm::uvec4(depthBindless, depthValid ? 1u : 0u, 0u, 0u);
                    p.DepthParams = glm::vec4(0.6f + radius, radius, 0.0f, 0.0f);
                    p.Collision = glm::vec4(2.0f, 0.0f, std::max(in(0, 0.4f), 0.0f), std::clamp(in(1, 0.2f), 0.0f, 1.0f));
                    p.Misc.x = std::clamp(in(3, 0.0f), 0.0f, 1.0f);
                    if (collideTerrain)
                    {
                        p.TerrainToWorld = collideTerrainWorld;
                        p.TerrainToLocal = glm::inverse(collideTerrainWorld);
                        p.TerrainInfo = glm::uvec4(SplitAddress(res.ParticleTerrain.Address), res.ParticleTerrain.Samples, 1u);
                        p.TerrainSize = glm::vec4(collideTerrain->Size, 0.0f, 0.0f);
                    }
                    fillColliders(p, glm::vec3(t.World[3]));
                    vp.Collide = glm::uvec4(SplitAddress(params.Address), 1u, 0u);
                    std::memcpy(simParams.Data, &vp, sizeof(vp));
                }
                if (trails)
                {
                    p.Trail = glm::vec4(1.0f, std::max(g.TrailLifetime, 1e-3f) / kGpuTrailPoints, std::max(g.TrailWidth, 0.0f), 0.0f);
                    p.TrailBuffer = glm::uvec4(SplitAddress(st.TrailAddress), 0u, 0u);
                }
                std::memcpy(params.Data, &p, sizeof(p));
                if (dispatch)
                {
                    beginCompute();
                    res.VfxSim.Bind(cmd);
                    if (clear) vkCmdFillBuffer(cmd, st.Particles.GetHandle(), 0, VK_WHOLE_SIZE, 0); // every slot dead
                    if (clear && outMesh) vkCmdFillBuffer(cmd, st.VfxInstances.GetHandle(), 0, VK_WHOLE_SIZE, 0);
                    if (clear && child >= 0) vkCmdFillBuffer(cmd, st.VfxHead.GetHandle(), 0, VK_WHOLE_SIZE, 0);
                    if (child < 0) vkCmdFillBuffer(cmd, st.Hits[slot].GetHandle(), 0, 16, 0);
                    toCompute();
                    struct { glm::uvec2 Particles, Params; } push{ SplitAddress(st.Address), SplitAddress(simParams.Address) };
                    vkCmdPushConstants(cmd, device.GetPipelineLayout(), VK_SHADER_STAGE_ALL, 0, sizeof(push), &push);
                    vkCmdDispatch(cmd, (capacity + 63) / 64, 1, 1);
                    if (child >= 0)
                    {
                        // The ring head moves on by the events taken, and the events are cleared for the next frame.
                        toCompute();
                        vp.Source.x = 2u;
                        TransientAllocation finish = device.AllocateTransient(sizeof(VfxParams), 16);
                        if (finish)
                        {
                            std::memcpy(finish.Data, &vp, sizeof(vp));
                            push.Params = SplitAddress(finish.Address);
                            vkCmdPushConstants(cmd, device.GetPipelineLayout(), VK_SHADER_STAGE_ALL, 0, sizeof(push), &push);
                            vkCmdDispatch(cmd, 1, 1, 1);
                        }
                    }
                    else
                        st.Head = (st.Head + spawn) % capacity;
                }
                if (outMesh)
                {
                    // Drawn by the mesh passes (BuildRenderList takes its materials), not as quads.
                    m_VfxMeshOutputs.push_back({ outMesh, g.Mesh, g.Material, st.VfxInstancesAddress, capacity, e.Index + 1 });
                    return;
                }
                bool sort = g.Sort && g.Blend == ParticleBlend::Alpha;
                if (sort) sortParticles(st, capacity, params.Address);
                auto tex = g.Texture.IsValid() ? AssetManager::GetTexture(g.Texture) : nullptr;
                m_GpuParticleDraws.push_back({ st.Address, params.Address, capacity, (tex ? tex : AssetManager::GetParticleTexture())->GetBindlessIndex(),
                                               e.Index + 1, g.Blend == ParticleBlend::Additive, 0, sort ? st.SortAddress : 0, trails });
            };
            // The children's event buffers (in the parent's state, one per slot), cleared when made or reset.
            VkDeviceAddress slots[VfxProgram::kMaxChildren] = {};
            auto& parentState = res.GpuParticleStates[parentKey];
            size_t children = std::min<size_t>(program->Children.size(), VfxProgram::kMaxChildren);
            bool freshEvents = false;
            if (parentState.VfxEvents.size() != children) parentState.VfxEvents.resize(children), freshEvents = true;
            for (size_t k = 0; k < children; ++k)
            {
                auto& ev = parentState.VfxEvents[k];
                if (!ev.Data.IsValid())
                {
                    makeBuffer(ev.Data, ev.Address, kEventBytes, MemoryUsage::GpuOnly, "VfxEvents");
                    freshEvents = true;
                }
                slots[k] = ev.Address;
            }
            // (new, or the effect restarts: no events left over)
            bool restart = !parentState.Particles.IsValid() || parentState.Generation != vfx.Generation ||
                           parentState.Capacity != (uint32_t)std::clamp(graph->Capacity, 1, (int32_t)kMaxGpuParticles);
            if ((freshEvents || restart) && children > 0)
            {
                beginCompute();
                for (size_t k = 0; k < children; ++k) vkCmdFillBuffer(cmd, parentState.VfxEvents[k].Data.GetHandle(), 0, 16, 0);
            }
            simulate(*graph, *program, parentKey, -1, 0, slots);
            for (size_t k = 0; k < children; ++k)
            {
                auto childGraph = AssetManager::GetVisualEffect(program->Children[k]);
                auto childProgram = childGraph ? VisualEffects::ProgramOf(program->Children[k]) : nullptr;
                if (!childGraph || !childProgram || !childProgram->Valid()) continue;
                toCompute(); // the parent's events are written
                simulate(*childGraph, *childProgram, parentKey ^ ((uint64_t)(k + 1) << 44), (int)k, slots[k], nullptr);
            }
        }
        if (!first)
            Vk::MemoryBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
                              VK_ACCESS_2_SHADER_READ_BIT);

        // Forget systems no view has drawn for a while (deleted, or their scene closed).
        if (frame % 120 == 0)
            std::erase_if(res.GpuParticleStates, [&](const auto& kv) { return kv.second.LastFrame + 240 < frame; });
        std::stable_sort(m_GpuParticleDraws.begin(), m_GpuParticleDraws.end(),
                         [](const GpuParticleDraw& a, const GpuParticleDraw& b) { return a.Order < b.Order; });
    }

    void SceneRenderer::DrawGpuParticles(VkCommandBuffer cmd)
    {
        if (m_GpuParticleDraws.empty()) return;
        auto& device = RenderDevice::Get();
        auto& res = Renderer::Get();
        vkCmdBindIndexBuffer(cmd, res.QuadIndices.GetHandle(), 0, VK_INDEX_TYPE_UINT32);
        for (const GpuParticleDraw& d : m_GpuParticleDraws)
        {
            if (!DebugDraw("Opaque", "HDR Color", d.Trails ? "Particles and Trails" : "Particles", d.Entity, d.Capacity, d.Capacity * 2)) continue;
            if (d.Trails)
            {
                // The ribbons first, under the particles: TRAIL_POINTS segments per slot.
                MainPipeline(d.Additive ? MainPipelines::GpuTrailsAdditive : MainPipelines::GpuTrails).Bind(cmd);
                struct { glm::uvec2 Particles, Params; uint32_t Texture, Entity; } push{ SplitAddress(d.Particles), SplitAddress(d.Params),
                                                                                      AssetManager::GetParticleTexture()->GetBindlessIndex(), d.Entity };
                vkCmdPushConstants(cmd, device.GetPipelineLayout(), VK_SHADER_STAGE_ALL, 0, sizeof(push), &push);
                uint32_t segments = d.Capacity * kGpuTrailPoints;
                for (uint32_t firstQuad = 0; firstQuad < segments; firstQuad += kMaxQuadsPerDraw)
                {
                    uint32_t n = std::min(kMaxQuadsPerDraw, segments - firstQuad);
                    vkCmdDrawIndexed(cmd, n * 6, 1, 0, (int32_t)(firstQuad * 4), 0);
                    m_Stats.DrawCalls++;
                }
            }
            MainPipeline(d.Additive ? MainPipelines::GpuParticlesAdditive : MainPipelines::GpuParticles).Bind(cmd);
            struct { glm::uvec2 Particles, Params; uint32_t Texture, Entity; glm::uvec2 Sorted; } push{
                SplitAddress(d.Particles), SplitAddress(d.Params), d.Texture, d.Entity, SplitAddress(d.Sorted) };
            vkCmdPushConstants(cmd, device.GetPipelineLayout(), VK_SHADER_STAGE_ALL, 0, sizeof(push), &push);
            for (uint32_t firstQuad = 0; firstQuad < d.Capacity; firstQuad += kMaxQuadsPerDraw)
            {
                uint32_t n = std::min(kMaxQuadsPerDraw, d.Capacity - firstQuad);
                vkCmdDrawIndexed(cmd, n * 6, 1, 0, (int32_t)(firstQuad * 4), 0);
                m_Stats.DrawCalls++;
            }
            m_Stats.GpuParticleCapacity += d.Capacity;
        }
    }

    void SceneRenderer::FullscreenPass(VkCommandBuffer cmd, const Image& dst, bool load, const Pipeline& pipeline, const void* push, uint32_t pushSize)
    {
        auto& device = RenderDevice::Get();
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
        vkCmdPushConstants(cmd, device.GetPipelineLayout(), VK_SHADER_STAGE_ALL, 0, pushSize, push);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRendering(cmd);
        Vk::TransitionImage(cmd, dst.GetHandle(), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        m_Stats.DrawCalls++;
    }

    void SceneRenderer::CollectDecals(Scene& scene)
    {
        m_Decals.clear();
        for (auto [e, d, t] : scene.GetRegistry().ViewOf<DecalComponent, TransformComponent>())
        {
            if (!t.ActiveInHierarchy || d.Color.a <= 0.0f) continue;
            if (LayerCulled(scene, e)) continue;
            auto tex = d.Texture.IsValid() ? AssetManager::GetTexture(d.Texture) : nullptr;
            DecalDraw draw;
            draw.Model = t.World * glm::scale(glm::mat4(1.0f), glm::max(glm::abs(d.Size), glm::vec3(1e-3f)));
            draw.Color = ToLinear(d.Color);
            draw.Texture = (tex ? tex : AssetManager::GetParticleTexture())->GetBindlessIndex();
            draw.Blend = (uint32_t)std::clamp((int)d.Blend, 0, 2);
            draw.AngleFade = std::clamp(d.AngleFade, 0.0f, 1.0f);
            draw.Order = d.SortingOrder;
            m_Decals.push_back(draw);
        }
        // Particles' collision decals: each mark a small projector facing into the surface it struck, turned at random
        // and fading out over the last third of its life.
        for (auto [e, ps, t] : scene.GetRegistry().ViewOf<ParticleSystemComponent, TransformComponent>())
        {
            if (ps.Decals.empty() || !t.ActiveInHierarchy) continue;
            if (LayerCulled(scene, e)) continue;
            auto tex = ps.DecalTexture.IsValid() ? AssetManager::GetTexture(ps.DecalTexture) : nullptr;
            uint32_t texture = (tex ? tex : AssetManager::GetParticleTexture())->GetBindlessIndex();
            float lifetime = std::max(ps.DecalLifetime, 1e-4f);
            for (const auto& mark : ps.Decals)
            {
                glm::vec3 z = glm::normalize(mark.Normal); // the decal projects along its -Z: into the surface
                glm::vec3 x = glm::normalize(glm::cross(std::abs(z.y) < 0.9f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0), z));
                glm::vec3 y = glm::cross(z, x);
                float c = std::cos(mark.Spin), s = std::sin(mark.Spin);
                glm::vec3 rx = x * c + y * s, ry = y * c - x * s;
                DecalDraw draw;
                draw.Model = glm::mat4(glm::vec4(rx * mark.Size, 0), glm::vec4(ry * mark.Size, 0), glm::vec4(z * mark.Size * 0.5f, 0), glm::vec4(mark.Position, 1));
                glm::vec4 color = ToLinear(mark.Color);
                color.a *= std::clamp((lifetime - mark.Age) / (lifetime / 3.0f), 0.0f, 1.0f);
                draw.Color = color;
                draw.Texture = texture;
                draw.Blend = (uint32_t)std::clamp(ps.DecalBlendMode, 0, 2);
                draw.AngleFade = 0.3f;
                draw.Order = ps.SortingOrder;
                m_Decals.push_back(draw);
            }
        }
        std::stable_sort(m_Decals.begin(), m_Decals.end(), [](const DecalDraw& a, const DecalDraw& b) { return a.Order < b.Order; });
    }

    void SceneRenderer::RenderVelocity(VkCommandBuffer cmd, SceneRenderTarget& target, const glm::mat4& viewProj, const glm::mat4& unjitteredViewProj,
                                       const glm::vec4& wind)
    {
        auto& device = RenderDevice::Get();
        auto& res = Renderer::Get();
        auto& motion = target.Motion;
        bool history = motion.Valid;
        glm::mat4 prevViewProj = history ? motion.PrevViewProj : unjitteredViewProj;
        uint64_t frame = ++motion.Frame;

        // The camera's motion everywhere.
        struct { glm::mat4 Reproject; uint32_t Depth, Pad; } cameraPush{ prevViewProj * glm::inverse(unjitteredViewProj), target.GetDepthBindless(), 0u };
        FullscreenPass(cmd, target.GetVelocity(), false, res.VelocityCamera, &cameraPush, sizeof(cameraPush));

        // Objects that moved or changed pose since the last frame (each item keyed by its entity and its place among
        // that entity's items), and those never to blur; everything else moves only with the camera.
        struct ObjectDraw { uint32_t Item, Object; };
        std::vector<ObjectDraw> draws;
        std::vector<VelocityGPU> objects;
        std::unordered_map<uint32_t, uint32_t> occurrence;
        Frustum frustum(viewProj);
        // Grass tufts swaying in the wind: every visible one, grouped by mesh into instanced draws.
        struct GrassGroup
        {
            uint32_t Item;
            std::vector<VelocityGPU> Entries;
        };
        std::map<std::tuple<const Mesh*, uint32_t, uint32_t>, GrassGroup> grass;
        float windTime = wind.w, prevWind = history ? motion.PrevWindTime : windTime;
        bool windMoves = prevWind != windTime && glm::vec3(wind) != glm::vec3(0.0f);
        for (size_t i = 0; i < m_RenderItems.size(); ++i)
        {
            const RenderItem& item = m_RenderItems[i];
            if (item.Motion && !item.Vertices && !item.Joints && glm::unpackHalf2x16(item.Motion).y >= 0.0f)
            {
                if (item.ShadowOnly || (!m_HiddenIds.empty() && m_HiddenIds.contains(item.EntityId))) continue;
                if (m_ItemOccluded.size() == m_RenderItems.size() && m_ItemOccluded[i]) continue;
                if (!item.AlwaysVisible && !frustum.IntersectsWorld(item.Center, item.Extents)) continue;
                const RenderPart& first = m_RenderParts[item.FirstPart];
                GrassGroup& group = grass[{ item.MeshRef.get(), first.FirstIndex, item.PartCount }];
                if (group.Entries.empty()) group.Item = (uint32_t)i;
                group.Entries.push_back({ item.World, item.World, glm::uvec4(0u), glm::uvec4(0u, 0u, item.Motion, 0u) });
                continue;
            }
            // Vertices made each frame that are not simulated (the ocean): camera only. Fur shells follow their
            // object, and sway in the wind on their own.
            bool shell = item.Motion && item.FurWind;
            bool swaying = shell && windMoves && glm::unpackHalf2x16(item.FurWind).y != 0.0f;
            // The ocean: its waves move every vertex while time runs (where they were comes from the wave time).
            bool waves = item.Ocean && m_OceanFrame.Volume && history && motion.PrevOceanTime != m_OceanFrame.Time;
            swaying |= waves;
            if ((item.MotionVectors == 0 && !swaying) || (item.Motion && !shell) || item.ShadowOnly || (item.Vertices && item.SimPositions < 0 && !item.Ocean)) continue;
            uint64_t key = ((uint64_t)item.EntityId << 32) | occurrence[item.EntityId]++;
            auto& last = motion.Objects[key];
            bool had = history && last.Frame + 1 == frame;
            glm::mat4 prevWorld = had ? last.World : item.World;
            bool posed = item.Joints && item.JointMatrices;
            bool samePose = !posed || !had || last.Joints.size() != item.JointMatrices->size() ||
                            std::memcmp(last.Joints.data(), item.JointMatrices->data(), last.Joints.size() * sizeof(glm::mat4)) == 0;
            const std::vector<glm::vec3>* positions = item.SimPositions >= 0 ? &m_SimPositions[item.SimPositions] : nullptr;
            bool samePositions = !positions || !had || last.Positions.size() != positions->size() ||
                                 std::memcmp(last.Positions.data(), positions->data(), positions->size() * sizeof(glm::vec3)) == 0;
            bool moved = had && (prevWorld != item.World || !samePose || !samePositions || swaying);
            VelocityGPU gpu{ item.World, prevWorld, glm::uvec4(0u), glm::uvec4(0u) };
            if (shell) gpu.Extra.z = item.Motion, gpu.Extra.w = item.FurWind;
            if (waves && item.MotionVectors != 2)
            {
                const OceanFrame& o = m_OceanFrame;
                Water::OceanVertices(*o.Volume, o.Center, o.Radius, motion.PrevOceanTime, m_OceanPrevVertices);
                if (TransientAllocation block = device.AllocateTransient(m_OceanPrevVertices.size() * sizeof(glm::vec3), 16))
                {
                    auto* out = (glm::vec3*)block.Data; // written once (mapped GPU memory)
                    for (size_t v = 0; v < m_OceanPrevVertices.size(); ++v) out[v] = m_OceanPrevVertices[v].Position;
                    gpu.Extra.x = SplitAddress(block.Address).x;
                    gpu.Extra.y = SplitAddress(block.Address).y;
                }
            }
            if (moved && !samePositions && item.MotionVectors == 1)
                if (TransientAllocation block = device.AllocateTransient(last.Positions.size() * sizeof(glm::vec3), 16))
                {
                    std::memcpy(block.Data, last.Positions.data(), last.Positions.size() * sizeof(glm::vec3));
                    gpu.Extra = glm::uvec4(SplitAddress(block.Address), gpu.Extra.z, gpu.Extra.w);
                }
            if (item.MotionVectors == 2) gpu.PrevModel = item.World; // no motion of its own (the camera's is taken off below)
            if (moved && posed && item.MotionVectors == 1)
            {
                size_t bytes = last.Joints.size() * sizeof(glm::mat4);
                if (TransientAllocation joints = !samePose ? device.AllocateTransient(bytes, 16) : TransientAllocation{})
                {
                    std::memcpy(joints.Data, last.Joints.data(), bytes);
                    glm::uvec2 now = SplitAddress(item.Joints), before = SplitAddress(joints.Address);
                    gpu.Joints = glm::uvec4(now, before);
                }
                else
                    gpu.Joints = glm::uvec4(SplitAddress(item.Joints), SplitAddress(item.Joints));
            }
            last.World = item.World;
            if (posed) last.Joints = *item.JointMatrices;
            else last.Joints.clear();
            if (positions) last.Positions = *positions;
            else last.Positions.clear();
            last.Frame = frame;
            if (!moved && item.MotionVectors != 2) continue;
            if (!m_HiddenIds.empty() && m_HiddenIds.contains(item.EntityId)) continue;
            if (m_ItemOccluded.size() == m_RenderItems.size() && m_ItemOccluded[i]) continue;
            if (!item.AlwaysVisible && !frustum.IntersectsWorld(item.Center, item.Extents)) continue;
            if (item.MotionVectors == 2)
            {
                // Force No Motion: last frame's matrices put it where the camera would have seen it then, so its
                // velocity comes out zero.
                gpu.PrevModel = glm::inverse(prevViewProj) * unjitteredViewProj * item.World;
                if (item.Joints) gpu.Joints = glm::uvec4(SplitAddress(item.Joints), SplitAddress(item.Joints));
            }
            uint32_t object = (uint32_t)objects.size();
            objects.push_back(gpu);
            draws.push_back({ (uint32_t)i, object });
        }
        // Sprites (and text, tilemaps) that moved since the last frame, by their object's last transform, and particles
        // by their own velocity; ribbons and trails move only with the camera.
        std::vector<VelocitySpriteVertex> spriteVertices;
        const glm::vec4 corners[4] = { { -0.5f, -0.5f, 0, 1 }, { 0.5f, -0.5f, 0, 1 }, { 0.5f, 0.5f, 0, 1 }, { -0.5f, 0.5f, 0, 1 } };
        for (const SpriteDraw& s : m_Sprites)
        {
            if (s.MotionKind == 2) continue;
            if (s.MotionKind == 3)
            {
                // Corners with their own last positions (a trail's head).
                for (int i = 0; i < 4; ++i)
                {
                    glm::vec3 p = s.HasCorners ? s.P[i] : glm::vec3(s.Transform * corners[i]);
                    glm::vec2 uv = s.HasCorners ? s.UV[i] : glm::vec2(i == 0 || i == 3 ? s.UVMin.x : s.UVMax.x, i < 2 ? s.UVMin.y : s.UVMax.y);
                    spriteVertices.push_back({ p, s.Before[i], uv, s.Texture, s.HasCorners ? s.C[i].a : s.Color.a });
                }
                continue;
            }
            glm::mat4 prevTransform = s.Transform;
            glm::vec3 displacement(0.0f);
            if (s.MotionKind == 0)
            {
                uint64_t key = ((uint64_t)s.Entity << 32) | 0x80000000ull | s.Occurrence;
                auto& last = motion.Objects[key];
                bool had = history && last.Frame + 1 == frame;
                if (had) prevTransform = last.World;
                last.World = s.Transform;
                last.Frame = frame;
                if (!had || prevTransform == s.Transform) continue;
            }
            else
            {
                displacement = s.Displacement;
                if (displacement == glm::vec3(0.0f)) continue;
            }
            if (s.HasCorners)
            {
                // A local-space line's corners, carried by its object's last transform.
                glm::mat4 back = prevTransform * glm::inverse(s.Transform);
                for (int i = 0; i < 4; ++i) spriteVertices.push_back({ s.P[i], glm::vec3(back * glm::vec4(s.P[i], 1.0f)), s.UV[i], s.Texture, s.C[i].a });
                continue;
            }
            glm::vec2 uvs[4] = { { s.UVMin.x, s.UVMin.y }, { s.UVMax.x, s.UVMin.y }, { s.UVMax.x, s.UVMax.y }, { s.UVMin.x, s.UVMax.y } };
            for (int i = 0; i < 4; ++i)
            {
                glm::vec3 p = glm::vec3(s.Transform * corners[i]);
                glm::vec3 before = s.MotionKind == 0 ? glm::vec3(prevTransform * corners[i]) : p - displacement;
                spriteVertices.push_back({ p, before, uvs[i], s.Texture, s.Color.a });
            }
        }
        std::erase_if(motion.Objects, [frame](const auto& entry) { return entry.second.Frame != frame; });
        motion.PrevViewProj = unjitteredViewProj;
        motion.PrevWindTime = windTime;
        if (m_OceanFrame.Volume) motion.PrevOceanTime = m_OceanFrame.Time;
        motion.Valid = true;
        struct GrassDraw { uint32_t Item, First, Count; };
        std::vector<GrassDraw> grassDraws;
        for (auto& [key, group] : grass)
        {
            grassDraws.push_back({ group.Item, (uint32_t)objects.size(), (uint32_t)group.Entries.size() });
            objects.insert(objects.end(), group.Entries.begin(), group.Entries.end());
        }
        if (draws.empty() && spriteVertices.empty() && m_GpuParticleDraws.empty() && grassDraws.empty()) return;

        TransientAllocation objectBlock = objects.empty() ? TransientAllocation{} : device.AllocateTransient(objects.size() * sizeof(VelocityGPU), 16);
        TransientAllocation frameBlock = device.AllocateTransient(2 * sizeof(glm::mat4) + sizeof(glm::vec4), 16);
        size_t spriteCount = std::min(spriteVertices.size() / 4, (size_t)kMaxQuadsPerDraw);
        TransientAllocation spriteBlock = spriteCount ? device.AllocateTransient(spriteCount * 4 * sizeof(VelocitySpriteVertex), 16) : TransientAllocation{};
        if ((!objects.empty() && !objectBlock) || !frameBlock) return;
        if (objectBlock) std::memcpy(objectBlock.Data, objects.data(), objects.size() * sizeof(VelocityGPU));
        if (spriteBlock) std::memcpy(spriteBlock.Data, spriteVertices.data(), spriteCount * 4 * sizeof(VelocitySpriteVertex));
        glm::mat4 matrices[2] = { unjitteredViewProj, prevViewProj };
        glm::vec4 frameParams(prevWind, 0.0f, 0.0f, 0.0f);
        std::memcpy(frameBlock.Data, matrices, sizeof(matrices));
        std::memcpy((char*)frameBlock.Data + sizeof(matrices), &frameParams, sizeof(frameParams));

        VkExtent2D extent{ target.GetWidth(), target.GetHeight() };
        Vk::TransitionImage(cmd, target.GetVelocity().GetHandle(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        VkRenderingAttachmentInfo att{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
        att.imageView = target.GetVelocity().GetView();
        att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        att.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingInfo info{ VK_STRUCTURE_TYPE_RENDERING_INFO };
        info.renderArea = { { 0, 0 }, extent };
        info.layerCount = 1;
        info.colorAttachmentCount = 1;
        info.pColorAttachments = &att;
        vkCmdBeginRendering(cmd, &info);
        VkViewport viewport{ 0.0f, (float)extent.height, (float)extent.width, -(float)extent.height, 0.0f, 1.0f }; // as the main pass
        VkRect2D scissor{ { 0, 0 }, extent };
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &scissor);
        if (!draws.empty() || !grassDraws.empty()) res.Velocity.Bind(cmd);
        Mesh* bound = nullptr;
        // Water last: its surface covers whatever under it moved (it is not in the depth to hide them).
        std::stable_partition(draws.begin(), draws.end(), [this](const ObjectDraw& d)
                              { const RenderItem& it = m_RenderItems[d.Item]; return !(it.PartCount && m_RenderParts[it.FirstPart].Water); });
        for (const ObjectDraw& d : draws)
        {
            const RenderItem& item = m_RenderItems[d.Item];
            if (!DebugDraw("Motion Vectors", "Velocity", item.MeshRef->GetName(), item.EntityId, 1, 0)) continue;
            bool water = item.PartCount && m_RenderParts[item.FirstPart].Water;
            struct { VkDeviceAddress Objects, Skin, Frame; uint32_t Depth, Flags; } push{ objectBlock.Address, item.Joints ? item.Skin : 0,
                                                                                          frameBlock.Address, target.GetDepthBindless(), water ? 1u : 0u };
            vkCmdPushConstants(cmd, device.GetPipelineLayout(), VK_SHADER_STAGE_ALL, 0, sizeof(push), &push);
            if (item.MeshRef.get() != bound)
            {
                item.MeshRef->Bind(cmd);
                bound = item.MeshRef.get();
            }
            if (item.Vertices)
            {
                // Simulated vertices (and torn cloth's triangles) instead of the mesh's.
                vkCmdBindVertexBuffers(cmd, 0, 1, &item.Vertices, &item.VertexOffset);
                if (item.Indices) vkCmdBindIndexBuffer(cmd, item.Indices, item.IndexOffset, VK_INDEX_TYPE_UINT32);
                bound = nullptr;
            }
            // Every part of the item, opaque ones only (transparent surfaces are not in the depth), and water.
            for (uint32_t p = item.FirstPart; p < item.FirstPart + item.PartCount; ++p)
            {
                const RenderPart& part = m_RenderParts[p];
                if (part.Transparent && !part.Water) continue;
                vkCmdDrawIndexed(cmd, part.IndexCount, 1, part.FirstIndex, 0, d.Object);
                m_Stats.DrawCalls++;
            }
        }
        for (const GrassDraw& d : grassDraws)
        {
            const RenderItem& item = m_RenderItems[d.Item];
            if (!DebugDraw("Motion Vectors", "Velocity", "Grass", item.EntityId, d.Count, 0)) continue;
            struct { VkDeviceAddress Objects, Skin, Frame; uint32_t Depth, Pad; } push{ objectBlock.Address, 0, frameBlock.Address, target.GetDepthBindless(), 0u };
            vkCmdPushConstants(cmd, device.GetPipelineLayout(), VK_SHADER_STAGE_ALL, 0, sizeof(push), &push);
            if (item.MeshRef.get() != bound)
            {
                item.MeshRef->Bind(cmd);
                bound = item.MeshRef.get();
            }
            for (uint32_t p = item.FirstPart; p < item.FirstPart + item.PartCount; ++p)
            {
                const RenderPart& part = m_RenderParts[p];
                if (part.Transparent) continue;
                for (uint32_t done = 0; done < d.Count; done += kMaxInstancesPerDraw)
                {
                    vkCmdDrawIndexed(cmd, part.IndexCount, std::min(kMaxInstancesPerDraw, d.Count - done), part.FirstIndex, 0, d.First + done);
                    m_Stats.DrawCalls++;
                }
            }
        }
        if (spriteBlock && DebugDraw("Motion Vectors", "Velocity", "Sprites", 0, 1, (uint32_t)spriteCount * 2))
        {
            res.VelocitySprite.Bind(cmd);
            struct { VkDeviceAddress Frame; uint32_t Depth, Pad; } push{ frameBlock.Address, target.GetDepthBindless(), 0u };
            vkCmdPushConstants(cmd, device.GetPipelineLayout(), VK_SHADER_STAGE_ALL, 0, sizeof(push), &push);
            VkBuffer buffer = spriteBlock.Buffer;
            vkCmdBindVertexBuffers(cmd, 0, 1, &buffer, &spriteBlock.Offset);
            vkCmdBindIndexBuffer(cmd, res.QuadIndices.GetHandle(), 0, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(cmd, (uint32_t)spriteCount * 6, 1, 0, 0, 0);
            m_Stats.DrawCalls++;
        }
        // GPU particles, from their simulation buffers.
        if (!m_GpuParticleDraws.empty())
        {
            res.VelocityParticleGpu.Bind(cmd);
            vkCmdBindIndexBuffer(cmd, res.QuadIndices.GetHandle(), 0, VK_INDEX_TYPE_UINT32);
            for (const GpuParticleDraw& d : m_GpuParticleDraws)
            {
                if (!DebugDraw("Motion Vectors", "Velocity", "Particles", d.Entity, d.Capacity, d.Capacity * 2)) continue;
                struct
                {
                    VkDeviceAddress Frame;
                    uint32_t Depth, Pad;
                    glm::uvec2 Particles, Params, Sorted;
                    uint32_t Texture, Pad2;
                } push{ frameBlock.Address, target.GetDepthBindless(), 0u, SplitAddress(d.Particles), SplitAddress(d.Params), SplitAddress(d.Sorted), d.Texture, 0u };
                vkCmdPushConstants(cmd, device.GetPipelineLayout(), VK_SHADER_STAGE_ALL, 0, sizeof(push), &push);
                for (uint32_t firstQuad = 0; firstQuad < d.Capacity; firstQuad += kMaxQuadsPerDraw)
                {
                    uint32_t n = std::min(kMaxQuadsPerDraw, d.Capacity - firstQuad);
                    vkCmdDrawIndexed(cmd, n * 6, 1, 0, (int32_t)(firstQuad * 4), 0);
                    m_Stats.DrawCalls++;
                }
            }
        }
        vkCmdEndRendering(cmd);
        Vk::TransitionImage(cmd, target.GetVelocity().GetHandle(), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }

    void SceneRenderer::DrawDecals(VkCommandBuffer cmd, SceneRenderTarget& target)
    {
        auto& device = RenderDevice::Get();
        auto& res = Renderer::Get();
        Ref<Mesh> cube = AssetManager::GetMesh(AssetManager::kCube);
        if (!cube) return;
        VkExtent2D extent{ target.GetWidth(), target.GetHeight() };
        Vk::TransitionImage(cmd, target.GetHdr().GetHandle(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        VkRenderingAttachmentInfo att{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
        att.imageView = target.GetHdr().GetView();
        att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        att.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingInfo info{ VK_STRUCTURE_TYPE_RENDERING_INFO };
        info.renderArea = { { 0, 0 }, extent };
        info.layerCount = 1;
        info.colorAttachmentCount = 1;
        info.pColorAttachments = &att;
        vkCmdBeginRendering(cmd, &info);
        VkViewport viewport{ 0.0f, (float)extent.height, (float)extent.width, -(float)extent.height, 0.0f, 1.0f }; // as the main pass
        VkRect2D scissor{ { 0, 0 }, extent };
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &scissor);
        cube->Bind(cmd);
        for (const DecalDraw& d : m_Decals)
        {
            if (!DebugDraw("Decals", "HDR Color", "Decal", 0, 1, cube->GetIndexCount() / 3)) continue;
            res.Decals[d.Blend].Bind(cmd);
            struct { glm::mat4 Model; glm::vec4 Color; uint32_t Texture, Depth, Blend; float AngleFade; } push{
                d.Model, d.Color, d.Texture, target.GetDepthBindless(), d.Blend, d.AngleFade };
            vkCmdPushConstants(cmd, device.GetPipelineLayout(), VK_SHADER_STAGE_ALL, 0, sizeof(push), &push);
            vkCmdDrawIndexed(cmd, cube->GetIndexCount(), 1, 0, 0, 0);
            m_Stats.DrawCalls++;
        }
        vkCmdEndRendering(cmd);
        Vk::TransitionImage(cmd, target.GetHdr().GetHandle(), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }

    void SceneRenderer::RenderIndirect(VkCommandBuffer cmd, SceneRenderTarget& target, const SceneSettings& settings, const glm::mat4& viewProj)
    {
        // Trace bounce light at half resolution (reading the HDR image), filter it over time into a full-resolution
        // history (reprojected, clamped to the trace), then add that onto the HDR image through the reflections' blur.
        auto& res = Renderer::Get();
        VkExtent2D extent{ target.GetWidth(), target.GetHeight() };
        const Image& traced = target.GetIndirect();
        struct
        {
            uint32_t Hdr, Depth, Surface, Rays;
            glm::vec2 Texel;
            float MaxDistance, Thickness, Intensity;
            uint32_t Steps;
            float Scale;
        } trace{ target.GetHdrBindless(), target.GetDepthBindless(), target.GetSurfaceBindless(), (uint32_t)std::clamp(settings.SSGIRays, 1, 16),
                 glm::vec2(1.0f / traced.GetWidth(), 1.0f / traced.GetHeight()), std::max(settings.SSGIDistance, 0.05f), std::max(settings.SSGIThickness, 0.001f),
                 settings.SSGIIntensity, 16u, (float)extent.width / traced.GetWidth() };
        FullscreenPass(cmd, traced, false, res.Ssgi, &trace, sizeof(trace));
        auto& gi = target.Gi;
        uint32_t read = gi.Written, write = 1 - gi.Written;
        struct
        {
            glm::mat4 PrevViewProj;
            uint32_t Current, History, Depth, Valid;
            glm::vec2 Texel;
            float Blend;
        } temporal{ gi.PrevViewProj, target.GetIndirectBindless(), target.GetGiHistoryBindless(read), target.GetDepthBindless(), gi.Valid ? 1u : 0u,
                    trace.Texel, 0.9f };
        FullscreenPass(cmd, target.GetGiHistory(write), false, res.SsgiTemporal, &temporal, sizeof(temporal));
        gi.Written = write;
        gi.Valid = true;
        gi.PrevViewProj = viewProj;
        struct { uint32_t Reflections, Pad; glm::vec2 Texel; } apply{ target.GetGiHistoryBindless(write), 0u, glm::vec2(1.0f / extent.width, 1.0f / extent.height) };
        FullscreenPass(cmd, target.GetHdr(), true, res.SsrApply, &apply, sizeof(apply));
    }

    void SceneRenderer::RenderReflections(VkCommandBuffer cmd, SceneRenderTarget& target, const SceneSettings& settings)
    {
        auto& device = RenderDevice::Get();
        auto& res = Renderer::Get();
        VkExtent2D extent{ target.GetWidth(), target.GetHeight() };
        auto pass = [&](const Image& dst, bool load, const Pipeline& pipeline, const void* push, uint32_t pushSize) {
            Vk::TransitionImage(cmd, dst.GetHandle(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
            VkRenderingAttachmentInfo att{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
            att.imageView = dst.GetView();
            att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            att.loadOp = load ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            VkRenderingInfo info{ VK_STRUCTURE_TYPE_RENDERING_INFO };
            info.renderArea = { { 0, 0 }, extent };
            info.layerCount = 1;
            info.colorAttachmentCount = 1;
            info.pColorAttachments = &att;
            vkCmdBeginRendering(cmd, &info);
            VkViewport viewport{ 0.0f, 0.0f, (float)extent.width, (float)extent.height, 0.0f, 1.0f };
            VkRect2D scissor{ { 0, 0 }, extent };
            vkCmdSetViewport(cmd, 0, 1, &viewport);
            vkCmdSetScissor(cmd, 0, 1, &scissor);
            pipeline.Bind(cmd);
            vkCmdPushConstants(cmd, device.GetPipelineLayout(), VK_SHADER_STAGE_ALL, 0, pushSize, push);
            vkCmdDraw(cmd, 3, 1, 0, 0);
            vkCmdEndRendering(cmd);
            Vk::TransitionImage(cmd, dst.GetHandle(), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            m_Stats.DrawCalls++;
        };

        // Trace into the reflections image (reading the HDR image), then add it onto the HDR image.
        struct
        {
            uint32_t Hdr, Depth, Surface, Steps;
            glm::vec2 Texel;
            float MaxDistance, Thickness, MaxRoughness, Intensity;
        } trace{ target.GetHdrBindless(), target.GetDepthBindless(), target.GetSurfaceBindless(), (uint32_t)std::clamp(settings.SSRSteps, 8, 256),
                 glm::vec2(1.0f / extent.width, 1.0f / extent.height), std::max(settings.SSRMaxDistance, 0.1f),
                 std::max(settings.SSRThickness, 0.001f), std::clamp(settings.SSRMaxRoughness, 0.01f, 1.0f), settings.SSRIntensity };
        pass(target.GetReflections(), false, res.Ssr, &trace, sizeof(trace));
        struct { uint32_t Reflections, Pad; glm::vec2 Texel; } apply{ target.GetReflectionsBindless(), 0u, trace.Texel };
        pass(target.GetHdr(), true, res.SsrApply, &apply, sizeof(apply));
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

    void SceneRenderer::RenderAmbientOcclusion(VkCommandBuffer cmd, Scene& scene, SceneRenderTarget& target, const glm::mat4& viewProj,
                                               const SceneSettings& settings, bool ambientOcclusion, bool copyDepth)
    {
        auto& device = RenderDevice::Get();
        auto& res = Renderer::Get();
        VkExtent2D extent{ target.GetWidth(), target.GetHeight() };

        // Opaque geometry into the scene depth; the main pass then loads it instead of clearing.
        VkRenderingAttachmentInfo depth{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
        depth.imageView = target.GetDepth().GetView();
        depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        depth.clearValue.depthStencil = { 1.0f, 0 };
        if (m_Samples > 1)
        {
            // Multisampled depth (the main pass loads it), resolved for SSAO to read.
            MultisampleInto(depth, target.GetDepthMS(), target.GetDepth(), VK_RESOLVE_MODE_SAMPLE_ZERO_BIT);
            depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        }
        VkRenderingInfo info{ VK_STRUCTURE_TYPE_RENDERING_INFO };
        info.renderArea = { { 0, 0 }, extent };
        info.layerCount = 1;
        info.pDepthAttachment = &depth;
        vkCmdBeginRendering(cmd, &info);
        VkViewport viewport{ 0.0f, (float)extent.height, (float)extent.width, -(float)extent.height, 0.0f, 1.0f };
        VkRect2D scissor{ { 0, 0 }, extent };
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &scissor);
        DrawMeshes(cmd, viewProj, MeshPass::Depth);
        vkCmdEndRendering(cmd);

        VkImage depthImage = target.GetDepth().GetHandle();
        Vk::TransitionImage(cmd, depthImage, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                            VK_IMAGE_ASPECT_DEPTH_BIT);
        if (copyDepth)
        {
            uint32_t source = target.GetDepthBindless();
            FullscreenPass(cmd, target.GetOpaqueDepth(), false, res.DepthCopy, &source, sizeof(source));
        }
        if (!ambientOcclusion)
        {
            Vk::TransitionImage(cmd, depthImage, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                                VK_IMAGE_ASPECT_DEPTH_BIT);
            return;
        }
        auto pass = [&](const Image& dst, const Pipeline& pipeline, const void* push, uint32_t pushSize) {
            Vk::TransitionImage(cmd, dst.GetHandle(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
            VkRenderingAttachmentInfo att{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
            att.imageView = dst.GetView();
            att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            att.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            VkRenderingInfo pi{ VK_STRUCTURE_TYPE_RENDERING_INFO };
            pi.renderArea = { { 0, 0 }, dst.GetExtent() };
            pi.layerCount = 1;
            pi.colorAttachmentCount = 1;
            pi.pColorAttachments = &att;
            vkCmdBeginRendering(cmd, &pi);
            VkViewport flat{ 0.0f, 0.0f, (float)dst.GetWidth(), (float)dst.GetHeight(), 0.0f, 1.0f };
            VkRect2D rect{ { 0, 0 }, dst.GetExtent() };
            vkCmdSetViewport(cmd, 0, 1, &flat);
            vkCmdSetScissor(cmd, 0, 1, &rect);
            pipeline.Bind(cmd);
            vkCmdPushConstants(cmd, device.GetPipelineLayout(), VK_SHADER_STAGE_ALL, 0, pushSize, push);
            vkCmdDraw(cmd, 3, 1, 0, 0);
            vkCmdEndRendering(cmd);
            Vk::TransitionImage(cmd, dst.GetHandle(), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            m_Stats.DrawCalls++;
        };
        SsaoPush ao{ target.GetDepthBindless(), 16u, glm::vec2(1.0f / extent.width, 1.0f / extent.height),
                     std::max(settings.SSAORadius, 0.01f), std::max(settings.SSAOIntensity, 0.0f), settings.SSAOBias, 0.0f };
        pass(target.GetAO(0), res.Ssao, &ao, sizeof(ao));
        const Image& raw = target.GetAO(0);
        SsaoBlurPush blur{ target.GetAOBindless(0), target.GetDepthBindless(), glm::vec2(1.0f / raw.GetWidth(), 1.0f / raw.GetHeight()) };
        pass(target.GetAO(1), res.SsaoBlur, &blur, sizeof(blur));
        Vk::TransitionImage(cmd, depthImage, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                            VK_IMAGE_ASPECT_DEPTH_BIT);
    }

    void SceneRenderer::UpdateReflectionProbes(VkCommandBuffer cmd, Scene& scene)
    {
        m_FrameProbes.clear();
        std::unordered_set<uint64_t> seen;
        for (auto [e, probe, t] : scene.GetRegistry().ViewOf<ReflectionProbeComponent, TransformComponent>())
        {
            if (!t.ActiveInHierarchy || m_FrameProbes.size() >= kMaxReflectionProbes) continue;
            uint64_t id = ((uint64_t)e.Generation << 32) | e.Index;
            seen.insert(id);
            auto& gpu = m_Probes[id];
            if (!gpu) gpu = std::make_unique<ProbeResources>();
            uint32_t resolution = 64u << std::clamp(probe.Resolution, 0, 3);
            glm::vec3 position = t.GetWorldPosition();
            // Re-capture when the probe or its scene changes (play mode works on a copy of the scene).
            struct { glm::vec3 P; uint32_t R; const void* S; uint32_t Requests; } keyData{ position, resolution, &scene, probe.RenderRequests };
            uint64_t key = 1469598103934665603ull;
            const auto* bytes = reinterpret_cast<const uint8_t*>(&keyData);
            for (size_t i = 0; i < sizeof(keyData); ++i) key = (key ^ bytes[i]) * 1099511628211ull;
            if (!gpu->Baked || gpu->Key != key || probe.Realtime || gpu->Generation != s_ProbeBakeGeneration)
            {
                BakeProbe(cmd, scene, *gpu, position, resolution);
                gpu->Key = key;
                gpu->Generation = s_ProbeBakeGeneration;
                gpu->Baked = true;
            }
            glm::vec3 half = glm::abs(probe.Size) * 0.5f;
            m_FrameProbes.push_back({ position, position - half, position + half, probe.Intensity, gpu->Bindless,
                                      gpu->Filtered.GetMipLevels(), probe.BoxProjection, std::max(probe.BlendDistance, 0.0f) });
        }
        std::erase_if(m_Probes, [&](const auto& entry) { return !seen.contains(entry.first); });
        // Smallest boxes first: they take priority where probes overlap (the shader hands leftover weight onward).
        std::stable_sort(m_FrameProbes.begin(), m_FrameProbes.end(), [](const FrameProbe& a, const FrameProbe& b) {
            glm::vec3 sa = a.Max - a.Min, sb = b.Max - b.Min;
            return sa.x * sa.y * sa.z < sb.x * sb.y * sb.z;
        });
    }

    void SceneRenderer::UpdateLightProbes(VkCommandBuffer cmd, Scene& scene)
    {
        auto& device = RenderDevice::Get();
        auto& reg = scene.GetRegistry();
        // Captures whose frame has finished: their equirect images become SH coefficients in the component.
        uint64_t completed = device.GetCompletedFrameNumber();
        for (auto& rb : m_LightProbeReadbacks)
        {
            if (!rb.Busy || rb.Frame > completed) continue;
            rb.Busy = false;
            Entity e{ rb.EntityIndex, rb.EntityGeneration };
            auto* lpv = rb.Owner == &scene && scene.IsValid(e) ? reg.TryGet<LightProbeVolumeComponent>(e) : nullptr;
            if (!lpv) continue;
            lpv->BakePending = std::max(lpv->BakePending - 1, 0);
            if ((int)lpv->Baked.size() != lpv->ProbeCount() * 9) lpv->Baked.assign((size_t)lpv->ProbeCount() * 9, glm::vec3(0.0f));
            if (rb.Probe >= lpv->ProbeCount()) continue;
            const auto* half = static_cast<const uint16_t*>(rb.Data.GetMapped());
            size_t texels = (size_t)rb.Width * rb.Height;
            std::vector<float> rgba(texels * 4);
            for (size_t i = 0; i < texels * 4; ++i) rgba[i] = glm::unpackHalf1x16(half[i]);
            Environment::SH9 sh = Environment::ComputeIrradianceSH(rgba.data(), rb.Width, rb.Height);
            for (int k = 0; k < 9; ++k) lpv->Baked[(size_t)rb.Probe * 9 + k] = sh[k];
        }
        // Start the next captures (one per frame, so baking never stalls the editor; a realtime volume takes Probes
        // Per Frame and starts over when done): the six-face capture of the reflection probes at the probe's
        // position, its equirect image copied back to the CPU.
        bool baking = false; // one bake capture per frame across all volumes
        for (auto [e, lpv, t] : reg.ViewOf<LightProbeVolumeComponent, TransformComponent>())
        {
            if (!t.ActiveInHierarchy) continue;
            if (!lpv.IsBaked() && lpv.BakeNext < 0 && lpv.BakePending == 0) lpv.BakeNext = 0; // unbaked: bake now
            if (lpv.Realtime && lpv.BakeNext < 0) lpv.BakeNext = 0;
            int count = lpv.Realtime && lpv.IsBaked() ? std::clamp(lpv.ProbesPerFrame, 1, 4) : baking ? 0 : 1;
            for (int capture = 0; capture < count; ++capture)
            {
                if (lpv.BakeNext < 0) break;
                if (lpv.BakeNext >= lpv.ProbeCount())
                {
                    lpv.BakeNext = lpv.Realtime ? 0 : -1;
                    if (!lpv.Realtime) break;
                }
                Readback* slot = nullptr;
                for (auto& rb : m_LightProbeReadbacks)
                    if (!rb.Busy) slot = &rb;
                if (!slot)
                {
                    if (m_LightProbeReadbacks.size() >= 12) break; // all in flight: continue next frame
                    slot = &m_LightProbeReadbacks.emplace_back();
                }
                const uint32_t res = 32, width = res * 4, height = res * 2;
                if (!m_LightProbeCapture) m_LightProbeCapture = std::make_unique<ProbeResources>();
                BakeProbe(cmd, scene, *m_LightProbeCapture, lpv.ProbePosition(t.GetWorldPosition(), lpv.BakeNext), res);
                if (!slot->Data.IsValid())
                    slot->Data = Buffer((VkDeviceSize)width * height * 8, VK_BUFFER_USAGE_TRANSFER_DST_BIT, MemoryUsage::GpuToCpu, "LightProbeReadback");
                VkBufferImageCopy region{};
                region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
                region.imageExtent = { width, height, 1 };
                vkCmdCopyImageToBuffer(cmd, m_LightProbeCapture->Base.GetHandle(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, slot->Data.GetHandle(), 1, &region);
                slot->Busy = true;
                slot->Frame = device.Frame().FrameNumber;
                slot->Owner = &scene;
                slot->EntityIndex = e.Index;
                slot->EntityGeneration = e.Generation;
                slot->Probe = lpv.BakeNext;
                slot->Width = width;
                slot->Height = height;
                ++lpv.BakeNext;
                ++lpv.BakePending;
                if (!lpv.Realtime || !lpv.IsBaked()) baking = true;
            }
        }
    }

    void SceneRenderer::BakeProbe(VkCommandBuffer cmd, Scene& scene, ProbeResources& gpu, const glm::vec3& position, uint32_t res)
    {
        auto& device = RenderDevice::Get();
        auto& renderer = Renderer::Get();
        using F = SceneRenderTarget;
        uint32_t width = res * 4, height = res * 2;
        uint32_t mips = (uint32_t)std::floor(std::log2((float)width)) + 1;
        if (gpu.Resolution != res)
        {
            for (int f = 0; f < 6; ++f)
            {
                if (gpu.FaceBindless[f]) device.UnregisterTexture(gpu.FaceBindless[f]);
                gpu.Faces[f] = Image({ res, res, F::kHdrFormat, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, 1, "ProbeFace" });
                gpu.FaceBindless[f] = device.RegisterTexture(gpu.Faces[f].GetView(), device.GetClampSampler());
            }
            if (gpu.Bindless) device.UnregisterTexture(gpu.Bindless);
            if (gpu.SourceBindless) device.UnregisterTexture(gpu.SourceBindless);
            gpu.Base = Image({ width, height, F::kHdrFormat, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, 1, "ProbeBase" });
            gpu.Equirect = Image({ width, height, F::kHdrFormat,
                                   VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, mips, "ProbeEquirect" });
            gpu.Filtered = Image({ width, height, F::kHdrFormat,
                                   VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                                   FilteredMipCount(width), "ProbeFiltered" });
            gpu.SourceBindless = device.RegisterTexture(gpu.Equirect.GetView(), device.GetLinearSampler());
            gpu.Bindless = device.RegisterTexture(gpu.Filtered.GetView(), device.GetLinearSampler());
            gpu.Resolution = res;
        }
        if (!m_ProbeTarget) m_ProbeTarget = MakeScope<SceneRenderTarget>(res, res);
        m_ProbeTarget->Resize(res, res);

        // Six 90 degree views (same frames as ProbeEquirect.frag).
        static const glm::vec3 forward[6] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
        static const glm::vec3 up[6] = { { 0, 1, 0 }, { 0, 1, 0 }, { 0, 0, 1 }, { 0, 0, 1 }, { 0, 1, 0 }, { 0, 1, 0 } };
        RenderStats outer = m_Stats;
        SceneRenderOptions capture;
        capture.ProbeCapture = true;
        for (int f = 0; f < 6; ++f)
        {
            CameraData cam;
            cam.View = glm::lookAt(position, position + forward[f], up[f]);
            cam.Projection = glm::perspective(glm::radians(90.0f), 1.0f, 0.05f, 500.0f);
            cam.Position = position;
            cam.Skybox = true;
            Render(cmd, scene, cam, *m_ProbeTarget, capture);
            VkImage face = gpu.Faces[f].GetHandle();
            Vk::TransitionImage(cmd, face, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            VkImageCopy copy{};
            copy.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            copy.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            copy.extent = { res, res, 1 };
            vkCmdCopyImage(cmd, m_ProbeTarget->GetHdr().GetHandle(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, face, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
            Vk::TransitionImage(cmd, face, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            Vk::TransitionImage(cmd, m_ProbeTarget->GetHdr().GetHandle(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
        m_Stats = outer;

        // Faces -> equirectangular image (mip 0), then a blurred mip chain for rough surfaces.
        VkImage base = gpu.Base.GetHandle(), equirect = gpu.Equirect.GetHandle();
        Vk::TransitionImage(cmd, base, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        VkRenderingAttachmentInfo att{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
        att.imageView = gpu.Base.GetView();
        att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        att.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingInfo info{ VK_STRUCTURE_TYPE_RENDERING_INFO };
        info.renderArea = { { 0, 0 }, { width, height } };
        info.layerCount = 1;
        info.colorAttachmentCount = 1;
        info.pColorAttachments = &att;
        vkCmdBeginRendering(cmd, &info);
        VkViewport viewport{ 0.0f, 0.0f, (float)width, (float)height, 0.0f, 1.0f };
        VkRect2D scissor{ { 0, 0 }, { width, height } };
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &scissor);
        device.BindBindless(cmd);
        renderer.ProbeEquirect.Bind(cmd);
        vkCmdPushConstants(cmd, device.GetPipelineLayout(), VK_SHADER_STAGE_ALL, 0, sizeof(gpu.FaceBindless), gpu.FaceBindless);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRendering(cmd);
        Vk::TransitionImage(cmd, base, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

        Vk::TransitionImage(cmd, equirect, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkImageCopy copy{};
        copy.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        copy.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        copy.extent = { width, height, 1 };
        vkCmdCopyImage(cmd, base, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, equirect, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        int32_t w = (int32_t)width, h = (int32_t)height;
        for (uint32_t mip = 1; mip < mips; ++mip)
        {
            Vk::TransitionImage(cmd, equirect, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT, mip - 1, 1);
            int32_t nw = std::max(1, w / 2), nh = std::max(1, h / 2);
            VkImageBlit blit{};
            blit.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, mip - 1, 0, 1 };
            blit.srcOffsets[1] = { w, h, 1 };
            blit.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, mip, 0, 1 };
            blit.dstOffsets[1] = { nw, nh, 1 };
            vkCmdBlitImage(cmd, equirect, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, equirect, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
            Vk::TransitionImage(cmd, equirect, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT, mip - 1, 1);
            w = nw;
            h = nh;
        }
        Vk::TransitionImage(cmd, equirect, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT, mips - 1, 1);

        // GGX prefilter: mip 0 is the sharp capture (copied); mip m holds the environment blurred for roughness
        // m / (mips - 1), the lod meshes sample for that roughness.
        VkImage filtered = gpu.Filtered.GetHandle();
        Vk::TransitionImage(cmd, filtered, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1);
        vkCmdCopyImage(cmd, base, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, filtered, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        Vk::TransitionImage(cmd, filtered, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1);
        PrefilterEquirectMips(cmd, gpu.Filtered, 1, gpu.SourceBindless, mips, 0.0f);
    }

    void SceneRenderer::RenderDepthOfField(VkCommandBuffer cmd, SceneRenderTarget& target, const CameraData& camera, const SceneSettings& settings)
    {
        auto& device = RenderDevice::Get();
        auto& res = Renderer::Get();
        auto pass = [&](const Image& dst, const Pipeline& pipeline, const void* push, uint32_t size) {
            Vk::TransitionImage(cmd, dst.GetHandle(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
            VkRenderingAttachmentInfo att{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
            att.imageView = dst.GetView();
            att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            att.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
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
            vkCmdPushConstants(cmd, device.GetPipelineLayout(), VK_SHADER_STAGE_ALL, 0, size, push);
            vkCmdDraw(cmd, 3, 1, 0, 0);
            vkCmdEndRendering(cmd);
            Vk::TransitionImage(cmd, dst.GetHandle(), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            m_Stats.DrawCalls++;
        };
        DofPrefilterPush pre{ target.GetHdrBindless(), target.GetDepthBindless(), camera.Projection[2][2], camera.Projection[3][2],
                              std::max(settings.FocusDistance, 0.01f), std::max(settings.FocusRange, 0.01f),
                              glm::vec2(1.0f / target.GetWidth(), 1.0f / target.GetHeight()) };
        pass(target.GetDof(0), res.DofPrefilter, &pre, sizeof(pre));
        const Image& half = target.GetDof(0);
        DofBlurPush blur{ target.GetDofBindless(0), 32u, glm::vec2(1.0f / half.GetWidth(), 1.0f / half.GetHeight()),
                          std::clamp(settings.BokehSize, 0.0f, 0.1f) * (float)half.GetHeight() };
        pass(target.GetDof(1), res.DofBlur, &blur, sizeof(blur));
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

    float SceneRenderer::BuildGrid(const CameraData& camera, const GridSettings& settings, std::vector<LineVertex>& out)
    {
        // Grid spacing adapts to zoom so lines stay readable (unless the cell size is set).
        bool ortho = camera.Orthographic;
        float halfHeight = 1.0f / camera.Projection[1][1];
        for (const GridLine& l : SceneGrid::Build(settings, camera.Position, ortho, halfHeight))
        {
            out.push_back({ l.A, l.Color });
            out.push_back({ l.B, l.Color });
        }
        return SceneGrid::Spacing(settings, camera.Position, ortho, halfHeight);
    }

    namespace
    {
        // Navigation area colors (Walkable, Not Walkable, Road, Grass, Mud, Water, Area 6, Area 7).
        const glm::vec4 kAreaColors[8] = { { 0.35f, 0.7f, 1.0f, 0.3f }, { 1.0f, 0.3f, 0.3f, 0.4f }, { 0.75f, 0.75f, 0.8f, 0.5f }, { 0.4f, 0.9f, 0.35f, 0.45f },
                                           { 0.7f, 0.45f, 0.2f, 0.55f }, { 0.2f, 0.85f, 0.95f, 0.45f }, { 0.9f, 0.5f, 0.9f, 0.45f }, { 0.95f, 0.85f, 0.3f, 0.45f } };
    }

    // A baked navigation mesh: links between its spots (colored by area), its edges brighter, the bake's own links.
    void SceneRenderer::DrawNavMeshOverlay(const NavMeshData& m, const glm::mat4& frame)
    {
        // Moving surfaces: from where the mesh was baked to where its object is now.
        auto DrawLine = [&](const glm::vec3& a, const glm::vec3& b, const glm::vec4& color) {
            this->DrawLine(glm::vec3(frame * glm::vec4(a, 1.0f)), glm::vec3(frame * glm::vec4(b, 1.0f)), color);
        };
        const glm::vec4 edge(0.45f, 0.85f, 1.0f, 0.95f);
        const glm::vec3 lift(0.0f, 0.04f, 0.0f);
        int n = m.NodeCount();
        bool interior = n <= 40000; // big meshes: only their edges
        float h = m.CellSize * 0.5f;
        for (int a = 0; a < n; ++a)
        {
            if (m.IsBlocked(a)) continue; // carved out: the edges around the hole show it
            glm::vec3 p = m.NodePosition(a) + lift;
            for (int d = 0; d < 4; ++d)
            {
                int b = m.Links[(size_t)a * 8 + d];
                if (b >= 0)
                {
                    if (interior && d < 2) DrawLine(p, m.NodePosition(b) + lift, kAreaColors[m.Areas.empty() ? 0 : m.Areas[a] & 7]);
                    continue;
                }
                glm::vec3 out(Navigation::kDirX[d] * h, 0.0f, Navigation::kDirZ[d] * h), side(Navigation::kDirZ[d] * h, 0.0f, Navigation::kDirX[d] * h);
                DrawLine(p + out - side, p + out + side, edge);
            }
        }
        // Generated links: small arcs, an arrow head where they land.
        for (const NavMeshData::AutoLink& l : m.AutoLinks)
        {
            if (m.IsBlocked(l.From) || m.IsBlocked(l.To)) continue;
            glm::vec3 a = m.NodePosition(l.From) + lift, b = m.NodePosition(l.To) + lift, prev = a;
            const glm::vec4 color(1.0f, 0.7f, 0.3f, 0.9f);
            for (int i = 1; i <= 8; ++i)
            {
                float s = i / 8.0f;
                glm::vec3 q = glm::mix(a, b, s) + glm::vec3(0.0f, l.Arc * 4.0f * s * (1.0f - s), 0.0f);
                DrawLine(prev, q, color);
                prev = q;
            }
            DrawLine(b, b + glm::vec3(0.0f, 0.15f, 0.0f), color);
        }
    }

    void SceneRenderer::BuildEditorOverlays(Scene& scene, const SceneRenderOptions& options)
    {
        // The Navigation window open: every baked surface.
        if (options.ShowNavMeshes)
            for (auto [e, s, t] : scene.GetRegistry().ViewOf<NavMeshSurfaceComponent, TransformComponent>())
                if (s.Data && t.ActiveInHierarchy) DrawNavMeshOverlay(*s.Data, s.Moving ? s.Frame : glm::mat4(1.0f));
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
        if (auto* line = reg.TryGet<LineRendererComponent>(sel))
        {
            // Point handles: a small sphere at each position, the first one larger.
            for (size_t i = 0; i < line->Positions.size(); ++i)
            {
                glm::vec3 p = line->UseWorldSpace ? line->Positions[i] : glm::vec3(t.World * glm::vec4(line->Positions[i], 1.0f));
                DrawWireSphere(glm::translate(glm::mat4(1.0f), p), glm::vec3(0), i == 0 ? 0.08f : 0.05f, orange);
            }
        }
        if (auto* mc = reg.TryGet<MeshColliderComponent>(sel); mc && mc->ConvexDecomposition)
        {
            // The convex parts, each in its own color.
            auto* mr = reg.TryGet<MeshRendererComponent>(sel);
            AssetHandle mesh = mc->Mesh.IsValid() ? mc->Mesh : mr ? mr->Mesh : AssetHandle::None();
            if (auto parts = ConvexDecomposition::Decompose(AssetManager::GetMeshData(mesh), mc->MaxParts))
                for (size_t i = 0; i < parts->size(); ++i)
                {
                    const ConvexPart& part = (*parts)[i];
                    float hue = std::fmod((float)i * 0.61803f, 1.0f);
                    glm::vec4 color(glm::clamp(glm::abs(glm::fract(glm::vec3(hue) + glm::vec3(1.0f, 2.0f / 3.0f, 1.0f / 3.0f)) * 6.0f - 3.0f) - 1.0f, 0.0f, 1.0f), 0.9f);
                    color = glm::vec4(glm::mix(glm::vec3(1.0f), glm::vec3(color), 0.7f), 0.9f);
                    auto world = [&](uint32_t k) { return glm::vec3(t.World * glm::vec4(part.Points[k], 1.0f)); };
                    for (size_t k = 0; k + 2 < part.Triangles.size(); k += 3)
                    {
                        uint32_t a = part.Triangles[k], b = part.Triangles[k + 1], c = part.Triangles[k + 2];
                        // Each edge is shared by two faces: draw it from the face that lists it in increasing order.
                        if (a < b) DrawLine(world(a), world(b), color);
                        if (b < c) DrawLine(world(b), world(c), color);
                        if (c < a) DrawLine(world(c), world(a), color);
                    }
                }
        }
        if (auto* vehicle = reg.TryGet<VehicleComponent>(sel))
        {
            // Each wheel at full suspension extension, with its travel from the mount (the body's scale does not apply).
            const glm::vec4 wheelGreen{ 0.45f, 1.0f, 0.5f, 1.0f };
            glm::mat4 body = glm::translate(glm::mat4(1.0f), t.GetWorldPosition()) * glm::toMat4(glm::quat_cast(glm::mat3(glm::normalize(glm::vec3(t.World[0])),
                                                                                                                         glm::normalize(glm::vec3(t.World[1])),
                                                                                                                         glm::normalize(glm::vec3(t.World[2])))));
            float hx = vehicle->TrackWidth * 0.5f, hz = vehicle->WheelBase * 0.5f, drop = 0.05f + vehicle->SuspensionTravel;
            std::vector<glm::vec3> mounts;
            if (vehicle->Type == VehicleType::Motorcycle)
                mounts = { { 0.0f, vehicle->WheelHeight, -hz }, { 0.0f, vehicle->WheelHeight, hz } };
            else if (vehicle->Type == VehicleType::Tank)
            {
                int perSide = std::clamp(vehicle->TrackWheels, 2, 16);
                for (float x : { -hx, hx })
                    for (int k = 0; k < perSide; ++k) mounts.push_back({ x, vehicle->WheelHeight, -hz + vehicle->WheelBase * (float)k / (float)(perSide - 1) });
            }
            else
                mounts = { { -hx, vehicle->WheelHeight, -hz }, { hx, vehicle->WheelHeight, -hz }, { -hx, vehicle->WheelHeight, hz }, { hx, vehicle->WheelHeight, hz } };
            for (glm::vec3 mount : mounts)
            {
                glm::vec3 center = mount - glm::vec3(0.0f, drop, 0.0f);
                for (float side : { -0.5f, 0.5f })
                    DrawWireCircle(body, center + glm::vec3(side * vehicle->WheelWidth, 0.0f, 0.0f), vehicle->WheelRadius, wheelGreen, 0);
                DrawLine(glm::vec3(body * glm::vec4(mount, 1.0f)), glm::vec3(body * glm::vec4(center, 1.0f)), wheelGreen);
            }
        }
        if (auto* probe = reg.TryGet<ReflectionProbeComponent>(sel))
        {
            glm::mat4 at = glm::translate(glm::mat4(1.0f), t.GetWorldPosition());
            DrawWireBox(at, glm::vec3(0.0f), glm::abs(probe->Size) * 0.5f, { 0.4f, 0.85f, 1.0f, 0.9f });
            DrawWireSphere(at, glm::vec3(0.0f), 0.25f, { 0.4f, 0.85f, 1.0f, 0.9f });
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
            if (joint->Type == JointType::Slider)
                DrawSliderGizmo(anchor, glm::mat3(t.World) * joint->Axis, joint->UseLimits, joint->SliderMin, joint->SliderMax);
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
            if (joint->Type == Joint2DType::Slider)
                DrawSliderGizmo(anchor, glm::vec3(t.World * glm::vec4(joint->Axis, 0.0f, 0.0f)), joint->UseLimits, joint->LowerTranslation,
                                joint->UpperTranslation);
        }
        if (auto* cc = reg.TryGet<CharacterControllerComponent>(sel))
        {
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
        if (auto* path = reg.TryGet<TerrainPathComponent>(sel))
        {
            // The curve, and its width on either side.
            std::vector<glm::vec3> curve = Terrain::PathCurve(path->Points);
            glm::vec4 color = path->Mode == TerrainPathMode::River ? glm::vec4(0.3f, 0.6f, 1.0f, 1.0f) : glm::vec4(0.95f, 0.75f, 0.4f, 1.0f);
            for (size_t k = 0; k + 1 < curve.size(); ++k)
            {
                glm::vec3 a = glm::vec3(t.World * glm::vec4(curve[k], 1.0f)), b = glm::vec3(t.World * glm::vec4(curve[k + 1], 1.0f));
                glm::vec3 side = glm::cross(b - a, glm::vec3(0, 1, 0));
                side = glm::dot(side, side) > 1e-12f ? glm::normalize(side) * (path->Width * 0.5f) : glm::vec3(0.0f);
                DrawLine(a, b, color);
                DrawLine(a + side, b + side, color * glm::vec4(1, 1, 1, 0.5f));
                DrawLine(a - side, b - side, color * glm::vec4(1, 1, 1, 0.5f));
            }
            for (const glm::vec3& p : path->Points) DrawWireCircle(t.World, p, 0.3f, color, 1);
        }
        if (auto* placer = reg.TryGet<PathPlacerComponent>(sel))
        {
            // The curve, and a tick where each copy goes (pointing the way it faces).
            std::vector<glm::vec3> points = placer->Points;
            if (placer->Closed && points.size() > 2) points.push_back(points.front());
            std::vector<glm::vec3> curve = Terrain::PathCurve(points);
            glm::vec4 color(0.5f, 0.95f, 0.6f, 1.0f);
            for (size_t k = 0; k + 1 < curve.size(); ++k)
                DrawLine(glm::vec3(t.World * glm::vec4(curve[k], 1.0f)), glm::vec3(t.World * glm::vec4(curve[k + 1], 1.0f)), color);
            for (const glm::vec3& p : placer->Points) DrawWireCircle(t.World, p, 0.3f, color, 1);
            for (const PathPlacer::Spot& s : PathPlacer::Spots(scene, sel))
            {
                DrawLine(s.Position, s.Position + glm::vec3(0.0f, 0.6f, 0.0f), color * glm::vec4(1, 1, 1, 0.7f));
                DrawLine(s.Position + glm::vec3(0.0f, 0.3f, 0.0f), s.Position + glm::vec3(0.0f, 0.3f, 0.0f) + s.Direction * 0.5f, color * glm::vec4(1, 1, 1, 0.7f));
            }
        }
        if (auto* wind = reg.TryGet<WindZoneComponent>(sel))
        {
            // Spherical: its reach; Directional: arrows the way it blows.
            glm::vec4 color(0.75f, 0.9f, 1.0f, 0.9f);
            glm::vec3 center = t.GetWorldPosition();
            if (wind->Mode == WindZoneMode::Spherical)
                DrawWireSphere(glm::translate(glm::mat4(1.0f), center), glm::vec3(0.0f), wind->Radius, color);
            else
            {
                glm::vec3 f = glm::normalize(-glm::vec3(t.World[2])), r = glm::normalize(glm::vec3(t.World[0])), u = glm::normalize(glm::vec3(t.World[1]));
                for (float s : { -0.6f, 0.0f, 0.6f })
                {
                    glm::vec3 a = center + r * s - f * 1.0f, b = center + r * s + f * 1.0f;
                    DrawLine(a, b, color);
                    DrawLine(b, b - f * 0.35f + r * 0.2f, color);
                    DrawLine(b, b - f * 0.35f - r * 0.2f, color);
                    DrawLine(b, b - f * 0.35f + u * 0.2f, color);
                }
            }
        }
        // Navigation: the baked mesh (links between its spots, its edges brighter), an agent's room and path.
        if (auto* volume = reg.TryGet<NavMeshModifierVolumeComponent>(sel))
        {
            glm::vec4 color = kAreaColors[(int)volume->Area & 7];
            color.a = 1.0f;
            DrawWireBox(t.World, volume->Center, volume->Size * 0.5f, color);
        }
        // Off-mesh links: an arc from start to end (arrow heads at the ends it can be crossed to).
        auto drawLink = [&](Entity e, const OffMeshLinkComponent& link) {
            const glm::mat4& w = reg.Get<TransformComponent>(e).World;
            glm::vec3 a = glm::vec3(w * glm::vec4(link.Start, 1.0f)), b = glm::vec3(w * glm::vec4(link.End, 1.0f));
            glm::vec4 color = link.Activated ? glm::vec4(1.0f, 0.55f, 0.2f, 1.0f) : glm::vec4(0.6f, 0.6f, 0.6f, 0.8f);
            if (link.Width > 0.0f)
            {
                // A wide link: its two edges across, and the arcs at its sides (the middle one is drawn below).
                auto crossings = Navigation::LinkCrossings(w, link, 1e6f);
                if (crossings.size() >= 2)
                {
                    const auto& [a0, b0] = crossings.front();
                    const auto& [a1, b1] = crossings.back();
                    DrawLine(a0, a1, color);
                    DrawLine(b0, b1, color);
                    for (const auto& [from, to] : { crossings.front(), crossings.back() })
                    {
                        glm::vec3 prev = from;
                        for (int i = 1; i <= 16; ++i)
                        {
                            float s = i / 16.0f;
                            glm::vec3 q = glm::mix(from, to, s) + glm::vec3(0.0f, std::max(link.ArcHeight, 0.0f) * 4.0f * s * (1.0f - s), 0.0f);
                            DrawLine(prev, q, color * glm::vec4(1.0f, 1.0f, 1.0f, 0.6f));
                            prev = q;
                        }
                    }
                }
            }
            glm::vec3 prev = a;
            for (int i = 1; i <= 16; ++i)
            {
                float s = i / 16.0f;
                glm::vec3 q = glm::mix(a, b, s) + glm::vec3(0.0f, std::max(link.ArcHeight, 0.0f) * 4.0f * s * (1.0f - s), 0.0f);
                DrawLine(prev, q, color);
                prev = q;
            }
            glm::mat4 at = glm::translate(glm::mat4(1.0f), a), bt = glm::translate(glm::mat4(1.0f), b);
            DrawWireCircle(at, glm::vec3(0.0f), 0.3f, color, 1);
            DrawWireCircle(bt, glm::vec3(0.0f), 0.3f, color, 1);
            glm::vec3 dir = b - a;
            dir.y = 0.0f;
            if (glm::length(dir) > 1e-4f)
            {
                dir = glm::normalize(dir) * 0.35f;
                glm::vec3 side(-dir.z, 0.0f, dir.x);
                DrawLine(b, b - dir + side, color);
                DrawLine(b, b - dir - side, color);
                if (link.Bidirectional)
                {
                    DrawLine(a, a + dir + side, color);
                    DrawLine(a, a + dir - side, color);
                }
            }
        };
        if (auto* link = reg.TryGet<OffMeshLinkComponent>(sel)) drawLink(sel, *link);
        if (auto* obstacle = reg.TryGet<NavMeshObstacleComponent>(sel))
        {
            const glm::vec4 color(0.95f, 0.45f, 0.35f, 1.0f);
            if (obstacle->Shape == NavObstacleShape::Box)
                DrawWireBox(t.World, obstacle->Center, obstacle->Size * 0.5f, color);
            else
                for (float y : { -0.5f, 0.0f, 0.5f })
                {
                    glm::vec3 c = obstacle->Center + glm::vec3(0.0f, y * obstacle->Height, 0.0f);
                    DrawWireCircle(t.World, c, obstacle->Radius, color, 1);
                }
        }
        if (auto* surface = reg.TryGet<NavMeshSurfaceComponent>(sel))
        {
            DrawWireBox(glm::translate(glm::mat4(1.0f), t.GetWorldPosition()), glm::vec3(0.0f), surface->Size * 0.5f, { 0.45f, 0.85f, 1.0f, 0.5f });
            if (surface->ShowNavMesh && surface->Data)
            {
                DrawNavMeshOverlay(*surface->Data, surface->Moving ? surface->Frame : glm::mat4(1.0f));
                for (auto [e, link] : reg.ViewOf<OffMeshLinkComponent>()) drawLink(e, link);
            }
        }
        if (auto* agent = reg.TryGet<NavMeshAgentComponent>(sel))
        {
            const glm::vec4 color(0.45f, 1.0f, 0.6f, 1.0f);
            glm::vec3 foot = t.GetWorldPosition() - glm::vec3(0.0f, agent->BaseOffset, 0.0f);
            glm::mat4 at = glm::translate(glm::mat4(1.0f), foot);
            DrawWireCircle(at, glm::vec3(0.0f), agent->Radius, color, 1);
            DrawWireCircle(at, glm::vec3(0.0f, agent->Height, 0.0f), agent->Radius, color, 1);
            for (glm::vec3 d : { glm::vec3(agent->Radius, 0, 0), glm::vec3(-agent->Radius, 0, 0), glm::vec3(0, 0, agent->Radius), glm::vec3(0, 0, -agent->Radius) })
                DrawLine(foot + d, foot + d + glm::vec3(0.0f, agent->Height, 0.0f), color * glm::vec4(1, 1, 1, 0.5f));
            glm::mat4 frame = Navigation::MeshFrame(scene, agent->Mesh.get()); // its path is in its mesh's space
            auto onMesh = [&](const glm::vec3& p) { return glm::vec3(frame * glm::vec4(p, 1.0f)); };
            if (agent->HasDestination && agent->Corner < agent->Path.size())
            {
                const glm::vec4 path(1.0f, 0.85f, 0.3f, 1.0f);
                const glm::vec3 lift(0.0f, 0.08f, 0.0f);
                DrawLine(foot + lift, onMesh(agent->Path[agent->Corner]) + lift, path);
                for (size_t k = agent->Corner; k + 1 < agent->Path.size(); ++k) DrawLine(onMesh(agent->Path[k]) + lift, onMesh(agent->Path[k + 1]) + lift, path);
                DrawWireCircle(glm::translate(glm::mat4(1.0f), onMesh(agent->Path.back()) + lift), glm::vec3(0.0f), 0.25f, path, 1);
            }
            if (agent->Mesh && !reg.Has<NavMeshSurfaceComponent>(sel)) DrawNavMeshOverlay(*agent->Mesh, frame);
        }
        if (reg.Has<LODGroupComponent>(sel))
        {
            // The size the group measures against the screen, as a sphere around its children.
            glm::vec3 center;
            float size = LODGroups::WorldSize(scene, sel, &center);
            DrawWireSphere(glm::translate(glm::mat4(1.0f), center), glm::vec3(0.0f), size * 0.5f, { 1.0f, 0.75f, 0.35f, 0.6f });
        }
        if (auto* water = reg.TryGet<WaterVolumeComponent>(sel))
        {
            glm::mat4 at = glm::translate(glm::mat4(1.0f), t.GetWorldPosition()) * glm::scale(glm::mat4(1.0f), glm::abs(t.Scale));
            DrawWireBox(at, glm::vec3(0.0f), water->Size * 0.5f, { 0.3f, 0.6f, 1.0f, 0.9f });
        }
        if (auto* ragdoll = reg.TryGet<RagdollComponent>(sel))
            for (const auto& c : ragdoll->Capsules) // while playing: the capsules where they are
            {
                for (float y : { -c.HalfHeight, c.HalfHeight })
                    for (int axis = 0; axis < 3; ++axis) DrawWireCircle(c.World, { 0, y, 0 }, c.Radius, { 1.0f, 0.7f, 0.3f, 1.0f }, axis);
                for (glm::vec3 d : { glm::vec3(c.Radius, 0, 0), glm::vec3(-c.Radius, 0, 0), glm::vec3(0, 0, c.Radius), glm::vec3(0, 0, -c.Radius) })
                    DrawLine(glm::vec3(c.World * glm::vec4(d - glm::vec3(0, c.HalfHeight, 0), 1)), glm::vec3(c.World * glm::vec4(d + glm::vec3(0, c.HalfHeight, 0), 1)),
                             { 1.0f, 0.7f, 0.3f, 1.0f });
            }
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
        if (auto* zone = reg.TryGet<AudioReverbZoneComponent>(sel))
        {
            glm::mat4 m = glm::translate(glm::mat4(1.0f), t.GetWorldPosition());
            DrawWireSphere(m, glm::vec3(0), zone->MinDistance, { 1.0f, 0.75f, 0.3f, 0.85f });
            DrawWireSphere(m, glm::vec3(0), zone->MaxDistance, { 1.0f, 0.6f, 0.2f, 0.45f });
        }
        if (auto* lpv = reg.TryGet<LightProbeVolumeComponent>(sel))
        {
            // The box, and each probe as a small circle tinted by its baked light (gray before baking).
            glm::vec3 center = t.GetWorldPosition();
            DrawWireBox(glm::mat4(1.0f), center, glm::abs(lpv->Size) * 0.5f, { 0.95f, 0.85f, 0.4f, 0.8f });
            for (int i = 0; i < lpv->ProbeCount(); ++i)
            {
                glm::vec3 color(0.6f);
                if (lpv->IsBaked()) color = glm::clamp(lpv->Baked[(size_t)i * 9] * 0.282095f * 1.5f, glm::vec3(0.05f), glm::vec3(1.0f));
                DrawWireSphere(glm::mat4(1.0f), lpv->ProbePosition(center, i), 0.12f, glm::vec4(color, 1.0f));
            }
        }
    }
}
