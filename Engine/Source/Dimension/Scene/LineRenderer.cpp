#include "Dimension/Scene/LineRenderer.h"
#include "Dimension/Scene/Scene.h"
#include "Dimension/Scene/Components.h"
#include <glm/gtc/constants.hpp>
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
        // Unit vector across the ribbon at point i for a direction along it.
        auto sideAt = [&](size_t i, const glm::vec3& tangent) {
            glm::vec3 side = glm::cross(tangent, viewAt(pts[i].Position));
            if (glm::dot(side, side) < 1e-12f) side = glm::cross(tangent, std::abs(tangent.y) < 0.99f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0));
            return glm::normalize(side);
        };
        std::vector<RibbonQuad> quads;
        // Triangle fan around `center` sweeping from direction `a` by `angle` radians toward `toward` (any vector with
        // a component perpendicular to a), as quads with coincident last corners.
        auto fan = [&](const RibbonPoint& p, glm::vec3 a, glm::vec3 toward, float angle, int segments, float u, float vEdge) {
            glm::vec3 m = toward - a * glm::dot(a, toward);
            if (glm::dot(m, m) < 1e-12f || angle <= 1e-4f) return;
            m = glm::normalize(m);
            float r = p.Width * 0.5f;
            segments = std::max(segments, 1);
            glm::vec3 prev = p.Position + a * r;
            for (int s = 1; s <= segments; ++s)
            {
                float phi = angle * s / segments;
                glm::vec3 next = p.Position + (a * std::cos(phi) + m * std::sin(phi)) * r;
                RibbonQuad q;
                q.P[0] = p.Position;
                q.P[1] = prev;
                q.P[2] = q.P[3] = next;
                for (auto& c : q.C) c = p.Color;
                q.UV[0] = { u, 0.5f };
                q.UV[1] = q.UV[2] = q.UV[3] = { u, vEdge };
                quads.push_back(q);
                prev = next;
            }
        };
        auto angleBetween = [](const glm::vec3& a, const glm::vec3& b) { return std::acos(std::clamp(glm::dot(a, b), -1.0f, 1.0f)); };

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

            glm::vec3 side = sideAt(i, tangent);
            // Miter: widen at corners so the segments keep their width (capped for sharp turns).
            float miter = 1.0f / std::max(glm::dot(tangent, segment), 0.5f);
            glm::vec3 half = side * (pts[i].Width * 0.5f * miter);
            left[k] = pts[i].Position - half;
            right[k] = pts[i].Position + half;
            if (k > 0) distance[k] = distance[k - 1] + glm::distance(pts[(k - 1) % n].Position, pts[i].Position);
        }

        float total = std::max(distance.back(), 1e-6f);
        auto u = [&](size_t k) { return params.TextureMode == LineTextureMode::Tile ? distance[k] : distance[k] / total; };
        bool round = params.CornerVertices > 0;
        for (size_t k = 0; k + 1 < count; ++k)
        {
            const RibbonPoint& a = pts[k % n];
            const RibbonPoint& b = pts[(k + 1) % n];
            RibbonQuad q;
            if (round)
            {
                // Round joins: each segment keeps square ends; fans fill the corners below.
                glm::vec3 d = dir(k % n, (k + 1) % n);
                glm::vec3 ha = sideAt(k % n, d) * (a.Width * 0.5f), hb = sideAt((k + 1) % n, d) * (b.Width * 0.5f);
                q.P[0] = a.Position - ha;
                q.P[1] = a.Position + ha;
                q.P[2] = b.Position + hb;
                q.P[3] = b.Position - hb;
            }
            else
            {
                q.P[0] = left[k];
                q.P[1] = right[k];
                q.P[2] = right[k + 1];
                q.P[3] = left[k + 1];
            }
            q.C[0] = q.C[1] = a.Color;
            q.C[2] = q.C[3] = b.Color;
            q.UV[0] = { u(k), 1.0f };
            q.UV[1] = { u(k), 0.0f };
            q.UV[2] = { u(k + 1), 0.0f };
            q.UV[3] = { u(k + 1), 1.0f };
            quads.push_back(q);
        }

        if (round)
        {
            // A fan on each side of every corner, from the incoming segment's edge to the outgoing one's. The inner
            // fan lies inside the overlapping segments; through a full reversal the outer arc passes the far side.
            for (size_t k = loop ? 0 : 1; k < (loop ? n : n - 1); ++k)
            {
                size_t i = k % n;
                glm::vec3 in = dir((i + n - 1) % n, i), out = dir(i, (i + 1) % n);
                glm::vec3 sIn = sideAt(i, in), sOut = sideAt(i, out);
                float angle = angleBetween(sIn, sOut);
                bool reversal = angle > 3.1f;
                fan(pts[i], sIn, reversal ? in : sOut, angle, params.CornerVertices + 1, u(k), 0.0f);
                fan(pts[i], -sIn, reversal ? in : -sOut, angle, params.CornerVertices + 1, u(k), 1.0f);
            }
        }
        if (params.CapVertices > 0 && !loop)
        {
            // Half discs beyond each end, sweeping from one edge round the far side to the other.
            glm::vec3 d0 = dir(0, 1), d1 = dir(n - 2, n - 1);
            fan(pts[0], sideAt(0, d0), -d0, glm::pi<float>(), params.CapVertices + 1, u(0), 0.0f);
            fan(pts[n - 1], sideAt(n - 1, d1), d1, glm::pi<float>(), params.CapVertices + 1, u(count - 1), 0.0f);
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
                if (!trail.Emitting || !t.ActiveInHierarchy)
                {
                    trail.HasHead = false;
                    continue;
                }
                glm::vec3 head = t.GetWorldPosition();
                trail.PreviousHead = trail.HasHead ? trail.Head : head;
                trail.Head = head;
                trail.HasHead = true;
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
