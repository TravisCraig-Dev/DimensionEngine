#pragma once
#include "Dimension/Reflection/TypeInfo.h"
#include <string>
#include <vector>

namespace Dimension
{
    // The Inspector's search box: which components and fields show for a query.
    namespace FieldSearch
    {
        // Every word of the query starts where a word of the text does (case ignored; "int" finds Intensity, not the end of
        // "Point"; "pointlight" finds "Point Light", "speed" finds "MaxSpeed").
        bool Matches(const std::string& query, const std::string& text);
        // A field shows when the query matches its component's name with its own ("light int" finds a Point Light's
        // Intensity); hidden fields never do.
        bool FieldShown(const std::string& query, const std::string& component, const FieldInfo& field);
        // A component shows when its own name matches (all its fields shown) or one of its fields does.
        bool ComponentNameMatches(const std::string& query, const std::string& component);
        bool ComponentShown(const std::string& query, const std::string& component, const std::vector<FieldInfo>& fields);
    }
}
