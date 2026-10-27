#include "Dimension/Scene/LODGroup.h"
#include "Dimension/Scene/Scene.h"
#include "Dimension/Asset/AssetManager.h"
#include "Dimension/Renderer/Mesh.h"
#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace Dimension
{
    namespace LODGroups
    {
        namespace
        {
            // A mesh's bounds from its CPU data (measured once per loaded version).
            bool MeshBounds(AssetHandle handle, AABB& bounds)
            {
                static std::unordered_map<uint64_t, std::pair<std::weak_ptr<const MeshData>, AABB>> cache;
                std::shared_ptr<const MeshData> data = AssetManager::GetMeshData(handle);
                if (!data || data->Vertices.empty()) return false;
                auto& entry = cache[(uint64_t)handle];
                if (entry.first.lock() != data)
                {
                    AABB b;
                    b.Min = b.Max = data->Vertices[0].Position;
                    for (const Vertex& v : data->Vertices)
                    {
                        b.Min = glm::min(b.Min, v.Position);
                        b.Max = glm::max(b.Max, v.Position);
                    }
                    entry = { data, b };
                }
                bounds = entry.second;
                return true;
            }

            void GrowBounds(Scene& scene, Entity e, glm::vec3& lo, glm::vec3& hi)
            {
                auto& reg = scene.GetRegistry();
                AABB b;
                if (auto* mr = reg.TryGet<MeshRendererComponent>(e))
                    if (MeshBounds(mr->Mesh, b))
                    {
                        const auto& world = reg.Get<TransformComponent>(e).World;
                        glm::vec3 c = glm::vec3(world * glm::vec4(b.Center(), 1.0f)), ext = b.Extents();
                        glm::mat3 m(world);
                        glm::vec3 half = glm::abs(m[0]) * ext.x + glm::abs(m[1]) * ext.y + glm::abs(m[2]) * ext.z;
                        lo = glm::min(lo, c - half);
                        hi = glm::max(hi, c + half);
                    }
                for (Entity child : scene.GetChildren(e)) GrowBounds(scene, child, lo, hi);
            }

            void MarkSubtree(Scene& scene, Entity e, std::vector<uint8_t>& hidden)
            {
                if (hidden.size() <= e.Index) hidden.resize(e.Index + 1, 0);
                hidden[e.Index] = 1;
                for (Entity child : scene.GetChildren(e)) MarkSubtree(scene, child, hidden);
            }
        }

        float WorldSize(Scene& scene, Entity group, glm::vec3* center)
        {
            auto& reg = scene.GetRegistry();
            const auto* lod = reg.TryGet<LODGroupComponent>(group);
            glm::vec3 lo(1e30f), hi(-1e30f);
            GrowBounds(scene, group, lo, hi);
            bool any = lo.x <= hi.x;
            glm::vec3 at = any ? (lo + hi) * 0.5f : reg.Get<TransformComponent>(group).GetWorldPosition();
            if (center) *center = at;
            if (lod && lod->Size > 0.0f)
            {
                const auto& world = reg.Get<TransformComponent>(group).World;
                float scale = std::max({ glm::length(glm::vec3(world[0])), glm::length(glm::vec3(world[1])), glm::length(glm::vec3(world[2])) });
                return lod->Size * scale;
            }
            if (!any) return 1.0f;
            glm::vec3 size = hi - lo;
            return std::max({ size.x, size.y, size.z });
        }

        int Choose(Scene& scene, Entity group, const glm::vec3& cameraPosition, const glm::mat4& projection, bool orthographic, float* screenHeight)
        {
            auto& reg = scene.GetRegistry();
            const auto* lod = reg.TryGet<LODGroupComponent>(group);
            if (!lod) return 0;
            glm::vec3 center;
            float size = WorldSize(scene, group, &center);
            // A sphere of that size projected: the fraction of the view's height it spans.
            float height = orthographic ? size * projection[1][1] * 0.5f
                                        : size * std::abs(projection[1][1]) * 0.5f / std::max(glm::distance(center, cameraPosition), 1e-4f);
            height *= std::max(lod->Bias, 0.0f);
            if (screenHeight) *screenHeight = height;
            int levels = std::min((int)scene.GetChildren(group).size(), 4);
            const float below[4] = { lod->LOD1Below, lod->LOD2Below, lod->LOD3Below, lod->CullBelow };
            // Level i shows down to the next level's threshold; the last child down to Cull Below.
            for (int i = 0; i < levels; ++i)
            {
                float floor = i == levels - 1 ? lod->CullBelow : below[i];
                if (height >= floor) return i;
            }
            return -1;
        }

        void Hidden(Scene& scene, const glm::vec3& cameraPosition, const glm::mat4& projection, bool orthographic, std::vector<uint8_t>& hidden)
        {
            auto& reg = scene.GetRegistry();
            std::vector<Entity> groups;
            for (auto [e, lod, t] : reg.ViewOf<LODGroupComponent, TransformComponent>())
                if (t.ActiveInHierarchy) groups.push_back(e);
            for (Entity g : groups)
            {
                int level = Choose(scene, g, cameraPosition, projection, orthographic);
                const auto& children = scene.GetChildren(g);
                for (size_t i = 0; i < children.size(); ++i)
                    if ((int)i != level) MarkSubtree(scene, children[i], hidden);
            }
        }
    }
}
