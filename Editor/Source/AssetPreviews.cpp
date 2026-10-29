#include "AssetPreviews.h"
#include <Dimension/Asset/AssetManager.h>
#include <Dimension/ImGui/ImGuiLayer.h>
#include <Dimension/Renderer/Material.h>
#include <Dimension/Renderer/Mesh.h>
#include <Dimension/Renderer/RenderDevice.h>
#include <Dimension/Scene/Components.h>
#include <Dimension/Scene/Prefab.h>
#include <glm/gtc/matrix_transform.hpp>

namespace Editor
{
    AssetPreviews::AssetPreviews()
    {
        // A sun and a camera; the subject is built for each picture.
        m_Scene = MakeRef<Scene>("Asset Preview");
        auto& settings = m_Scene->GetSettings();
        settings.TAA = settings.Bloom = settings.SSAO = settings.SSR = settings.SSGI = settings.Fog = false;
        settings.OcclusionCulling = false;
        settings.Tonemapping = true;
        settings.AmbientIntensity = 0.45f;
        auto& reg = m_Scene->GetRegistry();
        Entity sun = m_Scene->CreateEntity("Sun");
        reg.Get<TransformComponent>(sun).Rotation = { glm::radians(-40.0f), glm::radians(-35.0f), 0.0f };
        auto& light = reg.Emplace<DirectionalLightComponent>(sun);
        light.Intensity = 2.2f;
        light.CastShadows = false;
        m_Camera = m_Scene->CreateEntity("Camera");
        auto& camera = reg.Emplace<CameraComponent>(m_Camera);
        camera.FieldOfView = 30.0f;
        camera.Clear = ClearMode::Skybox;
        m_Target = MakeScope<SceneRenderTarget>(kSize, kSize);
    }

    AssetPreviews::~AssetPreviews()
    {
        for (auto& [handle, entry] : m_Entries)
            if (entry.ImGuiId) RenderDevice::Get().DeferRelease([id = entry.ImGuiId] { ImGuiLayer::RemoveTexture(id); });
        if (m_LiveEntry.ImGuiId) RenderDevice::Get().DeferRelease([id = m_LiveEntry.ImGuiId] { ImGuiLayer::RemoveTexture(id); });
    }

    uint64_t AssetPreviews::KeyOf(AssetHandle asset)
    {
        uint64_t h = 1469598103934665603ull;
        auto mix = [&](const void* p, size_t n) {
            const auto* b = static_cast<const uint8_t*>(p);
            for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 1099511628211ull;
        };
        const AssetMetadata* meta = AssetManager::GetMetadata(asset);
        if (!meta) return 0;
        if (meta->Type != AssetType::Material)
        {
            // Models and prefabs: drawn again when their file changes.
            auto stamp = meta->LastWrite.time_since_epoch().count();
            mix(&stamp, sizeof(stamp));
            return h;
        }
        Ref<Material> material = AssetManager::GetMaterial(asset);
        if (!material) return 0;
        const Material& m = *material;
        float values[] = { m.BaseColor.r, m.BaseColor.g, m.BaseColor.b, m.BaseColor.a, m.NormalStrength, m.Metallic, m.Roughness, m.EmissionColor.r,
                           m.EmissionColor.g, m.EmissionColor.b, m.EmissionStrength, m.Tiling.x, m.Tiling.y, m.Offset.x, m.Offset.y, m.AlphaCutoff, m.Refraction };
        mix(values, sizeof(values));
        int32_t flags[] = { (int32_t)m.BlendMode, m.DoubleSided ? 1 : 0 };
        mix(flags, sizeof(flags));
        for (AssetHandle t : { m.BaseColorMap, m.NormalMap, m.MetallicRoughnessMap, m.EmissionMap })
        {
            uint64_t id = (uint64_t)t;
            mix(&id, sizeof(id));
            uint8_t loaded = AssetManager::GetTexture(t) ? 1 : 0; // drawn again once a texture has loaded
            mix(&loaded, 1);
        }
        return h;
    }

    uint64_t AssetPreviews::Get(AssetHandle asset)
    {
        const AssetMetadata* meta = AssetManager::GetMetadata(asset);
        if (!meta || !HasPreview(meta->Type)) return 0;
        uint64_t key = KeyOf(asset);
        auto it = m_Entries.find((uint64_t)asset);
        bool current = it != m_Entries.end() && it->second.Drawn && it->second.Key == key;
        if (!current && m_Queued.insert((uint64_t)asset).second) m_Queue.push_back(asset);
        return it != m_Entries.end() && it->second.Drawn ? it->second.ImGuiId : 0;
    }

