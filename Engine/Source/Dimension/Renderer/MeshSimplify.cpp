#include "Dimension/Renderer/MeshSimplify.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <queue>
#include <unordered_map>

namespace Dimension
{
    namespace MeshSimplify
    {
        namespace
        {
            // A symmetric 4x4 quadric (sum of squared distances to planes), upper triangle.
            struct Quadric
            {
                double A[10] = {};
                void AddPlane(const glm::dvec3& n, double d, double w)
                {
                    A[0] += w * n.x * n.x; A[1] += w * n.x * n.y; A[2] += w * n.x * n.z; A[3] += w * n.x * d;
                    A[4] += w * n.y * n.y; A[5] += w * n.y * n.z; A[6] += w * n.y * d;
                    A[7] += w * n.z * n.z; A[8] += w * n.z * d;
                    A[9] += w * d * d;
                }
                Quadric& operator+=(const Quadric& o)
                {
                    for (int i = 0; i < 10; ++i) A[i] += o.A[i];
                    return *this;
                }
                double Error(const glm::dvec3& p) const
                {
                    return A[0] * p.x * p.x + 2 * A[1] * p.x * p.y + 2 * A[2] * p.x * p.z + 2 * A[3] * p.x + A[4] * p.y * p.y + 2 * A[5] * p.y * p.z +
                           2 * A[6] * p.y + A[7] * p.z * p.z + 2 * A[8] * p.z + A[9];
                }
            };

            struct Tri
            {
                uint32_t V[3]; // vertices (attributes)
                uint32_t P[3]; // welded positions
                uint32_t Sub;
                bool Dead = false;
                bool Has(uint32_t p) const { return P[0] == p || P[1] == p || P[2] == p; }
            };

            struct Candidate
            {
                double Cost;
                uint32_t From, To;
                uint32_t FromVersion, ToVersion;
                bool operator>(const Candidate& o) const { return Cost > o.Cost; }
            };

            struct PositionKey
            {
                int32_t Bits[3];
                bool operator==(const PositionKey& o) const { return Bits[0] == o.Bits[0] && Bits[1] == o.Bits[1] && Bits[2] == o.Bits[2]; }
            };
            struct PositionKeyHash
            {
                size_t operator()(const PositionKey& k) const
                {
                    return ((size_t)(uint32_t)k.Bits[0] * 73856093u) ^ ((size_t)(uint32_t)k.Bits[1] * 19349663u) ^ ((size_t)(uint32_t)k.Bits[2] * 83492791u);
                }
            };
        }

        size_t TriangleCount(const MeshData& data)
        {
            size_t n = 0;
            for (const SubMesh& s : data.SubMeshes) n += s.IndexCount / 3;
            return data.SubMeshes.empty() ? data.Indices.size() / 3 : n;
        }

