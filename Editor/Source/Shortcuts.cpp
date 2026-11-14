#include "Shortcuts.h"
#include <imgui_internal.h>
#include <cstring>

namespace Editor::Shortcuts
{
    namespace
    {
        std::string s_Injected; // Press (automation): reads as pressed once
    }

    void Press(const std::string& id) { s_Injected = id; }

    std::vector<Shortcut>& All()
    {
        static std::vector<Shortcut> shortcuts = [] {
            std::vector<Shortcut> s = {
                { "file.new", "File", "New Scene...", ImGuiMod_Ctrl | ImGuiKey_N },
                { "file.open", "File", "Open Scene", ImGuiMod_Ctrl | ImGuiKey_O },
                { "file.save", "File", "Save Scene", ImGuiMod_Ctrl | ImGuiKey_S },
                { "file.saveAs", "File", "Save Scene As", ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_S },
                { "file.buildSettings", "File", "Build Settings", ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_B },
                { "file.buildAndRun", "File", "Build And Run", ImGuiMod_Ctrl | ImGuiKey_B },
                { "file.buildModule", "File", "Build Game Module", ImGuiMod_Ctrl | ImGuiKey_R },
                { "edit.undo", "Edit", "Undo", ImGuiMod_Ctrl | ImGuiKey_Z, 0, true },
                { "edit.redo", "Edit", "Redo", ImGuiMod_Ctrl | ImGuiKey_Y, 0, true },
                { "edit.duplicate", "Edit", "Duplicate", ImGuiMod_Ctrl | ImGuiKey_D },
                { "edit.copy", "Edit", "Copy", ImGuiMod_Ctrl | ImGuiKey_C },
                { "edit.paste", "Edit", "Paste", ImGuiMod_Ctrl | ImGuiKey_V },
                { "edit.delete", "Edit", "Delete", ImGuiKey_Delete },
                { "edit.group", "Edit", "Group", ImGuiMod_Ctrl | ImGuiKey_G },
                { "edit.ungroup", "Edit", "Ungroup", ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_G },
                { "edit.search", "Edit", "Search", ImGuiMod_Ctrl | ImGuiKey_K },
                { "edit.selectBack", "Edit", "Previous Selection", ImGuiMod_Alt | ImGuiKey_LeftArrow },
                { "edit.selectForward", "Edit", "Next Selection", ImGuiMod_Alt | ImGuiKey_RightArrow },
                { "edit.selectionSet1", "Edit", "Select Selection 1", ImGuiMod_Alt | ImGuiKey_1 },
                { "edit.selectionSet2", "Edit", "Select Selection 2", ImGuiMod_Alt | ImGuiKey_2 },
                { "edit.selectionSet3", "Edit", "Select Selection 3", ImGuiMod_Alt | ImGuiKey_3 },
                { "edit.selectionSet4", "Edit", "Select Selection 4", ImGuiMod_Alt | ImGuiKey_4 },
                { "edit.selectionSet5", "Edit", "Select Selection 5", ImGuiMod_Alt | ImGuiKey_5 },
                { "edit.selectionSet6", "Edit", "Select Selection 6", ImGuiMod_Alt | ImGuiKey_6 },
                { "edit.selectionSet7", "Edit", "Select Selection 7", ImGuiMod_Alt | ImGuiKey_7 },
                { "edit.selectionSet8", "Edit", "Select Selection 8", ImGuiMod_Alt | ImGuiKey_8 },
                { "edit.selectionSet9", "Edit", "Select Selection 9", ImGuiMod_Alt | ImGuiKey_9 },
                { "edit.saveSelectionSet1", "Edit", "Save Selection 1", ImGuiMod_Ctrl | ImGuiMod_Alt | ImGuiKey_1 },
                { "edit.saveSelectionSet2", "Edit", "Save Selection 2", ImGuiMod_Ctrl | ImGuiMod_Alt | ImGuiKey_2 },
                { "edit.saveSelectionSet3", "Edit", "Save Selection 3", ImGuiMod_Ctrl | ImGuiMod_Alt | ImGuiKey_3 },
                { "edit.saveSelectionSet4", "Edit", "Save Selection 4", ImGuiMod_Ctrl | ImGuiMod_Alt | ImGuiKey_4 },
                { "edit.saveSelectionSet5", "Edit", "Save Selection 5", ImGuiMod_Ctrl | ImGuiMod_Alt | ImGuiKey_5 },
                { "edit.saveSelectionSet6", "Edit", "Save Selection 6", ImGuiMod_Ctrl | ImGuiMod_Alt | ImGuiKey_6 },
                { "edit.saveSelectionSet7", "Edit", "Save Selection 7", ImGuiMod_Ctrl | ImGuiMod_Alt | ImGuiKey_7 },
                { "edit.saveSelectionSet8", "Edit", "Save Selection 8", ImGuiMod_Ctrl | ImGuiMod_Alt | ImGuiKey_8 },
                { "edit.saveSelectionSet9", "Edit", "Save Selection 9", ImGuiMod_Ctrl | ImGuiMod_Alt | ImGuiKey_9 },
                { "play.play", "Play", "Play / Stop", ImGuiMod_Ctrl | ImGuiKey_P },
                { "play.pause", "Play", "Pause", ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_P },
                { "scene.hide", "Scene View", "Hide or Show Selection", ImGuiKey_H },
                { "scene.isolate", "Scene View", "Isolate Selection", ImGuiMod_Shift | ImGuiKey_H },
                { "scene.frame", "Scene View", "Frame Selected", ImGuiKey_F },
                { "scene.frameAll", "Scene View", "Frame All", ImGuiMod_Shift | ImGuiKey_F },
                { "scene.moveToView", "Scene View", "Move To View", ImGuiMod_Ctrl | ImGuiMod_Alt | ImGuiKey_F },
                { "scene.alignWithView", "Scene View", "Align With View", ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_F },
                { "scene.alignViewToSelected", "Scene View", "Align View to Selected", 0 },
                { "navigation.bakeAll", "Navigation", "Bake All (saved with the scene)", 0 },
                { "scene.view1", "Scene View", "Go to View 1", ImGuiMod_Ctrl | ImGuiKey_1 },
                { "scene.view2", "Scene View", "Go to View 2", ImGuiMod_Ctrl | ImGuiKey_2 },
                { "scene.view3", "Scene View", "Go to View 3", ImGuiMod_Ctrl | ImGuiKey_3 },
                { "scene.view4", "Scene View", "Go to View 4", ImGuiMod_Ctrl | ImGuiKey_4 },
                { "scene.view5", "Scene View", "Go to View 5", ImGuiMod_Ctrl | ImGuiKey_5 },
                { "scene.view6", "Scene View", "Go to View 6", ImGuiMod_Ctrl | ImGuiKey_6 },
                { "scene.view7", "Scene View", "Go to View 7", ImGuiMod_Ctrl | ImGuiKey_7 },
                { "scene.view8", "Scene View", "Go to View 8", ImGuiMod_Ctrl | ImGuiKey_8 },
                { "scene.view9", "Scene View", "Go to View 9", ImGuiMod_Ctrl | ImGuiKey_9 },
                { "scene.saveView1", "Scene View", "Save View 1", ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_1 },
                { "scene.saveView2", "Scene View", "Save View 2", ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_2 },
                { "scene.saveView3", "Scene View", "Save View 3", ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_3 },
                { "scene.saveView4", "Scene View", "Save View 4", ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_4 },
                { "scene.saveView5", "Scene View", "Save View 5", ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_5 },
                { "scene.saveView6", "Scene View", "Save View 6", ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_6 },
                { "scene.saveView7", "Scene View", "Save View 7", ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_7 },
                { "scene.saveView8", "Scene View", "Save View 8", ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_8 },
                { "scene.saveView9", "Scene View", "Save View 9", ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_9 },
                { "tool.view", "Tools", "View Tool", ImGuiKey_Q },
                { "tool.move", "Tools", "Move Tool", ImGuiKey_W },
                { "tool.rotate", "Tools", "Rotate Tool", ImGuiKey_E },
                { "tool.scale", "Tools", "Scale Tool", ImGuiKey_R },
                { "tool.measure", "Tools", "Measure", ImGuiKey_M },
                // Edit Mode (modeling in the Scene view): Blender's keys. They apply only while editing a mesh.
                { "mesh.editMode", "Mesh Edit", "Enter or Leave Edit Mode", ImGuiKey_Tab },
                { "mesh.vertexMode", "Mesh Edit", "Vertex Select Mode", ImGuiKey_1 },
                { "mesh.edgeMode", "Mesh Edit", "Edge Select Mode", ImGuiKey_2 },
                { "mesh.faceMode", "Mesh Edit", "Face Select Mode", ImGuiKey_3 },
                { "mesh.selectAll", "Mesh Edit", "Select All", ImGuiKey_A },
                { "mesh.deselectAll", "Mesh Edit", "Select None", ImGuiMod_Alt | ImGuiKey_A },
                { "mesh.invert", "Mesh Edit", "Invert Selection", ImGuiMod_Ctrl | ImGuiKey_I },
                { "mesh.selectLinked", "Mesh Edit", "Select Linked", ImGuiMod_Ctrl | ImGuiKey_L },
                { "mesh.linkedPick", "Mesh Edit", "Select Linked Under Mouse", ImGuiKey_L },
                { "mesh.selectMore", "Mesh Edit", "Select More", ImGuiMod_Ctrl | ImGuiKey_Equal },
                { "mesh.selectLess", "Mesh Edit", "Select Less", ImGuiMod_Ctrl | ImGuiKey_Minus },
                { "mesh.grab", "Mesh Edit", "Move", ImGuiKey_G },
                { "mesh.rotate", "Mesh Edit", "Rotate", ImGuiKey_R },
                { "mesh.scale", "Mesh Edit", "Scale", ImGuiKey_S },
                { "mesh.delete", "Mesh Edit", "Delete Menu", ImGuiKey_X },
                { "mesh.merge", "Mesh Edit", "Merge Menu", ImGuiKey_M },
                { "mesh.normals", "Mesh Edit", "Normals Menu", ImGuiMod_Alt | ImGuiKey_N },
                { "mesh.recalcNormals", "Mesh Edit", "Recalculate Normals Outside", ImGuiMod_Shift | ImGuiKey_N },
                { "mesh.add", "Mesh Edit", "Add Mesh Menu", ImGuiMod_Shift | ImGuiKey_A },
                { "mesh.edgeMenu", "Mesh Edit", "Edge Menu", ImGuiMod_Ctrl | ImGuiKey_E },
                { "mesh.hide", "Mesh Edit", "Hide Selected", ImGuiKey_H },
                { "mesh.hideUnselected", "Mesh Edit", "Hide Unselected", ImGuiMod_Shift | ImGuiKey_H },
                { "mesh.reveal", "Mesh Edit", "Reveal Hidden", ImGuiMod_Alt | ImGuiKey_H },
                { "mesh.proportional", "Mesh Edit", "Toggle Proportional Editing", ImGuiKey_O },
                { "mesh.xray", "Mesh Edit", "Toggle X-Ray", ImGuiMod_Alt | ImGuiKey_Z },
            };
            for (Shortcut& x : s) x.Key = x.Default;
            return s;
        }();
        return shortcuts;
    }