    uint64_t AssetPreviews::Live(AssetHandle asset, const LiveView& view)
    {
        const AssetMetadata* meta = AssetManager::GetMetadata(asset);
        if (!meta || (meta->Type != AssetType::Mesh && meta->Type != AssetType::Prefab)) return 0;
        if (asset != m_LiveAsset)
        {
            m_LiveAsset = asset;
            m_LiveEntry.Drawn = false;
            m_LiveClips.clear();
        }
        m_LiveRequest = view;
        return m_LiveEntry.Drawn ? m_LiveEntry.ImGuiId : 0;
    }

    bool AssetPreviews::Stage(AssetHandle asset, const LiveView* view)
    {
        auto& reg = m_Scene->GetRegistry();
        if (!m_Subject.IsNull() && m_Scene->IsValid(m_Subject)) m_Scene->DestroyEntity(m_Subject);
        m_Subject = Entity::Null();
        const AssetMetadata* meta = AssetManager::GetMetadata(asset);
        if (!meta) return false;
        glm::vec3 lo(1e30f), hi(-1e30f);
        auto grow = [&](const glm::mat4& world, const AABB& box) {
            for (int c = 0; c < 8; ++c)
            {
                glm::vec3 p((c & 1) ? box.Max.x : box.Min.x, (c & 2) ? box.Max.y : box.Min.y, (c & 4) ? box.Max.z : box.Min.z);
                glm::vec3 w = glm::vec3(world * glm::vec4(p, 1.0f));
                lo = glm::min(lo, w);
                hi = glm::max(hi, w);
            }
        };
        float pitch = 0.0f, yaw = 0.0f;
        if (meta->Type == AssetType::Material)
        {
            if (!AssetManager::GetMaterial(asset)) return false;
            m_Subject = m_Scene->CreateEntity("Sphere");
            auto& mr = reg.Emplace<MeshRendererComponent>(m_Subject);
            mr.Mesh = AssetManager::kSphere;
            mr.Material = asset;
            lo = glm::vec3(-0.5f);
            hi = glm::vec3(0.5f);
        }
        else if (meta->Type == AssetType::Mesh)
        {
            Ref<Mesh> mesh = AssetManager::GetMesh(asset);
            if (!mesh) return false;
            m_Subject = m_Scene->CreateEntity("Model");
            reg.Emplace<MeshRendererComponent>(m_Subject).Mesh = asset; // its own materials
            grow(glm::mat4(1.0f), mesh->GetBounds());
            pitch = -22.0f;
            yaw = 35.0f;
        }
        else if (meta->Type == AssetType::Prefab)
        {
            m_Subject = Prefabs::Instantiate(*m_Scene, asset);
            if (m_Subject.IsNull()) return false;
            m_Scene->UpdateTransforms();
            for (auto [e, mr, t] : reg.ViewOf<MeshRendererComponent, TransformComponent>())
                if (Ref<Mesh> mesh = AssetManager::GetMesh(mr.Mesh)) grow(t.World, mesh->GetBounds());
            for (auto [e, sr, t] : reg.ViewOf<SpriteRendererComponent, TransformComponent>()) grow(t.World, AABB{ glm::vec3(-0.5f, -0.5f, 0.0f), glm::vec3(0.5f, 0.5f, 0.0f) });
            if (lo.x > hi.x) // nothing visible: still a picture, of the empty space where it stands
            {
                lo = glm::vec3(-0.5f);
                hi = glm::vec3(0.5f);
            }
            pitch = -22.0f;
            yaw = 35.0f;
        }
        else
            return false;
        if (view && meta->Type != AssetType::Material)
        {
            pitch = view->Pitch;
            yaw = view->Yaw;
            // The clips it can play: the model's own, or those of the prefab's animated models.
            m_LiveClips.clear();
            auto addClips = [&](const AnimatorComponent* anim, const MeshRendererComponent& mr) {
                Ref<Mesh> source = AssetManager::GetMesh(anim && anim->ClipSource.IsValid() ? anim->ClipSource : mr.Mesh);
                Ref<Mesh> mesh = AssetManager::GetMesh(mr.Mesh);
                if (!source || !mesh || !mesh->GetSkeleton()) return;
                for (const auto& c : source->GetClips())
                    if (std::none_of(m_LiveClips.begin(), m_LiveClips.end(), [&](const ClipInfo& o) { return o.Name == c.Name; }))
                        m_LiveClips.push_back({ c.Name, c.Duration });
            };
            if (meta->Type == AssetType::Mesh)
            {
                addClips(nullptr, reg.Get<MeshRendererComponent>(m_Subject));
                if (!view->Clip.empty())
                {
                    auto& anim = reg.Emplace<AnimatorComponent>(m_Subject);
                    anim.Clip = view->Clip;
                    anim.Time = view->Time;
                }
            }
            else
                for (auto [e, anim, mr] : reg.ViewOf<AnimatorComponent, MeshRendererComponent>())
                {
                    addClips(&anim, mr);
                    if (!view->Clip.empty())
                    {
                        anim.Controller = AssetHandle::None(); // the chosen clip instead of its state machine
                        anim.Clip = view->Clip;
                    }
                    anim.Time = view->Time;
                }
            m_Scene->UpdateAnimators(0.0f); // the pose at that time
        }
        // The camera looks at the middle of the bounds from far enough to fit their sphere.
        glm::vec3 center = (lo + hi) * 0.5f;
        float radius = std::max(glm::length(hi - lo) * 0.5f, 0.01f);
        auto& t = reg.Get<TransformComponent>(m_Camera);
        t.Rotation = { glm::radians(pitch), glm::radians(yaw), 0.0f };
        t.Position = glm::vec3(0.0f);
        m_Scene->UpdateTransforms();
        glm::vec3 forward = -glm::normalize(glm::vec3(t.World[2]));
        float distance = meta->Type == AssetType::Material ? 2.35f : radius / std::sin(glm::radians(15.0f)) * 1.05f;
        if (view) distance *= std::clamp(view->Zoom, 0.2f, 5.0f);
        t.Position = center - forward * distance;
        auto& camera = reg.Get<CameraComponent>(m_Camera);
        camera.NearClip = std::max(distance - radius * 2.0f, distance * 0.01f);
        camera.FarClip = distance + radius * 2.0f + 1.0f;
        m_Scene->UpdateTransforms();
        return true;
    }

