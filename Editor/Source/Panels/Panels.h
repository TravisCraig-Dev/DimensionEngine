#pragma once
#include "../EditorContext.h"
#include <Dimension/Core/Log.h>
#include <mutex>

namespace Editor
{
    class HierarchyPanel
    {
    public:
        explicit HierarchyPanel(EditorContext& ctx) : m_Ctx(ctx) {}
        void OnImGui(bool* open);
        void BeginRename(Entity e) { m_Renaming = e; m_RenameFocus = true; }

    private:
        void DrawNode(Scene& scene, Entity e);
        void DrawSearchResults(Scene& scene);
        EditorContext& m_Ctx;
        Entity m_Renaming = Entity::Null();
        bool m_RenameFocus = false;
        std::string m_RenameBuffer;
        Entity m_PendingParent = Entity::Null(), m_PendingChild = Entity::Null();
        std::vector<Entity> m_VisibleOrder, m_LastVisibleOrder; // for Shift+click range selection
        Entity m_Anchor = Entity::Null();
        int m_PendingMode = -1; // 0 child, 1 before, 2 after, 3 root
    };

    class InspectorPanel
    {
    public:
        explicit InspectorPanel(EditorContext& ctx) : m_Ctx(ctx) {}
        void OnImGui(bool* open);

    private:
        void DrawEntity(Scene& scene, Entity e);
        void DrawScript(Scene& scene, Entity e);
        void DrawPrefabHeader(Scene& scene, Entity e);
        void DrawAsset(AssetHandle handle);
        void DrawAddComponent(Scene& scene, Entity e);
        void DrawLODBar(Scene& scene, Entity e, LODGroupComponent& lod);
        EditorContext& m_Ctx;
        std::string m_ComponentFilter;
    };

    class ContentBrowserPanel
    {
    public:
        explicit ContentBrowserPanel(EditorContext& ctx) : m_Ctx(ctx) {}
        void OnImGui(bool* open);
        const fs::path& GetCurrentDirectory() const { return m_Current; }
        void ImportFiles(const std::vector<fs::path>& files);

    private:
        void DrawFolderTree(const fs::path& dir);
        EditorContext& m_Ctx;
        fs::path m_Current;
        float m_ThumbnailSize = 76.0f;
        fs::path m_Renaming;
        std::string m_RenameBuffer;
        bool m_RenameFocus = false;
        std::string m_Search;
    };

    class ConsolePanel
    {
    public:
        ConsolePanel();
        ~ConsolePanel();
        void OnImGui(bool* open);
        void Clear();
        bool ClearOnPlay = true;

    private:
        struct Entry
        {
            LogLevel Level;
            std::string Text;
            double Time;
            int Count;
        };
        std::mutex m_Mutex;
        std::vector<Entry> m_Entries;
        int m_SinkId = 0;
        bool m_ShowInfo = true, m_ShowWarn = true, m_ShowError = true, m_Collapse = true;
        bool m_ScrollToBottom = true;
        int m_Counts[4] = {};
    };

    // State machine graph for Animator Controllers (like Unity's Animator window).
    class AnimatorPanel
    {
    public:
        explicit AnimatorPanel(EditorContext& ctx) : m_Ctx(ctx) {}
        void OnImGui(bool* open);

    private:
        EditorContext& m_Ctx;
        AssetHandle m_Handle = AssetHandle::None();
        AssetHandle m_FramedHandle = AssetHandle::None();
        int m_SelectedState = -1; // -2 = Any State
        int m_SelectedGroup = -1; // selected sub-state machine
        int m_ViewGroup = -1;     // sub-state machine shown in the graph (-1 = the top level)
        int m_Layer = 0;          // 0 = base layer, i = controller Layers[i - 1]
        glm::vec2 m_Pan{ 0.0f };
        float m_Zoom = 1.0f;
        glm::vec2 m_ContextPos{ 0.0f };
    };

    // Tracks and keys of the selected object's Timeline (like Unity's Timeline/Animation window), with preview.
    class TimelinePanel
    {
    public:
        explicit TimelinePanel(EditorContext& ctx) : m_Ctx(ctx) {}
        void OnImGui(bool* open);

    private:
        EditorContext& m_Ctx;
        AssetHandle m_Handle = AssetHandle::None();
        float m_Time = 0.0f;
        bool m_Previewing = false;
        enum DragMode { DragNone, DragKey, DragClip, DragClipEnd };
        DragMode m_Drag = DragNone;
        float m_DragOffset = 0.0f;
        int m_Track = -1, m_Key = -1, m_Clip = -1;
        int m_CurveDragKey = -1, m_CurveDragChannel = 0;
        float m_CurveMin = 0.0f, m_CurveMax = 1.0f;
        int m_AddObject = 0, m_AddKind = 0;
        std::string m_AddComponent, m_AddField;
    };

    class SceneSettingsPanel
    {
    public:
        explicit SceneSettingsPanel(EditorContext& ctx) : m_Ctx(ctx) {}
        void OnImGui(bool* open);

    private:
        EditorContext& m_Ctx;
    };
}
