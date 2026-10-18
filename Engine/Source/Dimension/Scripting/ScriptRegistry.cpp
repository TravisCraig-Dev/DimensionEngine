#include "Dimension/Scripting/ScriptRegistry.h"

namespace Dimension
{
    std::deque<ScriptInfo>& ScriptRegistry::Scripts()
    {
        static std::deque<ScriptInfo> scripts;
        return scripts;
    }
}