    void AssetPreviews::Render(VkCommandBuffer cmd)
    {
        // The live preview first (asked for this frame), then a couple of the waiting pictures.
        if (m_LiveRequest)
        {
            if (Stage(m_LiveAsset, &*m_LiveRequest)) Draw(cmd, m_LiveEntry, m_LiveAsset);
            m_LiveRequest.reset();
        }
        int drawn = 0;
        while (!m_Queue.empty() && drawn < 2)
        {
            AssetHandle asset = m_Queue.front();
            m_Queue.erase(m_Queue.begin());
            m_Queued.erase((uint64_t)asset);
            if (!Stage(asset)) continue;
            Draw(cmd, m_Entries[(uint64_t)asset], asset);
            ++drawn;
        }
    }

    void AssetPreviews::Draw(VkCommandBuffer cmd, Entry& entry, AssetHandle asset)
    {
        {
            CameraData camera = CameraData::FromEntity(*m_Scene, m_Camera, 1.0f);
            SceneRenderOptions options;
            m_Renderer.Render(cmd, *m_Scene, camera, *m_Target, options);

            VkImageLayout from = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            if (!entry.Picture.IsValid())
            {
                entry.Picture = Image(ImageDesc{ kSize, kSize, SceneRenderTarget::kOutputFormat,
                                                 VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, 1, "AssetPreview" });
                entry.ImGuiId = ImGuiLayer::AddTexture(RenderDevice::Get().GetClampSampler(), entry.Picture.GetView());
                from = VK_IMAGE_LAYOUT_UNDEFINED;
            }
            Vk::TransitionImage(cmd, entry.Picture.GetHandle(), from, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            m_Target->BlitOutputTo(cmd, entry.Picture.GetHandle(), { kSize, kSize });
            Vk::TransitionImage(cmd, entry.Picture.GetHandle(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            entry.Key = KeyOf(asset);
            entry.Drawn = true;
        }
    }
}
