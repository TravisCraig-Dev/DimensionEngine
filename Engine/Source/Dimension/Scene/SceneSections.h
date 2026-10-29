#pragma once
#include "Dimension/Scene/Scene.h"
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_set>
#include <vector>

namespace Dimension
{
    // Several scene files open in one Scene (the editor's multi-scene editing, like Unity's): the main scene (section
    // 0: the scene's own name, settings and file, where new objects go) and added scenes, each owning the root objects
    // it brought (by GUID; children belong to their root's scene). Each saves to its own file, with its own name and
    // settings, and only its objects.
    class SceneSections
    {
    public:
        struct Section
        {
            std::string Name;
            fs::path Path;
            nlohmann::json Settings;           // its file's settings (only the main scene's apply in the editor)
            std::unordered_set<uint64_t> Roots; // its root objects' GUIDs
            std::string Saved;                 // its file's content when opened or last saved (for IsDirty)
        };

        // Adds a scene file's objects to `scene` as a new section (a file already open, or unreadable: false).
        bool Open(Scene& scene, const fs::path& path);
        // Removes added scene `section` (1-based) and its objects.
        void Close(Scene& scene, size_t section);
        void Clear() { m_Added.clear(); }

        const std::vector<Section>& Added() const { return m_Added; }
        size_t Count() const { return m_Added.size() + 1; } // with the main scene
        // The section an object belongs to (its root's): 0 the main scene, n the n-th added one.
        size_t SectionOf(Scene& scene, Entity e) const;
        // A section's root objects, in the hierarchy's order.
        std::vector<Entity> Roots(Scene& scene, size_t section) const;
        // Moves a root object (with its children) into a section.
        void MoveRoot(Scene& scene, Entity root, size_t section);
        // A section as its file holds it; the main scene's with the scene's name and settings.
        nlohmann::json Serialize(Scene& scene, size_t section) const;
        // Writes an added scene to its file (the main scene saves through the editor, as before).
        bool Save(Scene& scene, size_t section);
        // Whether an added scene differs from its file.
        bool IsDirty(Scene& scene, size_t section) const;
        // The added scenes' names and roots, for a copy of the scene that plays (Scene::RegisterLoadedScene).
        void RegisterLoaded(Scene& playing) const;

    private:
        std::vector<Section> m_Added;
    };
}
