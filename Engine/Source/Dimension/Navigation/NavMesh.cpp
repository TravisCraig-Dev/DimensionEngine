#include "Dimension/Navigation/NavMesh.h"
#include "Dimension/Scene/Scene.h"
#include "Dimension/Physics/Physics3D.h"
#include "Dimension/Project/Project.h"
#include "Dimension/Scene/ComponentRegistry.h"
#include "Dimension/Asset/AssetManager.h"
#include "Dimension/Core/Log.h"
#include <fstream>
#include <future>
#include <algorithm>
#include <cmath>
#include <limits>
#include <cstring>
#include <map>
#include <queue>
#include <set>
#include <unordered_map>

namespace Dimension
{
    glm::vec3 NavMeshData::NodePosition(int node) const
    {
        int c = Columns[node];
        return { Origin.x + (c % Width + 0.5f) * CellSize, Heights[node], Origin.y + (c / Width + 0.5f) * CellSize };
    }

    namespace Navigation
    {
        namespace
        {
            constexpr float kInf = std::numeric_limits<float>::max();

            int Opposite(int d) { return d < 4 ? (d + 2) % 4 : 4 + (d - 4 + 2) % 4; }

            // What a bake walks on: a Moving surface its own object's colliders and its children's (moving bodies too);
            // the others fixed geometry, leaving Moving surfaces' objects out. Never sensors, agents or characters.
            struct BakeScope
            {
                Entity Own = Entity::Null();
                std::vector<Entity> Moving;
            };

            bool Under(Scene& scene, Entity e, Entity root)
            {
                for (Entity x = e; !x.IsNull() && scene.IsValid(x); x = scene.GetParent(x))
                    if (x == root) return true;
                return false;
            }

            // The NavMesh Modifier that decides for a collider's object: its own, else the nearest ancestor's with Apply
            // To Children (an ancestor's without it stops the search).
            const NavMeshModifierComponent* ModifierOf(Scene& scene, Entity e)
            {
                auto& reg = scene.GetRegistry();
                for (Entity x = e; !x.IsNull() && scene.IsValid(x); x = scene.GetParent(x))
                    if (auto* m = reg.TryGet<NavMeshModifierComponent>(x)) return x == e || m->ApplyToChildren ? m : nullptr;
                return nullptr;
            }

            bool Ignored(Scene& scene, Entity e, const BakeScope& scope)
            {
                auto& reg = scene.GetRegistry();
                if (!scope.Own.IsNull())
                {
                    if (!Under(scene, e, scope.Own)) return true;
                }
                else
                {
                    if (auto* rb = reg.TryGet<RigidbodyComponent>(e); rb && rb->Type == BodyType::Dynamic) return true;
                    for (Entity m : scope.Moving)
                        if (Under(scene, e, m)) return true;
                }
                if (reg.Has<NavMeshAgentComponent>(e) || reg.Has<NavMeshObstacleComponent>(e) || reg.Has<CharacterControllerComponent>(e) || reg.Has<ClothComponent>(e) ||
                    reg.Has<SoftBodyComponent>(e) || reg.Has<RopeComponent>(e) || reg.Has<RagdollComponent>(e))
                    return true;
                if (auto* c = reg.TryGet<BoxColliderComponent>(e); c && c->IsTrigger) return true;
                if (auto* c = reg.TryGet<SphereColliderComponent>(e); c && c->IsTrigger) return true;
                if (auto* c = reg.TryGet<CapsuleColliderComponent>(e); c && c->IsTrigger) return true;
                if (auto* c = reg.TryGet<MeshColliderComponent>(e); c && c->IsTrigger) return true;
                if (const NavMeshModifierComponent* m = ModifierOf(scene, e); m && m->IgnoreFromBuild) return true;
                return false;
            }

            // Links every node to the node of each neighbouring column nearest its height (within Step Height), both
            // ways; diagonals only where the two ways around the corner both lead to the same node.
            void BuildLinks(NavMeshData& m)
            {
                int n = m.NodeCount();
                m.Links.assign((size_t)n * 8, -1);
                for (int a = 0; a < n; ++a)
                {
                    int c = m.Columns[a], x = c % m.Width, z = c / m.Width;
                    for (int d = 0; d < 4; ++d)
                    {
                        int nc = m.Column(x + kDirX[d], z + kDirZ[d]);
                        if (nc < 0) continue;
                        int best = -1;
                        float bestDy = m.StepHeight + 1e-4f;
                        for (uint32_t b = m.ColumnStart[nc]; b < m.ColumnStart[nc + 1]; ++b)
                        {
                            float dy = std::abs(m.Heights[b] - m.Heights[a]);
                            if (dy <= bestDy)
                            {
                                bestDy = dy;
                                best = (int)b;
                            }
                        }
                        m.Links[(size_t)a * 8 + d] = best;
                    }
                }
                for (int a = 0; a < n; ++a)
                    for (int d = 0; d < 4; ++d)
                    {
                        int b = m.Links[(size_t)a * 8 + d];
                        if (b >= 0 && m.Links[(size_t)b * 8 + Opposite(d)] != a) m.Links[(size_t)a * 8 + d] = -1;
                    }
                for (int a = 0; a < n; ++a)
                    for (int d = 4; d < 8; ++d)
                    {
                        int ox = kDirX[d] > 0 ? 0 : 2, oz = kDirZ[d] > 0 ? 1 : 3;
                        int p = m.Links[(size_t)a * 8 + ox], r = m.Links[(size_t)a * 8 + oz];
                        int q = p >= 0 ? m.Links[(size_t)p * 8 + oz] : -1, s = r >= 0 ? m.Links[(size_t)r * 8 + ox] : -1;
                        if (q >= 0 && q == s) m.Links[(size_t)a * 8 + d] = q;
                    }
            }

            // Distances along the mesh: from the nodes already given one (the rest kInf), on through the links.
            void Spread(const NavMeshData& m, std::vector<float>& dist)
            {
                using Item = std::pair<float, int>;
                std::priority_queue<Item, std::vector<Item>, std::greater<>> open;
                for (int a = 0; a < (int)dist.size(); ++a)
                    if (dist[a] < kInf) open.push({ dist[a], a });
                while (!open.empty())
                {
                    auto [da, a] = open.top();
                    open.pop();
                    if (da > dist[a]) continue;
                    for (int d = 0; d < 8; ++d)
                    {
                        int b = m.Links[(size_t)a * 8 + d];
                        float nd = da + (d < 4 ? m.CellSize : m.CellSize * 1.41421356f);
                        if (b >= 0 && nd < dist[b])
                        {
                            dist[b] = nd;
                            open.push({ nd, b });
                        }
                    }
                }
            }

            // Keeps only the nodes for which keep[] is set (and their Clearance).
            void Compact(NavMeshData& m, const std::vector<uint8_t>& keep)
            {
                std::vector<float> heights, clearance;
                std::vector<int32_t> columns;
                std::vector<uint32_t> starts(m.ColumnStart.size(), 0);
                for (size_t c = 0; c + 1 < m.ColumnStart.size(); ++c)
                {
                    starts[c] = (uint32_t)heights.size();
                    for (uint32_t a = m.ColumnStart[c]; a < m.ColumnStart[c + 1]; ++a)
                        if (keep[a])
                        {
                            heights.push_back(m.Heights[a]);
                            columns.push_back((int32_t)c);
                            if (!m.Clearance.empty()) clearance.push_back(m.Clearance[a]);
                        }
                }
                starts.back() = (uint32_t)heights.size();
                m.Heights = std::move(heights);
                m.Clearance = std::move(clearance);
                m.Columns = std::move(columns);
                m.ColumnStart = std::move(starts);
                BuildLinks(m);
            }

            void BuildIslands(NavMeshData& m)
            {
                int n = m.NodeCount();
                m.Islands.assign(n, -1);
                std::vector<int> stack;
                int island = 0;
                for (int s = 0; s < n; ++s)
                {
                    if (m.Islands[s] >= 0) continue;
                    m.Islands[s] = island;
                    stack.push_back(s);
                    while (!stack.empty())
                    {
                        int a = stack.back();
                        stack.pop_back();
                        for (int d = 0; d < 8; ++d)
                        {
                            int b = m.Links[(size_t)a * 8 + d];
                            if (b >= 0 && m.Islands[b] < 0)
                            {
                                m.Islands[b] = island;
                                stack.push_back(b);
                            }
                        }
                    }
                    ++island;
                }
            }

            // Links of the mesh's own: from each spot at an edge, looking straight out past it for the first ground to land
            // on: lower by more than a step and at most Drop Height (a drop, one way), or level across a gap up to Jump
            // Distance (both ways), with nothing in between. At most one per meter of edge and area landed in.
            void GenerateLinks(Scene& scene, NavMeshData& m, float dropHeight, float jumpDistance, const BakeScope& scope)
            {
                float c = m.CellSize;
                int reach = (int)std::ceil((jumpDistance + 2.0f * m.AgentRadius + c) / c);
                std::set<std::tuple<int, int, int, int>> taken;
                const glm::vec3 lift(0.0f, m.AgentHeight * 0.5f, 0.0f);
                for (int a = 0; a < m.NodeCount(); ++a)
                    for (int d = 0; d < 4; ++d)
                    {
                        if (m.Links[(size_t)a * 8 + d] >= 0) continue;
                        int col = m.Columns[a], x = col % m.Width, z = col / m.Width;
                        float ya = m.Heights[a];
                        for (int k = 1; k <= reach; ++k)
                        {
                            int nc = m.Column(x + kDirX[d] * k, z + kDirZ[d] * k);
                            if (nc < 0) break;
                            int land = -1;
                            bool across = false;
                            for (uint32_t b = m.ColumnStart[nc]; b < m.ColumnStart[nc + 1]; ++b)
                            {
                                float dy = ya - m.Heights[b];
                                if (dy > m.StepHeight && dy <= dropHeight && (land < 0 || m.Heights[b] > m.Heights[land])) land = (int)b;
                                else if (std::abs(dy) <= m.StepHeight && k >= 2 && k * c - 2.0f * m.AgentRadius - c <= jumpDistance)
                                {
                                    land = (int)b;
                                    across = true;
                                    break;
                                }
                            }
                            if (land < 0) continue;
                            glm::vec3 pa = m.NodePosition(a), pb = m.NodePosition(land);
                            auto key = std::make_tuple(d, (int)std::floor(pa.x), (int)std::floor(pa.z), m.Islands[land]);
                            if (taken.count(key)) break;
                            bool clear = true;
                            glm::vec3 dir = pb - pa;
                            for (const RaycastHit& h : Physics3DWorld::RaycastAll(scene, pa + lift, dir, glm::length(dir)))
                                if (!Ignored(scene, h.Hit, scope))
                                {
                                    clear = false;
                                    break;
                                }
                            if (!clear) break;
                            taken.insert(key);
                            m.AutoLinks.push_back({ a, land, across ? 0.5f : 0.3f });
                            if (across) m.AutoLinks.push_back({ land, a, 0.5f });
                            break;
                        }
                    }
            }

            // A surface's grid (cell size, columns, corner) for where it stands now.
            void GridOf(const NavMeshSurfaceComponent& s, const glm::vec3& center, NavMeshData& m)
            {
                float c = std::max(s.CellSize, 0.05f);
                glm::vec3 size = glm::max(s.Size, glm::vec3(0.1f));
                m.CellSize = c;
                m.Width = std::clamp((int)std::ceil(size.x / c), 1, 4096);
                m.Depth = std::clamp((int)std::ceil(size.z / c), 1, 4096);
                m.Origin = { center.x - m.Width * c * 0.5f, center.z - m.Depth * c * 0.5f };
            }

            // Modifier volumes, as boxes in their objects' space.
            struct Volume
            {
                glm::mat4 ToLocal;
                glm::vec3 Center, Half;
                int Area;
                float Cost;
                bool Contains(const glm::vec3& p) const
                {
                    glm::vec3 local = glm::vec3(ToLocal * glm::vec4(p, 1.0f)) - Center;
                    return glm::all(glm::lessThanEqual(glm::abs(local), Half));
                }
            };

            std::vector<Volume> VolumesOf(Scene& scene)
            {
                std::vector<Volume> volumes;
                for (auto [e, v, t] : scene.GetRegistry().ViewOf<NavMeshModifierVolumeComponent, TransformComponent>())
                    if (t.ActiveInHierarchy)
                    {
                        // Cost 0: the area's own (Project Settings, the Navigation window).
                        int area = std::clamp((int)v.Area, 0, kAreaCount - 1);
                        float cost = v.Cost > 0.0f ? v.Cost : Project::GetConfig().NavAreaCosts[area];
                        volumes.push_back({ glm::inverse(t.World), v.Center, glm::abs(v.Size) * 0.5f, area, std::max(cost, 0.01f) });
                    }
                return volumes;
            }

            // What sampling a column needs to know: the surface's box (top and height), its agent and slope limits.
            struct Sampler
            {
                float Top, Depth, AgentHeight, CosSlope;
                const std::vector<Volume>* Volumes;
                const BakeScope* Scope;
            };

            Sampler SamplerOf(const NavMeshSurfaceComponent& s, const glm::vec3& center, const std::vector<Volume>& volumes, const BakeScope& scope)
            {
                glm::vec3 size = glm::max(s.Size, glm::vec3(0.1f));
                return { center.y + size.y * 0.5f, size.y, std::max(s.AgentHeight, 0.1f), std::cos(glm::radians(std::clamp(s.MaxSlope, 0.0f, 89.0f))),
                         &volumes, &scope };
            }

            // The walkable surfaces under a column's center (x/z), top down, appended with their areas and costs. A
            // surface is walkable when it is flat enough, nothing seen above it is within Agent Height, and a ray up
            // finds no underside (or solid) that close.
            void SampleColumn(Scene& scene, const Sampler& k, glm::vec2 xz, std::vector<float>& heights, std::vector<uint8_t>& areas,
                              std::vector<float>& costs)
            {
                const glm::vec3 down(0.0f, -1.0f, 0.0f), up(0.0f, 1.0f, 0.0f);
                glm::vec3 p(xz.x, k.Top, xz.y);
                float above = kInf;
                for (const RaycastHit& h : Physics3DWorld::RaycastAll(scene, p, down, k.Depth, Physics::kDefaultQueryMask, true))
                {
                    if (Ignored(scene, h.Hit, *k.Scope)) continue;
                    float y = h.Point.y;
                    if (above - y < 0.02f) continue; // the same surface again (on a shared triangle edge)
                    bool walkable = h.Normal.y >= k.CosSlope && above - y >= k.AgentHeight;
                    above = y;
                    if (!walkable) continue;
                    bool blocked = false;
                    for (const RaycastHit& u : Physics3DWorld::RaycastAll(scene, h.Point + up * 0.02f, up, k.AgentHeight - 0.02f))
                        if (!Ignored(scene, u.Hit, *k.Scope))
                        {
                            blocked = true;
                            break;
                        }
                    if (blocked) continue;
                    bool notWalkable = false;
                    for (const Volume& v : *k.Volumes)
                        if (v.Area == kNotWalkable && v.Contains(h.Point)) notWalkable = true;
                    if (notWalkable) continue;
                    // The ground's own area, from a NavMesh Modifier on what it lies on.
                    uint8_t area = 255;
                    float cost = 1.0f;
                    if (const NavMeshModifierComponent* mod = ModifierOf(scene, h.Hit); mod && mod->OverrideArea)
                    {
                        area = (uint8_t)std::clamp((int)mod->Area, 0, kAreaCount - 1);
                        if (area == kNotWalkable) continue;
                        cost = std::max(mod->Cost > 0.0f ? mod->Cost : Project::GetConfig().NavAreaCosts[area], 0.01f);
                    }
                    heights.push_back(y);
                    areas.push_back(area);
                    costs.push_back(cost);
                }
            }

            // Makes the mesh from its sampled surfaces (Raw*, on its grid): the spots linked up, room for the agent taken
            // off the edges, areas and costs, islands, and the bake's own links.
            void FinishMesh(Scene& scene, const NavMeshSurfaceComponent& s, NavMeshData& mesh, const std::vector<Volume>& volumes, const BakeScope& scope)
            {
                NavMeshData* m = &mesh;
                float c = m->CellSize;
                m->ColumnStart = m->RawStart;
                m->Heights = m->RawHeights;
                m->Columns.resize(m->Heights.size());
                for (size_t col = 0; col + 1 < m->RawStart.size(); ++col)
                    for (uint32_t a = m->RawStart[col]; a < m->RawStart[col + 1]; ++a) m->Columns[a] = (int32_t)col;
                BuildLinks(*m);

                // Each spot's clearance: how far it is from an edge along the mesh (a node missing a side is half a cell
                // from it). Room for the agent: the spots nearer than Agent Radius go.
                int n = m->NodeCount();
                m->Clearance.assign(n, kInf);
                for (int a = 0; a < n; ++a)
                    for (int d = 0; d < 4; ++d)
                        if (m->Links[(size_t)a * 8 + d] < 0)
                        {
                            m->Clearance[a] = 0.5f * c;
                            break;
                        }
                Spread(*m, m->Clearance);
                // Each spot's area from its ground's modifier (kept in step with the spots compacted below).
                std::vector<uint8_t> nodeAreas = m->RawAreas;
                std::vector<float> nodeCosts = m->RawCosts;
                if (m->AgentRadius > 0.0f && n > 0)
                {
                    std::vector<uint8_t> keep(n);
                    for (int a = 0; a < n; ++a) keep[a] = m->Clearance[a] >= m->AgentRadius - 1e-4f;
                    Compact(*m, keep);
                    if (nodeAreas.size() == keep.size())
                    {
                        size_t w = 0;
                        for (size_t a = 0; a < keep.size(); ++a)
                            if (keep[a]) nodeAreas[w] = nodeAreas[a], nodeCosts[w] = nodeCosts[a], ++w;
                        nodeAreas.resize(w);
                        nodeCosts.resize(w);
                    }
                }
                n = m->NodeCount();
                // Areas and costs: the ground's modifier's, then the modifier volumes' (the last one containing a spot wins).
                m->Areas.assign(n, 0);
                m->Costs.assign(n, 1.0f);
                if (nodeAreas.size() == (size_t)n && nodeCosts.size() == (size_t)n)
                    for (int a = 0; a < n; ++a)
                        if (nodeAreas[a] != 255)
                        {
                            m->Areas[a] = nodeAreas[a];
                            m->Costs[a] = nodeCosts[a];
                        }
                for (int a = 0; a < n; ++a)
                    for (const Volume& v : volumes)
                        if (v.Area != kNotWalkable && v.Contains(m->NodePosition(a) + glm::vec3(0.0f, 0.05f, 0.0f)))
                        {
                            m->Areas[a] = (uint8_t)v.Area;
                            m->Costs[a] = v.Cost;
                        }
                m->MinCost = 1.0f;
                for (float cost : m->Costs) m->MinCost = std::min(m->MinCost, cost);
                BuildIslands(*m);
                if (s.GenerateLinks) GenerateLinks(scene, *m, std::max(s.DropHeight, 0.0f), std::max(s.JumpDistance, 0.0f), scope);
            }

