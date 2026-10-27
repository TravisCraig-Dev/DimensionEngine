#pragma once
#include "Dimension/Scene/Scene.h"
#include <string>
#include <vector>

namespace Dimension
{
    // Hierarchy search (like Unity's): the query's words must all match. A plain word is part of the object's name;
    // "t:Type" names a component (spaces ignored: t:MeshRenderer finds Mesh Renderer) or a script class the object
    // has; "tag:Name" is its tag. Case does not matter. Matches come in hierarchy order (depth first).
    namespace SceneSearch
    {
        std::vector<Entity> Find(Scene& scene, const std::string& query);
        bool Matches(Scene& scene, Entity e, const std::string& query);
    }
}
