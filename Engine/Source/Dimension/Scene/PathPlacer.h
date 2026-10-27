#pragma once
#include "Dimension/Scene/Scene.h"

namespace Dimension
{
    // Path placement (like Unity's Spline Instantiate): see PathPlacerComponent.
    namespace PathPlacer
    {
        // Replaces the object's children with copies of its Prefab (or Source object) every Spacing meters along its
        // path, turned and scattered as its settings say. Returns how many it placed.
        int Place(Scene& scene, Entity placer);
        // Removes the placed copies (the object's children).
        void Clear(Scene& scene, Entity placer);
        // Where the copies go (world space): position and the path's direction there, one per copy.
        struct Spot { glm::vec3 Position, Direction; };
        std::vector<Spot> Spots(Scene& scene, Entity placer);
    }
}
