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
