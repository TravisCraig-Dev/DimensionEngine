#include "Dimension/Modeling/MeshRaycast.h"
#include <algorithm>

namespace Dimension::Modeling
{
    void MeshRaycaster::Build(const EditableMesh& mesh)
    {
        m_Tris.clear();
        m_Nodes.clear();
        std::vector<uint32_t> corners;
        for (uint32_t f = 0; f < mesh.FaceCount(); ++f)
        {
            if (mesh.Faces[f].Flags & Hidden) continue;
            corners.clear();
            mesh.TriangulateFace(f, corners);
            const auto& v = mesh.Faces[f].Verts;
            for (size_t i = 0; i + 2 < corners.size(); i += 3)
                m_Tris.push_back({ mesh.Positions[v[corners[i]]], mesh.Positions[v[corners[i + 1]]], mesh.Positions[v[corners[i + 2]]], f });
        }
        if (m_Tris.empty()) return;
        m_Nodes.reserve(m_Tris.size() * 2 / 3 + 1);
        m_Nodes.emplace_back();
        BuildNode(0, 0, (uint32_t)m_Tris.size());
    }

    void MeshRaycaster::BuildNode(uint32_t node, uint32_t first, uint32_t count)
    {
        glm::vec3 mn(1e30f), mx(-1e30f), cmn(1e30f), cmx(-1e30f);
        for (uint32_t i = first; i < first + count; ++i)
        {
            const Tri& t = m_Tris[i];
            mn = glm::min(mn, glm::min(t.A, glm::min(t.B, t.C)));
            mx = glm::max(mx, glm::max(t.A, glm::max(t.B, t.C)));
            glm::vec3 c = (t.A + t.B + t.C) / 3.0f;
            cmn = glm::min(cmn, c);
            cmx = glm::max(cmx, c);
        }
        m_Nodes[node].Min = mn;
        m_Nodes[node].Max = mx;
        if (count <= 4)
        {
            m_Nodes[node].Left = first;
            m_Nodes[node].Count = count;
            return;
        }
        // Median split along the centers' longest axis.
        glm::vec3 ext = cmx - cmn;
        int axis = ext.x > ext.y && ext.x > ext.z ? 0 : ext.y > ext.z ? 1 : 2;
        uint32_t half = count / 2;
        std::nth_element(m_Tris.begin() + first, m_Tris.begin() + first + half, m_Tris.begin() + first + count,
                         [axis](const Tri& a, const Tri& b) { return (a.A[axis] + a.B[axis] + a.C[axis]) < (b.A[axis] + b.B[axis] + b.C[axis]); });
        uint32_t left = (uint32_t)m_Nodes.size();
        m_Nodes.emplace_back();
        m_Nodes.emplace_back();
        m_Nodes[node].Left = left;
        m_Nodes[node].Count = 0;
        BuildNode(left, first, half);
        BuildNode(left + 1, first + half, count - half);
    }

    namespace
    {
        bool RayBox(const glm::vec3& o, const glm::vec3& inv, const glm::vec3& mn, const glm::vec3& mx, float maxT)
        {
            glm::vec3 t0 = (mn - o) * inv, t1 = (mx - o) * inv;
            glm::vec3 lo = glm::min(t0, t1), hi = glm::max(t0, t1);
            float enter = std::max({ lo.x, lo.y, lo.z, 0.0f }), exit = std::min({ hi.x, hi.y, hi.z, maxT });
            return enter <= exit;
        }

        // Möller-Trumbore, both sides.
        bool RayTri(const glm::vec3& o, const glm::vec3& d, const glm::vec3& a, const glm::vec3& b, const glm::vec3& c, float& t)
        {
            glm::vec3 e1 = b - a, e2 = c - a;
            glm::vec3 p = glm::cross(d, e2);
            float det = glm::dot(e1, p);
            if (std::abs(det) < 1e-12f) return false;
            float inv = 1.0f / det;
            glm::vec3 s = o - a;
            float u = glm::dot(s, p) * inv;
            if (u < 0.0f || u > 1.0f) return false;
            glm::vec3 q = glm::cross(s, e1);
            float v = glm::dot(d, q) * inv;
            if (v < 0.0f || u + v > 1.0f) return false;
            t = glm::dot(e2, q) * inv;
            return t > 0.0f;
        }
    }

    MeshRaycaster::RayHit MeshRaycaster::Raycast(const glm::vec3& origin, const glm::vec3& direction, float maxDistance) const
    {
        RayHit hit;
        if (m_Nodes.empty()) return hit;
        glm::vec3 dir = glm::normalize(direction);
        glm::vec3 inv(1.0f / (dir.x != 0.0f ? dir.x : 1e-30f), 1.0f / (dir.y != 0.0f ? dir.y : 1e-30f), 1.0f / (dir.z != 0.0f ? dir.z : 1e-30f));
        float best = maxDistance;
        uint32_t stack[64];
        int top = 0;
        stack[top++] = 0;
        while (top > 0)
        {
            const Node& n = m_Nodes[stack[--top]];
            if (!RayBox(origin, inv, n.Min, n.Max, best)) continue;
            if (n.Count > 0)
            {
                for (uint32_t i = n.Left; i < n.Left + n.Count; ++i)
                {
                    float t;
                    if (RayTri(origin, dir, m_Tris[i].A, m_Tris[i].B, m_Tris[i].C, t) && t < best)
                    {
                        best = t;
                        hit.Hit = true;
                        hit.Distance = t;
                        hit.Face = m_Tris[i].Face;
                    }
                }
            }
            else if (top + 2 <= 64)
            {
                stack[top++] = n.Left;
                stack[top++] = n.Left + 1;
            }
        }
        if (hit.Hit) hit.Point = origin + dir * hit.Distance;
        return hit;
    }

    bool MeshRaycaster::Occluded(const glm::vec3& from, const glm::vec3& to, float epsilon) const
    {
        glm::vec3 d = to - from;
        float len = glm::length(d);
        if (len <= epsilon) return false;
        return Raycast(from, d / len, len - epsilon * std::max(1.0f, len)).Hit;
    }
}
