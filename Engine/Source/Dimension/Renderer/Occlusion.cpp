#include "Dimension/Renderer/Occlusion.h"
#include <algorithm>
#include <cmath>

namespace Dimension
{
    bool OccluderDepth::Hides(const glm::vec3& center, const glm::vec3& extents) const
    {
        if (!IsValid()) return false;
        glm::vec2 lo(1e30f), hi(-1e30f);
        float nearest = 1e30f;
        for (int i = 0; i < 8; ++i)
        {
            glm::vec3 corner = center + extents * glm::vec3((i & 1) ? 1.0f : -1.0f, (i & 2) ? 1.0f : -1.0f, (i & 4) ? 1.0f : -1.0f);
            glm::vec4 clip = ViewProj * glm::vec4(corner, 1.0f);
            if (clip.w <= 1e-4f) return false; // reaches behind the camera
            glm::vec3 ndc = glm::vec3(clip) / clip.w;
            lo = glm::min(lo, glm::vec2(ndc));
            hi = glm::max(hi, glm::vec2(ndc));
            nearest = std::min(nearest, ndc.z);
        }
        if (nearest <= 0.0f) return false;
        if (hi.x < -1.0f || lo.x > 1.0f || hi.y < -1.0f || lo.y > 1.0f) return false; // off screen: the frustum decides
        if ((hi.x - lo.x) * (hi.y - lo.y) > 2.0f) return false;                        // over half the view
        // Cells covered, one more all round (the views are drawn with +y up: the top row is ndc y = 1).
        int x0 = std::max((int)std::floor((lo.x * 0.5f + 0.5f) * Width) - 1, 0), x1 = std::min((int)std::floor((hi.x * 0.5f + 0.5f) * Width) + 1, (int)Width - 1);
        int y0 = std::max((int)std::floor((0.5f - hi.y * 0.5f) * Height) - 1, 0), y1 = std::min((int)std::floor((0.5f - lo.y * 0.5f) * Height) + 1, (int)Height - 1);
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x)
                if (Farthest[(size_t)y * Width + x] >= nearest - 1e-5f) return false; // something there is farther: may show
        return true;
    }
}
