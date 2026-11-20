#include "Dimension/Modeling/FaceSets.h"
#include "Dimension/Modeling/MeshOps.h"
#include <cmath>
#include <numeric>

namespace Dimension::Modeling::FaceSets
{
    namespace
    {
        bool FaceHidden(const EditableMesh& m, uint32_t f) { return (m.Faces[f].Flags & Hidden) != 0; }

        uint32_t Find(std::vector<uint32_t>& parent, uint32_t x)
        {
            while (parent[x] != x)
            {
                parent[x] = parent[parent[x]];
                x = parent[x];
            }
            return x;
        }
    }

    int32_t Next(const EditableMesh& m)
    {
        int32_t best = 0;
        for (const Face& f : m.Faces) best = std::max(best, f.FaceSet);
        return best + 1;
    }

    std::vector<int32_t> Used(const EditableMesh& m)
    {
        std::vector<int32_t> out;
        for (const Face& f : m.Faces)
            if (f.FaceSet != 0) out.push_back(f.FaceSet);
        std::sort(out.begin(), out.end());
        out.erase(std::unique(out.begin(), out.end()), out.end());
        return out;
    }

    glm::vec3 Color(int32_t set, uint32_t seed)
    {
        if (set == 0) return glm::vec3(1.0f);
        // Hues a golden angle apart (neighbouring ids look different), saturation and value varied a little.
        uint32_t h = (uint32_t)set * 2654435761u ^ (seed * 40503u + seed);
        h ^= h >> 15;
        h *= 2246822519u;
        h ^= h >> 13;
        float hue = std::fmod((float)set * 0.61803398875f + (float)(seed % 1000u) * 0.1234567f, 1.0f);
        float sat = 0.45f + (float)(h & 0xFF) / 255.0f * 0.3f, val = 0.85f + (float)((h >> 8) & 0xFF) / 255.0f * 0.15f;
        glm::vec3 k = glm::clamp(glm::abs(glm::fract(glm::vec3(hue) + glm::vec3(1.0f, 2.0f / 3.0f, 1.0f / 3.0f)) * 6.0f - 3.0f) - 1.0f, 0.0f, 1.0f);
        return val * glm::mix(glm::vec3(1.0f), k, sat);
    }

    int32_t Assign(EditableMesh& m, const std::vector<uint32_t>& faces)
    {
        int32_t id = Next(m);
        bool any = false;
        for (uint32_t f : faces)
            if (f < m.FaceCount() && !FaceHidden(m, f))
            {
                m.Faces[f].FaceSet = id;
                any = true;
            }
        return any ? id : 0;
    }

    int32_t FromMasked(EditableMesh& m, float threshold)
    {
        if (m.Mask.size() != m.VertexCount()) return 0;
        std::vector<uint32_t> faces;
        for (uint32_t f = 0; f < m.FaceCount(); ++f)
        {
            bool all = !m.Faces[f].Verts.empty();
            for (uint32_t v : m.Faces[f].Verts) all &= m.Mask[v] >= threshold;
            if (all) faces.push_back(f);
        }
        return Assign(m, faces);
    }

    int32_t FromVisible(EditableMesh& m)
    {
        std::vector<uint32_t> faces(m.FaceCount());
        std::iota(faces.begin(), faces.end(), 0u);
        return Assign(m, faces);
    }

    int32_t FromSelection(EditableMesh& m)
    {
        std::vector<uint32_t> faces;
        for (uint32_t f = 0; f < m.FaceCount(); ++f)
            if (m.Faces[f].Flags & Selected) faces.push_back(f);
        return Assign(m, faces);
    }

    int InitFrom(EditableMesh& m, InitMode mode, float angle)
    {
        const Topology& t = m.Topo();
        std::vector<uint32_t> parent(m.FaceCount());
        std::iota(parent.begin(), parent.end(), 0u);
        float cosLimit = std::cos(glm::radians(angle));
        std::vector<glm::vec3> normals;
        if (mode == InitMode::Normals)
            for (uint32_t f = 0; f < m.FaceCount(); ++f) normals.push_back(m.FaceNormal(f));
        for (size_t e = 0; e < t.Edges.size(); ++e)
        {
            const auto& faces = t.EdgeFaces[e];
            if (faces.size() < 2) continue;
            EdgeData d = m.GetEdge(t.Edges[e]);
            if (mode == InitMode::UVSeams && d.Seam) continue;
            if (mode == InitMode::SharpEdges && d.Sharp) continue;
            for (size_t i = 1; i < faces.size(); ++i)
            {
                uint32_t a = faces[0], b = faces[i];
                if (mode == InitMode::Materials && m.Faces[a].Material != m.Faces[b].Material) continue;
                if (mode == InitMode::Normals && glm::dot(normals[a], normals[b]) < cosLimit) continue;
                uint32_t ra = Find(parent, a), rb = Find(parent, b);
                if (ra != rb) parent[rb] = ra;
            }
        }
        std::vector<int32_t> idOf(m.FaceCount(), 0);
        int32_t count = 0;
        for (uint32_t f = 0; f < m.FaceCount(); ++f)
        {
            uint32_t r = Find(parent, f);
            if (idOf[r] == 0) idOf[r] = ++count;
            m.Faces[f].FaceSet = idOf[r];
        }
        return count;
    }

