#include "Dimension/Scene/DebugDraw.h"
#include <algorithm>
#include <mutex>

namespace Dimension::DebugDraw
{
    namespace
    {
        constexpr size_t kMaxLines = 100000; // a script drawing every frame without a duration stays bounded anyway
        std::vector<DebugDraw::DebugLine> s_Lines;
        std::mutex s_Mutex;
    }

    void Line(const glm::vec3& a, const glm::vec3& b, const glm::vec4& color, float duration)
    {
        std::lock_guard lock(s_Mutex);
        if (s_Lines.size() >= kMaxLines) return;
        s_Lines.push_back({ a, b, color, std::max(duration, 0.0f) });
    }

    void Ray(const glm::vec3& origin, const glm::vec3& direction, const glm::vec4& color, float duration)
    {
        Line(origin, origin + direction, color, duration);
    }

    void Tick(float dt)
    {
        std::lock_guard lock(s_Mutex);
        std::erase_if(s_Lines, [&](DebugDraw::DebugLine& l) {
            if (l.Remaining <= 0.0f) return true;
            l.Remaining -= dt;
            return l.Remaining <= 0.0f;
        });
    }

    void Clear()
    {
        std::lock_guard lock(s_Mutex);
        s_Lines.clear();
    }

    const std::vector<DebugDraw::DebugLine>& Lines() { return s_Lines; }
}
