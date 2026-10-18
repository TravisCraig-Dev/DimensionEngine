#include "Dimension/Core/Guid.h"
#include <random>

namespace Dimension
{
    static uint64_t RandomU64()
    {
        thread_local std::mt19937_64 engine(std::random_device{}());
        uint64_t v;
        do { v = engine(); } while (v < 1024);
        return v;
    }

    Guid::Guid() : m_Value(RandomU64()) {}
}
