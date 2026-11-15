#include "Dimension/Modeling/MeshOps.h"
#include <algorithm>
#include <cmath>
#include <numeric>
#include <limits>
#include <queue>
#include <unordered_set>

namespace Dimension::Modeling::Ops
{
    namespace
    {
        uint32_t OtherVert(EdgeKey k, uint32_t v) { return EdgeA(k) == v ? EdgeB(k) : EdgeA(k); }

        bool FaceHidden(const EditableMesh& m, uint32_t f) { return m.Faces[f].Flags & Hidden; }

        void SetFlag(uint8_t& flags, uint8_t flag, bool on) { flags = on ? (flags | flag) : (flags & ~flag); }

        // Union-find over indices.
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
                if (a != b) Parent[std::max(a, b)] = std::min(a, b); // the smallest index is the root
            }
        };

        // Buckets of points for neighbour searches within a distance.
        struct SpatialHash
        {
            float Cell;
            std::unordered_map<uint64_t, std::vector<uint32_t>> Buckets;
            explicit SpatialHash(float cell) : Cell(std::max(cell, 1e-9f)) {}
            static uint64_t Key(int64_t x, int64_t y, int64_t z)
            {
                return ((uint64_t)(x & 0x1FFFFF) << 42) | ((uint64_t)(y & 0x1FFFFF) << 21) | (uint64_t)(z & 0x1FFFFF);
            }
            glm::i64vec3 CellOf(const glm::vec3& p) const
            {
                return { (int64_t)std::floor(p.x / Cell), (int64_t)std::floor(p.y / Cell), (int64_t)std::floor(p.z / Cell) };
            }
            void Insert(const glm::vec3& p, uint32_t i)
            {
                auto c = CellOf(p);
                Buckets[Key(c.x, c.y, c.z)].push_back(i);
            }
            template <class F> void Near(const glm::vec3& p, F&& f) const
            {
                auto c = CellOf(p);
                for (int64_t x = c.x - 1; x <= c.x + 1; ++x)
                    for (int64_t y = c.y - 1; y <= c.y + 1; ++y)
                        for (int64_t z = c.z - 1; z <= c.z + 1; ++z)
                            if (auto it = Buckets.find(Key(x, y, z)); it != Buckets.end())
                                for (uint32_t i : it->second) f(i);
            }
        };
    }

    // ---- Selection

    void SelectVertex(EditableMesh& m, uint32_t v, bool on)
    {
        if (v < m.VertFlags.size() && !(on && m.IsVertHidden(v))) SetFlag(m.VertFlags[v], Selected, on);
    }

    void SelectEdge(EditableMesh& m, EdgeKey e, bool on)
    {
        if (on && m.IsEdgeHidden(e)) return;
        m.SetEdgeFlag(e, Selected, on);
    }

    void SelectFace(EditableMesh& m, uint32_t f, bool on)
    {
        if (f < m.Faces.size() && !(on && FaceHidden(m, f))) SetFlag(m.Faces[f].Flags, Selected, on);
    }

    void FlushSelection(EditableMesh& m, SelectMode mode)
    {
        const Topology& t = m.Topo();
        for (uint32_t v = 0; v < m.VertexCount(); ++v)
            if (m.IsVertHidden(v)) SetFlag(m.VertFlags[v], Selected, false);
        for (uint32_t f = 0; f < m.FaceCount(); ++f)
            if (FaceHidden(m, f)) SetFlag(m.Faces[f].Flags, Selected, false);
        for (EdgeKey k : t.Edges)
            if (m.IsEdgeHidden(k)) m.SetEdgeFlag(k, Selected, false);

        switch (mode)
        {
        case SelectMode::Vertex:
            for (EdgeKey k : t.Edges) m.SetEdgeFlag(k, Selected, m.IsVertSelected(EdgeA(k)) && m.IsVertSelected(EdgeB(k)));
            for (auto& f : m.Faces)
            {
                bool all = !f.Verts.empty();
                for (uint32_t v : f.Verts) all &= m.IsVertSelected(v);
                SetFlag(f.Flags, Selected, all);
            }
            break;
        case SelectMode::Edge:
            for (auto& fl : m.VertFlags) SetFlag(fl, Selected, false);
            for (EdgeKey k : t.Edges)
                if (m.IsEdgeSelected(k))
                {
                    SetFlag(m.VertFlags[EdgeA(k)], Selected, true);
                    SetFlag(m.VertFlags[EdgeB(k)], Selected, true);
                }
            for (uint32_t f = 0; f < m.FaceCount(); ++f)
            {
                bool all = !t.FaceEdges[f].empty();
                for (uint32_t e : t.FaceEdges[f]) all &= m.IsEdgeSelected(t.Edges[e]);
                SetFlag(m.Faces[f].Flags, Selected, all);
            }
            break;
        case SelectMode::Face:
            for (auto& fl : m.VertFlags) SetFlag(fl, Selected, false);
            for (EdgeKey k : t.Edges) m.SetEdgeFlag(k, Selected, false);
            for (uint32_t f = 0; f < m.FaceCount(); ++f)
                if (m.Faces[f].Flags & Selected)
                {
                    for (uint32_t v : m.Faces[f].Verts) SetFlag(m.VertFlags[v], Selected, true);
                    for (uint32_t e : t.FaceEdges[f]) m.SetEdgeFlag(t.Edges[e], Selected, true);
                }
            break;
        }
    }

    void ConvertSelection(EditableMesh& m, SelectMode from, SelectMode to)
    {
        FlushSelection(m, from);
        if (from == SelectMode::Edge && to == SelectMode::Vertex) return; // already consistent
        FlushSelection(m, to);
    }

    void SelectAll(EditableMesh& m, SelectMode mode)
    {
        for (uint32_t v = 0; v < m.VertexCount(); ++v) SelectVertex(m, v, true);
        for (EdgeKey k : m.Topo().Edges) SelectEdge(m, k, true);
        for (uint32_t f = 0; f < m.FaceCount(); ++f) SelectFace(m, f, true);
        FlushSelection(m, mode);
    }

    void DeselectAll(EditableMesh& m)
    {
        for (auto& f : m.VertFlags) SetFlag(f, Selected, false);
        for (auto& f : m.Faces) SetFlag(f.Flags, Selected, false);
        for (auto it = m.EdgeAttrs.begin(); it != m.EdgeAttrs.end();)
        {
            it->second.Flags &= ~Selected;
            if (it->second.IsDefault()) it = m.EdgeAttrs.erase(it);
            else ++it;
        }
    }

    void InvertSelection(EditableMesh& m, SelectMode mode)
    {
        switch (mode)
        {
        case SelectMode::Vertex:
            for (uint32_t v = 0; v < m.VertexCount(); ++v) SelectVertex(m, v, !m.IsVertSelected(v));
            break;
        case SelectMode::Edge:
            for (EdgeKey k : m.Topo().Edges) SelectEdge(m, k, !m.IsEdgeSelected(k));
            break;
        case SelectMode::Face:
            for (uint32_t f = 0; f < m.FaceCount(); ++f) SelectFace(m, f, !(m.Faces[f].Flags & Selected));
            break;
        }
        FlushSelection(m, mode);
    }

    bool AnySelected(const EditableMesh& m)
    {
        for (uint8_t f : m.VertFlags)
            if (f & Selected) return true;
        return false;
    }

    void SelectLinked(EditableMesh& m, SelectMode mode, int64_t seedVertex, bool deselect)
    {
        const Topology& t = m.Topo();
        std::vector<uint8_t> reached(m.VertexCount(), 0);
        std::vector<uint32_t> stack;
        if (seedVertex >= 0 && seedVertex < (int64_t)m.VertexCount()) stack.push_back((uint32_t)seedVertex);
        else
            for (uint32_t v = 0; v < m.VertexCount(); ++v)
                if (m.IsVertSelected(v)) stack.push_back(v);
        for (uint32_t v : stack) reached[v] = 1;
        while (!stack.empty())
        {
            uint32_t v = stack.back();
            stack.pop_back();
            for (uint32_t e : t.VertEdges[v])
            {
                uint32_t o = OtherVert(t.Edges[e], v);
                if (reached[o] || m.IsVertHidden(o)) continue;
                reached[o] = 1;
                stack.push_back(o);
            }
            // Faces link their corners too (non-manifold corners that share no edge).
            for (uint32_t f : t.VertFaces[v])
                for (uint32_t o : m.Faces[f].Verts)
                    if (!reached[o] && !m.IsVertHidden(o))
                    {
                        reached[o] = 1;
                        stack.push_back(o);
                    }
        }
        switch (mode)
        {
        case SelectMode::Vertex:
            for (uint32_t v = 0; v < m.VertexCount(); ++v)
                if (reached[v]) SelectVertex(m, v, !deselect);
            break;
        case SelectMode::Edge:
            for (EdgeKey k : t.Edges)
                if (reached[EdgeA(k)]) SelectEdge(m, k, !deselect);
            break;
        case SelectMode::Face:
            for (uint32_t f = 0; f < m.FaceCount(); ++f)
                if (!m.Faces[f].Verts.empty() && reached[m.Faces[f].Verts[0]]) SelectFace(m, f, !deselect);
            break;
        }
        FlushSelection(m, mode);
    }

    void SelectMore(EditableMesh& m, SelectMode mode)
    {
        const Topology& t = m.Topo();
        if (mode == SelectMode::Face)
        {
            std::vector<uint8_t> grow(m.FaceCount(), 0);
            for (uint32_t f = 0; f < m.FaceCount(); ++f)
                if (m.Faces[f].Flags & Selected)
                    for (uint32_t v : m.Faces[f].Verts)
                        for (uint32_t g : t.VertFaces[v]) grow[g] = 1;
            for (uint32_t f = 0; f < m.FaceCount(); ++f)
                if (grow[f]) SelectFace(m, f, true);
        }
        else
        {
            // Vertices of faces touching the selection (Blender's face step), and along wire edges.
            std::vector<uint8_t> grow(m.VertexCount(), 0);
            for (uint32_t v = 0; v < m.VertexCount(); ++v)
            {
                if (!m.IsVertSelected(v)) continue;
                for (uint32_t f : t.VertFaces[v])
                    for (uint32_t o : m.Faces[f].Verts) grow[o] = 1;
                for (uint32_t e : t.VertEdges[v]) grow[OtherVert(t.Edges[e], v)] = 1;
            }
            for (uint32_t v = 0; v < m.VertexCount(); ++v)
                if (grow[v]) SelectVertex(m, v, true);
            if (mode == SelectMode::Edge) FlushSelection(m, SelectMode::Vertex);
        }
        FlushSelection(m, mode);
    }

    void SelectLess(EditableMesh& m, SelectMode mode)
    {
        const Topology& t = m.Topo();
        if (mode == SelectMode::Face)
        {
            std::vector<uint8_t> drop(m.FaceCount(), 0);
            for (uint32_t f = 0; f < m.FaceCount(); ++f)
            {
                if (!(m.Faces[f].Flags & Selected)) continue;
                for (uint32_t v : m.Faces[f].Verts)
                    for (uint32_t g : t.VertFaces[v])
                        if (!(m.Faces[g].Flags & Selected)) drop[f] = 1;
            }
            for (uint32_t f = 0; f < m.FaceCount(); ++f)
                if (drop[f]) SelectFace(m, f, false);
        }
        else
        {
            std::vector<uint8_t> drop(m.VertexCount(), 0);
            for (uint32_t v = 0; v < m.VertexCount(); ++v)
            {
                if (!m.IsVertSelected(v)) continue;
                for (uint32_t e : t.VertEdges[v])
                    if (!m.IsVertSelected(OtherVert(t.Edges[e], v))) drop[v] = 1;
            }
            for (uint32_t v = 0; v < m.VertexCount(); ++v)
                if (drop[v]) SelectVertex(m, v, false);
            if (mode == SelectMode::Edge) FlushSelection(m, SelectMode::Vertex);
        }
        FlushSelection(m, mode);
    }

    namespace
    {
        bool EdgeInFace(const Topology& t, uint32_t f, uint32_t e)
        {
            return std::find(t.FaceEdges[f].begin(), t.FaceEdges[f].end(), e) != t.FaceEdges[f].end();
        }

        // The edge continuing a loop from `cur` through vertex `v` (-1: the loop stops here).
        int64_t NextLoopEdge(const EditableMesh& m, uint32_t cur, uint32_t v)
        {
            const Topology& t = m.Topo();
            const auto& faces = t.EdgeFaces[cur];
            const auto& edges = t.VertEdges[v];
            if (faces.empty())
            {
                // Wire edges: through vertices with exactly two edges.
                if (edges.size() != 2) return -1;
                return edges[0] == cur ? edges[1] : edges[0];
            }
            if (faces.size() == 1)
            {
                // Along the border: the other border edge at v.
                int64_t next = -1;
                int count = 0;
                for (uint32_t e : edges)
                    if (e != cur && t.EdgeFaces[e].size() == 1)
                    {
                        next = e;
                        ++count;
                    }
                return count == 1 ? next : -1;
            }
            if (faces.size() != 2 || edges.size() != 4) return -1;
            for (uint32_t e : edges)
                if (t.EdgeFaces[e].size() != 2) return -1;
            for (uint32_t e : edges)
                if (e != cur && !EdgeInFace(t, faces[0], e) && !EdgeInFace(t, faces[1], e)) return e;
            return -1;
        }
    }

    std::vector<uint32_t> EdgeLoop(const EditableMesh& m, uint32_t edge)
    {
        const Topology& t = m.Topo();
        std::vector<uint32_t> loop = { edge };
        if (edge >= t.Edges.size()) return {};
        std::unordered_set<uint32_t> seen = { edge };
        for (uint32_t start : { EdgeA(t.Edges[edge]), EdgeB(t.Edges[edge]) })
        {
            uint32_t cur = edge, v = start;
            for (;;)
            {
                int64_t next = NextLoopEdge(m, cur, v);
                if (next < 0 || seen.contains((uint32_t)next)) break;
                seen.insert((uint32_t)next);
                loop.push_back((uint32_t)next);
                cur = (uint32_t)next;
                v = OtherVert(t.Edges[cur], v);
            }
        }
        return loop;
    }

    std::vector<uint32_t> EdgeRing(const EditableMesh& m, uint32_t edge, std::vector<uint32_t>* faces)
    {
        const Topology& t = m.Topo();
        if (edge >= t.Edges.size()) return {};
        std::vector<uint32_t> ring = { edge };
        std::unordered_set<uint32_t> seenEdges = { edge }, seenFaces;
        for (uint32_t startFace : t.EdgeFaces[edge])
        {
            uint32_t cur = edge, f = startFace;
            for (;;)
            {
                if (seenFaces.contains(f) || m.Faces[f].Verts.size() != 4 || FaceHidden(m, f)) break;
                seenFaces.insert(f);
                if (faces) faces->push_back(f);
                const auto& fe = t.FaceEdges[f];
                size_t i = std::find(fe.begin(), fe.end(), cur) - fe.begin();
                uint32_t opposite = fe[(i + 2) % 4];
                if (seenEdges.contains(opposite)) break;
                seenEdges.insert(opposite);
                ring.push_back(opposite);
                const auto& of = t.EdgeFaces[opposite];
                if (of.size() != 2) break;
                f = of[0] == f ? of[1] : of[0];
                cur = opposite;
            }
        }
        return ring;
    }

    void SelectLoop(EditableMesh& m, SelectMode mode, uint32_t edge, bool extend, bool ring)
    {
        const Topology& t = m.Topo();
        if (edge >= t.Edges.size()) return;
        if (mode == SelectMode::Face)
        {
            std::vector<uint32_t> faces;
            EdgeRing(m, edge, &faces);
            bool all = !faces.empty();
            for (uint32_t f : faces) all &= (m.Faces[f].Flags & Selected) != 0;
            if (!extend) DeselectAll(m);
            for (uint32_t f : faces) SelectFace(m, f, !(extend && all));
        }
        else
        {
            std::vector<uint32_t> edges = ring ? EdgeRing(m, edge) : EdgeLoop(m, edge);
            bool all = true;
            for (uint32_t e : edges) all &= m.IsEdgeSelected(t.Edges[e]);
            bool on = !(extend && all);
            if (!extend) DeselectAll(m);
            for (uint32_t e : edges)
            {
                EdgeKey k = t.Edges[e];
                if (m.IsEdgeHidden(k)) continue;
                SelectEdge(m, k, on);
                if (mode == SelectMode::Vertex || on)
                {
                    SelectVertex(m, EdgeA(k), on);
                    SelectVertex(m, EdgeB(k), on);
                }
            }
        }
        FlushSelection(m, mode);
    }

    std::vector<uint32_t> SelectedVertices(const EditableMesh& m)
    {
        std::vector<uint32_t> out;
        for (uint32_t v = 0; v < m.VertexCount(); ++v)
            if (m.IsVertSelected(v) && !m.IsVertHidden(v)) out.push_back(v);
        return out;
    }

    glm::vec3 SelectionCenter(const EditableMesh& m)
    {
        glm::vec3 sum(0.0f);
        size_t n = 0;
        for (uint32_t v = 0; v < m.VertexCount(); ++v)
            if (m.IsVertSelected(v) && !m.IsVertHidden(v))
            {
                sum += m.Positions[v];
                ++n;
            }
        if (n) return sum / (float)n;
        return m.Bounds().Center();
    }

    // ---- Hiding

    void HideSelected(EditableMesh& m, SelectMode mode, bool unselected)
    {
        const Topology& t = m.Topo();
        auto target = [&](bool selected, bool hidden) { return !hidden && selected != unselected; };
        std::vector<uint8_t> hideV(m.VertexCount(), 0), hideE(t.Edges.size(), 0), hideF(m.FaceCount(), 0);
        switch (mode)
        {
        case SelectMode::Vertex:
            for (uint32_t v = 0; v < m.VertexCount(); ++v) hideV[v] = target(m.IsVertSelected(v), m.IsVertHidden(v));
            for (size_t e = 0; e < t.Edges.size(); ++e) hideE[e] = hideV[EdgeA(t.Edges[e])] || hideV[EdgeB(t.Edges[e])];
            for (uint32_t f = 0; f < m.FaceCount(); ++f)
                for (uint32_t v : m.Faces[f].Verts) hideF[f] |= hideV[v];
            break;
        case SelectMode::Edge:
            for (size_t e = 0; e < t.Edges.size(); ++e) hideE[e] = target(m.IsEdgeSelected(t.Edges[e]), m.IsEdgeHidden(t.Edges[e]));
            for (uint32_t f = 0; f < m.FaceCount(); ++f)
                for (uint32_t e : t.FaceEdges[f]) hideF[f] |= hideE[e];
            for (uint32_t v = 0; v < m.VertexCount(); ++v)
            {
                bool all = !t.VertEdges[v].empty();
                for (uint32_t e : t.VertEdges[v]) all &= hideE[e] || m.IsEdgeHidden(t.Edges[e]);
                hideV[v] = all;
            }
            break;
        case SelectMode::Face:
            for (uint32_t f = 0; f < m.FaceCount(); ++f) hideF[f] = target((m.Faces[f].Flags & Selected) != 0, FaceHidden(m, f));
            for (size_t e = 0; e < t.Edges.size(); ++e)
            {
                bool all = !t.EdgeFaces[e].empty();
                for (uint32_t f : t.EdgeFaces[e]) all &= hideF[f] || FaceHidden(m, f);
                hideE[e] = all;
            }
            for (uint32_t v = 0; v < m.VertexCount(); ++v)
            {
                bool all = !t.VertEdges[v].empty();
                for (uint32_t e : t.VertEdges[v]) all &= hideE[e] || m.IsEdgeHidden(t.Edges[e]);
                hideV[v] = all;
            }
            break;
        }
        for (uint32_t v = 0; v < m.VertexCount(); ++v)
            if (hideV[v]) m.VertFlags[v] = (m.VertFlags[v] | Hidden) & ~Selected;
        for (size_t e = 0; e < t.Edges.size(); ++e)
            if (hideE[e])
            {
                m.SetEdgeFlag(t.Edges[e], Hidden, true);
                m.SetEdgeFlag(t.Edges[e], Selected, false);
            }
        for (uint32_t f = 0; f < m.FaceCount(); ++f)
            if (hideF[f]) m.Faces[f].Flags = (m.Faces[f].Flags | Hidden) & ~Selected;
        FlushSelection(m, mode);
    }

    void RevealAll(EditableMesh& m, bool select)
    {
        for (auto& f : m.VertFlags)
            if (f & Hidden) f = select ? Selected : 0;
        for (auto& f : m.Faces)
            if (f.Flags & Hidden) f.Flags = select ? Selected : 0;
        for (auto it = m.EdgeAttrs.begin(); it != m.EdgeAttrs.end();)
        {
            if (it->second.Flags & Hidden) it->second.Flags = select ? Selected : 0;
            if (it->second.IsDefault()) it = m.EdgeAttrs.erase(it);
            else ++it;
        }
    }

    // ---- Proportional editing

    float FalloffWeight(Falloff f, float t)
    {
        t = std::clamp(t, 0.0f, 1.0f);
        float s = 1.0f - t;
        switch (f)
        {
        case Falloff::Smooth: return 3.0f * s * s - 2.0f * s * s * s;
        case Falloff::Sphere: return std::sqrt(std::max(0.0f, 2.0f * s - s * s));
        case Falloff::Root: return std::sqrt(s);
        case Falloff::Linear: return s;
        case Falloff::Sharp: return s * s;
        case Falloff::Constant: return t < 1.0f ? 1.0f : 0.0f;
        case Falloff::InverseSquare: return s * (2.0f - s);
        }
        return s;
    }

    std::vector<float> ProportionalWeights(const EditableMesh& m, float radius, Falloff falloff, bool connected)
    {
        uint32_t n = m.VertexCount();
        std::vector<float> w(n, 0.0f);
        std::vector<float> dist(n, std::numeric_limits<float>::infinity());
        std::vector<uint32_t> selected;
        for (uint32_t v = 0; v < n; ++v)
            if (m.IsVertSelected(v) && !m.IsVertHidden(v))
            {
                dist[v] = 0.0f;
                selected.push_back(v);
            }
        radius = std::max(radius, 1e-6f);
        if (connected)
        {
            // Shortest paths along edges, up to the radius.
            const Topology& t = m.Topo();
            using Item = std::pair<float, uint32_t>;
            std::priority_queue<Item, std::vector<Item>, std::greater<Item>> queue;
            for (uint32_t v : selected) queue.push({ 0.0f, v });
            while (!queue.empty())
            {
                auto [d, v] = queue.top();
                queue.pop();
                if (d > dist[v]) continue;
                for (uint32_t e : t.VertEdges[v])
                {
                    uint32_t o = OtherVert(t.Edges[e], v);
                    if (m.IsVertHidden(o)) continue;
                    float nd = d + glm::distance(m.Positions[v], m.Positions[o]);
                    if (nd < dist[o] && nd < radius)
                    {
                        dist[o] = nd;
                        queue.push({ nd, o });
                    }
                }
            }
        }
        else
        {
            SpatialHash hash(radius);
            for (uint32_t v : selected) hash.Insert(m.Positions[v], v);
            for (uint32_t v = 0; v < n; ++v)
            {
                if (dist[v] == 0.0f || m.IsVertHidden(v)) continue;
                hash.Near(m.Positions[v], [&](uint32_t s) { dist[v] = std::min(dist[v], glm::distance(m.Positions[v], m.Positions[s])); });
            }
        }
        for (uint32_t v = 0; v < n; ++v)
            if (dist[v] == 0.0f) w[v] = 1.0f;
            else if (dist[v] < radius) w[v] = FalloffWeight(falloff, dist[v] / radius);
        return w;
    }

    // ---- Topology edits

    void RemoveUnusedVertices(EditableMesh& m, const std::vector<uint8_t>* onlyThese)
    {
        uint32_t n = m.VertexCount();
        std::vector<uint8_t> used(n, 0);
        for (const auto& f : m.Faces)
            for (uint32_t v : f.Verts) used[v] = 1;
        for (EdgeKey k : m.LooseEdges)
        {
            used[EdgeA(k)] = 1;
            used[EdgeB(k)] = 1;
        }
        std::vector<uint32_t> newIndex(n, UINT32_MAX);
        uint32_t next = 0;
        for (uint32_t v = 0; v < n; ++v)
        {
            bool remove = !used[v] && (!onlyThese || (v < onlyThese->size() && (*onlyThese)[v]));
            if (remove) continue;
            newIndex[v] = next;
            m.Positions[next] = m.Positions[v];
            m.VertFlags[next] = m.VertFlags[v];
            if (m.Weights.size() == n) m.Weights[next] = std::move(m.Weights[v]);
            if (m.Mask.size() == n) m.Mask[next] = m.Mask[v];
            for (ShapeKey& k : m.ShapeKeys)
                if (k.Positions.size() == n) k.Positions[next] = k.Positions[v];
            ++next;
        }
        if (next == n) return;
        m.Positions.resize(next);
        m.VertFlags.resize(next);
        if (m.Weights.size() == n) m.Weights.resize(next);
        if (m.Mask.size() == n) m.Mask.resize(next);
        for (ShapeKey& k : m.ShapeKeys)
            if (k.Positions.size() == n) k.Positions.resize(next);
        for (auto& f : m.Faces)
            for (uint32_t& v : f.Verts) v = newIndex[v];
        for (EdgeKey& k : m.LooseEdges) k = MakeEdgeKey(newIndex[EdgeA(k)], newIndex[EdgeB(k)]);
        std::unordered_map<EdgeKey, EdgeData> attrs;
        for (auto& [k, d] : m.EdgeAttrs)
            if (newIndex[EdgeA(k)] != UINT32_MAX && newIndex[EdgeB(k)] != UINT32_MAX) attrs[MakeEdgeKey(newIndex[EdgeA(k)], newIndex[EdgeB(k)])] = d;
        m.EdgeAttrs = std::move(attrs);
        m.Changed();
    }

    void RemapVertices(EditableMesh& m, const std::vector<uint32_t>& remap)
    {
        // Faces: repeated corners (in a row) collapse; faces left with fewer than 3 corners go, as do duplicates.
        std::vector<Face> faces;
        std::unordered_set<std::string> seen;
        for (auto& f : m.Faces)
        {
            Face out = f;
            out.Verts.clear();
            out.UVs.clear();
            out.Colors.clear();
            out.UVFlags.clear();
            for (size_t c = 0; c < f.Verts.size(); ++c)
            {
                uint32_t v = remap[f.Verts[c]];
                if (!out.Verts.empty() && out.Verts.back() == v) continue;
                out.Verts.push_back(v);
                out.UVs.push_back(c < f.UVs.size() ? f.UVs[c] : glm::vec2(0.0f));
                out.Colors.push_back(f.ColorAt(c));
                out.UVFlags.push_back(f.CornerFlags(c));
            }
            while (out.Verts.size() > 1 && out.Verts.front() == out.Verts.back())
            {
                out.Verts.pop_back();
                out.UVs.pop_back();
                out.Colors.pop_back();
                out.UVFlags.pop_back();
            }
            if (out.Verts.size() < 3) continue;
            std::vector<uint32_t> key = out.Verts;
            std::sort(key.begin(), key.end());
            if (!seen.insert(std::string((const char*)key.data(), key.size() * 4)).second) continue;
            faces.push_back(std::move(out));
        }
        m.Faces = std::move(faces);
        // Edge data merges (flags and marks combine; the larger crease wins).
        std::unordered_map<EdgeKey, EdgeData> attrs;
        for (auto& [k, d] : m.EdgeAttrs)
        {
            uint32_t a = remap[EdgeA(k)], b = remap[EdgeB(k)];
            if (a == b) continue;
            EdgeData& o = attrs[MakeEdgeKey(a, b)];
            o.Flags |= d.Flags;
            o.Sharp |= d.Sharp;
            o.Seam |= d.Seam;
            o.Crease = std::max(o.Crease, d.Crease);
        }
        m.EdgeAttrs = std::move(attrs);
        std::vector<EdgeKey> loose;
        for (EdgeKey k : m.LooseEdges)
        {
            uint32_t a = remap[EdgeA(k)], b = remap[EdgeB(k)];
            if (a != b) loose.push_back(MakeEdgeKey(a, b));
        }
        m.LooseEdges.clear();
        m.Changed();
        // Loose edges that faces now cover, or repeated, are dropped.
        std::unordered_set<EdgeKey> faceEdges(m.Topo().Edges.begin(), m.Topo().Edges.end());
        for (EdgeKey k : loose)
            if (faceEdges.insert(k).second) m.LooseEdges.push_back(k);
        m.Changed();
        std::vector<uint8_t> mappedAway(m.VertexCount(), 0);
        for (uint32_t v = 0; v < remap.size() && v < mappedAway.size(); ++v) mappedAway[v] = remap[v] != v;
        RemoveUnusedVertices(m, &mappedAway);
        m.PruneEdgeAttrs();
    }

    namespace
    {
        // Removes faces flagged in `drop`. `keepEdges`: their edges that no remaining face uses stay as wire edges
        // (except those flagged in `dropEdges`).
        void RemoveFaces(EditableMesh& m, const std::vector<uint8_t>& drop, bool keepEdges, const std::unordered_set<EdgeKey>& dropEdges = {})
        {
            std::vector<EdgeKey> orphan;
            if (keepEdges)
                for (uint32_t f = 0; f < m.FaceCount(); ++f)
                    if (drop[f])
                    {
                        const auto& v = m.Faces[f].Verts;
                        for (size_t i = 0; i < v.size(); ++i) orphan.push_back(MakeEdgeKey(v[i], v[(i + 1) % v.size()]));
                    }
            std::vector<Face> kept;
            for (uint32_t f = 0; f < m.FaceCount(); ++f)
                if (!drop[f]) kept.push_back(std::move(m.Faces[f]));
            m.Faces = std::move(kept);
            m.Changed();
            if (!orphan.empty())
            {
                const Topology& t = m.Topo();
                std::unordered_set<EdgeKey> have(t.Edges.begin(), t.Edges.end());
                for (EdgeKey k : orphan)
                    if (!dropEdges.contains(k) && have.insert(k).second) m.LooseEdges.push_back(k);
                m.Changed();
            }
        }
    }

    void Delete(EditableMesh& m, DeleteKind kind)
    {
        const Topology& t = m.Topo();
        uint32_t n = m.VertexCount();
        std::vector<uint8_t> dropFace(m.FaceCount(), 0);
        std::vector<uint8_t> candidates(n, 0); // vertices removed if nothing uses them afterwards
        std::unordered_set<EdgeKey> dropEdges;
        for (EdgeKey k : t.Edges)
            if (m.IsEdgeSelected(k)) dropEdges.insert(k);

        switch (kind)
        {
        case DeleteKind::Vertices:
            for (uint32_t f = 0; f < m.FaceCount(); ++f)
                for (uint32_t v : m.Faces[f].Verts) dropFace[f] |= (uint8_t)m.IsVertSelected(v);
            for (uint32_t v = 0; v < n; ++v) candidates[v] = m.IsVertSelected(v);
            std::erase_if(m.LooseEdges, [&](EdgeKey k) { return m.IsVertSelected(EdgeA(k)) || m.IsVertSelected(EdgeB(k)); });
            RemoveFaces(m, dropFace, false);
            // Selected vertices go even if a wire edge still used them (those edges went with them above).
            RemoveUnusedVertices(m, &candidates);
            break;
        case DeleteKind::Edges:
            for (uint32_t f = 0; f < m.FaceCount(); ++f)
                for (uint32_t e : t.FaceEdges[f]) dropFace[f] |= (uint8_t)m.IsEdgeSelected(t.Edges[e]);
            for (uint32_t v = 0; v < n; ++v) candidates[v] = m.IsVertSelected(v);
            std::erase_if(m.LooseEdges, [&](EdgeKey k) { return dropEdges.contains(k); });
            // Unselected edges of removed faces remain as wire edges.
            RemoveFaces(m, dropFace, true, dropEdges);
            RemoveUnusedVertices(m, &candidates);
            break;
        case DeleteKind::Faces:
            for (uint32_t f = 0; f < m.FaceCount(); ++f)
                if (m.Faces[f].Flags & Selected)
                {
                    dropFace[f] = 1;
                    for (uint32_t v : m.Faces[f].Verts) candidates[v] = 1;
                }
            RemoveFaces(m, dropFace, false);
            RemoveUnusedVertices(m, &candidates);
            break;
        case DeleteKind::OnlyEdgesAndFaces:
            for (uint32_t f = 0; f < m.FaceCount(); ++f)
            {
                dropFace[f] = (m.Faces[f].Flags & Selected) != 0;
                for (uint32_t e : t.FaceEdges[f]) dropFace[f] |= (uint8_t)m.IsEdgeSelected(t.Edges[e]);
            }
            std::erase_if(m.LooseEdges, [&](EdgeKey k) { return dropEdges.contains(k); });
            RemoveFaces(m, dropFace, true, dropEdges);
            break;
        case DeleteKind::OnlyFaces:
            for (uint32_t f = 0; f < m.FaceCount(); ++f) dropFace[f] = (m.Faces[f].Flags & Selected) != 0;
            RemoveFaces(m, dropFace, true);
            break;
        }
        m.PruneEdgeAttrs();
    }

    size_t MergeAt(EditableMesh& m, const glm::vec3& point)
    {
        std::vector<uint32_t> sel = SelectedVertices(m);
        if (sel.size() < 1) return 0;
        uint32_t target = sel[0];
        std::vector<uint32_t> remap(m.VertexCount());
        std::iota(remap.begin(), remap.end(), 0u);
        for (uint32_t v : sel) remap[v] = target;
        m.Positions[target] = point;
        size_t before = m.VertexCount();
        RemapVertices(m, remap);
        return before - m.VertexCount();
    }

    size_t MergeAtCenter(EditableMesh& m, bool collapse)
    {
        if (!collapse) return MergeAt(m, SelectionCenter(m));
        // Each group of selected vertices joined by selected edges merges at its own center.
        const Topology& t = m.Topo();
        Groups groups(m.VertexCount());
        for (EdgeKey k : t.Edges)
            if (m.IsVertSelected(EdgeA(k)) && m.IsVertSelected(EdgeB(k)) && !m.IsVertHidden(EdgeA(k)) && !m.IsVertHidden(EdgeB(k)))
                groups.Join(EdgeA(k), EdgeB(k));
        std::unordered_map<uint32_t, std::pair<glm::vec3, int>> centers;
        std::vector<uint32_t> remap(m.VertexCount());
        for (uint32_t v = 0; v < m.VertexCount(); ++v)
        {
            remap[v] = v;
            if (!m.IsVertSelected(v) || m.IsVertHidden(v)) continue;
            remap[v] = groups.Find(v);
            auto& c = centers[remap[v]];
            c.first += m.Positions[v];
            ++c.second;
        }
        for (auto& [root, c] : centers) m.Positions[root] = c.first / (float)c.second;
        size_t before = m.VertexCount();
        RemapVertices(m, remap);
        return before - m.VertexCount();
    }

    size_t MergeByDistance(EditableMesh& m, float distance, bool selectedOnly)
    {
        uint32_t n = m.VertexCount();
        auto eligible = [&](uint32_t v) { return !m.IsVertHidden(v) && (!selectedOnly || m.IsVertSelected(v)); };
        SpatialHash hash(std::max(distance, 1e-7f));
        Groups groups(n);
        for (uint32_t v = 0; v < n; ++v)
        {
            if (!eligible(v)) continue;
            hash.Near(m.Positions[v], [&](uint32_t o) {
                if (glm::distance(m.Positions[v], m.Positions[o]) <= distance) groups.Join(v, o);
            });
            hash.Insert(m.Positions[v], v);
        }
        std::vector<uint32_t> remap(n);
        for (uint32_t v = 0; v < n; ++v) remap[v] = eligible(v) ? groups.Find(v) : v;
        size_t before = n;
        RemapVertices(m, remap);
        return before - m.VertexCount();
    }

    // ---- Normals and shading

    namespace
    {
        void FlipFace(Face& f)
        {
            f.Reverse();
        }

        // Whether face f walks from a to b (a directly followed by b).
        bool Walks(const Face& f, uint32_t a, uint32_t b)
        {
            for (size_t i = 0, n = f.Verts.size(); i < n; ++i)
                if (f.Verts[i] == a && f.Verts[(i + 1) % n] == b) return true;
            return false;
        }
    }

    size_t RepairColors(EditableMesh& m, const EditableMesh& before)
    {
        bool needed = false;
        for (const Face& f : m.Faces) needed |= f.Colors.size() != f.Verts.size();
        if (!needed || !before.HasColors()) return 0;
        // Each old vertex's color (its corners' average).
        size_t n = before.VertexCount();
        std::vector<glm::vec4> color(n, glm::vec4(0.0f));
        std::vector<float> count(n, 0.0f);
        for (const Face& f : before.Faces)
            for (size_t c = 0; c < f.Verts.size(); ++c)
            {
                color[f.Verts[c]] += UnpackRGBA8(f.ColorAt(c));
                count[f.Verts[c]] += 1.0f;
            }
        std::vector<uint32_t> withColor;
        for (uint32_t v = 0; v < n; ++v)
            if (count[v] > 0.0f)
            {
                color[v] /= count[v];
                withColor.push_back(v);
            }
        if (withColor.empty()) return 0;
        // Nearest old vertex for new ones: a grid over the old positions.
        AABB b = before.Bounds();
        float cell = std::max(glm::length(b.Max - b.Min) / 32.0f, 1e-5f);
        auto key = [&](const glm::vec3& p) {
            glm::ivec3 c = glm::ivec3(glm::floor((p - b.Min) / cell));
            return ((int64_t)c.x * 73856093ll) ^ ((int64_t)c.y * 19349663ll) ^ ((int64_t)c.z * 83492791ll);
        };
        std::unordered_map<int64_t, std::vector<uint32_t>> grid;
        for (uint32_t v : withColor) grid[key(before.Positions[v])].push_back(v);
        auto nearest = [&](const glm::vec3& p) {
            glm::ivec3 c = glm::ivec3(glm::floor((p - b.Min) / cell));
            uint32_t best = withColor[0];
            float bestD = 1e30f;
            for (int r = 1; r <= 4 && bestD > 1e29f; ++r)
                for (int dz = -r; dz <= r; ++dz)
                    for (int dy = -r; dy <= r; ++dy)
                        for (int dx = -r; dx <= r; ++dx)
                        {
                            glm::ivec3 q = c + glm::ivec3(dx, dy, dz);
                            int64_t k = ((int64_t)q.x * 73856093ll) ^ ((int64_t)q.y * 19349663ll) ^ ((int64_t)q.z * 83492791ll);
                            if (auto it = grid.find(k); it != grid.end())
                                for (uint32_t v : it->second)
                                    if (float d = glm::length(before.Positions[v] - p); d < bestD)
                                    {
                                        bestD = d;
                                        best = v;
                                    }
                        }
            if (bestD > 1e29f)
                for (uint32_t v : withColor)
                    if (float d = glm::length(before.Positions[v] - p); d < bestD)
                    {
                        bestD = d;
                        best = v;
                    }
            return best;
        };
        size_t repaired = 0;
        for (Face& f : m.Faces)
        {
            if (f.Colors.size() == f.Verts.size()) continue;
            f.Colors.resize(f.Verts.size());
            for (size_t c = 0; c < f.Verts.size(); ++c)
            {
                uint32_t v = f.Verts[c];
                bool same = v < n && count[v] > 0.0f && glm::length(before.Positions[v] - m.Positions[v]) < 1e-6f;
                f.Colors[c] = PackRGBA8(color[same ? v : nearest(m.Positions[v])]);
            }
            ++repaired;
        }
        return repaired;
    }

    void FlipNormals(EditableMesh& m, bool selectedOnly)
    {
        for (auto& f : m.Faces)
            if (!(f.Flags & Hidden) && (!selectedOnly || (f.Flags & Selected))) FlipFace(f);
        m.Changed();
    }

    void RecalculateNormals(EditableMesh& m, bool inside)
    {
        const Topology& t = m.Topo();
        uint32_t fc = m.FaceCount();
        std::vector<uint8_t> use(fc, 0);
        bool any = false;
        for (uint32_t f = 0; f < fc; ++f) any |= (m.Faces[f].Flags & Selected) && !FaceHidden(m, f);
        for (uint32_t f = 0; f < fc; ++f) use[f] = !FaceHidden(m, f) && (!any || (m.Faces[f].Flags & Selected));

        std::vector<uint8_t> visited(fc, 0), flip(fc, 0);
        for (uint32_t seed = 0; seed < fc; ++seed)
        {
            if (!use[seed] || visited[seed]) continue;
            // Spread the seed's winding over the part (across edges with exactly two faces).
            std::vector<uint32_t> part = { seed }, queue = { seed };
            visited[seed] = 1;
            while (!queue.empty())
            {
                uint32_t f = queue.back();
                queue.pop_back();
                const auto& verts = m.Faces[f].Verts;
                for (size_t i = 0; i < verts.size(); ++i)
                {
                    uint32_t a = verts[i], b = verts[(i + 1) % verts.size()];
                    if (flip[f]) std::swap(a, b); // as it will be once flipped
                    int32_t e = t.FindEdge(a, b);
                    if (e < 0 || t.EdgeFaces[e].size() != 2) continue;
                    uint32_t g = t.EdgeFaces[e][0] == f ? t.EdgeFaces[e][1] : t.EdgeFaces[e][0];
                    if (!use[g] || visited[g]) continue;
                    visited[g] = 1;
                    flip[g] = Walks(m.Faces[g], a, b); // must walk b -> a
                    part.push_back(g);
                    queue.push_back(g);
                }
            }
            // Outward: the part's signed volume about its center should be positive.
            glm::vec3 center(0.0f);
            size_t count = 0;
            for (uint32_t f : part)
                for (uint32_t v : m.Faces[f].Verts)
                {
                    center += m.Positions[v];
                    ++count;
                }
            center /= (float)std::max<size_t>(1, count);
            double volume = 0.0;
            std::vector<uint32_t> tri;
            for (uint32_t f : part)
            {
                tri.clear();
                m.TriangulateFace(f, tri);
                const auto& verts = m.Faces[f].Verts;
                for (size_t i = 0; i + 2 < tri.size(); i += 3)
                {
                    glm::vec3 a = m.Positions[verts[tri[i]]] - center, b = m.Positions[verts[tri[i + 1]]] - center, c = m.Positions[verts[tri[i + 2]]] - center;
                    double v = glm::dot(a, glm::cross(b, c));
                    volume += flip[f] ? -v : v;
                }
            }
            bool reverse = inside ? volume > 0.0 : volume < 0.0;
            if (reverse)
                for (uint32_t f : part) flip[f] ^= 1;
        }
        for (uint32_t f = 0; f < fc; ++f)
            if (flip[f]) FlipFace(m.Faces[f]);
        m.Changed();
    }

    void SetSmooth(EditableMesh& m, bool smooth, bool selectedOnly)
    {
        for (auto& f : m.Faces)
            if (!(f.Flags & Hidden) && (!selectedOnly || (f.Flags & Selected))) f.Smooth = smooth;
    }

    void MarkSharp(EditableMesh& m, bool sharp)
    {
        for (EdgeKey k : m.Topo().Edges)
            if (m.IsEdgeSelected(k)) m.EditEdge(k).Sharp = sharp;
        m.PruneEdgeAttrs();
    }

    void MarkSeam(EditableMesh& m, bool seam)
    {
        for (EdgeKey k : m.Topo().Edges)
            if (m.IsEdgeSelected(k)) m.EditEdge(k).Seam = seam;
        m.PruneEdgeAttrs();
    }

    void SetCrease(EditableMesh& m, float crease)
    {
        for (EdgeKey k : m.Topo().Edges)
            if (m.IsEdgeSelected(k)) m.EditEdge(k).Crease = std::clamp(crease, 0.0f, 1.0f);
        m.PruneEdgeAttrs();
    }

    // ---- Transform

    void TransformVertices(EditableMesh& m, const glm::mat4& transform, const std::vector<float>* weights)
    {
        for (uint32_t v = 0; v < m.VertexCount(); ++v)
        {
            if (m.IsVertHidden(v)) continue;
            float w = weights ? (v < weights->size() ? (*weights)[v] : 0.0f) : (m.IsVertSelected(v) ? 1.0f : 0.0f);
            if (w <= 0.0f) continue;
            glm::vec3 p = glm::vec3(transform * glm::vec4(m.Positions[v], 1.0f));
            m.Positions[v] = glm::mix(m.Positions[v], p, w);
        }
    }
}