            void SetAgentSizes(const NavMeshSurfaceComponent& s, NavMeshData& m)
            {
                m.AgentRadius = std::max(s.AgentRadius, 0.0f);
                m.AgentHeight = std::max(s.AgentHeight, 0.1f);
                m.StepHeight = std::max(s.StepHeight, 0.0f);
            }

            // Bakes a surface. With `previous` (the same grid) and `region` (columns x0, z0, x1, z1), only the region is
            // sampled and the rest kept.
            std::shared_ptr<NavMeshData> BakeIn(Scene& scene, const NavMeshSurfaceComponent& surface, const glm::vec3& center, const BakeScope& scope,
                                                const NavMeshData* previous = nullptr, glm::ivec4 region = glm::ivec4(0))
            {
                const NavMeshSurfaceComponent s = WithAgentType(surface); // (an agent type's sizes)
                auto m = std::make_shared<NavMeshData>();
                GridOf(s, center, *m);
                SetAgentSizes(s, *m);
                float c = m->CellSize;
                std::vector<Volume> volumes = VolumesOf(scene);
                Sampler sampler = SamplerOf(s, center, volumes, scope);
                m->RawStart.reserve((size_t)m->Width * m->Depth + 1);
                if (previous && previous->RawStart.size() != (size_t)m->Width * m->Depth + 1) previous = nullptr;
                for (int z = 0; z < m->Depth; ++z)
                    for (int x = 0; x < m->Width; ++x)
                    {
                        m->RawStart.push_back((uint32_t)m->RawHeights.size());
                        if (previous && (x < region.x || z < region.y || x > region.z || z > region.w))
                        {
                            int col = z * m->Width + x;
                            uint32_t from = previous->RawStart[col], to = previous->RawStart[col + 1];
                            m->RawHeights.insert(m->RawHeights.end(), previous->RawHeights.begin() + from, previous->RawHeights.begin() + to);
                            bool areas = previous->RawAreas.size() == previous->RawHeights.size() && previous->RawCosts.size() == previous->RawHeights.size();
                            for (uint32_t k = from; k < to; ++k)
                            {
                                m->RawAreas.push_back(areas ? previous->RawAreas[k] : 255);
                                m->RawCosts.push_back(areas ? previous->RawCosts[k] : 1.0f);
                            }
                            continue;
                        }
                        SampleColumn(scene, sampler, { m->Origin.x + (x + 0.5f) * c, m->Origin.y + (z + 0.5f) * c }, m->RawHeights, m->RawAreas, m->RawCosts);
                    }
                m->RawStart.push_back((uint32_t)m->RawHeights.size());
                FinishMesh(scene, s, *m, volumes, scope);
                return m;
            }

            // The node `node` is linked to in the column (ox, oz) away (-1..1 each); -1 = none.
            int NodeAt(const NavMeshData& m, int node, int ox, int oz)
            {
                if (ox == 0 && oz == 0) return node;
                for (int d = 0; d < 8; ++d)
                    if (kDirX[d] == ox && kDirZ[d] == oz) return m.Links[(size_t)node * 8 + d];
                return -1;
            }

            glm::ivec2 CellOf(const NavMeshData& m, glm::vec2 p)
            {
                return { (int)std::floor((p.x - m.Origin.x) / m.CellSize), (int)std::floor((p.y - m.Origin.y) / m.CellSize) };
            }

            glm::vec3 OnMesh(const NavMeshData& m, int node, glm::vec2 p)
            {
                // Inside the node's column the point keeps its x/z; else it is the node's spot.
                glm::ivec2 cell = CellOf(m, p);
                if (m.Column(cell.x, cell.y) != m.Columns[node]) return m.NodePosition(node);
                return { p.x, HeightAt(m, node, p.x, p.y), p.y };
            }

            NavPathStatus PathFrom(const NavMeshData& m, int start, const glm::vec3& from, const glm::vec3& to, std::vector<glm::vec3>& corners,
                                   const std::vector<Link>& links, std::vector<float>* linkArcs, const Filter& filter,
                                   std::vector<Entity>* linkOwners = nullptr)
            {
                corners.clear();
                if (linkArcs) linkArcs->clear();
                if (linkOwners) linkOwners->clear();
                if (start < 0 || start >= m.NodeCount() || m.IsBlocked(start)) return NavPathStatus::Invalid;
                auto cost = [&](int node) { return m.Costs.empty() ? 1.0f : m.Costs[node]; };
                int n = m.NodeCount();
                // The areas reachable from the start's, across off-mesh links.
                int islandCount = 0;
                for (int id : m.Islands) islandCount = std::max(islandCount, id + 1);
                std::vector<uint8_t> reachable(islandCount + 1, 0);
                reachable[m.Islands[start]] = 1;
                for (bool grew = true; grew;)
                {
                    grew = false;
                    for (const Link& l : links)
                        if (reachable[m.Islands[l.From]] && !reachable[m.Islands[l.To]]) reachable[m.Islands[l.To]] = grew = true;
                }
                NavPathStatus status = NavPathStatus::Complete;
                int goal = Nearest(m, to, std::max(10.0f, m.AgentHeight * 2.0f), filter);
                if (goal < 0 || !reachable[m.Islands[goal]])
                {
                    // Out of reach: as near to it as the reachable areas get.
                    status = NavPathStatus::Partial;
                    float best = kInf;
                    for (int a = 0; a < n; ++a)
                        if (reachable[m.Islands[a]] && filter.Passes(m, a))
                        {
                            glm::vec3 d = m.NodePosition(a) - to;
                            float d2 = glm::dot(d, d);
                            if (d2 < best)
                            {
                                best = d2;
                                goal = a;
                            }
                        }
                }

                // A* over the nodes and the off-mesh links.
                std::unordered_map<int, std::vector<int>> linksFrom;
                for (int k = 0; k < (int)links.size(); ++k) linksFrom[links[k].From].push_back(k);
                // Search state per node, kept between searches (a crowd searches many times a second): a node's entries
                // count only when stamped with this search's number.
                struct SearchState
                {
                    std::vector<float> G;
                    std::vector<int> Parent, ParentLink;
                    std::vector<uint32_t> Stamp, Closed;
                    uint32_t Number = 0;
                };
                static thread_local SearchState state;
                if (state.Stamp.size() < (size_t)n)
                {
                    state.G.resize(n);
                    state.Parent.resize(n);
                    state.ParentLink.resize(n);
                    state.Stamp.assign(n, 0);
                    state.Closed.assign(n, 0);
                    state.Number = 0;
                }
                if (++state.Number == 0)
                {
                    std::fill(state.Stamp.begin(), state.Stamp.end(), 0u);
                    std::fill(state.Closed.begin(), state.Closed.end(), 0u);
                    state.Number = 1;
                }
                const uint32_t search = state.Number;
                struct GView
                {
                    SearchState& S;
                    uint32_t Search;
                    float operator[](int i) const { return S.Stamp[i] == Search ? S.G[i] : kInf; }
                };
                auto setG = [&](int i, float v, int from, int link) {
                    state.Stamp[i] = search;
                    state.G[i] = v;
                    state.Parent[i] = from;
                    state.ParentLink[i] = link;
                };
                GView g{ state, search };
                auto isClosed = [&](int i) { return state.Closed[i] == search; };
                auto parentOf = [&](int i) { return state.Stamp[i] == search ? state.Parent[i] : -1; };
                auto linkOf = [&](int i) { return state.Stamp[i] == search ? state.ParentLink[i] : -1; };
                using Item = std::pair<float, int>;
                std::priority_queue<Item, std::vector<Item>, std::greater<>> open;
                glm::vec3 goalPos = m.NodePosition(goal);
                float h = std::min(m.MinCost, 1.0f); // keeps the estimate below the real cost on cheap ground
                setG(start, 0.0f, -1, -1);
                open.push({ glm::distance(m.NodePosition(start), goalPos) * h, start });
                // The spot reached nearest the goal, should the goal not be reached (too narrow for the agent, or
                // only through areas it leaves out).
                int nearestReached = start;
                float nearestD = glm::distance(m.NodePosition(start), to);
                while (!open.empty())
                {
                    int a = open.top().second;
                    open.pop();
                    if (isClosed(a)) continue;
                    state.Closed[a] = search;
                    if (a == goal) break;
                    glm::vec3 pa = m.NodePosition(a);
                    if (float d = glm::distance(pa, to); d < nearestD)
                    {
                        nearestD = d;
                        nearestReached = a;
                    }
                    auto relax = [&](int b, float edge, int link) {
                        if (b < 0 || isClosed(b) || !filter.Passes(m, b)) return;
                        float nd = g[a] + edge;
                        if (nd < g[b])
                        {
                            setG(b, nd, a, link);
                            open.push({ nd + glm::distance(m.NodePosition(b), goalPos) * h, b });
                        }
                    };
                    for (int d = 0; d < 8; ++d)
                        if (int b = m.Links[(size_t)a * 8 + d]; b >= 0) relax(b, glm::distance(pa, m.NodePosition(b)) * 0.5f * (cost(a) + cost(b)), -1);
                    if (auto it = linksFrom.find(a); it != linksFrom.end())
                        for (int k : it->second) relax(links[k].To, links[k].Cost, k);
                }
                if (!isClosed(goal))
                {
                    status = NavPathStatus::Partial;
                    goal = nearestReached;
                }
                std::vector<int> nodes, arrivedBy; // arrivedBy: the link that led to the node (-1 = walked)
                for (int a = goal; a >= 0; a = parentOf(a))
                {
                    nodes.push_back(a);
                    arrivedBy.push_back(linkOf(a));
                }
                std::reverse(nodes.begin(), nodes.end());
                std::reverse(arrivedBy.begin(), arrivedBy.end());

                // String pulling, between the links: from each corner, straight on to the farthest node in sight.
                glm::vec3 begin = OnMesh(m, start, { from.x, from.z });
                glm::vec3 end = status == NavPathStatus::Complete ? OnMesh(m, goal, { to.x, to.z }) : m.NodePosition(goal);
                corners.push_back(begin);
                std::vector<float> arcs{ -1.0f };
                std::vector<Entity> owners{ Entity::Null() }; // per corner: the Off Mesh Link crossed from it (Null = none)
                size_t runStart = 0;
                glm::vec3 runBegin = begin;
                while (true)
                {
                    size_t runEnd = runStart;
                    while (runEnd + 1 < nodes.size() && arrivedBy[runEnd + 1] < 0) ++runEnd;
                    bool lastRun = runEnd + 1 >= nodes.size();
                    const Link* link = lastRun ? nullptr : &links[arrivedBy[runEnd + 1]];
                    glm::vec3 runFinish = lastRun ? end : OnMesh(m, nodes[runEnd], { link->FromPoint.x, link->FromPoint.z });
                    auto point = [&](size_t k) { return k == runEnd ? runFinish : m.NodePosition(nodes[k]); };
                    size_t i = runStart;
                    glm::vec2 cur(runBegin.x, runBegin.z);
                    while (i < runEnd)
                    {
                        size_t far = i + 1;
                        for (size_t k = i + 2; k <= runEnd; ++k)
                        {
                            glm::vec3 pk = point(k);
                            int endNode = -1;
                            if (!Walk(m, nodes[i], cur, { pk.x, pk.z }, &endNode, nullptr, filter) || endNode != nodes[k] || cost(nodes[k]) != cost(nodes[i])) break;
                            far = k;
                        }
                        corners.push_back(point(far));
                        arcs.push_back(-1.0f);
                        owners.push_back(Entity::Null());
                        i = far;
                        cur = { corners.back().x, corners.back().z };
                    }
                    if (glm::distance(corners.back(), runFinish) > 1e-4f)
                    {
                        corners.push_back(runFinish);
                        arcs.push_back(-1.0f);
                        owners.push_back(Entity::Null());
                    }
                    if (lastRun) break;
                    // Across the link: the corner before it starts the crossing.
                    arcs.back() = link->Arc;
                    owners.back() = link->Owner;
                    runStart = runEnd + 1;
                    runBegin = OnMesh(m, nodes[runStart], { link->ToPoint.x, link->ToPoint.z });
                    corners.push_back(runBegin);
                    arcs.push_back(-1.0f);
                        owners.push_back(Entity::Null());
                }
                if (corners.size() == 1)
                {
                    corners.push_back(end);
                    arcs.push_back(-1.0f);
                        owners.push_back(Entity::Null());
                }
                if (linkArcs) *linkArcs = std::move(arcs);
                if (linkOwners) *linkOwners = std::move(owners);
                return status;
            }

            Filter FilterOf(const NavMeshAgentComponent& agent) { return { agent.Radius, (uint32_t)agent.AreaMask, agent.AgentType }; }

            NavMeshAgentComponent* AgentOf(Scene& scene, Entity e) { return scene.IsValid(e) ? scene.GetRegistry().TryGet<NavMeshAgentComponent>(e) : nullptr; }

            // The Moving surface whose mesh this is (null: a fixed one, or none).
            const NavMeshSurfaceComponent* MovingSurfaceOf(Scene& scene, const NavMeshData* mesh)
            {
                if (!mesh) return nullptr;
                for (auto [e, s] : scene.GetRegistry().ViewOf<NavMeshSurfaceComponent>())
                    if (s.Moving && (s.Data.get() == mesh || s.Baked.get() == mesh)) return &s;
                return nullptr;
            }

            // A point between the world and a mesh's space (as baked; the same for fixed surfaces).
            glm::vec3 ToMesh(Scene& scene, const NavMeshData* mesh, const glm::vec3& world)
            {
                const NavMeshSurfaceComponent* s = MovingSurfaceOf(scene, mesh);
                return s ? glm::vec3(s->FrameInverse * glm::vec4(world, 1.0f)) : world;
            }
            glm::vec3 ToWorld(Scene& scene, const NavMeshData* mesh, const glm::vec3& local)
            {
                const NavMeshSurfaceComponent* s = MovingSurfaceOf(scene, mesh);
                return s ? glm::vec3(s->Frame * glm::vec4(local, 1.0f)) : local;
            }
            // How far a mesh's space is turned about the vertical from the world's.
            float YawOf(const glm::mat4& frame) { return std::atan2(frame[2].x, frame[2].z); }

            // Where the agent stands, in its mesh's space.
            glm::vec3 GroundOf(Scene& scene, Entity e, const NavMeshAgentComponent& agent)
            {
                glm::vec3 at = !agent.UpdatePosition && agent.NextValid ? agent.NextPosition
                                                                         : scene.GetRegistry().Get<TransformComponent>(e).GetWorldPosition();
                return ToMesh(scene, agent.Mesh.get(), at) - glm::vec3(0.0f, agent.BaseOffset, 0.0f);
            }

            // Moves the object to a world position (agents are roots; under a parent it is converted).
            void PlaceObject(Scene& scene, Entity e, const glm::vec3& world)
            {
                auto& reg = scene.GetRegistry();
                auto& t = reg.Get<TransformComponent>(e);
                Entity parent = scene.GetParent(e);
                t.Position = parent.IsNull() ? world : glm::vec3(glm::inverse(reg.Get<TransformComponent>(parent).World) * glm::vec4(world, 1.0f));
                t.World[3] = glm::vec4(world, 1.0f);
            }

            // Stands the agent at a ground spot of its mesh (Base Offset above it): its NextPosition, and its object unless a
            // script moves that itself (UpdatePosition off).
            void PlaceAgent(Scene& scene, Entity e, NavMeshAgentComponent& agent, const glm::vec3& ground)
            {
                agent.NextPosition = ToWorld(scene, agent.Mesh.get(), ground + glm::vec3(0.0f, agent.BaseOffset, 0.0f));
                agent.NextValid = true;
                if (agent.UpdatePosition) PlaceObject(scene, e, agent.NextPosition);
            }

