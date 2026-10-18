#include "Dimension/ECS/Entity.h"
#include <mutex>
#include <string>
#include <unordered_map>

namespace Dimension::Detail
{
    ComponentTypeId ComponentTypeIdFromName(const char* typeName)
    {
        static std::mutex mutex;
        static std::unordered_map<std::string, ComponentTypeId> ids;
        std::lock_guard lock(mutex);
        auto [it, inserted] = ids.try_emplace(typeName, (ComponentTypeId)ids.size());
        return it->second;
    }
}
