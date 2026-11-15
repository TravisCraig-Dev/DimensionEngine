#pragma once
#include <glm/glm.hpp>
#include <nlohmann/json.hpp>
#include <cstdint>
#include <string>

namespace Dimension::Modeling
{
    // Non-destructive modifiers (like Blender's): kept with a model and applied in order whenever it is turned into a
    // render mesh, so the editable mesh stays as it was.
    enum class ModifierType
    {
        Subdivision, // Catmull-Clark subdivision surface (edge creases kept)
        Mirror,      // a mirrored copy across the model's own planes, welded at the plane
        Array,       // copies in a row
        Bevel,       // bevelled edges (all, or those sharper than an angle)
        Solidify,    // thickness: a shell behind the surface joined at its open borders
        Boolean,     // difference, union or intersection with another model
        Decimate,    // fewer triangles (collapsing edges)
        Weld,        // vertices closer than a distance merged
        Displace,    // vertices pushed by noise
        Count
    };
    const char* ModifierTypeName(ModifierType type);

    // One modifier: the settings of every type in one record (only its own type's are used and saved).
    struct Modifier
    {
        ModifierType Type = ModifierType::Subdivision;
        bool Enabled = true;

        // Subdivision
        int Levels = 1;          // 0..6
        bool UseCreases = true;  // edge creases keep edges sharp (1) or partly (0..1)
        // Mirror (axes of the model's own space, through its origin)
        bool Axis[3] = { true, false, false };
        bool Merge = true;            // mirror: weld at the plane; array: weld neighbouring copies
        float MergeDistance = 0.001f; // mirror, array and weld
        // Array
        int Count = 2;
        bool UseRelativeOffset = true;
        glm::vec3 RelativeOffset{ 1.0f, 0.0f, 0.0f }; // times the model's size
        bool UseConstantOffset = false;
        glm::vec3 ConstantOffset{ 0.0f };
        // Bevel
        float Width = 0.1f;
        int Segments = 1;
        float AngleLimit = 30.0f; // degrees: edges whose faces meet sharper than this (0: every edge)
        // Solidify
        float Thickness = 0.1f;
        float Offset = -1.0f; // -1 behind the surface, 0 centered, 1 in front
        bool FillRim = true;
        // Boolean
        int Operation = 0;            // 0 difference, 1 union, 2 intersect
        uint64_t Operand = 0;         // the other model's asset id
        glm::vec3 OperandPosition{ 0.0f }, OperandRotation{ 0.0f } /* degrees */, OperandScale{ 1.0f }; // in this model's space
        // Decimate
        float Ratio = 0.5f;
        // Displace
        float Strength = 0.1f;
        float Midlevel = 0.5f;
        float NoiseSize = 1.0f; // world units per noise cell
        int Direction = 0;      // 0 normal, 1 X, 2 Y, 3 Z
        int Seed = 0;

        static Modifier Make(ModifierType type);
        nlohmann::json ToJson() const;
        static bool FromJson(const nlohmann::json& j, Modifier& out);
        glm::mat4 OperandMatrix() const;
    };
}
