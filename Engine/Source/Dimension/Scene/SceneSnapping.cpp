#include "Dimension/Scene/SceneSnapping.h"
#include "Dimension/Asset/AssetManager.h"
#include "Dimension/Scene/Components.h"
#include "Dimension/Terrain/Terrain.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>

namespace Dimension::SceneSnapping
{
    namespace
    {
        bool Skipped(Scene& scene, Entity e, const std::vector<Entity>& skip)
        {
            for (Entity s : skip)
                if (e == s || (scene.IsValid(s) && scene.IsDescendantOf(e, s))) return true;
            return false;
        }

        bool Included(Scene& scene, Entity e, const std::vector<Entity>& only)
        {
            if (only.empty()) return true;
            for (Entity s : only)
                if (e == s || (scene.IsValid(s) && scene.IsDescendantOf(e, s))) return true;
            return false;
        }

        // Each visible mesh renderer with its geometry and world transform.
        template <class F> void EachMesh(Scene& scene, F&& fn)
        {
            auto& reg = scene.GetRegistry();
            for (auto [e, t, mr] : reg.ViewOf<TransformComponent, MeshRendererComponent>())
            {
                if (!t.ActiveInHierarchy || !mr.Mesh.IsValid()) continue;
                auto data = AssetManager::GetMeshData(mr.Mesh);
                if (!data || data->Vertices.empty()) continue;
                fn(e, t.World, *data);
            }
        }

        // The mesh's bounds in its own space (cached while the geometry stays loaded).
        AABB LocalBounds(const MeshData& data)
        {
            static std::unordered_map<const MeshData*, std::pair<size_t, AABB>> cache;
            auto it = cache.find(&data);
            if (it != cache.end() && it->second.first == data.Vertices.size()) return it->second.second;
            AABB b{ glm::vec3(std::numeric_limits<float>::max()), glm::vec3(-std::numeric_limits<float>::max()) };
            for (const Vertex& v : data.Vertices)
            {
                b.Min = glm::min(b.Min, v.Position);
                b.Max = glm::max(b.Max, v.Position);
            }
            cache[&data] = { data.Vertices.size(), b };
            return b;
        }

        bool RayHitsBox(const glm::vec3& o, const glm::vec3& d, const AABB& b)
        {
            float t0 = 0.0f, t1 = std::numeric_limits<float>::max();
            for (int a = 0; a < 3; ++a)
            {
                if (std::abs(d[a]) < 1e-12f)
                {
                    if (o[a] < b.Min[a] || o[a] > b.Max[a]) return false;
                    continue;
                }
                float n = (b.Min[a] - o[a]) / d[a], f = (b.Max[a] - o[a]) / d[a];
                if (n > f) std::swap(n, f);
                t0 = std::max(t0, n);
                t1 = std::min(t1, f);
                if (t0 > t1) return false;
            }
            return true;
        }

        // Moller-Trumbore, both sides.
        bool RayTriangle(const glm::vec3& o, const glm::vec3& d, const glm::vec3& a, const glm::vec3& b, const glm::vec3& c, float& t)
        {
            glm::vec3 e1 = b - a, e2 = c - a, p = glm::cross(d, e2);
            float det = glm::dot(e1, p);
            if (std::abs(det) < 1e-12f) return false;
            float inv = 1.0f / det;
            glm::vec3 s = o - a;
            float u = glm::dot(s, p) * inv;
            if (u < 0.0f || u > 1.0f) return false;
            glm::vec3 q = glm::cross(s, e1);
            float v = glm::dot(d, q) * inv;
            if (v < 0.0f || u + v > 1.0f) return false;
            t = glm::dot(e2, q) * inv;
            return t > 0.0f;
        }
    }

    glm::vec3 ToGrid(const glm::vec3& p, float step, const glm::bvec3& axes)
    {
        if (step <= 0.0f) return p;
        glm::vec3 out = p;
        for (int a = 0; a < 3; ++a)
            if (axes[a]) out[a] = std::round(p[a] / step) * step;
        return out;
    }

