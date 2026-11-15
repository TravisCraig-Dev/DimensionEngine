#include "Dimension/Modeling/MeshTools.h"
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/constants.hpp>
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <numeric>
#include <tuple>
#include <unordered_set>

namespace Dimension::Modeling::Ops
{
    namespace
    {
        struct Corner
        {
            uint32_t V;
            glm::vec2 UV;
        };

        bool Hid(const EditableMesh& m, uint32_t f) { return (m.Faces[f].Flags & Hidden) != 0; }
        bool FaceSel(const EditableMesh& m, uint32_t f) { return (m.Faces[f].Flags & (Selected | Hidden)) == Selected; }
        glm::vec2 CornerUV(const Face& f, size_t c) { return c < f.UVs.size() ? f.UVs[c] : glm::vec2(0.0f); }
        int CornerOf(const Face& f, uint32_t v)
        {
            for (size_t i = 0; i < f.Verts.size(); ++i)
                if (f.Verts[i] == v) return (int)i;
            return -1;
        }
        // Whether face f walks from a straight to b.
        bool Walks(const Face& f, uint32_t a, uint32_t b)
        {
            for (size_t i = 0, n = f.Verts.size(); i < n; ++i)
                if (f.Verts[i] == a && f.Verts[(i + 1) % n] == b) return true;
            return false;
        }
        bool EdgeUsable(const EditableMesh& m, EdgeKey k) { return !m.IsVertHidden(EdgeA(k)) && !m.IsVertHidden(EdgeB(k)) && !m.IsEdgeHidden(k); }

        struct Groups
        {
            std::vector<uint32_t> Parent;
            explicit Groups(size_t n) : Parent(n) { std::iota(Parent.begin(), Parent.end(), 0u); }
            uint32_t Find(uint32_t i)
            {
                while (Parent[i] != i) i = Parent[i] = Parent[Parent[i]];
                return i;
            }
            void Join(uint32_t a, uint32_t b)
            {
                a = Find(a);
                b = Find(b);
                if (a != b) Parent[std::max(a, b)] = std::min(a, b);
            }
        };

        uint32_t AddFaceFrom(EditableMesh& m, const std::vector<Corner>& cs, const Face* like)
        {
            std::vector<uint32_t> v;
            std::vector<glm::vec2> uv;
            for (const Corner& c : cs)
            {
                v.push_back(c.V);
                uv.push_back(c.UV);
            }
            return m.AddFace(std::move(v), std::move(uv), like ? like->Material : 0, like ? like->Smooth : false);
        }

        // Newell normal of a loop of positions (not normalized).
        glm::vec3 LoopNormal(const EditableMesh& m, const std::vector<uint32_t>& loop)
        {
            glm::vec3 n(0.0f);
            for (size_t i = 0, c = loop.size(); i < c; ++i)
            {
                const glm::vec3& a = m.Positions[loop[i]];
                const glm::vec3& b = m.Positions[loop[(i + 1) % c]];
                n += glm::vec3((a.y - b.y) * (a.z + b.z), (a.z - b.z) * (a.x + b.x), (a.x - b.x) * (a.y + b.y));
            }
            return n;
        }

        // UVs for a new face with no source: its corners projected onto its plane, one unit per world unit.
        std::vector<glm::vec2> PlanarUVs(const EditableMesh& m, const std::vector<uint32_t>& loop)
        {
            std::vector<glm::vec2> uvs(loop.size(), glm::vec2(0.0f));
            glm::vec3 n = LoopNormal(m, loop);
            if (glm::length(n) < 1e-12f || loop.size() < 2) return uvs;
            n = glm::normalize(n);
            glm::vec3 u = m.Positions[loop[1]] - m.Positions[loop[0]];
            u -= n * glm::dot(u, n);
            if (glm::length(u) < 1e-12f) return uvs;
            u = glm::normalize(u);
            glm::vec3 w = glm::cross(n, u);
            for (size_t i = 0; i < loop.size(); ++i)
            {
                glm::vec3 d = m.Positions[loop[i]] - m.Positions[loop[0]];
                uvs[i] = { glm::dot(d, u), glm::dot(d, w) };
            }
            return uvs;
        }

        // Solves p ~ a * e1 + b * e2 (least squares in their plane).
        glm::vec2 PlaneCoords(const glm::vec3& e1, const glm::vec3& e2, const glm::vec3& p)
        {
            float a11 = glm::dot(e1, e1), a12 = glm::dot(e1, e2), a22 = glm::dot(e2, e2);
            float b1 = glm::dot(p, e1), b2 = glm::dot(p, e2);
            float det = a11 * a22 - a12 * a12;
            if (std::abs(det) < 1e-20f) return { a11 > 0.0f ? b1 / a11 : 0.0f, 0.0f };
            return { (b1 * a22 - b2 * a12) / det, (a11 * b2 - a12 * b1) / det };
        }

        void RemoveLooseCoveredByFaces(EditableMesh& m)
        {
            if (m.LooseEdges.empty()) return;
            std::unordered_set<EdgeKey> faceEdges;
            for (const Face& f : m.Faces)
                for (size_t i = 0, n = f.Verts.size(); i < n; ++i) faceEdges.insert(MakeEdgeKey(f.Verts[i], f.Verts[(i + 1) % n]));
            std::unordered_set<EdgeKey> seen;
            std::erase_if(m.LooseEdges, [&](EdgeKey k) { return faceEdges.contains(k) || !seen.insert(k).second; });
            m.Changed();
        }

        // Faces around a vertex in order: each corner's in-edge (from the previous corner) is the out-edge (to the next
        // corner) of the corner before it in the fan.
        struct FanCorner
        {
            uint32_t F, C;
            int32_t In, Out; // edge indices
        };
        bool BuildFan(const EditableMesh& m, const Topology& t, uint32_t v, std::vector<FanCorner>& fan, bool& closed)
        {
            fan.clear();
            closed = false;
            const auto& faces = t.VertFaces[v];
            if (faces.empty()) return false;
            for (uint32_t e : t.VertEdges[v])
                if (t.EdgeFaces[e].size() > 2) return false;
            std::vector<FanCorner> corners;
            for (uint32_t f : faces)
            {
                const Face& face = m.Faces[f];
                if (std::count(face.Verts.begin(), face.Verts.end(), v) != 1) return false;
                uint32_t c = (uint32_t)CornerOf(face, v), n = (uint32_t)face.Verts.size();
                corners.push_back({ f, c, t.FindEdge(face.Verts[(c + n - 1) % n], v), t.FindEdge(v, face.Verts[(c + 1) % n]) });
            }
            // Start at a corner whose in-edge is on the border (an open fan), else anywhere.
            size_t start = 0;
            for (size_t i = 0; i < corners.size(); ++i)
                if (t.EdgeFaces[corners[i].In].size() == 1)
                {
                    start = i;
                    break;
                }
            std::vector<uint8_t> used(corners.size(), 0);
            size_t cur = start;
            for (;;)
            {
                fan.push_back(corners[cur]);
                used[cur] = 1;
                int64_t next = -1;
                for (size_t i = 0; i < corners.size(); ++i)
                    if (corners[i].In == corners[cur].Out && corners[i].F != corners[cur].F)
                    {
                        next = (int64_t)i;
                        break;
                    }
                if (next < 0) break;
                if ((size_t)next == start)
                {
                    closed = true;
                    break;
                }
                if (used[(size_t)next]) return false;
                cur = (size_t)next;
            }
            return fan.size() == corners.size();
        }

        // Merges each group of two or more faces into one face when its outline is a single simple loop. Returns the
        // new faces' indices (after the rebuild).
        std::vector<uint32_t> MergeFaceGroups(EditableMesh& m, const std::vector<std::vector<uint32_t>>& groups)
        {
            std::vector<uint8_t> drop(m.FaceCount(), 0);
            std::vector<Face> made;
            for (const auto& group : groups)
            {
                if (group.size() < 2) continue;
                std::unordered_set<uint64_t> halves;
                for (uint32_t f : group)
                {
                    const auto& v = m.Faces[f].Verts;
                    for (size_t i = 0; i < v.size(); ++i) halves.insert(((uint64_t)v[i] << 32) | v[(i + 1) % v.size()]);
                }
                std::unordered_map<uint32_t, std::pair<uint32_t, glm::vec2>> next;
                bool simple = true;
                size_t count = 0;
                for (uint32_t f : group)
                {
                    const Face& face = m.Faces[f];
                    for (size_t i = 0, n = face.Verts.size(); i < n && simple; ++i)
                    {
                        uint32_t a = face.Verts[i], b = face.Verts[(i + 1) % n];
                        if (halves.contains(((uint64_t)b << 32) | a)) continue; // inside the group
                        if (next.contains(a)) simple = false;
                        next[a] = { b, CornerUV(face, i) };
                        ++count;
                    }
                }
                if (!simple || count < 3) continue;
                Face out = m.Faces[group[0]];
                out.Verts.clear();
                out.UVs.clear();
                uint32_t startV = next.begin()->first, cur = startV;
                for (size_t guard = 0; guard <= count; ++guard)
                {
                    auto it = next.find(cur);
                    if (it == next.end()) break;
                    out.Verts.push_back(cur);
                    out.UVs.push_back(it->second.second);
                    cur = it->second.first;
                    if (cur == startV) break;
                }
                if (cur != startV || out.Verts.size() != count) continue; // holes: more than one outline
                for (uint32_t f : group) drop[f] = 1;
                made.push_back(std::move(out));
            }
            std::vector<Face> faces;
            for (uint32_t f = 0; f < m.FaceCount(); ++f)
                if (!drop[f]) faces.push_back(std::move(m.Faces[f]));
            std::vector<uint32_t> idx;
            for (Face& f : made)
            {
                idx.push_back((uint32_t)faces.size());
                faces.push_back(std::move(f));
            }
            m.Faces = std::move(faces);
            m.Changed();
            return idx;
        }

        // Takes candidate vertices that sit between exactly two edges (inside faces' outlines), when every face keeps
        // three corners, out of the faces. `maxBend` (radians, < 0: any) only takes those where the two edges run nearly straight.
        void RemoveTwoEdgeVertices(EditableMesh& m, const std::vector<uint8_t>& candidates, float maxBend = -1.0f)
        {
            const Topology& t = m.Topo();
            std::unordered_set<uint32_t> loose;
            for (EdgeKey k : m.LooseEdges)
            {
                loose.insert(EdgeA(k));
                loose.insert(EdgeB(k));
            }
            std::vector<uint8_t> remove(m.VertexCount(), 0);
            for (uint32_t v = 0; v < m.VertexCount() && v < candidates.size(); ++v)
            {
                if (!candidates[v] || t.VertEdges[v].size() != 2 || t.VertFaces[v].empty() || loose.contains(v)) continue;
                bool ok = true;
                for (uint32_t f : t.VertFaces[v]) ok &= !Hid(m, f);
                if (!ok) continue;
                if (maxBend >= 0.0f)
                {
                    glm::vec3 a = m.Positions[EdgeA(t.Edges[t.VertEdges[v][0]]) == v ? EdgeB(t.Edges[t.VertEdges[v][0]]) : EdgeA(t.Edges[t.VertEdges[v][0]])];
                    glm::vec3 b = m.Positions[EdgeA(t.Edges[t.VertEdges[v][1]]) == v ? EdgeB(t.Edges[t.VertEdges[v][1]]) : EdgeA(t.Edges[t.VertEdges[v][1]])];
                    glm::vec3 da = a - m.Positions[v], db = b - m.Positions[v];
                    if (glm::length(da) < 1e-12f || glm::length(db) < 1e-12f) continue;
                    float bend = glm::pi<float>() - std::acos(std::clamp(glm::dot(glm::normalize(da), glm::normalize(db)), -1.0f, 1.0f));
                    if (bend > maxBend) continue;
                }
                remove[v] = 1;
            }
            // Faces must keep three corners.
            for (auto& f : m.Faces)
            {
                size_t keep = 0;
                for (uint32_t v : f.Verts) keep += remove[v] ? 0 : 1;
                if (keep < 3)
                    for (uint32_t v : f.Verts) remove[v] = 0;
            }
            bool any = false;
            for (auto& f : m.Faces)
            {
                Face out = f;
                out.Verts.clear();
                out.UVs.clear();
                for (size_t c = 0; c < f.Verts.size(); ++c)
                    if (!remove[f.Verts[c]])
                    {
                        out.Verts.push_back(f.Verts[c]);
                        out.UVs.push_back(CornerUV(f, c));
                    }
                    else any = true;
                f = std::move(out);
            }
            if (any) m.Changed(); // (the callers remove the vertices nothing uses now: they are among their candidates)
        }