    Shortcut* Find(const std::string& id)
    {
        for (Shortcut& s : All())
            if (id == s.Id) return &s;
        return nullptr;
    }

    bool Pressed(const char* id)
    {
        if (!s_Injected.empty() && s_Injected == id)
        {
            s_Injected.clear();
            return true;
        }
        const Shortcut* s = Find(id);
        if (!s || s->Key == 0) return false;
        ImGuiKey key = (ImGuiKey)(s->Key & ~ImGuiMod_Mask_);
        int mods = s->Key & ImGuiMod_Mask_;
        return ImGui::GetIO().KeyMods == mods && ImGui::IsKeyPressed(key, s->Repeat);
    }

    std::string ChordName(ImGuiKeyChord chord) { return chord ? ImGui::GetKeyChordName(chord) : ""; }

    std::string Label(const char* id)
    {
        const Shortcut* s = Find(id);
        return s ? ChordName(s->Key) : std::string();
    }

    std::vector<const Shortcut*> Conflicts(const Shortcut& s)
    {
        std::vector<const Shortcut*> out;
        if (s.Key == 0) return out;
        // Edit Mode's keys replace the object tools' and Scene view's while a mesh is edited, so those never clash.
        auto contextual = [](const Shortcut& x) {
            std::string_view c = x.Category;
            return c == "Tools" || c == "Scene View" || c == "Navigation";
        };
        for (const Shortcut& other : All())
        {
            if (&other == &s || other.Key != s.Key) continue;
            bool meshA = std::string_view(s.Category) == "Mesh Edit", meshB = std::string_view(other.Category) == "Mesh Edit";
            if (meshA != meshB && (contextual(meshA ? other : s))) continue;
            out.push_back(&other);
        }
        return out;
    }

    nlohmann::json Save()
    {
        nlohmann::json changed = nlohmann::json::object();
        for (const Shortcut& s : All())
            if (s.Key != s.Default) changed[s.Id] = s.Key;
        return changed;
    }

    void Load(const nlohmann::json& changed)
    {
        for (Shortcut& s : All()) s.Key = s.Default;
        if (!changed.is_object()) return;
        for (auto& [id, key] : changed.items())
            if (Shortcut* s = Find(id); s && key.is_number_integer()) s->Key = key.get<int>();
    }
}
