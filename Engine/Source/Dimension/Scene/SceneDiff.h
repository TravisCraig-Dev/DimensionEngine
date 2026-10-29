#pragma once
#include <nlohmann/json.hpp>
#include <string>

namespace Dimension
{
    // What changed between two serialized states of a scene, said the way Unity names undo steps: "Create Cube",
    // "Delete Wall A", "Rename Crate to Box", "Add Point Light to Lamp", "Wall A: Transform (Position)", "Transform on
    // 3 objects", "Scene Settings", "Reorder objects", or "No change". The editor's Undo History names its steps with it.
    namespace SceneDiff
    {
        std::string Describe(const nlohmann::json& before, const nlohmann::json& after);
    }
}
