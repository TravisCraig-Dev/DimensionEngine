#pragma once
#include <imgui.h>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace Editor
{
    // The editor's keyboard shortcuts: each command's key (with Ctrl, Shift or Alt), rebound in Window > Shortcuts and
    // kept with the editor preferences (only the ones changed from their defaults).
    struct Shortcut
    {
        const char* Id;
        const char* Category;
        const char* Name;
        ImGuiKeyChord Default;
        ImGuiKeyChord Key = 0; // 0: none
        bool Repeat = false;   // fires again while held (Undo, Redo)
    };

    namespace Shortcuts
    {
        std::vector<Shortcut>& All();
        Shortcut* Find(const std::string& id);
        // Pressed this frame with exactly its modifiers (Ctrl+Shift+S does not count as Ctrl+S).
        bool Pressed(const char* id);
        // Makes the next Pressed(id) true once, as if its keys were pressed (automation: --press).
        void Press(const std::string& id);
        // "Ctrl+Shift+S" ("" for none), for menus and the Shortcuts window.
        std::string Label(const char* id);
        std::string ChordName(ImGuiKeyChord chord);
        // Whether a shortcut outside Edit Mode shares its keys with an Edit Mode one (Edit Mode's wins while editing).
        bool ShadowedInEditMode(const char* id);
        // The shortcuts bound to the same keys as `s`.
        std::vector<const Shortcut*> Conflicts(const Shortcut& s);
        nlohmann::json Save();
        void Load(const nlohmann::json& changed);
    }
}
