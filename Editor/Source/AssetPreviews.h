#pragma once
#include <Dimension/Renderer/SceneRenderer.h>
#include <Dimension/Renderer/RenderTarget.h>
#include <Dimension/Renderer/Image.h>
#include <Dimension/Scene/Scene.h>
#include <Dimension/Asset/AssetManager.h>
#include <map>
#include <optional>
#include <string>
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

        // The Inspector's preview of a model or prefab: turned and zoomed by dragging, posed at an animation clip's time.
        struct LiveView
        {
            float Yaw = 35.0f, Pitch = -22.0f, Zoom = 1.0f;
            std::string Clip; // empty: the model at rest (a prefab: as its Animators start)
            float Time = 0.0f;
        };
        struct ClipInfo
        {
            std::string Name;
            float Duration = 0.0f;
        };

        AssetPreviews();
        ~AssetPreviews();
        // The asset's picture as an ImGui texture (0 for other kinds of asset, and until it has been drawn once; an
        // older one while it is drawn again).
        uint64_t Get(AssetHandle asset);
        // Asks for the live preview of a model or prefab this frame, as `view` says; returns its picture as last drawn (0
        // until drawn once). Only one asset at a time has a live preview.
        // `slot` tells live previews apart (one per Inspector window).
        uint64_t Live(AssetHandle asset, const LiveView& view, int slot = 0);
        // The animation clips the live preview's subject can play (known once it has been drawn).
        const std::vector<ClipInfo>& LiveClips(int slot = 0) { return m_Live[slot].Clips; }
        // Draws the waiting ones (call while recording the frame).
        void Render(VkCommandBuffer cmd);
        static bool HasPreview(AssetType type)
        {
            return type == AssetType::Material || type == AssetType::Mesh || type == AssetType::Prefab || type == AssetType::VisualEffect;
        }
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
        bool Stage(AssetHandle asset, const LiveView* view = nullptr);
        void Draw(VkCommandBuffer cmd, Entry& entry, AssetHandle asset);
        struct LiveSlot
        {
            Entry Picture;
            AssetHandle Asset = AssetHandle::None();
            std::optional<LiveView> Request;
            std::vector<ClipInfo> Clips;
        };
        std::map<int, LiveSlot> m_Live;
        std::vector<ClipInfo> m_StagedClips; // the clips of the subject Stage built last (with a view)
        Ref<Scene> m_Scene;
        Entity m_Subject, m_Camera;
        SceneRenderer m_Renderer;
        Scope<SceneRenderTarget> m_Target;
        std::unordered_map<uint64_t, Entry> m_Entries;
        std::vector<AssetHandle> m_Queue;
        std::unordered_set<uint64_t> m_Queued;
        // Visual Effects: the picture after a moment of play (a few simulation steps drawn), and the live one playing.
        uint32_t m_EffectGeneration = 1000; // (each staged effect starts with no particles of the last)
        AssetHandle m_LiveEffect = AssetHandle::None();
    };
}
