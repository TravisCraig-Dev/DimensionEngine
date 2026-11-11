#include "EditorContext.h"
#include <Dimension/Scene/SceneSerializer.h>
#include <Dimension/Scene/SceneDiff.h>
#include <Dimension/Scene/SceneSearch.h>
#include <Dimension/Navigation/NavMesh.h>
#include <Dimension/Asset/AssetManager.h>
#include <Dimension/Core/Log.h>
#include <imgui.h>

namespace Editor
{
    void EditorContext::BakeNavMesh(Entity surface)
    {
        Scene& scene = ActiveScene();
        Navigation::Bake(scene, surface);
        if (IsPlaying()) return; // (a look at it: play keeps nothing)
        auto* s = scene.GetRegistry().TryGet<NavMeshSurfaceComponent>(surface);
        if (!s || s->Streamed) return; // streamed surfaces bake their tiles while playing
        if (ScenePath.empty())
        {
            Log::Warn("Save the scene to keep its navigation bake (play bakes it until then)");
            return;
        }
        AssetHandle before = s->BakedData;
        if (Navigation::SaveBaked(scene, surface, ScenePath))
        {
            if (s->BakedData != before) MarkDirty();
            Log::Info("Saved the navigation bake of '{}' to {}", scene.GetRegistry().Get<TagComponent>(surface).Name,
                      AssetManager::GetDisplayName(s->BakedData));
        }
    }

    void EditorContext::ClearNavMesh(Entity surface)
    {
        Scene& scene = ActiveScene();
        auto* s = scene.GetRegistry().TryGet<NavMeshSurfaceComponent>(surface);
        if (!s) return;
        s->Data.reset();
        s->Baked.reset();
        if (IsPlaying() || !s->BakedData.IsValid()) return;
        const AssetMetadata* meta = AssetManager::GetMetadata(s->BakedData);
        if (meta && meta->Type == AssetType::NavMeshData) AssetManager::DeleteAsset(AssetManager::GetAbsolutePath(s->BakedData));
        s->BakedData = AssetHandle::None();
        MarkDirty();
    }

    EditorContext::Snapshot EditorContext::Capture()
    {
        uint64_t guid = HasValidSelection() ? (uint64_t)EditorScene->GetGuid(Selected) : 0;
        std::vector<uint64_t> all;
        for (Entity e : Selection)
            if (EditorScene->IsValid(e)) all.push_back(EditorScene->GetGuid(e));
        return { SceneSerializer::Serialize(*EditorScene), guid, all };
    }

    void EditorContext::Restore(const Snapshot& s)
    {
        EditorScene = SceneSerializer::Deserialize(s.Scene);
        Selected = s.SelectedGuid ? EditorScene->FindEntityByGuid(Guid(s.SelectedGuid)) : Entity::Null();
        Selection.clear();
        for (uint64_t g : s.SelectionGuids)
            if (Entity e = EditorScene->FindEntityByGuid(Guid(g)); !e.IsNull()) Selection.push_back(e);
        if (!Selected.IsNull() && !IsSelected(Selected)) Selection.push_back(Selected);
        Dirty = true;
    }

    const std::vector<Entity>& EditorContext::SearchMatches()
    {
        Scene& scene = ActiveScene();
        int frame = ImGui::GetCurrentContext() ? ImGui::GetFrameCount() : -2;
        if (frame != m_SearchMatchesFrame || frame == -2 || &scene != m_SearchMatchesScene || HierarchySearch != m_SearchMatchesQuery)
        {
            m_SearchMatches = SceneSearch::Find(scene, HierarchySearch);
            m_SearchMatchesFrame = frame;
            m_SearchMatchesScene = &scene;
            m_SearchMatchesQuery = HierarchySearch;
        }
        return m_SearchMatches;
    }

    SelectionSnapshot EditorContext::CaptureSelection()
    {
        // Objects by GUID (the primary one too); without objects, the selected assets.
        Scene& scene = ActiveScene();
        SelectionSnapshot s;
        for (Entity e : Selection)
            if (scene.IsValid(e)) s.Objects.push_back((uint64_t)scene.GetGuid(e));
        if (HasValidSelection()) s.Primary = (uint64_t)scene.GetGuid(Selected);
        if (s.Objects.empty())
        {
            for (AssetHandle a : SelectedAssetList()) s.Assets.push_back((uint64_t)a);
            s.PrimaryAsset = s.Assets.empty() ? 0 : (uint64_t)SelectedAsset;
        }
        return s;
    }

    bool EditorContext::CanApplySelection(const SelectionSnapshot& s)
    {
        Scene& scene = ActiveScene();
        for (uint64_t g : s.Objects)
            if (!scene.FindEntityByGuid(Guid(g)).IsNull()) return true;
        for (uint64_t a : s.Assets)
            if (AssetManager::GetMetadata(Guid(a))) return true;
        return false;
    }

