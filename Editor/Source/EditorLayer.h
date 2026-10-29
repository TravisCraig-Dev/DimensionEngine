#pragma once
#include <imgui.h>
#include "EditorContext.h"
#include "EditorCamera.h"
#include "Panels/Panels.h"
#include <Dimension/Core/Application.h>
#include <Dimension/Renderer/SceneRenderer.h>
#include "AssetPreviews.h"
#include <Dimension/Audio/AudioMixer.h>
#include <Dimension/Asset/AssetPackage.h>
#include <Dimension/Scene/SceneTemplates.h>
#include <atomic>
#include <unordered_set>
#include <memory>

namespace Editor
{
    struct EditorOptions
    {
        bool StartInPlayMode = false;
        std::string SelectEntity; // select by name on startup (automation/testing)
        std::string SelectAsset;  // --select-asset path: select this asset (Assets-relative) and show its folder in the Project window
        std::string FocusWindow;  // bring a panel to front on startup (automation/testing)
        std::string BuildFolder;  // batch build: build the project here and exit (CI)
        std::string ScenePath;    // open this scene instead of the project's start scene
        std::string PrefabPath;   // open this prefab in Prefab Mode on startup (path relative to Assets or absolute)
        std::string OpenSceneLater; // request this scene like the UI does, after a few frames (automation/testing)
        bool MenuOpenScene = false; // run File > Open Scene... after a few frames (automation/testing)
        std::string OpenWindow;   // open a tool window on startup: "Build Settings", "Preferences", "Project Settings"
        int Theme = -1;           // --theme dark|light|midnight: this session only (preferences stay unchanged)
        int LinePoint = -1;       // --line-point N: start editing point N of the selected Line Renderer
        int FrameEvent = -1;      // --frame-event N: with --open "Frame Debugger", step the Scene view to draw N (0-based)
        std::string MenuCommand;  // --menu "Tools/Item": run a C# extension's menu item once the scripts have loaded
        std::string Search;       // --search "query": start with this Hierarchy search
        std::string SearchQuery;  // --find "query": with --open "Search", the Search window's query
        std::string PackagePath;  // --package file: with --open "Import Package", the package shown
        std::string AdditiveScenes; // --open-additive "a.scene,b.scene": opened alongside the scene
        std::string PrefabInstance; // --prefab-instance "Name": that instance's prefab in Prefab Mode, in context
        std::string NewSceneTemplate; // --new-scene "Template": start with a new scene from that template
        std::string Layout;           // --layout "Name": apply a saved layout at startup
        std::string ProjectView;      // --project-view list|grid: the Project window's view for the session
        std::string ProjectSearch;    // --project-search "query": the Project window's search
        bool RememberProject = true;  // add the project to the Hub's recent list (--no-recent: automation)
        bool InspectorDebug = false;      // --inspector-debug: the Inspector starts in Debug mode
        std::string DrawMode;             // --draw-mode wireframe|shaded-wireframe|unlit|overdraw: the Scene view's
        std::string HideEntities;         // --hide "A,B": hidden in the Scene view for the session
        std::string IsolateEntities;      // --isolate "A,B": the Scene view's isolation
        bool VisualizeNavigation = false; // Game view overlay of UI navigation links (--visualize-nav starts with it on)
        bool WaitForDebugger = false;     // --wait-for-debugger: wait up to 60 s for a C# debugger before loading scripts
        bool BuildSucceeded = false; // result of the batch build
    };

    class EditorLayer : public Dimension::Layer
    {
    public:
        explicit EditorLayer(EditorOptions& options) : m_Options(options), m_OptionsOut(&options) {}

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
        void NewSceneFrom(const Dimension::SceneTemplates::Template& t);
        // File > New Scene...: the templates (the chosen one), and naming the open scene as a new template.
        bool m_ShowNewScene = false;
        int m_NewSceneTemplate = 0;
        std::string m_TemplateName, m_TemplateDescription;
        void DrawNewSceneWindow();
        // File > Project Hub... (recent projects, a new one, another folder) and switching the open project.
        bool m_ShowHub = false;
        std::string m_HubName, m_HubLocation;
        int m_HubStarter = 0;
        bool SwitchProject(const fs::path& dir);
        void DrawProjectHub();
        // Window > Layouts: named window arrangements of the project (applied between frames).
        std::string m_PendingLayout, m_LayoutName;
        std::vector<std::pair<const char*, bool*>> WindowToggles();
        fs::path LayoutsFolder() const;
        std::vector<std::string> LayoutNames() const;
        void SaveLayout(const std::string& name);
        void ApplyPendingLayout();
        void DrawLayoutsMenu();
        // Edit > Undo History (cached between refreshes), and notes' labels in the Scene view.
        bool m_ShowUndoHistory = false;
        std::vector<EditorContext::HistoryStep> m_HistoryCache;
        int m_HistoryFrame = 0;
        size_t m_HistoryDone = 0, m_HistoryCount = 0;
        void DrawUndoHistory();
        void DrawNoteLabels(const ImVec2& origin, const ImVec2& size);
        void OpenScene(const fs::path& path);
        // Another scene's objects alongside the main scene's (a Hierarchy section of its own), and closing it.
        void OpenSceneAdditive(const fs::path& path);
        void CloseAddedScene(size_t section);
        bool SaveScene();
        bool SaveSceneAs();
        bool ConfirmDiscardChanges(); // false = user cancelled
        void OpenSceneWithDialog();   // File > Open Scene...

