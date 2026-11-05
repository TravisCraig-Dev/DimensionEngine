#include "Dimension/Renderer/CameraStack.h"
#include "Dimension/Renderer/SceneRenderer.h"
#include "Dimension/Renderer/RenderTarget.h"
#include "Dimension/Renderer/Texture.h"
#include "Dimension/Asset/AssetManager.h"
#include "Dimension/Scene/Scene.h"
#include <algorithm>

namespace Dimension
{
    CameraStack::CameraStack() = default;
    CameraStack::~CameraStack() = default;

    void CameraStack::Clear() { m_Views.clear(); }

    std::vector<std::pair<Entity, glm::vec4>> CameraStack::Cameras(Scene& scene, glm::uvec2 screen)
    {
        std::vector<std::pair<Entity, glm::vec4>> out;
        if (screen.x == 0 || screen.y == 0) return out;
        Entity primary = scene.GetPrimaryCamera();
        std::vector<std::pair<float, Entity>> extra;
        for (auto [e, cam, t] : scene.GetRegistry().ViewOf<CameraComponent, TransformComponent>())
        {
            if (!t.ActiveInHierarchy || e == primary || cam.TargetTexture.IsValid()) continue;
            glm::vec4 r = cam.Viewport;
            bool whole = r.x <= 0.0f && r.y <= 0.0f && r.x + r.z >= 1.0f && r.y + r.w >= 1.0f;
            // Depth Only cameras add to the image even over the whole screen.
            if ((!whole || cam.Clear == ClearMode::DepthOnly) && r.z > 0.0f && r.w > 0.0f) extra.emplace_back(cam.Depth, e);
        }
        std::stable_sort(extra.begin(), extra.end(), [](auto& a, auto& b) { return a.first < b.first; });
        for (auto& [depth, e] : extra)
        {
            glm::vec4 r = scene.GetRegistry().Get<CameraComponent>(e).Viewport;
            glm::vec2 size(screen);
            glm::vec2 mn = glm::clamp(glm::vec2(r.x, r.y), glm::vec2(0.0f), glm::vec2(1.0f)) * size;
            glm::vec2 mx = glm::clamp(glm::vec2(r.x + r.z, r.y + r.w), glm::vec2(0.0f), glm::vec2(1.0f)) * size;
            mn = glm::floor(mn);
            mx = glm::floor(mx);
            if (mx.x - mn.x < 1.0f || mx.y - mn.y < 1.0f) continue;
            out.emplace_back(e, glm::vec4(mn, mx));
        }
        return out;
    }

    void CameraStack::Render(VkCommandBuffer cmd, Scene& scene, glm::uvec2 screen, std::vector<UI::DrawCommand>& out)
    {
        ++m_Frame;
        // Cameras drawing into Render Textures: rendered at the texture's size, then copied into it (the output holds
        // sRGB-encoded bytes, which the texture's sRGB format reads back as linear colors).
        for (auto [e, cam, t] : scene.GetRegistry().ViewOf<CameraComponent, TransformComponent>())
        {
            if (!t.ActiveInHierarchy || !cam.TargetTexture.IsValid()) continue;
            Ref<Texture2D> texture = AssetManager::GetTexture(cam.TargetTexture);
            if (!texture) continue;
            uint32_t w = texture->GetWidth(), h = texture->GetHeight();
            uint64_t id = ((uint64_t)e.Generation << 32) | e.Index;
            View& view = m_Views[id];
            view.Frame = m_Frame;
            // Manual Render: only when Camera.Render() asked since it last drew (never before the first request).
            if (cam.ManualRender && view.Texture == texture && view.Requests == cam.RenderRequests) continue;
            if (cam.ManualRender && cam.RenderRequests == 0) continue;
            view.Requests = cam.RenderRequests;
            if (!view.Renderer) view.Renderer = MakeScope<SceneRenderer>();
            if (!view.Target) view.Target = MakeScope<SceneRenderTarget>(w, h);
            else if (view.Target->GetWidth() != w || view.Target->GetHeight() != h) view.Target->Resize(w, h);
            view.Texture = texture; // kept alive while the GPU may still copy into it
            CameraData data = CameraData::FromEntity(scene, e, view.Target->GetAspect());
            view.Renderer->Render(cmd, scene, data, *view.Target, {});
            VkImage src = view.Target->GetOutput().GetHandle(), dst = texture->GetImage().GetHandle();
            Vk::TransitionImage(cmd, src, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            Vk::TransitionImage(cmd, dst, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            VkImageCopy copy{};
            copy.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            copy.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            copy.extent = { w, h, 1 };
            vkCmdCopyImage(cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
            Vk::TransitionImage(cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            Vk::TransitionImage(cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
        for (auto& [e, rect] : Cameras(scene, screen))
        {
            uint64_t id = ((uint64_t)e.Generation << 32) | e.Index;
            View& view = m_Views[id];
            uint32_t w = (uint32_t)(rect.z - rect.x), h = (uint32_t)(rect.w - rect.y);
            if (!view.Renderer) view.Renderer = MakeScope<SceneRenderer>();
            if (!view.Target) view.Target = MakeScope<SceneRenderTarget>(w, h);
            else if (view.Target->GetWidth() != w || view.Target->GetHeight() != h) view.Target->Resize(w, h);
            view.Frame = m_Frame;
            CameraData data = CameraData::FromEntity(scene, e, view.Target->GetAspect());
            view.Renderer->Render(cmd, scene, data, *view.Target, {});
            UI::DrawCommand quad{};
            quad.Min = { rect.x, rect.y };
            quad.Max = { rect.z, rect.w };
            quad.UVMin = { 0.0f, 0.0f };
            quad.UVMax = { 1.0f, 1.0f };
            quad.Color = { 1.0f, 1.0f, 1.0f, 1.0f };
            quad.Texture = view.Target->GetOutputBindless() | UI::kDisplaySpaceTextureBit;
            out.push_back(quad);
        }
        // Cameras gone or no longer drawn: their renderers and images go (after the GPU is done with this frame's).
        std::erase_if(m_Views, [&](const auto& v) { return v.second.Frame + 4 < m_Frame; });
    }
}
