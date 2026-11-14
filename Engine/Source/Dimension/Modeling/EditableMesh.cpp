#include "Dimension/Modeling/EditableMesh.h"
#include "Dimension/Core/Log.h"
#include <algorithm>
#include <cstring>
#include <cmath>
#include <fstream>
#include <numeric>
#include <unordered_set>

namespace Dimension::Modeling
{
    using json = nlohmann::json;

    uint32_t EditableMesh::AddVertex(const glm::vec3& p, uint8_t flags)
    {
        Positions.push_back(p);
        VertFlags.push_back(flags);
        return (uint32_t)Positions.size() - 1;
    }

    uint32_t EditableMesh::AddFace(std::vector<uint32_t> verts, std::vector<glm::vec2> uvs, int32_t material, bool smooth)
    {
        Face f;
        uvs.resize(verts.size(), glm::vec2(0.0f));
        f.Verts = std::move(verts);
        f.UVs = std::move(uvs);
        f.Material = material;
        f.Smooth = smooth;
        Faces.push_back(std::move(f));
        Changed();
        return (uint32_t)Faces.size() - 1;
    }

    void EditableMesh::AddLooseEdge(uint32_t a, uint32_t b)
    {
        if (a == b) return;
        EdgeKey k = MakeEdgeKey(a, b);
        if (Topo().EdgeIndex.contains(k)) return;
        LooseEdges.push_back(k);
        Changed();
    }

    uint32_t EditableMesh::Append(const EditableMesh& other)
    {
        uint32_t base = VertexCount();
        Positions.insert(Positions.end(), other.Positions.begin(), other.Positions.end());
        VertFlags.insert(VertFlags.end(), other.VertFlags.begin(), other.VertFlags.end());
        // The other mesh's slots map onto this one's by name.
        std::vector<int32_t> slotMap(std::max<size_t>(1, other.MaterialSlots.size()), 0);
        for (size_t i = 0; i < other.MaterialSlots.size(); ++i)
        {
            auto it = std::find(MaterialSlots.begin(), MaterialSlots.end(), other.MaterialSlots[i]);
            if (it == MaterialSlots.end())
            {
                MaterialSlots.push_back(other.MaterialSlots[i]);
                it = MaterialSlots.end() - 1;
            }
            slotMap[i] = (int32_t)(it - MaterialSlots.begin());
        }
        for (Face f : other.Faces)
        {
            for (uint32_t& v : f.Verts) v += base;
            f.Material = f.Material >= 0 && f.Material < (int32_t)slotMap.size() ? slotMap[f.Material] : 0;
            Faces.push_back(std::move(f));
        }
        for (EdgeKey k : other.LooseEdges) LooseEdges.push_back(MakeEdgeKey(EdgeA(k) + base, EdgeB(k) + base));
        for (auto& [k, d] : other.EdgeAttrs) EdgeAttrs[MakeEdgeKey(EdgeA(k) + base, EdgeB(k) + base)] = d;
        Changed();
        return base;
    }

    const Topology& EditableMesh::Topo() const
    {
        if (m_Topology) return *m_Topology;
        auto t = std::make_shared<Topology>();
        t->VertFaces.resize(Positions.size());
        t->VertEdges.resize(Positions.size());
        t->FaceEdges.resize(Faces.size());
        auto edge = [&](uint32_t a, uint32_t b) -> uint32_t {
            EdgeKey k = MakeEdgeKey(a, b);
            auto [it, inserted] = t->EdgeIndex.try_emplace(k, (uint32_t)t->Edges.size());
            if (inserted)
            {
                t->Edges.push_back(k);
                t->EdgeFaces.emplace_back();
                t->VertEdges[a].push_back(it->second);
                t->VertEdges[b].push_back(it->second);
            }
            return it->second;
        };
        for (uint32_t f = 0; f < Faces.size(); ++f)
        {
            const auto& verts = Faces[f].Verts;
            size_t n = verts.size();
            t->FaceEdges[f].resize(n);
            for (size_t i = 0; i < n; ++i)
            {
                uint32_t a = verts[i], b = verts[(i + 1) % n];
                auto& vf = t->VertFaces[a];
                if (vf.empty() || vf.back() != f) vf.push_back(f);
                if (a == b) continue;
                uint32_t e = edge(a, b);
                t->FaceEdges[f][i] = e;
                auto& ef = t->EdgeFaces[e];
                if (ef.empty() || ef.back() != f) ef.push_back(f);
            }
        }
        for (EdgeKey k : LooseEdges)
            if (EdgeA(k) < Positions.size() && EdgeB(k) < Positions.size() && EdgeA(k) != EdgeB(k)) edge(EdgeA(k), EdgeB(k));
        m_Topology = t;
        return *t;
    }

