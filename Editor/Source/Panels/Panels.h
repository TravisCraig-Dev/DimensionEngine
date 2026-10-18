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
        glm::vec2 m_Pan{ 0.0f };
        float m_Zoom = 1.0f;
        glm::vec2 m_ContextPos{ 0.0f };
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
