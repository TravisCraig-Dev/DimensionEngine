#include "Dimension/Modeling/Sculpt.h"
#include "Dimension/Core/Log.h"
#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace Dimension::Modeling::Sculpt
{
    namespace
    {
        constexpr uint32_t kDead = ~0u;
        constexpr float kDrawRate = 0.03f; // of the radius per dab at full strength

        // 1 at the center, 0 at the edge, smooth both ends.
        float Falloff(float t)
        {
            if (t >= 1.0f) return 0.0f;
            return 1.0f - t * t * (3.0f - 2.0f * t);
        }

        glm::vec3 TriNormal(const glm::vec3& a, const glm::vec3& b, const glm::vec3& c) { return glm::cross(b - a, c - a); }

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
    }

    const char* BrushName(Brush b)
    {
        static const char* names[] = { "Draw", "Clay", "Inflate", "Smooth", "Flatten", "Scrape", "Pinch", "Crease", "Grab", "Snake Hook", "Mask" };
        return names[std::clamp((int)b, 0, (int)Brush::Count - 1)];
    }

    // ---- Building and writing back

    SculptMesh SculptMesh::FromEditable(const EditableMesh& m)
    {
        SculptMesh s;
        size_t n = m.VertexCount();
        s.Positions = m.Positions;
        s.Normals.assign(n, glm::vec3(0.0f));
        s.UVs.assign(n, glm::vec2(0.0f));
        s.Mask = m.Mask.size() == n ? m.Mask : std::vector<float>(n, 0.0f);
        s.VertTris.resize(n);
        std::vector<uint8_t> hasUV(n, 0);
        std::vector<uint32_t> tri;
        for (uint32_t f = 0; f < m.FaceCount(); ++f)
        {
            const Face& face = m.Faces[f];
            tri.clear();
            m.TriangulateFace(f, tri);
            for (size_t k = 0; k + 2 < tri.size(); k += 3)
            {
                uint32_t a = face.Verts[tri[k]], b = face.Verts[tri[k + 1]], c = face.Verts[tri[k + 2]];
                if (a == b || b == c || a == c) continue;
                s.AddTri(a, b, c, face.Material);
            }
            for (size_t c = 0; c < face.Verts.size(); ++c)
                if (!hasUV[face.Verts[c]] && c < face.UVs.size())
                {
                    s.UVs[face.Verts[c]] = face.UVs[c];
                    hasUV[face.Verts[c]] = 1;
                }
        }
        s.UpdateNormals();
        s.m_TopologyChanged = false;
        return s;
    }

    void SculptMesh::WriteTo(EditableMesh& m) const
    {
        bool anyMask = std::any_of(Mask.begin(), Mask.end(), [](float v) { return v > 0.0f; });
        if (!m_TopologyChanged && m.VertexCount() == Positions.size())
        {
            m.Positions = Positions;
            if (anyMask) m.Mask = Mask;
            else m.Mask.clear();
            return;
        }
        EditableMesh out;
        out.Name = m.Name;
        out.MaterialSlots = m.MaterialSlots;
        out.AutoSmooth = m.AutoSmooth;
        out.AutoSmoothAngle = m.AutoSmoothAngle;
        out.Modifiers = m.Modifiers;
        std::vector<int64_t> index(Positions.size(), -1);
        for (size_t t = 0; t < Tris.size(); ++t)
        {
            if (IsDead((uint32_t)t)) continue;
            uint32_t v[3] = { Tris[t].x, Tris[t].y, Tris[t].z };
            for (uint32_t& x : v)
            {
                if (index[x] < 0)
                {
                    index[x] = out.AddVertex(Positions[x]);
                    out.Mask.push_back(Mask[x]);
                }
                x = (uint32_t)index[x];
            }
            out.AddFace({ v[0], v[1], v[2] }, {}, TriMaterial[t], true);
        }
        if (!anyMask) out.Mask.clear();
        out.Changed();
        m = std::move(out);
    }

    MeshData SculptMesh::ToMeshData(size_t materialSlots) const
    {
        MeshData out;
        std::vector<int64_t> index(Positions.size(), -1);
        size_t slots = std::max<size_t>(materialSlots, 1);
        std::vector<std::vector<uint32_t>> bySlot(slots);
        for (size_t t = 0; t < Tris.size(); ++t)
        {
            if (IsDead((uint32_t)t)) continue;
            size_t slot = (size_t)std::clamp<int32_t>(TriMaterial[t], 0, (int32_t)slots - 1);
            for (uint32_t v : { Tris[t].x, Tris[t].y, Tris[t].z })
            {
                if (index[v] < 0)
                {
                    index[v] = (int64_t)out.Vertices.size();
                    Vertex vx;
                    vx.Position = Positions[v];
                    vx.Normal = Normals[v];
                    vx.UV = UVs[v];
                    out.Vertices.push_back(vx);
                }
                bySlot[slot].push_back((uint32_t)index[v]);
            }
        }
        for (size_t s = 0; s < slots; ++s)
        {
            if (bySlot[s].empty() && slots > 1) continue;
            SubMesh sm;
            sm.IndexOffset = (uint32_t)out.Indices.size();
            sm.IndexCount = (uint32_t)bySlot[s].size();
            sm.MaterialSlot = materialSlots == 0 ? -1 : (int32_t)s;
            out.Indices.insert(out.Indices.end(), bySlot[s].begin(), bySlot[s].end());
            out.SubMeshes.push_back(sm);
        }
        return out;
    }

    size_t SculptMesh::LiveVertexCount() const
    {
        size_t n = 0;
        for (const auto& t : VertTris) n += t.empty() ? 0 : 1;
        return n;
    }

    size_t SculptMesh::LiveTriangleCount() const
    {
        size_t n = 0;
        for (size_t t = 0; t < Tris.size(); ++t) n += IsDead((uint32_t)t) ? 0 : 1;
        return n;
    }

    // ---- Topology bookkeeping

    uint32_t SculptMesh::AddVertex(const glm::vec3& p, const glm::vec3& n, const glm::vec2& uv, float mask)
    {
        Positions.push_back(p);
        Normals.push_back(n);
        UVs.push_back(uv);
        Mask.push_back(mask);
        VertTris.emplace_back();
        return (uint32_t)Positions.size() - 1;
    }

    uint32_t SculptMesh::AddTri(uint32_t a, uint32_t b, uint32_t c, int32_t material)
    {
        uint32_t t = (uint32_t)Tris.size();
        Tris.push_back({ a, b, c });
        TriMaterial.push_back(material);
        VertTris[a].push_back(t);
        VertTris[b].push_back(t);
        VertTris[c].push_back(t);
        return t;
    }

    void SculptMesh::KillTri(uint32_t t)
    {
        for (uint32_t v : { Tris[t].x, Tris[t].y, Tris[t].z })
        {
            auto& list = VertTris[v];
            list.erase(std::remove(list.begin(), list.end(), t), list.end());
        }
        Tris[t] = glm::uvec3(kDead);
    }

    std::vector<uint32_t> SculptMesh::Neighbors(uint32_t v) const
    {
        std::vector<uint32_t> out;
        for (uint32_t t : VertTris[v])
            for (uint32_t u : { Tris[t].x, Tris[t].y, Tris[t].z })
                if (u != v && std::find(out.begin(), out.end(), u) == out.end()) out.push_back(u);
        return out;
    }

    bool SculptMesh::IsBorder(uint32_t v) const
    {
        for (uint32_t u : Neighbors(v))
        {
            int shared = 0;
            for (uint32_t t : VertTris[v])
                if (Tris[t].x == u || Tris[t].y == u || Tris[t].z == u) ++shared;
            if (shared != 2) return true;
        }
        return false;
    }

    void SculptMesh::UpdateNormals()
    {
        std::vector<uint32_t> all(Positions.size());
        for (uint32_t v = 0; v < all.size(); ++v) all[v] = v;
        UpdateNormals(all);
    }

    void SculptMesh::UpdateNormals(const std::vector<uint32_t>& verts)
    {
        for (uint32_t v : verts)
        {
            glm::vec3 n(0.0f);
            for (uint32_t t : VertTris[v]) n += TriNormal(Positions[Tris[t].x], Positions[Tris[t].y], Positions[Tris[t].z]);
            float l = glm::length(n);
            if (l > 1e-20f) Normals[v] = n / l;
        }
    }

    // ---- Picking

    bool SculptMesh::Raycast(const glm::vec3& from, const glm::vec3& dir, float& tHit, glm::vec3& normal) const
    {
        float best = 1e30f;
        bool hit = false;
        const float ox = from.x, oy = from.y, oz = from.z, dx = dir.x, dy = dir.y, dz = dir.z;
        for (size_t t = 0; t < Tris.size(); ++t)
        {
            const glm::uvec3& tri = Tris[t];
            if (tri.x == kDead) continue;
            const glm::vec3 &a = Positions[tri.x], &b = Positions[tri.y], &c = Positions[tri.z];
            // Moller-Trumbore in plain floats.
            float e1x = b.x - a.x, e1y = b.y - a.y, e1z = b.z - a.z, e2x = c.x - a.x, e2y = c.y - a.y, e2z = c.z - a.z;
            float px = dy * e2z - dz * e2y, py = dz * e2x - dx * e2z, pz = dx * e2y - dy * e2x;
            float det = e1x * px + e1y * py + e1z * pz;
            if (det > -1e-12f && det < 1e-12f) continue;
            float inv = 1.0f / det;
            float sx = ox - a.x, sy = oy - a.y, sz = oz - a.z;
            float u = (sx * px + sy * py + sz * pz) * inv;
            if (u < 0.0f || u > 1.0f) continue;
            float qx = sy * e1z - sz * e1y, qy = sz * e1x - sx * e1z, qz = sx * e1y - sy * e1x;
            float v = (dx * qx + dy * qy + dz * qz) * inv;
            if (v < 0.0f || u + v > 1.0f) continue;
            float d = (e2x * qx + e2y * qy + e2z * qz) * inv;
            if (d <= 0.0f || d >= best) continue;
            best = d;
            normal = glm::vec3(e1y * e2z - e1z * e2y, e1z * e2x - e1x * e2z, e1x * e2y - e1y * e2x);
            hit = true;
        }
        if (hit)
        {
            tHit = best;
            normal = glm::normalize(normal);
        }
        return hit;
    }

    // ---- Strokes

    namespace
    {
        // The mirror copies of a brush for the symmetry bits: sign vectors.
        std::vector<glm::vec3> Mirrors(uint8_t symmetry)
        {
            std::vector<glm::vec3> out;
            for (int bits = 0; bits < 8; ++bits)
            {
                if ((bits & ~symmetry) != 0) continue;
                out.push_back(glm::vec3(bits & 1 ? -1.0f : 1.0f, bits & 2 ? -1.0f : 1.0f, bits & 4 ? -1.0f : 1.0f));
            }
            return out;
        }
    }

    void SculptMesh::BeginStroke(const glm::vec3& center, const BrushSettings& brush)
    {
        m_Caught.clear();
        if (brush.Type != Brush::Grab) return;
        float r2 = brush.Radius * brush.Radius;
        for (const glm::vec3& s : Mirrors(brush.Symmetry))
        {
            glm::vec3 c = center * s, view = brush.ViewDir * s;
            for (uint32_t v = 0; v < Positions.size(); ++v)
            {
                if (VertTris[v].empty()) continue;
                glm::vec3 d = Positions[v] - c;
                float d2 = glm::dot(d, d);
                if (d2 > r2) continue;
                if (brush.FrontFacesOnly && glm::dot(Normals[v], view) > 0.0f) continue;
                float w = Falloff(std::sqrt(d2) / brush.Radius) * (1.0f - Mask[v]);
                if (w <= 0.0f) continue;
                bool already = false;
                for (const Caught& k : m_Caught)
                    if (k.Vertex == v) already = true; // (on a symmetry plane: caught once)
                if (!already) m_Caught.push_back({ v, w, Positions[v], s });
            }
        }
    }

    void SculptMesh::EndStroke() { m_Caught.clear(); }

    void SculptMesh::Dab(const glm::vec3& center, const BrushSettings& brush, const glm::vec3& move)
    {
        if (brush.Type == Brush::Grab)
        {
            std::vector<uint32_t> touched;
            for (const Caught& k : m_Caught)
            {
                Positions[k.Vertex] = k.Start + move * k.Mirror * k.Weight * std::min(1.0f, brush.Strength * 2.0f);
                touched.push_back(k.Vertex);
            }
            std::vector<uint32_t> ring = touched;
            for (uint32_t v : touched)
                for (uint32_t u : Neighbors(v)) ring.push_back(u);
            std::sort(ring.begin(), ring.end());
            ring.erase(std::unique(ring.begin(), ring.end()), ring.end());
            UpdateNormals(ring);
            return;
        }
        std::vector<glm::vec3> mirrors = Mirrors(brush.Symmetry);
        // The detail first (so even a big flat face has vertices under the brush), then the brush.
        if (DetailSize > 0.0f && brush.Type != Brush::Mask)
            for (const glm::vec3& s : mirrors) Retopologize(center * s, brush.Radius);
        for (const glm::vec3& s : mirrors) ApplyBrush(center * s, brush, move * s, s);
    }

    void SculptMesh::ApplyBrush(const glm::vec3& c, const BrushSettings& brush, const glm::vec3& move, const glm::vec3& mirror)
    {
        float r = std::max(brush.Radius, 1e-6f), r2 = r * r;
        glm::vec3 view = brush.ViewDir * mirror;
        std::vector<std::pair<uint32_t, float>> hit; // vertex, falloff
        glm::vec3 areaN(0.0f), areaC(0.0f);
        float areaW = 0.0f;
        for (uint32_t v = 0; v < Positions.size(); ++v)
        {
            if (VertTris[v].empty()) continue;
            const glm::vec3& p = Positions[v];
            float dx = p.x - c.x, dy = p.y - c.y, dz = p.z - c.z, d2 = dx * dx + dy * dy + dz * dz;
            if (d2 > r2) continue;
            if (brush.FrontFacesOnly && glm::dot(Normals[v], view) > 0.0f) continue;
            float f = Falloff(std::sqrt(d2) / r);
            if (f <= 0.0f) continue;
            hit.push_back({ v, f });
            areaN += Normals[v] * f;
            areaC += p * f;
            areaW += f;
        }
        if (hit.empty()) return;
        areaC /= areaW;
        areaN = glm::length(areaN) > 1e-12f ? glm::normalize(areaN) : -view;
        float sign = brush.Invert ? -1.0f : 1.0f, str = std::clamp(brush.Strength, 0.0f, 1.0f);
        std::vector<glm::vec3> next(hit.size());
        for (size_t i = 0; i < hit.size(); ++i)
        {
            uint32_t v = hit[i].first;
            float f = hit[i].second;
            glm::vec3 p = Positions[v];
            float w = f * (1.0f - Mask[v]);
            switch (brush.Type)
            {
            case Brush::Draw: p += areaN * (sign * w * r * kDrawRate * str); break;
            case Brush::Inflate: p += Normals[v] * (sign * w * r * kDrawRate * str); break;
            case Brush::Smooth:
            {
                glm::vec3 avg(0.0f);
                std::vector<uint32_t> nb = Neighbors(v);
                for (uint32_t u : nb) avg += Positions[u];
                if (!nb.empty()) p += (avg / (float)nb.size() - p) * std::min(1.0f, w * str);
                break;
            }
            case Brush::Flatten:
            {
                float d = glm::dot(p - areaC, areaN);
                p -= areaN * (d * w * str * 0.5f);
                break;
            }
            case Brush::Scrape:
            {
                float d = glm::dot(p - areaC, areaN);
                if (sign * d > 0.0f) p -= areaN * (d * w * str * 0.5f);
                break;
            }
            case Brush::Clay:
            {
                // A plane a little above (below with Invert) the surface; what lies under it rises to it.
                glm::vec3 plane = areaC + areaN * (sign * r * kDrawRate * 2.0f * str);
                float d = glm::dot(p - plane, areaN);
                if (sign * d < 0.0f) p -= areaN * (d * w * str * 0.5f);
                break;
            }
            case Brush::Pinch:
            {
                glm::vec3 to = c - p;
                to -= areaN * glm::dot(to, areaN);
                p += to * (sign * w * str * 0.3f);
                break;
            }
            case Brush::Crease:
            {
                glm::vec3 to = c - p;
                to -= areaN * glm::dot(to, areaN);
                p += to * (w * str * 0.25f) - areaN * (sign * w * r * kDrawRate * str);
                break;
            }
            case Brush::SnakeHook: p += move * (w * std::min(1.0f, str * 2.0f)); break;
            case Brush::Mask: Mask[v] = std::clamp(Mask[v] + sign * f * str * 0.5f, 0.0f, 1.0f); break;
            default: break;
            }
            next[i] = p;
        }
        if (brush.Type == Brush::Mask) return;
        // Vertices on a symmetry plane stay on it (both halves pull them the same way).
        for (size_t i = 0; i < hit.size(); ++i)
        {
            glm::vec3 p = next[i];
            const glm::vec3& old = Positions[hit[i].first];
            for (int a = 0; a < 3; ++a)
                if ((brush.Symmetry >> a) & 1)
                    if (std::abs(old[a]) < r * 1e-4f) p[a] = old[a];
            Positions[hit[i].first] = p;
        }
        std::vector<uint32_t> ring;
        for (auto& [v, f] : hit)
        {
            ring.push_back(v);
            for (uint32_t u : Neighbors(v)) ring.push_back(u);
        }
        std::sort(ring.begin(), ring.end());
        ring.erase(std::unique(ring.begin(), ring.end()), ring.end());
        UpdateNormals(ring);
    }

    // ---- Dynamic topology

    bool SculptMesh::Split(uint32_t a, uint32_t b)
    {
        std::vector<uint32_t> shared;
        for (uint32_t t : VertTris[a])
            if (Tris[t].x == b || Tris[t].y == b || Tris[t].z == b) shared.push_back(t);
        if (shared.empty()) return false;
        glm::vec3 n = Normals[a] + Normals[b];
        uint32_t m = AddVertex((Positions[a] + Positions[b]) * 0.5f, glm::length(n) > 1e-12f ? glm::normalize(n) : Normals[a],
                               (UVs[a] + UVs[b]) * 0.5f, (Mask[a] + Mask[b]) * 0.5f);
        for (uint32_t t : shared)
        {
            glm::uvec3 tri = Tris[t];
            uint32_t v[3] = { tri.x, tri.y, tri.z };
            int i = 0;
            for (; i < 3; ++i)
                if ((v[i] == a && v[(i + 1) % 3] == b) || (v[i] == b && v[(i + 1) % 3] == a)) break;
            if (i == 3) continue;
            uint32_t x = v[i], y = v[(i + 1) % 3], z = v[(i + 2) % 3];
            int32_t mat = TriMaterial[t];
            KillTri(t);
            AddTri(x, m, z, mat);
            AddTri(m, y, z, mat);
        }
        m_TopologyChanged = true;
        return true;
    }

    bool SculptMesh::Collapse(uint32_t a, uint32_t b, float maxLen)
    {
        if (VertTris[a].empty() || VertTris[b].empty()) return false;
        std::vector<uint32_t> shared;
        for (uint32_t t : VertTris[a])
            if (Tris[t].x == b || Tris[t].y == b || Tris[t].z == b) shared.push_back(t);
        if (shared.size() != 2 || IsBorder(a) || IsBorder(b)) return false;
        std::vector<uint32_t> na = Neighbors(a), nb = Neighbors(b), common;
        for (uint32_t u : na)
            if (std::find(nb.begin(), nb.end(), u) != nb.end()) common.push_back(u);
        if (common.size() != 2) return false; // the link condition: else the surface would pinch
        for (uint32_t c : common)
            if (Neighbors(c).size() <= 3) return false;
        glm::vec3 mid = (Positions[a] + Positions[b]) * 0.5f;
        for (uint32_t u : nb)
            if (u != a && glm::length(Positions[u] - mid) > maxLen) return false; // (would make an edge to split again)
        // No triangle may turn over or flatten to nothing.
        auto check = [&](uint32_t v) {
            for (uint32_t t : VertTris[v])
            {
                if (std::find(shared.begin(), shared.end(), t) != shared.end()) continue;
                glm::vec3 p[3], q[3];
                uint32_t id[3] = { Tris[t].x, Tris[t].y, Tris[t].z };
                for (int k = 0; k < 3; ++k)
                {
                    p[k] = Positions[id[k]];
                    q[k] = id[k] == a || id[k] == b ? mid : p[k];
                }
                glm::vec3 n0 = TriNormal(p[0], p[1], p[2]), n1 = TriNormal(q[0], q[1], q[2]);
                if (glm::dot(n0, n1) <= 0.0f || glm::length(n1) < glm::length(n0) * 1e-3f) return false;
            }
            return true;
        };
        if (!check(a) || !check(b)) return false;
        for (uint32_t t : shared) KillTri(t);
        std::vector<uint32_t> moving = VertTris[b];
        for (uint32_t t : moving)
        {
            glm::uvec3& tri = Tris[t];
            for (int k = 0; k < 3; ++k)
                if (tri[k] == b) tri[k] = a;
            VertTris[a].push_back(t);
        }
        VertTris[b].clear();
        Positions[a] = mid;
        Mask[a] = (Mask[a] + Mask[b]) * 0.5f;
        UVs[a] = (UVs[a] + UVs[b]) * 0.5f;
        m_TopologyChanged = true;
        return true;
    }

    void SculptMesh::Retopologize(const glm::vec3& center, float radius)
    {
        float maxLen = DetailSize, minLen = DetailSize * 0.4f;
        if (maxLen <= 0.0f) return;
        float reach = radius * 1.1f;
        float reach2 = reach < 1e18f ? reach * reach : 1e36f;
        auto regionEdges = [&](bool longOnes) {
            std::vector<std::pair<float, std::pair<uint32_t, uint32_t>>> edges;
            // The triangles the brush's sphere touches.
            for (uint32_t t = 0; t < Tris.size(); ++t)
            {
                if (IsDead(t)) continue;
                const glm::vec3 &a = Positions[Tris[t].x], &b = Positions[Tris[t].y], &c = Positions[Tris[t].z];
                if (reach2 < 1e35f)
                {
                    glm::vec3 lo = glm::min(a, glm::min(b, c)), hi = glm::max(a, glm::max(b, c));
                    glm::vec3 q = glm::clamp(center, lo, hi) - center;
                    if (glm::dot(q, q) > reach2) continue;
                    glm::vec3 d = ClosestOnTriangle(center, a, b, c) - center;
                    if (glm::dot(d, d) > reach2) continue;
                }
                for (int k = 0; k < 3; ++k)
                {
                    uint32_t x = std::min(Tris[t][k], Tris[t][(k + 1) % 3]), y = std::max(Tris[t][k], Tris[t][(k + 1) % 3]);
                    float len = glm::length(Positions[x] - Positions[y]);
                    if (longOnes ? len > maxLen : len < minLen) edges.push_back({ len, { x, y } });
                }
            }
            // Each edge once (two triangles share it), longest (or shortest) first.
            std::sort(edges.begin(), edges.end(), [](auto& p, auto& q) { return p.second < q.second; });
            edges.erase(std::unique(edges.begin(), edges.end(), [](auto& p, auto& q) { return p.second == q.second; }), edges.end());
            std::sort(edges.begin(), edges.end(), [&](auto& p, auto& q) { return longOnes ? p.first > q.first : p.first < q.first; });
            return edges;
        };
        size_t budget = 20000;
        for (int pass = 0; pass < 6 && budget; ++pass)
        {
            auto edges = regionEdges(true);
            if (edges.empty()) break;
            for (auto& [len, e] : edges)
            {
                if (!budget) break;
                if (glm::length(Positions[e.first] - Positions[e.second]) <= maxLen) continue;
                if (Split(e.first, e.second)) --budget;
            }
        }
        for (int pass = 0; pass < 2; ++pass)
        {
            auto edges = regionEdges(false);
            if (edges.empty()) break;
            for (auto& [len, e] : edges)
                if (!VertTris[e.first].empty() && !VertTris[e.second].empty() && glm::length(Positions[e.first] - Positions[e.second]) < minLen)
                    Collapse(e.first, e.second, maxLen);
        }
        // Normals of everything touched (new vertices included).
        std::vector<uint32_t> near;
        for (uint32_t v = 0; v < Positions.size(); ++v)
        {
            if (VertTris[v].empty()) continue;
            glm::vec3 d = Positions[v] - center;
            if (glm::dot(d, d) <= reach2 * 1.3f) near.push_back(v);
        }
        UpdateNormals(near);
    }

    bool SculptMesh::FloodFillDetail(float detail, int maxPasses)
    {
        float saved = DetailSize;
        DetailSize = detail;
        bool settled = false;
        for (int pass = 0; pass < maxPasses && !settled; ++pass)
        {
            size_t before = LiveTriangleCount();
            bool changedBefore = m_TopologyChanged;
            m_TopologyChanged = false;
            Retopologize(glm::vec3(0.0f), 1e30f);
            settled = !m_TopologyChanged || LiveTriangleCount() == before;
            m_TopologyChanged |= changedBefore;
        }
        DetailSize = saved;
        UpdateNormals();
        return settled;
    }

    // ---- Mask and smoothing

    void SculptMesh::ClearMask() { std::fill(Mask.begin(), Mask.end(), 0.0f); }

    void SculptMesh::InvertMask()
    {
        for (float& m : Mask) m = 1.0f - m;
    }

    void SculptMesh::SmoothMask(int iterations)
    {
        for (int it = 0; it < iterations; ++it)
        {
            std::vector<float> next = Mask;
            for (uint32_t v = 0; v < Positions.size(); ++v)
            {
                std::vector<uint32_t> nb = Neighbors(v);
                if (nb.empty()) continue;
                float sum = 0.0f;
                for (uint32_t u : nb) sum += Mask[u];
                next[v] = (Mask[v] + sum / (float)nb.size()) * 0.5f;
            }
            Mask = std::move(next);
        }
    }

    void SculptMesh::SmoothAll(int iterations, float factor, bool respectMask)
    {
        for (int it = 0; it < iterations; ++it)
        {
            std::vector<glm::vec3> next = Positions;
            for (uint32_t v = 0; v < Positions.size(); ++v)
            {
                std::vector<uint32_t> nb = Neighbors(v);
                if (nb.empty()) continue;
                glm::vec3 avg(0.0f);
                for (uint32_t u : nb) avg += Positions[u];
                float w = factor * (respectMask ? 1.0f - Mask[v] : 1.0f);
                next[v] = Positions[v] + (avg / (float)nb.size() - Positions[v]) * w;
            }
            Positions = std::move(next);
        }
        UpdateNormals();
    }

    // ---- Voxel remesh

    EditableMesh VoxelRemesh(const EditableMesh& m, float voxelSize, int smooth)
    {
        EditableMesh out;
        out.Name = m.Name;
        out.MaterialSlots = m.MaterialSlots;
        out.Modifiers = m.Modifiers;
        out.AutoSmooth = m.AutoSmooth;
        out.AutoSmoothAngle = m.AutoSmoothAngle;
        // Triangles.
        std::vector<glm::vec3> tp;
        std::vector<uint32_t> tri;
        for (uint32_t f = 0; f < m.FaceCount(); ++f)
        {
            tri.clear();
            m.TriangulateFace(f, tri);
            for (uint32_t c : tri) tp.push_back(m.Positions[m.Faces[f].Verts[c]]);
        }
        if (tp.empty()) return out;
        glm::vec3 mn(1e30f), mx(-1e30f);
        for (const glm::vec3& p : tp)
        {
            mn = glm::min(mn, p);
            mx = glm::max(mx, p);
        }
        float h = std::max(voxelSize, 1e-5f);
        // At most ~200 cells across the longest side (else the voxel grows).
        float longest = std::max({ mx.x - mn.x, mx.y - mn.y, mx.z - mn.z });
        if (longest / h > 200.0f)
        {
            float grown = longest / 200.0f;
            Log::Warn("Voxel Remesh: voxel size {:.4f} is too fine for this model; using {:.4f}", h, grown);
            h = grown;
        }
        // A slightly shifted grid, so grid lines don't run exactly through vertices and edges.
        glm::vec3 origin = mn - glm::vec3(2.0f * h) + h * glm::vec3(0.01371f, 0.02193f, 0.03117f);
        glm::ivec3 dim = glm::ivec3(glm::ceil((mx - origin) / h)) + 3;
        auto node = [&](int i, int j, int k) { return ((size_t)k * dim.y + j) * dim.x + i; };
        auto pos = [&](int i, int j, int k) { return origin + glm::vec3((float)i, (float)j, (float)k) * h; };
        // Crossings along grid lines of each axis: per line, (coordinate along it, +1 entering / -1 leaving).
        std::vector<std::vector<std::pair<float, int>>> lines[3];
        int other[3][2] = { { 1, 2 }, { 0, 2 }, { 0, 1 } };
        for (int a = 0; a < 3; ++a)
        {
            int b = other[a][0], c = other[a][1];
            lines[a].resize((size_t)dim[b] * dim[c]);
            for (size_t t = 0; t + 2 < tp.size(); t += 3)
            {
                const glm::vec3 &p0 = tp[t], &p1 = tp[t + 1], &p2 = tp[t + 2];
                glm::vec3 n = glm::cross(p1 - p0, p2 - p0);
                if (std::abs(n[a]) < 1e-20f) continue;
                float bmin = std::min({ p0[b], p1[b], p2[b] }), bmax = std::max({ p0[b], p1[b], p2[b] });
                float cmin = std::min({ p0[c], p1[c], p2[c] }), cmax = std::max({ p0[c], p1[c], p2[c] });
                int j0 = std::max(0, (int)std::ceil((bmin - origin[b]) / h)), j1 = std::min(dim[b] - 1, (int)std::floor((bmax - origin[b]) / h));
                int k0 = std::max(0, (int)std::ceil((cmin - origin[c]) / h)), k1 = std::min(dim[c] - 1, (int)std::floor((cmax - origin[c]) / h));
                for (int k = k0; k <= k1; ++k)
                    for (int j = j0; j <= j1; ++j)
                    {
                        float yb = origin[b] + j * h, zc = origin[c] + k * h;
                        // Barycentrics of the line's point in the triangle's (b, c) projection.
                        float d0 = (p1[b] - p0[b]) * (zc - p0[c]) - (p1[c] - p0[c]) * (yb - p0[b]);
                        float d1 = (p2[b] - p1[b]) * (zc - p1[c]) - (p2[c] - p1[c]) * (yb - p1[b]);
                        float d2 = (p0[b] - p2[b]) * (zc - p2[c]) - (p0[c] - p2[c]) * (yb - p2[b]);
                        bool neg = d0 < 0 || d1 < 0 || d2 < 0, posv = d0 > 0 || d1 > 0 || d2 > 0;
                        if (neg && posv) continue;
                        float sum = d0 + d1 + d2;
                        if (std::abs(sum) < 1e-30f) continue;
                        // d1 weighs p0, d2 weighs p1, d0 weighs p2.
                        float x = (d1 * p0[a] + d2 * p1[a] + d0 * p2[a]) / sum;
                        lines[a][(size_t)k * dim[b] + j].push_back({ x, n[a] < 0.0f ? 1 : -1 });
                    }
            }
            for (auto& l : lines[a]) std::sort(l.begin(), l.end());
        }
        // Inside: the winding number along X lines is not zero (so overlapping parts join).
        std::vector<uint8_t> inside((size_t)dim.x * dim.y * dim.z, 0);
        for (int k = 0; k < dim.z; ++k)
            for (int j = 0; j < dim.y; ++j)
            {
                const auto& l = lines[0][(size_t)k * dim.y + j];
                size_t h0 = 0;
                int winding = 0;
                for (int i = 0; i < dim.x; ++i)
                {
                    float x = origin.x + i * h;
                    while (h0 < l.size() && l[h0].first < x) winding += l[h0++].second;
                    inside[node(i, j, k)] = winding != 0;
                }
            }
        // Where the surface crosses a grid edge from node (i,j,k) along axis a.
        auto crossing = [&](int a, int i, int j, int k) {
            glm::ivec3 id(i, j, k);
            int b = other[a][0], c = other[a][1];
            const auto& l = lines[a][(size_t)id[c] * dim[b] + id[b]];
            glm::vec3 p = pos(i, j, k);
            float lo = p[a], hi = lo + h;
            for (const auto& [x, s] : l)
                if (x >= lo && x <= hi)
                {
                    p[a] = x;
                    return p;
                }
            p[a] = lo + h * 0.5f;
            return p;
        };
        // Surface nets: a vertex in each cell the surface passes through, at its crossings' average.
        glm::ivec3 cells = dim - 1;
        std::unordered_map<size_t, uint32_t> cellVertex;
        auto cellIndex = [&](int i, int j, int k) { return ((size_t)k * cells.y + j) * cells.x + i; };
        for (int k = 0; k < cells.z; ++k)
            for (int j = 0; j < cells.y; ++j)
                for (int i = 0; i < cells.x; ++i)
                {
                    uint8_t first = inside[node(i, j, k)];
                    bool mixed = false;
                    for (int q = 1; q < 8 && !mixed; ++q) mixed = inside[node(i + (q & 1), j + ((q >> 1) & 1), k + ((q >> 2) & 1))] != first;
                    if (!mixed) continue;
                    glm::vec3 sum(0.0f);
                    int count = 0;
                    for (int a = 0; a < 3; ++a)
                        for (int e = 0; e < 4; ++e)
                        {
                            glm::ivec3 s(i, j, k);
                            s[other[a][0]] += e & 1;
                            s[other[a][1]] += e >> 1;
                            glm::ivec3 t = s;
                            t[a] += 1;
                            if (inside[node(s.x, s.y, s.z)] == inside[node(t.x, t.y, t.z)]) continue;
                            sum += crossing(a, s.x, s.y, s.z);
                            ++count;
                        }
                    cellVertex[cellIndex(i, j, k)] = out.AddVertex(count ? sum / (float)count : pos(i, j, k) + glm::vec3(h * 0.5f));
                }
        // A quad around each grid edge the surface crosses, facing out of the inside.
        for (int k = 1; k < dim.z - 1; ++k)
            for (int j = 1; j < dim.y - 1; ++j)
                for (int i = 1; i < dim.x - 1; ++i)
                {
                    uint8_t in = inside[node(i, j, k)];
                    for (int a = 0; a < 3; ++a)
                    {
                        glm::ivec3 t(i, j, k);
                        t[a] += 1;
                        if (t[a] >= dim[a] || inside[node(t.x, t.y, t.z)] == in) continue;
                        // The four cells around the edge, counter-clockwise about +axis.
                        int b = other[a][0], c = other[a][1];
                        if (a == 1) std::swap(b, c); // (z, x) order for Y keeps it right-handed
                        glm::ivec3 q[4];
                        int offs[4][2] = { { -1, -1 }, { 0, -1 }, { 0, 0 }, { -1, 0 } };
                        bool ok = true;
                        for (int v = 0; v < 4; ++v)
                        {
                            q[v] = glm::ivec3(i, j, k);
                            q[v][b] += offs[v][0];
                            q[v][c] += offs[v][1];
                            ok &= cellVertex.count(cellIndex(q[v].x, q[v].y, q[v].z)) > 0;
                        }
                        if (!ok) continue;
                        std::vector<uint32_t> verts(4);
                        for (int v = 0; v < 4; ++v) verts[v] = cellVertex[cellIndex(q[v].x, q[v].y, q[v].z)];
                        if (!in) std::reverse(verts.begin(), verts.end()); // the inside is further along: face back
                        out.AddFace(std::move(verts), {}, 0, true);
                    }
                }
        // Relax (Taubin: smooth, then un-shrink).
        out.Changed();
        if (smooth > 0)
        {
            const Topology& topo = out.Topo();
            std::vector<std::vector<uint32_t>> nb(out.VertexCount());
            for (EdgeKey e : topo.Edges)
            {
                nb[EdgeA(e)].push_back(EdgeB(e));
                nb[EdgeB(e)].push_back(EdgeA(e));
            }
            for (int it = 0; it < smooth * 2; ++it)
            {
                float lambda = (it & 1) ? -0.53f : 0.5f;
                std::vector<glm::vec3> next = out.Positions;
                for (uint32_t v = 0; v < out.VertexCount(); ++v)
                {
                    if (nb[v].empty()) continue;
                    glm::vec3 avg(0.0f);
                    for (uint32_t u : nb[v]) avg += out.Positions[u];
                    next[v] += (avg / (float)nb[v].size() - out.Positions[v]) * lambda;
                }
                out.Positions = std::move(next);
            }
        }
        return out;
    }
}
