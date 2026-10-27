#include "Dimension/Scene/PathPlacer.h"
#include "Dimension/Scene/Components.h"
#include "Dimension/Terrain/Terrain.h"
#include <algorithm>
#include <cmath>

namespace Dimension::PathPlacer
{
    namespace
    {
        constexpr int kMaxCopies = 10000;

        float Random01(uint32_t& state)
        {
            state = state * 1664525u + 1013904223u;
            return (state >> 8) / 16777216.0f;
        }
    }

    std::vector<Spot> Spots(Scene& scene, Entity placer)
    {
        std::vector<Spot> spots;
        auto& reg = scene.GetRegistry();
        auto* pp = reg.TryGet<PathPlacerComponent>(placer);
        if (!pp || pp->Points.size() < 2) return spots;
        const glm::mat4& world = reg.Get<TransformComponent>(placer).World;
        std::vector<glm::vec3> points = pp->Points;
        if (pp->Closed && points.size() > 2) points.push_back(points.front());
        std::vector<glm::vec3> curve = Terrain::PathCurve(points, 16);
        for (glm::vec3& p : curve) p = glm::vec3(world * glm::vec4(p, 1.0f));
        // Walk the curve, dropping a spot every Spacing meters (measured along it).
        float spacing = std::max(pp->Spacing, 0.05f), next = std::max(pp->StartOffset, 0.0f), walked = 0.0f;
        for (size_t k = 0; k + 1 < curve.size() && (int)spots.size() < kMaxCopies; ++k)
        {
            glm::vec3 a = curve[k], b = curve[k + 1];
            float length = glm::distance(a, b);
            if (length < 1e-6f) continue;
            glm::vec3 dir = (b - a) / length;
            while (next <= walked + length && (int)spots.size() < kMaxCopies)
            {
                spots.push_back({ a + dir * (next - walked), dir });
                next += spacing;
            }
            walked += length;
        }
        // A closed loop's last spot would land on its first.
        if (pp->Closed && spots.size() > 1 && glm::distance(spots.back().Position, spots.front().Position) < spacing * 0.5f) spots.pop_back();
        // Side Offset moves each spot across the path (+ = right of its direction); Snap To Ground puts it on the
        // terrain under (or over) it.
        struct Ground { const TerrainComponent* Terrain; glm::mat4 World, ToLocal; std::vector<float> Heights; };
        std::vector<Ground> grounds;
        if (pp->SnapToGround)
            for (auto [te, terrain, tt] : reg.ViewOf<TerrainComponent, TransformComponent>())
                grounds.push_back({ &terrain, tt.World, glm::inverse(tt.World), Terrain::GetHeights(terrain) });
        for (Spot& s : spots)
        {
            glm::vec3 right = glm::cross(s.Direction, glm::vec3(0.0f, 1.0f, 0.0f));
            if (glm::dot(right, right) > 1e-8f) s.Position += glm::normalize(right) * pp->SideOffset;
            float best = -1e30f;
            for (const Ground& g : grounds)
            {
                glm::vec3 local = glm::vec3(g.ToLocal * glm::vec4(s.Position, 1.0f));
                glm::vec2 half = glm::max(g.Terrain->Size, glm::vec2(0.01f)) * 0.5f;
                if (std::abs(local.x) > half.x || std::abs(local.z) > half.y) continue;
                local.y = Terrain::HeightAt(*g.Terrain, g.Heights, local.x, local.z);
                best = std::max(best, glm::vec3(g.World * glm::vec4(local, 1.0f)).y);
            }
            if (best > -1e30f) s.Position.y = best;
        }
        return spots;
    }

    void Clear(Scene& scene, Entity placer)
    {
        if (!scene.IsValid(placer)) return;
        auto* pp = scene.GetRegistry().TryGet<PathPlacerComponent>(placer);
        std::vector<Entity> children = scene.GetChildren(placer);
        for (Entity c : children)
            if (!pp || pp->Source.IsNull() || (c != pp->Source && !scene.IsDescendantOf(pp->Source, c))) scene.DestroyEntity(c);
    }

    int Place(Scene& scene, Entity placer)
    {
        auto& reg = scene.GetRegistry();
        auto* pp = reg.TryGet<PathPlacerComponent>(placer);
        if (!pp) return 0;
        Entity source = !pp->Source.IsNull() && scene.IsValid(pp->Source) && pp->Source != placer && !scene.IsDescendantOf(pp->Source, placer)
                      ? pp->Source : Entity::Null();
        if (source.IsNull() && !pp->Prefab.IsValid()) return 0;
        Clear(scene, placer);
        scene.UpdateTransforms();
        std::vector<Spot> spots = Spots(scene, placer);
        // Copy the settings: creating entities may move the component.
        AssetHandle prefab = pp->Prefab;
        bool align = pp->AlignToPath;
        float yawOffset = glm::radians(pp->Rotation), randomYaw = glm::radians(std::max(pp->RandomRotation, 0.0f));
        float randomScale = std::clamp(pp->RandomScale, 0.0f, 0.95f);
        uint32_t state = (uint32_t)pp->Seed * 2654435761u + 12345u;
        int placed = 0;
        for (const Spot& s : spots)
        {
            float r0 = Random01(state), r1 = Random01(state);
            Entity copy = source.IsNull() ? scene.InstantiatePrefab(prefab, s.Position) : scene.DuplicateEntity(source);
            if (copy.IsNull()) break;
            if (!source.IsNull()) scene.SetParent(copy, Entity::Null(), false);
            auto& t = reg.Get<TransformComponent>(copy);
            glm::vec3 scale = t.Scale * (1.0f + (r1 * 2.0f - 1.0f) * randomScale);
            // Facing along the path means the copy's forward (-Z) follows the direction.
            float yaw = (align ? std::atan2(-s.Direction.x, -s.Direction.z) : 0.0f) + yawOffset + (r0 * 2.0f - 1.0f) * randomYaw;
            t.Position = s.Position;
            t.Rotation = align || randomYaw > 0.0f || yawOffset != 0.0f ? glm::vec3(t.Rotation.x, yaw, t.Rotation.z) : t.Rotation;
            t.Scale = scale;
            scene.UpdateTransforms();
            scene.SetParent(copy, placer, true);
            ++placed;
        }
        scene.UpdateTransforms();
        return placed;
    }
}