            // Finds the agent's mesh and the node it stands on (keeping it where it is when that is on the mesh).
            bool Place(Scene& scene, Entity e, NavMeshAgentComponent& agent)
            {
                glm::ivec2 cell;
                glm::vec3 ground = GroundOf(scene, e, agent);
                if (agent.Mesh && agent.Node >= 0 && agent.Node < agent.Mesh->NodeCount() && FilterOf(agent).Passes(*agent.Mesh, agent.Node))
                {
                    // Still in (or next to) its node's column? Otherwise it was moved: look again.
                    cell = CellOf(*agent.Mesh, { ground.x, ground.z });
                    int col = agent.Mesh->Columns[agent.Node];
                    if (std::abs(cell.x - col % agent.Mesh->Width) <= 1 && std::abs(cell.y - col / agent.Mesh->Width) <= 1) return true;
                }
                int node = -1;
                glm::vec3 world = ToWorld(scene, agent.Mesh.get(), ground);
                auto previous = agent.Mesh;
                agent.Mesh = MeshNear(scene, world, std::max(agent.Height, 2.0f), &node, FilterOf(agent));
                if (!agent.Mesh) // nowhere roomy enough: anywhere (of its type)
                {
                    Filter anyRoom;
                    anyRoom.AgentType = agent.AgentType;
                    agent.Mesh = MeshNear(scene, world, std::max(agent.Height, 2.0f), &node, anyRoom);
                }
                agent.Node = agent.Mesh ? node : -1;
                if (agent.Mesh != previous)
                {
                    agent.HasMeshDestination = false; // another space
                    agent.Placed = false;
                }
                if (agent.Node < 0) return false;
                glm::vec3 local = ToMesh(scene, agent.Mesh.get(), world);
                PlaceAgent(scene, e, agent, OnMesh(*agent.Mesh, agent.Node, { local.x, local.z }));
                return true;
            }

            void Repath(Scene& scene, Entity e, NavMeshAgentComponent& agent)
            {
                agent.PathDirty = false;
                agent.Corner = 1;
                if (!Place(scene, e, agent))
                {
                    agent.Path.clear();
                    agent.PathStatus = NavPathStatus::Invalid;
                    return;
                }
                if (!agent.HasMeshDestination)
                {
                    // The mesh the destination lies on (its own when it lies on none near).
                    agent.DestinationMesh = MeshNear(scene, agent.Destination, 2.0f, nullptr, FilterOf(agent));
                    if (!agent.DestinationMesh) agent.DestinationMesh = agent.Mesh;
                    agent.MeshDestination = ToMesh(scene, agent.DestinationMesh.get(), agent.Destination);
                    agent.HasMeshDestination = true;
                }
                agent.CrossMesh.reset();
                agent.CrossLink = Entity::Null();
                agent.Waiting = false;
                glm::vec3 ground = GroundOf(scene, e, agent);
                std::vector<Link> links = LinksOn(scene, *agent.Mesh);
                if (agent.DestinationMesh == agent.Mesh)
                {
                    agent.PathStatus = PathFrom(*agent.Mesh, agent.Node, ground, agent.MeshDestination, agent.Path, links, &agent.PathLinks, FilterOf(agent),
                                                &agent.PathLinkOwners);
                    return;
                }
                // On another surface: to the start of the off-mesh link over to it that makes the shortest way (to the
                // link, across, and straight on to the destination), then across.
                glm::vec3 goal = ToWorld(scene, agent.DestinationMesh.get(), agent.MeshDestination);
                float best = kInf;
                std::vector<glm::vec3> corners;
                std::vector<float> arcs;
                std::vector<Entity> owners;
                for (auto [l, link, t] : scene.GetRegistry().ViewOf<OffMeshLinkComponent, TransformComponent>())
                {
                    if ((!link.Activated && !link.AgentsWait) || !t.ActiveInHierarchy) continue;
                    for (int way = 0; way < (link.Bidirectional ? 2 : 1); ++way)
                    {
                        glm::vec3 fromLocal = way == 0 ? link.Start : link.End, toLocal = way == 0 ? link.End : link.Start;
                        glm::vec3 a = glm::vec3(t.World * glm::vec4(fromLocal, 1.0f)), b = glm::vec3(t.World * glm::vec4(toLocal, 1.0f));
                        float reach = std::max(1.0f, agent.Mesh->AgentRadius + agent.Mesh->CellSize * 2.0f);
                        glm::vec3 aMesh = ToMesh(scene, agent.Mesh.get(), a);
                        if (Nearest(*agent.Mesh, aMesh, reach, FilterOf(agent)) < 0 ||
                            Nearest(*agent.DestinationMesh, ToMesh(scene, agent.DestinationMesh.get(), b), reach, FilterOf(agent)) < 0)
                            continue;
                        std::vector<glm::vec3> c;
                        std::vector<float> arc;
                        std::vector<Entity> own;
                        if (PathFrom(*agent.Mesh, agent.Node, ground, aMesh, c, links, &arc, FilterOf(agent), &own) != NavPathStatus::Complete) continue;
                        float length = 0.0f;
                        for (size_t k = 0; k + 1 < c.size(); ++k) length += glm::distance(c[k], c[k + 1]);
                        length += glm::distance(a, b) * std::max(link.CostModifier, 0.0f) + glm::distance(b, goal);
                        if (length >= best) continue;
                        best = length;
                        // The link's far end, in this mesh's space (where it is now; taken again as it is crossed).
                        c.push_back(ToMesh(scene, agent.Mesh.get(), b));
                        arc.back() = std::max(link.ArcHeight, 0.0f);
                        own.back() = l;
                        arc.push_back(-1.0f);
                        own.push_back(Entity::Null());
                        corners = std::move(c);
                        arcs = std::move(arc);
                        owners = std::move(own);
                        agent.CrossLink = l;
                        agent.CrossEnd = toLocal;
                    }
                }
                if (corners.empty())
                {
                    // No way over: as near as it gets on its own surface.
                    PathFrom(*agent.Mesh, agent.Node, ground, ToMesh(scene, agent.Mesh.get(), goal), agent.Path, links, &agent.PathLinks, FilterOf(agent),
                             &agent.PathLinkOwners);
                    agent.PathStatus = agent.Path.empty() ? NavPathStatus::Invalid : NavPathStatus::Partial;
                    return;
                }
                agent.Path = std::move(corners);
                agent.PathLinks = std::move(arcs);
                agent.PathLinkOwners = std::move(owners);
                agent.CrossMesh = agent.DestinationMesh;
                agent.PathStatus = NavPathStatus::Complete;
            }

            float Horizontal(const glm::vec3& a, const glm::vec3& b) { return glm::length(glm::vec2(a.x - b.x, a.z - b.z)); }

            float WrapAngle(float a)
            {
                a = std::fmod(a + glm::pi<float>(), glm::two_pi<float>());
                if (a < 0.0f) a += glm::two_pi<float>();
                return a - glm::pi<float>();
            }
        }

        int Nearest(const NavMeshData& m, const glm::vec3& p, float maxDistance, const Filter& filter)
        {
            if (m.NodeCount() == 0 || maxDistance < 0.0f) return -1;
            float c = m.CellSize;
            glm::ivec2 center = CellOf(m, { p.x, p.z });
            int rings = std::min((int)std::ceil(maxDistance / c) + 1, 1024);
            int best = -1;
            float bestD2 = maxDistance * maxDistance + 1e-6f; // ties go to the first found: the column the point is in
            auto visit = [&](int x, int z) {
                int col = m.Column(x, z);
                if (col < 0) return;
                // Distance to the column's square, plus the height.
                float x0 = m.Origin.x + x * c, z0 = m.Origin.y + z * c;
                float dx = std::max({ x0 - p.x, 0.0f, p.x - (x0 + c) }), dz = std::max({ z0 - p.z, 0.0f, p.z - (z0 + c) });
                for (uint32_t a = m.ColumnStart[col]; a < m.ColumnStart[col + 1]; ++a)
                {
                    float dy = m.Heights[a] - p.y;
                    float d2 = dx * dx + dz * dz + dy * dy;
                    if (d2 < bestD2 && filter.Passes(m, (int)a))
                    {
                        bestD2 = d2;
                        best = (int)a;
                    }
                }
            };
            for (int ring = 0; ring <= rings; ++ring)
            {
                float nearest = (ring - 1) * c;
                if (ring > 1 && nearest * nearest > bestD2) break;
                // Rings wholly off the grid hold nothing (farther ones may reach it).
                if (center.x + ring < 0 || center.x - ring >= m.Width || center.y + ring < 0 || center.y - ring >= m.Depth) continue;
                if (ring == 0)
                {
                    visit(center.x, center.y);
                    continue;
                }
                int x0 = std::max(center.x - ring, -1), x1 = std::min(center.x + ring, m.Width);
                for (int x = x0; x <= x1; ++x)
                {
                    visit(x, center.y - ring);
                    visit(x, center.y + ring);
                }
                int z0 = std::max(center.y - ring + 1, -1), z1 = std::min(center.y + ring - 1, m.Depth);
                for (int z = z0; z <= z1; ++z)
                {
                    visit(center.x - ring, z);
                    visit(center.x + ring, z);
                }
            }
            return best;
        }

        float HeightAt(const NavMeshData& m, int node, float x, float z)
        {
            float c = m.CellSize;
            float fx = (x - m.Origin.x) / c - 0.5f, fz = (z - m.Origin.y) / c - 0.5f;
            int x0 = (int)std::floor(fx), z0 = (int)std::floor(fz);
            float tx = fx - x0, tz = fz - z0;
            int col = m.Columns[node], nx = col % m.Width, nz = col / m.Width;
            float h[2][2];
            for (int j = 0; j < 2; ++j)
                for (int i = 0; i < 2; ++i)
                {
                    int ox = x0 + i - nx, oz = z0 + j - nz;
                    int other = std::abs(ox) <= 1 && std::abs(oz) <= 1 ? NodeAt(m, node, ox, oz) : -1;
                    h[j][i] = m.Heights[other >= 0 ? other : node];
                }
            return glm::mix(glm::mix(h[0][0], h[0][1], tx), glm::mix(h[1][0], h[1][1], tx), tz);
        }

        bool Walk(const NavMeshData& m, int startNode, glm::vec2 from, glm::vec2 to, int* endNode, glm::vec2* reached, const Filter& filter)
        {
            int node = startNode;
            int col = m.Columns[node];
            int x = col % m.Width, z = col / m.Width;
            glm::vec2 g0 = (from - m.Origin) / m.CellSize, g1 = (to - m.Origin) / m.CellSize;
            int tx = (int)std::floor(g1.x), tz = (int)std::floor(g1.y);
            glm::vec2 d = g1 - g0;
            int sx = d.x > 0.0f ? 1 : -1, sz = d.y > 0.0f ? 1 : -1;
            // Grid traversal (Amanatides-Woo): which column boundary the segment crosses next, as a fraction of it.
            float tMaxX = std::abs(d.x) > 1e-9f ? std::max(0.0f, (sx > 0 ? x + 1 - g0.x : g0.x - x) / std::abs(d.x)) : kInf;
            float tMaxZ = std::abs(d.y) > 1e-9f ? std::max(0.0f, (sz > 0 ? z + 1 - g0.y : g0.y - z) / std::abs(d.y)) : kInf;
            float tDeltaX = std::abs(d.x) > 1e-9f ? 1.0f / std::abs(d.x) : kInf, tDeltaZ = std::abs(d.y) > 1e-9f ? 1.0f / std::abs(d.y) : kInf;
            int guard = std::abs(tx - x) + std::abs(tz - z) + 4;
            while ((x != tx || z != tz) && guard-- > 0)
            {
                bool stepX = z == tz || (x != tx && tMaxX < tMaxZ);
                int dir;
                float t;
                if (stepX)
                {
                    dir = sx > 0 ? 0 : 2;
                    t = tMaxX;
                    tMaxX += tDeltaX;
                }
                else
                {
                    dir = sz > 0 ? 1 : 3;
                    t = tMaxZ;
                    tMaxZ += tDeltaZ;
                }
                int next = m.Links[(size_t)node * 8 + dir];
                if (next < 0 || !filter.Passes(m, next))
                {
                    if (endNode) *endNode = node;
                    if (reached)
                    {
                        float len = glm::length(to - from);
                        *reached = from + (to - from) * std::clamp(t - 1e-3f / std::max(len, 1e-6f), 0.0f, 1.0f);
                    }
                    return false;
                }
                node = next;
                if (stepX)
                    x += sx;
                else
                    z += sz;
            }
            if (endNode) *endNode = node;
            if (reached) *reached = to;
            return true;
        }

        NavPathStatus FindPath(const NavMeshData& mesh, const glm::vec3& from, const glm::vec3& to, std::vector<glm::vec3>& corners,
                               const std::vector<Link>& links, std::vector<float>* linkArcs, const Filter& filter)
        {
            return PathFrom(mesh, Nearest(mesh, from, std::max(mesh.AgentHeight, 2.0f), filter), from, to, corners, links, linkArcs, filter);
        }

        std::vector<std::pair<glm::vec3, glm::vec3>> LinkCrossings(const glm::mat4& world, const OffMeshLinkComponent& link, float spacing)
        {
            std::vector<std::pair<glm::vec3, glm::vec3>> out;
            glm::vec3 along = link.End - link.Start;
            glm::vec3 side = glm::cross(glm::vec3(0.0f, 1.0f, 0.0f), along); // (in the object's space, level)
            side.y = 0.0f;
            float width = std::max(link.Width, 0.0f);
            if (width <= 1e-4f || glm::dot(side, side) < 1e-10f)
            {
                out.push_back({ glm::vec3(world * glm::vec4(link.Start, 1.0f)), glm::vec3(world * glm::vec4(link.End, 1.0f)) });
                return out;
            }
            side = glm::normalize(side);
            // The spacing is measured in the world (the object may be scaled).
            float worldWidth = width * glm::length(glm::vec3(world * glm::vec4(side, 0.0f)));
            int count = std::clamp((int)std::ceil(worldWidth / std::max(spacing, 0.05f)) + 1, 2, 256);
            for (int i = 0; i < count; ++i)
            {
                glm::vec3 offset = side * (width * ((float)i / (count - 1) - 0.5f));
                out.push_back({ glm::vec3(world * glm::vec4(link.Start + offset, 1.0f)), glm::vec3(world * glm::vec4(link.End + offset, 1.0f)) });
            }
            return out;
        }

        std::vector<Link> LinksOn(Scene& scene, const NavMeshData& mesh)
        {
            std::vector<Link> out;
            const NavMeshSurfaceComponent* moving = MovingSurfaceOf(scene, &mesh);
            glm::mat4 toMesh = moving ? moving->FrameInverse : glm::mat4(1.0f);
            for (auto [e, link, t] : scene.GetRegistry().ViewOf<OffMeshLinkComponent, TransformComponent>())
            {
                // Closed (not Activated): gone, unless agents wait at it (a door) - then paths still go through.
                if ((!link.Activated && !link.AgentsWait) || !t.ActiveInHierarchy) continue;
                // A wide link: a crossing every couple of cells across it.
                for (auto [a, b] : LinkCrossings(toMesh * t.World, link, std::max(mesh.CellSize * 2.0f, 0.5f)))
                {
                    // Each end joins the mesh within a meter (or the agent's radius past the edge it was baked back from).
                    float reach = std::max(1.0f, mesh.AgentRadius + mesh.CellSize * 2.0f);
                    int na = Nearest(mesh, a, reach), nb = Nearest(mesh, b, reach);
                    if (na < 0 || nb < 0 || na == nb) continue;
                    float cost = glm::distance(a, b) * std::max(link.CostModifier, 0.0f);
                    out.push_back({ na, nb, a, b, cost, std::max(link.ArcHeight, 0.0f), e, !link.Activated });
                    if (link.Bidirectional) out.push_back({ nb, na, b, a, cost, std::max(link.ArcHeight, 0.0f), e, !link.Activated });
                }
            }
            // The bake's own (a little dearer than walking the same length, so they are used where they save a way round).
            for (const NavMeshData::AutoLink& l : mesh.AutoLinks)
            {
                if (mesh.IsBlocked(l.From) || mesh.IsBlocked(l.To)) continue;
                glm::vec3 a = mesh.NodePosition(l.From), b = mesh.NodePosition(l.To);
                out.push_back({ l.From, l.To, a, b, glm::distance(a, b) * 1.5f + 0.5f, l.Arc });
            }
            return out;
        }

