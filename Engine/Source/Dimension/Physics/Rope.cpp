#include "Dimension/Physics/Rope.h"
#include "Dimension/Scene/Components.h"
#include <glm/gtc/constants.hpp>
#include <map>
#include <mutex>

namespace Dimension::Rope
{
    int PointCount(const RopeComponent& rope) { return std::clamp(rope.Segments, 1, 256) + 1; }

    std::vector<glm::vec3> RestPoints(const RopeComponent& rope, const glm::vec3& start, const glm::vec3& down, const glm::vec3* end)
    {
        int count = PointCount(rope);
        float length = std::max(rope.Length, 0.01f);
        std::vector<glm::vec3> points(count);
        glm::vec3 d = glm::length(down) > 1e-6f ? glm::normalize(down) : glm::vec3(0.0f, -1.0f, 0.0f);
        if (!end)
        {
            for (int i = 0; i < count; ++i) points[i] = start + d * (length * i / (count - 1));
            return points;
        }
        // Between the two ends, sagging (a parabola below the line) until the curve is Length long.
        auto curve = [&](float sag, std::vector<glm::vec3>& out) {
            float total = 0.0f;
            for (int i = 0; i < count; ++i)
            {
                float t = (float)i / (count - 1);
                out[i] = glm::mix(start, *end, t) + glm::vec3(0.0f, -1.0f, 0.0f) * (sag * 4.0f * t * (1.0f - t));
                if (i > 0) total += glm::distance(out[i], out[i - 1]);
            }
            return total;
        };
        if (curve(0.0f, points) >= length) return points; // taut
        float lo = 0.0f, hi = length;
        for (int k = 0; k < 40; ++k)
        {
            float mid = 0.5f * (lo + hi);
            if (curve(mid, points) < length) lo = mid;
            else hi = mid;
        }
        curve(0.5f * (lo + hi), points);
        return points;
    }

    void BuildTube(const RopeComponent& rope, const std::vector<glm::vec3>& points, std::vector<Vertex>& out)
    {
        size_t count = points.size();
        out.resize(count * (kSides + 1));
        if (count < 2) return;
        float radius = std::max(rope.Radius, 0.001f);
        // A snapped rope: the rings either side of the break shrink to points, so the gap between them vanishes and
        // the two ends taper off (frayed).
        // A frame carried along the rope (parallel transport), so the tube does not twist where it bends.
        glm::vec3 tangent = glm::normalize(points[1] - points[0] + glm::vec3(0.0f, 1e-7f, 0.0f));
        glm::vec3 normal = glm::normalize(glm::cross(tangent, std::abs(tangent.y) < 0.9f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0)));
        float along = 0.0f;
        for (size_t i = 0; i < count; ++i)
        {
            glm::vec3 next = i + 1 < count ? points[i + 1] - points[i] : points[i] - points[i - 1];
            glm::vec3 prev = i > 0 ? points[i] - points[i - 1] : next;
            glm::vec3 t = next + prev;
            if (glm::dot(t, t) < 1e-12f) t = tangent;
            t = glm::normalize(t);
            normal = normal - t * glm::dot(normal, t);
            normal = glm::dot(normal, normal) > 1e-12f ? glm::normalize(normal) : glm::normalize(glm::cross(t, glm::vec3(1, 0, 0)));
            glm::vec3 binormal = glm::cross(t, normal);
            tangent = t;
            if (i > 0) along += glm::distance(points[i], points[i - 1]);
            for (int s = 0; s <= kSides; ++s)
            {
                float a = glm::two_pi<float>() * s / kSides;
                glm::vec3 n = normal * std::cos(a) + binormal * std::sin(a);
                Vertex& v = out[i * (kSides + 1) + s];
                bool frayed = rope.BrokenAt >= 0 && (i == (size_t)rope.BrokenAt || i == (size_t)rope.BrokenAt + 1);
                v.Position = points[i] + n * (frayed ? 0.0f : radius);
                v.Normal = n;
                v.UV = { (float)s / kSides, along };
                v.Tangent = glm::vec4(-normal * std::sin(a) + binormal * std::cos(a), 1.0f);
            }
        }
    }

    namespace
    {
        std::mutex s_Mutex;
        std::map<int, Ref<Mesh>> s_Meshes;
    }

    Ref<Mesh> TubeMesh(const RopeComponent& rope)
    {
        std::lock_guard lock(s_Mutex);
        int count = PointCount(rope);
        Ref<Mesh>& mesh = s_Meshes[count];
        if (!mesh)
        {
            std::vector<glm::vec3> points(count);
            for (int i = 0; i < count; ++i) points[i] = glm::vec3(0.0f, -(float)i / (count - 1), 0.0f);
            std::vector<Vertex> vertices;
            BuildTube(rope, points, vertices);
            std::vector<uint32_t> indices;
            for (int i = 0; i + 1 < count; ++i)
                for (int s = 0; s < kSides; ++s)
                {
                    uint32_t a = i * (kSides + 1) + s, b = a + 1, c = a + (kSides + 1), d = c + 1;
                    indices.insert(indices.end(), { a, c, b, b, c, d });
                }
            mesh = MakeRef<Mesh>(std::move(vertices), std::move(indices), "Rope");
        }
        return mesh;
    }

    void ReleaseMeshes()
    {
        std::lock_guard lock(s_Mutex);
        s_Meshes.clear();
    }
}
