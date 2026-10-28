#pragma once
#include <Dimension/Renderer/SceneRenderer.h>
#include <Dimension/Renderer/RenderTarget.h>
#include <Dimension/Renderer/Image.h>
#include <Dimension/Scene/Scene.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Editor
{
    using namespace Dimension;

    // Material previews (the Inspector and the Project window, like Unity's): each material on a lit sphere against
    // the sky, rendered by its own small scene into a picture kept per material. A picture is asked for with Get and
    // drawn (again) on a later frame when the material changed, a couple a frame.
    class MaterialPreviews
    {
    public:
        static constexpr uint32_t kSize = 192;

        MaterialPreviews();
        ~MaterialPreviews();
        // The material's picture as an ImGui texture (0 until it has been drawn once; an older one while it is drawn
        // again).
        uint64_t Get(AssetHandle material);
        // Draws the waiting ones (call while recording the frame).
        void Render(VkCommandBuffer cmd);
        // What a picture shows: the material's values and which of its textures are loaded.
        static uint64_t KeyOf(const Material& material);

    private:
        struct Entry
        {
            Image Picture;
            uint64_t ImGuiId = 0;
            uint64_t Key = 0;
            bool Drawn = false;
        };
        Ref<Scene> m_Scene;
        Entity m_Sphere, m_Camera;
        SceneRenderer m_Renderer;
        Scope<SceneRenderTarget> m_Target;
        std::unordered_map<uint64_t, Entry> m_Entries;
        std::vector<AssetHandle> m_Queue;
        std::unordered_set<uint64_t> m_Queued;
    };
}
