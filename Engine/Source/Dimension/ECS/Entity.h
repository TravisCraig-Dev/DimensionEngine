#pragma once
#include <cstdint>
#include <functional>
#include <limits>
#include <typeinfo>

namespace Dimension
{
    // Generational entity handle. The index addresses component storage; the
    // generation detects stale handles after an entity slot is reused.
    struct Entity
    {
        static constexpr uint32_t kInvalidIndex = std::numeric_limits<uint32_t>::max();

        uint32_t Index = kInvalidIndex;
        uint32_t Generation = 0;

        constexpr bool IsNull() const { return Index == kInvalidIndex; }
        constexpr explicit operator bool() const { return !IsNull(); }
        constexpr bool operator==(const Entity&) const = default;
        static constexpr Entity Null() { return {}; }
    };

    using ComponentTypeId = uint32_t;

    namespace Detail
    {
        // Implemented once inside the engine DLL so every module (editor, game DLL, tests) agrees on ids.
        ComponentTypeId ComponentTypeIdFromName(const char* typeName);
    }

    // Dense, process-wide id per component type (0, 1, 2, ...), stable across modules and hot reloads.
    template <class T> ComponentTypeId GetComponentTypeId()
    {
        static const ComponentTypeId id = Detail::ComponentTypeIdFromName(typeid(T).name());
        return id;
    }
}

template <> struct std::hash<Dimension::Entity>
{
    size_t operator()(const Dimension::Entity& e) const noexcept
    {
        return std::hash<uint64_t>()((uint64_t(e.Generation) << 32) | e.Index);
    }
};
