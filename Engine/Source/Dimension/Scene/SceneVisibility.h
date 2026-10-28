#pragma once
#include "Dimension/Scene/Scene.h"
#include <nlohmann/json.hpp>
#include <unordered_set>
#include <vector>

namespace Dimension
{
    // Scene visibility (the editor's eye and hand in the Hierarchy, like Unity's): objects hidden in the Scene view, and
    // objects clicks there do not pick; an object's children follow it. Only the editor's Scene view: the game, the
    // Game view and play mode's own rules are unaffected. Kept by object GUID, so it holds across play mode, undo and
    // reloading the scene.
    class SceneVisibility
    {
    public:
        // The Scene view's toggle: off shows and picks everything (the settings stay).
        bool Enabled = true;

        void SetHidden(Scene& scene, Entity e, bool hidden);
        void SetPickable(Scene& scene, Entity e, bool pickable);
        // Its own setting, and whether it or an ancestor is hidden / unpickable (what applies).
        bool IsHiddenSelf(Scene& scene, Entity e) const;
        bool IsUnpickableSelf(Scene& scene, Entity e) const;
        bool IsHidden(Scene& scene, Entity e) const;
        bool IsUnpickable(Scene& scene, Entity e) const;
        // Every object that applies to (children included), for the renderer; empty while not Enabled.
        std::vector<Entity> Hidden(Scene& scene) const;
        std::vector<Entity> Unpickable(Scene& scene) const;
        bool Empty() const { return m_Hidden.empty() && m_Unpickable.empty(); }
        void Clear();

        nlohmann::json ToJson() const;
        void FromJson(const nlohmann::json& j);
        // The settings of one scene (`key`: its path) in a file of every scene's.
        void Load(const fs::path& file, const std::string& key);
        void Save(const fs::path& file, const std::string& key) const;

    private:
        bool Applies(Scene& scene, Entity e, const std::unordered_set<uint64_t>& set) const;
        std::vector<Entity> Expand(Scene& scene, const std::unordered_set<uint64_t>& set) const;
        std::unordered_set<uint64_t> m_Hidden, m_Unpickable; // object GUIDs
    };
}
