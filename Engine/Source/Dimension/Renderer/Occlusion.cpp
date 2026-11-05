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

    OccluderDepth OccluderDepth::Reprojected(const glm::mat4& viewProj) const
    {
        OccluderDepth out;
        out.ViewProj = viewProj;
        if (!IsValid()) return out;
        out.Width = Width;
        out.Height = Height;
        out.Farthest.assign(Farthest.size(), -1.0f); // -1: no quad yet
        // The old view's NDC straight to the new view's clip space. It is linear, so a cell's other corners are its
        // first plus a step along x and y (plain floats: this runs every frame the camera moves, Debug builds too).
        const glm::mat4 m = viewProj * glm::inverse(ViewProj);
        const float* e = &m[0][0];
        const float sx = 2.0f / (float)Width, sy = 2.0f / (float)Height;
        const float w = (float)Width, h = (float)Height;
        const float stepX[4] = { e[0] * sx, e[1] * sx, e[2] * sx, e[3] * sx }, stepY[4] = { -e[4] * sy, -e[5] * sy, -e[6] * sy, -e[7] * sy };
        float* cells = out.Farthest.data();
        const int width = (int)Width, height = (int)Height;
        for (int cy = 0; cy < height; ++cy)
        {
            const float y0 = 1.0f - sy * (float)cy; // row 0 at the top: ndc y = 1
            for (int cx = 0; cx < width; ++cx)
            {
                float depth = Farthest[(size_t)cy * Width + cx];
                if (depth >= 1.0f) continue; // nothing drawn there
                const float x0 = -1.0f + sx * (float)cx;
                float c[4][4];
                for (int k = 0; k < 4; ++k) c[0][k] = e[k] * x0 + e[4 + k] * y0 + e[8 + k] * depth + e[12 + k];
                for (int k = 0; k < 4; ++k)
                {
                    c[1][k] = c[0][k] + stepX[k];
                    c[2][k] = c[1][k] + stepY[k];
                    c[3][k] = c[0][k] + stepY[k];
                }
                // The corners in the new grid (cells), and the farthest new depth.
                float px[4], py[4], farthest = -1.0f;
                bool behind = false;
                for (int i = 0; i < 4 && !behind; ++i)
                {
                    if (c[i][3] <= 1e-4f)
                    {
                        behind = true;
                        break;
                    }
                    float inv = 1.0f / c[i][3];
                    px[i] = (c[i][0] * inv * 0.5f + 0.5f) * w;
                    py[i] = (0.5f - c[i][1] * inv * 0.5f) * h;
                    float z = c[i][2] * inv;
                    if (z > farthest) farthest = z;
                }
                if (behind || farthest <= 0.0f) continue;
                if (farthest > 1.0f) farthest = 1.0f;
                float minX = px[0], maxX = px[0], minY = py[0], maxY = py[0];
                for (int i = 1; i < 4; ++i)
                {
                    minX = px[i] < minX ? px[i] : minX;
                    maxX = px[i] > maxX ? px[i] : maxX;
                    minY = py[i] < minY ? py[i] : minY;
                    maxY = py[i] > maxY ? py[i] : maxY;
                }
                int ix0 = (int)std::ceil(minX - 0.5f), ix1 = (int)std::floor(maxX - 0.5f);
                int iy0 = (int)std::ceil(minY - 0.5f), iy1 = (int)std::floor(maxY - 0.5f);
                if (ix0 < 0) ix0 = 0;
                if (iy0 < 0) iy0 = 0;
                if (ix1 > width - 1) ix1 = width - 1;
                if (iy1 > height - 1) iy1 = height - 1;
                if (ix0 > ix1 || iy0 > iy1) continue;
                // New cells whose centers lie inside the (convex) quad: on the same side of all four edges.
                float ex[4], ey[4];
                for (int i = 0; i < 4; ++i)
                {
                    ex[i] = px[(i + 1) & 3] - px[i];
                    ey[i] = py[(i + 1) & 3] - py[i];
                }
                for (int y = iy0; y <= iy1; ++y)
                {
                    float fy = (float)y + 0.5f;
                    for (int x = ix0; x <= ix1; ++x)
                    {
                        float fx = (float)x + 0.5f;
                        bool positive = true, negative = true;
                        for (int i = 0; i < 4; ++i)
                        {
                            float d = ex[i] * (fy - py[i]) - ey[i] * (fx - px[i]);
                            positive = positive && d >= 0.0f;
                            negative = negative && d <= 0.0f;
                        }
                        if (!positive && !negative) continue;
                        float& cell = cells[(size_t)y * width + x];
                        if (farthest > cell) cell = farthest;
                    }
                }
            }
        }
        for (float& cell : out.Farthest)
            if (cell < 0.0f) cell = 1.0f; // no quad reached it: anything may show there
        return out;
    }
}
