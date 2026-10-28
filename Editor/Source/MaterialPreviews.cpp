#include "MaterialPreviews.h"
#include <Dimension/Asset/AssetManager.h>
#include <Dimension/ImGui/ImGuiLayer.h>
#include <Dimension/Renderer/Material.h>
#include <Dimension/Renderer/RenderDevice.h>
#include <Dimension/Scene/Components.h>

namespace Editor
{
    MaterialPreviews::MaterialPreviews()
    {
        // A sphere under a sun, seen from the front, against the default sky.
        m_Scene = MakeRef<Scene>("Material Preview");
        auto& settings = m_Scene->GetSettings();
        settings.TAA = settings.Bloom = settings.SSAO = settings.SSR = settings.SSGI = settings.Fog = false;
        settings.OcclusionCulling = false;
        settings.Tonemapping = true;
        settings.AmbientIntensity = 0.45f;
        auto& reg = m_Scene->GetRegistry();
        m_Sphere = m_Scene->CreateEntity("Sphere");
        reg.Emplace<MeshRendererComponent>(m_Sphere).Mesh = AssetManager::kSphere;
        Entity sun = m_Scene->CreateEntity("Sun");
        reg.Get<TransformComponent>(sun).Rotation = { glm::radians(-40.0f), glm::radians(-35.0f), 0.0f };
        auto& light = reg.Emplace<DirectionalLightComponent>(sun);
        light.Intensity = 2.2f;
        light.CastShadows = false;
        m_Camera = m_Scene->CreateEntity("Camera");
        reg.Get<TransformComponent>(m_Camera).Position = { 0.0f, 0.0f, 2.35f };
        auto& camera = reg.Emplace<CameraComponent>(m_Camera);
        camera.FieldOfView = 30.0f;
        camera.Clear = ClearMode::Skybox;
        m_Scene->UpdateTransforms();
        m_Target = MakeScope<SceneRenderTarget>(kSize, kSize);
    }

    MaterialPreviews::~MaterialPreviews()
    {
        for (auto& [handle, entry] : m_Entries)
            if (entry.ImGuiId) RenderDevice::Get().DeferRelease([id = entry.ImGuiId] { ImGuiLayer::RemoveTexture(id); });
    }

    uint64_t MaterialPreviews::KeyOf(const Material& m)
    {
        uint64_t h = 1469598103934665603ull;
        auto mix = [&](const void* p, size_t n) {
            const auto* b = static_cast<const uint8_t*>(p);
            for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 1099511628211ull;
        };
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

    uint64_t MaterialPreviews::Get(AssetHandle handle)
    {
        Ref<Material> material = AssetManager::GetMaterial(handle);
        if (!material) return 0;
        uint64_t key = KeyOf(*material);
        auto it = m_Entries.find((uint64_t)handle);
        bool current = it != m_Entries.end() && it->second.Drawn && it->second.Key == key;
        if (!current && m_Queued.insert((uint64_t)handle).second) m_Queue.push_back(handle);
        return it != m_Entries.end() && it->second.Drawn ? it->second.ImGuiId : 0;
    }

    void MaterialPreviews::Render(VkCommandBuffer cmd)
    {
        auto& reg = m_Scene->GetRegistry();
        int drawn = 0;
        while (!m_Queue.empty() && drawn < 2)
        {
            AssetHandle handle = m_Queue.front();
            m_Queue.erase(m_Queue.begin());
            m_Queued.erase((uint64_t)handle);
            Ref<Material> material = AssetManager::GetMaterial(handle);
            if (!material) continue;
            reg.Get<MeshRendererComponent>(m_Sphere).Material = handle;
            CameraData camera = CameraData::FromEntity(*m_Scene, m_Camera, 1.0f);
            SceneRenderOptions options;
            m_Renderer.Render(cmd, *m_Scene, camera, *m_Target, options);

            Entry& entry = m_Entries[(uint64_t)handle];
            VkImageLayout from = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            if (!entry.Picture.IsValid())
            {
                entry.Picture = Image(ImageDesc{ kSize, kSize, SceneRenderTarget::kOutputFormat,
                                                 VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, 1, "MaterialPreview" });
                entry.ImGuiId = ImGuiLayer::AddTexture(RenderDevice::Get().GetClampSampler(), entry.Picture.GetView());
                from = VK_IMAGE_LAYOUT_UNDEFINED;
            }
            Vk::TransitionImage(cmd, entry.Picture.GetHandle(), from, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            m_Target->BlitOutputTo(cmd, entry.Picture.GetHandle(), { kSize, kSize });
            Vk::TransitionImage(cmd, entry.Picture.GetHandle(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            entry.Key = KeyOf(*material);
            entry.Drawn = true;
            ++drawn;
        }
    }
}
