#include "Dimension/Navigation/NavMesh.h"
#include "Dimension/Scene/Scene.h"
#include "Dimension/Physics/Physics3D.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <cstring>
#include <queue>
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

            // Only fixed geometry is walked on: not moving bodies, sensors, agents or characters.
            bool Ignored(Registry& reg, Entity e)
            {
                if (auto* rb = reg.TryGet<RigidbodyComponent>(e); rb && rb->Type == BodyType::Dynamic) return true;
                if (reg.Has<NavMeshAgentComponent>(e) || reg.Has<NavMeshObstacleComponent>(e) || reg.Has<CharacterControllerComponent>(e) || reg.Has<ClothComponent>(e) ||
                    reg.Has<SoftBodyComponent>(e) || reg.Has<RopeComponent>(e) || reg.Has<RagdollComponent>(e))
                    return true;
                if (auto* c = reg.TryGet<BoxColliderComponent>(e); c && c->IsTrigger) return true;
                if (auto* c = reg.TryGet<SphereColliderComponent>(e); c && c->IsTrigger) return true;
                if (auto* c = reg.TryGet<CapsuleColliderComponent>(e); c && c->IsTrigger) return true;
                if (auto* c = reg.TryGet<MeshColliderComponent>(e); c && c->IsTrigger) return true;
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

            // Keeps only the nodes for which keep[] is set.
            void Compact(NavMeshData& m, const std::vector<uint8_t>& keep)
            {
                std::vector<float> heights;
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
                        }
                }
                starts.back() = (uint32_t)heights.size();
                m.Heights = std::move(heights);
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

            std::shared_ptr<NavMeshData> BakeIn(Scene& scene, const NavMeshSurfaceComponent& s, const glm::vec3& center)
            {
                auto& reg = scene.GetRegistry();
                auto m = std::make_shared<NavMeshData>();
                float c = std::max(s.CellSize, 0.05f);
                glm::vec3 size = glm::max(s.Size, glm::vec3(0.1f));
                m->CellSize = c;
                m->Width = std::clamp((int)std::ceil(size.x / c), 1, 4096);
                m->Depth = std::clamp((int)std::ceil(size.z / c), 1, 4096);
                m->Origin = { center.x - m->Width * c * 0.5f, center.z - m->Depth * c * 0.5f };
                m->AgentRadius = std::max(s.AgentRadius, 0.0f);
                m->AgentHeight = std::max(s.AgentHeight, 0.1f);
                m->StepHeight = std::max(s.StepHeight, 0.0f);
                float top = center.y + size.y * 0.5f;
                float cosSlope = std::cos(glm::radians(std::clamp(s.MaxSlope, 0.0f, 89.0f)));
                const glm::vec3 down(0.0f, -1.0f, 0.0f), up(0.0f, 1.0f, 0.0f);

                // Every column: the surfaces under its center, top down. A surface is walkable when it is flat enough,
                // nothing seen above it is within Agent Height, and a ray up finds no underside (or solid) that close.
                m->ColumnStart.reserve((size_t)m->Width * m->Depth + 1);
                for (int z = 0; z < m->Depth; ++z)
                    for (int x = 0; x < m->Width; ++x)
                    {
                        m->ColumnStart.push_back((uint32_t)m->Heights.size());
                        glm::vec3 p(m->Origin.x + (x + 0.5f) * c, top, m->Origin.y + (z + 0.5f) * c);
                        float above = kInf;
                        for (const RaycastHit& h : Physics3DWorld::RaycastAll(scene, p, down, size.y, Physics::kDefaultQueryMask, true))
                        {
                            if (Ignored(reg, h.Hit)) continue;
                            float y = h.Point.y;
                            if (above - y < 0.02f) continue; // the same surface again (on a shared triangle edge)
                            bool walkable = h.Normal.y >= cosSlope && above - y >= m->AgentHeight;
                            above = y;
                            if (!walkable) continue;
                            bool blocked = false;
                            for (const RaycastHit& u : Physics3DWorld::RaycastAll(scene, h.Point + up * 0.02f, up, m->AgentHeight - 0.02f))
                                if (!Ignored(reg, u.Hit))
                                {
                                    blocked = true;
                                    break;
                                }
                            if (blocked) continue;
                            m->Heights.push_back(y);
                            m->Columns.push_back(z * m->Width + x);
                        }
                    }
                m->ColumnStart.push_back((uint32_t)m->Heights.size());
                BuildLinks(*m);

                // Room for the agent: drop the nodes nearer than Agent Radius to an edge (a node missing a side is half a
                // cell from it), measured along the mesh.
                int n = m->NodeCount();
                if (m->AgentRadius > 0.0f && n > 0)
                {
                    std::vector<float> dist(n, kInf);
                    using Item = std::pair<float, int>;
                    std::priority_queue<Item, std::vector<Item>, std::greater<>> open;
                    for (int a = 0; a < n; ++a)
                        for (int d = 0; d < 4; ++d)
                            if (m->Links[(size_t)a * 8 + d] < 0)
                            {
                                dist[a] = 0.5f * c;
                                open.push({ dist[a], a });
                                break;
                            }
                    while (!open.empty())
                    {
                        auto [da, a] = open.top();
                        open.pop();
                        if (da > dist[a]) continue;
                        for (int d = 0; d < 8; ++d)
                        {
                            int b = m->Links[(size_t)a * 8 + d];
                            float nd = da + (d < 4 ? c : c * 1.41421356f);
                            if (b >= 0 && nd < dist[b])
                            {
                                dist[b] = nd;
                                open.push({ nd, b });
                            }
                        }
                    }
                    std::vector<uint8_t> keep(n);
                    for (int a = 0; a < n; ++a) keep[a] = dist[a] >= m->AgentRadius - 1e-4f;
                    Compact(*m, keep);
                }
                BuildIslands(*m);
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
                                   const std::vector<Link>& links, std::vector<float>* linkArcs)
            {
                corners.clear();
                if (linkArcs) linkArcs->clear();
                if (start < 0 || start >= m.NodeCount() || m.IsBlocked(start)) return NavPathStatus::Invalid;
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
                int goal = Nearest(m, to, std::max(10.0f, m.AgentHeight * 2.0f));
                if (goal < 0 || !reachable[m.Islands[goal]])
                {
                    // Out of reach: as near to it as the reachable areas get.
                    status = NavPathStatus::Partial;
                    float best = kInf;
                    for (int a = 0; a < n; ++a)
                        if (reachable[m.Islands[a]] && !m.IsBlocked(a))
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
                std::vector<float> g(n, kInf);
                std::vector<int> parent(n, -1), parentLink(n, -1);
                std::vector<uint8_t> closed(n, 0);
                using Item = std::pair<float, int>;
                std::priority_queue<Item, std::vector<Item>, std::greater<>> open;
                glm::vec3 goalPos = m.NodePosition(goal);
                g[start] = 0.0f;
                open.push({ glm::distance(m.NodePosition(start), goalPos), start });
                while (!open.empty())
                {
                    int a = open.top().second;
                    open.pop();
                    if (closed[a]) continue;
                    closed[a] = 1;
                    if (a == goal) break;
                    glm::vec3 pa = m.NodePosition(a);
                    auto relax = [&](int b, float cost, int link) {
                        if (b < 0 || closed[b]) return;
                        float nd = g[a] + cost;
                        if (nd < g[b])
                        {
                            g[b] = nd;
                            parent[b] = a;
                            parentLink[b] = link;
                            open.push({ nd + glm::distance(m.NodePosition(b), goalPos), b });
                        }
                    };
                    for (int d = 0; d < 8; ++d)
                        if (int b = m.Links[(size_t)a * 8 + d]; b >= 0) relax(b, glm::distance(pa, m.NodePosition(b)), -1);
                    if (auto it = linksFrom.find(a); it != linksFrom.end())
                        for (int k : it->second) relax(links[k].To, links[k].Cost, k);
                }
                if (!closed[goal]) return NavPathStatus::Invalid;
                std::vector<int> nodes, arrivedBy; // arrivedBy: the link that led to the node (-1 = walked)
                for (int a = goal; a >= 0; a = parent[a])
                {
                    nodes.push_back(a);
                    arrivedBy.push_back(parentLink[a]);
                }
                std::reverse(nodes.begin(), nodes.end());
                std::reverse(arrivedBy.begin(), arrivedBy.end());

                // String pulling, between the links: from each corner, straight on to the farthest node in sight.
                glm::vec3 begin = OnMesh(m, start, { from.x, from.z });
                glm::vec3 end = status == NavPathStatus::Complete ? OnMesh(m, goal, { to.x, to.z }) : m.NodePosition(goal);
                corners.push_back(begin);
                std::vector<float> arcs{ -1.0f };
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
                            if (!Walk(m, nodes[i], cur, { pk.x, pk.z }, &endNode) || endNode != nodes[k]) break;
                            far = k;
                        }
                        corners.push_back(point(far));
                        arcs.push_back(-1.0f);
                        i = far;
                        cur = { corners.back().x, corners.back().z };
                    }
                    if (glm::distance(corners.back(), runFinish) > 1e-4f)
                    {
                        corners.push_back(runFinish);
                        arcs.push_back(-1.0f);
                    }
                    if (lastRun) break;
                    // Across the link: the corner before it starts the crossing.
                    arcs.back() = link->Arc;
                    runStart = runEnd + 1;
                    runBegin = OnMesh(m, nodes[runStart], { link->ToPoint.x, link->ToPoint.z });
                    corners.push_back(runBegin);
                    arcs.push_back(-1.0f);
                }
                if (corners.size() == 1)
                {
                    corners.push_back(end);
                    arcs.push_back(-1.0f);
                }
                if (linkArcs) *linkArcs = std::move(arcs);
                return status;
            }

            NavMeshAgentComponent* AgentOf(Scene& scene, Entity e) { return scene.IsValid(e) ? scene.GetRegistry().TryGet<NavMeshAgentComponent>(e) : nullptr; }

            glm::vec3 GroundOf(Scene& scene, Entity e, const NavMeshAgentComponent& agent)
            {
                return scene.GetRegistry().Get<TransformComponent>(e).GetWorldPosition() - glm::vec3(0.0f, agent.BaseOffset, 0.0f);
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

            // Finds the agent's mesh and the node it stands on (keeping it where it is when that is on the mesh).
            bool Place(Scene& scene, Entity e, NavMeshAgentComponent& agent)
            {
                glm::ivec2 cell;
                glm::vec3 ground = GroundOf(scene, e, agent);
                if (agent.Mesh && agent.Node >= 0 && agent.Node < agent.Mesh->NodeCount() && !agent.Mesh->IsBlocked(agent.Node))
                {
                    // Still in (or next to) its node's column? Otherwise it was moved: look again.
                    cell = CellOf(*agent.Mesh, { ground.x, ground.z });
                    int col = agent.Mesh->Columns[agent.Node];
                    if (std::abs(cell.x - col % agent.Mesh->Width) <= 1 && std::abs(cell.y - col / agent.Mesh->Width) <= 1) return true;
                }
                int node = -1;
                agent.Mesh = MeshNear(scene, ground, std::max(agent.Height, 2.0f), &node);
                agent.Node = agent.Mesh ? node : -1;
                if (agent.Node < 0) return false;
                glm::vec3 on = OnMesh(*agent.Mesh, agent.Node, { ground.x, ground.z });
                PlaceObject(scene, e, on + glm::vec3(0.0f, agent.BaseOffset, 0.0f));
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
                agent.PathStatus = PathFrom(*agent.Mesh, agent.Node, GroundOf(scene, e, agent), agent.Destination, agent.Path,
                                            LinksOn(scene, *agent.Mesh), &agent.PathLinks);
            }

            float Horizontal(const glm::vec3& a, const glm::vec3& b) { return glm::length(glm::vec2(a.x - b.x, a.z - b.z)); }

            float WrapAngle(float a)
            {
                a = std::fmod(a + glm::pi<float>(), glm::two_pi<float>());
                if (a < 0.0f) a += glm::two_pi<float>();
                return a - glm::pi<float>();
            }
        }

        int Nearest(const NavMeshData& m, const glm::vec3& p, float maxDistance)
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
                    if (d2 < bestD2 && !m.IsBlocked((int)a))
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

        bool Walk(const NavMeshData& m, int startNode, glm::vec2 from, glm::vec2 to, int* endNode, glm::vec2* reached)
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
                if (next < 0)
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
                               const std::vector<Link>& links, std::vector<float>* linkArcs)
        {
            return PathFrom(mesh, Nearest(mesh, from, std::max(mesh.AgentHeight, 2.0f)), from, to, corners, links, linkArcs);
        }

        std::vector<Link> LinksOn(Scene& scene, const NavMeshData& mesh)
        {
            std::vector<Link> out;
            for (auto [e, link, t] : scene.GetRegistry().ViewOf<OffMeshLinkComponent, TransformComponent>())
            {
                if (!link.Activated || !t.ActiveInHierarchy) continue;
                glm::vec3 a = glm::vec3(t.World * glm::vec4(link.Start, 1.0f)), b = glm::vec3(t.World * glm::vec4(link.End, 1.0f));
                // Each end joins the mesh within a meter (or the agent's radius past the edge it was baked back from).
                float reach = std::max(1.0f, mesh.AgentRadius + mesh.CellSize * 2.0f);
                int na = Nearest(mesh, a, reach), nb = Nearest(mesh, b, reach);
                if (na < 0 || nb < 0 || na == nb) continue;
                float cost = glm::distance(a, b) * std::max(link.CostModifier, 0.0f);
                out.push_back({ na, nb, a, b, cost, std::max(link.ArcHeight, 0.0f) });
                if (link.Bidirectional) out.push_back({ nb, na, b, a, cost, std::max(link.ArcHeight, 0.0f) });
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
            BuildIslands(*m);
            return m;
        }

        std::shared_ptr<const NavMeshData> Bake(Scene& scene, Entity e)
        {
            auto& reg = scene.GetRegistry();
            auto* surface = scene.IsValid(e) ? reg.TryGet<NavMeshSurfaceComponent>(e) : nullptr;
            if (!surface) return nullptr;
            scene.UpdateTransforms();
            glm::vec3 center = reg.Get<TransformComponent>(e).GetWorldPosition();
            std::shared_ptr<NavMeshData> data;
            if (scene.GetPhysics3D())
                data = BakeIn(scene, *surface, center);
            else
            {
                // Editing: the colliders in a physics world of a copy (the scene itself is left alone).
                Ref<Scene> copy = scene.Clone();
                copy->StartQueryPhysics();
                data = BakeIn(*copy, *surface, center);
            }
            surface->Data = surface->Baked = data;
            surface->CarveKey = 0;
            for (auto [a, agent] : reg.ViewOf<NavMeshAgentComponent>())
            {
                agent.Mesh.reset();
                agent.Node = -1;
                if (agent.HasDestination) agent.PathDirty = true;
            }
            return data;
        }

        void BakeAll(Scene& scene, bool onlyMissing)
        {
            std::vector<Entity> surfaces;
            for (auto [e, s] : scene.GetRegistry().ViewOf<NavMeshSurfaceComponent>())
                if (!onlyMissing || !s.Data) surfaces.push_back(e);
            for (Entity e : surfaces) Bake(scene, e);
        }

        std::shared_ptr<const NavMeshData> MeshNear(Scene& scene, const glm::vec3& point, float maxDistance, int* node)
        {
            std::shared_ptr<const NavMeshData> best;
            float bestD = kInf;
            int bestNode = -1;
            for (auto [e, s, t] : scene.GetRegistry().ViewOf<NavMeshSurfaceComponent, TransformComponent>())
            {
                if (!s.Data || !t.ActiveInHierarchy) continue;
                int a = Nearest(*s.Data, point, maxDistance);
                if (a < 0) continue;
                glm::vec3 on = OnMesh(*s.Data, a, { point.x, point.z });
                float d = glm::distance(on, point);
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
            result = OnMesh(*mesh, node, { point.x, point.z });
            return true;
        }

        NavPathStatus CalculatePath(Scene& scene, const glm::vec3& from, const glm::vec3& to, std::vector<glm::vec3>& corners)
        {
            int node = -1;
            corners.clear();
            auto mesh = MeshNear(scene, from, 2.0f, &node);
            return mesh ? PathFrom(*mesh, node, from, to, corners, LinksOn(scene, *mesh), nullptr) : NavPathStatus::Invalid;
        }

        bool SetDestination(Scene& scene, Entity e, const glm::vec3& destination)
        {
            NavMeshAgentComponent* agent = AgentOf(scene, e);
            if (!agent) return false;
            agent->Destination = destination;
            agent->HasDestination = true;
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
            agent->Velocity = glm::vec3(0.0f);
            PlaceObject(scene, e, OnMesh(*mesh, node, { position.x, position.z }) + glm::vec3(0.0f, agent->BaseOffset, 0.0f));
            if (agent->HasDestination) agent->PathDirty = true;
            return true;
        }

        void Update(Scene& scene, float dt)
        {
            if (dt <= 0.0f) return;
            auto& reg = scene.GetRegistry();

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
                if (!s.Baked || s.CarveKey == carveKey) continue;
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

            struct Mover
            {
                Entity E;
                NavMeshAgentComponent* Agent;
                glm::vec3 Ground;
            };
            std::vector<Mover> movers;
            for (auto [e, agent, t] : reg.ViewOf<NavMeshAgentComponent, TransformComponent>())
                if (t.ActiveInHierarchy) movers.push_back({ e, &agent, glm::vec3(0.0f) });

            for (Mover& mv : movers)
            {
                NavMeshAgentComponent& agent = *mv.Agent;
                agent.RepathTimer -= dt;
                // Following a target: a new path whenever it has moved on (a few times a second at most).
                if (!agent.Target.IsNull() && scene.IsValid(agent.Target) && agent.RepathTimer <= 0.0f)
                {
                    glm::vec3 goal = reg.Get<TransformComponent>(agent.Target).GetWorldPosition();
                    if (!agent.HasDestination || glm::distance(goal, agent.Destination) > 0.3f)
                    {
                        agent.Destination = goal;
                        agent.HasDestination = agent.PathDirty = true;
                        agent.RepathTimer = 0.25f;
                    }
                }
                if (agent.Traversing)
                {
                    mv.Ground = GroundOf(scene, mv.E, agent);
                    continue;
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
                    !Walk(*agent.Mesh, agent.Node, { mv.Ground.x, mv.Ground.z }, { agent.Path[agent.Corner].x, agent.Path[agent.Corner].z }))
                {
                    agent.PathDirty = true;
                    agent.RepathTimer = 0.2f;
                }
                if (agent.PathDirty) Repath(scene, mv.E, agent);
                mv.Ground = GroundOf(scene, mv.E, agent);
            }

            for (Mover& mv : movers)
            {
                NavMeshAgentComponent& agent = *mv.Agent;
                if (!agent.Mesh) continue;
                auto& transform = reg.Get<TransformComponent>(mv.E);
                // Face the way it goes.
                auto face = [&](glm::vec2 v2) {
                    if (glm::length(v2) <= 0.05f) return;
                    float want = std::atan2(-v2.x, -v2.y);
                    float turn = WrapAngle(want - transform.Rotation.y);
                    float maxTurn = glm::radians(agent.AngularSpeed) * dt;
                    transform.Rotation.y = WrapAngle(transform.Rotation.y + std::clamp(turn, -maxTurn, maxTurn));
                };
                if (agent.Traversing)
                {
                    // Crossing an off-mesh link: straight from end to end, Arc Height up in the middle.
                    agent.TraverseTime += dt;
                    float t = std::clamp(agent.TraverseTime / std::max(agent.TraverseDuration, 1e-3f), 0.0f, 1.0f);
                    mv.Ground = glm::mix(agent.TraverseFrom, agent.TraverseTo, t) + glm::vec3(0.0f, agent.TraverseArc * 4.0f * t * (1.0f - t), 0.0f);
                    glm::vec3 across = (agent.TraverseTo - agent.TraverseFrom) / std::max(agent.TraverseDuration, 1e-3f);
                    agent.Velocity = glm::vec3(across.x, 0.0f, across.z);
                    PlaceObject(scene, mv.E, mv.Ground + glm::vec3(0.0f, agent.BaseOffset, 0.0f));
                    face({ across.x, across.z });
                    if (t >= 1.0f)
                    {
                        agent.Traversing = false;
                        agent.Node = Nearest(*agent.Mesh, agent.TraverseTo, 1.0f);
                    }
                    continue;
                }
                if (agent.Node < 0) continue;
                const NavMeshData& mesh = *agent.Mesh;
                glm::vec2 p(mv.Ground.x, mv.Ground.z);

                // Where it wants to go: on to the next corner, slowing to arrive.
                glm::vec2 desired(0.0f);
                bool moving = agent.HasDestination && agent.Path.size() >= 2 && agent.PathStatus != NavPathStatus::Invalid;
                agent.RemainingDistance = 0.0f;
                float brakeDistance = kInf;
                if (moving)
                {
                    size_t last = agent.Path.size() - 1;
                    agent.Corner = std::clamp<size_t>(agent.Corner, 1, last);
                    while (agent.Corner < last && Horizontal(mv.Ground, agent.Path[agent.Corner]) < std::max(0.1f, glm::length(agent.Velocity) * dt * 1.5f))
                        ++agent.Corner;
                    float remaining = Horizontal(mv.Ground, agent.Path[agent.Corner]);
                    for (size_t k = agent.Corner; k < last; ++k) remaining += Horizontal(agent.Path[k], agent.Path[k + 1]);
                    agent.RemainingDistance = remaining;
                    brakeDistance = remaining - agent.StoppingDistance;
                    if (!agent.IsStopped && agent.Corner - 1 < agent.PathLinks.size() && agent.PathLinks[agent.Corner - 1] >= 0.0f)
                    {
                        // At an off-mesh link's start: across it, at walking speed.
                        agent.Traversing = true;
                        agent.TraverseFrom = mv.Ground;
                        agent.TraverseTo = agent.Path[agent.Corner];
                        agent.TraverseArc = agent.PathLinks[agent.Corner - 1];
                        agent.TraverseTime = 0.0f;
                        agent.TraverseDuration = std::max(glm::distance(agent.TraverseFrom, agent.TraverseTo) / std::max(agent.Speed, 0.1f), 0.25f);
                        agent.PathLinks[agent.Corner - 1] = -1.0f;
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

                // Avoidance: of a fan of velocities around the wanted one, the one nearest it that does not run into
                // another agent soon (agents with a higher priority number are left to give way themselves).
                glm::vec2 chosen = desired;
                if (agent.ObstacleAvoidance)
                {
                    struct Other
                    {
                        glm::vec2 P, V;
                        float R;
                    };
                    std::vector<Other> others;
                    float reach = agent.Radius + agent.Speed * 2.0f;
                    for (const Mover& o : movers)
                    {
                        if (o.E == mv.E || o.Agent->AvoidancePriority > agent.AvoidancePriority || !o.Agent->Mesh) continue;
                        glm::vec2 op(o.Ground.x, o.Ground.z);
                        glm::vec2 ov(o.Agent->Velocity.x, o.Agent->Velocity.z);
                        if (std::abs(o.Ground.y - mv.Ground.y) > agent.Height) continue;
                        if (glm::distance(op, p) > reach + o.Agent->Radius + glm::length(ov) * 2.0f) continue;
                        others.push_back({ op, ov, agent.Radius + o.Agent->Radius });
                    }
                    for (const Blocker& b : blockers)
                        if (std::abs(b.Y - mv.Ground.y - agent.Height * 0.5f) < b.HalfHeight + agent.Height * 0.5f &&
                            glm::distance(b.P, p) < reach + b.R + glm::length(b.V) * 2.0f)
                            others.push_back({ b.P, b.V, agent.Radius + b.R });
                    if (!others.empty())
                    {
                        const float horizon = 2.0f;
                        auto cost = [&](glm::vec2 v) {
                            float c = glm::length(v - desired) / std::max(agent.Speed, 0.1f);
                            for (const Other& o : others)
                            {
                                glm::vec2 rp = o.P - p, rv = v - o.V;
                                float c2 = glm::dot(rp, rp) - o.R * o.R;
                                float b = glm::dot(rp, rv);
                                if (c2 < 0.0f)
                                {
                                    if (b > 0.0f) c += 3.0f + b; // overlapping and closing in
                                    continue;
                                }
                                float a = glm::dot(rv, rv);
                                float disc = b * b - a * c2;
                                if (b <= 0.0f || a < 1e-8f || disc <= 0.0f) continue;
                                float t = (b - std::sqrt(disc)) / a;
                                if (t < horizon) c += 2.5f * (horizon - t) / horizon;
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

                // Accelerate toward it, then move on the mesh (sliding along its edges).
                glm::vec2 vel(agent.Velocity.x, agent.Velocity.z);
                glm::vec2 change = chosen - vel;
                float maxChange = agent.Acceleration > 0.0f ? agent.Acceleration * dt : kInf;
                if (glm::length(change) > maxChange) change = glm::normalize(change) * maxChange;
                vel += change;
                glm::vec2 step = vel * dt;
                if (moving && agent.Corner + 1 == agent.Path.size() && glm::length(step) > brakeDistance && brakeDistance > 0.0f)
                    step = glm::normalize(step) * brakeDistance; // no overshooting the end
                glm::vec2 target = p + step, reached = p;
                int node = agent.Node;
                if (glm::length(step) > 1e-7f && !Walk(mesh, agent.Node, p, target, &node, &reached))
                {
                    int along = node;
                    glm::vec2 slid;
                    if (Walk(mesh, agent.Node, p, { target.x, p.y }, &along, &slid) || Walk(mesh, agent.Node, p, { p.x, target.y }, &along, &slid))
                    {
                        node = along;
                        reached = slid;
                    }
                }
                else if (glm::length(step) > 1e-7f)
                    reached = target;
                agent.Node = node;
                glm::vec2 moved = (reached - p) / dt;
                agent.Velocity = glm::vec3(moved.x, 0.0f, moved.y);
                mv.Ground = glm::vec3(reached.x, HeightAt(mesh, node, reached.x, reached.y), reached.y);
                PlaceObject(scene, mv.E, mv.Ground + glm::vec3(0.0f, agent.BaseOffset, 0.0f));
                face({ agent.Velocity.x, agent.Velocity.z });
            }
        }
    }
}