    bool EditorContext::ApplySelection(const SelectionSnapshot& s)
    {
        // What still exists of it (objects deleted since are left out).
        Scene& scene = ActiveScene();
        if (!s.Objects.empty())
        {
            std::vector<Entity> found;
            for (uint64_t g : s.Objects)
                if (Entity e = scene.FindEntityByGuid(Guid(g)); !e.IsNull()) found.push_back(e);
            if (found.empty()) return false;
            Select(Entity::Null());
            for (Entity e : found) AddToSelection(e);
            if (Entity p = scene.FindEntityByGuid(Guid(s.Primary)); !p.IsNull() && IsSelected(p)) AddToSelection(p); // primary again
            return true;
        }
        std::vector<AssetHandle> assets;
        for (uint64_t a : s.Assets)
            if (AssetManager::GetMetadata(Guid(a))) assets.push_back(Guid(a));
        if (assets.empty()) return false;
        AssetHandle primary = std::find(assets.begin(), assets.end(), Guid(s.PrimaryAsset)) != assets.end() ? AssetHandle(Guid(s.PrimaryAsset)) : assets.back();
        SelectAssets(assets, primary);
        return true;
    }

    void EditorContext::RestoreSelection(const SelectionSnapshot& s)
    {
        if (ApplySelection(s)) return;
        Select(Entity::Null());
        ClearAssetSelection();
    }

    std::vector<EditorContext::HistoryStep> EditorContext::History()
    {
        std::vector<HistoryStep> out;
        for (size_t i = 0; i < m_UndoStack.size(); ++i)
        {
            Snapshot& s = m_UndoStack[i];
            if (!s.Named && i + 1 < m_UndoStack.size())
            {
                s.Name = SceneDiff::Describe(s.Scene, m_UndoStack[i + 1].Scene);
                s.Named = true;
            }
            // The last step's result is the scene as it is now (still changing: not kept).
            std::string name = s.Named ? s.Name : SceneDiff::Describe(s.Scene, SceneSerializer::Serialize(*EditorScene));
            out.push_back({ name, false });
        }
        for (auto it = m_RedoStack.rbegin(); it != m_RedoStack.rend(); ++it) out.push_back({ it->Name, true });
        return out;
    }

    void EditorContext::JumpTo(size_t done)
    {
        while (m_UndoStack.size() > done && !m_UndoStack.empty()) Undo();
        while (m_UndoStack.size() < done && !m_RedoStack.empty()) Redo();
    }

    void EditorContext::EnterPrefabMode(AssetHandle prefab, Ref<Scene> stageScene)
    {
        Snapshot selection = { {}, HasValidSelection() ? (uint64_t)EditorScene->GetGuid(Selected) : 0, {} };
        for (Entity e : Selection)
            if (EditorScene->IsValid(e)) selection.SelectionGuids.push_back(EditorScene->GetGuid(e));
        m_Stage = PrefabStage{ prefab, EditorScene, EditorScene->GetName(), ScenePath, Dirty,
                               std::move(m_UndoStack), std::move(m_RedoStack), selection.SelectionGuids, selection.SelectedGuid };
        EditorScene = std::move(stageScene);
        ScenePath.clear();
        Dirty = false;
        ClearHistory();
        Select(Entity::Null());
    }

    void EditorContext::ExitPrefabMode()
    {
        if (!m_Stage) return;
        PrefabStage stage = std::move(*m_Stage);
        m_Stage.reset();
        PrefabContext = Entity::Null();
        EditorScene = stage.Scene;
        ScenePath = stage.ScenePath;
        Dirty = stage.Dirty;
        m_UndoStack = std::move(stage.UndoStack);
        m_RedoStack = std::move(stage.RedoStack);
        Select(Entity::Null());
        for (uint64_t g : stage.SelectionGuids)
            if (Entity e = EditorScene->FindEntityByGuid(Guid(g)); !e.IsNull()) AddToSelection(e);
        if (stage.SelectedGuid)
            if (Entity e = EditorScene->FindEntityByGuid(Guid(stage.SelectedGuid)); !e.IsNull()) Selected = e;
    }

    void EditorContext::RecordUndo(const char* name)
    {
        if (IsPlaying()) return;
        Snapshot now = Capture();
        // The step before this one is now complete: name it while both of its states are at hand.
        if (!m_UndoStack.empty() && !m_UndoStack.back().Named)
        {
            m_UndoStack.back().Name = SceneDiff::Describe(m_UndoStack.back().Scene, now.Scene);
            m_UndoStack.back().Named = true;
        }
        m_UndoStack.push_back(std::move(now));
        if (name && *name)
        {
            m_UndoStack.back().Name = name;
            m_UndoStack.back().Named = true;
        }
        if (m_UndoStack.size() > kMaxHistory) m_UndoStack.pop_front();
        m_RedoStack.clear();
        Dirty = true;
    }

    void EditorContext::Undo()
    {
        if (IsPlaying() || m_UndoStack.empty()) return;
        Snapshot current = Capture();
        Snapshot s = std::move(m_UndoStack.back());
        m_UndoStack.pop_back();
        // Redo does the same step again.
        current.Name = s.Named ? s.Name : SceneDiff::Describe(s.Scene, current.Scene);
        current.Named = true;
        m_RedoStack.push_back(std::move(current));
        Restore(s);
    }

    void EditorContext::Redo()
    {
        if (IsPlaying() || m_RedoStack.empty()) return;
        Snapshot s = std::move(m_RedoStack.back());
        m_RedoStack.pop_back();
        Snapshot current = Capture();
        current.Name = s.Name;
        current.Named = true;
        m_UndoStack.push_back(std::move(current));
        Restore(s);
    }
}