        // Splits a face between two of its corners (not neighbours). Returns the new face's index.
        uint32_t SplitFace(EditableMesh& m, uint32_t f, uint32_t ci, uint32_t cj)
        {
            Face& face = m.Faces[f];
            size_t n = face.Verts.size();
            Face a = face, b = face;
            a.Verts.clear();
            a.UVs.clear();
            b.Verts.clear();
            b.UVs.clear();
            for (size_t k = ci;; k = (k + 1) % n)
            {
                a.Verts.push_back(face.Verts[k]);
                a.UVs.push_back(CornerUV(face, k));
                if (k == cj) break;
            }
            for (size_t k = cj;; k = (k + 1) % n)
            {
                b.Verts.push_back(face.Verts[k]);
                b.UVs.push_back(CornerUV(face, k));
                if (k == ci) break;
            }
            m.Faces[f] = std::move(a);
            m.Faces.push_back(std::move(b));
            m.Changed();
            return m.FaceCount() - 1;
        }

        // Puts a new vertex on edge a-b at t (from a): every face using the edge gains a corner, a wire edge splits.
        uint32_t InsertOnEdge(EditableMesh& m, uint32_t a, uint32_t b, float t)
        {
            uint32_t nv = m.AddVertex(glm::mix(m.Positions[a], m.Positions[b], t));
            for (auto& f : m.Faces)
            {
                size_t n = f.Verts.size();
                for (size_t i = 0; i < n; ++i)
                {
                    uint32_t x = f.Verts[i], y = f.Verts[(i + 1) % n];
                    if (!((x == a && y == b) || (x == b && y == a))) continue;
                    glm::vec2 uv = glm::mix(CornerUV(f, i), CornerUV(f, (i + 1) % n), x == a ? t : 1.0f - t);
                    f.Verts.insert(f.Verts.begin() + (std::ptrdiff_t)i + 1, nv);
                    if (f.UVs.size() >= i + 1) f.UVs.insert(f.UVs.begin() + (std::ptrdiff_t)i + 1, uv);
                    break;
                }
            }
            EdgeKey k = MakeEdgeKey(a, b);
            if (auto it = std::find(m.LooseEdges.begin(), m.LooseEdges.end(), k); it != m.LooseEdges.end())
            {
                *it = MakeEdgeKey(a, nv);
                m.LooseEdges.push_back(MakeEdgeKey(nv, b));
            }
            if (auto it = m.EdgeAttrs.find(k); it != m.EdgeAttrs.end())
            {
                EdgeData d = it->second;
                m.EdgeAttrs.erase(it);
                m.EdgeAttrs[MakeEdgeKey(a, nv)] = d;
                m.EdgeAttrs[MakeEdgeKey(nv, b)] = d;
            }
            m.Changed();
            return nv;
        }

        // What an extrusion made.
        struct SideFace
        {
            uint32_t Face, Source, Corner; // the new side face, and the extruded face's corner it stands on
        };
        struct ExtrudeInfo
        {
            std::vector<uint32_t> Faces;     // the extruded faces (on the new vertices)
            std::vector<SideFace> Sides;
            std::vector<uint32_t> NewVerts;  // made for the faces' outlines
            std::unordered_map<uint32_t, uint32_t> OldOf; // new vertex -> the one it copies
            glm::vec3 Normal{ 0.0f };
        };

        ExtrudeInfo ExtrudeImpl(EditableMesh& m, bool individual, bool facesOnly)
        {
            ExtrudeInfo info;
            const Topology t = m.Topo();
            uint32_t nF = m.FaceCount();
            std::vector<int64_t> region(nF, -1);
            Groups g(nF);
            if (!individual)
                for (size_t e = 0; e < t.Edges.size(); ++e)
                    if (t.EdgeFaces[e].size() == 2 && FaceSel(m, t.EdgeFaces[e][0]) && FaceSel(m, t.EdgeFaces[e][1])) g.Join(t.EdgeFaces[e][0], t.EdgeFaces[e][1]);
            for (uint32_t f = 0; f < nF; ++f)
                if (FaceSel(m, f))
                {
                    region[f] = g.Find(f);
                    info.Faces.push_back(f);
                    info.Normal += m.FaceNormal(f);
                }
            auto interior = [&](uint32_t e, int64_t r) {
                const auto& ef = t.EdgeFaces[e];
                return ef.size() == 2 && region[ef[0]] == r && region[ef[1]] == r;
            };
            std::unordered_map<uint64_t, uint32_t> copies; // (vertex, region + 1) -> copy
            auto copyOf = [&](uint32_t v, int64_t r) {
                uint64_t key = ((uint64_t)v << 32) | (uint32_t)(r + 1);
                auto it = copies.find(key);
                if (it != copies.end()) return it->second;
                uint32_t nv = m.AddVertex(m.Positions[v]);
                copies[key] = nv;
                info.NewVerts.push_back(nv);
                info.OldOf[nv] = v;
                return nv;
            };
            // Region outlines: side faces, then the faces move onto the copies.
            std::vector<std::pair<uint32_t, uint32_t>> boundaryCorners;
            std::unordered_set<uint64_t> outline; // (vertex, region + 1) on an outline
            for (uint32_t f : info.Faces)
            {
                const auto& fe = t.FaceEdges[f];
                const auto& v = m.Faces[f].Verts;
                for (uint32_t i = 0; i < fe.size(); ++i)
                    if (!interior(fe[i], region[f]))
                    {
                        boundaryCorners.push_back({ f, i });
                        outline.insert(((uint64_t)v[i] << 32) | (uint32_t)(region[f] + 1));
                        outline.insert(((uint64_t)v[(i + 1) % v.size()] << 32) | (uint32_t)(region[f] + 1));
                    }
            }
            for (auto [f, i] : boundaryCorners)
            {
                Face src = m.Faces[f]; // (AddFace may move the array)
                size_t n = src.Verts.size();
                uint32_t a = src.Verts[i], b = src.Verts[(i + 1) % n];
                uint32_t na = copyOf(a, region[f]), nb = copyOf(b, region[f]);
                glm::vec2 ua = CornerUV(src, i), ub = CornerUV(src, (i + 1) % n);
                glm::vec2 d = ub - ua, perp(-d.y, d.x);
                uint32_t side = AddFaceFrom(m, { { a, ua }, { b, ub }, { nb, ub + perp }, { na, ua + perp } }, &src);
                info.Sides.push_back({ side, f, i });
            }
            for (uint32_t f : info.Faces)
                for (uint32_t& v : m.Faces[f].Verts)
                    if (outline.contains(((uint64_t)v << 32) | (uint32_t)(region[f] + 1))) v = copyOf(v, region[f]);

            std::vector<std::pair<uint32_t, uint32_t>> newEdges;
            std::vector<uint32_t> newLone;
            if (!facesOnly)
            {
                // Edges no selected face uses become quads.
                std::vector<uint8_t> inSelEdge(m.VertexCount(), 0);
                for (size_t e = 0; e < t.Edges.size(); ++e)
                {
                    EdgeKey k = t.Edges[e];
                    if (!m.IsEdgeSelected(k) || !EdgeUsable(m, k)) continue;
                    inSelEdge[EdgeA(k)] = inSelEdge[EdgeB(k)] = 1;
                    bool usedBySelected = false;
                    for (uint32_t f : t.EdgeFaces[e]) usedBySelected |= region[f] >= 0;
                    if (usedBySelected) continue;
                    uint32_t a = EdgeA(k), b = EdgeB(k);
                    uint32_t na = copyOf(a, -1), nb = copyOf(b, -1);
                    const Face* like = t.EdgeFaces[e].empty() ? nullptr : &m.Faces[t.EdgeFaces[e][0]];
                    Face likeCopy = like ? *like : Face{};
                    std::vector<Corner> cs;
                    if (like && Walks(*like, a, b)) cs = { { b, { 0, 0 } }, { a, { 1, 0 } }, { na, { 1, 1 } }, { nb, { 0, 1 } } };
                    else cs = { { a, { 0, 0 } }, { b, { 1, 0 } }, { nb, { 1, 1 } }, { na, { 0, 1 } } };
                    AddFaceFrom(m, cs, like ? &likeCopy : nullptr);
                    newEdges.push_back({ na, nb });
                }
                // Lone vertices become wire edges.
                for (uint32_t v = 0; v < (uint32_t)inSelEdge.size(); ++v)
                {
                    if (!m.IsVertSelected(v) || m.IsVertHidden(v) || inSelEdge[v]) continue;
                    bool inFace = false;
                    for (uint32_t f : t.VertFaces[v]) inFace |= region[f] >= 0;
                    if (inFace) continue;
                    uint32_t nv = copyOf(v, -1);
                    m.LooseEdges.push_back(MakeEdgeKey(v, nv));
                    newLone.push_back(nv);
                }
            }
            m.Changed();
            RemoveLooseCoveredByFaces(m);
            DeselectAll(m);
            SelectFacesFully(m, info.Faces);
            for (auto [a, b] : newEdges)
            {
                SelectVertex(m, a, true);
                SelectVertex(m, b, true);
                SelectEdge(m, MakeEdgeKey(a, b), true);
            }
            for (uint32_t v : newLone) SelectVertex(m, v, true);
            if (glm::length(info.Normal) > 1e-12f) info.Normal = glm::normalize(info.Normal);
            m.PruneEdgeAttrs();
            return info;
        }
    }

    // ---- Helpers

    void SelectFacesFully(EditableMesh& m, const std::vector<uint32_t>& faces)
    {
        for (uint32_t f : faces)
        {
            if (f >= m.FaceCount() || Hid(m, f)) continue;
            Face& face = m.Faces[f];
            face.Flags |= Selected;
            for (size_t i = 0, n = face.Verts.size(); i < n; ++i)
            {
                SelectVertex(m, face.Verts[i], true);
                m.SetEdgeFlag(MakeEdgeKey(face.Verts[i], face.Verts[(i + 1) % n]), Selected, true);
            }
        }
    }

    glm::vec2 FaceUVAt(const EditableMesh& m, uint32_t f, const glm::vec3& p)
    {
        const Face& face = m.Faces[f];
        size_t n = face.Verts.size();
        if (n == 0) return glm::vec2(0.0f);
        glm::vec3 p0 = m.Positions[face.Verts[0]];
        glm::vec2 uv0 = CornerUV(face, 0);
        if (n < 3) return uv0;
        // The two corners spanning the largest triangle with the first.
        size_t bi = 1, bj = 2;
        float best = -1.0f;
        for (size_t i = 1; i < n; ++i)
            for (size_t j = i + 1; j < n; ++j)
            {
                float area = glm::length(glm::cross(m.Positions[face.Verts[i]] - p0, m.Positions[face.Verts[j]] - p0));
                if (area > best)
                {
                    best = area;
                    bi = i;
                    bj = j;
                }
            }
        if (best < 1e-12f) return uv0;
        glm::vec2 c = PlaneCoords(m.Positions[face.Verts[bi]] - p0, m.Positions[face.Verts[bj]] - p0, p - p0);
        return uv0 + c.x * (CornerUV(face, bi) - uv0) + c.y * (CornerUV(face, bj) - uv0);
    }

    // ---- Extrude and duplicate

    glm::vec3 Extrude(EditableMesh& m, bool individual) { return ExtrudeImpl(m, individual, false).Normal; }

    void Duplicate(EditableMesh& m)
    {
        const Topology t = m.Topo();
        std::unordered_map<uint32_t, uint32_t> copy;
        auto copyOf = [&](uint32_t v) {
            auto it = copy.find(v);
            if (it != copy.end()) return it->second;
            uint32_t nv = m.AddVertex(m.Positions[v]);
            copy[v] = nv;
            return nv;
        };
        std::vector<uint32_t> faces;
        uint32_t nF = m.FaceCount();
        for (uint32_t f = 0; f < nF; ++f)
        {
            if (!FaceSel(m, f)) continue;
            Face src = m.Faces[f];
            std::vector<uint32_t> verts;
            for (uint32_t v : src.Verts) verts.push_back(copyOf(v));
            uint32_t nf = m.AddFace(std::move(verts), src.UVs, src.Material, src.Smooth);
            faces.push_back(nf);
            // Edge marks come along.
            for (size_t i = 0, n = src.Verts.size(); i < n; ++i)
            {
                EdgeData d = m.GetEdge(MakeEdgeKey(src.Verts[i], src.Verts[(i + 1) % n]));
                d.Flags = 0;
                if (!d.IsDefault()) m.EdgeAttrs[MakeEdgeKey(copyOf(src.Verts[i]), copyOf(src.Verts[(i + 1) % n]))] = d;
            }
        }
        std::vector<EdgeKey> edges;
        for (size_t e = 0; e < t.Edges.size(); ++e)
        {
            EdgeKey k = t.Edges[e];
            if (!m.IsEdgeSelected(k) || !EdgeUsable(m, k)) continue;
            bool covered = false;
            for (uint32_t f : t.EdgeFaces[e]) covered |= FaceSel(m, f);
            if (covered) continue;
            EdgeKey nk = MakeEdgeKey(copyOf(EdgeA(k)), copyOf(EdgeB(k)));
            m.LooseEdges.push_back(nk);
            edges.push_back(nk);
        }
        for (uint32_t v = 0; v < (uint32_t)t.VertEdges.size(); ++v)
            if (m.IsVertSelected(v) && !m.IsVertHidden(v)) copyOf(v);
        m.Changed();
        DeselectAll(m);
        SelectFacesFully(m, faces);
        for (EdgeKey k : edges) SelectEdge(m, k, true);
        for (auto [o, n] : copy) SelectVertex(m, n, true);
    }