        std::shared_ptr<NavMeshData> Carve(const NavMeshData& baked, const std::vector<CarveShape>& obstacles)
        {
            auto m = std::make_shared<NavMeshData>(baked);
            int n = m->NodeCount();
            m->Blocked.assign(n, 0);
            float c = m->CellSize, radius = m->AgentRadius, height = m->AgentHeight;
            for (const CarveShape& o : obstacles)
            {
                // The shape in the world: a box's center and half axes, or a capsule's axis and radius.
                glm::vec3 center = glm::vec3(o.World * glm::vec4(o.Center, 1.0f));
                glm::vec3 axes[3];
                for (int k = 0; k < 3; ++k) axes[k] = glm::vec3(o.World[k]);
                glm::vec3 half = o.Size * 0.5f;
                float capRadius = o.Radius * std::max(glm::length(axes[0]), glm::length(axes[2]));
                float capHalf = 0.5f * o.Height * glm::length(axes[1]);
                glm::vec3 reachOut = o.Shape == NavObstacleShape::Box
                                         ? glm::abs(axes[0] * half.x) + glm::abs(axes[1] * half.y) + glm::abs(axes[2] * half.z)
                                         : glm::vec3(capRadius, capHalf, capRadius);
                reachOut += glm::vec3(radius + c);
                glm::ivec2 lo = CellOf(*m, { center.x - reachOut.x, center.z - reachOut.z }), hi = CellOf(*m, { center.x + reachOut.x, center.z + reachOut.z });
                for (int z = std::max(lo.y, 0); z <= std::min(hi.y, m->Depth - 1); ++z)
                    for (int x = std::max(lo.x, 0); x <= std::min(hi.x, m->Width - 1); ++x)
                    {
                        int col = m->Column(x, z);
                        for (uint32_t a = m->ColumnStart[col]; a < m->ColumnStart[col + 1]; ++a)
                        {
                            glm::vec3 p = m->NodePosition((int)a);
                            bool inside;
                            if (o.Shape == NavObstacleShape::Box)
                            {
                                // The agent's column point nearest the box's middle height, against the box.
                                glm::vec3 q = p + glm::vec3(0.0f, std::clamp(center.y - p.y, 0.0f, height), 0.0f);
                                glm::vec3 d = q - center, closest = center;
                                for (int k = 0; k < 3; ++k)
                                {
                                    float len = glm::length(axes[k]);
                                    if (len < 1e-6f) continue;
                                    glm::vec3 u = axes[k] / len;
                                    float e = half[k] * len;
                                    closest += u * std::clamp(glm::dot(d, u), -e, e);
                                }
                                inside = glm::distance(q, closest) < radius;
                            }
                            else
                                inside = glm::length(glm::vec2(p.x - center.x, p.z - center.z)) < capRadius + radius &&
                                         p.y + height > center.y - capHalf && p.y < center.y + capHalf;
                            if (inside) m->Blocked[a] = 1;
                        }
                    }
            }
            // Cut every link into or out of a blocked spot.
            for (int a = 0; a < n; ++a)
                for (int d = 0; d < 8; ++d)
                {
                    int& l = m->Links[(size_t)a * 8 + d];
                    if (l >= 0 && (m->Blocked[a] || m->Blocked[l])) l = -1;
                }
            // Less room next to the holes: about Agent Radius at their edge, more farther off.
            std::vector<float> hole(n, kInf);
            for (int a = 0; a < n; ++a)
                if (!m->Blocked[a])
                    for (int d = 0; d < 8; ++d)
                        if (int b = baked.Links[(size_t)a * 8 + d]; b >= 0 && m->Blocked[b]) hole[a] = radius;
            Spread(*m, hole);
            if (m->Clearance.size() == (size_t)n)
                for (int a = 0; a < n; ++a) m->Clearance[a] = std::min(m->Clearance[a], hole[a]);
            BuildIslands(*m);
            return m;
        }

        namespace
        {
            // A surface's new mesh; agents find their place on it again.
            BakeScope ScopeOf(Scene& scene, Entity surface)
            {
                BakeScope scope;
                auto& reg = scene.GetRegistry();
                if (reg.Get<NavMeshSurfaceComponent>(surface).Moving)
                    scope.Own = surface;
                else
                    for (auto [e, s] : reg.ViewOf<NavMeshSurfaceComponent>())
                        if (s.Moving) scope.Moving.push_back(e);
                return scope;
            }

            void Install(Scene& scene, NavMeshSurfaceComponent& surface, std::shared_ptr<NavMeshData> data)
            {
                surface.Data = surface.Baked = data;
                surface.CarveKey = 0;
                for (auto [a, agent] : scene.GetRegistry().ViewOf<NavMeshAgentComponent>())
                {
                    agent.Mesh.reset();
                    agent.DestinationMesh.reset();
                    agent.CrossMesh.reset();
                    agent.HasMeshDestination = false;
                    agent.Node = -1;
                    if (agent.HasDestination) agent.PathDirty = true;
                }
            }
        }

        namespace
        {
            // A copy of the scene for sampling with physics of its own: the copied bodies' handles cleared (in a playing
            // scene they name its own world's bodies, and the copy's physics would skip them).
            Ref<Scene> Copy(Scene& scene)
            {
                Ref<Scene> copy = scene.Clone();
                for (auto [e, rb] : copy->GetRegistry().ViewOf<RigidbodyComponent>()) rb.RuntimeBody = 0xFFFFFFFF;
                return copy;
            }

            Ref<Scene> LevelCopy(Scene& scene)
            {
                Ref<Scene> copy = Copy(scene);
                copy->StartQueryPhysics();
                return copy;
            }

            // A copy of the scene whose physics is what is drawn: the Mesh Renderers' meshes as static mesh colliders,
            // the colliders gone (terrains keep theirs), for surfaces baked from Render Meshes.
            Ref<Scene> RenderMeshScene(Scene& scene)
            {
                Ref<Scene> copy = Copy(scene);
                auto& reg = copy->GetRegistry();
                std::vector<Entity> drawn;
                for (auto [e, mr, t] : reg.ViewOf<MeshRendererComponent, TransformComponent>())
                    if (t.ActiveInHierarchy && mr.Mesh.IsValid()) drawn.push_back(e);
                std::vector<Entity> solid;
                for (auto [e, c] : reg.ViewOf<BoxColliderComponent>()) solid.push_back(e);
                for (auto [e, c] : reg.ViewOf<SphereColliderComponent>()) solid.push_back(e);
                for (auto [e, c] : reg.ViewOf<CapsuleColliderComponent>()) solid.push_back(e);
                for (auto [e, c] : reg.ViewOf<MeshColliderComponent>()) solid.push_back(e);
                for (Entity e : solid)
                {
                    reg.Remove<BoxColliderComponent>(e);
                    reg.Remove<SphereColliderComponent>(e);
                    reg.Remove<CapsuleColliderComponent>(e);
                    reg.Remove<MeshColliderComponent>(e);
                }
                for (Entity e : drawn) reg.Emplace<MeshColliderComponent>(e); // (its renderer's mesh)
                copy->StartQueryPhysics();
                return copy;
            }

            bool FromRenderMeshes(const NavMeshSurfaceComponent& s) { return s.UseGeometry == NavMeshSurfaceComponent::Geometry::RenderMeshes && !s.Streamed; }
        }

        namespace
        {
            bool UpdateStreamedArea(Scene& scene, Entity e, const glm::vec3& center, const glm::vec3& lo, const glm::vec3& hi);
        }

        int UpdateArea(Scene& scene, const glm::vec3& lo, const glm::vec3& hi)
        {
            Ref<Scene> drawnCopy; // (for surfaces baked from render meshes)
            auto& reg = scene.GetRegistry();
            scene.UpdateTransforms();
            Ref<Scene> copy; // editing: the colliders in a physics world of a copy
            int updated = 0;
            std::vector<Entity> surfaces;
            for (auto [e, s] : reg.ViewOf<NavMeshSurfaceComponent>()) surfaces.push_back(e);
            for (Entity e : surfaces)
            {
                auto& s = reg.Get<NavMeshSurfaceComponent>(e);
                if (s.Moving) continue; // the level around changed, not the platform
                glm::vec3 center = reg.Get<TransformComponent>(e).GetWorldPosition();
                if (s.Streamed && s.Stream && scene.GetPhysics3D())
                {
                    updated += UpdateStreamedArea(scene, e, center, lo, hi) ? 1 : 0;
                    continue;
                }
                glm::vec3 half = glm::max(s.Size, glm::vec3(0.1f)) * 0.5f;
                if (hi.x < center.x - half.x || lo.x > center.x + half.x || hi.z < center.z - half.z || lo.z > center.z + half.z ||
                    hi.y < center.y - half.y || lo.y > center.y + half.y)
                    continue;
                NavMeshData grid;
                GridOf(s, center, grid);
                const NavMeshData* previous = s.Baked.get();
                // Moved or resized since its bake: all of it again.
                if (previous && (previous->Width != grid.Width || previous->Depth != grid.Depth || previous->CellSize != grid.CellSize ||
                                 glm::distance(previous->Origin, grid.Origin) > 1e-4f))
                    previous = nullptr;
                // The columns over the box, one more all round (their spots' links and room change too).
                glm::ivec2 a = CellOf(grid, { lo.x, lo.z }) - glm::ivec2(1), b = CellOf(grid, { hi.x, hi.z }) + glm::ivec2(1);
                glm::ivec4 region(std::max(a.x, 0), std::max(a.y, 0), std::min(b.x, grid.Width - 1), std::min(b.y, grid.Depth - 1));
                Scene* sampled = &scene;
                if (FromRenderMeshes(s))
                {
                    if (!drawnCopy) drawnCopy = RenderMeshScene(scene);
                    sampled = drawnCopy.get();
                }
                else if (!scene.GetPhysics3D())
                {
                    if (!copy) copy = LevelCopy(scene);
                    sampled = copy.get();
                }
                Install(scene, s, BakeIn(*sampled, s, center, ScopeOf(scene, e), previous, region));
                ++updated;
            }
            return updated;
        }

    }

    // A streamed surface's tiles (runtime): each tile's sampled surfaces kept on their own, keyed by the tile's place
    // on the surface's grid (which may be any size), and the mesh agents walk made from the loaded ones: a window just
    // over them (at most kMaxWindow columns a side), its corner on a tile's. New tiles are sampled on a worker thread
    // against a copy of the level (taken at play start and after UpdateArea), one batch at a time.
    struct NavTileStream
    {
        struct Tile
        {
            int W = 0, D = 0;            // columns
            std::vector<uint32_t> Start; // W * D + 1, as NavMeshData::RawStart
            std::vector<float> Heights, Costs;
            std::vector<uint8_t> Areas;
        };
        using TileMap = std::map<std::pair<int, int>, std::shared_ptr<const Tile>>; // (tx, tz)
        static constexpr int kMaxWindow = 4096;
        static constexpr int kBatch = 8; // tiles a worker bake samples

        // The grid: the surface box's corner, its columns, and the tiles over them.
        glm::vec2 Corner{ 0.0f };
        float CellSize = 0.25f;
        int Columns = 0, Rows = 0, TileCells = 1, TilesX = 0, TilesZ = 0;
        uint64_t Key = 0; // what the grid was made for (settings and place)
        TileMap Loaded;
        Ref<Scene> Level; // the level copy worker bakes sample (null: take a new one)

        struct Result
        {
            TileMap Tiles;
            std::shared_ptr<NavMeshData> Mesh;
            int Changed = 0;
        };
        std::future<Result> Job;
    };

    namespace Navigation
    {
        namespace
        {
            using Tile = NavTileStream::Tile;
            using TileMap = NavTileStream::TileMap;

            uint64_t StreamKey(const NavMeshSurfaceComponent& s, const glm::vec3& center)
            {
                uint64_t h = 1469598103934665603ull;
                auto mix = [&](float v) {
                    uint32_t bits;
                    std::memcpy(&bits, &v, 4);
                    h = (h ^ bits) * 1099511628211ull;
                };
                for (float v : { s.CellSize, s.TileSize, s.Size.x, s.Size.y, s.Size.z, center.x, center.y, center.z, s.AgentRadius, s.AgentHeight,
                                 s.StepHeight, s.MaxSlope, s.DropHeight, s.JumpDistance, (float)s.GenerateLinks, (float)s.AgentType })
                    mix(v);
                return h;
            }

            // The surface's stream, made anew when its grid changed (settings or place).
            NavTileStream& StreamOf(NavMeshSurfaceComponent& surface, const glm::vec3& center)
            {
                uint64_t key = StreamKey(surface, center);
                if (surface.Stream && surface.Stream->Key == key) return *surface.Stream;
                if (surface.Stream && surface.Stream->Job.valid()) surface.Stream->Job.wait();
                auto st = std::make_shared<NavTileStream>();
                st->Key = key;
                float c = std::max(surface.CellSize, 0.05f);
                glm::vec3 size = glm::max(surface.Size, glm::vec3(0.1f));
                st->CellSize = c;
                st->Columns = std::max((int)std::ceil(size.x / c), 1);
                st->Rows = std::max((int)std::ceil(size.z / c), 1);
                st->Corner = { center.x - st->Columns * c * 0.5f, center.z - st->Rows * c * 0.5f };
                st->TileCells = std::clamp((int)std::round(std::max(surface.TileSize, 1.0f) / c), 1, NavTileStream::kMaxWindow);
                st->TilesX = (st->Columns + st->TileCells - 1) / st->TileCells;
                st->TilesZ = (st->Rows + st->TileCells - 1) / st->TileCells;
                surface.Stream = st;
                return *st;
            }

            // Samples a tile's columns.
            std::shared_ptr<const Tile> SampleTile(Scene& scene, const NavTileStream& st, const Sampler& sampler, int tx, int tz)
            {
                auto t = std::make_shared<Tile>();
                int x0 = tx * st.TileCells, z0 = tz * st.TileCells;
                t->W = std::min(st.TileCells, st.Columns - x0);
                t->D = std::min(st.TileCells, st.Rows - z0);
                t->Start.reserve((size_t)t->W * t->D + 1);
                for (int z = 0; z < t->D; ++z)
                    for (int x = 0; x < t->W; ++x)
                    {
                        t->Start.push_back((uint32_t)t->Heights.size());
                        glm::vec2 xz = st.Corner + glm::vec2(x0 + x + 0.5f, z0 + z + 0.5f) * st.CellSize;
                        SampleColumn(scene, sampler, xz, t->Heights, t->Areas, t->Costs);
                    }
                t->Start.push_back((uint32_t)t->Heights.size());
                return t;
            }

            // The mesh over the loaded tiles: a window from the lowest to the highest tile loaded (columns of tiles not
            // loaded are empty).
            std::shared_ptr<NavMeshData> BuildWindow(Scene& scene, const NavMeshSurfaceComponent& s, const NavTileStream& st, const TileMap& tiles,
                                                     const BakeScope& scope)
            {
                auto m = std::make_shared<NavMeshData>();
                SetAgentSizes(s, *m);
                m->CellSize = st.CellSize;
                glm::ivec2 lo(INT32_MAX), hi(INT32_MIN);
                for (const auto& [key, tile] : tiles)
                {
                    lo = glm::min(lo, glm::ivec2(key.first, key.second));
                    hi = glm::max(hi, glm::ivec2(key.first, key.second));
                }
                if (tiles.empty()) lo = hi = glm::ivec2(0);
                int x0 = lo.x * st.TileCells, z0 = lo.y * st.TileCells;
                m->Width = tiles.empty() ? 1 : std::min((hi.x + 1) * st.TileCells, st.Columns) - x0;
                m->Depth = tiles.empty() ? 1 : std::min((hi.y + 1) * st.TileCells, st.Rows) - z0;
                m->Origin = st.Corner + glm::vec2((float)x0, (float)z0) * st.CellSize;
                m->RawStart.reserve((size_t)m->Width * m->Depth + 1);
                for (int z = 0; z < m->Depth; ++z)
                {
                    int tz = (z0 + z) / st.TileCells, lz = (z0 + z) % st.TileCells;
                    for (int x = 0; x < m->Width; ++x)
                    {
                        m->RawStart.push_back((uint32_t)m->RawHeights.size());
                        int tx = (x0 + x) / st.TileCells, lx = (x0 + x) % st.TileCells;
                        auto it = tiles.find({ tx, tz });
                        if (it == tiles.end()) continue;
                        const Tile& t = *it->second;
                        int col = lz * t.W + lx;
                        uint32_t from = t.Start[col], to = t.Start[col + 1];
                        m->RawHeights.insert(m->RawHeights.end(), t.Heights.begin() + from, t.Heights.begin() + to);
                        m->RawAreas.insert(m->RawAreas.end(), t.Areas.begin() + from, t.Areas.begin() + to);
                        m->RawCosts.insert(m->RawCosts.end(), t.Costs.begin() + from, t.Costs.begin() + to);
                    }
                }
                m->RawStart.push_back((uint32_t)m->RawHeights.size());
                FinishMesh(scene, s, *m, VolumesOf(scene), scope);
                return m;
            }

            // Which tiles a streamed surface wants now: those within Stream Radius of an agent or the main camera to
            // load (nearest first), and the loaded ones to drop (a tile farther than that, or outside the largest window
            // around the camera, or the first agent without one).
            void WantedTiles(Scene& scene, const NavMeshSurfaceComponent& s, const NavTileStream& st, std::vector<std::pair<int, int>>& load,
                             std::vector<std::pair<int, int>>& drop)
            {
                auto& reg = scene.GetRegistry();
                std::vector<glm::vec2> focus;
                if (Entity cam = scene.GetPrimaryCamera(); !cam.IsNull())
                {
                    const glm::mat4& w = reg.Get<TransformComponent>(cam).World;
                    focus.push_back({ w[3].x, w[3].z });
                }
                for (auto [a, agent, t] : reg.ViewOf<NavMeshAgentComponent, TransformComponent>())
                    if (t.ActiveInHierarchy) focus.push_back({ t.World[3].x, t.World[3].z });
                float tileM = st.TileCells * st.CellSize, radius = std::max(s.StreamRadius, 0.0f);
                auto tileOf = [&](glm::vec2 p) { return glm::ivec2(glm::floor((p - st.Corner) / tileM)); };
                // The window allowed: kMaxWindow columns around the first focus.
                int half = std::max((NavTileStream::kMaxWindow / st.TileCells - 1) / 2, 0);
                glm::ivec2 center = focus.empty() ? glm::ivec2(0) : tileOf(focus[0]);
                auto allowed = [&](int tx, int tz) {
                    return tx >= 0 && tz >= 0 && tx < st.TilesX && tz < st.TilesZ && std::abs(tx - center.x) <= half && std::abs(tz - center.y) <= half;
                };
                auto distance = [&](int tx, int tz) {
                    glm::vec2 lo = st.Corner + glm::vec2(tx, tz) * tileM, hi = lo + glm::vec2(tileM);
                    float best = 1e30f;
                    for (glm::vec2 p : focus) best = std::min(best, glm::length(glm::max(glm::max(lo - p, p - hi), glm::vec2(0.0f))));
                    return best;
                };
                std::vector<std::pair<float, std::pair<int, int>>> wanted;
                std::set<std::pair<int, int>> seen;
                for (glm::vec2 p : focus)
                {
                    glm::ivec2 a = tileOf(p - glm::vec2(radius)), b = tileOf(p + glm::vec2(radius));
                    for (int tz = std::max(a.y, 0); tz <= std::min(b.y, st.TilesZ - 1); ++tz)
                        for (int tx = std::max(a.x, 0); tx <= std::min(b.x, st.TilesX - 1); ++tx)
                        {
                            if (st.Loaded.count({ tx, tz }) || !allowed(tx, tz) || !seen.insert({ tx, tz }).second) continue;
                            if (float d = distance(tx, tz); d <= radius) wanted.push_back({ d, { tx, tz } });
                        }
                }
                std::sort(wanted.begin(), wanted.end());
                load.clear();
                for (const auto& w : wanted) load.push_back(w.second);
                drop.clear();
                for (const auto& [key, tile] : st.Loaded)
                    if (!allowed(key.first, key.second) || distance(key.first, key.second) > radius + tileM) drop.push_back(key);
            }

