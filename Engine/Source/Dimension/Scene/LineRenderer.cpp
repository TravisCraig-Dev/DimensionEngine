#include "Dimension/Scene/LineRenderer.h"
#include "Dimension/Scene/Scene.h"
#include "Dimension/Scene/Components.h"
#include <algorithm>

namespace Dimension
{
    std::vector<RibbonQuad> BuildRibbon(const std::vector<RibbonPoint>& input, const RibbonParams& params)
    {
        std::vector<RibbonPoint> pts;
        pts.reserve(input.size() + 1);
        for (const auto& p : input)
            if (pts.empty() || glm::distance(pts.back().Position, p.Position) > 1e-5f) pts.push_back(p);
        bool loop = params.Loop && pts.size() >= 3;
        if (loop && glm::distance(pts.front().Position, pts.back().Position) <= 1e-5f) pts.pop_back();
        size_t n = pts.size();
        if (n < 2) return {};

        auto viewAt = [&](const glm::vec3& p) {
            if (!params.FaceCamera) return params.Normal;
            return params.Orthographic ? -params.CameraForward : params.CameraPosition - p;
        };
        auto dir = [&](size_t from, size_t to) {
            glm::vec3 d = pts[to].Position - pts[from].Position;
            return d / glm::length(d);
        };

        // Edge vertices per point; a looped ribbon repeats the first point at the end.
        size_t count = loop ? n + 1 : n;
        std::vector<glm::vec3> left(count), right(count);
        std::vector<float> distance(count, 0.0f);
        for (size_t k = 0; k < count; ++k)
        {
            size_t i = k % n;
            glm::vec3 tangent, segment;
            if (loop)
            {
                glm::vec3 in = dir((i + n - 1) % n, i), out = dir(i, (i + 1) % n);
                tangent = in + out;
                segment = out;
            }
            else if (i == 0) tangent = segment = dir(0, 1);
            else if (i == n - 1) tangent = segment = dir(n - 2, n - 1);
            else
            {
                glm::vec3 in = dir(i - 1, i), out = dir(i, i + 1);
                tangent = in + out;
                segment = out;
            }
            if (glm::dot(tangent, tangent) < 1e-8f) tangent = segment; // a full reversal
            tangent = glm::normalize(tangent);

            glm::vec3 view = viewAt(pts[i].Position);
            glm::vec3 side = glm::cross(tangent, view);
            if (glm::dot(side, side) < 1e-12f) side = glm::cross(tangent, std::abs(tangent.y) < 0.99f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0));
            side = glm::normalize(side);
            // Miter: widen at corners so the segments keep their width (capped for sharp turns).
            float miter = 1.0f / std::max(glm::dot(tangent, segment), 0.5f);
            glm::vec3 half = side * (pts[i].Width * 0.5f * miter);
            left[k] = pts[i].Position - half;
            right[k] = pts[i].Position + half;
            if (k > 0) distance[k] = distance[k - 1] + glm::distance(pts[(k - 1) % n].Position, pts[i].Position);
        }

        float total = std::max(distance.back(), 1e-6f);
        auto u = [&](size_t k) { return params.TextureMode == LineTextureMode::Tile ? distance[k] : distance[k] / total; };
        std::vector<RibbonQuad> quads(count - 1);
        for (size_t k = 0; k + 1 < count; ++k)
        {
            const RibbonPoint& a = pts[k % n];
            const RibbonPoint& b = pts[(k + 1) % n];
            RibbonQuad& q = quads[k];
            q.P[0] = left[k];
            q.P[1] = right[k];
            q.P[2] = right[k + 1];
            q.P[3] = left[k + 1];
            q.C[0] = q.C[1] = a.Color;
            q.C[2] = q.C[3] = b.Color;
            q.UV[0] = { u(k), 1.0f };
            q.UV[1] = { u(k), 0.0f };
            q.UV[2] = { u(k + 1), 0.0f };
            q.UV[3] = { u(k + 1), 1.0f };
        }
        return quads;
    }

    namespace TrailRenderers
    {
        void Update(Scene& scene, float time)
        {
            for (auto [e, trail, t] : scene.GetRegistry().ViewOf<TrailRendererComponent, TransformComponent>())
            {
                auto& pts = trail.Points;
                float lifetime = std::max(trail.Time, 1e-3f);
                auto expired = std::find_if(pts.begin(), pts.end(), [&](const auto& p) { return time - p.Time <= lifetime; });
                pts.erase(pts.begin(), expired);
                if (!trail.Emitting || !t.ActiveInHierarchy) continue;
                glm::vec3 head = t.GetWorldPosition();
                if (pts.empty() || glm::distance(pts.back().Position, head) >= std::max(trail.MinVertexDistance, 1e-4f))
                    pts.push_back({ head, time });
            }
        }

        void ClearAll(Scene& scene)
        {
            for (auto [e, trail] : scene.GetRegistry().ViewOf<TrailRendererComponent>()) trail.Clear();
        }
    }
}
