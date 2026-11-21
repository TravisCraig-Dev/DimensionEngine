#include "Dimension/Scene/LODGroup.h"
#include "Dimension/Scene/Scene.h"
#include "Dimension/Asset/AssetManager.h"
#include "Dimension/Renderer/Mesh.h"
#include "Dimension/Renderer/MeshSimplify.h"
#include <algorithm>
#include <cmath>
#include <future>
#include <map>
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

            void FadeSubtree(Scene& scene, Entity e, uint32_t fade, std::unordered_map<uint32_t, uint32_t>& fades)
            {
                fades[e.Index] = fade;
                for (Entity child : scene.GetChildren(e)) FadeSubtree(scene, child, fade, fades);
            }

            bool Generates(Scene& scene, Entity group)
            {
                auto& reg = scene.GetRegistry();
                const auto* lod = reg.TryGet<LODGroupComponent>(group);
                return lod && lod->GeneratedLevels > 0 && reg.Has<MeshRendererComponent>(group);
            }

            // Coverage as fade bits for the level (side 0) and the next one (side 1); 0 = no fade.
            void FadeBits(float coverage, uint32_t& level, uint32_t& next)
            {
                level = next = 0;
                if (coverage >= 0.999f) return;
                uint32_t amount = (uint32_t)std::clamp((int)std::lround(coverage * 255.0f), 1, 254);
                level = amount;
                next = amount | (1u << 8);
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

        namespace
        {
            // The mesh levels (without the impostor).
            int MeshLevels(Scene& scene, Entity group, const LODGroupComponent& lod)
            {
                int levels = Generates(scene, group) ? 1 + std::min(lod.GeneratedLevels, 3) : std::min((int)scene.GetChildren(group).size(), 4);
                return lod.Impostor ? std::min(levels, 3) : levels;
            }
        }

        int LevelCount(Scene& scene, Entity group)
        {
            const auto* lod = scene.GetRegistry().TryGet<LODGroupComponent>(group);
            if (!lod) return 0;
            int levels = MeshLevels(scene, group, *lod);
            return lod->Impostor && levels > 0 ? levels + 1 : levels;
        }

        int ImpostorLevel(Scene& scene, Entity group)
        {
            const auto* lod = scene.GetRegistry().TryGet<LODGroupComponent>(group);
            return lod && lod->Impostor && MeshLevels(scene, group, *lod) > 0 ? MeshLevels(scene, group, *lod) : -1;
        }

        bool Bounds(Scene& scene, Entity e, glm::vec3& lo, glm::vec3& hi)
        {
            lo = glm::vec3(1e30f);
            hi = glm::vec3(-1e30f);
            GrowBounds(scene, e, lo, hi);
            return lo.x <= hi.x;
        }

        Pick Evaluate(Scene& scene, Entity group, const glm::vec3& cameraPosition, const glm::mat4& projection, bool orthographic, float* screenHeight)
        {
            Pick pick;
            auto& reg = scene.GetRegistry();
            const auto* lod = reg.TryGet<LODGroupComponent>(group);
            if (!lod) return pick;
            glm::vec3 center;
            float size = WorldSize(scene, group, &center);
            // A sphere of that size projected: the fraction of the view's height it spans.
            float height = orthographic ? size * projection[1][1] * 0.5f
                                        : size * std::abs(projection[1][1]) * 0.5f / std::max(glm::distance(center, cameraPosition), 1e-4f);
            height *= std::max(lod->Bias, 0.0f);
            if (screenHeight) *screenHeight = height;
            int levels = LevelCount(scene, group);
            const float below[4] = { lod->LOD1Below, lod->LOD2Below, lod->LOD3Below, lod->CullBelow };
            // Level i shows down to the next level's threshold, the last one down to Cull Below; within Fade Width
            // (a fraction of the threshold) above it, it fades into the next.
            pick.Level = -1;
            for (int i = 0; i < levels; ++i)
            {
                float floor = i == levels - 1 ? lod->CullBelow : below[i];
                if (height < floor) continue;
                pick.Level = i;
                pick.Next = i + 1 < levels ? i + 1 : -1;
                float band = floor * std::max(lod->FadeWidth, 0.0f);
                pick.Coverage = band > 0.0f ? std::clamp((height - floor) / band, 0.0f, 1.0f) : 1.0f;
                if (pick.Coverage >= 1.0f) pick.Next = -1;
                break;
            }
            return pick;
        }

        int Choose(Scene& scene, Entity group, const glm::vec3& cameraPosition, const glm::mat4& projection, bool orthographic, float* screenHeight)
        {
            return Evaluate(scene, group, cameraPosition, projection, orthographic, screenHeight).Level;
        }

        void Evaluate(Scene& scene, const glm::vec3& cameraPosition, const glm::mat4& projection, bool orthographic, Frame& frame)
        {
            auto& reg = scene.GetRegistry();
            std::vector<Entity> groups;
            for (auto [e, lod, t] : reg.ViewOf<LODGroupComponent, TransformComponent>())
                if (t.ActiveInHierarchy) groups.push_back(e);
            for (Entity g : groups)
            {
                Pick pick = Evaluate(scene, g, cameraPosition, projection, orthographic);
                uint32_t fade, nextFade;
                FadeBits(pick.Coverage, fade, nextFade);
                int impostor = ImpostorLevel(scene, g);
                // The impostor: drawn at its level (fading into nothing) or fading in after the last mesh.
                if (impostor >= 0 && pick.Level == impostor) frame.Impostors.push_back({ g, fade });
                else if (impostor >= 0 && pick.Next == impostor) frame.Impostors.push_back({ g, nextFade });
                if (Generates(scene, g))
                {
                    if (pick.Level < 0 || pick.Level == impostor)
                        MarkSubtree(scene, g, frame.Hidden);
                    else
                        frame.Generated[g.Index] = { pick.Level, pick.Next == impostor ? -1 : pick.Next, fade, nextFade };
                    continue;
                }
                const auto& children = scene.GetChildren(g);
                int meshLevels = impostor >= 0 ? impostor : (int)children.size();
                for (size_t i = 0; i < children.size(); ++i)
                {
                    if ((int)i >= meshLevels) MarkSubtree(scene, children[i], frame.Hidden);
                    else if ((int)i == pick.Level && fade) FadeSubtree(scene, children[i], fade, frame.Fade);
                    else if ((int)i == pick.Next) FadeSubtree(scene, children[i], nextFade, frame.Fade);
                    else if ((int)i != pick.Level) MarkSubtree(scene, children[i], frame.Hidden);
                }
            }
        }

        void Hidden(Scene& scene, const glm::vec3& cameraPosition, const glm::mat4& projection, bool orthographic, std::vector<uint8_t>& hidden)
        {
            Frame frame;
            frame.Hidden = std::move(hidden);
            Evaluate(scene, cameraPosition, projection, orthographic, frame);
            hidden = std::move(frame.Hidden);
        }

        AssetHandle GeneratedMesh(AssetHandle mesh, int level, float reduction, bool wait)
        {
            if (level <= 0) return mesh;
            struct Entry
            {
                std::weak_ptr<const MeshData> Source;
                AssetHandle Handle;
                std::shared_future<MeshData> Making; // simplifying on a worker thread
                std::weak_ptr<const MeshData> MakingFrom;
                bool Failed = false;                 // nothing came of it: the mesh itself stands in
            };
            static std::map<std::tuple<uint64_t, int, int>, Entry> cache;
            std::shared_ptr<const MeshData> data = AssetManager::GetMeshData(mesh);
            if (!data || data->Indices.empty()) return mesh;
            int quantized = (int)std::lround(std::clamp(reduction, 0.01f, 1.0f) * 1000.0f);
            Entry& entry = cache[{ (uint64_t)mesh, level, quantized }];
            if (entry.Source.lock() == data && (entry.Handle.IsValid() || entry.Failed)) return entry.Failed ? mesh : entry.Handle;
            // Started from another version of the mesh (reloaded since): start over.
            if (entry.Making.valid() && entry.MakingFrom.lock() != data) entry.Making = {};
            if (!entry.Making.valid())
            {
                float ratio = std::pow(quantized / 1000.0f, (float)level);
                entry.Making = std::async(std::launch::async, [data, ratio] { return MeshSimplify::Simplify(*data, ratio, 0.08f); }).share();
                entry.MakingFrom = data;
            }
            if (!wait && entry.Making.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
                return GeneratedMesh(mesh, level - 1, reduction, false); // a finer level meanwhile
            MeshData simple = entry.Making.get();
            entry.Making = {};
            if (entry.Handle.IsValid()) AssetManager::ReleaseRuntimeMesh(entry.Handle);
            entry.Source = data;
            entry.Failed = simple.Indices.empty();
            entry.Handle = entry.Failed ? AssetHandle(0ull) : AssetManager::RegisterRuntimeMesh(std::move(simple));
            return entry.Failed ? mesh : entry.Handle;
        }
    }
}
