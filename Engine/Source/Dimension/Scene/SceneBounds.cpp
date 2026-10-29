#include "Dimension/Scene/SceneBounds.h"
#include "Dimension/Scene/Components.h"
#include "Dimension/Asset/AssetManager.h"
#include <unordered_map>

namespace Dimension::SceneBounds
{
    std::optional<AABB> World(Scene& scene, const std::function<bool(Entity)>& skip)
    {
        auto& reg = scene.GetRegistry();
        scene.UpdateTransforms();
        glm::vec3 lo(1e30f), hi(-1e30f);
        auto grow = [&](const glm::mat4& world, const glm::vec3& a, const glm::vec3& b) {
            for (int c = 0; c < 8; ++c)
            {
                glm::vec3 p((c & 1) ? b.x : a.x, (c & 2) ? b.y : a.y, (c & 4) ? b.z : a.z);
                glm::vec3 w = glm::vec3(world * glm::vec4(p, 1.0f));
                lo = glm::min(lo, w);
                hi = glm::max(hi, w);
            }
        };
        auto shown = [&](Entity e, const TransformComponent& t) { return t.ActiveInHierarchy && !(skip && skip(e)); };

        std::unordered_map<uint64_t, std::optional<AABB>> meshBounds; // per mesh, computed once
        for (auto [e, mr, t] : reg.ViewOf<MeshRendererComponent, TransformComponent>())
        {
            if (!shown(e, t)) continue;
            auto [it, added] = meshBounds.try_emplace((uint64_t)mr.Mesh);
            if (added)
                if (auto data = AssetManager::GetMeshData(mr.Mesh); data && !data->Vertices.empty())
                {
                    AABB box{ glm::vec3(1e30f), glm::vec3(-1e30f) };
                    for (const Vertex& v : data->Vertices)
                    {
                        box.Min = glm::min(box.Min, v.Position);
                        box.Max = glm::max(box.Max, v.Position);
                    }
                    it->second = box;
                }
            if (it->second) grow(t.World, it->second->Min, it->second->Max);
        }
        for (auto [e, sr, t] : reg.ViewOf<SpriteRendererComponent, TransformComponent>())
            if (shown(e, t)) grow(t.World, glm::vec3(-0.5f, -0.5f, 0.0f), glm::vec3(0.5f, 0.5f, 0.0f));
        for (auto [e, terrain, t] : reg.ViewOf<TerrainComponent, TransformComponent>())
            if (shown(e, t))
                grow(t.World, glm::vec3(-terrain.Size.x * 0.5f, 0.0f, -terrain.Size.y * 0.5f), glm::vec3(terrain.Size.x * 0.5f, terrain.Height, terrain.Size.y * 0.5f));

        if (lo.x > hi.x)
            for (auto [e, t] : reg.ViewOf<TransformComponent>())
                if (shown(e, t))
                {
                    glm::vec3 p = t.GetWorldPosition();
                    lo = glm::min(lo, p);
                    hi = glm::max(hi, p);
                }
        if (lo.x > hi.x) return std::nullopt;
        return AABB{ lo, hi };
    }
}