        MeshData Simplify(const MeshData& source, float ratio, float maxError)
        {
            MeshData out = source;
            std::vector<SubMesh> subs = source.SubMeshes;
            if (subs.empty()) subs.push_back({ 0, (uint32_t)source.Indices.size(), -1 });
            if (ratio >= 1.0f || source.Vertices.empty()) return out;

            // Corners at the same place (to a millionth of the mesh's size) share a position: UV seams split vertices
            // but not the surface.
            const auto& verts = source.Vertices;
            glm::vec3 vlo(1e30f), vhi(-1e30f);
            for (const Vertex& v : verts)
            {
                vlo = glm::min(vlo, v.Position);
                vhi = glm::max(vhi, v.Position);
            }
            float weld = std::max(glm::length(vhi - vlo), 1e-12f) * 1e-6f;
            std::unordered_map<PositionKey, uint32_t, PositionKeyHash> welded;
            std::vector<uint32_t> posOf(verts.size());
            std::vector<glm::dvec3> pos;
            std::vector<std::vector<uint32_t>> vertsAt;
            for (size_t i = 0; i < verts.size(); ++i)
            {
                PositionKey key;
                for (int k = 0; k < 3; ++k) key.Bits[k] = (int32_t)std::lround(verts[i].Position[k] / weld);
                auto [it, added] = welded.emplace(key, (uint32_t)pos.size());
                if (added)
                {
                    pos.push_back(glm::dvec3(verts[i].Position));
                    vertsAt.emplace_back();
                }
                posOf[i] = it->second;
                vertsAt[it->second].push_back((uint32_t)i);
            }
            size_t P = pos.size();
            glm::dvec3 lo(1e300), hi(-1e300);
            for (const auto& p : pos)
            {
                lo = glm::min(lo, p);
                hi = glm::max(hi, p);
            }
            double diag = std::max(glm::length(hi - lo), 1e-9);

            std::vector<Tri> tris;
            for (uint32_t s = 0; s < subs.size(); ++s)
                for (uint32_t i = subs[s].IndexOffset; i + 2 < subs[s].IndexOffset + subs[s].IndexCount && i + 2 < source.Indices.size(); i += 3)
                {
                    Tri t;
                    for (int k = 0; k < 3; ++k)
                    {
                        t.V[k] = source.Indices[i + k];
                        t.P[k] = posOf[t.V[k]];
                    }
                    t.Sub = s;
                    t.Dead = t.P[0] == t.P[1] || t.P[1] == t.P[2] || t.P[0] == t.P[2];
                    tris.push_back(t);
                }
            std::vector<std::vector<uint32_t>> trisAt(P);
            size_t alive = 0;
            for (uint32_t t = 0; t < tris.size(); ++t)
            {
                if (tris[t].Dead) continue;
                ++alive;
                for (int k = 0; k < 3; ++k) trisAt[tris[t].P[k]].push_back(t);
            }

            // The planes of the triangles around each position, and planes across borders and seams (perpendicular
            // to the surface, through the edge) so those edges keep their line.
            std::vector<Quadric> quadric(P);
            std::vector<uint8_t> locked(P, 0);
            struct EdgeUse
            {
                int Count = 0;
                uint32_t T0 = 0, T1 = 0;
            };
            std::unordered_map<uint64_t, EdgeUse> edges;
            auto edgeKey = [](uint32_t a, uint32_t b) { return a < b ? ((uint64_t)a << 32) | b : ((uint64_t)b << 32) | a; };
            auto faceNormal = [&](const Tri& t) { return glm::cross(pos[t.P[1]] - pos[t.P[0]], pos[t.P[2]] - pos[t.P[0]]); };
            for (uint32_t ti = 0; ti < tris.size(); ++ti)
            {
                const Tri& t = tris[ti];
                if (t.Dead) continue;
                glm::dvec3 n = faceNormal(t);
                double len = glm::length(n);
                if (len > 1e-30)
                {
                    n /= len;
                    for (int k = 0; k < 3; ++k) quadric[t.P[k]].AddPlane(n, -glm::dot(n, pos[t.P[0]]), 1.0);
                }
                for (int k = 0; k < 3; ++k)
                {
                    EdgeUse& e = edges[edgeKey(t.P[k], t.P[(k + 1) % 3])];
                    (e.Count == 0 ? e.T0 : e.T1) = ti;
                    ++e.Count;
                }
            }
            for (auto& [key, use] : edges)
            {
                uint32_t a = (uint32_t)(key >> 32), b = (uint32_t)(key & 0xFFFFFFFFu);
                if (use.Count > 2)
                {
                    locked[a] = locked[b] = 1; // non-manifold: left alone
                    continue;
                }
                double weight = 0.0;
                if (use.Count == 1)
                    weight = 100.0; // an open border
                else
                {
                    const Tri &t0 = tris[use.T0], &t1 = tris[use.T1];
                    auto vertexAt = [](const Tri& t, uint32_t p) {
                        for (int k = 0; k < 3; ++k)
                            if (t.P[k] == p) return t.V[k];
                        return 0u;
                    };
                    if (t0.Sub != t1.Sub)
                        weight = 100.0; // between materials
                    else if (vertexAt(t0, a) != vertexAt(t1, a) || vertexAt(t0, b) != vertexAt(t1, b))
                        weight = 10.0;  // a UV or normal seam
                }
                if (weight == 0.0) continue;
                glm::dvec3 n = faceNormal(tris[use.T0]);
                glm::dvec3 dir = pos[b] - pos[a];
                glm::dvec3 side = glm::cross(dir, n);
                double len = glm::length(side);
                if (len < 1e-30) continue;
                side /= len;
                double d = -glm::dot(side, pos[a]);
                quadric[a].AddPlane(side, d, weight);
                quadric[b].AddPlane(side, d, weight);
            }

            std::vector<uint32_t> version(P, 0);
            std::vector<uint8_t> removed(P, 0);
            std::priority_queue<Candidate, std::vector<Candidate>, std::greater<>> heap;
            auto push = [&](uint32_t from, uint32_t to) {
                if (locked[from] || removed[from] || removed[to]) return;
                Quadric q = quadric[from];
                q += quadric[to];
                heap.push({ q.Error(pos[to]), from, to, version[from], version[to] });
            };
            for (auto& [key, use] : edges)
            {
                uint32_t a = (uint32_t)(key >> 32), b = (uint32_t)(key & 0xFFFFFFFFu);
                push(a, b);
                push(b, a);
            }

            auto neighbours = [&](uint32_t p, std::vector<uint32_t>& outList) {
                outList.clear();
                for (uint32_t ti : trisAt[p])
                {
                    const Tri& t = tris[ti];
                    if (t.Dead) continue;
                    for (int k = 0; k < 3; ++k)
                        if (t.P[k] != p && std::find(outList.begin(), outList.end(), t.P[k]) == outList.end()) outList.push_back(t.P[k]);
                }
            };
            // The vertex at `p` whose attributes are nearest vertex v's (where a corner lands when it moves there).
            auto vertexNear = [&](uint32_t p, uint32_t v) {
                uint32_t best = vertsAt[p][0];
                float bestD = std::numeric_limits<float>::max();
                for (uint32_t c : vertsAt[p])
                {
                    glm::vec2 du = verts[c].UV - verts[v].UV;
                    glm::vec3 dn = verts[c].Normal - verts[v].Normal;
                    float d = glm::dot(du, du) + 0.1f * glm::dot(dn, dn);
                    if (d < bestD)
                    {
                        bestD = d;
                        best = c;
                    }
                }
                return best;
            };

            size_t target = std::max<size_t>((size_t)std::ceil(alive * std::max(ratio, 0.0f)), 1);
            double limit = (double)maxError * diag;
            limit *= limit;
            std::vector<uint32_t> na, nb;
            while (alive > target && !heap.empty())
            {
                Candidate c = heap.top();
                heap.pop();
                uint32_t a = c.From, b = c.To;
                if (removed[a] || removed[b] || version[a] != c.FromVersion || version[b] != c.ToVersion) continue;
                if (c.Cost > limit) break;
                // Still an edge, and collapsing it keeps the surface a surface: the two ends share exactly the
                // neighbours of the triangles on the edge.
                int shared = 0;
                for (uint32_t ti : trisAt[a])
                    if (!tris[ti].Dead && tris[ti].Has(b)) ++shared;
                if (shared == 0) continue;
                neighbours(a, na);
                neighbours(b, nb);
                int common = 0;
                for (uint32_t x : na)
                    if (std::find(nb.begin(), nb.end(), x) != nb.end()) ++common;
                if (common != shared) continue;
                // No triangle may turn over or collapse to nothing.
                bool ok = true;
                for (uint32_t ti : trisAt[a])
                {
                    const Tri& t = tris[ti];
                    if (t.Dead || t.Has(b)) continue;
                    glm::dvec3 p[3];
                    for (int k = 0; k < 3; ++k) p[k] = pos[t.P[k] == a ? b : t.P[k]];
                    glm::dvec3 before = faceNormal(t), after = glm::cross(p[1] - p[0], p[2] - p[0]);
                    double lb = glm::length(before), la = glm::length(after);
                    if (la < 1e-12 * diag * diag || (lb > 0.0 && glm::dot(before, after) / (lb * la) < 0.2))
                    {
                        ok = false;
                        break;
                    }
                }
                if (!ok) continue;

                for (uint32_t ti : trisAt[a])
                {
                    Tri& t = tris[ti];
                    if (t.Dead) continue;
                    if (t.Has(b))
                    {
                        t.Dead = true;
                        --alive;
                        continue;
                    }
                    for (int k = 0; k < 3; ++k)
                        if (t.P[k] == a)
                        {
                            t.P[k] = b;
                            t.V[k] = vertexNear(b, t.V[k]);
                        }
                    trisAt[b].push_back(ti);
                }
                removed[a] = 1;
                trisAt[a].clear();
                std::erase_if(trisAt[b], [&](uint32_t ti) { return tris[ti].Dead; });
                quadric[b] += quadric[a];
                ++version[b];
                neighbours(b, nb);
                for (uint32_t n : nb)
                {
                    push(b, n);
                    push(n, b);
                }
            }

            out.Indices.clear();
            out.SubMeshes = subs;
            for (uint32_t s = 0; s < subs.size(); ++s)
            {
                out.SubMeshes[s].IndexOffset = (uint32_t)out.Indices.size();
                for (const Tri& t : tris)
                    if (!t.Dead && t.Sub == s) out.Indices.insert(out.Indices.end(), { t.V[0], t.V[1], t.V[2] });
                out.SubMeshes[s].IndexCount = (uint32_t)out.Indices.size() - out.SubMeshes[s].IndexOffset;
            }
            return out;
        }
    }
}