    std::vector<glm::vec3> ShrinkFattenDirections(const EditableMesh& m)
    {
        std::vector<glm::vec3> sel(m.VertexCount(), glm::vec3(0.0f)), all(m.VertexCount(), glm::vec3(0.0f));
        for (uint32_t f = 0; f < m.FaceCount(); ++f)
        {
            if (Hid(m, f)) continue;
            glm::vec3 n = m.FaceNormal(f);
            for (uint32_t v : m.Faces[f].Verts)
            {
                all[v] += n;
                if (FaceSel(m, f)) sel[v] += n;
            }
        }
        std::vector<glm::vec3> out(m.VertexCount(), glm::vec3(0.0f));
        for (uint32_t v = 0; v < m.VertexCount(); ++v)
        {
            if (!m.IsVertSelected(v) || m.IsVertHidden(v)) continue;
            glm::vec3 d = glm::length(sel[v]) > 1e-12f ? sel[v] : all[v];
            if (glm::length(d) > 1e-12f) out[v] = glm::normalize(d);
        }
        return out;
    }

    // ---- Inset

    void Inset(EditableMesh& m, float thickness, float depth, bool individual)
    {
        bool any = false;
        for (uint32_t f = 0; f < m.FaceCount(); ++f) any |= FaceSel(m, f);
        if (!any) return;
        std::vector<glm::vec3> normals(m.FaceCount());
        for (uint32_t f = 0; f < m.FaceCount(); ++f) normals[f] = m.FaceNormal(f);
        ExtrudeInfo info = ExtrudeImpl(m, individual, true);

        // Each outline vertex moves inward from the outline edges beside it (mitred), and up by the depth.
        std::unordered_map<uint32_t, std::vector<glm::vec3>> inward;
        for (const SideFace& s : info.Sides)
        {
            const Face& face = m.Faces[s.Source];
            size_t n = face.Verts.size();
            uint32_t a = face.Verts[s.Corner], b = face.Verts[(s.Corner + 1) % n];
            glm::vec3 d = m.Positions[b] - m.Positions[a];
            glm::vec3 in = glm::cross(normals[s.Source], d);
            if (glm::length(in) < 1e-12f) continue;
            in = glm::normalize(in);
            inward[a].push_back(in);
            inward[b].push_back(in);
        }
        std::unordered_map<uint32_t, glm::vec3> up;
        for (uint32_t f : info.Faces)
            for (uint32_t v : m.Faces[f].Verts) up[v] += normals[f];
        std::unordered_map<uint32_t, glm::vec3> moved;
        for (auto& [v, n] : up)
        {
            glm::vec3 offset = glm::length(n) > 1e-12f ? glm::normalize(n) * depth : glm::vec3(0.0f);
            if (auto it = inward.find(v); it != inward.end() && !it->second.empty())
            {
                glm::vec3 sum(0.0f);
                for (const glm::vec3& d : it->second) sum += d;
                if (glm::length(sum) > 1e-12f)
                {
                    glm::vec3 avg = glm::normalize(sum);
                    float least = 1.0f;
                    for (const glm::vec3& d : it->second) least = std::min(least, glm::dot(avg, d));
                    offset += avg * (thickness / std::max(least, 0.2f));
                }
            }
            moved[v] = m.Positions[v] + offset;
        }
        // UVs follow the faces' own mapping (worked out before anything moves).
        std::vector<std::vector<glm::vec2>> faceUVs;
        for (uint32_t f : info.Faces)
        {
            const Face& face = m.Faces[f];
            std::vector<glm::vec2> uvs = face.UVs;
            for (size_t c = 0; c < face.Verts.size(); ++c)
                if (auto it = moved.find(face.Verts[c]); it != moved.end()) uvs[c] = FaceUVAt(m, f, it->second);
            faceUVs.push_back(std::move(uvs));
        }
        std::vector<std::pair<glm::vec2, glm::vec2>> sideUVs;
        for (const SideFace& s : info.Sides)
        {
            Face& side = m.Faces[s.Face];
            sideUVs.push_back({ FaceUVAt(m, s.Source, moved.contains(side.Verts[2]) ? moved[side.Verts[2]] : m.Positions[side.Verts[2]]),
                                FaceUVAt(m, s.Source, moved.contains(side.Verts[3]) ? moved[side.Verts[3]] : m.Positions[side.Verts[3]]) });
        }
        for (size_t i = 0; i < info.Faces.size(); ++i) m.Faces[info.Faces[i]].UVs = faceUVs[i];
        for (size_t i = 0; i < info.Sides.size(); ++i)
        {
            Face& side = m.Faces[info.Sides[i].Face];
            side.UVs[2] = sideUVs[i].first;
            side.UVs[3] = sideUVs[i].second;
        }
        for (auto& [v, p] : moved) m.Positions[v] = p;
        m.Changed();
    }

    // ---- Bevel

