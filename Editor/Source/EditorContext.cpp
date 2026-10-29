#include "EditorContext.h"
#include <Dimension/Scene/SceneSerializer.h>

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

    void EditorContext::RecordUndo()
    {
        if (IsPlaying()) return;
        m_UndoStack.push_back(Capture());
        if (m_UndoStack.size() > kMaxHistory) m_UndoStack.pop_front();
        m_RedoStack.clear();
        Dirty = true;
    }

    void EditorContext::Undo()
    {
        if (IsPlaying() || m_UndoStack.empty()) return;
        m_RedoStack.push_back(Capture());
        Snapshot s = std::move(m_UndoStack.back());
        m_UndoStack.pop_back();
        Restore(s);
    }

    void EditorContext::Redo()
    {
        if (IsPlaying() || m_RedoStack.empty()) return;
        m_UndoStack.push_back(Capture());
        Snapshot s = std::move(m_RedoStack.back());
        m_RedoStack.pop_back();
        Restore(s);
    }
}
