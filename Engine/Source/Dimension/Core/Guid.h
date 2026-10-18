#pragma once
#include <cstdint>
#include <functional>

namespace Dimension
{
    // 64-bit random identifier. Used for entity identity across save/load and for
    // asset handles stored in .meta files (like Unity GUIDs). Zero means "none" and
    // values below 1024 are reserved for built-in assets.
    class Guid
    {
    public:
        Guid(); // random
        constexpr explicit Guid(uint64_t value) : m_Value(value) {}
        static constexpr Guid None() { return Guid(uint64_t(0)); }

        constexpr operator uint64_t() const { return m_Value; }
        constexpr bool IsValid() const { return m_Value != 0; }

    private:
        uint64_t m_Value;
    };

    using AssetHandle = Guid;
}

template <> struct std::hash<Dimension::Guid>
{
    size_t operator()(const Dimension::Guid& g) const noexcept { return std::hash<uint64_t>()((uint64_t)g); }
};
