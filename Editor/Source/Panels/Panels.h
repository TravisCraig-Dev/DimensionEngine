#pragma once
#include "../EditorContext.h"
#include <Dimension/Core/Log.h>
#include <Dimension/Renderer/SceneRenderer.h>
#include <Dimension/Renderer/SpriteSlicing.h>
#include <Dimension/Asset/AssetListing.h>
#include <Dimension/Asset/ProjectFavorites.h>
#include "../EditorPreferences.h"
#include "../AssetPreviews.h"
#include <Dimension/Audio/AudioEngine.h>
#include <Dimension/Core/Profiler.h>
#include <optional>
#include <imgui.h>
#include <map>
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
        void DrawVisibilityToggles(Scene& scene, Entity e, ImVec2 rowMin, ImVec2 rowMax, bool rowHovered);
        void DrawSearchResults(Scene& scene);
        EditorContext& m_Ctx;
        Entity m_Renaming = Entity::Null();
        bool m_RenameFocus = false;
        std::string m_RenameBuffer;
        Entity m_PendingParent = Entity::Null(), m_PendingChild = Entity::Null();
        std::vector<Entity> m_VisibleOrder, m_LastVisibleOrder; // for Shift+click range selection
        Entity m_Anchor = Entity::Null();
        int m_PendingMode = -1; // 0 child, 1 before, 2 after, 3 root
        int m_PendingSection = -1; // with 3: into this added scene (1-based)
        std::map<size_t, bool> m_SectionDirty; // added scenes' unsaved changes (checked now and then)
    };

    class InspectorPanel
    {
    public:
        // `title` names the window (extra Inspectors: "Inspector 2###Inspector2"); `previewSlot` keeps its asset
        // previews apart from the other Inspectors'.
        explicit InspectorPanel(EditorContext& ctx, std::string title = "Inspector", int previewSlot = 0)
            : m_Ctx(ctx), m_Title(std::move(title)), m_PreviewSlot(previewSlot) {}
        void OnImGui(bool* open);
        // The lock (like Unity's padlock): a locked Inspector keeps showing what was selected when it was locked.
        void LockToSelection();
        void Unlock() { m_Locked = false; }
        bool IsLocked() const { return m_Locked; }
        // The search box's text (automation).
        void SetSearch(const std::string& query) { m_Search = query; }
        // Debug mode (like Unity's): hidden fields, and C# scripts' private state, shown read-only.
        bool DebugMode = false;
        // The next asset preview's view, "yaw=..,pitch=..,zoom=..,clip=..,time=..,play" (automation).
        void SetPreview(const std::string& spec);

    private:
        void DrawEntity(Scene& scene, Entity e);
        void DrawScript(Scene& scene, Entity e);
        void DrawPrefabHeader(Scene& scene, Entity e);
        void DrawAsset(AssetHandle handle);
        // Several selected assets: their import settings together (textures or models), else their names.
        void DrawAssets(const std::vector<AssetHandle>& assets);
        // A texture's or model's folder import default: which applies, and a button to make its settings the default.
        void DrawImportDefault(AssetHandle handle);
        // An asset's Uses / Used By lists and its Find References In Scene button.
        void DrawAssetReferences(AssetHandle handle);
        void DrawLockBar(bool missing);
        std::string m_Title;
        int m_PreviewSlot = 0;
        bool m_Locked = false;
        SelectionSnapshot m_Lock;
        fs::path m_PreviewOwnFile; // the audio clip this Inspector plays
        std::string m_Search;      // the search box: components and fields shown
        // A model's or prefab's live preview with its clips; an audio clip's waveform, Play and playhead.
        void DrawLivePreview(AssetHandle handle);
        void DrawAudioPreview(AssetHandle handle);
        AssetHandle m_LiveShown = AssetHandle::None(), m_WaveShown = AssetHandle::None();
        AssetPreviews::LiveView m_LiveView;
        bool m_LivePlaying = false, m_LiveKeep = false, m_PlayPreviewRequested = false, m_PreviewLoop = false;
        std::optional<AudioWaveform> m_Wave;
        AssetHandle m_RefsShown = AssetHandle::None();
        int m_RefsFrame = 0;
        std::vector<AssetHandle> m_Uses, m_UsedBy;
        void DrawAddComponent(Scene& scene, Entity e);
        void DrawLODBar(Scene& scene, Entity e, LODGroupComponent& lod);
        // Debug mode: the hidden fields of an object's data, read-only rows of the property grid.
        void DrawHiddenFields(const std::vector<FieldInfo>& fields, void* object, Scene& scene);
        EditorContext& m_Ctx;
        std::string m_ComponentFilter;
        // A preset asset's values on a stand-in object (its Inspector).
        Scope<Scene> m_PresetScene;
        Entity m_PresetEntity = Entity::Null();
        AssetHandle m_PresetShown = AssetHandle::None();
    };

    class ContentBrowserPanel
    {
    public:
        explicit ContentBrowserPanel(EditorContext& ctx) : m_Ctx(ctx) {}
        void OnImGui(bool* open);
        const fs::path& GetCurrentDirectory() const { return m_Current; }
        void ShowDirectory(const fs::path& dir) { m_Current = dir; }
        // Grid or list (saved in the Preferences); a search shown (automation).
        void SetListView(bool list, bool remember = true);
        void SetSearch(const std::string& query) { m_Search = query; }
        void ImportFiles(const std::vector<fs::path>& files);

    private:
        void DrawFolderTree(const fs::path& dir);
        // An item's clicks (selection, Ctrl and Shift), double click, drag and drop and context menu (grid and list).
        void ItemBehavior(const fs::path& path, const std::string& name, bool isDir, AssetHandle handle, AssetType type, bool hovered);
        void DrawRenameField(const fs::path& path, float width);
        void DrawList(const std::vector<fs::directory_entry>& entries, std::vector<AssetHandle>& shown);
        void DrawFavorites();
        EditorContext& m_Ctx;
        // What this frame's items asked for, done after drawing them.
        fs::path m_DeletePath, m_EnterDir;
        AssetHandle m_RangeTo = AssetHandle::None();
        // The list view and its sort; Favorites (read again now and then), the one being renamed.
        bool m_ListView = EditorPreferences::Get().ProjectListView;
        AssetSort m_SortBy = AssetSort::Name;
        bool m_SortAscending = true;
        std::vector<Favorite> m_Favorites;
        int m_FavoritesFrame = -1000;
        int m_RenamingFavorite = -1;
        fs::path m_Current;
        float m_ThumbnailSize = 76.0f;
        fs::path m_Renaming;
        std::string m_RenameBuffer;
        AssetHandle m_AssetAnchor = AssetHandle::None(); // where a Shift click's range starts
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
    class ProfilerPanel
    {
    public:
        void OnImGui(bool* open);
        // The timeline's visible stretch of the frame, in ms (automation; ms <= 0: the whole frame).
        void SetView(float startMs, float ms) { m_ViewStart = startMs, m_ViewMs = ms; }
        // A saved recording shown instead of the live frames (false: not a recording); back to live with Close.
        bool OpenRecording(const fs::path& file);
        void CloseRecording() { m_Loaded.reset(), m_Picked = -1; }
        // Picks a frame (index in what is shown), or pins it for comparison (automation).
        void PickFrame(int index) { m_Picked = index; }
        void PinFrame(int index);

    private:
        // The picked frame's calls as bars along its time, a row per nesting level, zoomed and panned with the mouse.
        void DrawTimeline(const Profiler::FrameRecord& frame);
        int m_Picked = -1; // frame index in the history while paused
        bool m_ShowMemory = true;
        // A recording opened from a file (shown instead of the live frames), and a frame pinned to compare others with.
        std::optional<std::deque<Profiler::FrameRecord>> m_Loaded;
        std::string m_LoadedName;
        std::optional<Profiler::FrameRecord> m_Pinned;
        std::string m_PinnedLabel;
        bool m_ShowTimeline = true;
        float m_ViewStart = 0.0f, m_ViewMs = 0.0f; // 0: the whole frame
    };

    class SpriteEditorPanel
    {
    public:
        explicit SpriteEditorPanel(EditorContext& ctx) : m_Ctx(ctx) {}
        void OnImGui(bool* open);
        // Starts editing a texture's sprites (as saved).
        void Open(AssetHandle texture);

    private:
        EditorContext& m_Ctx;
        AssetHandle m_Texture = AssetHandle::None();
        std::vector<SpriteRect> m_Sprites; // being edited (Apply saves them)
        std::vector<uint32_t> m_Pixels;    // the image, for slicing by what is opaque
        int m_Width = 0, m_Height = 0;
        int m_Selected = -1;
        bool m_Dirty = false;
        int m_Mode = 0; // grid by count, grid by size, automatic
        glm::ivec2 m_Cells{ 4, 4 }, m_CellSize{ 32, 32 }, m_Offset{ 0 }, m_Gap{ 0 };
        bool m_SkipEmpty = true;
        int m_AlphaThreshold = 8, m_MinSize = 2;
    };

    class FrameDebuggerPanel
    {
    public:
        explicit FrameDebuggerPanel(EditorContext& ctx) : m_Ctx(ctx) {}
        // `preview`: the picked draw's target as an ImGui texture (0 = none) and that target's size.
        void OnImGui(bool* open, const std::vector<FrameDebugEvent>& events, uint64_t preview, glm::uvec2 previewSize);
        // SceneRenderOptions::FrameDebugStop for a view (0 Scene, 1 Game): -2 unless it is the one being debugged.
        int StopFor(int view) const;
        int View() const { return m_View; }
        // Automation (--open "Frame Debugger" --frame-event N): enabled on the Scene view, stopped at a draw.
        void Start(int event)
        {
            m_Enabled = true;
            m_Event = event;
            m_ScrollTo = true;
        }

    private:
        EditorContext& m_Ctx;
        bool m_Enabled = false;
        int m_View = 0;
        int m_Event = -1; // the last draw shown (-1 all)
        bool m_ScrollTo = false;
        bool m_ShowTarget = true;
    };

    class NavigationPanel
    {
    public:
        explicit NavigationPanel(EditorContext& ctx) : m_Ctx(ctx) {}
        void OnImGui(bool* open);

    private:
        EditorContext& m_Ctx;
    };

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

    // Window > Animation: keys of the selected Animator's skeletal clips (.anim made for its model), joint by joint.
    class AnimationPanel
    {
    public:
        explicit AnimationPanel(EditorContext& ctx) : m_Ctx(ctx) {}
        void OnImGui(bool* open);
        // Puts the previewed Animator back (before play starts or the scene is saved).
        void StopPreview();
        // The Scene view while previewing: the skeleton drawn (click a joint to select it) and a rotation gizmo on the
        // selected joint that keys its rotation at the current time. True when it took the mouse; IsPosing: the
        // object's transform gizmo makes way.
        bool UpdateSceneView(ImVec2 origin, ImVec2 size, const glm::mat4& view, const glm::mat4& proj, bool orthographic, bool hovered);
        bool IsPosing() const;
        // Automation (--anim-key "Joint,rotation|position,axis,key"): that joint selected and that key of its curve.
        void SelectCurveKey(const std::string& spec);

    private:
        EditorContext& m_Ctx;
        AssetHandle m_Clip = AssetHandle::None();
        float m_Time = 0.0f, m_KeyTime = -1.0f; // the current time; the selected key's (-1 = none)
        int m_Joint = -1;              // the selected joint (the one whose values show)
        int m_Shape = -1;              // or the selected blend shape (its weight shows)
        int m_Event = -1;              // or the selected event (index in the clip's)
        bool m_EventDragging = false;
        std::vector<int> m_Selected;   // every selected joint (Ctrl / Shift click adds): keys, deletes and moves apply to all
        bool m_Dragging = false, m_GizmoUsing = false;
        // Preview: the object posed by the clip, and its Animator's settings to put back.
        Entity m_PreviewEntity = Entity::Null();
        AssetHandle m_SavedController = AssetHandle::None();
        std::string m_SavedClip;
        float m_SavedTime = 0.0f;
        bool m_SavedPlaying = true;
        std::vector<float> m_SavedWeights; // its renderer's blend shape weights (the previewed clip sets them)
        // The selected joint's rotation as edited (degrees), kept while it is dragged.
        glm::vec3 m_Euler{ 0.0f };
        int m_EulerJoint = -1;
        float m_EulerTime = -1.0f;
        // The curve view: its selected key (set 0 rotation / 1 position, axis, key; -1 none), the tangent handle being
        // dragged (1 in, 2 out) with the scale held while it is, and whether a click there scrubs the time.
        int m_CurveJoint = -1, m_CurveSet = 0, m_CurveAxis = 0, m_CurveKey = -1, m_CurveDrag = 0;
        AssetHandle m_CurveClip = AssetHandle::None();
        bool m_CurveScrub = false;
        float m_CurveLo[2]{}, m_CurveHi[2]{};
        struct AutoCurveKey { std::string Joint; int Set = 0, Axis = 0, Key = -1; } m_AutoCurve;
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
