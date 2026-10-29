#pragma once
#include <Dimension/Scene/Scene.h>
#include <Dimension/Scene/SceneVisibility.h>
#include <Dimension/Scene/SceneSections.h>
#include <Dimension/Asset/AssetManager.h>
#include <Dimension/Terrain/Terrain.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <deque>
#include <optional>

namespace Editor
{
    using namespace Dimension;
    class AssetPreviews;

    enum class PlayState { Edit, Play, Paused };

    // State shared by all editor panels.
    class EditorContext
    {
    public:
        // Scene currently shown/edited: the runtime copy while playing.
        Scene& ActiveScene() { return RuntimeScene ? *RuntimeScene : *EditorScene; }
        // Material pictures for the Inspector and the Project window (owned by the editor layer).
        AssetPreviews* Previews = nullptr;
        bool OpenSpriteEditor = false; // a panel asks for the Sprite Editor on the selected texture
        bool IsPlaying() const { return State != PlayState::Edit; }

        // Selection (by entity handle; survives play mode because clones keep handles).
        // `Selected` is the primary (last clicked) entity; `Selection` holds all selected entities.
        Entity Selected = Entity::Null();
        std::vector<Entity> Selection;
        // The Hierarchy's search (the Scene view dims everything it does not match).
        std::string HierarchySearch;
        AssetHandle SelectedAsset = AssetHandle::None();
        void Select(Entity e)
        {
            Selected = e;
            Selection.clear();
            if (!e.IsNull())
            {
                Selection.push_back(e);
                SelectedAsset = AssetHandle::None();
            }
        }
        void ToggleSelect(Entity e)
        {
            if (e.IsNull()) return;
            auto it = std::find(Selection.begin(), Selection.end(), e);
            if (it != Selection.end())
            {
                Selection.erase(it);
                Selected = Selection.empty() ? Entity::Null() : Selection.back();
            }
            else
            {
                Selection.push_back(e);
                Selected = e;
                SelectedAsset = AssetHandle::None();
            }
        }
        void AddToSelection(Entity e)
        {
            if (e.IsNull()) return;
            if (std::find(Selection.begin(), Selection.end(), e) == Selection.end()) Selection.push_back(e);
            Selected = e;
            SelectedAsset = AssetHandle::None();
        }
        bool IsSelected(Entity e) const { return std::find(Selection.begin(), Selection.end(), e) != Selection.end(); }
        // Valid selected entities, excluding any whose ancestor is also selected (for delete/duplicate/copy).
        std::vector<Entity> SelectedRoots()
        {
            Scene& scene = ActiveScene();
            std::vector<Entity> roots;
            for (Entity e : Selection)
            {
                if (!scene.IsValid(e)) continue;
                bool nested = false;
                for (Entity other : Selection)
                    if (other != e && scene.IsValid(other) && scene.IsDescendantOf(e, other)) nested = true;
                if (!nested) roots.push_back(e);
            }
            return roots;
        }
        void SelectAsset(AssetHandle a)
        {
            SelectedAsset = a;
            Selected = Entity::Null();
            Selection.clear();
        }
        bool HasValidSelection() { return !Selected.IsNull() && ActiveScene().IsValid(Selected); }

        // Undo/redo via full scene snapshots. Call RecordUndo() *before* a change.
        // Edits during play mode are not recorded (they are discarded on Stop, like Unity). A step is named after what
        // it changed (SceneDiff), or by `name` when given.
        void RecordUndo(const char* name = nullptr);
        // The Undo History: the steps done (oldest first), then the ones undone (next to redo first).
        struct HistoryStep
        {
            std::string Name;
            bool Undone = false;
        };
        std::vector<HistoryStep> History();
        size_t HistoryDone() const { return m_UndoStack.size(); }
        // Undoes or redoes until `done` steps are done (0: all undone).
        void JumpTo(size_t done);
        void Undo();
        void Redo();
        bool CanUndo() const { return !m_UndoStack.empty(); }
        bool CanRedo() const { return !m_RedoStack.empty(); }
        void ClearHistory()
        {
            m_UndoStack.clear();
            m_RedoStack.clear();
        }
        void MarkDirty()
        {
            if (!IsPlaying()) Dirty = true;
        }

        Ref<Scene> EditorScene;
        Ref<Scene> RuntimeScene;
        PlayState State = PlayState::Edit;
        fs::path ScenePath; // empty until saved
        bool Dirty = false;
        // Scene visibility (the Hierarchy's eye and hand, H): saved per scene in .dimension/SceneVisibility.json when
        // VisibilityChanged is set.
        SceneVisibility Visibility;
        bool VisibilityChanged = false;
        // Scenes open alongside the main one (File > Open Scene Additive...): a Hierarchy section each, saved to their
        // own files with the main scene.
        SceneSections Sections;
        std::optional<fs::path> RequestOpenAdditive;
        int RequestCloseSection = 0, RequestSaveSection = 0; // 1-based (0: none)
        // Prefab Mode in context: the scene around the instance it was opened from, a hidden container in the stage
        // (drawn greyed and not picked; never saved with the prefab).
        Entity PrefabContext = Entity::Null();
        bool PrefabContextShown = true;
        std::optional<uint64_t> RequestOpenPrefabFrom; // with RequestOpenPrefab: the instance's GUID in the scene

        // Prefab Mode: EditorScene is the prefab alone; the scene being edited (with its path, dirty flag,
        // undo history and selection) waits in the stage and comes back on exit.
        bool InPrefabMode() const { return m_Stage.has_value(); }
        AssetHandle PrefabModeAsset() const { return m_Stage ? m_Stage->Prefab : AssetHandle::None(); }
        const std::string& PrefabModeSceneName() const { return m_Stage->SceneName; }
        void EnterPrefabMode(AssetHandle prefab, Ref<Scene> stageScene);
        void ExitPrefabMode();
        bool PrefabAutoSave = true;

        // Terrain brush (Inspector picks it, the Scene view paints with it): 0 = off, else Terrain::BrushMode + 1.
        int TerrainTool = 0;
        float TerrainBrushSize = 6.0f, TerrainBrushStrength = 0.5f;
        int TerrainPaintLayer = 0; // the Paint tool's layer (its place in the terrain's Layers)
        Terrain::StampSettings TerrainStamp; // the Stamp tool's image, height and turn

        // Deferred requests handled by the EditorLayer at a safe point in the frame.
        std::optional<AssetHandle> RequestOpenPrefab;
        std::optional<fs::path> RequestOpenScene;
        std::optional<glm::vec3> RequestFocus; // frame point in scene view
        bool RequestFocusSelected = false;
        // The Project window's Export Package... (an asset or folder, absolute) and Import Package...
        std::optional<fs::path> RequestExportPackage;
        bool RequestImportPackage = false;

    private:
        struct Snapshot
        {
            nlohmann::json Scene;
            uint64_t SelectedGuid;
            std::vector<uint64_t> SelectionGuids;
            // The step from this state (undo) or to it (redo); Named once known for good.
            std::string Name = {};
            bool Named = false;
        };
        Snapshot Capture();
        void Restore(const Snapshot& s);

        std::deque<Snapshot> m_UndoStack, m_RedoStack;
        static constexpr size_t kMaxHistory = 100;

        struct PrefabStage
        {
            AssetHandle Prefab;
            Ref<Dimension::Scene> Scene;
            std::string SceneName;
            fs::path ScenePath;
            bool Dirty;
            std::deque<Snapshot> UndoStack, RedoStack;
            std::vector<uint64_t> SelectionGuids;
            uint64_t SelectedGuid;
        };
        std::optional<PrefabStage> m_Stage;
    };
}