            // A streamed surface's tiles brought up to date at once (up to `budget` loaded, every far one dropped),
            // sampling the scene itself. Returns how many tiles changed.
            int StreamSurface(Scene& scene, Entity e, int budget)
            {
                auto& reg = scene.GetRegistry();
                auto& surface = reg.Get<NavMeshSurfaceComponent>(e);
                if (!surface.Streamed || surface.Moving || !surface.Baked || !scene.GetPhysics3D()) return 0;
                glm::vec3 center = reg.Get<TransformComponent>(e).GetWorldPosition();
                NavTileStream& st = StreamOf(surface, center);
                int changed = 0;
                if (st.Job.valid())
                {
                    NavTileStream::Result r = st.Job.get(); // (a worker bake first)
                    st.Loaded = std::move(r.Tiles);
                    if (r.Mesh) Install(scene, surface, r.Mesh);
                    changed += r.Changed;
                }
                std::vector<std::pair<int, int>> load, drop;
                WantedTiles(scene, surface, st, load, drop);
                if (load.empty() && drop.empty()) return changed;
                const NavMeshSurfaceComponent s = WithAgentType(surface);
                BakeScope scope = ScopeOf(scene, e);
                std::vector<Volume> volumes = VolumesOf(scene);
                Sampler sampler = SamplerOf(s, center, volumes, scope);
                for (const auto& key : drop) st.Loaded.erase(key), ++changed;
                for (size_t k = 0; k < load.size() && (int)k < budget; ++k, ++changed) st.Loaded[load[k]] = SampleTile(scene, st, sampler, load[k].first, load[k].second);
                Install(scene, surface, BuildWindow(scene, s, st, st.Loaded, scope));
                return changed;
            }

            // The same on a worker thread: installs a finished worker bake, then starts the next one when tiles are
            // wanted (sampling a batch of them in a copy of the level). Returns how many tiles changed.
            int StreamSurfaceAsync(Scene& scene, Entity e)
            {
                auto& reg = scene.GetRegistry();
                auto& surface = reg.Get<NavMeshSurfaceComponent>(e);
                if (!surface.Streamed || surface.Moving || !surface.Baked || !scene.GetPhysics3D()) return 0;
                glm::vec3 center = reg.Get<TransformComponent>(e).GetWorldPosition();
                NavTileStream& st = StreamOf(surface, center);
                int changed = 0;
                if (st.Job.valid())
                {
                    if (st.Job.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return 0;
                    NavTileStream::Result r = st.Job.get();
                    st.Loaded = std::move(r.Tiles);
                    if (r.Mesh) Install(scene, surface, r.Mesh);
                    changed += r.Changed;
                }
                std::vector<std::pair<int, int>> load, drop;
                WantedTiles(scene, surface, st, load, drop);
                if (load.empty() && drop.empty()) return changed;
                if (!st.Level) st.Level = LevelCopy(scene);
                if (load.size() > (size_t)NavTileStream::kBatch) load.resize(NavTileStream::kBatch);
                TileMap tiles = st.Loaded;
                for (const auto& key : drop) tiles.erase(key);
                Ref<Scene> level = st.Level;
                NavMeshSurfaceComponent s = WithAgentType(surface);
                s.Stream.reset(), s.Data.reset(), s.Baked.reset(); // (the job's copy holds no runtime state)
                BakeScope scope = ScopeOf(scene, e);
                const NavTileStream* grid = &st; // (its grid fields stay as they are while the job runs: StreamOf waits for it)
                int count = (int)(load.size() + drop.size());
                st.Job = std::async(std::launch::async, [level, s, center, scope, grid, tiles = std::move(tiles), load, count]() mutable {
                    std::vector<Volume> volumes = VolumesOf(*level);
                    Sampler sampler = SamplerOf(s, center, volumes, scope);
                    for (const auto& key : load) tiles[key] = SampleTile(*level, *grid, sampler, key.first, key.second);
                    NavTileStream::Result r;
                    r.Mesh = BuildWindow(*level, s, *grid, tiles, scope);
                    r.Tiles = std::move(tiles);
                    r.Changed = count;
                    return r;
                });
                return changed;
            }

            std::vector<Entity> StreamedSurfaces(Scene& scene)
            {
                std::vector<Entity> streamed;
                for (auto [e, s] : scene.GetRegistry().ViewOf<NavMeshSurfaceComponent>())
                    if (s.Streamed && s.Baked) streamed.push_back(e);
                return streamed;
            }
        }

        namespace
        {
            // The loaded tiles of a streamed surface over a box sampled again (one more column all round, as their
            // spots' room changes too); later worker bakes sample a new copy of the level.
            bool UpdateStreamedArea(Scene& scene, Entity e, const glm::vec3& center, const glm::vec3& lo, const glm::vec3& hi)
            {
                auto& surface = scene.GetRegistry().Get<NavMeshSurfaceComponent>(e);
                NavTileStream& st = StreamOf(surface, center);
                if (st.Job.valid())
                {
                    NavTileStream::Result r = st.Job.get();
                    st.Loaded = std::move(r.Tiles);
                }
                st.Level.reset();
                float tileM = st.TileCells * st.CellSize, pad = st.CellSize;
                glm::ivec2 a(glm::floor((glm::vec2(lo.x, lo.z) - pad - st.Corner) / tileM)), b(glm::floor((glm::vec2(hi.x, hi.z) + pad - st.Corner) / tileM));
                const NavMeshSurfaceComponent s = WithAgentType(surface);
                BakeScope scope = ScopeOf(scene, e);
                std::vector<Volume> volumes = VolumesOf(scene);
                Sampler sampler = SamplerOf(s, center, volumes, scope);
                bool any = false;
                for (auto& [key, tile] : st.Loaded)
                    if (key.first >= a.x && key.first <= b.x && key.second >= a.y && key.second <= b.y)
                    {
                        tile = SampleTile(scene, st, sampler, key.first, key.second);
                        any = true;
                    }
                if (any) Install(scene, surface, BuildWindow(scene, s, st, st.Loaded, scope));
                return any;
            }
        }

        int StreamTiles(Scene& scene, int budget)
        {
            int changed = 0;
            for (Entity e : StreamedSurfaces(scene)) changed += StreamSurface(scene, e, budget);
            return changed;
        }

        int StreamTilesAsync(Scene& scene)
        {
            int changed = 0;
            for (Entity e : StreamedSurfaces(scene)) changed += StreamSurfaceAsync(scene, e);
            return changed;
        }

        bool IsStreaming(Scene& scene)
        {
            for (Entity e : StreamedSurfaces(scene))
                if (auto& st = scene.GetRegistry().Get<NavMeshSurfaceComponent>(e).Stream; st && st->Job.valid()) return true;
            return false;
        }

        std::vector<glm::ivec2> LoadedTiles(Scene& scene, Entity e, glm::ivec2* tileCount)
        {
            std::vector<glm::ivec2> out;
            auto* s = scene.IsValid(e) ? scene.GetRegistry().TryGet<NavMeshSurfaceComponent>(e) : nullptr;
            if (tileCount) *tileCount = s && s->Stream ? glm::ivec2(s->Stream->TilesX, s->Stream->TilesZ) : glm::ivec2(0);
            if (!s || !s->Stream) return out;
            for (const auto& [key, tile] : s->Stream->Loaded) out.push_back({ key.first, key.second });
            return out;
        }

        std::shared_ptr<const NavMeshData> Bake(Scene& scene, Entity e)
        {
            auto& reg = scene.GetRegistry();
            auto* surface = scene.IsValid(e) ? reg.TryGet<NavMeshSurfaceComponent>(e) : nullptr;
            if (!surface) return nullptr;
            scene.UpdateTransforms();
            glm::vec3 center = reg.Get<TransformComponent>(e).GetWorldPosition();
            if (surface->Streamed && !surface->Moving && scene.GetPhysics3D())
            {
                // Streamed while playing: the tiles around the agents and the camera now (the rest come as they get
                // near), and the copy of the level later tiles are sampled in.
                if (surface->Stream && surface->Stream->Job.valid()) surface->Stream->Job.wait();
                surface->Stream.reset();
                NavTileStream& st = StreamOf(*surface, center);
                st.Level = LevelCopy(scene);
                Install(scene, *surface, BuildWindow(scene, WithAgentType(*surface), st, st.Loaded, ScopeOf(scene, e)));
                surface->BakedAt = reg.Get<TransformComponent>(e).World;
                surface->Frame = surface->FrameInverse = glm::mat4(1.0f);
                StreamSurface(scene, e, INT32_MAX);
                return surface->Baked;
            }
            std::shared_ptr<NavMeshData> data;
            BakeScope scope = ScopeOf(scene, e);
            if (FromRenderMeshes(*surface))
            {
                // What is drawn, in a copy (the scene's own physics is left alone).
                Ref<Scene> drawn = RenderMeshScene(scene);
                data = BakeIn(*drawn, *surface, center, scope);
            }
            else if (scene.GetPhysics3D())
                data = BakeIn(scene, *surface, center, scope);
            else
            {
                // Editing: the colliders in a physics world of a copy (the scene itself is left alone).
                Ref<Scene> copy = LevelCopy(scene);
                data = BakeIn(*copy, *surface, center, scope);
            }
            Install(scene, *surface, data);
            // A Moving surface: its mesh in the pose its object has now.
            surface->BakedAt = reg.Get<TransformComponent>(e).World;
            surface->Frame = surface->FrameInverse = glm::mat4(1.0f);
            return data;
        }

        namespace
        {
            // Background bakes, per scene and surface: the copy of the level they sample (kept alive while they run).
            struct PendingBake
            {
                Ref<Scene> Level;
                std::future<std::shared_ptr<NavMeshData>> Result;
                glm::mat4 BakedAt{ 1.0f };
            };
            std::map<std::pair<const Scene*, uint64_t>, PendingBake>& Pending()
            {
                static std::map<std::pair<const Scene*, uint64_t>, PendingBake> pending;
                return pending;
            }
            uint64_t KeyOf(Entity e) { return (uint64_t(e.Generation) << 32) | e.Index; }
        }

        bool BakeAsync(Scene& scene, Entity e)
        {
            auto& reg = scene.GetRegistry();
            auto* surface = scene.IsValid(e) ? reg.TryGet<NavMeshSurfaceComponent>(e) : nullptr;
            if (!surface) return false;
            if (surface->Streamed || surface->Moving) return Bake(scene, e) != nullptr; // (these follow the scene as it runs)
            scene.UpdateTransforms();
            auto key = std::make_pair((const Scene*)&scene, KeyOf(e));
            if (Pending().count(key)) FinishBakes(scene, true); // (one at a time per surface: the earlier one first)
            // The level as it is now, with its own physics to sample (the scene's own runs on meanwhile).
            PendingBake job;
            job.Level = FromRenderMeshes(*surface) ? RenderMeshScene(scene) : LevelCopy(scene);
            job.BakedAt = reg.Get<TransformComponent>(e).World;
            NavMeshSurfaceComponent settings = *surface;
            glm::vec3 center = reg.Get<TransformComponent>(e).GetWorldPosition();
            BakeScope scope = ScopeOf(scene, e);
            Scene* level = job.Level.get();
            job.Result = std::async(std::launch::async, [level, settings, center, scope] { return BakeIn(*level, settings, center, scope); });
            Pending()[key] = std::move(job);
            return true;
        }

        int BakeAllAsync(Scene& scene)
        {
            std::vector<Entity> surfaces;
            for (auto [e, s] : scene.GetRegistry().ViewOf<NavMeshSurfaceComponent>()) surfaces.push_back(e);
            int started = 0;
            for (Entity e : surfaces) started += BakeAsync(scene, e) ? 1 : 0;
            return started;
        }

        bool IsBaking(Scene& scene, Entity e) { return Pending().count({ &scene, KeyOf(e) }) > 0; }

        bool IsBaking(Scene& scene)
        {
            for (const auto& [key, job] : Pending())
                if (key.first == &scene) return true;
            return false;
        }

        int FinishBakes(Scene& scene, bool wait)
        {
            int installed = 0;
            auto& pending = Pending();
            for (auto it = pending.begin(); it != pending.end();)
            {
                if (it->first.first != &scene ||
                    (!wait && it->second.Result.wait_for(std::chrono::seconds(0)) != std::future_status::ready))
                {
                    ++it;
                    continue;
                }
                std::shared_ptr<NavMeshData> data = it->second.Result.get();
                Entity e{ uint32_t(it->first.second & 0xFFFFFFFFu), uint32_t(it->first.second >> 32) };
                if (data && scene.IsValid(e))
                    if (auto* s = scene.GetRegistry().TryGet<NavMeshSurfaceComponent>(e))
                    {
                        Install(scene, *s, data);
                        s->BakedAt = it->second.BakedAt;
                        s->Frame = s->FrameInverse = glm::mat4(1.0f);
                        ++installed;
                    }
                it = pending.erase(it);
            }
            return installed;
        }

        void BakeAll(Scene& scene, bool onlyMissing, bool useSaved)
        {
            std::vector<Entity> surfaces;
            for (auto [e, s] : scene.GetRegistry().ViewOf<NavMeshSurfaceComponent>())
                if (!onlyMissing || !s.Data) surfaces.push_back(e);
            for (Entity e : surfaces)
                if (!useSaved || !LoadSaved(scene, e)) Bake(scene, e);
        }

        uint64_t SettingsKey(Scene& scene, Entity e)
        {
            auto& reg = scene.GetRegistry();
            auto* s = scene.IsValid(e) ? reg.TryGet<NavMeshSurfaceComponent>(e) : nullptr;
            if (!s) return 0;
            scene.UpdateTransforms();
            NavMeshSurfaceComponent typed = WithAgentType(*s); // (an agent type's sizes count)
            s = &typed;
            // Rounded, so float noise from saving the scene does not count as a change.
            uint64_t key = 1469598103934665603ull;
            auto add = [&](float v) {
                int64_t q = (int64_t)std::llround((double)v * 10000.0);
                for (int b = 0; b < 8; ++b) key = (key ^ (uint8_t)(q >> (b * 8))) * 1099511628211ull;
            };
            for (int a = 0; a < 3; ++a) add(s->Size[a]);
            for (float v : { s->CellSize, s->AgentRadius, s->AgentHeight, s->MaxSlope, s->StepHeight, s->DropHeight, s->JumpDistance }) add(v);
            add(s->GenerateLinks ? 1.0f : 0.0f);
            add(s->Moving ? 1.0f : 0.0f);
            if (s->UseGeometry != NavMeshSurfaceComponent::Geometry::PhysicsColliders) add(100.0f + (float)s->UseGeometry); // (keys of collider bakes as before)
            const glm::mat4& world = reg.Get<TransformComponent>(e).World;
            for (int c = 0; c < 4; ++c)
                for (int r = 0; r < 4; ++r) add(world[c][r]);
            return key;
        }

        namespace
        {
            constexpr uint32_t kNavMagic = 0x56414E44; // "DNAV"
            constexpr uint32_t kNavVersion = 2; // 2: raw areas and costs (NavMesh Modifiers)

            template <typename T> void Put(std::ofstream& out, const T& v) { out.write((const char*)&v, sizeof(T)); }
            template <typename T> void PutList(std::ofstream& out, const std::vector<T>& v)
            {
                Put<uint64_t>(out, v.size());
                if (!v.empty()) out.write((const char*)v.data(), (std::streamsize)(v.size() * sizeof(T)));
            }
            template <typename T> bool Take(std::ifstream& in, T& v) { return (bool)in.read((char*)&v, sizeof(T)); }
            template <typename T> bool TakeList(std::ifstream& in, std::vector<T>& v)
            {
                uint64_t n = 0;
                if (!Take(in, n) || n > (1ull << 32)) return false;
                v.resize((size_t)n);
                return n == 0 || (bool)in.read((char*)v.data(), (std::streamsize)(n * sizeof(T)));
            }
        }

        bool WriteData(const fs::path& file, const NavMeshData& m, uint64_t key, const glm::mat4& bakedAt)
        {
            std::error_code ec;
            fs::create_directories(file.parent_path(), ec);
            std::ofstream out(file, std::ios::binary | std::ios::trunc);
            if (!out) return false;
            Put(out, kNavMagic);
            Put(out, kNavVersion);
            Put(out, key);
            Put(out, bakedAt);
            Put(out, m.Origin);
            Put(out, m.CellSize);
            Put(out, m.Width);
            Put(out, m.Depth);
            for (float v : { m.AgentRadius, m.AgentHeight, m.StepHeight, m.MinCost }) Put(out, v);
            PutList(out, m.ColumnStart);
            PutList(out, m.Heights);
            PutList(out, m.Columns);
            PutList(out, m.Links);
            PutList(out, m.Islands);
            PutList(out, m.Clearance);
            PutList(out, m.Areas);
            PutList(out, m.Costs);
            Put<uint64_t>(out, m.AutoLinks.size());
            for (const auto& l : m.AutoLinks)
            {
                Put<int32_t>(out, l.From);
                Put<int32_t>(out, l.To);
                Put(out, l.Arc);
            }
            PutList(out, m.RawStart);
            PutList(out, m.RawHeights);
            PutList(out, m.RawAreas);
            PutList(out, m.RawCosts);
            return (bool)out;
        }

