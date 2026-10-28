#pragma once
#include <Dimension/Renderer/SceneRenderer.h>
#include <Dimension/Renderer/RenderTarget.h>
#include <Dimension/Renderer/Image.h>
#include <Dimension/Scene/Scene.h>
#include <Dimension/Asset/AssetManager.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Editor
{
    using namespace Dimension;

    // Asset previews (the Inspector and the Project window, like Unity's): a material on a lit sphere, a model with
    // its own materials and a prefab as it is built, each framed from a little above, against the sky. They are
    // rendered by a small scene of their own into a picture kept per asset. A picture is asked for with Get and
    // drawn (again) on a later frame when the asset changed, a couple a frame.
    class AssetPreviews
    {
    public:
        static constexpr uint32_t kSize = 192;

        AssetPreviews();
        ~AssetPreviews();
        // The asset's picture as an ImGui texture (0 for other kinds of asset, and until it has been drawn once; an
        // older one while it is drawn again).
        uint64_t Get(AssetHandle asset);
        // Draws the waiting ones (call while recording the frame).
        void Render(VkCommandBuffer cmd);
        static bool HasPreview(AssetType type) { return type == AssetType::Material || type == AssetType::Mesh || type == AssetType::Prefab; }
        // What a picture shows: a material's values and which of its textures are loaded; a model or prefab's file.
        static uint64_t KeyOf(AssetHandle asset);

    private:
        struct Entry
        {
            Image Picture;
            uint64_t ImGuiId = 0;
            uint64_t Key = 0;
            bool Drawn = false;
        };
        // Builds the subject for an asset and frames it; false when there is nothing to show.
        bool Stage(AssetHandle asset);
        Ref<Scene> m_Scene;
        Entity m_Subject, m_Camera;
        SceneRenderer m_Renderer;
        Scope<SceneRenderTarget> m_Target;
        std::unordered_map<uint64_t, Entry> m_Entries;
        std::vector<AssetHandle> m_Queue;
        std::unordered_set<uint64_t> m_Queued;
    };
}