        // Prefab Mode
        // Opened from an instance (`from`: its GUID in the scene), the scene around it shows greyed (in context).
        void OpenPrefabMode(AssetHandle prefab, std::optional<uint64_t> from = std::nullopt);
        bool LeavePrefabMode(); // saves or asks first; false = user cancelled
        bool SavePrefabStage();
        void DrawPrefabModeBar();
        EditorCamera m_SceneCamera; // the scene's view while a prefab is open
        bool m_LeavePrefabRequested = false;
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
        void DrawBuildSettings();
        void DrawPreferences();
        void DrawAudioMixer();
        void DrawLevelMeter(int group, float height);
        std::vector<AudioMixerGroup> m_MixerBeforePlay; // play mode changes to the mixer (scripts, snapshots) revert on Stop
        struct MeterDisplay { float Rms = 0.0f, Peak = 0.0f, Hold = 0.0f, HoldTime = 0.0f; };
        std::vector<MeterDisplay> m_MixerMeters; // per group, smoothed for display
        void DrawTilePalette();
        // Scene-view tile painting for the selected Tilemap; returns true while it owns the mouse.
        bool UpdateTilePainting(const ImVec2& origin, const ImVec2& size);
        // Terrain sculpting/painting with the Inspector's brush (true while it owns the mouse).
        bool UpdateTerrainBrush(const ImVec2& origin, const ImVec2& size);
        bool m_TerrainStroke = false;
        float m_TerrainFlatten = 0.0f;
        void ApplyPreferences();
        void Autosave(float dt);
        void DrawAbout();
        void HandleShortcuts();
        void BuildDefaultLayout(unsigned int dockspaceId);
        // Builds the game into `folder` (asks for one when empty); optionally runs it afterwards.
        bool BuildGame(bool run = false, fs::path folder = {});
        void FocusSelected();
        void LoadGameModule();
        void ReloadGameModule();
        void BuildGameModule();
        void PollGameModule(float dt);
        void InitCSharp();
        // C# editor extensions: the hooks their API reaches, their menu entries, and their open windows (by title).
        void InstallScriptHooks();
        void DrawScriptMenus();
        void DrawScriptWindows();
        // Handles of the selected object's custom Inspectors (OnSceneGUI) in the Scene view: true while one is hovered
        // or dragged. DoHandle draws one (EditorHooks::Handle).
        bool UpdateScriptHandles(const ImVec2& origin, const ImVec2& size);
        int DoHandle(int kind, float* v, int n, const char* text);
        struct HandleView
        {
            bool Active = false;
            ImVec2 Origin, Size;
            glm::mat4 View{ 1.0f }, Proj{ 1.0f };
            int Counter = 0; // the handle drawn last (ids: per script, in drawing order)
            bool Hot = false;
        } m_HandleView;
        int m_HandleDrag = 0;            // the dot being dragged (0: none)
        bool m_HandleEditActive = false; // a handle drag is under way (its edits share one undo step)
        std::map<std::string, bool> m_ScriptWindows;
        void BuildCSharp();
        void PollCSharp(float dt);

        EditorOptions m_Options;
        EditorOptions* m_OptionsOut = nullptr; // batch build result is reported back to main
        bool m_BatchBuildDone = false;
        EditorContext m_Ctx;
        EditorCamera m_Camera;
        Scope<HierarchyPanel> m_Hierarchy;
        Scope<InspectorPanel> m_Inspector;
        Scope<ContentBrowserPanel> m_Content;
        Scope<ConsolePanel> m_Console;
        Scope<SceneSettingsPanel> m_SceneSettings;
        Scope<AnimatorPanel> m_Animator;
        Scope<TimelinePanel> m_Timeline;
        Scope<NavigationPanel> m_Navigation;
        Scope<ProfilerPanel> m_Profiler;

