#include "Dimension/Scene/System.h"

namespace Dimension
{
    std::deque<SystemRegistry::Entry>& SystemRegistry::Entries()
    {
        static std::deque<Entry> entries;
        return entries;
    }
}
