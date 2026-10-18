#pragma once
#include "EditorContext.h"
#include "EditorCamera.h"
#include "Panels/Panels.h"
#include <Dimension/Core/Application.h>
#include <Dimension/Renderer/SceneRenderer.h>
#include <atomic>
#include <memory>

namespace Editor
{
    struct EditorOptions
    {
        bool StartInPlayMode = false;
        std::string SelectEntity; // select by name on startup (automation/testing)
        std::string FocusWindow;  // bring a panel to front on startup (automation/testing)
    };

    class EditorLayer : public Dimension::Layer
    {
    public:
        explicit EditorLayer(const EditorOptions& options) : m_Options(options) {}

        void OnAttach() override;
        void OnDetach() override;
        void OnUpdate(float dt) override;
        void OnImGui() override;
        void OnRender(VkCommandBuffer cmd) override;
        void OnFilesDropped(const std::vector<fs::path>& files) override;
        bool OnCloseRequested() override;

    private:
        // Scene management
        void NewScene();
        void OpenScene(const fs::path& path);
        bool SaveScene();
        bool SaveSceneAs();
        bool ConfirmDiscardChanges(); // false = user cancelled
        void UpdateWindowTitle();

        // Play mode
        void Play();
        void Stop();
        void TogglePause();

        // UI
        void DrawDockspace();
        void DrawMenuBar();
        void DrawToolbar();
        void DrawSceneView();
        void DrawGameView();
        void DrawStats();
        void DrawProjectSettings();
        void DrawAbout();
        void HandleShortcuts();
        void BuildDefaultLayout(unsigned int dockspaceId);
        void BuildGame();
        void FocusSelected();
        void LoadGameModule();
        void ReloadGameModule();
        void BuildGameModule();
        void PollGameModule(float dt);
        void InitCSharp();
        void BuildCSharp();
        void PollCSharp(float dt);

        EditorOptions m_Options;
        EditorContext m_Ctx;
        EditorCamera m_Camera;
        Scope<HierarchyPanel> m_Hierarchy;
        Scope<InspectorPanel> m_Inspector;
        Scope<ContentBrowserPanel> m_Content;
        Scope<ConsolePanel> m_Console;
        Scope<SceneSettingsPanel> m_SceneSettings;
        Scope<AnimatorPanel> m_Animator;

        Scope<SceneRenderTarget> m_SceneTarget, m_GameTarget;
        SceneRenderer m_SceneRenderer, m_GameRenderer;
        glm::uvec2 m_SceneViewSize{ 1280, 720 }, m_GameViewSize{ 1280, 720 };
        bool m_SceneViewVisible = false, m_GameViewVisible = false;
        bool m_SceneViewHovered = false, m_SceneViewFocused = false, m_GameViewFocused = false;
        int m_FocusGameView = 0;
        int m_FocusRequestFrames = 3; // frames left to request focus (docking may not be ready on the first one)
        int m_GizmoOperation = 7; // ImGuizmo::TRANSLATE
        bool m_GizmoLocal = false;
        bool m_ShowGrid = true;
        bool m_WasWindowFocused = true;
        bool m_PickAdditive = false; // Ctrl+click toggles instead of replacing the selection
        // Game module hot reload
        std::shared_ptr<std::atomic<int>> m_BuildState; // 0 idle, 1 building, 2 succeeded, 3 failed
        bool m_ReloadPending = false;
        float m_ModuleCheckTimer = 0.0f;
        // C# scripts
        std::shared_ptr<std::atomic<int>> m_CSharpBuild; // 0 idle, 1 building, 2 ok, 3 failed
        fs::file_time_type m_CSharpBuiltSources{};
        float m_CSharpCheckTimer = 0.0f;
        bool m_CSharpReloadPending = false;

        bool m_ShowHierarchy = true, m_ShowInspector = true, m_ShowContent = true, m_ShowConsole = true;
        bool m_ShowAnimator = true;
        bool m_ShowStats = true, m_ShowSceneSettings = true, m_ShowProjectSettings = false, m_ShowAbout = false;
        bool m_ResetLayout = false;
        bool m_ShowImGuiDemo = false;
        std::string m_IniPath;
        float m_StatsTimer = 0.0f, m_DisplayFps = 0.0f, m_DisplayMs = 0.0f;
    };
}