        std::shared_ptr<NavMeshData> ReadData(const fs::path& file, uint64_t* key, glm::mat4* bakedAt)
        {
            std::ifstream in(file, std::ios::binary);
            uint32_t magic = 0, version = 0;
            uint64_t k = 0;
            glm::mat4 at(1.0f);
            auto m = std::make_shared<NavMeshData>();
            if (!in || !Take(in, magic) || magic != kNavMagic || !Take(in, version) || version < 1 || version > kNavVersion || !Take(in, k) || !Take(in, at))
                return nullptr;
            bool ok = Take(in, m->Origin) && Take(in, m->CellSize) && Take(in, m->Width) && Take(in, m->Depth) && Take(in, m->AgentRadius) &&
                      Take(in, m->AgentHeight) && Take(in, m->StepHeight) && Take(in, m->MinCost) && TakeList(in, m->ColumnStart) &&
                      TakeList(in, m->Heights) && TakeList(in, m->Columns) && TakeList(in, m->Links) && TakeList(in, m->Islands) &&
                      TakeList(in, m->Clearance) && TakeList(in, m->Areas) && TakeList(in, m->Costs);
            uint64_t links = 0;
            ok = ok && Take(in, links) && links < (1ull << 28);
            for (uint64_t i = 0; ok && i < links; ++i)
            {
                NavMeshData::AutoLink l{};
                int32_t from = 0, to = 0;
                ok = Take(in, from) && Take(in, to) && Take(in, l.Arc);
                l.From = from, l.To = to;
                m->AutoLinks.push_back(l);
            }
            ok = ok && TakeList(in, m->RawStart) && TakeList(in, m->RawHeights);
            if (version >= 2) ok = ok && TakeList(in, m->RawAreas) && TakeList(in, m->RawCosts);
            // Consistent sizes, or it is not used.
            size_t nodes = m->Heights.size();
            ok = ok && m->Width >= 0 && m->Depth >= 0 && m->ColumnStart.size() == (size_t)m->Width * m->Depth + 1 && m->Columns.size() == nodes &&
                 m->Links.size() == nodes * 8 && m->Islands.size() == nodes && m->ColumnStart.back() == nodes;
            if (!ok) return nullptr;
            if (key) *key = k;
            if (bakedAt) *bakedAt = at;
            return m;
        }

        bool SaveBaked(Scene& scene, Entity e, const fs::path& sceneFile)
        {
            auto& reg = scene.GetRegistry();
            auto* s = scene.IsValid(e) ? reg.TryGet<NavMeshSurfaceComponent>(e) : nullptr;
            if (!s || !s->Baked || sceneFile.empty()) return false;
            // Over the file it already uses, else a new one in the scene's folder (named after the scene).
            const AssetMetadata* meta = s->BakedData.IsValid() ? AssetManager::GetMetadata(s->BakedData) : nullptr;
            fs::path file = meta && meta->Type == AssetType::NavMeshData ? AssetManager::GetAbsolutePath(s->BakedData) : fs::path();
            if (file.empty())
            {
                std::string name;
                for (char c : reg.Get<TagComponent>(e).Name) name += std::strchr("<>:\"/\\|?*", c) || (unsigned char)c < 32 ? '_' : c;
                fs::path folder = sceneFile.parent_path() / sceneFile.stem();
                file = folder / ("NavMesh-" + name + ".navmesh");
                for (int n = 1; fs::exists(file); ++n) file = folder / ("NavMesh-" + name + " " + std::to_string(n) + ".navmesh");
            }
            if (!WriteData(file, *s->Baked, SettingsKey(scene, e), s->BakedAt)) return false;
            AssetHandle handle = AssetManager::GetHandle(file);
            if (!handle.IsValid()) handle = AssetManager::Import(file);
            if (!handle.IsValid()) return false;
            s->BakedData = handle;
            return true;
        }

        bool IsSavedCurrent(Scene& scene, Entity e)
        {
            auto* s = scene.IsValid(e) ? scene.GetRegistry().TryGet<NavMeshSurfaceComponent>(e) : nullptr;
            if (!s || !s->BakedData.IsValid()) return false;
            std::ifstream in(AssetManager::GetAbsolutePath(s->BakedData), std::ios::binary);
            uint32_t magic = 0, version = 0;
            uint64_t key = 0;
            return in && Take(in, magic) && magic == kNavMagic && Take(in, version) && version >= 1 && version <= kNavVersion && Take(in, key) &&
                   key == SettingsKey(scene, e);
        }

        bool LoadSaved(Scene& scene, Entity e)
        {
            auto& reg = scene.GetRegistry();
            auto* s = scene.IsValid(e) ? reg.TryGet<NavMeshSurfaceComponent>(e) : nullptr;
            if (!s || !s->BakedData.IsValid() || s->Streamed) return false; // (streamed surfaces bake their tiles as they go)
            uint64_t key = 0;
            glm::mat4 bakedAt(1.0f);
            auto data = ReadData(AssetManager::GetAbsolutePath(s->BakedData), &key, &bakedAt);
            const std::string& name = reg.Get<TagComponent>(e).Name;
            if (!data)
            {
                Log::Warn("NavMesh Surface '{}': its saved navigation could not be read; baking it now", name);
                return false;
            }
            if (key != SettingsKey(scene, e))
            {
                Log::Warn("NavMesh Surface '{}': its saved navigation is out of date (its settings or place changed; bake it again); baking it now", name);
                return false;
            }
            Install(scene, *s, data);
            s->BakedAt = bakedAt;
            s->Frame = s->FrameInverse = glm::mat4(1.0f);
            Log::Info("NavMesh Surface '{}': using its saved bake ({} spots)", name, data->NodeCount());
            return true;
        }

        std::shared_ptr<const NavMeshData> MeshNear(Scene& scene, const glm::vec3& point, float maxDistance, int* node, const Filter& filter)
        {
            std::shared_ptr<const NavMeshData> best;
            float bestD = kInf;
            int bestNode = -1;
            for (auto [e, s, t] : scene.GetRegistry().ViewOf<NavMeshSurfaceComponent, TransformComponent>())
            {
                if (!s.Data || !t.ActiveInHierarchy || !TypesMatch(filter.AgentType, s.AgentType)) continue;
                glm::vec3 local = s.Moving ? glm::vec3(s.FrameInverse * glm::vec4(point, 1.0f)) : point;
                int a = Nearest(*s.Data, local, maxDistance, filter);
                if (a < 0) continue;
                glm::vec3 on = OnMesh(*s.Data, a, { local.x, local.z });
                float d = glm::distance(on, local);
                if (d < bestD)
                {
                    bestD = d;
                    best = s.Data;
                    bestNode = a;
                }
            }
            if (node) *node = bestNode;
            return best;
        }

        bool SamplePosition(Scene& scene, const glm::vec3& point, float maxDistance, glm::vec3& result)
        {
            int node = -1;
            auto mesh = MeshNear(scene, point, maxDistance, &node);
            if (!mesh) return false;
            glm::vec3 local = ToMesh(scene, mesh.get(), point);
            result = ToWorld(scene, mesh.get(), OnMesh(*mesh, node, { local.x, local.z }));
            return true;
        }

        NavPathStatus CalculatePath(Scene& scene, const glm::vec3& from, const glm::vec3& to, std::vector<glm::vec3>& corners, uint32_t areaMask)
        {
            int node = -1;
            corners.clear();
            Filter filter;
            filter.AreaMask = areaMask;
            auto mesh = MeshNear(scene, from, 2.0f, &node, filter);
            if (!mesh) return NavPathStatus::Invalid;
            NavPathStatus status = PathFrom(*mesh, node, ToMesh(scene, mesh.get(), from), ToMesh(scene, mesh.get(), to), corners, LinksOn(scene, *mesh), nullptr, filter);
            for (glm::vec3& c : corners) c = ToWorld(scene, mesh.get(), c);
            return status;
        }

        bool Raycast(Scene& scene, const glm::vec3& from, const glm::vec3& to, NavHit& hit, uint32_t areaMask)
        {
            hit = {};
            Filter filter;
            filter.AreaMask = areaMask;
            int node = -1;
            auto mesh = MeshNear(scene, from, 2.0f, &node, filter);
            if (!mesh)
            {
                hit.Position = from;
                hit.Hit = true;
                return true;
            }
            glm::vec3 a = ToMesh(scene, mesh.get(), from), b = ToMesh(scene, mesh.get(), to);
            int end = node;
            glm::vec2 reached;
            bool clear = Walk(*mesh, node, { a.x, a.z }, { b.x, b.z }, &end, &reached, filter);
            glm::vec3 local = OnMesh(*mesh, end, reached);
            hit.Position = ToWorld(scene, mesh.get(), local);
            hit.Distance = glm::length(reached - glm::vec2(a.x, a.z));
            hit.Hit = !clear;
            if (!clear)
            {
                glm::vec2 d = glm::vec2(b.x, b.z) - glm::vec2(a.x, a.z);
                glm::vec2 n = glm::length(d) > 1e-6f ? -glm::normalize(d) : glm::vec2(0.0f);
                hit.Normal = ToWorld(scene, mesh.get(), local + glm::vec3(n.x, 0.0f, n.y)) - hit.Position;
            }
            return !clear;
        }

        bool FindClosestEdge(Scene& scene, const glm::vec3& point, NavHit& hit, uint32_t areaMask)
        {
            hit = {};
            Filter filter;
            filter.AreaMask = areaMask;
            int node = -1;
            auto mesh = MeshNear(scene, point, 2.0f, &node, filter);
            if (!mesh) return false;
            const NavMeshData& m = *mesh;
            glm::vec3 local = ToMesh(scene, mesh.get(), point);
            glm::vec2 p(local.x, local.z);
            // The edge is no further than the node's clearance (and a cell): look at the node sides in the columns that near.
            float reach = (m.Clearance.empty() ? 5.0f : std::min(m.Clearance[node], 50.0f)) + m.CellSize * 2.0f;
            int col = m.Columns[node], cx = col % m.Width, cz = col / m.Width;
            int r = (int)std::ceil(reach / m.CellSize);
            float best = kInf;
            glm::vec3 edge(0.0f);
            for (int z = std::max(cz - r, 0); z <= std::min(cz + r, m.Depth - 1); ++z)
                for (int x = std::max(cx - r, 0); x <= std::min(cx + r, m.Width - 1); ++x)
                {
                    int c = m.Column(x, z);
                    for (uint32_t n = m.ColumnStart[c]; n < m.ColumnStart[c + 1]; ++n)
                    {
                        if (m.Islands[n] != m.Islands[node] || !filter.Passes(m, (int)n)) continue;
                        glm::vec3 at = m.NodePosition((int)n);
                        for (int dir = 0; dir < 4; ++dir)
                        {
                            int next = m.Links[(size_t)n * 8 + dir];
                            if (next >= 0 && filter.Passes(m, next)) continue;
                            // The open side: half a cell out from the node, the nearest point along it.
                            glm::vec2 out((float)kDirX[dir], (float)kDirZ[dir]), side(-out.y, out.x);
                            glm::vec2 mid = glm::vec2(at.x, at.z) + out * (m.CellSize * 0.5f);
                            float along = std::clamp(glm::dot(p - mid, side), -m.CellSize * 0.5f, m.CellSize * 0.5f);
                            glm::vec2 q = mid + side * along;
                            float d = glm::length(p - q);
                            if (d < best)
                            {
                                best = d;
                                edge = glm::vec3(q.x, at.y, q.y);
                            }
                        }
                    }
                }
            if (best == kInf) return false;
            hit.Position = ToWorld(scene, mesh.get(), edge);
            glm::vec2 away = p - glm::vec2(edge.x, edge.z);
            glm::vec2 n = glm::length(away) > 1e-6f ? glm::normalize(away) : glm::vec2(0.0f);
            hit.Normal = ToWorld(scene, mesh.get(), edge + glm::vec3(n.x, 0.0f, n.y)) - hit.Position;
            hit.Distance = best;
            hit.Hit = true;
            return true;
        }

        bool SetPath(Scene& scene, Entity e, const std::vector<glm::vec3>& corners, NavPathStatus status)
        {
            NavMeshAgentComponent* agent = AgentOf(scene, e);
            if (!agent || corners.empty() || status == NavPathStatus::Invalid || !Place(scene, e, *agent)) return false;
            agent->Destination = corners.back();
            agent->HasDestination = true;
            agent->DestinationMesh = agent->Mesh;
            agent->MeshDestination = ToMesh(scene, agent->Mesh.get(), corners.back());
            agent->HasMeshDestination = true;
            agent->Path.clear();
            for (const glm::vec3& c : corners) agent->Path.push_back(ToMesh(scene, agent->Mesh.get(), c));
            agent->PathLinks.assign(agent->Path.size(), -1.0f);
            agent->PathLinkOwners.assign(agent->Path.size(), Entity::Null());
            agent->Corner = agent->Path.size() > 1 ? 1 : 0;
            agent->PathStatus = status;
            agent->PathDirty = agent->Waiting = agent->Queued = false;
            agent->CrossMesh.reset();
            agent->CrossLink = Entity::Null();
            return true;
        }

        std::vector<glm::vec3> GetPath(Scene& scene, Entity e)
        {
            std::vector<glm::vec3> out;
            NavMeshAgentComponent* agent = AgentOf(scene, e);
            if (!agent || !agent->HasDestination) return out;
            for (const glm::vec3& c : agent->Path) out.push_back(ToWorld(scene, agent->Mesh.get(), c));
            return out;
        }

        glm::vec3 GetNextPosition(Scene& scene, Entity e)
        {
            NavMeshAgentComponent* agent = AgentOf(scene, e);
            bool simulated = agent && agent->NextValid && !agent->UpdatePosition;
            return simulated ? agent->NextPosition : scene.GetRegistry().Get<TransformComponent>(e).GetWorldPosition();
        }

        void SetNextPosition(Scene& scene, Entity e, const glm::vec3& position)
        {
            NavMeshAgentComponent* agent = AgentOf(scene, e);
            if (!agent) return;
            agent->NextPosition = position;
            agent->NextValid = true;
            if (agent->UpdatePosition) PlaceObject(scene, e, position);
        }

        void SetUpdatePosition(Scene& scene, Entity e, bool update)
        {
            NavMeshAgentComponent* agent = AgentOf(scene, e);
            if (!agent || agent->UpdatePosition == update) return;
            if (!update) agent->NextPosition = scene.GetRegistry().Get<TransformComponent>(e).GetWorldPosition(), agent->NextValid = true;
            else if (agent->NextValid) PlaceObject(scene, e, agent->NextPosition);
            agent->UpdatePosition = update;
        }

        bool SetDestination(Scene& scene, Entity e, const glm::vec3& destination)
        {
            NavMeshAgentComponent* agent = AgentOf(scene, e);
            if (!agent) return false;
            agent->Destination = destination;
            agent->HasDestination = true;
            agent->HasMeshDestination = false;
            Repath(scene, e, *agent);
            return agent->PathStatus != NavPathStatus::Invalid;
        }

        void ResetPath(Scene& scene, Entity e)
        {
            if (NavMeshAgentComponent* agent = AgentOf(scene, e))
            {
                agent->HasDestination = agent->PathDirty = false;
                agent->Path.clear();
                agent->PathStatus = NavPathStatus::Invalid;
                agent->RemainingDistance = 0.0f;
            }
        }

        bool Warp(Scene& scene, Entity e, const glm::vec3& position)
        {
            NavMeshAgentComponent* agent = AgentOf(scene, e);
            if (!agent) return false;
            int node = -1;
            auto mesh = MeshNear(scene, position, std::max(agent->Height, 2.0f), &node);
            if (!mesh) return false;
            agent->Mesh = mesh;
            agent->Node = node;
            agent->Velocity = agent->DesiredVelocity = glm::vec3(0.0f);
            agent->Placed = agent->HasMeshDestination = false;
            glm::vec3 local = ToMesh(scene, mesh.get(), position);
            PlaceAgent(scene, e, *agent, OnMesh(*mesh, node, { local.x, local.z }));
            if (agent->HasDestination) agent->PathDirty = true;
            return true;
        }

        void SetPathBudget(int pathsPerFrame) { Project::GetConfig().NavPathBudget = std::max(pathsPerFrame, 1); }
        int GetPathBudget() { return std::max(Project::GetConfig().NavPathBudget, 1); }

        void ApplyAreaNames()
        {
            // The Area dropdown shows the project's names.
            for (const char* component : { "NavMesh Modifier Volume", "NavMesh Modifier" })
                if (ComponentInfo* info = ComponentRegistry::FindMutable(component))
                    for (FieldInfo& f : info->Fields)
                        if (f.Name == "Area") f.EnumNames.assign(Project::GetConfig().NavAreaNames.begin(), Project::GetConfig().NavAreaNames.end());
            std::vector<std::string> types = { "Any" };
            for (const auto& t : Project::GetConfig().NavAgentTypes) types.push_back(t.Name);
            for (const char* component : { "NavMesh Surface", "NavMesh Agent" })
                if (ComponentInfo* info = ComponentRegistry::FindMutable(component))
                    for (FieldInfo& f : info->Fields)
                        if (f.Name == "Agent Type") f.EnumNames = types;
        }

        NavMeshSurfaceComponent WithAgentType(const NavMeshSurfaceComponent& surface)
        {
            NavMeshSurfaceComponent s = surface;
            const auto& types = Project::GetConfig().NavAgentTypes;
            if (s.AgentType >= 1 && s.AgentType <= (int)types.size())
            {
                const auto& type = types[s.AgentType - 1];
                s.AgentRadius = type.Radius;
                s.AgentHeight = type.Height;
                s.StepHeight = type.StepHeight;
                s.MaxSlope = type.MaxSlope;
            }
            return s;
        }