    SurfaceHit Raycast(Scene& scene, const glm::vec3& origin, const glm::vec3& direction, const std::vector<Entity>& skip)
    {
        SurfaceHit best;
        best.Distance = std::numeric_limits<float>::max();
        glm::vec3 dir = glm::normalize(direction);
        EachMesh(scene, [&](Entity e, const glm::mat4& world, const MeshData& data) {
            if (Skipped(scene, e, skip)) return;
            // In the mesh's space; hits go back to world space to compare distances.
            glm::mat4 inv = glm::inverse(world);
            glm::vec3 o = glm::vec3(inv * glm::vec4(origin, 1.0f)), d = glm::vec3(inv * glm::vec4(dir, 0.0f));
            if (!RayHitsBox(o, d, LocalBounds(data))) return;
            for (size_t i = 0; i + 2 < data.Indices.size(); i += 3)
            {
                const glm::vec3& a = data.Vertices[data.Indices[i]].Position;
                const glm::vec3& b = data.Vertices[data.Indices[i + 1]].Position;
                const glm::vec3& c = data.Vertices[data.Indices[i + 2]].Position;
                float t;
                if (!RayTriangle(o, d, a, b, c, t)) continue;
                glm::vec3 point = glm::vec3(world * glm::vec4(o + d * t, 1.0f));
                float distance = glm::length(point - origin);
                if (distance >= best.Distance) continue;
                glm::vec3 n = glm::normalize(glm::transpose(glm::mat3(inv)) * glm::cross(b - a, c - a));
                if (glm::dot(n, dir) > 0.0f) n = -n; // the side facing the ray
                best = { true, point, n, distance, e };
            }
        });
        auto& reg = scene.GetRegistry();
        for (auto [e, t, terrain] : reg.ViewOf<TransformComponent, TerrainComponent>())
        {
            if (!t.ActiveInHierarchy || Skipped(scene, e, skip)) continue;
            glm::vec3 point;
            if (!Terrain::Raycast(terrain, t.World, origin, dir, point)) continue;
            float distance = glm::length(point - origin);
            if (distance >= best.Distance) continue;
            // The normal from the height slopes around the point (object space).
            glm::mat4 inv = glm::inverse(t.World);
            glm::vec3 local = glm::vec3(inv * glm::vec4(point, 1.0f));
            std::vector<float> heights = Terrain::GetHeights(terrain);
            float h = 0.1f;
            float dx = Terrain::HeightAt(terrain, heights, local.x + h, local.z) - Terrain::HeightAt(terrain, heights, local.x - h, local.z);
            float dz = Terrain::HeightAt(terrain, heights, local.x, local.z + h) - Terrain::HeightAt(terrain, heights, local.x, local.z - h);
            glm::vec3 n = glm::normalize(glm::transpose(glm::mat3(inv)) * glm::vec3(-dx, 2.0f * h, -dz));
            best = { true, point, n, distance, e };
        }
        if (!best.Hit) best.Distance = 0.0f;
        return best;
    }

    VertexHit NearestVertex(Scene& scene, const glm::mat4& viewProj, const glm::vec2& viewport, const glm::vec2& screen,
                            float maxPixels, const std::vector<Entity>& only, const std::vector<Entity>& skip)
    {
        VertexHit best;
        best.Pixels = maxPixels;
        auto toScreen = [&](const glm::vec4& clip) {
            glm::vec2 ndc = glm::vec2(clip) / clip.w;
            return glm::vec2((ndc.x * 0.5f + 0.5f) * viewport.x, (0.5f - ndc.y * 0.5f) * viewport.y); // NDC y up
        };
        EachMesh(scene, [&](Entity e, const glm::mat4& world, const MeshData& data) {
            if (Skipped(scene, e, skip) || !Included(scene, e, only)) return;
            glm::mat4 m = viewProj * world;
            // Skip meshes whose on-screen box is farther than the best so far (unless a corner is behind the camera).
            AABB b = LocalBounds(data);
            glm::vec2 lo(std::numeric_limits<float>::max()), hi(-std::numeric_limits<float>::max());
            bool behind = false;
            for (int c = 0; c < 8; ++c)
            {
                glm::vec4 clip = m * glm::vec4(c & 1 ? b.Max.x : b.Min.x, c & 2 ? b.Max.y : b.Min.y, c & 4 ? b.Max.z : b.Min.z, 1.0f);
                if (clip.w <= 1e-5f)
                {
                    behind = true;
                    break;
                }
                glm::vec2 s = toScreen(clip);
                lo = glm::min(lo, s);
                hi = glm::max(hi, s);
            }
            if (!behind && (screen.x < lo.x - best.Pixels || screen.x > hi.x + best.Pixels || screen.y < lo.y - best.Pixels ||
                            screen.y > hi.y + best.Pixels))
                return;
            for (const Vertex& v : data.Vertices)
            {
                glm::vec4 clip = m * glm::vec4(v.Position, 1.0f);
                if (clip.w <= 1e-5f) continue;
                float pixels = glm::length(toScreen(clip) - screen);
                if (pixels > best.Pixels || (best.Hit && pixels == best.Pixels)) continue;
                best = { true, glm::vec3(world * glm::vec4(v.Position, 1.0f)), pixels, e };
            }
        });
        if (!best.Hit) best.Pixels = 0.0f;
        return best;
    }

    float RestOffset(Scene& scene, Entity e, const glm::vec3& normal)
    {
        if (!scene.IsValid(e)) return 0.0f;
        glm::vec3 pivot = scene.GetRegistry().Get<TransformComponent>(e).GetWorldPosition();
        glm::vec3 n = glm::normalize(normal);
        float reach = -std::numeric_limits<float>::max();
        EachMesh(scene, [&](Entity m, const glm::mat4& world, const MeshData& data) {
            if (m != e && !scene.IsDescendantOf(m, e)) return;
            for (const Vertex& v : data.Vertices)
                reach = std::max(reach, glm::dot(pivot - glm::vec3(world * glm::vec4(v.Position, 1.0f)), n));
        });
        return reach == -std::numeric_limits<float>::max() ? 0.0f : reach;
    }
}
