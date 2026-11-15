#include "Dimension/Modeling/Retopo.h"
#include "Dimension/Modeling/Modifiers.h"
#include <algorithm>
#include <cmath>

namespace Dimension::Modeling
{
    namespace
    {
        // The point of triangle abc nearest p (Ericson, Real-Time Collision Detection 5.1.5).
        glm::vec3 ClosestOnTriangle(const glm::vec3& p, const glm::vec3& a, const glm::vec3& b, const glm::vec3& c)
        {
            glm::vec3 ab = b - a, ac = c - a, ap = p - a;
            float d1 = glm::dot(ab, ap), d2 = glm::dot(ac, ap);
            if (d1 <= 0.0f && d2 <= 0.0f) return a;
            glm::vec3 bp = p - b;
            float d3 = glm::dot(ab, bp), d4 = glm::dot(ac, bp);
            if (d3 >= 0.0f && d4 <= d3) return b;
            float vc = d1 * d4 - d3 * d2;
            if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) return a + ab * (d1 / (d1 - d3));
            glm::vec3 cp = p - c;
            float d5 = glm::dot(ab, cp), d6 = glm::dot(ac, cp);
            if (d6 >= 0.0f && d5 <= d6) return c;
            float vb = d5 * d2 - d1 * d6;
            if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) return a + ac * (d2 / (d2 - d6));
            float va = d3 * d6 - d5 * d4;
            if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f) return b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)));
            float denom = 1.0f / (va + vb + vc);
            return a + ab * (vb * denom) + ac * (vc * denom);
        }

        float BoxDistance2(const glm::vec3& p, const glm::vec3& mn, const glm::vec3& mx)
        {
            glm::vec3 d = glm::max(glm::max(mn - p, p - mx), glm::vec3(0.0f));
            return glm::dot(d, d);
        }
    }

    void TriangleTree::Build(std::vector<glm::vec3> positions, const std::vector<uint32_t>& indices)
    {
        m_Tris.clear();
        Add(positions, indices, glm::mat4(1.0f));
        Finish();
    }

    void TriangleTree::Build(const EditableMesh& mesh, const glm::mat4& transform)
    {
        std::vector<uint32_t> indices, tri;
        for (uint32_t f = 0; f < mesh.FaceCount(); ++f)
        {
            tri.clear();
            mesh.TriangulateFace(f, tri);
            for (uint32_t c : tri) indices.push_back(mesh.Faces[f].Verts[c]);
        }
        m_Tris.clear();
        Add(mesh.Positions, indices, transform);
        Finish();
    }

    void TriangleTree::Add(const std::vector<glm::vec3>& positions, const std::vector<uint32_t>& indices, const glm::mat4& transform)
    {
        for (size_t i = 0; i + 2 < indices.size(); i += 3)
        {
            if (indices[i] >= positions.size() || indices[i + 1] >= positions.size() || indices[i + 2] >= positions.size()) continue;
            Tri t{ glm::vec3(transform * glm::vec4(positions[indices[i]], 1.0f)), glm::vec3(transform * glm::vec4(positions[indices[i + 1]], 1.0f)),
                   glm::vec3(transform * glm::vec4(positions[indices[i + 2]], 1.0f)) };
            if (glm::length(glm::cross(t.B - t.A, t.C - t.A)) > 1e-14f) m_Tris.push_back(t);
        }
    }

    void TriangleTree::Finish()
    {
        m_Nodes.clear();
        if (m_Tris.empty()) return;
        m_Nodes.push_back({});
        BuildNode(0, 0, (uint32_t)m_Tris.size());
    }

    void TriangleTree::BuildNode(uint32_t node, uint32_t first, uint32_t count)
    {
        glm::vec3 mn(1e30f), mx(-1e30f), cmn(1e30f), cmx(-1e30f);
        for (uint32_t i = first; i < first + count; ++i)
        {
            const Tri& t = m_Tris[i];
            for (const glm::vec3& p : { t.A, t.B, t.C })
            {
                mn = glm::min(mn, p);
                mx = glm::max(mx, p);
            }
            glm::vec3 c = (t.A + t.B + t.C) / 3.0f;
            cmn = glm::min(cmn, c);
            cmx = glm::max(cmx, c);
        }
        m_Nodes[node].Min = mn - glm::vec3(1e-6f);
        m_Nodes[node].Max = mx + glm::vec3(1e-6f);
        if (count <= 4)
        {
            m_Nodes[node].Left = first;
            m_Nodes[node].Count = count;
            return;
        }
        glm::vec3 ext = cmx - cmn;
        int axis = ext.x >= ext.y && ext.x >= ext.z ? 0 : ext.y >= ext.z ? 1 : 2;
        uint32_t mid = first + count / 2;
        std::nth_element(m_Tris.begin() + first, m_Tris.begin() + mid, m_Tris.begin() + first + count,
                         [&](const Tri& a, const Tri& b) { return (a.A[axis] + a.B[axis] + a.C[axis]) < (b.A[axis] + b.B[axis] + b.C[axis]); });
        uint32_t left = (uint32_t)m_Nodes.size();
        m_Nodes.push_back({});
        m_Nodes.push_back({});
        m_Nodes[node].Left = left;
        m_Nodes[node].Count = 0;
        BuildNode(left, first, mid - first);
        BuildNode(left + 1, mid, first + count - mid);
    }

    TriangleTree::Hit TriangleTree::Raycast(const glm::vec3& o, const glm::vec3& d, float maxDistance) const
    {
        Hit hit;
        if (m_Nodes.empty()) return hit;
        glm::vec3 inv(1.0f / (std::abs(d.x) > 1e-12f ? d.x : 1e-12f), 1.0f / (std::abs(d.y) > 1e-12f ? d.y : 1e-12f), 1.0f / (std::abs(d.z) > 1e-12f ? d.z : 1e-12f));
        float best = maxDistance;
        uint32_t stack[64];
        int sp = 0;
        stack[sp++] = 0;
        while (sp > 0)
        {
            const Node& n = m_Nodes[stack[--sp]];
            glm::vec3 t0 = (n.Min - o) * inv, t1 = (n.Max - o) * inv;
            float tmin = std::max({ std::min(t0.x, t1.x), std::min(t0.y, t1.y), std::min(t0.z, t1.z), 0.0f });
            float tmax = std::min({ std::max(t0.x, t1.x), std::max(t0.y, t1.y), std::max(t0.z, t1.z), best });
            if (tmin > tmax) continue;
            if (n.Count == 0)
            {
                if (sp + 2 <= 64)
                {
                    stack[sp++] = n.Left;
                    stack[sp++] = n.Left + 1;
                }
                continue;
            }
            for (uint32_t i = n.Left; i < n.Left + n.Count; ++i)
            {
                const Tri& t = m_Tris[i];
                glm::vec3 e1 = t.B - t.A, e2 = t.C - t.A, p = glm::cross(d, e2);
                float det = glm::dot(e1, p);
                if (std::abs(det) < 1e-14f) continue;
                float invDet = 1.0f / det;
                glm::vec3 s = o - t.A;
                float u = glm::dot(s, p) * invDet;
                if (u < 0.0f || u > 1.0f) continue;
                glm::vec3 q = glm::cross(s, e1);
                float v = glm::dot(d, q) * invDet;
                if (v < 0.0f || u + v > 1.0f) continue;
                float tt = glm::dot(e2, q) * invDet;
                if (tt <= 0.0f || tt >= best) continue;
                best = tt;
                hit.Found = true;
                hit.Distance = tt;
                hit.Point = o + d * tt;
                hit.Normal = glm::normalize(glm::cross(e1, e2));
            }
        }
        return hit;
    }

    TriangleTree::Hit TriangleTree::Closest(const glm::vec3& p) const
    {
        Hit hit;
        if (m_Nodes.empty()) return hit;
        float best2 = 1e30f;
        uint32_t stack[64];
        int sp = 0;
        stack[sp++] = 0;
        while (sp > 0)
        {
            const Node& n = m_Nodes[stack[--sp]];
            if (BoxDistance2(p, n.Min, n.Max) >= best2) continue;
            if (n.Count == 0)
            {
                if (sp + 2 > 64) continue;
                // The nearer child last (searched first).
                const Node &a = m_Nodes[n.Left], &b = m_Nodes[n.Left + 1];
                bool aFirst = BoxDistance2(p, a.Min, a.Max) <= BoxDistance2(p, b.Min, b.Max);
                stack[sp++] = aFirst ? n.Left + 1 : n.Left;
                stack[sp++] = aFirst ? n.Left : n.Left + 1;
                continue;
            }
            for (uint32_t i = n.Left; i < n.Left + n.Count; ++i)
            {
                const Tri& t = m_Tris[i];
                glm::vec3 q = ClosestOnTriangle(p, t.A, t.B, t.C);
                float d2 = glm::dot(q - p, q - p);
                if (d2 < best2)
                {
                    best2 = d2;
                    hit.Found = true;
                    hit.Point = q;
                    hit.Normal = glm::normalize(glm::cross(t.B - t.A, t.C - t.A));
                }
            }
        }
        hit.Distance = std::sqrt(best2);
        return hit;
    }

    size_t Retopo::Shrinkwrap(EditableMesh& m, const TriangleTree& target, float offset, WrapMode mode, const std::vector<uint8_t>* only)
    {
        if (target.Empty()) return 0;
        std::vector<glm::vec3> normals = mode == WrapMode::Project ? ModifierOps::VertexNormals(m) : std::vector<glm::vec3>();
        size_t moved = 0;
        for (uint32_t v = 0; v < m.VertexCount(); ++v)
        {
            if (only && (v >= only->size() || !(*only)[v])) continue;
            glm::vec3 p = m.Positions[v];
            TriangleTree::Hit h;
            if (mode == WrapMode::Project && glm::length(normals[v]) > 1e-12f)
            {
                TriangleTree::Hit a = target.Raycast(p, normals[v]), b = target.Raycast(p, -normals[v]);
                h = a.Found && (!b.Found || a.Distance <= b.Distance) ? a : b;
            }
            else
                h = target.Closest(p);
            if (!h.Found) continue;
            m.Positions[v] = h.Point + h.Normal * offset;
            ++moved;
        }
        return moved;
    }
}