    void Bevel(EditableMesh& m, float width, uint32_t segments, bool vertexOnly)
    {
        if (width <= 0.0f) return;
        segments = std::max(1u, segments);
        const Topology t = m.Topo();
        uint32_t nV = m.VertexCount();
        std::vector<uint8_t> bev(t.Edges.size(), 0), affected(nV, 0);
        if (vertexOnly)
        {
            for (uint32_t v = 0; v < nV; ++v) affected[v] = m.IsVertSelected(v) && !m.IsVertHidden(v) && !t.VertFaces[v].empty();
        }
        else
            for (size_t e = 0; e < t.Edges.size(); ++e)
            {
                EdgeKey k = t.Edges[e];
                const auto& ef = t.EdgeFaces[e];
                if (m.IsEdgeSelected(k) && EdgeUsable(m, k) && ef.size() == 2 && !Hid(m, ef[0]) && !Hid(m, ef[1]))
                    bev[e] = affected[EdgeA(k)] = affected[EdgeB(k)] = 1;
            }
        // Vertices whose faces don't make one fan can't be bevelled: leave their edges.
        std::vector<std::vector<FanCorner>> fans(nV);
        std::vector<uint8_t> closedFan(nV, 0);
        for (bool again = true; again;)
        {
            again = false;
            for (uint32_t v = 0; v < nV; ++v)
            {
                if (!affected[v] || !fans[v].empty()) continue;
                bool closed = false;
                if (BuildFan(m, t, v, fans[v], closed))
                {
                    closedFan[v] = closed;
                    continue;
                }
                fans[v].clear();
                affected[v] = 0;
                for (uint32_t e : t.VertEdges[v])
                    if (bev[e])
                    {
                        bev[e] = 0;
                        again = true;
                    }
            }
            if (again && !vertexOnly)
            {
                std::fill(affected.begin(), affected.end(), 0);
                for (size_t e = 0; e < t.Edges.size(); ++e)
                    if (bev[e]) affected[EdgeA(t.Edges[e])] = affected[EdgeB(t.Edges[e])] = 1;
            }
        }
        bool anyAffected = false;
        for (uint32_t v = 0; v < nV; ++v) anyAffected |= affected[v] != 0;
        if (!anyAffected) return;

        std::vector<glm::vec2> uvHint; // per new vertex (index - nV)
        auto newVert = [&](const glm::vec3& p, const glm::vec2& uv) {
            uvHint.push_back(uv);
            return m.AddVertex(p);
        };
        auto hint = [&](uint32_t v, const glm::vec2& fallback) { return v >= nV ? uvHint[v - nV] : fallback; };
        auto other = [&](uint32_t e, uint32_t v) { EdgeKey k = t.Edges[e]; return EdgeA(k) == v ? EdgeB(k) : EdgeA(k); };

        std::map<uint64_t, std::vector<Corner>> cornerLists; // (face, corner) -> what replaces the corner
        struct Patch
        {
            uint32_t V;
            std::vector<uint32_t> Loop;
            uint32_t LikeFace;
        };
        std::vector<Patch> patches;
        std::map<std::pair<uint32_t, uint32_t>, std::vector<uint32_t>> profiles; // (from, to) -> the points between

        auto makeProfile = [&](uint32_t from, uint32_t to, uint32_t v) {
            if (segments < 2 || from == to || profiles.contains({ from, to }) || profiles.contains({ to, from })) return;
            std::vector<uint32_t> pts;
            for (uint32_t k = 1; k < segments; ++k)
            {
                float s = (float)k / (float)segments;
                glm::vec3 p = (1 - s) * (1 - s) * m.Positions[from] + 2 * s * (1 - s) * m.Positions[v] + s * s * m.Positions[to];
                glm::vec2 uv = glm::mix(hint(from, glm::vec2(0.0f)), hint(to, glm::vec2(0.0f)), s);
                pts.push_back(newVert(p, uv));
            }
            profiles[{ from, to }] = std::move(pts);
        };
        auto profile = [&](uint32_t from, uint32_t to) -> std::vector<uint32_t> {
            if (auto it = profiles.find({ from, to }); it != profiles.end()) return it->second;
            if (auto it = profiles.find({ to, from }); it != profiles.end()) return { it->second.rbegin(), it->second.rend() };
            return {};
        };

        for (uint32_t v = 0; v < nV; ++v)
        {
            if (!affected[v]) continue;
            const glm::vec3 pv = m.Positions[v];
            // Points that slide along the edges at v.
            std::unordered_map<uint32_t, std::pair<uint32_t, float>> slide; // edge -> (vertex, fraction from v)
            for (uint32_t e : t.VertEdges[v])
            {
                if (bev[e]) continue;
                float dist = 0.0f;
                int count = 0;
                uint32_t far = other(e, v);
                glm::vec3 de = m.Positions[far] - pv;
                float len = glm::length(de);
                if (len < 1e-12f) continue;
                de /= len;
                if (vertexOnly)
                {
                    dist = width;
                    count = 1;
                }
                else
                    for (const FanCorner& fc : fans[v])
                    {
                        int32_t b = fc.In == (int32_t)e ? fc.Out : fc.Out == (int32_t)e ? fc.In : -1;
                        if (b < 0 || !bev[b]) continue;
                        glm::vec3 db = m.Positions[other((uint32_t)b, v)] - pv;
                        if (glm::length(db) < 1e-12f) continue;
                        float s = glm::length(glm::cross(de, glm::normalize(db)));
                        dist += width / std::max(s, 0.05f);
                        ++count;
                    }
                if (!count) continue;
                dist /= (float)count;
                dist = std::min(dist, len * (affected[far] ? 0.49f : 0.95f));
                slide[e] = { newVert(pv + de * dist, glm::vec2(0.0f)), dist / len };
            }
            std::vector<uint32_t> sequence;
            for (const FanCorner& fc : fans[v])
            {
                const Face& face = m.Faces[fc.F];
                size_t n = face.Verts.size();
                uint32_t p = face.Verts[(fc.C + n - 1) % n], q = face.Verts[(fc.C + 1) % n];
                glm::vec2 uvV = CornerUV(face, fc.C), uvP = CornerUV(face, (fc.C + n - 1) % n), uvQ = CornerUV(face, (fc.C + 1) % n);
                std::vector<Corner> list;
                auto in = slide.find((uint32_t)fc.In), out = slide.find((uint32_t)fc.Out);
                if (in != slide.end())
                {
                    glm::vec2 uv = glm::mix(uvV, uvP, in->second.second);
                    list.push_back({ in->second.first, uv });
                    if (uvHint[in->second.first - nV] == glm::vec2(0.0f)) uvHint[in->second.first - nV] = uv;
                }
                if (!vertexOnly && bev[fc.In] && bev[fc.Out])
                {
                    // Mitred inside the face, `width` from both bevelled edges.
                    glm::vec3 n3 = m.FaceNormal(fc.F);
                    glm::vec3 l1 = glm::cross(n3, pv - m.Positions[p]), l2 = glm::cross(n3, m.Positions[q] - pv);
                    if (glm::length(l1) > 1e-12f) l1 = glm::normalize(l1);
                    if (glm::length(l2) > 1e-12f) l2 = glm::normalize(l2);
                    glm::vec3 off = width * (l1 + l2) / std::max(1.0f + glm::dot(l1, l2), 0.2f);
                    glm::vec2 c = PlaneCoords(m.Positions[p] - pv, m.Positions[q] - pv, off);
                    glm::vec2 uv = uvV + c.x * (uvP - uvV) + c.y * (uvQ - uvV);
                    list.push_back({ newVert(pv + off, uv), uv });
                }
                else if (!vertexOnly && !bev[fc.In] && !bev[fc.Out] && (in == slide.end() || out == slide.end())) list.push_back({ v, uvV });
                if (out != slide.end())
                {
                    glm::vec2 uv = glm::mix(uvV, uvQ, out->second.second);
                    list.push_back({ out->second.first, uv });
                    if (uvHint[out->second.first - nV] == glm::vec2(0.0f)) uvHint[out->second.first - nV] = uv;
                }
                if (list.empty()) list.push_back({ v, uvV }); // (nothing to cut here)
                for (const Corner& c : list)
                    if (sequence.empty() || sequence.back() != c.V) sequence.push_back(c.V);
                if (vertexOnly && list.size() == 2) makeProfile(list[0].V, list[1].V, v);
                cornerLists[((uint64_t)fc.F << 32) | fc.C] = std::move(list);
            }
            if (closedFan[v] && sequence.size() > 1 && sequence.front() == sequence.back()) sequence.pop_back();
            std::vector<uint32_t> distinct = sequence;
            std::sort(distinct.begin(), distinct.end());
            distinct.erase(std::unique(distinct.begin(), distinct.end()), distinct.end());
            if (distinct.size() >= 3 && distinct.size() == sequence.size() && (closedFan[v] || vertexOnly))
                patches.push_back({ v, sequence, fans[v][0].F });
        }

        // Where a bevelled edge's side meets the face beside it, at end v.
        auto boundary = [&](uint32_t e, uint32_t v, uint32_t f) -> uint32_t {
            for (const FanCorner& fc : fans[v])
                if (fc.F == f)
                {
                    const auto& list = cornerLists[((uint64_t)fc.F << 32) | fc.C];
                    if (fc.In == (int32_t)e) return list.front().V;
                    if (fc.Out == (int32_t)e) return list.back().V;
                }
            return v;
        };
        if (!vertexOnly)
            for (size_t e = 0; e < t.Edges.size(); ++e)
                if (bev[e])
                {
                    uint32_t a = EdgeA(t.Edges[e]), b = EdgeB(t.Edges[e]);
                    makeProfile(boundary((uint32_t)e, a, t.EdgeFaces[e][0]), boundary((uint32_t)e, a, t.EdgeFaces[e][1]), a);
                    makeProfile(boundary((uint32_t)e, b, t.EdgeFaces[e][0]), boundary((uint32_t)e, b, t.EdgeFaces[e][1]), b);
                }

        // The faces again, with their corners replaced (and rounded where a profile runs between two of them).
        std::vector<Face> faces;
        for (uint32_t f = 0; f < m.FaceCount(); ++f)
        {
            const Face& face = m.Faces[f];
            Face out = face;
            out.Verts.clear();
            out.UVs.clear();
            for (uint32_t c = 0; c < face.Verts.size(); ++c)
            {
                auto it = cornerLists.find(((uint64_t)f << 32) | c);
                if (it == cornerLists.end())
                {
                    out.Verts.push_back(face.Verts[c]);
                    out.UVs.push_back(CornerUV(face, c));
                    continue;
                }
                const auto& list = it->second;
                for (size_t i = 0; i < list.size(); ++i)
                {
                    out.Verts.push_back(list[i].V);
                    out.UVs.push_back(list[i].UV);
                    if (i + 1 < list.size())
                    {
                        auto pts = profile(list[i].V, list[i + 1].V);
                        for (size_t k = 0; k < pts.size(); ++k)
                        {
                            float s = (float)(k + 1) / (float)segments;
                            glm::vec2 uvV = CornerUV(face, c);
                            out.Verts.push_back(pts[k]);
                            out.UVs.push_back((1 - s) * (1 - s) * list[i].UV + 2 * s * (1 - s) * uvV + s * s * list[i + 1].UV);
                        }
                    }
                }
            }
            // Repeated corners (in a row) collapse.
            Face clean = out;
            clean.Verts.clear();
            clean.UVs.clear();
            for (size_t i = 0; i < out.Verts.size(); ++i)
                if (clean.Verts.empty() || clean.Verts.back() != out.Verts[i])
                {
                    clean.Verts.push_back(out.Verts[i]);
                    clean.UVs.push_back(out.UVs[i]);
                }
            while (clean.Verts.size() > 1 && clean.Verts.front() == clean.Verts.back())
            {
                clean.Verts.pop_back();
                clean.UVs.pop_back();
            }
            if (clean.Verts.size() >= 3) faces.push_back(std::move(clean));
        }
        std::vector<Face> original = std::move(m.Faces);
        m.Faces = std::move(faces);
        m.Changed();

        std::vector<uint32_t> made;
        auto addLoop = [&](std::vector<uint32_t> loop, const Face& like, const glm::vec3& outward) {
            std::vector<uint32_t> clean;
            for (uint32_t v : loop)
                if (clean.empty() || clean.back() != v) clean.push_back(v);
            while (clean.size() > 1 && clean.front() == clean.back()) clean.pop_back();
            if (clean.size() < 3) return;
            if (glm::length(outward) > 1e-12f && glm::dot(LoopNormal(m, clean), outward) < 0.0f) std::reverse(clean.begin(), clean.end());
            std::vector<glm::vec2> uvs;
            for (uint32_t v : clean) uvs.push_back(hint(v, glm::vec2(0.0f)));
            made.push_back(m.AddFace(std::move(clean), std::move(uvs), like.Material, like.Smooth));
        };

        // The bevel's own faces: strips along each edge.
        if (!vertexOnly)
            for (size_t e = 0; e < t.Edges.size(); ++e)
            {
                if (!bev[e]) continue;
                uint32_t a = EdgeA(t.Edges[e]), b = EdgeB(t.Edges[e]);
                uint32_t fL = t.EdgeFaces[e][0], fR = t.EdgeFaces[e][1];
                uint32_t u = a, w = b; // fL walks u -> w
                if (!Walks(original[fL], a, b)) std::swap(u, w);
                auto ends = [&](uint32_t v) {
                    uint32_t x = boundary((uint32_t)e, v, fL), y = boundary((uint32_t)e, v, fR);
                    std::vector<uint32_t> seq = { x };
                    for (uint32_t p : profile(x, y)) seq.push_back(p);
                    if (y != x) seq.push_back(y);
                    return seq;
                };
                std::vector<uint32_t> sw = ends(w), su = ends(u);
                const glm::vec3 outward(0.0f); // (the strips are wound to match the faces beside them)
                for (uint32_t k = 0; k < segments; ++k)
                {
                    auto at = [](const std::vector<uint32_t>& s, uint32_t i) { return s[std::min<size_t>(i, s.size() - 1)]; };
                    std::vector<uint32_t> quad = { at(sw, k), at(su, k), at(su, k + 1), at(sw, k + 1) };
                    addLoop(quad, original[fL], outward);
                }
            }
        // Corner patches.
        for (const Patch& p : patches)
        {
            std::vector<uint32_t> loop;
            for (size_t i = 0; i < p.Loop.size(); ++i)
            {
                loop.push_back(p.Loop[i]);
                if (i + 1 < p.Loop.size() || closedFan[p.V])
                    for (uint32_t x : profile(p.Loop[i], p.Loop[(i + 1) % p.Loop.size()])) loop.push_back(x);
            }
            glm::vec3 outward(0.0f);
            for (const FanCorner& fc : fans[p.V])
            {
                const Face& face = original[fc.F];
                glm::vec3 n(0.0f);
                for (size_t i = 0, c = face.Verts.size(); i < c; ++i)
                {
                    const glm::vec3& x = m.Positions[face.Verts[i]];
                    const glm::vec3& y = m.Positions[face.Verts[(i + 1) % c]];
                    n += glm::vec3((x.y - y.y) * (x.z + y.z), (x.z - y.z) * (x.x + y.x), (x.x - y.x) * (x.y + y.y));
                }
                outward += n;
            }
            if (segments > 1 && loop.size() > 4)
            {
                // A fan of triangles around a center pulled toward the old corner.
                glm::vec3 avg(0.0f);
                glm::vec2 uvAvg(0.0f);
                for (uint32_t v : loop)
                {
                    avg += m.Positions[v];
                    uvAvg += hint(v, glm::vec2(0.0f));
                }
                avg /= (float)loop.size();
                uvAvg /= (float)loop.size();
                uint32_t center = newVert(0.5f * avg + 0.5f * m.Positions[p.V], uvAvg);
                std::vector<uint32_t> oriented = loop;
                if (glm::dot(LoopNormal(m, oriented), outward) < 0.0f) std::reverse(oriented.begin(), oriented.end());
                for (size_t i = 0; i < oriented.size(); ++i) addLoop({ oriented[i], oriented[(i + 1) % oriented.size()], center }, original[p.LikeFace], glm::vec3(0.0f));
            }
            else addLoop(loop, original[p.LikeFace], outward);
        }
        // Old corners nothing uses any more go.
        RemoveLooseCoveredByFaces(m);
        DeselectAll(m);
        SelectFacesFully(m, made);
        std::vector<uint8_t> candidates(m.VertexCount(), 0);
        for (uint32_t v = 0; v < nV; ++v) candidates[v] = affected[v];
        RemoveUnusedVertices(m, &candidates);
        m.PruneEdgeAttrs();
    }

    // ---- Loop cut and edge slide

