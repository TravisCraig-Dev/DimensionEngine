#pragma once
#include "Dimension/Scene/Scene.h"
#include <string>
#include <vector>

namespace Dimension
{
    // Hierarchy grouping (Ctrl+G, Ctrl+Shift+G) and sorting, keeping every object where it is in the world.
    namespace SceneGrouping
    {
        // Puts the objects (those whose ancestor is also given count once, with it) under a new empty object at the
        // center of their positions. It goes where the first of them was among its siblings, under their common parent
        // (at the root when they have different ones). Null when no valid object is given.
        Entity Group(Scene& scene, const std::vector<Entity>& objects, const std::string& name = "Group");
        // Moves a group's children up to its parent, where it was among its siblings (in their order), and deletes it
        // when it is an empty object (no components besides its transform); returns the children.
        std::vector<Entity> Ungroup(Scene& scene, Entity group);
        // Whether an object holds nothing but children (Ungroup deletes such an object).
        bool IsEmptyObject(Scene& scene, Entity e);
        // Orders a parent's children (the scene's root objects for Null) by name, ignoring case, with numbers in
        // number order ("Crate 2" before "Crate 10"); with `recursive`, their children too.
        void SortChildren(Scene& scene, Entity parent, bool recursive = false);
        // The name order SortChildren uses: true when `a` goes before `b`.
        bool NameBefore(const std::string& a, const std::string& b);
    }
}
