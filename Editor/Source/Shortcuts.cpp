#include "Shortcuts.h"
#include <imgui_internal.h>
#include <cstring>

namespace Editor::Shortcuts
{
    std::vector<Shortcut>& All()
    {
        static std::vector<Shortcut> shortcuts = [] {
            std::vector<Shortcut> s = {
                { "file.new", "File", "New Scene", ImGuiMod_Ctrl | ImGuiKey_N },
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
                { "edit.search", "Edit", "Search", ImGuiMod_Ctrl | ImGuiKey_K },
                { "play.play", "Play", "Play / Stop", ImGuiMod_Ctrl | ImGuiKey_P },
                { "play.pause", "Play", "Pause", ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_P },
                { "scene.hide", "Scene View", "Hide or Show Selection", ImGuiKey_H },
                { "scene.isolate", "Scene View", "Isolate Selection", ImGuiMod_Shift | ImGuiKey_H },
                { "scene.frame", "Scene View", "Frame Selected", ImGuiKey_F },
                { "tool.view", "Tools", "View Tool", ImGuiKey_Q },
                { "tool.move", "Tools", "Move Tool", ImGuiKey_W },
                { "tool.rotate", "Tools", "Rotate Tool", ImGuiKey_E },
                { "tool.scale", "Tools", "Scale Tool", ImGuiKey_R },
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
        for (const Shortcut& other : All())
            if (&other != &s && other.Key == s.Key) out.push_back(&other);
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