    void Grow(EditableMesh& m, int32_t set)
    {
        const Topology& t = m.Topo();
        std::vector<uint8_t> inSet(m.VertexCount(), 0);
        for (const Face& f : m.Faces)
            if (f.FaceSet == set && !(f.Flags & Hidden))
                for (uint32_t v : f.Verts) inSet[v] = 1;
        for (uint32_t v = 0; v < m.VertexCount(); ++v)
            if (inSet[v])
                for (uint32_t f : t.VertFaces[v])
                    if (!FaceHidden(m, f)) m.Faces[f].FaceSet = set;
    }

    void Shrink(EditableMesh& m, int32_t set)
    {
        const Topology& t = m.Topo();
        std::vector<int32_t> next(m.FaceCount());
        for (uint32_t f = 0; f < m.FaceCount(); ++f) next[f] = m.Faces[f].FaceSet;
        for (uint32_t f = 0; f < m.FaceCount(); ++f)
        {
            if (m.Faces[f].FaceSet != set || FaceHidden(m, f)) continue;
            for (uint32_t v : m.Faces[f].Verts)
            {
                for (uint32_t g : t.VertFaces[v])
                    if (m.Faces[g].FaceSet != set && !FaceHidden(m, g))
                    {
                        next[f] = m.Faces[g].FaceSet;
                        break;
                    }
                if (next[f] != set) break;
            }
        }
        for (uint32_t f = 0; f < m.FaceCount(); ++f) m.Faces[f].FaceSet = next[f];
    }

    void SetFacesHidden(EditableMesh& m, const std::vector<uint8_t>& hidden)
    {
        const Topology& t = m.Topo();
        for (uint32_t f = 0; f < m.FaceCount(); ++f)
        {
            if (hidden[f]) m.Faces[f].Flags = (uint8_t)((m.Faces[f].Flags | Hidden) & ~Selected);
            else m.Faces[f].Flags &= (uint8_t)~Hidden;
        }
        std::vector<uint8_t> edgeHidden(t.Edges.size(), 0);
        for (size_t e = 0; e < t.Edges.size(); ++e)
        {
            const auto& faces = t.EdgeFaces[e];
            if (faces.empty()) continue; // (wire edges keep their own state)
            bool all = true;
            for (uint32_t f : faces) all &= hidden[f] != 0;
            edgeHidden[e] = all;
            m.SetEdgeFlag(t.Edges[e], Hidden, all);
            if (all) m.SetEdgeFlag(t.Edges[e], Selected, false);
        }
        for (uint32_t v = 0; v < m.VertexCount(); ++v)
        {
            if (t.VertFaces[v].empty()) continue;
            bool all = true;
            for (uint32_t f : t.VertFaces[v]) all &= hidden[f] != 0;
            if (all) m.VertFlags[v] = (uint8_t)((m.VertFlags[v] | Hidden) & ~Selected);
            else m.VertFlags[v] &= (uint8_t)~Hidden;
        }
        m.PruneEdgeAttrs();
    }

    bool AnyHidden(const EditableMesh& m)
    {
        for (const Face& f : m.Faces)
            if (f.Flags & Hidden) return true;
        return false;
    }

    void Isolate(EditableMesh& m, int32_t set)
    {
        std::vector<uint8_t> hidden(m.FaceCount());
        for (uint32_t f = 0; f < m.FaceCount(); ++f) hidden[f] = m.Faces[f].FaceSet != set;
        SetFacesHidden(m, hidden);
    }

    void ToggleIsolate(EditableMesh& m, int32_t set)
    {
        bool othersShow = false;
        for (const Face& f : m.Faces) othersShow |= f.FaceSet != set && !(f.Flags & Hidden);
        if (othersShow) Isolate(m, set);
        else RevealAll(m);
    }

    void Hide(EditableMesh& m, int32_t set)
    {
        std::vector<uint8_t> hidden(m.FaceCount());
        for (uint32_t f = 0; f < m.FaceCount(); ++f) hidden[f] = FaceHidden(m, f) || m.Faces[f].FaceSet == set;
        SetFacesHidden(m, hidden);
    }

    void RevealAll(EditableMesh& m) { Ops::RevealAll(m, false); }

    void MaskFromSet(EditableMesh& m, int32_t set, bool invert)
    {
        std::vector<uint8_t> inSet(m.VertexCount(), 0);
        for (const Face& f : m.Faces)
            if (f.FaceSet == set)
                for (uint32_t v : f.Verts) inSet[v] = 1;
        m.Mask.assign(m.VertexCount(), 0.0f);
        for (uint32_t v = 0; v < m.VertexCount(); ++v) m.Mask[v] = (inSet[v] != 0) == invert ? 1.0f : 0.0f;
    }
}