    EdgeData EditableMesh::GetEdge(EdgeKey k) const
    {
        auto it = EdgeAttrs.find(k);
        return it == EdgeAttrs.end() ? EdgeData{} : it->second;
    }

    void EditableMesh::SetEdgeFlag(EdgeKey k, ElementFlag flag, bool on)
    {
        if (!on)
        {
            auto it = EdgeAttrs.find(k);
            if (it == EdgeAttrs.end()) return;
            it->second.Flags &= ~flag;
            if (it->second.IsDefault()) EdgeAttrs.erase(it);
            return;
        }
        EdgeAttrs[k].Flags |= flag;
    }

    void EditableMesh::PruneEdgeAttrs()
    {
        const Topology& t = Topo();
        for (auto it = EdgeAttrs.begin(); it != EdgeAttrs.end();)
            if (it->second.IsDefault() || !t.EdgeIndex.contains(it->first)) it = EdgeAttrs.erase(it);
            else ++it;
    }

    glm::vec3 EditableMesh::FaceNormal(uint32_t f) const
    {
        const auto& v = Faces[f].Verts;
        glm::vec3 n(0.0f);
        for (size_t i = 0, c = v.size(); i < c; ++i)
        {
            const glm::vec3& a = Positions[v[i]];
            const glm::vec3& b = Positions[v[(i + 1) % c]];
            n += glm::vec3((a.y - b.y) * (a.z + b.z), (a.z - b.z) * (a.x + b.x), (a.x - b.x) * (a.y + b.y));
        }
        float len = glm::length(n);
        return len > 1e-20f ? n / len : glm::vec3(0.0f);
    }

    glm::vec3 EditableMesh::FaceCenter(uint32_t f) const
    {
        const auto& v = Faces[f].Verts;
        glm::vec3 c(0.0f);
        for (uint32_t i : v) c += Positions[i];
        return v.empty() ? c : c / (float)v.size();
    }

    float EditableMesh::FaceArea(uint32_t f) const
    {
        const auto& v = Faces[f].Verts;
        glm::vec3 n(0.0f);
        for (size_t i = 0, c = v.size(); i < c; ++i) n += glm::cross(Positions[v[i]], Positions[v[(i + 1) % c]]);
        return glm::length(n) * 0.5f;
    }

    AABB EditableMesh::Bounds() const
    {
        AABB b;
        if (Positions.empty()) return b;
        b.Min = b.Max = Positions[0];
        for (const auto& p : Positions)
        {
            b.Min = glm::min(b.Min, p);
            b.Max = glm::max(b.Max, p);
        }
        return b;
    }

    namespace
    {
        float Cross2(const glm::vec2& a, const glm::vec2& b, const glm::vec2& c) { return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x); }