        glm::mat4 MeshFrame(Scene& scene, const NavMeshData* mesh)
        {
            const NavMeshSurfaceComponent* s = MovingSurfaceOf(scene, mesh);
            return s ? s->Frame : glm::mat4(1.0f);
        }

        void FollowPlatforms(Scene& scene)
        {
            // Each Moving surface's frame now; the agents on it go (and turn) with it as it moved since.
            auto& reg = scene.GetRegistry();
            for (auto [e, s, t] : reg.ViewOf<NavMeshSurfaceComponent, TransformComponent>())
            {
                if (!s.Moving || !s.Baked) continue;
                glm::mat4 frame = t.World * glm::inverse(s.BakedAt);
                glm::mat4 moved = frame * s.FrameInverse;
                s.Frame = frame;
                s.FrameInverse = glm::inverse(frame);
                float turned = YawOf(moved);
                bool still = true;
                for (int c = 0; c < 4 && still; ++c) still = glm::all(glm::lessThan(glm::abs(moved[c] - glm::mat4(1.0f)[c]), glm::vec4(1e-6f)));
                if (still) continue;
                for (auto [a, agent, at] : reg.ViewOf<NavMeshAgentComponent, TransformComponent>())
                {
                    if (!agent.Mesh || (agent.Mesh != s.Data && agent.Mesh != s.Baked)) continue;
                    if (agent.NextValid) agent.NextPosition = glm::vec3(moved * glm::vec4(agent.NextPosition, 1.0f));
                    if (agent.UpdatePosition) PlaceObject(scene, a, glm::vec3(moved * glm::vec4(at.GetWorldPosition(), 1.0f)));
                    if (agent.UpdateRotation) at.Rotation.y += turned;
                }
            }
        }