    bool LoopCut(EditableMesh& m, uint32_t edge, uint32_t cuts, float slide)
    {
        const Topology t = m.Topo();
        if (edge >= t.Edges.size() || cuts == 0) return false;
        // Walk the ring both ways, keeping each edge's direction matched across the quads.
        struct RingQuad
        {
            uint32_t F;
            uint32_t X1, Y1, X2, Y2; // two opposite edges, X1 matching X2
        };
        std::vector<RingQuad> quads;
        std::vector<std::pair<uint32_t, uint32_t>> ringEdges; // directed
        std::unordered_set<uint32_t> seenFaces;
        std::unordered_set<EdgeKey> seenEdges;
        EdgeKey k0 = t.Edges[edge];
        ringEdges.push_back({ EdgeA(k0), EdgeB(k0) });
        seenEdges.insert(k0);
        for (uint32_t startFace : t.EdgeFaces[edge])
        {
            uint32_t x = EdgeA(k0), y = EdgeB(k0), f = startFace;
            for (;;)
            {
                const Face& face = m.Faces[f];
                if (seenFaces.contains(f) || face.Verts.size() != 4 || Hid(m, f)) break;
                int cx = CornerOf(face, x), cy = CornerOf(face, y);
                if (cx < 0 || cy < 0) break;
                uint32_t nx, ny;
                if ((cx + 1) % 4 == cy) // the face walks x -> y
                {
                    nx = face.Verts[(cx + 3) % 4];
                    ny = face.Verts[(cx + 2) % 4];
                }
                else
                {
                    nx = face.Verts[(cy + 2) % 4];
                    ny = face.Verts[(cy + 3) % 4];
                }
                seenFaces.insert(f);
                quads.push_back({ f, x, y, nx, ny });
                EdgeKey nk = MakeEdgeKey(nx, ny);
                if (seenEdges.contains(nk)) break;
                seenEdges.insert(nk);
                ringEdges.push_back({ nx, ny });
                int32_t ne = t.FindEdge(nx, ny);
                if (ne < 0 || t.EdgeFaces[ne].size() != 2) break;
                f = t.EdgeFaces[ne][0] == f ? t.EdgeFaces[ne][1] : t.EdgeFaces[ne][0];
                x = nx;
                y = ny;
            }
        }
        if (quads.empty()) return false;
        std::vector<float> ts;
        float step = 1.0f / (float)(cuts + 1);
        for (uint32_t c = 0; c < cuts; ++c) ts.push_back(std::clamp(step * (float)(c + 1) + slide * step, 0.001f, 0.999f));
        // Points along each ring edge, from its X end.
        std::unordered_map<EdgeKey, std::pair<uint32_t, std::vector<uint32_t>>> points;
        for (auto [x, y] : ringEdges)
        {
            std::vector<uint32_t> pts;
            for (float s : ts) pts.push_back(m.AddVertex(glm::mix(m.Positions[x], m.Positions[y], s)));
            points[MakeEdgeKey(x, y)] = { x, std::move(pts) };
        }
        auto along = [&](uint32_t from, uint32_t to) -> std::vector<uint32_t> {
            auto it = points.find(MakeEdgeKey(from, to));
            if (it == points.end()) return {};
            if (it->second.first == from) return it->second.second;
            return { it->second.second.rbegin(), it->second.second.rend() };
        };
        std::unordered_set<uint32_t> ringFace;
        for (const RingQuad& q : quads) ringFace.insert(q.F);
        std::vector<Face> faces;
        std::vector<std::pair<uint32_t, uint32_t>> loopEdges;
        for (uint32_t f = 0; f < m.FaceCount(); ++f)
        {
            const Face& face = m.Faces[f];
            if (ringFace.contains(f)) continue;
            Face out = face;
            out.Verts.clear();
            out.UVs.clear();
            size_t n = face.Verts.size();
            for (size_t i = 0; i < n; ++i)
            {
                uint32_t a = face.Verts[i], b = face.Verts[(i + 1) % n];
                out.Verts.push_back(a);
                out.UVs.push_back(CornerUV(face, i));
                auto pts = along(a, b);
                for (size_t j = 0; j < pts.size(); ++j)
                {
                    glm::vec3 pa = m.Positions[a], pb = m.Positions[b];
                    float s = glm::length(pb - pa) > 1e-12f ? glm::length(m.Positions[pts[j]] - pa) / glm::length(pb - pa) : 0.5f;
                    out.Verts.push_back(pts[j]);
                    out.UVs.push_back(glm::mix(CornerUV(face, i), CornerUV(face, (i + 1) % n), s));
                }
            }
            faces.push_back(std::move(out));
        }
        std::vector<uint32_t> newFaces;
        for (const RingQuad& q : quads)
        {
            const Face& face = m.Faces[q.F];
            auto uvOf = [&](uint32_t v) { return CornerUV(face, (size_t)CornerOf(face, v)); };
            std::vector<uint32_t> l1 = { q.X1 }, l2 = { q.X2 };
            for (uint32_t p : along(q.X1, q.Y1)) l1.push_back(p);
            for (uint32_t p : along(q.X2, q.Y2)) l2.push_back(p);
            l1.push_back(q.Y1);
            l2.push_back(q.Y2);
            std::vector<float> fr = { 0.0f };
            for (float s : ts) fr.push_back(s);
            fr.push_back(1.0f);
            bool forward = Walks(face, q.X1, q.Y1);
            for (size_t k = 0; k + 1 < l1.size(); ++k)
            {
                Face strip = face;
                strip.Verts = { l1[k], l1[k + 1], l2[k + 1], l2[k] };
                strip.UVs = { glm::mix(uvOf(q.X1), uvOf(q.Y1), fr[k]), glm::mix(uvOf(q.X1), uvOf(q.Y1), fr[k + 1]),
                              glm::mix(uvOf(q.X2), uvOf(q.Y2), fr[k + 1]), glm::mix(uvOf(q.X2), uvOf(q.Y2), fr[k]) };
                if (!forward)
                {
                    std::reverse(strip.Verts.begin(), strip.Verts.end());
                    std::reverse(strip.UVs.begin(), strip.UVs.end());
                }
                newFaces.push_back((uint32_t)faces.size());
                faces.push_back(std::move(strip));
                if (k > 0) loopEdges.push_back({ l1[k], l2[k] });
            }
        }
        // Edge marks carry over to the pieces of cut edges.
        for (auto& [key, pts] : points)
            if (auto it = m.EdgeAttrs.find(key); it != m.EdgeAttrs.end())
            {
                EdgeData d = it->second;
                std::vector<uint32_t> chain = { pts.first };
                for (uint32_t p : pts.second) chain.push_back(p);
                chain.push_back(EdgeA(key) == pts.first ? EdgeB(key) : EdgeA(key));
                for (size_t i = 0; i + 1 < chain.size(); ++i) m.EdgeAttrs[MakeEdgeKey(chain[i], chain[i + 1])] = d;
            }
        m.Faces = std::move(faces);
        m.Changed();
        DeselectAll(m);
        for (auto [a, b] : loopEdges)
        {
            SelectVertex(m, a, true);
            SelectVertex(m, b, true);
            SelectEdge(m, MakeEdgeKey(a, b), true);
        }
        m.PruneEdgeAttrs();
        return true;
    }

    SlideRails EdgeSlideRails(const EditableMesh& m)
    {
        SlideRails rails;
        const Topology& t = m.Topo();
        std::vector<std::vector<uint32_t>> adj(m.VertexCount());
        for (EdgeKey k : t.Edges)
            if (m.IsEdgeSelected(k) && EdgeUsable(m, k))
            {
                adj[EdgeA(k)].push_back(EdgeB(k));
                adj[EdgeB(k)].push_back(EdgeA(k));
            }
        // Walk the selected edges in chains so each edge gets a direction that agrees with its neighbours'.
        std::vector<std::pair<uint32_t, uint32_t>> directed;
        std::unordered_set<EdgeKey> walked;
        std::vector<uint32_t> starts;
        for (uint32_t v = 0; v < m.VertexCount(); ++v)
            if (adj[v].size() == 1) starts.push_back(v);
        for (uint32_t v = 0; v < m.VertexCount(); ++v)
            if (adj[v].size() > 1) starts.push_back(v);
        for (uint32_t s : starts)
        {
            uint32_t v = s;
            for (;;)
            {
                int64_t next = -1;
                for (uint32_t w : adj[v])
                    if (!walked.contains(MakeEdgeKey(v, w)))
                    {
                        next = w;
                        break;
                    }
                if (next < 0) break;
                walked.insert(MakeEdgeKey(v, (uint32_t)next));
                directed.push_back({ v, (uint32_t)next });
                v = (uint32_t)next;
            }
        }
        std::unordered_map<uint32_t, std::pair<glm::vec3, int>> plus, minus;
        for (auto [a, b] : directed)
        {
            int32_t e = t.FindEdge(a, b);
            if (e < 0) continue;
            for (uint32_t f : t.EdgeFaces[e])
            {
                if (Hid(m, f)) continue;
                const Face& face = m.Faces[f];
                size_t n = face.Verts.size();
                int ca = CornerOf(face, a), cb = CornerOf(face, b);
                bool forward = Walks(face, a, b);
                // The corners beside a and b away from the edge.
                uint32_t ra = forward ? face.Verts[(ca + n - 1) % n] : face.Verts[(ca + 1) % n];
                uint32_t rb = forward ? face.Verts[(cb + 1) % n] : face.Verts[(cb + n - 1) % n];
                auto& side = forward ? plus : minus;
                if (!m.IsEdgeSelected(MakeEdgeKey(a, ra)))
                {
                    side[a].first += m.Positions[ra];
                    ++side[a].second;
                }
                if (!m.IsEdgeSelected(MakeEdgeKey(b, rb)))
                {
                    side[b].first += m.Positions[rb];
                    ++side[b].second;
                }
            }
        }
        for (uint32_t v = 0; v < m.VertexCount(); ++v)
        {
            if (adj[v].empty() || m.IsVertHidden(v)) continue;
            rails.Verts.push_back(v);
            rails.Start.push_back(m.Positions[v]);
            auto target = [&](auto& side) {
                auto it = side.find(v);
                return it == side.end() || it->second.second == 0 ? m.Positions[v] : it->second.first / (float)it->second.second;
            };
            rails.Plus.push_back(target(plus));
            rails.Minus.push_back(target(minus));
        }
        return rails;
    }

    void ApplySlide(EditableMesh& m, const SlideRails& rails, float factor)
    {
        factor = std::clamp(factor, -1.0f, 1.0f);
        for (size_t i = 0; i < rails.Verts.size(); ++i)
        {
            const glm::vec3& s = rails.Start[i];
            m.Positions[rails.Verts[i]] = factor >= 0.0f ? s + (rails.Plus[i] - s) * factor : s + (rails.Minus[i] - s) * -factor;
        }
    }

    // ---- Knife and connect

    size_t Knife(EditableMesh& m, const std::vector<CutPoint>& path)
    {
        if (path.size() < 2) return 0;
        std::map<EdgeKey, std::vector<std::pair<float, uint32_t>>> splits; // original edge -> (t from its A end, vertex)
        std::vector<uint32_t> ids;
        for (const CutPoint& p : path)
        {
            if (p.A >= m.VertexCount() || p.B >= m.VertexCount()) continue;
            if (p.A == p.B || p.T <= 1e-4f)
            {
                ids.push_back(p.A);
                continue;
            }
            if (p.T >= 1.0f - 1e-4f)
            {
                ids.push_back(p.B);
                continue;
            }
            EdgeKey k = MakeEdgeKey(p.A, p.B);
            float tk = p.A == EdgeA(k) ? p.T : 1.0f - p.T;
            auto& list = splits[k];
            std::pair<float, uint32_t> lo = { 0.0f, EdgeA(k) }, hi = { 1.0f, EdgeB(k) };
            int64_t same = -1;
            for (auto& s : list)
            {
                if (std::abs(s.first - tk) < 1e-4f) same = s.second;
                if (s.first <= tk && s.first > lo.first) lo = s;
                if (s.first >= tk && s.first < hi.first) hi = s;
            }
            if (same >= 0)
            {
                ids.push_back((uint32_t)same);
                continue;
            }
            uint32_t nv = InsertOnEdge(m, lo.second, hi.second, (tk - lo.first) / std::max(hi.first - lo.first, 1e-6f));
            list.push_back({ tk, nv });
            ids.push_back(nv);
        }
        size_t count = 0;
        std::vector<std::pair<uint32_t, uint32_t>> made;
        for (size_t i = 0; i + 1 < ids.size(); ++i)
        {
            uint32_t u = ids[i], w = ids[i + 1];
            if (u == w) continue;
            bool joined = false;
            for (uint32_t f = 0; f < m.FaceCount() && !joined; ++f)
            {
                const Face& face = m.Faces[f];
                if (Hid(m, f)) continue;
                int cu = CornerOf(face, u), cw = CornerOf(face, w);
                if (cu < 0 || cw < 0) continue;
                int n = (int)face.Verts.size();
                if ((cu + 1) % n == cw || (cw + 1) % n == cu)
                {
                    joined = true; // already an edge
                    break;
                }
                SplitFace(m, f, (uint32_t)cu, (uint32_t)cw);
                ++count;
                joined = true;
            }
            made.push_back({ u, w });
        }
        DeselectAll(m);
        m.Changed();
        const Topology& t = m.Topo();
        for (uint32_t v : ids) SelectVertex(m, v, true);
        for (auto [u, w] : made)
            if (t.FindEdge(u, w) >= 0) SelectEdge(m, MakeEdgeKey(u, w), true);
        return count;
    }

    size_t ConnectVertices(EditableMesh& m)
    {
        size_t count = 0;
        std::vector<std::pair<uint32_t, uint32_t>> made;
        for (bool again = true; again && count < 100000;)
        {
            again = false;
            for (uint32_t f = 0; f < m.FaceCount() && !again; ++f)
            {
                if (Hid(m, f)) continue;
                const Face& face = m.Faces[f];
                std::vector<uint32_t> sel;
                for (uint32_t c = 0; c < face.Verts.size(); ++c)
                    if (m.IsVertSelected(face.Verts[c]) && !m.IsVertHidden(face.Verts[c])) sel.push_back(c);
                if (sel.size() < 2) continue;
                size_t n = face.Verts.size();
                for (size_t i = 0; i < sel.size(); ++i)
                {
                    uint32_t ci = sel[i], cj = sel[(i + 1) % sel.size()];
                    if (ci == cj || (ci + 1) % n == cj || (cj + 1) % n == ci) continue;
                    made.push_back({ face.Verts[ci], face.Verts[cj] });
                    SplitFace(m, f, ci, cj);
                    ++count;
                    again = true;
                    break;
                }
            }
        }
        for (auto [a, b] : made) SelectEdge(m, MakeEdgeKey(a, b), true);
        return count;
    }

    // ---- Fill and bridge