        bool InTriangle(const glm::vec2& p, const glm::vec2& a, const glm::vec2& b, const glm::vec2& c)
        {
            float d1 = Cross2(a, b, p), d2 = Cross2(b, c, p), d3 = Cross2(c, a, p);
            return d1 >= 0.0f && d2 >= 0.0f && d3 >= 0.0f;
        }
    }

    void EditableMesh::TriangulateFace(uint32_t f, std::vector<uint32_t>& out) const
    {
        const auto& verts = Faces[f].Verts;
        uint32_t n = (uint32_t)verts.size();
        if (n < 3) return;
        if (n == 3)
        {
            out.insert(out.end(), { 0u, 1u, 2u });
            return;
        }
        // The polygon in its own plane (counter-clockwise there).
        glm::vec3 normal = FaceNormal(f);
        if (normal == glm::vec3(0.0f))
        {
            for (uint32_t i = 1; i + 1 < n; ++i) out.insert(out.end(), { 0u, i, i + 1 }); // degenerate: a fan
            return;
        }
        glm::vec3 u = std::abs(normal.x) < 0.9f ? glm::normalize(glm::cross(normal, glm::vec3(1, 0, 0))) : glm::normalize(glm::cross(normal, glm::vec3(0, 1, 0)));
        glm::vec3 w = glm::cross(normal, u);
        std::vector<glm::vec2> p(n);
        for (uint32_t i = 0; i < n; ++i) p[i] = { glm::dot(Positions[verts[i]], u), glm::dot(Positions[verts[i]], w) };
        // (u, w, normal) is right-handed, so counter-clockwise around the normal is counter-clockwise in (u, w).

        if (n == 4)
        {
            // Split along the shorter diagonal when both halves are proper triangles (convex quads), else where it works.
            bool okA = Cross2(p[0], p[1], p[2]) > 0.0f && Cross2(p[0], p[2], p[3]) > 0.0f; // diagonal 0-2
            bool okB = Cross2(p[1], p[2], p[3]) > 0.0f && Cross2(p[1], p[3], p[0]) > 0.0f; // diagonal 1-3
            float dA = glm::distance(Positions[verts[0]], Positions[verts[2]]), dB = glm::distance(Positions[verts[1]], Positions[verts[3]]);
            if (okB && (!okA || dB < dA * 0.9999f)) out.insert(out.end(), { 1u, 2u, 3u, 1u, 3u, 0u });
            else out.insert(out.end(), { 0u, 1u, 2u, 0u, 2u, 3u });
            return;
        }

        std::vector<uint32_t> ring(n);
        std::iota(ring.begin(), ring.end(), 0u);
        while (ring.size() > 3)
        {
            size_t m = ring.size();
            bool clipped = false;
            for (size_t i = 0; i < m && !clipped; ++i)
            {
                uint32_t a = ring[(i + m - 1) % m], b = ring[i], c = ring[(i + 1) % m];
                if (Cross2(p[a], p[b], p[c]) <= 1e-12f) continue; // reflex or flat corner: not an ear
                bool empty = true;
                for (size_t k = 0; k < m && empty; ++k)
                {
                    uint32_t q = ring[k];
                    if (q == a || q == b || q == c) continue;
                    if (p[q] == p[a] || p[q] == p[b] || p[q] == p[c]) continue;
                    if (InTriangle(p[q], p[a], p[b], p[c])) empty = false;
                }
                if (!empty) continue;
                out.insert(out.end(), { a, b, c });
                ring.erase(ring.begin() + (ptrdiff_t)i);
                clipped = true;
            }
            if (!clipped)
            {
                // Self-intersecting or degenerate: cut the first corner anyway.
                out.insert(out.end(), { ring[m - 1], ring[0], ring[1] });
                ring.erase(ring.begin());
            }
        }
        out.insert(out.end(), { ring[0], ring[1], ring[2] });
    }

    namespace
    {
        float CornerAngle(const EditableMesh& m, uint32_t f, size_t corner)
        {
            const auto& v = m.Faces[f].Verts;
            size_t n = v.size();
            glm::vec3 p = m.Positions[v[corner]];
            glm::vec3 a = m.Positions[v[(corner + n - 1) % n]] - p, b = m.Positions[v[(corner + 1) % n]] - p;
            float la = glm::length(a), lb = glm::length(b);
            if (la < 1e-12f || lb < 1e-12f) return 0.0f;
            return std::acos(std::clamp(glm::dot(a, b) / (la * lb), -1.0f, 1.0f));
        }

        struct CornerKey
        {
            uint32_t Vert, NormalId, U, V;
            bool operator==(const CornerKey&) const = default;
        };
        struct CornerHash
        {
            size_t operator()(const CornerKey& k) const
            {
                uint64_t h = (uint64_t)k.Vert * 0x9E3779B97F4A7C15ull;
                h ^= (uint64_t)k.NormalId + 0x7F4A7C15ull + (h << 6) + (h >> 2);
                h ^= ((uint64_t)k.U << 32 | k.V) + 0x9E3779B9ull + (h << 6) + (h >> 2);
                return (size_t)h;
            }
        };
        uint32_t Bits(float f)
        {
            uint32_t u;
            std::memcpy(&u, &f, 4);
            return u;
        }
    }

    MeshData EditableMesh::ToMeshData() const
    {
        MeshData out;
        out.Name = Name;
        const Topology& t = Topo();
        size_t faceCount = Faces.size();
        std::vector<glm::vec3> fn(faceCount);
        for (uint32_t f = 0; f < faceCount; ++f) fn[f] = FaceNormal(f);

        // Which edges split smooth shading.
        float cosLimit = std::cos(glm::radians(std::clamp(AutoSmoothAngle, 0.0f, 180.0f)));
        std::vector<uint8_t> smoothEdge(t.Edges.size(), 0);
        for (size_t e = 0; e < t.Edges.size(); ++e)
        {
            const auto& ef = t.EdgeFaces[e];
            if (ef.size() < 2) continue;
            if (GetEdge(t.Edges[e]).Sharp) continue;
            bool ok = true;
            for (uint32_t f : ef) ok &= Faces[f].Smooth;
            if (ok && AutoSmooth)
                for (size_t i = 1; i < ef.size(); ++i) ok &= glm::dot(fn[ef[0]], fn[ef[i]]) >= cosLimit - 1e-6f;
            smoothEdge[e] = ok;
        }

        // Per vertex, its smooth faces in groups joined across smooth edges; a group's normal is its faces' normals
        // weighted by their corner angles. groupOf[(vertex, face)] -> normal index.
        std::vector<glm::vec3> normals;
        std::unordered_map<uint64_t, uint32_t> groupOf;
        std::vector<uint32_t> parent;
        for (uint32_t v = 0; v < Positions.size(); ++v)
        {
            const auto& vf = t.VertFaces[v];
            if (vf.empty()) continue;
            parent.resize(vf.size());
            std::iota(parent.begin(), parent.end(), 0u);
            auto find = [&](uint32_t i) {
                while (parent[i] != i) i = parent[i] = parent[parent[i]];
                return i;
            };
            auto local = [&](uint32_t f) -> int {
                for (size_t i = 0; i < vf.size(); ++i)
                    if (vf[i] == f) return (int)i;
                return -1;
            };
            for (uint32_t e : t.VertEdges[v])
            {
                if (!smoothEdge[e]) continue;
                const auto& ef = t.EdgeFaces[e];
                int a = local(ef[0]);
                for (size_t i = 1; i < ef.size(); ++i)
                {
                    int b = local(ef[i]);
                    if (a >= 0 && b >= 0) parent[find((uint32_t)a)] = find((uint32_t)b);
                }
            }
            std::unordered_map<uint32_t, uint32_t> rootNormal;
            for (size_t i = 0; i < vf.size(); ++i)
            {
                uint32_t f = vf[i];
                if (!Faces[f].Smooth) continue;
                uint32_t root = find((uint32_t)i);
                auto [it, inserted] = rootNormal.try_emplace(root, (uint32_t)normals.size());
                if (inserted) normals.push_back(glm::vec3(0.0f));
                const auto& verts = Faces[f].Verts;
                for (size_t c = 0; c < verts.size(); ++c)
                    if (verts[c] == v) normals[it->second] += fn[f] * CornerAngle(*this, f, c);
                groupOf[(uint64_t)v << 32 | f] = it->second;
            }
        }
        for (auto& n : normals)
        {
            float len = glm::length(n);
            n = len > 1e-20f ? n / len : glm::vec3(0, 1, 0);
        }
        uint32_t flatBase = (uint32_t)normals.size(); // flat faces: normal id flatBase + face

        // Corners -> render vertices (shared when vertex, normal and UV match), triangles grouped by material slot.
        size_t slots = std::max<size_t>(1, MaterialSlots.size());
        std::vector<std::vector<uint32_t>> bySlot(slots);
        std::unordered_map<CornerKey, uint32_t, CornerHash> corners;
        std::vector<uint32_t> tri, cornerVertex;
        for (uint32_t f = 0; f < faceCount; ++f)
        {
            const Face& face = Faces[f];
            size_t n = face.Verts.size();
            if (n < 3) continue;
            cornerVertex.resize(n);
            for (size_t c = 0; c < n; ++c)
            {
                uint32_t v = face.Verts[c];
                uint32_t nid = flatBase + f;
                glm::vec3 normal = fn[f];
                if (face.Smooth)
                    if (auto it = groupOf.find((uint64_t)v << 32 | f); it != groupOf.end())
                    {
                        nid = it->second;
                        normal = normals[nid];
                    }
                if (normal == glm::vec3(0.0f)) normal = glm::vec3(0, 1, 0);
                glm::vec2 uv = c < face.UVs.size() ? face.UVs[c] : glm::vec2(0.0f);
                CornerKey key{ v, nid, Bits(uv.x), Bits(uv.y) };
                auto [it, inserted] = corners.try_emplace(key, (uint32_t)out.Vertices.size());
                if (inserted) out.Vertices.push_back({ Positions[v], normal, uv, glm::vec4(0.0f) });
                cornerVertex[c] = it->second;
            }
            tri.clear();
            TriangulateFace(f, tri);
            size_t slot = std::clamp<int32_t>(face.Material, 0, (int32_t)slots - 1);
            for (uint32_t c : tri) bySlot[slot].push_back(cornerVertex[c]);
        }
        for (size_t s = 0; s < slots; ++s)
        {
            if (bySlot[s].empty() && slots > 1) continue;
            SubMesh sm;
            sm.IndexOffset = (uint32_t)out.Indices.size();
            sm.IndexCount = (uint32_t)bySlot[s].size();
            sm.MaterialSlot = MaterialSlots.empty() ? -1 : (int32_t)s;
            out.Indices.insert(out.Indices.end(), bySlot[s].begin(), bySlot[s].end());
            out.SubMeshes.push_back(sm);
        }
        for (const auto& name : MaterialSlots)
        {
            ImportedMaterial m;
            m.Name = name;
            out.Materials.push_back(m);
        }
        out.GenerateTangents();
        return out;
    }

    EditableMesh EditableMesh::FromMeshData(const MeshData& data, bool joinQuads, float weldDistance)
    {
        EditableMesh m;
        m.Name = data.Name;
        // Weld corners at the same position.
        std::vector<uint32_t> remap(data.Vertices.size());
        {
            struct Cell
            {
                int64_t X, Y, Z;
                bool operator==(const Cell&) const = default;
            };
            struct CellHash
            {
                size_t operator()(const Cell& c) const { return (size_t)(c.X * 73856093ll ^ c.Y * 19349663ll ^ c.Z * 83492791ll); }
            };
            std::unordered_map<Cell, uint32_t, CellHash> cells;
            float inv = 1.0f / std::max(weldDistance, 1e-9f);
            for (size_t i = 0; i < data.Vertices.size(); ++i)
            {
                glm::vec3 p = data.Vertices[i].Position;
                Cell c{ (int64_t)std::llround(p.x * inv), (int64_t)std::llround(p.y * inv), (int64_t)std::llround(p.z * inv) };
                auto [it, inserted] = cells.try_emplace(c, m.VertexCount());
                if (inserted) m.AddVertex(p);
                remap[i] = it->second;
            }
        }
        for (const auto& mat : data.Materials) m.MaterialSlots.push_back(mat.Name.empty() ? "Material" : mat.Name);

        // Triangles -> faces.
        struct Tri
        {
            uint32_t Corner[3]; // render vertex indices
            int32_t Material;
        };
        std::vector<Tri> tris;
        auto addRange = [&](uint32_t offset, uint32_t count, int32_t material) {
            for (size_t i = offset; i + 3 <= (size_t)offset + count && i + 3 <= data.Indices.size(); i += 3)
            {
                Tri t{ { data.Indices[i], data.Indices[i + 1], data.Indices[i + 2] }, material };
                uint32_t a = remap[t.Corner[0]], b = remap[t.Corner[1]], c = remap[t.Corner[2]];
                if (a == b || b == c || a == c) continue; // degenerate after welding
                tris.push_back(t);
            }
        };
        if (data.SubMeshes.empty()) addRange(0, (uint32_t)data.Indices.size(), 0);
        else
            for (const auto& sm : data.SubMeshes) addRange(sm.IndexOffset, sm.IndexCount, std::max(0, sm.MaterialSlot));

        std::vector<int32_t> joined(tris.size(), -1);
        auto triNormal = [&](const Tri& t) {
            glm::vec3 a = m.Positions[remap[t.Corner[0]]], b = m.Positions[remap[t.Corner[1]]], c = m.Positions[remap[t.Corner[2]]];
            glm::vec3 n = glm::cross(b - a, c - a);
            float len = glm::length(n);
            return len > 1e-20f ? n / len : glm::vec3(0.0f);
        };
        auto isSmooth = [&](const Tri& t) {
            glm::vec3 n = triNormal(t);
            for (uint32_t c : t.Corner)
                if (glm::dot(glm::normalize(data.Vertices[c].Normal + glm::vec3(1e-30f)), n) < 0.9995f) return true;
            return false;
        };
        std::vector<Face> quads; // joined pairs
        std::vector<std::vector<glm::vec3>> quadNormals, faceNormals; // the file's normals per corner
        if (joinQuads)
        {
            // Candidate pairs across shared edges: coplanar, same material and shading, matching UVs along the edge,
            // convex. Best (most square) first.
            std::unordered_map<EdgeKey, std::vector<uint32_t>> edgeTris;
            for (uint32_t i = 0; i < tris.size(); ++i)
                for (int k = 0; k < 3; ++k) edgeTris[MakeEdgeKey(remap[tris[i].Corner[k]], remap[tris[i].Corner[(k + 1) % 3]])].push_back(i);
            struct Pair
            {
                uint32_t A, B;
                float Score;
            };
            std::vector<Pair> pairs;
            for (auto& [key, list] : edgeTris)
            {
                if (list.size() != 2) continue;
                const Tri &ta = tris[list[0]], &tb = tris[list[1]];
                if (ta.Material != tb.Material) continue;
                glm::vec3 na = triNormal(ta), nb = triNormal(tb);
                if (glm::dot(na, nb) < 0.9998f) continue;
                if (isSmooth(ta) != isSmooth(tb)) continue;
                // UVs must agree at the shared vertices.
                bool uvOk = true;
                for (int i = 0; i < 3; ++i)
                    for (int j = 0; j < 3; ++j)
                        if (remap[ta.Corner[i]] == remap[tb.Corner[j]] && glm::distance(data.Vertices[ta.Corner[i]].UV, data.Vertices[tb.Corner[j]].UV) > 1e-5f) uvOk = false;
                if (!uvOk) continue;
                // Squareness: the shared edge should be the long diagonal.
                float diag = glm::distance(m.Positions[EdgeA(key)], m.Positions[EdgeB(key)]);
                float longest = 0.0f;
                for (const Tri* t : { &ta, &tb })
                    for (int k = 0; k < 3; ++k)
                        longest = std::max(longest, glm::distance(m.Positions[remap[t->Corner[k]]], m.Positions[remap[t->Corner[(k + 1) % 3]]]));
                pairs.push_back({ list[0], list[1], diag / std::max(longest, 1e-12f) });
            }
            std::sort(pairs.begin(), pairs.end(), [](const Pair& a, const Pair& b) { return a.Score > b.Score; });
            for (const Pair& p : pairs)
            {
                if (joined[p.A] >= 0 || joined[p.B] >= 0) continue;
                const Tri &ta = tris[p.A], &tb = tris[p.B];
                // ta rotated so its shared edge comes first (x -> y), tb's third vertex w: the quad is x, w, y, z.
                int sa = -1, wb = -1;
                for (int k = 0; k < 3 && sa < 0; ++k)
                {
                    uint32_t x = remap[ta.Corner[k]], y = remap[ta.Corner[(k + 1) % 3]];
                    for (int j = 0; j < 3; ++j)
                        if (remap[tb.Corner[j]] == y && remap[tb.Corner[(j + 1) % 3]] == x)
                        {
                            sa = k;
                            wb = (j + 2) % 3;
                        }
                }
                if (sa < 0) continue; // opposite windings: not joined
                uint32_t cx = ta.Corner[sa], cy = ta.Corner[(sa + 1) % 3], cz = ta.Corner[(sa + 2) % 3], cw = tb.Corner[wb];
                // Convex in its plane?
                glm::vec3 n = triNormal(ta);
                glm::vec3 q[4] = { m.Positions[remap[cx]], m.Positions[remap[cw]], m.Positions[remap[cy]], m.Positions[remap[cz]] };
                bool convex = true;
                for (int k = 0; k < 4; ++k)
                    if (glm::dot(glm::cross(q[(k + 1) % 4] - q[k], q[(k + 2) % 4] - q[(k + 1) % 4]), n) <= 1e-12f) convex = false;
                if (!convex) continue;
                Face f;
                quadNormals.emplace_back();
                for (uint32_t c : { cx, cw, cy, cz })
                {
                    f.Verts.push_back(remap[c]);
                    f.UVs.push_back(data.Vertices[c].UV);
                    quadNormals.back().push_back(data.Vertices[c].Normal);
                }
                f.Material = ta.Material;
                f.Smooth = isSmooth(ta);
                joined[p.A] = joined[p.B] = (int32_t)quads.size();
                quads.push_back(std::move(f));
            }
        }
        // Faces in the order of their first triangle.
        std::vector<uint8_t> emitted(quads.size(), 0);
        for (uint32_t i = 0; i < tris.size(); ++i)
        {
            if (joined[i] >= 0)
            {
                if (!emitted[joined[i]])
                {
                    m.Faces.push_back(quads[joined[i]]);
                    faceNormals.push_back(quadNormals[joined[i]]);
                }
                emitted[joined[i]] = 1;
                continue;
            }
            Face f;
            faceNormals.emplace_back();
            for (uint32_t c : tris[i].Corner)
            {
                f.Verts.push_back(remap[c]);
                f.UVs.push_back(data.Vertices[c].UV);
                faceNormals.back().push_back(data.Vertices[c].Normal);
            }
            f.Material = tris[i].Material;
            f.Smooth = isSmooth(tris[i]);
            m.Faces.push_back(std::move(f));
        }
        m.Changed();
        // Smooth surfaces with hard edges (the file's normals split along an edge between smooth faces) keep them as
        // sharp edges.
        const Topology& topo = m.Topo();
        auto cornerNormal = [&](uint32_t f, uint32_t v) {
            const auto& verts = m.Faces[f].Verts;
            for (size_t c = 0; c < verts.size(); ++c)
                if (verts[c] == v) return faceNormals[f][c];
            return glm::vec3(0.0f);
        };
        for (size_t e = 0; e < topo.Edges.size(); ++e)
        {
            const auto& ef = topo.EdgeFaces[e];
            if (ef.size() != 2 || !m.Faces[ef[0]].Smooth || !m.Faces[ef[1]].Smooth) continue;
            for (uint32_t v : { EdgeA(topo.Edges[e]), EdgeB(topo.Edges[e]) })
            {
                glm::vec3 a = cornerNormal(ef[0], v), b = cornerNormal(ef[1], v);
                if (glm::dot(a, b) < 0.999f * glm::length(a) * glm::length(b))
                {
                    m.EdgeAttrs[topo.Edges[e]].Sharp = true;
                    break;
                }
            }
        }
        return m;
    }

    // ---- .dmodel files

    namespace
    {
        double Round(float f) { return std::round((double)f * 1e6) / 1e6; }
    }

    json EditableMesh::ToJson() const
    {
        json j;
        j["format"] = "dmodel";
        j["version"] = 1;
        j["name"] = Name;
        json pos = json::array();
        for (const auto& p : Positions)
        {
            pos.push_back(Round(p.x));
            pos.push_back(Round(p.y));
            pos.push_back(Round(p.z));
        }
        j["positions"] = std::move(pos);
        json sizes = json::array(), verts = json::array(), uvs = json::array(), mats = json::array(), smooth = json::array();
        bool anyMaterial = false;
        for (uint32_t f = 0; f < Faces.size(); ++f)
        {
            const Face& face = Faces[f];
            sizes.push_back(face.Verts.size());
            for (size_t c = 0; c < face.Verts.size(); ++c)
            {
                verts.push_back(face.Verts[c]);
                glm::vec2 uv = c < face.UVs.size() ? face.UVs[c] : glm::vec2(0.0f);
                uvs.push_back(Round(uv.x));
                uvs.push_back(Round(uv.y));
            }
            mats.push_back(face.Material);
            anyMaterial |= face.Material != 0;
            if (face.Smooth) smooth.push_back(f);
        }
        j["faceSizes"] = std::move(sizes);
        j["faceVerts"] = std::move(verts);
        j["uvs"] = std::move(uvs);
        if (anyMaterial) j["faceMaterials"] = std::move(mats);
        if (!smooth.empty()) j["smoothFaces"] = std::move(smooth);
        if (!LooseEdges.empty())
        {
            json loose = json::array();
            for (EdgeKey k : LooseEdges)
            {
                loose.push_back(EdgeA(k));
                loose.push_back(EdgeB(k));
            }
            j["looseEdges"] = std::move(loose);
        }
        // Edge attributes: [a, b, sharp | seam << 1, crease].
        json edges = json::array();
        std::vector<std::pair<EdgeKey, EdgeData>> sorted(EdgeAttrs.begin(), EdgeAttrs.end());
        std::sort(sorted.begin(), sorted.end(), [](auto& a, auto& b) { return a.first < b.first; });
        for (auto& [k, d] : sorted)
            if (d.Sharp || d.Seam || d.Crease != 0.0f) edges.push_back({ EdgeA(k), EdgeB(k), (d.Sharp ? 1 : 0) | (d.Seam ? 2 : 0), Round(d.Crease) });
        if (!edges.empty()) j["edges"] = std::move(edges);
        if (!MaterialSlots.empty()) j["materials"] = MaterialSlots;
        if (AutoSmooth) j["autoSmooth"] = Round(AutoSmoothAngle);
        // Selection and hiding (kept like Blender keeps them in the file).
        auto flagged = [&](uint8_t flag) {
            json s = json::object();
            json v = json::array(), f = json::array(), e = json::array();
            for (uint32_t i = 0; i < VertFlags.size(); ++i)
                if (VertFlags[i] & flag) v.push_back(i);
            for (uint32_t i = 0; i < Faces.size(); ++i)
                if (Faces[i].Flags & flag) f.push_back(i);
            for (auto& [k, d] : sorted)
                if (d.Flags & flag)
                {
                    e.push_back(EdgeA(k));
                    e.push_back(EdgeB(k));
                }
            if (!v.empty()) s["verts"] = std::move(v);
            if (!e.empty()) s["edges"] = std::move(e);
            if (!f.empty()) s["faces"] = std::move(f);
            return s;
        };
        if (json s = flagged(Selected); !s.empty()) j["selected"] = std::move(s);
        if (json h = flagged(Hidden); !h.empty()) j["hidden"] = std::move(h);
        return j;
    }

    bool EditableMesh::FromJson(const json& j, EditableMesh& out)
    {
        out = EditableMesh();
        if (!j.is_object() || j.value("format", "") != "dmodel") return false;
        out.Name = j.value("name", "");
        const json& pos = j.value("positions", json::array());
        if (!pos.is_array() || pos.size() % 3) return false;
        for (size_t i = 0; i + 2 < pos.size(); i += 3) out.AddVertex({ pos[i].get<float>(), pos[i + 1].get<float>(), pos[i + 2].get<float>() });
        uint32_t vc = out.VertexCount();
        const json sizes = j.value("faceSizes", json::array()), verts = j.value("faceVerts", json::array()), uvs = j.value("uvs", json::array());
        const json mats = j.value("faceMaterials", json::array());
        size_t corner = 0;
        for (size_t f = 0; f < sizes.size(); ++f)
        {
            size_t n = sizes[f].get<size_t>();
            if (corner + n > verts.size()) return false;
            Face face;
            bool valid = n >= 3;
            for (size_t c = 0; c < n; ++c, ++corner)
            {
                uint32_t v = verts[corner].get<uint32_t>();
                valid &= v < vc;
                face.Verts.push_back(v);
                glm::vec2 uv(0.0f);
                if (2 * corner + 1 < uvs.size()) uv = { uvs[2 * corner].get<float>(), uvs[2 * corner + 1].get<float>() };
                face.UVs.push_back(uv);
            }
            face.Material = f < mats.size() ? mats[f].get<int32_t>() : 0;
            if (valid) out.Faces.push_back(std::move(face));
            else Log::Warn("Model '{}': face {} skipped (bad vertex indices)", out.Name, f);
        }
        for (const auto& s : j.value("smoothFaces", json::array()))
            if (s.get<size_t>() < out.Faces.size()) out.Faces[s.get<size_t>()].Smooth = true;
        const json loose = j.value("looseEdges", json::array());
        for (size_t i = 0; i + 1 < loose.size(); i += 2)
        {
            uint32_t a = loose[i].get<uint32_t>(), b = loose[i + 1].get<uint32_t>();
            if (a < vc && b < vc && a != b) out.LooseEdges.push_back(MakeEdgeKey(a, b));
        }
        for (const auto& e : j.value("edges", json::array()))
        {
            if (!e.is_array() || e.size() < 4) continue;
            uint32_t a = e[0].get<uint32_t>(), b = e[1].get<uint32_t>();
            if (a >= vc || b >= vc) continue;
            EdgeData& d = out.EdgeAttrs[MakeEdgeKey(a, b)];
            int bits = e[2].get<int>();
            d.Sharp = bits & 1;
            d.Seam = bits & 2;
            d.Crease = e[3].get<float>();
        }
        for (const auto& m : j.value("materials", json::array())) out.MaterialSlots.push_back(m.get<std::string>());
        if (j.contains("autoSmooth"))
        {
            out.AutoSmooth = true;
            out.AutoSmoothAngle = j["autoSmooth"].get<float>();
        }
        auto readFlags = [&](const char* key, uint8_t flag) {
            if (!j.contains(key)) return;
            const json& s = j[key];
            for (const auto& v : s.value("verts", json::array()))
                if (v.get<uint32_t>() < vc) out.VertFlags[v.get<uint32_t>()] |= flag;
            for (const auto& f : s.value("faces", json::array()))
                if (f.get<size_t>() < out.Faces.size()) out.Faces[f.get<size_t>()].Flags |= flag;
            const json e = s.value("edges", json::array());
            for (size_t i = 0; i + 1 < e.size(); i += 2)
            {
                uint32_t a = e[i].get<uint32_t>(), b = e[i + 1].get<uint32_t>();
                if (a < vc && b < vc) out.EdgeAttrs[MakeEdgeKey(a, b)].Flags |= flag;
            }
        };
        readFlags("selected", Selected);
        readFlags("hidden", Hidden);
        out.Changed();
        out.PruneEdgeAttrs();
        return true;
    }

    bool EditableMesh::Save(const fs::path& path) const
    {
        std::ofstream file(path, std::ios::binary);
        if (!file) return false;
        file << ToJson().dump();
        return (bool)file;
    }

    bool EditableMesh::Load(const fs::path& path, EditableMesh& out)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file) return false;
        json j = json::parse(file, nullptr, false);
        if (j.is_discarded() || !FromJson(j, out))
        {
            Log::Error("'{}' is not a valid model file", path.filename().string());
            return false;
        }
        if (out.Name.empty()) out.Name = path.stem().string();
        return true;
    }
}
