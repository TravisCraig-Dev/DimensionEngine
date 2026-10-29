#include "EditorContext.h"
#include <Dimension/Scene/SceneSerializer.h>
#include <Dimension/Scene/SceneDiff.h>
#include <Dimension/Scene/SceneSearch.h>
#include <imgui.h>

namespace Editor
{
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