        void Update(Scene& scene, float dt)
        {
            FinishBakes(scene); // (background bakes that are done)
            if (dt <= 0.0f) return;
            auto& reg = scene.GetRegistry();
            FollowPlatforms(scene);
            StreamTilesAsync(scene); // streamed surfaces: tiles near the agents baked on a worker thread

            // Obstacles: how they move, and whether (and where) they cut the meshes. Those not cutting are steered
            // around.
            struct Blocker
            {
                glm::vec2 P, V;
                float R, Y, HalfHeight;
            };
            std::vector<Blocker> blockers;
            std::vector<CarveShape> carving;
            uint64_t carveKey = 1469598103934665603ull;
            auto hashIn = [&](const void* data, size_t bytes) {
                const auto* words = (const uint32_t*)data;
                for (size_t i = 0; i < bytes / 4; ++i) carveKey = (carveKey ^ words[i]) * 1099511628211ull;
            };
            for (auto [e, ob, t] : reg.ViewOf<NavMeshObstacleComponent, TransformComponent>())
            {
                if (!t.ActiveInHierarchy) continue;
                glm::vec3 at = glm::vec3(t.World * glm::vec4(ob.Center, 1.0f));
                ob.Velocity = ob.Seen ? (at - ob.LastPosition) / dt : glm::vec3(0.0f);
                ob.Seen = true;
                ob.LastPosition = at;
                ob.StillTime = glm::length(ob.Velocity) > 0.02f ? 0.0f : ob.StillTime + dt;
                bool carve = ob.Carve && (!ob.CarveOnlyStationary || ob.StillTime >= ob.TimeToStationary);
                if (carve)
                {
                    // Cut again only once it has moved Move Threshold (or turned) since it last did.
                    float turned = glm::length(glm::vec3(ob.CarvedAt[0]) - glm::vec3(t.World[0])) + glm::length(glm::vec3(ob.CarvedAt[2]) - glm::vec3(t.World[2]));
                    if (!ob.Carving || glm::distance(glm::vec3(ob.CarvedAt[3]), glm::vec3(t.World[3])) > ob.MoveThreshold || turned > 0.05f)
                        ob.CarvedAt = t.World;
                    carving.push_back({ ob.CarvedAt, ob.Shape, ob.Center, ob.Size, ob.Radius, ob.Height });
                    hashIn(&e.Index, 4);
                    hashIn(&ob.CarvedAt, sizeof(glm::mat4));
                    float shape[6] = { (float)ob.Shape, ob.Size.x, ob.Size.y, ob.Size.z, ob.Radius, ob.Height };
                    hashIn(shape, sizeof(shape));
                }
                ob.Carving = carve;
                if (carve) continue;
                glm::vec3 ext;
                float halfHeight;
                if (ob.Shape == NavObstacleShape::Box)
                {
                    ext = glm::abs(glm::vec3(t.World[0]) * ob.Size.x * 0.5f) + glm::abs(glm::vec3(t.World[1]) * ob.Size.y * 0.5f) + glm::abs(glm::vec3(t.World[2]) * ob.Size.z * 0.5f);
                    halfHeight = ext.y;
                }
                else
                {
                    float r = ob.Radius * std::max(glm::length(glm::vec3(t.World[0])), glm::length(glm::vec3(t.World[2])));
                    ext = glm::vec3(r, 0.0f, r);
                    halfHeight = 0.5f * ob.Height * glm::length(glm::vec3(t.World[1]));
                }
                blockers.push_back({ { at.x, at.z }, { ob.Velocity.x, ob.Velocity.z }, ob.Shape == NavObstacleShape::Box ? glm::length(glm::vec2(ext.x, ext.z)) : ext.x, at.y, halfHeight });
            }
            if (carving.empty()) carveKey = 0;
            for (auto [e, s] : reg.ViewOf<NavMeshSurfaceComponent>())
            {
                if (!s.Baked || s.CarveKey == carveKey || s.Moving) continue; // obstacles do not cut Moving surfaces
                std::shared_ptr<const NavMeshData> old = s.Data;
                s.Data = carving.empty() ? s.Baked : std::shared_ptr<const NavMeshData>(Carve(*s.Baked, carving));
                s.CarveKey = carveKey;
                // Same node numbers: agents keep their place, with paths found again around the holes.
                for (auto [a, agent] : reg.ViewOf<NavMeshAgentComponent>())
                    if (agent.Mesh && agent.Mesh == old)
                    {
                        agent.Mesh = s.Data;
                        if (agent.HasDestination) agent.PathDirty = true;
                    }
            }

            // Moves on the mesh from p toward target, sliding along its edges where it is blocked; `node` follows.
            auto slide = [](const NavMeshData& mesh, int& node, glm::vec2 p, glm::vec2 target, const Filter& filter) {
                glm::vec2 reached = p;
                int end = node;
                if (glm::length(target - p) <= 1e-7f) return p;
                if (Walk(mesh, node, p, target, &end, &reached, filter))
                {
                    node = end;
                    return target;
                }
                int along = node;
                glm::vec2 slid;
                if (Walk(mesh, node, p, { target.x, p.y }, &along, &slid, filter) || Walk(mesh, node, p, { p.x, target.y }, &along, &slid, filter))
                {
                    node = along;
                    return slid;
                }
                node = end;
                return reached;
            };

            struct Mover
            {
                Entity E;
                NavMeshAgentComponent* Agent;
                glm::vec3 Ground;
                bool Driven; // moved by its Animator's root motion
                const NavMeshSurfaceComponent* Platform = nullptr; // on a Moving surface (Ground and the rest in its space)
            };
            std::vector<Mover> movers;
            for (auto [e, agent, t] : reg.ViewOf<NavMeshAgentComponent, TransformComponent>())
                if (t.ActiveInHierarchy)
                {
                    const auto* animator = reg.TryGet<AnimatorComponent>(e);
                    movers.push_back({ e, &agent, glm::vec3(0.0f), agent.RootMotion && animator && animator->ApplyRootMotion });
                }
            int pathsThisFrame = 0;
            const int pathBudget = GetPathBudget();

            for (Mover& mv : movers)
            {
                NavMeshAgentComponent& agent = *mv.Agent;
                agent.RepathTimer -= dt;
                // Following a target: a new path whenever it has moved on (a few times a second at most).
                if (!agent.Target.IsNull() && scene.IsValid(agent.Target) && agent.RepathTimer <= 0.0f)
                {
                    glm::vec3 goal = reg.Get<TransformComponent>(agent.Target).GetWorldPosition();
                    if (!agent.HasDestination || glm::distance(goal, agent.Destination) > 0.3f || MovingSurfaceOf(scene, agent.Mesh.get()))
                    {
                        agent.Destination = goal;
                        agent.HasMeshDestination = false;
                        agent.HasDestination = agent.PathDirty = true;
                        agent.RepathTimer = 0.25f;
                    }
                }
                if (agent.Traversing)
                {
                    mv.Ground = GroundOf(scene, mv.E, agent);
                    continue;
                }
                if (mv.Driven && agent.Placed && agent.Mesh && agent.Node >= 0 && agent.Node < agent.Mesh->NodeCount())
                {
                    // The animation moved it since the last update: that step, kept on the mesh (a jump of meters was
                    // some other move, and is found again below).
                    glm::vec3 now = GroundOf(scene, mv.E, agent);
                    glm::vec2 from(agent.LastGround.x, agent.LastGround.z), to(now.x, now.z);
                    if (glm::distance(from, to) < 2.0f)
                    {
                        int node = agent.Node;
                        glm::vec2 reached = slide(*agent.Mesh, node, from, to, FilterOf(agent));
                        agent.Node = node;
                        glm::vec2 moved = (reached - from) / dt;
                        agent.Velocity = glm::vec3(moved.x, 0.0f, moved.y);
                        PlaceAgent(scene, mv.E, agent, glm::vec3(reached.x, HeightAt(*agent.Mesh, node, reached.x, reached.y), reached.y));
                    }
                }
                if (!Place(scene, mv.E, agent))
                {
                    agent.Velocity = glm::vec3(0.0f);
                    continue;
                }
                mv.Ground = GroundOf(scene, mv.E, agent);
                // Pushed off its line (by others, or cutting a corner) so the next corner is out of sight: a new path
                // from where it stands (not across an off-mesh link, which is not walked).
                bool onLink = agent.Corner >= 1 && agent.Corner - 1 < agent.PathLinks.size() && agent.PathLinks[agent.Corner - 1] >= 0.0f;
                if (!onLink && !agent.PathDirty && agent.HasDestination && agent.RepathTimer <= 0.0f && agent.Corner < agent.Path.size() &&
                    !Walk(*agent.Mesh, agent.Node, { mv.Ground.x, mv.Ground.z }, { agent.Path[agent.Corner].x, agent.Path[agent.Corner].z }, nullptr, nullptr, FilterOf(agent)))
                {
                    agent.PathDirty = true;
                    agent.RepathTimer = 0.2f;
                }
                // New paths (for following targets, after obstacles moved, or when pushed off the way) at most Path
                // Budget a frame: the rest keep to their old path until their turn.
                if (agent.PathDirty && pathsThisFrame < pathBudget)
                {
                    Repath(scene, mv.E, agent);
                    ++pathsThisFrame;
                }
                mv.Ground = GroundOf(scene, mv.E, agent);
            }

            for (Mover& mv : movers) mv.Platform = MovingSurfaceOf(scene, mv.Agent->Mesh.get());

            // Doors: the agents whose way goes next through a closed door (Agents Wait), within a few meters of it, line up
            // before it, nearest first, each about an agent's width behind the one ahead, rather than crowding it.
            {
                struct InLine
                {
                    Mover* M;
                    float Distance;
                };
                std::map<std::tuple<uint32_t, int, int>, std::vector<InLine>> queues; // door, its start (to 10 cm)
                std::map<std::tuple<uint32_t, int, int>, glm::vec3> starts;
                for (Mover& mv : movers)
                {
                    NavMeshAgentComponent& a = *mv.Agent;
                    a.Queued = false;
                    if (!a.Mesh || a.Traversing || !a.HasDestination || a.IsStopped || a.Path.size() < 2) continue;
                    for (size_t k = a.Corner > 0 ? a.Corner - 1 : 0; k + 1 < a.Path.size() && k < a.PathLinks.size(); ++k)
                    {
                        if (a.PathLinks[k] < 0.0f) continue;
                        Entity owner = k < a.PathLinkOwners.size() ? a.PathLinkOwners[k] : Entity::Null();
                        const auto* door = !owner.IsNull() && scene.IsValid(owner) ? reg.TryGet<OffMeshLinkComponent>(owner) : nullptr;
                        if (door && door->AgentsWait && !door->Activated)
                        {
                            float d = Horizontal(mv.Ground, a.Path[k]);
                            if (d < 8.0f)
                            {
                                auto key = std::make_tuple(owner.Index, (int)std::lround(a.Path[k].x * 10.0f), (int)std::lround(a.Path[k].z * 10.0f));
                                queues[key].push_back({ &mv, d });
                                starts[key] = a.Path[k];
                            }
                        }
                        break; // only the next link on the way
                    }
                }
                for (auto& [key, line] : queues)
                {
                    std::sort(line.begin(), line.end(), [](const InLine& x, const InLine& y) { return x.Distance < y.Distance; });
                    glm::vec3 start = starts[key];
                    // The line runs back from the door the way they come.
                    glm::vec2 back(0.0f);
                    float widest = 0.0f;
                    for (const InLine& q : line)
                    {
                        back += glm::vec2(q.M->Ground.x - start.x, q.M->Ground.z - start.z);
                        widest = std::max(widest, q.M->Agent->Radius);
                    }
                    back = glm::length(back) > 1e-3f ? glm::normalize(back) : glm::vec2(0.0f, 1.0f);
                    float spacing = widest * 2.0f + 0.3f;
                    for (size_t r = 0; r < line.size(); ++r)
                    {
                        NavMeshAgentComponent& a = *line[r].M->Agent;
                        a.Queued = true;
                        a.QueueSpot = start + glm::vec3(back.x, 0.0f, back.y) * (spacing * (float)r);
                    }
                }
                for (Mover& mv : movers) mv.Agent->Waiting = mv.Agent->Queued;
            }

            // Agents by where they stand, in cells of a few meters, for finding the neighbours to steer around.
            constexpr float kCell = 4.0f;
            auto cellKey = [](int x, int z) { return ((int64_t)x << 32) ^ (int64_t)(uint32_t)z; };
            std::unordered_map<int64_t, std::vector<int>> cells;
            float maxRadius = 0.0f, maxSpeed = 0.0f;
            for (int i = 0; i < (int)movers.size(); ++i)
            {
                const Mover& o = movers[i];
                if (!o.Agent->Mesh) continue;
                cells[cellKey((int)std::floor(o.Ground.x / kCell), (int)std::floor(o.Ground.z / kCell))].push_back(i);
                maxRadius = std::max(maxRadius, o.Agent->Radius);
                maxSpeed = std::max({ maxSpeed, o.Agent->Speed, glm::length(o.Agent->Velocity) });
            }

            for (Mover& mv : movers)
            {
                NavMeshAgentComponent& agent = *mv.Agent;
                if (!agent.Mesh) continue;
                auto& transform = reg.Get<TransformComponent>(mv.E);
                // Face the way it goes.
                auto face = [&](glm::vec2 v2) {
                    if (glm::length(v2) <= 0.05f || !agent.UpdateRotation) return;
                    float want = std::atan2(-v2.x, -v2.y) + (mv.Platform ? YawOf(mv.Platform->Frame) : 0.0f);
                    float turn = WrapAngle(want - transform.Rotation.y);
                    float maxTurn = glm::radians(agent.AngularSpeed) * dt;
                    transform.Rotation.y = WrapAngle(transform.Rotation.y + std::clamp(turn, -maxTurn, maxTurn));
                };
                if (agent.Traversing)
                {
                    // Crossing an off-mesh link: straight from end to end, Arc Height up in the middle. With the link's
                    // animation and Match Animation, it takes as long as that animation (known once it has played a frame).
                    if (agent.TraverseMatch)
                        if (auto* an = reg.TryGet<AnimatorComponent>(mv.E); an && an->Info.Valid && an->Info.Length > 0.0f &&
                                                                              (an->CurrentState == agent.TraverseAnimation || an->Clip == agent.TraverseAnimation))
                        {
                            agent.TraverseDuration = std::max(an->Info.Length / std::max(std::abs(an->Info.Speed * an->Speed), 0.01f), 0.1f);
                            agent.TraverseMatch = false;
                        }
                    agent.TraverseTime += dt;
                    float t = std::clamp(agent.TraverseTime / std::max(agent.TraverseDuration, 1e-3f), 0.0f, 1.0f);
                    mv.Ground = glm::mix(agent.TraverseFrom, agent.TraverseTo, t) + glm::vec3(0.0f, agent.TraverseArc * 4.0f * t * (1.0f - t), 0.0f);
                    glm::vec3 across = (agent.TraverseTo - agent.TraverseFrom) / std::max(agent.TraverseDuration, 1e-3f);
                    agent.Velocity = glm::vec3(across.x, 0.0f, across.z);
                    PlaceAgent(scene, mv.E, agent, mv.Ground);
                    face({ across.x, across.z });
                    if (t >= 1.0f)
                    {
                        agent.Traversing = false;
                        agent.TraverseMatch = false;
                        if (!agent.TraverseAnimation.empty())
                        {
                            // Back to what it was doing (its controller's state; without one, the clip it played).
                            if (!agent.TraverseReturnState.empty()) scene.PlayAnimatorState(mv.E, agent.TraverseReturnState, 0, 0.2f, 0.0f, false);
                            agent.TraverseAnimation.clear();
                        }
                        if (agent.CrossMesh && agent.Corner + 1 >= agent.Path.size())
                        {
                            // Over onto the other surface: on its mesh from here, and a path there.
                            glm::vec3 world = ToWorld(scene, agent.Mesh.get(), agent.TraverseTo);
                            agent.Mesh = agent.CrossMesh;
                            agent.CrossMesh.reset();
                            agent.CrossLink = Entity::Null();
                            mv.Ground = ToMesh(scene, agent.Mesh.get(), world);
                            agent.Node = Nearest(*agent.Mesh, mv.Ground, 1.0f, FilterOf(agent));
                            agent.Path.clear();
                            agent.PathDirty = true;
                            agent.Placed = false;
                        }
                        else
                            agent.Node = Nearest(*agent.Mesh, agent.TraverseTo, 1.0f, FilterOf(agent));
                    }
                    continue;
                }
                if (agent.Node < 0) continue;
                const NavMeshData& mesh = *agent.Mesh;
                glm::vec2 p(mv.Ground.x, mv.Ground.z);

                // Where it wants to go: on to the next corner, slowing to arrive.
                glm::vec2 desired(0.0f);
                bool moving = agent.HasDestination && agent.Path.size() >= 2 && agent.PathStatus != NavPathStatus::Invalid && !agent.Queued;
                agent.RemainingDistance = 0.0f;
                float brakeDistance = kInf;
                if (moving)
                {
                    size_t last = agent.Path.size() - 1;
                    agent.Corner = std::clamp<size_t>(agent.Corner, 1, last);
                    while (agent.Corner < last && Horizontal(mv.Ground, agent.Path[agent.Corner]) < std::max(0.1f, glm::length(agent.Velocity) * dt * 1.5f))
                        ++agent.Corner;
                    // Moved by its animation it cannot hit corners exactly: on to the next one as soon as it is in
                    // straight sight (not past an off-mesh link).
                    while (mv.Driven && agent.Corner < last && !(agent.Corner < agent.PathLinks.size() && agent.PathLinks[agent.Corner] >= 0.0f) &&
                           !(agent.Corner - 1 < agent.PathLinks.size() && agent.PathLinks[agent.Corner - 1] >= 0.0f) &&
                           Walk(mesh, agent.Node, p, { agent.Path[agent.Corner + 1].x, agent.Path[agent.Corner + 1].z }, nullptr, nullptr, FilterOf(agent)))
                        ++agent.Corner;
                    float remaining = Horizontal(mv.Ground, agent.Path[agent.Corner]);
                    for (size_t k = agent.Corner; k < last; ++k) remaining += Horizontal(agent.Path[k], agent.Path[k + 1]);
                    agent.RemainingDistance = remaining;
                    brakeDistance = remaining - agent.StoppingDistance;
                    if (!agent.IsStopped && agent.Corner - 1 < agent.PathLinks.size() && agent.PathLinks[agent.Corner - 1] >= 0.0f)
                    {
                        // A closed door: wait at it while agents wait there, else find another way.
                        Entity owner = agent.Corner - 1 < agent.PathLinkOwners.size() ? agent.PathLinkOwners[agent.Corner - 1] : Entity::Null();
                        const auto* door = !owner.IsNull() && scene.IsValid(owner) ? reg.TryGet<OffMeshLinkComponent>(owner) : nullptr;
                        if (!owner.IsNull() && (!door || !door->Activated))
                        {
                            if (door && door->AgentsWait)
                                agent.Waiting = true;
                            else
                                agent.PathDirty = true;
                            agent.Velocity = agent.DesiredVelocity = glm::vec3(0.0f);
                            continue;
                        }
                        agent.Waiting = false;
                        // At an off-mesh link's start: across it, at walking speed.
                        agent.Traversing = true;
                        agent.TraverseFrom = mv.Ground;
                        agent.TraverseTo = agent.Path[agent.Corner];
                        if (agent.CrossMesh && agent.Corner + 1 == agent.Path.size() && scene.IsValid(agent.CrossLink))
                        {
                            // Over to another surface: its far end where the link is now.
                            const auto& lt = reg.Get<TransformComponent>(agent.CrossLink);
                            agent.TraverseTo = ToMesh(scene, agent.Mesh.get(), glm::vec3(lt.World * glm::vec4(agent.CrossEnd, 1.0f)));
                        }
                        agent.TraverseArc = agent.PathLinks[agent.Corner - 1];
                        agent.TraverseTime = 0.0f;
                        agent.TraverseDuration = std::max(glm::distance(agent.TraverseFrom, agent.TraverseTo) / std::max(agent.Speed, 0.1f), 0.25f);
                        // The link's animation (a jump, a climb) on the agent's Animator.
                        agent.TraverseAnimation.clear();
                        agent.TraverseMatch = false;
                        const auto* link = !owner.IsNull() && scene.IsValid(owner) ? reg.TryGet<OffMeshLinkComponent>(owner) : nullptr;
                        if (auto* an = reg.TryGet<AnimatorComponent>(mv.E); link && an && !link->Animation.empty())
                        {
                            agent.TraverseReturnState = an->Controller.IsValid() ? an->CurrentState : an->Clip;
                            if (scene.PlayAnimatorState(mv.E, link->Animation, 0, 0.1f, 0.0f, false))
                            {
                                agent.TraverseAnimation = link->Animation;
                                agent.TraverseMatch = link->MatchAnimation;
                            }
                        }
                        agent.PathLinks[agent.Corner - 1] = -1.0f;
                        agent.DesiredVelocity = glm::vec3(0.0f);
                        continue;
                    }
                    if (!agent.IsStopped && brakeDistance > 0.03f)
                    {
                        glm::vec2 to = glm::vec2(agent.Path[agent.Corner].x, agent.Path[agent.Corner].z) - p;
                        float len = glm::length(to);
                        float speed = agent.Speed;
                        if (agent.AutoBraking) speed = std::min(speed, std::sqrt(2.0f * std::max(agent.Acceleration, 0.1f) * brakeDistance));
                        if (len > 1e-5f) desired = to / len * speed;
                    }
                }
                if (agent.Queued)
                {
                    // In line at a door: to its spot, and stand there.
                    glm::vec2 to = glm::vec2(agent.QueueSpot.x, agent.QueueSpot.z) - p;
                    float len = glm::length(to);
                    if (len > 0.05f) desired = to / len * std::min(agent.Speed, std::sqrt(2.0f * std::max(agent.Acceleration, 0.1f) * len));
                }

                // Avoidance: of a fan of velocities around the wanted one, the one nearest it that does not run into
                // another agent soon (agents with a higher priority number are left to give way themselves).
                glm::vec2 chosen = desired;
                if (agent.ObstacleAvoidance)
                {
                    struct Other
                    {
                        glm::vec2 P, V;
                        float R;
                        bool Shared = false; // an agent of the same priority: each of the two dodges half (reciprocal)
                        bool Idle = false;   // standing (arrived, stopped, nowhere to go): gets pushed aside rather than kept clear of
                    };
                    std::vector<Other> others;
                    float reach = agent.Radius + agent.Speed * 2.0f;
                    // The nearest few agents in the cells around (a crowd steers by its neighbours, not by everyone).
                    float search = reach + maxRadius + maxSpeed * 2.0f;
                    int cx0 = (int)std::floor((p.x - search) / kCell), cx1 = (int)std::floor((p.x + search) / kCell);
                    int cz0 = (int)std::floor((p.y - search) / kCell), cz1 = (int)std::floor((p.y + search) / kCell);
                    std::vector<std::pair<float, Other>> near;
                    for (int cz = cz0; cz <= cz1; ++cz)
                        for (int cx = cx0; cx <= cx1; ++cx)
                        {
                            auto it = cells.find(cellKey(cx, cz));
                            if (it == cells.end()) continue;
                            for (int i : it->second)
                            {
                                const Mover& o = movers[i];
                                if (o.E == mv.E || o.Agent->AvoidancePriority > agent.AvoidancePriority) continue;
                                if ((o.Platform || mv.Platform) && o.Agent->Mesh != agent.Mesh) continue; // another space
                                glm::vec2 op(o.Ground.x, o.Ground.z);
                                glm::vec2 ov(o.Agent->Velocity.x, o.Agent->Velocity.z);
                                if (std::abs(o.Ground.y - mv.Ground.y) > agent.Height) continue;
                                float d = glm::distance(op, p);
                                if (d > reach + o.Agent->Radius + glm::length(ov) * 2.0f) continue;
                                const NavMeshAgentComponent& oa = *o.Agent;
                                bool idle = !oa.HasDestination || oa.IsStopped || oa.Waiting || oa.PathStatus == NavPathStatus::Invalid ||
                                            oa.RemainingDistance <= oa.StoppingDistance + 0.05f;
                                near.push_back({ d, { op, ov, agent.Radius + oa.Radius, oa.AvoidancePriority == agent.AvoidancePriority, idle } });
                            }
                        }
                    constexpr size_t kMaxNeighbours = 10;
                    if (near.size() > kMaxNeighbours)
                        std::partial_sort(near.begin(), near.begin() + kMaxNeighbours, near.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
                    for (size_t i = 0; i < near.size() && i < kMaxNeighbours; ++i) others.push_back(near[i].second);
                    for (Blocker b : blockers)
                    {
                        if (mv.Platform)
                        {
                            // Into the platform's space.
                            glm::vec3 at = glm::vec3(mv.Platform->FrameInverse * glm::vec4(b.P.x, b.Y, b.P.y, 1.0f));
                            glm::vec3 v = glm::mat3(mv.Platform->FrameInverse) * glm::vec3(b.V.x, 0.0f, b.V.y);
                            b.P = { at.x, at.z };
                            b.Y = at.y;
                            b.V = { v.x, v.z };
                        }
                        if (std::abs(b.Y - mv.Ground.y - agent.Height * 0.5f) < b.HalfHeight + agent.Height * 0.5f &&
                            glm::distance(b.P, p) < reach + b.R + glm::length(b.V) * 2.0f)
                            others.push_back({ b.P, b.V, agent.Radius + b.R });
                    }
                    if (!others.empty())
                    {
                        const float horizon = 2.0f;
                        glm::vec2 ownVelocity(agent.Velocity.x, agent.Velocity.z);
                        auto cost = [&](glm::vec2 v) {
                            float c = glm::length(v - desired) / std::max(agent.Speed, 0.1f);
                            for (const Other& o : others)
                            {
                                glm::vec2 rp = o.P - p, rv = o.Shared ? 2.0f * v - ownVelocity - o.V : v - o.V;
                                float c2 = glm::dot(rp, rp) - o.R * o.R;
                                float b = glm::dot(rp, rv);
                                if (c2 < 0.0f)
                                {
                                    if (b > 0.0f) c += o.Idle ? 0.3f + 0.2f * b : 3.0f + b; // overlapping and closing in
                                    continue;
                                }
                                float a = glm::dot(rv, rv);
                                float disc = b * b - a * c2;
                                if (b <= 0.0f || a < 1e-8f || disc <= 0.0f) continue;
                                float t = (b - std::sqrt(disc)) / a;
                                if (t < horizon) c += (o.Idle ? 0.4f : 2.5f) * (horizon - t) / horizon;
                            }
                            return c;
                        };
                        glm::vec2 forward = glm::length(desired) > 1e-4f ? glm::normalize(desired)
                                            : glm::length(glm::vec2(agent.Velocity.x, agent.Velocity.z)) > 1e-4f ? glm::normalize(glm::vec2(agent.Velocity.x, agent.Velocity.z))
                                                                                                                 : glm::vec2(0.0f, -1.0f);
                        glm::vec2 right(-forward.y, forward.x);
                        float speed = glm::length(desired) > 1e-4f ? glm::length(desired) : agent.Speed * 0.5f;
                        float best = cost(desired);
                        float zero = cost(glm::vec2(0.0f));
                        if (zero < best)
                        {
                            best = zero;
                            chosen = glm::vec2(0.0f);
                        }
                        for (float deg : { 15.0f, -15.0f, 30.0f, -30.0f, 45.0f, -45.0f, 60.0f, -60.0f, 90.0f, -90.0f, 120.0f, -120.0f, 180.0f })
                            for (float scale : { 1.0f, 0.6f, 0.3f })
                            {
                                float a = glm::radians(deg);
                                glm::vec2 v = (forward * std::cos(a) + right * std::sin(a)) * speed * scale;
                                float c = cost(v) + (deg < 0.0f ? 0.02f : 0.0f); // passing on the right breaks ties the same way for both
                                if (c < best)
                                {
                                    best = c;
                                    chosen = v;
                                }
                            }
                    }
                }

                // Accelerate toward it (from how fast it goes; driven by its animation, from what it asked for last),
                // then move on the mesh (sliding along its edges).
                glm::vec2 vel = mv.Driven ? glm::vec2(agent.DesiredVelocity.x, agent.DesiredVelocity.z) : glm::vec2(agent.Velocity.x, agent.Velocity.z);
                glm::vec2 change = chosen - vel;
                float maxChange = agent.Acceleration > 0.0f ? agent.Acceleration * dt : kInf;
                if (glm::length(change) > maxChange) change = glm::normalize(change) * maxChange;
                vel += change;
                agent.DesiredVelocity = glm::vec3(vel.x, 0.0f, vel.y);
                if (mv.Driven)
                {
                    // Its animation takes the steps (next update keeps them on the mesh): it only turns it.
                    face(vel);
                    continue;
                }
                glm::vec2 step = vel * dt;
                if (moving && agent.Corner + 1 == agent.Path.size() && glm::length(step) > brakeDistance && brakeDistance > 0.0f)
                    step = glm::normalize(step) * brakeDistance; // no overshooting the end
                int node = agent.Node;
                glm::vec2 reached = slide(mesh, node, p, p + step, FilterOf(agent));
                agent.Node = node;
                glm::vec2 moved = (reached - p) / dt;
                agent.Velocity = glm::vec3(moved.x, 0.0f, moved.y);
                mv.Ground = glm::vec3(reached.x, HeightAt(mesh, node, reached.x, reached.y), reached.y);
                PlaceAgent(scene, mv.E, agent, mv.Ground);
                face({ agent.Velocity.x, agent.Velocity.z });
            }

            // Collisions: agents still overlapping are pushed apart, each by half (the less important one, by its
            // Avoidance Priority, all the way), along the mesh.
            auto push = [&](Mover& mv, glm::vec2 delta) {
                NavMeshAgentComponent& agent = *mv.Agent;
                const NavMeshData& mesh = *agent.Mesh;
                glm::vec2 p(mv.Ground.x, mv.Ground.z), reached = p + delta;
                int node = agent.Node;
                if (!Walk(mesh, agent.Node, p, p + delta, &node, &reached, FilterOf(agent))) {}
                agent.Node = node;
                mv.Ground = glm::vec3(reached.x, HeightAt(mesh, node, reached.x, reached.y), reached.y);
                PlaceAgent(scene, mv.E, agent, mv.Ground);
            };
            for (int pass = 0; pass < 2; ++pass)
                for (int i = 0; i < (int)movers.size(); ++i)
                {
                    Mover& a = movers[i];
                    if (!a.Agent->Mesh || a.Agent->Node < 0 || a.Agent->Traversing) continue;
                    int cx = (int)std::floor(a.Ground.x / kCell), cz = (int)std::floor(a.Ground.z / kCell);
                    for (int dz = -1; dz <= 1; ++dz)
                        for (int dx = -1; dx <= 1; ++dx)
                        {
                            auto it = cells.find(cellKey(cx + dx, cz + dz));
                            if (it == cells.end()) continue;
                            for (int j : it->second)
                            {
                                if (j <= i) continue;
                                Mover& b = movers[j];
                                if (!b.Agent->Mesh || b.Agent->Node < 0 || b.Agent->Traversing || b.Agent->Mesh != a.Agent->Mesh) continue;
                                if (std::abs(a.Ground.y - b.Ground.y) > std::max(a.Agent->Height, b.Agent->Height)) continue;
                                glm::vec2 d(b.Ground.x - a.Ground.x, b.Ground.z - a.Ground.z);
                                float dist = glm::length(d), overlap = a.Agent->Radius + b.Agent->Radius - dist;
                                if (overlap <= 0.0f) continue;
                                glm::vec2 dir = dist > 1e-4f ? d / dist : glm::vec2(1.0f, 0.0f);
                                float shareA = a.Agent->AvoidancePriority > b.Agent->AvoidancePriority ? 1.0f
                                             : a.Agent->AvoidancePriority < b.Agent->AvoidancePriority ? 0.0f : 0.5f;
                                if (shareA > 0.0f) push(a, -dir * overlap * shareA);
                                if (shareA < 1.0f) push(b, dir * overlap * (1.0f - shareA));
                            }
                        }
                }

            // Where each stands now (the next update finds how far its animation took it), and its Animator's speed.
            for (Mover& mv : movers)
            {
                NavMeshAgentComponent& agent = *mv.Agent;
                agent.LastGround = mv.Ground;
                agent.Placed = agent.Mesh && agent.Node >= 0;
                if (agent.SpeedParameter.empty()) continue;
                if (auto* animator = reg.TryGet<AnimatorComponent>(mv.E))
                {
                    glm::vec2 v = mv.Driven && !agent.Traversing ? glm::vec2(agent.DesiredVelocity.x, agent.DesiredVelocity.z) : glm::vec2(agent.Velocity.x, agent.Velocity.z);
                    float speed = glm::length(v);
                    if (mv.Driven && !agent.Traversing && speed > 1e-4f)
                    {
                        // Slower while it still turns toward the way (stopping to turn round), so the animation's
                        // steps follow the path rather than run wide.
                        float yaw = reg.Get<TransformComponent>(mv.E).Rotation.y;
                        speed *= std::clamp(glm::dot(v / speed, glm::vec2(-std::sin(yaw), -std::cos(yaw))), 0.0f, 1.0f);
                    }
                    animator->SetFloat(agent.SpeedParameter, speed);
                }
            }
        }
    }
}