        Scope<SceneRenderTarget> m_SceneTarget, m_GameTarget;
        SceneRenderer m_SceneRenderer, m_GameRenderer;
        glm::uvec2 m_SceneViewSize{ 1280, 720 }, m_GameViewSize{ 1280, 720 };
        bool m_SceneViewVisible = false, m_GameViewVisible = false;
        bool m_SceneViewHovered = false, m_SceneViewFocused = false, m_GameViewFocused = false;
        int m_FocusGameView = 0;
        int m_FocusSceneView = 0;
        int m_AutomationFrames = 0, m_MenuAutomationFrames = 0;
        int m_FocusRequestFrames = 3; // frames left to request focus (docking may not be ready on the first one)
        int m_GizmoOperation = 7; // ImGuizmo::TRANSLATE
        bool m_GizmoLocal = false;
        // Point editing in the Scene view (Line Renderer, Terrain Path, Path Placer): the picked point (-1 = none).
        int m_LinePoint = -1;
        Dimension::Entity m_LinePointOwner;
        bool UpdateLinePointEditing(ImVec2 origin, ImVec2 size);
        // Vertex snapping: hold V over the Scene view to pick the selection's vertex nearest the mouse, and drag it
        // onto another object's vertex (true while it owns the mouse).
        bool UpdateVertexSnap(const ImVec2& origin, const ImVec2& size);
        bool m_VertexDrag = false;
        bool m_ShowSnapSettings = false;
        // Window > Lighting, and the Search window (Ctrl+K).
        bool m_ShowLighting = false, m_ShowSearch = false, m_SearchFocus = false;
        std::string m_SearchQuery;
        int m_SearchIndex = 0;
        struct EditorCommand
        {
            std::string Name;
            std::function<void()> Run;
        };
        std::vector<EditorCommand> EditorCommands();
        void OpenSearch(bool open);
        void DrawSearchWindow();
        void DrawLightingWindow();
        void BakeAllProbes();
        // Window > Shortcuts (rebinding: the shortcut listening for its new key, -1 none).
        bool m_ShowShortcuts = false;
        int m_RebindingShortcut = -1;
        void DrawShortcutsWindow();
        // Asset packages: File > Export Package... (the chosen assets, files left out by path) and Import Package...
        bool m_ShowExportPackage = false, m_ExportDependencies = true, m_ShowImportPackage = false;
        std::vector<Dimension::AssetHandle> m_ExportRoots;
        std::unordered_set<std::string> m_ExportExcluded;
        Dimension::AssetPackage::Contents m_ImportContents;
        std::vector<bool> m_ImportChosen;
        std::string m_ImportName;
        void OpenExportPackage(const std::vector<Dimension::AssetHandle>& assets);
        void DrawExportPackage();
        void ChooseImportPackage();
        void OpenImportPackage(const fs::path& package);
        void DrawImportPackage();
        void DrawSnapSettings();
        glm::vec3 m_VertexOffset{ 0.0f }; // the dragged vertex from the selected object's pivot
        // Moves the selection's top-level objects by a world-space offset.
        void MoveSelection(const glm::vec3& delta);
        void DrawNavigationOverlay(ImVec2 origin, ImVec2 size);
        // Edit mode: drag the selected UI Rect (move, or resize by its edges and corners) in the Game view, and click a
        // widget there to select it.
        void UpdateUIRectEditing(ImVec2 origin, ImVec2 size, bool hovered);
        int m_UIRectDrag = -1; // -1 none, 0 move, 1..8 handles (see UpdateUIRectEditing)
        glm::vec2 m_UIRectDragStart{ 0.0f }, m_UIRectStartPosition{ 0.0f }, m_UIRectStartSize{ 0.0f };
        bool DeleteLinePoint(); // removes the picked point; false when no point is picked
        bool m_ShowGrid = true;
        bool m_ShowGizmos = true; // C# scripts' OnDrawGizmos in the Scene view
        Dimension::SceneDrawMode m_DrawMode = Dimension::SceneDrawMode::Shaded;
        std::string m_VisibilityScene = "\x01"; // the scene whose visibility is loaded (none yet)
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
        bool m_ShowTimeline = false;
        bool m_ShowNavigation = false;
        bool m_ShowFrameDebugger = false;
        bool m_ShowSpriteEditor = false;
        Scope<SpriteEditorPanel> m_SpriteEditor;
        Scope<AssetPreviews> m_AssetPreviews;
        Scope<FrameDebuggerPanel> m_FrameDebugger;
        bool m_ShowProfiler = false;
        bool m_ShowStats = true, m_ShowSceneSettings = true, m_ShowProjectSettings = false, m_ShowAbout = false;
        bool m_ShowBuildSettings = false, m_ShowPreferences = false, m_ShowAudioMixer = true, m_ShowTilePalette = false;
        int m_TileBrush = 0;
        int m_TileTool = 0; // 0 paint, 1 erase
        bool m_TileStroke = false;
        float m_AutosaveTimer = 0.0f;
        bool m_ResetLayout = false;
        bool m_ShowImGuiDemo = false;
        std::string m_IniPath;
        float m_StatsTimer = 0.0f, m_DisplayFps = 0.0f, m_DisplayMs = 0.0f;
    };
}