    bool Fill(EditableMesh& m)
    {
        const Topology t = m.Topo();
        glm::vec3 meshCenter = m.Bounds().Center();
        std::vector<uint32_t> made;
        auto orient = [&](std::vector<uint32_t>& loop) {
            // Agree with a neighbouring face if there is one, else face away from the mesh's middle.
            for (size_t i = 0; i < loop.size(); ++i)
            {
                int32_t e = t.FindEdge(loop[i], loop[(i + 1) % loop.size()]);
                if (e < 0 || t.EdgeFaces[e].empty()) continue;
                if (Walks(m.Faces[t.EdgeFaces[e][0]], loop[i], loop[(i + 1) % loop.size()])) std::reverse(loop.begin(), loop.end());
                return;
            }
            glm::vec3 c(0.0f);
            for (uint32_t v : loop) c += m.Positions[v];
            c /= (float)loop.size();
            if (m.FaceCount() > 0 && glm::dot(LoopNormal(m, loop), c - meshCenter) < 0.0f) std::reverse(loop.begin(), loop.end());
        };
        auto exists = [&](const std::vector<uint32_t>& loop) {
            std::vector<uint32_t> key = loop;
            std::sort(key.begin(), key.end());
            for (const Face& f : m.Faces)
            {
                if (f.Verts.size() != key.size()) continue;
                std::vector<uint32_t> o = f.Verts;
                std::sort(o.begin(), o.end());
                if (o == key) return true;
            }
            return false;
        };
        auto addFace = [&](std::vector<uint32_t> loop) {
            if (loop.size() < 3 || exists(loop)) return;
            orient(loop);
            std::vector<glm::vec2> uvs = PlanarUVs(m, loop);
            const Face* like = nullptr;
            for (uint32_t v : loop)
                if (!t.VertFaces[v].empty())
                {
                    like = &m.Faces[t.VertFaces[v][0]];
                    break;
                }
            made.push_back(m.AddFace(loop, std::move(uvs), like ? like->Material : 0, like ? like->Smooth : false));
        };
        // Loops (or open chains) of selected edges on the border or wire.
        std::unordered_map<uint32_t, std::vector<uint32_t>> adj;
        for (size_t e = 0; e < t.Edges.size(); ++e)
        {
            EdgeKey k = t.Edges[e];
            if (!m.IsEdgeSelected(k) || !EdgeUsable(m, k) || t.EdgeFaces[e].size() >= 2) continue;
            adj[EdgeA(k)].push_back(EdgeB(k));
            adj[EdgeB(k)].push_back(EdgeA(k));
        }
        bool chains = !adj.empty();
        for (auto& [v, n] : adj) chains &= n.size() <= 2;
        if (chains)
        {
            std::unordered_set<uint32_t> seen;
            std::vector<uint32_t> order;
            for (auto& [v, n] : adj) order.push_back(v);
            std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) { return adj[a].size() < adj[b].size() || (adj[a].size() == adj[b].size() && a < b); });
            for (uint32_t s : order)
            {
                if (seen.contains(s)) continue;
                std::vector<uint32_t> loop = { s };
                seen.insert(s);
                uint32_t cur = s;
                for (;;)
                {
                    int64_t next = -1;
                    for (uint32_t w : adj[cur])
                        if (!seen.contains(w))
                        {
                            next = w;
                            break;
                        }
                    if (next < 0) break;
                    seen.insert((uint32_t)next);
                    loop.push_back((uint32_t)next);
                    cur = (uint32_t)next;
                }
                addFace(loop);
            }
        }
        if (made.empty())
        {
            std::vector<uint32_t> sel = SelectedVertices(m);
            if (sel.size() == 2)
            {
                if (t.FindEdge(sel[0], sel[1]) >= 0) return false;
                m.LooseEdges.push_back(MakeEdgeKey(sel[0], sel[1]));
                m.Changed();
                SelectEdge(m, MakeEdgeKey(sel[0], sel[1]), true);
                return true;
            }
            if (sel.size() < 3) return false;
            // One face around the selected vertices, in order of angle about their middle.
            glm::vec3 c(0.0f);
            for (uint32_t v : sel) c += m.Positions[v];
            c /= (float)sel.size();
            uint32_t far = sel[0];
            for (uint32_t v : sel)
                if (glm::length(m.Positions[v] - c) > glm::length(m.Positions[far] - c)) far = v;
            glm::vec3 u = m.Positions[far] - c;
            glm::vec3 best(0.0f);
            for (uint32_t v : sel)
            {
                glm::vec3 n = glm::cross(u, m.Positions[v] - c);
                if (glm::length(n) > glm::length(best)) best = n;
            }
            if (glm::length(u) < 1e-12f || glm::length(best) < 1e-12f) return false;
            glm::vec3 n = glm::normalize(best), x = glm::normalize(u), y = glm::cross(n, x);
            std::sort(sel.begin(), sel.end(), [&](uint32_t a, uint32_t b) {
                glm::vec3 da = m.Positions[a] - c, db = m.Positions[b] - c;
                return std::atan2(glm::dot(da, y), glm::dot(da, x)) < std::atan2(glm::dot(db, y), glm::dot(db, x));
            });
            addFace(sel);
        }
        if (made.empty()) return false;
        RemoveLooseCoveredByFaces(m);
        DeselectAll(m);
        SelectFacesFully(m, made);
        return true;
    }

    bool BridgeEdgeLoops(EditableMesh& m)
    {
        const Topology t = m.Topo();
        // The loops: the outlines of selected faces (which go), or the selected edges.
        std::vector<std::pair<uint32_t, uint32_t>> loopEdges;
        std::vector<uint8_t> dropFace(m.FaceCount(), 0);
        bool anyFace = false;
        for (uint32_t f = 0; f < m.FaceCount(); ++f) anyFace |= FaceSel(m, f);
        std::vector<uint8_t> inner(m.VertexCount(), 0);
        if (anyFace)
        {
            for (size_t e = 0; e < t.Edges.size(); ++e)
            {
                int sel = 0;
                for (uint32_t f : t.EdgeFaces[e]) sel += FaceSel(m, f) ? 1 : 0;
                if (sel == 1) loopEdges.push_back({ EdgeA(t.Edges[e]), EdgeB(t.Edges[e]) });
            }
            for (uint32_t f = 0; f < m.FaceCount(); ++f)
                if (FaceSel(m, f))
                {
                    dropFace[f] = 1;
                    for (uint32_t v : m.Faces[f].Verts) inner[v] = 1;
                }
            for (auto [a, b] : loopEdges) inner[a] = inner[b] = 0;
        }
        else
            for (EdgeKey k : t.Edges)
                if (m.IsEdgeSelected(k) && EdgeUsable(m, k)) loopEdges.push_back({ EdgeA(k), EdgeB(k) });
        std::unordered_map<uint32_t, std::vector<uint32_t>> adj;
        for (auto [a, b] : loopEdges)
        {
            adj[a].push_back(b);
            adj[b].push_back(a);
        }
        for (auto& [v, n] : adj)
            if (n.size() > 2) return false;
        // Order the two chains.
        std::vector<std::vector<uint32_t>> loops;
        std::vector<bool> closed;
        std::unordered_set<uint32_t> seen;
        std::vector<uint32_t> order;
        for (auto& [v, n] : adj) order.push_back(v);
        std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) { return adj[a].size() < adj[b].size() || (adj[a].size() == adj[b].size() && a < b); });
        for (uint32_t s : order)
        {
            if (seen.contains(s)) continue;
            std::vector<uint32_t> loop = { s };
            seen.insert(s);
            uint32_t cur = s;
            for (;;)
            {
                int64_t next = -1;
                for (uint32_t w : adj[cur])
                    if (!seen.contains(w))
                    {
                        next = w;
                        break;
                    }
                if (next < 0) break;
                seen.insert((uint32_t)next);
                loop.push_back((uint32_t)next);
                cur = (uint32_t)next;
            }
            closed.push_back(adj[s].size() == 2);
            loops.push_back(std::move(loop));
        }
        if (loops.size() != 2 || closed[0] != closed[1] || loops[0].size() < 2 || loops[1].size() < 2) return false;
        bool isClosed = closed[0];
        std::vector<uint32_t> A = loops[0], B = loops[1];
        auto centroid = [&](const std::vector<uint32_t>& l) {
            glm::vec3 c(0.0f);
            for (uint32_t v : l) c += m.Positions[v];
            return c / (float)l.size();
        };
        glm::vec3 cA = centroid(A), cB = centroid(B);
        size_t nA = A.size(), nB = B.size();
        // Line B up with A: direction and (closed) starting point with the least twist.
        {
            float best = std::numeric_limits<float>::max();
            std::vector<uint32_t> bestB = B;
            for (int dir = 0; dir < 2; ++dir)
            {
                std::vector<uint32_t> cand = B;
                if (dir) std::reverse(cand.begin(), cand.end());
                size_t shifts = isClosed ? nB : 1;
                for (size_t s = 0; s < shifts; ++s)
                {
                    float sum = 0.0f;
                    for (size_t i = 0; i < nA; ++i)
                    {
                        size_t j = nA == nB ? (i + s) % nB : ((size_t)std::lround((double)i * nB / nA) + s) % nB;
                        glm::vec3 d = (m.Positions[A[i]] - cA) - (m.Positions[cand[j]] - cB);
                        sum += glm::dot(d, d);
                    }
                    if (sum < best)
                    {
                        best = sum;
                        bestB.clear();
                        for (size_t i = 0; i < nB; ++i) bestB.push_back(cand[(i + s) % nB]);
                    }
                }
            }
            B = bestB;
        }
        std::vector<std::vector<uint32_t>> faces;
        if (nA == nB)
        {
            size_t count = isClosed ? nA : nA - 1;
            for (size_t i = 0; i < count; ++i) faces.push_back({ A[i], A[(i + 1) % nA], B[(i + 1) % nB], B[i] });
        }
        else
        {
            // Unequal: triangles, advancing whichever side is behind along its length.
            auto params = [&](const std::vector<uint32_t>& l) {
                std::vector<float> p = { 0.0f };
                size_t n = l.size(), segs = isClosed ? n : n - 1;
                for (size_t i = 0; i < segs; ++i) p.push_back(p.back() + glm::length(m.Positions[l[(i + 1) % n]] - m.Positions[l[i]]));
                for (float& x : p) x /= std::max(p.back(), 1e-12f);
                return p;
            };
            std::vector<float> pa = params(A), pb = params(B);
            size_t sa = isClosed ? nA : nA - 1, sb = isClosed ? nB : nB - 1;
            size_t i = 0, j = 0;
            while (i < sa || j < sb)
            {
                bool advanceA = j >= sb || (i < sa && pa[i + 1] <= pb[j + 1]);
                if (advanceA)
                {
                    faces.push_back({ A[i % nA], A[(i + 1) % nA], B[j % nB] });
                    ++i;
                }
                else
                {
                    faces.push_back({ A[i % nA], B[(j + 1) % nB], B[j % nB] });
                    ++j;
                }
            }
        }
        // Remove the faces inside the loops first, then turn the bridge to agree with the faces beside it.
        std::vector<Face> kept;
        for (uint32_t f = 0; f < m.FaceCount(); ++f)
            if (!dropFace[f]) kept.push_back(m.Faces[f]);
        m.Faces = std::move(kept);
        m.Changed();
        bool flip = false;
        {
            const Topology& t2 = m.Topo();
            bool decided = false;
            for (const auto& face : faces)
            {
                for (size_t i = 0; i < face.size() && !decided; ++i)
                {
                    uint32_t a = face[i], b = face[(i + 1) % face.size()];
                    int32_t e = t2.FindEdge(a, b);
                    if (e < 0 || t2.EdgeFaces[e].empty()) continue;
                    flip = Walks(m.Faces[t2.EdgeFaces[e][0]], a, b);
                    decided = true;
                }
                if (decided) break;
            }
            if (!decided && m.FaceCount() == 0)
            {
                // Loose loops: face away from the middle of both.
                glm::vec3 mid = (cA + cB) * 0.5f;
                glm::vec3 c(0.0f);
                for (uint32_t v : faces[0]) c += m.Positions[v];
                c /= (float)faces[0].size();
                flip = glm::dot(LoopNormal(m, faces[0]), c - mid) < 0.0f;
            }
        }
        std::vector<uint32_t> made;
        for (auto face : faces)
        {
            if (flip) std::reverse(face.begin(), face.end());
            std::vector<glm::vec2> uvs = PlanarUVs(m, face);
            made.push_back(m.AddFace(std::move(face), std::move(uvs)));
        }
        RemoveLooseCoveredByFaces(m);
        DeselectAll(m);
        SelectFacesFully(m, made);
        if (anyFace) RemoveUnusedVertices(m, &inner);
        m.PruneEdgeAttrs();
        return true;
    }

    // ---- Dissolve

    void DissolveVertices(EditableMesh& m)
    {
        const Topology t = m.Topo();
        Groups g(m.FaceCount());
        std::vector<uint8_t> candidates(m.VertexCount(), 0);
        for (uint32_t v = 0; v < m.VertexCount(); ++v)
        {
            if (!m.IsVertSelected(v) || m.IsVertHidden(v)) continue;
            candidates[v] = 1;
            const auto& vf = t.VertFaces[v];
            for (size_t i = 1; i < vf.size(); ++i)
                if (!Hid(m, vf[0]) && !Hid(m, vf[i])) g.Join(vf[0], vf[i]);
        }
        std::map<uint32_t, std::vector<uint32_t>> groups;
        for (uint32_t f = 0; f < m.FaceCount(); ++f) groups[g.Find(f)].push_back(f);
        std::vector<std::vector<uint32_t>> list;
        for (auto& [r, fs] : groups)
            if (fs.size() > 1) list.push_back(fs);
        std::vector<uint32_t> made = MergeFaceGroups(m, list);
        // Wire-only vertices: two edges become one, others just go.
        std::vector<EdgeKey> loose;
        std::unordered_map<uint32_t, std::vector<uint32_t>> wires;
        for (EdgeKey k : m.LooseEdges)
        {
            wires[EdgeA(k)].push_back(EdgeB(k));
            wires[EdgeB(k)].push_back(EdgeA(k));
        }
        std::unordered_set<EdgeKey> drop;
        for (auto& [v, others] : wires)
        {
            if (!candidates[v] || !m.Topo().VertFaces[v].empty()) continue;
            for (uint32_t o : others) drop.insert(MakeEdgeKey(v, o));
            if (others.size() == 2 && !candidates[others[0]] && !candidates[others[1]]) loose.push_back(MakeEdgeKey(others[0], others[1]));
        }
        std::erase_if(m.LooseEdges, [&](EdgeKey k) { return drop.contains(k); });
        for (EdgeKey k : loose) m.LooseEdges.push_back(k);
        m.Changed();
        RemoveLooseCoveredByFaces(m);
        RemoveTwoEdgeVertices(m, candidates);
        candidates.resize(m.VertexCount(), 0);
        DeselectAll(m);
        SelectFacesFully(m, made);
        RemoveUnusedVertices(m, &candidates);
        m.PruneEdgeAttrs();
    }

    void DissolveEdges(EditableMesh& m)
    {
        const Topology t = m.Topo();
        Groups g(m.FaceCount());
        std::vector<uint8_t> candidates(m.VertexCount(), 0);
        for (size_t e = 0; e < t.Edges.size(); ++e)
        {
            EdgeKey k = t.Edges[e];
            if (!m.IsEdgeSelected(k) || !EdgeUsable(m, k)) continue;
            candidates[EdgeA(k)] = candidates[EdgeB(k)] = 1;
            const auto& ef = t.EdgeFaces[e];
            if (ef.size() == 2 && !Hid(m, ef[0]) && !Hid(m, ef[1])) g.Join(ef[0], ef[1]);
        }
        std::map<uint32_t, std::vector<uint32_t>> groups;
        for (uint32_t f = 0; f < m.FaceCount(); ++f) groups[g.Find(f)].push_back(f);
        std::vector<std::vector<uint32_t>> list;
        for (auto& [r, fs] : groups)
            if (fs.size() > 1) list.push_back(fs);
        std::erase_if(m.LooseEdges, [&](EdgeKey k) { return m.IsEdgeSelected(k); });
        m.Changed();
        std::vector<uint32_t> made = MergeFaceGroups(m, list);
        DeselectAll(m);
        SelectFacesFully(m, made);
        RemoveTwoEdgeVertices(m, candidates);
        candidates.resize(m.VertexCount(), 0);
        // Vertices only the removed edges used go too.
        std::vector<uint8_t> unused(m.VertexCount(), 0);
        for (uint32_t v = 0; v < m.VertexCount(); ++v) unused[v] = candidates[v];
        RemoveUnusedVertices(m, &unused);
        m.PruneEdgeAttrs();
    }

    void DissolveFaces(EditableMesh& m)
    {
        const Topology t = m.Topo();
        Groups g(m.FaceCount());
        std::vector<uint8_t> candidates(m.VertexCount(), 0);
        for (size_t e = 0; e < t.Edges.size(); ++e)
        {
            const auto& ef = t.EdgeFaces[e];
            if (ef.size() == 2 && FaceSel(m, ef[0]) && FaceSel(m, ef[1]))
            {
                g.Join(ef[0], ef[1]);
                candidates[EdgeA(t.Edges[e])] = candidates[EdgeB(t.Edges[e])] = 1;
            }
        }
        std::map<uint32_t, std::vector<uint32_t>> groups;
        for (uint32_t f = 0; f < m.FaceCount(); ++f)
            if (FaceSel(m, f)) groups[g.Find(f)].push_back(f);
        std::vector<std::vector<uint32_t>> list;
        for (auto& [r, fs] : groups)
            if (fs.size() > 1) list.push_back(fs);
        std::vector<uint32_t> made = MergeFaceGroups(m, list);
        DeselectAll(m);
        SelectFacesFully(m, made);
        RemoveTwoEdgeVertices(m, candidates);
        candidates.resize(m.VertexCount(), 0);
        RemoveUnusedVertices(m, &candidates);
        m.PruneEdgeAttrs();
    }

    void LimitedDissolve(EditableMesh& m, float angleDegrees)
    {
        const Topology t = m.Topo();
        float limit = glm::radians(std::max(angleDegrees, 0.0f));
        bool anySelected = false;
        for (EdgeKey k : t.Edges) anySelected |= m.IsEdgeSelected(k);
        std::vector<glm::vec3> normals(m.FaceCount());
        for (uint32_t f = 0; f < m.FaceCount(); ++f) normals[f] = m.FaceNormal(f);
        Groups g(m.FaceCount());
        std::vector<uint8_t> candidates(m.VertexCount(), 0);
        for (size_t e = 0; e < t.Edges.size(); ++e)
        {
            EdgeKey k = t.Edges[e];
            if ((anySelected && !m.IsEdgeSelected(k)) || !EdgeUsable(m, k)) continue;
            candidates[EdgeA(k)] = candidates[EdgeB(k)] = 1;
            const auto& ef = t.EdgeFaces[e];
            if (ef.size() != 2 || Hid(m, ef[0]) || Hid(m, ef[1])) continue;
            const Face &a = m.Faces[ef[0]], &b = m.Faces[ef[1]];
            if (a.Material != b.Material || a.Smooth != b.Smooth) continue;
            EdgeData d = m.GetEdge(k);
            if (d.Sharp || d.Seam) continue;
            float angle = std::acos(std::clamp(glm::dot(normals[ef[0]], normals[ef[1]]), -1.0f, 1.0f));
            if (angle <= limit) g.Join(ef[0], ef[1]);
        }
        std::map<uint32_t, std::vector<uint32_t>> groups;
        for (uint32_t f = 0; f < m.FaceCount(); ++f) groups[g.Find(f)].push_back(f);
        std::vector<std::vector<uint32_t>> list;
        for (auto& [r, fs] : groups)
            if (fs.size() > 1) list.push_back(fs);
        std::vector<uint32_t> made = MergeFaceGroups(m, list);
        bool hadSelection = anySelected;
        RemoveTwoEdgeVertices(m, candidates, limit);
        candidates.resize(m.VertexCount(), 0);
        RemoveUnusedVertices(m, &candidates);
        if (hadSelection)
        {
            DeselectAll(m);
            SelectFacesFully(m, made);
        }
        m.PruneEdgeAttrs();
    }

    // ---- Subdivide

    void Subdivide(EditableMesh& m, uint32_t cuts)
    {
        cuts = std::max(1u, cuts);
        const uint32_t n = cuts + 1;
        const Topology t = m.Topo();
        std::unordered_map<EdgeKey, std::vector<uint32_t>> pts; // from the key's A end
        std::vector<uint32_t> newVerts;
        for (EdgeKey k : t.Edges)
        {
            if (!m.IsEdgeSelected(k) || !EdgeUsable(m, k)) continue;
            std::vector<uint32_t> list;
            for (uint32_t j = 1; j < n; ++j)
            {
                list.push_back(m.AddVertex(glm::mix(m.Positions[EdgeA(k)], m.Positions[EdgeB(k)], (float)j / (float)n)));
                newVerts.push_back(list.back());
            }
            pts[k] = std::move(list);
        }
        if (pts.empty()) return;
        auto edgePoint = [&](uint32_t a, uint32_t b, uint32_t k) -> uint32_t {
            if (k == 0) return a;
            if (k == n) return b;
            EdgeKey key = MakeEdgeKey(a, b);
            const auto& list = pts.at(key);
            return a == EdgeA(key) ? list[k - 1] : list[n - k - 1];
        };
        auto selectedEdge = [&](uint32_t a, uint32_t b) { return pts.contains(MakeEdgeKey(a, b)); };
        std::vector<Face> faces;
        std::vector<uint32_t> made;
        auto emit = [&](const Face& like, std::vector<uint32_t> v, std::vector<glm::vec2> uv, bool select) {
            Face f = like;
            f.Verts = std::move(v);
            f.UVs = std::move(uv);
            f.Flags &= ~Selected;
            if (select) made.push_back((uint32_t)faces.size());
            faces.push_back(std::move(f));
        };
        for (uint32_t fi = 0; fi < m.FaceCount(); ++fi)
        {
            const Face face = m.Faces[fi];
            size_t c = face.Verts.size();
            std::vector<uint8_t> sel(c);
            size_t selCount = 0;
            for (size_t i = 0; i < c; ++i)
            {
                sel[i] = selectedEdge(face.Verts[i], face.Verts[(i + 1) % c]);
                selCount += sel[i];
            }
            bool all = selCount == c && !Hid(m, fi);
            const auto& V = face.Verts;
            auto uv = [&](size_t i) { return CornerUV(face, i); };
            if (all && c == 4)
            {
                // A grid: Q(i, j), i along corner 0 -> 1, j along 0 -> 3.
                std::vector<uint32_t> grid((n + 1) * (n + 1));
                std::vector<glm::vec2> guv((n + 1) * (n + 1));
                for (uint32_t j = 0; j <= n; ++j)
                    for (uint32_t i = 0; i <= n; ++i)
                    {
                        float s = (float)i / n, r = (float)j / n;
                        uint32_t id;
                        if (j == 0) id = edgePoint(V[0], V[1], i);
                        else if (j == n) id = edgePoint(V[3], V[2], i);
                        else if (i == 0) id = edgePoint(V[0], V[3], j);
                        else if (i == n) id = edgePoint(V[1], V[2], j);
                        else
                        {
                            glm::vec3 p = glm::mix(glm::mix(m.Positions[V[0]], m.Positions[V[1]], s), glm::mix(m.Positions[V[3]], m.Positions[V[2]], s), r);
                            id = m.AddVertex(p);
                            newVerts.push_back(id);
                        }
                        grid[j * (n + 1) + i] = id;
                        guv[j * (n + 1) + i] = glm::mix(glm::mix(uv(0), uv(1), s), glm::mix(uv(3), uv(2), s), r);
                    }
                for (uint32_t j = 0; j < n; ++j)
                    for (uint32_t i = 0; i < n; ++i)
                    {
                        uint32_t a = j * (n + 1) + i, b = a + 1, d = a + n + 1, e = d + 1;
                        emit(face, { grid[a], grid[b], grid[e], grid[d] }, { guv[a], guv[b], guv[e], guv[d] }, true);
                    }
            }
            else if (all && c == 3)
            {
                // Triangles: P(i, j) = c0 + i/n (c1 - c0) + j/n (c2 - c0).
                std::map<std::pair<uint32_t, uint32_t>, std::pair<uint32_t, glm::vec2>> P;
                for (uint32_t i = 0; i <= n; ++i)
                    for (uint32_t j = 0; i + j <= n; ++j)
                    {
                        float s = (float)i / n, r = (float)j / n;
                        uint32_t id;
                        if (j == 0) id = edgePoint(V[0], V[1], i);
                        else if (i == 0) id = edgePoint(V[0], V[2], j);
                        else if (i + j == n) id = edgePoint(V[1], V[2], j);
                        else
                        {
                            id = m.AddVertex(m.Positions[V[0]] + s * (m.Positions[V[1]] - m.Positions[V[0]]) + r * (m.Positions[V[2]] - m.Positions[V[0]]));
                            newVerts.push_back(id);
                        }
                        P[{ i, j }] = { id, uv(0) + s * (uv(1) - uv(0)) + r * (uv(2) - uv(0)) };
                    }
                for (uint32_t i = 0; i < n; ++i)
                    for (uint32_t j = 0; i + j < n; ++j)
                    {
                        auto a = P[{ i, j }], b = P[{ i + 1, j }], d = P[{ i, j + 1 }];
                        emit(face, { a.first, b.first, d.first }, { a.second, b.second, d.second }, true);
                        if (i + j + 2 <= n)
                        {
                            auto e = P[{ i + 1, j + 1 }];
                            emit(face, { b.first, e.first, d.first }, { b.second, e.second, d.second }, true);
                        }
                    }
            }
            else if (all && c > 4 && cuts == 1)
            {
                // Quads around a center point.
                uint32_t center = m.AddVertex(m.FaceCenter(fi));
                newVerts.push_back(center);
                glm::vec2 uvc(0.0f);
                for (size_t i = 0; i < c; ++i) uvc += uv(i);
                uvc /= (float)c;
                for (size_t i = 0; i < c; ++i)
                {
                    size_t p = (i + c - 1) % c, q = (i + 1) % c;
                    emit(face, { V[i], edgePoint(V[i], V[q], 1), center, edgePoint(V[p], V[i], 1) },
                         { uv(i), (uv(i) + uv(q)) * 0.5f, uvc, (uv(p) + uv(i)) * 0.5f }, true);
                }
            }
            else if (c == 4 && selCount == 2 && ((sel[0] && sel[2]) || (sel[1] && sel[3])) && !Hid(m, fi))
            {
                // Strips across a quad with two opposite edges cut.
                size_t o = sel[0] ? 0 : 1;
                size_t c0 = o, c1 = (o + 1) % 4, c2 = (o + 2) % 4, c3 = (o + 3) % 4;
                for (uint32_t k = 0; k < n; ++k)
                {
                    float s0 = (float)k / n, s1 = (float)(k + 1) / n;
                    emit(face, { edgePoint(V[c0], V[c1], k), edgePoint(V[c0], V[c1], k + 1), edgePoint(V[c3], V[c2], k + 1), edgePoint(V[c3], V[c2], k) },
                         { glm::mix(uv(c0), uv(c1), s0), glm::mix(uv(c0), uv(c1), s1), glm::mix(uv(c3), uv(c2), s1), glm::mix(uv(c3), uv(c2), s0) },
                         (face.Flags & Selected) != 0);
                }
            }
            else
            {
                // The cut edges' points become extra corners.
                std::vector<uint32_t> v;
                std::vector<glm::vec2> u;
                for (size_t i = 0; i < c; ++i)
                {
                    v.push_back(V[i]);
                    u.push_back(uv(i));
                    if (sel[i])
                        for (uint32_t k = 1; k < n; ++k)
                        {
                            v.push_back(edgePoint(V[i], V[(i + 1) % c], k));
                            u.push_back(glm::mix(uv(i), uv((i + 1) % c), (float)k / n));
                        }
                }
                Face f = face;
                f.Verts = std::move(v);
                f.UVs = std::move(u);
                faces.push_back(std::move(f));
            }
        }
        // Wire edges that were cut become chains.
        std::vector<EdgeKey> loose;
        for (EdgeKey k : m.LooseEdges)
        {
            if (!pts.contains(k))
            {
                loose.push_back(k);
                continue;
            }
            for (uint32_t j = 0; j < n; ++j) loose.push_back(MakeEdgeKey(edgePoint(EdgeA(k), EdgeB(k), j), edgePoint(EdgeA(k), EdgeB(k), j + 1)));
        }
        // Edge marks follow the pieces.
        for (auto& [k, list] : pts)
            if (auto it = m.EdgeAttrs.find(k); it != m.EdgeAttrs.end())
            {
                EdgeData d = it->second;
                for (uint32_t j = 0; j < n; ++j) m.EdgeAttrs[MakeEdgeKey(edgePoint(EdgeA(k), EdgeB(k), j), edgePoint(EdgeA(k), EdgeB(k), j + 1))] = d;
            }
        m.Faces = std::move(faces);
        m.LooseEdges = std::move(loose);
        m.Changed();
        RemoveLooseCoveredByFaces(m);
        // The pieces of the cut edges and the subdivided faces stay selected.
        std::vector<EdgeKey> pieces;
        for (auto& [k, list] : pts)
            for (uint32_t j = 0; j < n; ++j) pieces.push_back(MakeEdgeKey(edgePoint(EdgeA(k), EdgeB(k), j), edgePoint(EdgeA(k), EdgeB(k), j + 1)));
        DeselectAll(m);
        SelectFacesFully(m, made);
        for (EdgeKey k : pieces)
        {
            SelectVertex(m, EdgeA(k), true);
            SelectVertex(m, EdgeB(k), true);
            SelectEdge(m, k, true);
        }
        for (uint32_t v : newVerts) SelectVertex(m, v, true);
        m.PruneEdgeAttrs();
    }

    // ---- Spin, mirror

    void Spin(EditableMesh& m, const glm::vec3& center, const glm::vec3& axis, float angle, uint32_t steps)
    {
        if (glm::length(axis) < 1e-12f || steps == 0 || !AnySelected(m)) return;
        steps = std::max(1u, steps);
        std::vector<uint32_t> start = SelectedVertices(m);
        bool faces = false;
        for (uint32_t f = 0; f < m.FaceCount(); ++f) faces |= FaceSel(m, f);
        glm::mat4 turn = glm::translate(glm::mat4(1.0f), center) * glm::rotate(glm::mat4(1.0f), angle / (float)steps, glm::normalize(axis)) *
                         glm::translate(glm::mat4(1.0f), -center);
        for (uint32_t s = 0; s < steps; ++s)
        {
            Extrude(m, false);
            TransformVertices(m, turn);
        }
        if (std::abs(std::abs(angle) - glm::two_pi<float>()) > 1e-4f) return;
        // A full turn: the last copy lands on the start; weld them.
        AABB b = m.Bounds();
        float eps = std::max(glm::length(b.Max - b.Min), 1.0f) * 1e-4f;
        std::vector<uint32_t> remap(m.VertexCount());
        std::iota(remap.begin(), remap.end(), 0u);
        for (uint32_t v : SelectedVertices(m))
            for (uint32_t o : start)
                if (glm::length(m.Positions[v] - m.Positions[o]) <= eps)
                {
                    remap[v] = o;
                    break;
                }
        if (faces)
        {
            // The end cap now sits inside the ring.
            std::vector<Face> kept;
            for (uint32_t f = 0; f < m.FaceCount(); ++f)
                if (!FaceSel(m, f)) kept.push_back(m.Faces[f]);
            m.Faces = std::move(kept);
            m.Changed();
        }
        RemapVertices(m, remap);
    }

    void Mirror(EditableMesh& m, const glm::vec3& pivot, const glm::vec3& axis)
    {
        if (glm::length(axis) < 1e-12f) return;
        glm::vec3 n = glm::normalize(axis);
        for (uint32_t v : SelectedVertices(m)) m.Positions[v] -= 2.0f * glm::dot(m.Positions[v] - pivot, n) * n;
        for (uint32_t f = 0; f < m.FaceCount(); ++f)
        {
            Face& face = m.Faces[f];
            if (Hid(m, f)) continue;
            bool all = true;
            for (uint32_t v : face.Verts) all &= m.IsVertSelected(v);
            if (!all) continue;
            std::reverse(face.Verts.begin(), face.Verts.end());
            std::reverse(face.UVs.begin(), face.UVs.end());
        }
        m.Changed();
    }

    // ---- Separate and join

    namespace
    {
        // A mesh of the given faces, wire edges and vertices of `m` (renumbered), carrying its settings.
        EditableMesh Extract(const EditableMesh& m, const std::vector<uint32_t>& faces, const std::vector<EdgeKey>& loose, const std::vector<uint32_t>& lone)
        {
            EditableMesh out;
            out.Name = m.Name;
            out.MaterialSlots = m.MaterialSlots;
            out.AutoSmooth = m.AutoSmooth;
            out.AutoSmoothAngle = m.AutoSmoothAngle;
            std::unordered_map<uint32_t, uint32_t> map;
            auto get = [&](uint32_t v) {
                auto it = map.find(v);
                if (it != map.end()) return it->second;
                uint32_t nv = out.AddVertex(m.Positions[v], m.VertFlags[v] & Hidden);
                map[v] = nv;
                return nv;
            };
            for (uint32_t f : faces)
            {
                Face face = m.Faces[f];
                for (uint32_t& v : face.Verts) v = get(v);
                face.Flags &= Hidden;
                out.Faces.push_back(std::move(face));
            }
            for (EdgeKey k : loose) out.LooseEdges.push_back(MakeEdgeKey(get(EdgeA(k)), get(EdgeB(k))));
            for (uint32_t v : lone) get(v);
            for (auto& [k, d] : m.EdgeAttrs)
            {
                auto a = map.find(EdgeA(k)), b = map.find(EdgeB(k));
                if (a == map.end() || b == map.end()) continue;
                EdgeData nd = d;
                nd.Flags &= Hidden;
                if (!nd.IsDefault()) out.EdgeAttrs[MakeEdgeKey(a->second, b->second)] = nd;
            }
            out.Changed();
            out.PruneEdgeAttrs();
            return out;
        }
    }

    EditableMesh Separate(EditableMesh& m)
    {
        std::vector<uint32_t> faces;
        for (uint32_t f = 0; f < m.FaceCount(); ++f)
            if (FaceSel(m, f)) faces.push_back(f);
        std::vector<EdgeKey> loose;
        for (EdgeKey k : m.LooseEdges)
            if (m.IsEdgeSelected(k) && EdgeUsable(m, k)) loose.push_back(k);
        std::vector<uint8_t> used(m.VertexCount(), 0);
        for (uint32_t f : faces)
            for (uint32_t v : m.Faces[f].Verts) used[v] = 1;
        for (EdgeKey k : loose) used[EdgeA(k)] = used[EdgeB(k)] = 1;
        const Topology& t = m.Topo();
        std::vector<uint32_t> lone;
        for (uint32_t v = 0; v < m.VertexCount(); ++v)
            if (m.IsVertSelected(v) && !m.IsVertHidden(v) && !used[v] && t.VertEdges[v].empty()) lone.push_back(v);
        EditableMesh out = Extract(m, faces, loose, lone);
        // Remove them here.
        std::vector<uint8_t> dropFace(m.FaceCount(), 0);
        for (uint32_t f : faces) dropFace[f] = 1;
        std::vector<Face> kept;
        for (uint32_t f = 0; f < m.FaceCount(); ++f)
            if (!dropFace[f]) kept.push_back(std::move(m.Faces[f]));
        m.Faces = std::move(kept);
        std::unordered_set<EdgeKey> dropLoose(loose.begin(), loose.end());
        std::erase_if(m.LooseEdges, [&](EdgeKey k) { return dropLoose.contains(k); });
        for (uint32_t v : lone) used[v] = 1;
        m.Changed();
        RemoveUnusedVertices(m, &used);
        DeselectAll(m);
        m.PruneEdgeAttrs();
        return out;
    }

    std::vector<EditableMesh> SeparateLooseParts(EditableMesh& m)
    {
        Groups g(m.VertexCount());
        for (const Face& f : m.Faces)
            for (size_t i = 1; i < f.Verts.size(); ++i) g.Join(f.Verts[0], f.Verts[i]);
        for (EdgeKey k : m.LooseEdges) g.Join(EdgeA(k), EdgeB(k));
        std::map<uint32_t, std::tuple<std::vector<uint32_t>, std::vector<EdgeKey>, std::vector<uint32_t>>> parts;
        for (uint32_t f = 0; f < m.FaceCount(); ++f)
            if (!m.Faces[f].Verts.empty()) std::get<0>(parts[g.Find(m.Faces[f].Verts[0])]).push_back(f);
        for (EdgeKey k : m.LooseEdges) std::get<1>(parts[g.Find(EdgeA(k))]).push_back(k);
        for (uint32_t v = 0; v < m.VertexCount(); ++v)
            if (g.Find(v) == v && !parts.contains(v)) std::get<2>(parts[v]).push_back(v);
        if (parts.size() < 2) return {};
        std::vector<EditableMesh> out;
        EditableMesh first;
        bool haveFirst = false;
        for (auto& [root, p] : parts)
        {
            EditableMesh part = Extract(m, std::get<0>(p), std::get<1>(p), std::get<2>(p));
            if (!haveFirst)
            {
                first = std::move(part);
                haveFirst = true;
            }
            else out.push_back(std::move(part));
        }
        m = std::move(first);
        m.Changed();
        return out;
    }

    uint32_t Join(EditableMesh& m, const EditableMesh& other, const glm::mat4& transform)
    {
        uint32_t firstFace = m.FaceCount();
        uint32_t base = m.Append(other);
        for (uint32_t v = base; v < m.VertexCount(); ++v) m.Positions[v] = glm::vec3(transform * glm::vec4(m.Positions[v], 1.0f));
        if (glm::determinant(glm::mat3(transform)) < 0.0f)
            for (uint32_t f = firstFace; f < m.FaceCount(); ++f)
            {
                std::reverse(m.Faces[f].Verts.begin(), m.Faces[f].Verts.end());
                std::reverse(m.Faces[f].UVs.begin(), m.Faces[f].UVs.end());
            }
        m.Changed();
        return base;
    }
}
