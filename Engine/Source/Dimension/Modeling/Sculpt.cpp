#include "Dimension/Modeling/Sculpt.h"
#include "Dimension/Core/Log.h"
#include <algorithm>
#include <cmath>
#include <numeric>
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
        static const char* names[] = { "Draw", "Clay", "Inflate", "Smooth", "Flatten", "Scrape", "Pinch", "Crease", "Grab", "Snake Hook", "Mask",
                                       "Pose", "Boundary", "Cloth" };
        return names[std::clamp((int)b, 0, (int)Brush::Count - 1)];
    }

    // ---- Brush textures

    BrushTexture BrushTexture::FromRGBA8(const uint32_t* pixels, uint32_t width, uint32_t height)
    {
        BrushTexture t;
        t.Width = width;
        t.Height = height;
        t.Values.resize((size_t)width * height);
        for (size_t i = 0; i < t.Values.size(); ++i)
        {
            uint32_t p = pixels[i];
            float r = (float)(p & 0xFF), g = (float)((p >> 8) & 0xFF), b = (float)((p >> 16) & 0xFF), a = (float)(p >> 24);
            t.Values[i] = (0.2126f * r + 0.7152f * g + 0.0722f * b) / 255.0f * (a / 255.0f);
        }
        return t;
    }

    float BrushTexture::Sample(glm::vec2 uv) const
    {
        if (Width == 0 || Height == 0 || Values.size() < (size_t)Width * Height) return 1.0f;
        glm::vec2 p = (uv - glm::floor(uv)) * glm::vec2((float)Width, (float)Height) - 0.5f;
        glm::vec2 f = p - glm::floor(p);
        int x0 = (int)std::floor(p.x), y0 = (int)std::floor(p.y);
        auto at = [&](int x, int y) {
            x = ((x % (int)Width) + (int)Width) % (int)Width;
            y = ((y % (int)Height) + (int)Height) % (int)Height;
            return Values[(size_t)y * Width + (size_t)x];
        };
        float top = at(x0, y0) * (1.0f - f.x) + at(x0 + 1, y0) * f.x;
        float bottom = at(x0, y0 + 1) * (1.0f - f.x) + at(x0 + 1, y0 + 1) * f.x;
        return top * (1.0f - f.y) + bottom * f.y;
    }

    float BrushTexture::At(const glm::vec3& p, const glm::vec3& center, float radius, const glm::vec3& view) const
    {
        glm::vec3 n = glm::length(view) > 1e-12f ? glm::normalize(view) : glm::vec3(0, 0, -1);
        glm::vec3 helper = std::abs(n.y) < 0.99f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
        glm::vec3 right = glm::normalize(glm::cross(helper, n)), up = glm::cross(n, right);
        glm::vec2 q = Mapping == 1 ? glm::vec2(glm::dot(p, right), glm::dot(p, up)) / std::max(Scale, 1e-6f)
                                   : glm::vec2(glm::dot(p - center, right), glm::dot(p - center, up)) / (2.0f * std::max(radius, 1e-9f));
        float a = glm::radians(Angle), cs = std::cos(a), sn = std::sin(a);
        q = glm::vec2(cs * q.x - sn * q.y, sn * q.x + cs * q.y);
        if (Mapping != 1)
        {
            q += 0.5f;
            if (q.x < 0.0f || q.y < 0.0f || q.x > 1.0f || q.y > 1.0f) return 0.0f;
        }
        return Sample(glm::vec2(q.x, 1.0f - q.y)); // (rows from the top)
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
                auto uv = [&](uint32_t corner) { return corner < face.UVs.size() ? face.UVs[corner] : glm::vec2(0.0f); };
                s.AddTri(a, b, c, face.Material, { uv(tri[k]), uv(tri[k + 1]), uv(tri[k + 2]) });
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
            out.AddFace({ v[0], v[1], v[2] }, { TriUVs[t][0], TriUVs[t][1], TriUVs[t][2] }, TriMaterial[t], true);
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
        uint32_t v = (uint32_t)Positions.size() - 1;
        if (m_GridOk)
        {
            m_VertKey.push_back(CellKey(CellOf(p)));
            m_Grid[m_VertKey.back()].push_back(v);
            m_Lo = glm::min(m_Lo, p);
            m_Hi = glm::max(m_Hi, p);
        }
        return v;
    }

    uint32_t SculptMesh::AddTri(uint32_t a, uint32_t b, uint32_t c, int32_t material, const std::array<glm::vec2, 3>& uv)
    {
        uint32_t t = (uint32_t)Tris.size();
        Tris.push_back({ a, b, c });
        TriMaterial.push_back(material);
        TriUVs.push_back(uv);
        VertTris[a].push_back(t);
        VertTris[b].push_back(t);
        VertTris[c].push_back(t);
        if (m_GridOk)
        {
            float e = std::max({ glm::length(Positions[a] - Positions[b]), glm::length(Positions[b] - Positions[c]), glm::length(Positions[c] - Positions[a]) });
            m_MaxEdge = std::max(m_MaxEdge, e);
            if (m_MaxEdge > m_Cell) m_GridOk = false;
        }
        return t;
    }

    // ---- The vertex grid

    glm::ivec3 SculptMesh::CellOf(const glm::vec3& p) const { return glm::ivec3(glm::floor(p / m_Cell)); }

    uint64_t SculptMesh::CellKey(const glm::ivec3& c) const
    {
        auto bits = [](int v) { return (uint64_t)((uint32_t)(v + (1 << 20)) & 0x1FFFFFu); };
        return (bits(c.x) << 42) | (bits(c.y) << 21) | bits(c.z);
    }

    void SculptMesh::EnsureGrid() const
    {
        if (m_GridOk && m_VertKey.size() == Positions.size()) return;
        m_Lo = glm::vec3(1e30f);
        m_Hi = glm::vec3(-1e30f);
        for (size_t v = 0; v < Positions.size(); ++v)
            if (!VertTris[v].empty())
            {
                m_Lo = glm::min(m_Lo, Positions[v]);
                m_Hi = glm::max(m_Hi, Positions[v]);
            }
        if (m_Lo.x > m_Hi.x) m_Lo = m_Hi = glm::vec3(0.0f);
        m_MaxEdge = 0.0f;
        for (const glm::uvec3& t : Tris)
        {
            if (t.x == kDead) continue;
            m_MaxEdge = std::max({ m_MaxEdge, glm::length(Positions[t.x] - Positions[t.y]), glm::length(Positions[t.y] - Positions[t.z]),
                                   glm::length(Positions[t.z] - Positions[t.x]) });
        }
        float diag = glm::length(m_Hi - m_Lo);
        // Room to grow (edges stretched by brushes) before the next rebuild; never so fine the grid is mostly empty.
        m_Cell = std::max({ m_MaxEdge * 1.5f, diag / 200.0f, 1e-6f });
        m_Grid.clear();
        m_VertKey.resize(Positions.size());
        for (uint32_t v = 0; v < Positions.size(); ++v)
        {
            m_VertKey[v] = CellKey(CellOf(Positions[v]));
            m_Grid[m_VertKey[v]].push_back(v);
        }
        m_GridOk = true;
    }

    void SculptMesh::GridMoved(uint32_t v)
    {
        if (!m_GridOk) return;
        const glm::vec3& p = Positions[v];
        m_Lo = glm::min(m_Lo, p);
        m_Hi = glm::max(m_Hi, p);
        uint64_t key = CellKey(CellOf(p));
        if (key == m_VertKey[v]) return;
        std::vector<uint32_t>& old = m_Grid[m_VertKey[v]];
        auto it = std::find(old.begin(), old.end(), v);
        if (it != old.end())
        {
            *it = old.back();
            old.pop_back();
        }
        m_Grid[key].push_back(v);
        m_VertKey[v] = key;
    }

    void SculptMesh::NoteEdges(uint32_t v)
    {
        if (!m_GridOk) return;
        for (uint32_t t : VertTris[v])
            for (int k = 0; k < 3; ++k)
                if (uint32_t u = Tris[t][k]; u != v) m_MaxEdge = std::max(m_MaxEdge, glm::length(Positions[u] - Positions[v]));
        if (m_MaxEdge > m_Cell) m_GridOk = false;
    }

    void SculptMesh::VertsNear(const glm::vec3& c, float r, std::vector<uint32_t>& out) const
    {
        out.clear();
        EnsureGrid();
        float r2 = r * r;
        auto scanAll = [&] {
            for (uint32_t v = 0; v < Positions.size(); ++v)
            {
                if (VertTris[v].empty()) continue;
                glm::vec3 d = Positions[v] - c;
                if (!(r < 1e18f) || glm::dot(d, d) <= r2) out.push_back(v);
            }
        };
        if (!(r < 1e18f)) return scanAll();
        glm::vec3 lo = glm::max(c - r, m_Lo), hi = glm::min(c + r, m_Hi);
        if (glm::any(glm::greaterThan(lo, hi))) return; // (the sphere misses the mesh)
        glm::ivec3 a = CellOf(lo), b = CellOf(hi);
        double cells = (double)(b.x - a.x + 1) * (double)(b.y - a.y + 1) * (double)(b.z - a.z + 1);
        if (cells > (double)m_Grid.size()) return scanAll(); // (more cells than filled ones)
        for (int z = a.z; z <= b.z; ++z)
            for (int y = a.y; y <= b.y; ++y)
                for (int x = a.x; x <= b.x; ++x)
                {
                    auto it = m_Grid.find(CellKey({ x, y, z }));
                    if (it == m_Grid.end()) continue;
                    for (uint32_t v : it->second)
                    {
                        if (VertTris[v].empty()) continue;
                        glm::vec3 d = Positions[v] - c;
                        if (glm::dot(d, d) <= r2) out.push_back(v);
                    }
                }
        std::sort(out.begin(), out.end()); // (the same order as a plain scan, so results don't depend on the grid)
    }

    uint32_t SculptMesh::NextStamp() const
    {
        if (m_TriStamp.size() < Tris.size()) m_TriStamp.resize(Tris.size(), 0);
        if (++m_StampId == 0)
        {
            std::fill(m_TriStamp.begin(), m_TriStamp.end(), 0);
            m_StampId = 1;
        }
        return m_StampId;
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
        auto test = [&](uint32_t t) {
            const glm::uvec3& tri = Tris[t];
            if (tri.x == kDead) return;
            const glm::vec3 &a = Positions[tri.x], &b = Positions[tri.y], &c = Positions[tri.z];
            // Moller-Trumbore in plain floats.
            float e1x = b.x - a.x, e1y = b.y - a.y, e1z = b.z - a.z, e2x = c.x - a.x, e2y = c.y - a.y, e2z = c.z - a.z;
            float px = dy * e2z - dz * e2y, py = dz * e2x - dx * e2z, pz = dx * e2y - dy * e2x;
            float det = e1x * px + e1y * py + e1z * pz;
            if (det > -1e-12f && det < 1e-12f) return;
            float inv = 1.0f / det;
            float sx = ox - a.x, sy = oy - a.y, sz = oz - a.z;
            float u = (sx * px + sy * py + sz * pz) * inv;
            if (u < 0.0f || u > 1.0f) return;
            float qx = sy * e1z - sz * e1y, qy = sz * e1x - sx * e1z, qz = sx * e1y - sy * e1x;
            float v = (dx * qx + dy * qy + dz * qz) * inv;
            if (v < 0.0f || u + v > 1.0f) return;
            float d = (e2x * qx + e2y * qy + e2z * qz) * inv;
            if (d <= 0.0f || d >= best) return;
            best = d;
            normal = glm::vec3(e1y * e2z - e1z * e2y, e1z * e2x - e1x * e2z, e1x * e2y - e1y * e2x);
            hit = true;
        };
        EnsureGrid();
        // The ray's stretch inside the bounds (a cell wider all round).
        glm::vec3 lo = m_Lo - m_Cell, hi = m_Hi + m_Cell;
        float t0 = 0.0f, t1 = 1e30f;
        for (int k = 0; k < 3; ++k)
        {
            if (std::abs(dir[k]) < 1e-20f)
            {
                if (from[k] < lo[k] || from[k] > hi[k]) return false;
                continue;
            }
            float a = (lo[k] - from[k]) / dir[k], b = (hi[k] - from[k]) / dir[k];
            if (a > b) std::swap(a, b);
            t0 = std::max(t0, a);
            t1 = std::min(t1, b);
        }
        if (t0 > t1) return false;
        glm::ivec3 span = CellOf(hi) - CellOf(lo) + 1;
        if ((double)span.x * span.y * span.z <= 64.0)
        {
            for (uint32_t t = 0; t < Tris.size(); ++t) test(t); // (a coarse grid: just test them all)
        }
        else
        {
            // Walking the cells along the ray (Amanatides & Woo); a triangle crossing the ray in a cell has its corners
            // in that cell or a neighbour, and the walk stops once a hit is nearer than the cell's far side.
            uint32_t stamp = NextStamp();
            glm::vec3 p = from + dir * t0;
            glm::ivec3 cell = CellOf(p), step, last = CellOf(from + dir * t1);
            glm::vec3 tMax, tDelta;
            for (int k = 0; k < 3; ++k)
            {
                step[k] = dir[k] > 0.0f ? 1 : dir[k] < 0.0f ? -1 : 0;
                if (step[k] == 0)
                {
                    tMax[k] = tDelta[k] = 1e30f;
                    continue;
                }
                float boundary = (float)(cell[k] + (step[k] > 0 ? 1 : 0)) * m_Cell;
                tMax[k] = (boundary - from[k]) / dir[k];
                tDelta[k] = m_Cell / std::abs(dir[k]);
            }
            for (int guard = 0; guard < 1000000; ++guard)
            {
                for (int z = -1; z <= 1; ++z)
                    for (int y = -1; y <= 1; ++y)
                        for (int x = -1; x <= 1; ++x)
                        {
                            auto it = m_Grid.find(CellKey(cell + glm::ivec3(x, y, z)));
                            if (it == m_Grid.end()) continue;
                            for (uint32_t v : it->second)
                                for (uint32_t t : VertTris[v])
                                    if (m_TriStamp[t] != stamp)
                                    {
                                        m_TriStamp[t] = stamp;
                                        test(t);
                                    }
                        }
                float exit = std::min({ tMax.x, tMax.y, tMax.z });
                if (hit && best <= exit) break;
                if (cell == last || exit > t1) break;
                int k = tMax.x < tMax.y ? (tMax.x < tMax.z ? 0 : 2) : (tMax.y < tMax.z ? 1 : 2);
                cell[k] += step[k];
                tMax[k] += tDelta[k];
            }
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
        m_Pose.clear();
        m_ClothRest.clear();
        if (brush.Type == Brush::Pose || brush.Type == Brush::Boundary)
        {
            for (const glm::vec3& s : Mirrors(brush.Symmetry))
                if (brush.Type == Brush::Pose) CatchPose(center * s, brush, s);
                else CatchBoundary(center * s, brush, s);
            return;
        }
        if (brush.Type != Brush::Grab) return;
        float r2 = brush.Radius * brush.Radius;
        for (const glm::vec3& s : Mirrors(brush.Symmetry))
        {
            glm::vec3 c = center * s, view = brush.ViewDir * s;
            std::vector<uint32_t> near;
            VertsNear(c, brush.Radius, near);
            for (uint32_t v : near)
            {
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

    void SculptMesh::EndStroke()
    {
        m_Caught.clear();
        m_Pose.clear();
        m_ClothRest.clear();
    }

    void SculptMesh::CatchPose(const glm::vec3& c, const BrushSettings& brush, const glm::vec3& mirror)
    {
        float r = std::max(brush.Radius, 1e-6f);
        std::vector<uint32_t> near;
        VertsNear(c, r, near);
        if (near.empty()) return;
        // The pivot: the middle of the ring where the brush's sphere cuts the surface (a limb's joint when the brush
        // covers its end).
        glm::vec3 pivot(0.0f), areaN(0.0f);
        float count = 0.0f;
        for (uint32_t v : near)
        {
            areaN += Normals[v];
            if (glm::length(Positions[v] - c) >= r * 0.75f)
            {
                pivot += Positions[v];
                count += 1.0f;
            }
        }
        pivot = count > 0.0f ? pivot / count : c - (glm::length(areaN) > 1e-12f ? glm::normalize(areaN) : glm::vec3(0, 0, 1)) * r;
        glm::vec3 axis = c - pivot;
        float len2 = glm::dot(axis, axis);
        if (len2 < 1e-12f) return;
        m_Pose.push_back({ mirror, pivot, c });
        // What turns: the surface joined to the brush's part beyond the pivot (within twice the radius), more the
        // farther past the pivot.
        auto along = [&](uint32_t v) { return glm::dot(Positions[v] - pivot, axis) / len2; };
        std::unordered_map<uint32_t, float> weight;
        std::vector<uint32_t> queue;
        for (uint32_t v : near)
            if (along(v) > 0.0f)
            {
                weight[v] = 0.0f;
                queue.push_back(v);
            }
        for (size_t q = 0; q < queue.size(); ++q)
            for (uint32_t u : Neighbors(queue[q]))
                if (!weight.count(u) && along(u) > 0.0f && glm::length(Positions[u] - c) < 2.0f * r)
                {
                    weight[u] = 0.0f;
                    queue.push_back(u);
                }
        for (uint32_t v : queue)
        {
            float t = std::clamp(along(v), 0.0f, 1.0f);
            float w = t * t * (3.0f - 2.0f * t) * (1.0f - Mask[v]);
            if (w <= 0.0f) continue;
            bool already = false;
            for (const Caught& k : m_Caught) already |= k.Vertex == v;
            if (!already) m_Caught.push_back({ v, w, Positions[v], mirror });
        }
    }

    void SculptMesh::CatchBoundary(const glm::vec3& c, const BrushSettings& brush, const glm::vec3& mirror)
    {
        float r = std::max(brush.Radius, 1e-6f);
        std::vector<uint32_t> near;
        VertsNear(c, r, near);
        // From the open edges under the brush, along the surface: nearer (to the brush's center, then along the
        // surface from the edge) moves more.
        std::unordered_map<uint32_t, float> dist;
        std::vector<std::pair<float, uint32_t>> heap;
        for (uint32_t v : near)
            if (IsBorder(v))
            {
                float d = glm::length(Positions[v] - c);
                dist[v] = d;
                heap.push_back({ -d, v });
            }
        std::make_heap(heap.begin(), heap.end());
        while (!heap.empty())
        {
            std::pop_heap(heap.begin(), heap.end());
            auto [negD, v] = heap.back();
            heap.pop_back();
            if (-negD > dist[v]) continue;
            for (uint32_t u : Neighbors(v))
            {
                float nd = -negD + glm::length(Positions[u] - Positions[v]);
                if (nd >= r) continue;
                auto it = dist.find(u);
                if (it != dist.end() && it->second <= nd) continue;
                dist[u] = nd;
                heap.push_back({ -nd, u });
                std::push_heap(heap.begin(), heap.end());
            }
        }
        for (auto& [v, d] : dist)
        {
            float w = Falloff(d / r) * (1.0f - Mask[v]);
            if (w <= 0.0f) continue;
            bool already = false;
            for (const Caught& k : m_Caught) already |= k.Vertex == v;
            if (!already) m_Caught.push_back({ v, w, Positions[v], mirror });
        }
        std::sort(m_Caught.begin(), m_Caught.end(), [](const Caught& p, const Caught& q) { return p.Vertex < q.Vertex; });
    }

    void SculptMesh::ClothDab(const glm::vec3& c, const BrushSettings& brush, const glm::vec3& move)
    {
        float r = std::max(brush.Radius, 1e-6f);
        std::vector<uint32_t> region;
        VertsNear(c, r * 1.5f, region);
        if (region.empty()) return;
        std::unordered_map<uint32_t, uint32_t> local;
        for (uint32_t i = 0; i < region.size(); ++i) local[region[i]] = i;
        // The edges among them, with their rest lengths (as first seen in the stroke, before this dab moves them).
        std::vector<std::pair<uint32_t, uint32_t>> edges;
        for (uint32_t v : region)
            for (uint32_t u : Neighbors(v))
                if (v < u && local.count(u)) edges.push_back({ v, u });
        std::vector<float> rest(edges.size());
        for (size_t e = 0; e < edges.size(); ++e)
        {
            uint64_t key = ((uint64_t)edges[e].first << 32) | edges[e].second;
            auto it = m_ClothRest.find(key);
            if (it == m_ClothRest.end()) it = m_ClothRest.emplace(key, glm::length(Positions[edges[e].first] - Positions[edges[e].second])).first;
            rest[e] = it->second;
        }
        // The brush drags the middle along; the cloth around it follows, folding where it bunches up. Farther out
        // the surface holds still; masked parts too. (Position-based dynamics: each edge pulled back to its length,
        // the held ends not moving.)
        float str = std::min(1.0f, std::clamp(brush.Strength, 0.0f, 1.0f) * 2.0f);
        std::vector<glm::vec3> p(region.size());
        std::vector<float> freedom(region.size());
        for (size_t i = 0; i < region.size(); ++i)
        {
            uint32_t v = region[i];
            float d = glm::length(Positions[v] - c);
            float f = Falloff(d / r) * (1.0f - Mask[v]);
            // (a slight lift where the brush's edge bunches the cloth up, so it folds out of the surface rather than
            // squashing flat)
            p[i] = Positions[v] + move * (f * str) + Normals[v] * (f * (1.0f - f) * glm::length(move) * str);
            // (the middle goes with the brush, a ring past it is free to be pulled along, the rest holds)
            freedom[i] = d >= r * 1.3f ? 0.0f : (1.0f - f) * (1.0f - Mask[v]);
        }
        for (int it = 0; it < 20; ++it)
            for (size_t e = 0; e < edges.size(); ++e)
            {
                uint32_t i = local[edges[e].first], j = local[edges[e].second];
                float wi = freedom[i], wj = freedom[j];
                if (wi + wj <= 0.0f) continue;
                glm::vec3 d = p[j] - p[i];
                float len = glm::length(d);
                if (len < 1e-12f) continue;
                glm::vec3 fix = d * ((len - rest[e]) / len / (wi + wj));
                p[i] += fix * wi;
                p[j] -= fix * wj;
            }
        std::vector<uint32_t> ring;
        for (size_t i = 0; i < region.size(); ++i)
        {
            uint32_t v = region[i];
            for (int a = 0; a < 3; ++a)
                if ((brush.Symmetry >> a) & 1)
                    if (std::abs(Positions[v][a]) < r * 1e-4f) p[i][a] = Positions[v][a];
            Positions[v] = p[i];
            GridMoved(v);
        }
        for (uint32_t v : region)
        {
            NoteEdges(v);
            ring.push_back(v);
            for (uint32_t u : Neighbors(v)) ring.push_back(u);
        }
        std::sort(ring.begin(), ring.end());
        ring.erase(std::unique(ring.begin(), ring.end()), ring.end());
        UpdateNormals(ring);
    }

    void SculptMesh::Dab(const glm::vec3& center, const BrushSettings& brush, const glm::vec3& move)
    {
        if (brush.Type == Brush::Pose)
        {
            // Each mirror copy turns about its pivot by the angle the mouse has swung the brush's center through, the
            // caught vertices by as much of it as their weight.
            float k = std::min(1.0f, brush.Strength * 2.0f);
            std::vector<uint32_t> touched;
            for (const PoseFrame& pf : m_Pose)
            {
                glm::vec3 a = pf.Center - pf.Pivot, b = pf.Center + move * pf.Mirror - pf.Pivot;
                glm::vec3 axis = glm::cross(a, b);
                float sinA = glm::length(axis), cosA = glm::dot(a, b);
                if (sinA < 1e-12f) continue;
                axis /= sinA;
                float angle = std::atan2(sinA, cosA);
                for (const Caught& c : m_Caught)
                {
                    if (c.Mirror != pf.Mirror) continue;
                    float t = angle * c.Weight * k, cs = std::cos(t), sn = std::sin(t);
                    glm::vec3 d = c.Start - pf.Pivot;
                    // Rodrigues' rotation about the axis.
                    glm::vec3 rd = d * cs + glm::cross(axis, d) * sn + axis * glm::dot(axis, d) * (1.0f - cs);
                    Positions[c.Vertex] = pf.Pivot + rd;
                    GridMoved(c.Vertex);
                    touched.push_back(c.Vertex);
                }
            }
            for (uint32_t v : touched) NoteEdges(v);
            std::vector<uint32_t> ring = touched;
            for (uint32_t v : touched)
                for (uint32_t u : Neighbors(v)) ring.push_back(u);
            std::sort(ring.begin(), ring.end());
            ring.erase(std::unique(ring.begin(), ring.end()), ring.end());
            UpdateNormals(ring);
            return;
        }
        if (brush.Type == Brush::Grab || brush.Type == Brush::Boundary)
        {
            std::vector<uint32_t> touched;
            for (const Caught& k : m_Caught)
            {
                Positions[k.Vertex] = k.Start + move * k.Mirror * k.Weight * std::min(1.0f, brush.Strength * 2.0f);
                GridMoved(k.Vertex);
                touched.push_back(k.Vertex);
            }
            for (uint32_t v : touched) NoteEdges(v);
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
        for (const glm::vec3& s : mirrors)
            if (brush.Type == Brush::Cloth) ClothDab(center * s, brush, move * s);
            else ApplyBrush(center * s, brush, move * s, s);
    }

    void SculptMesh::ApplyBrush(const glm::vec3& c, const BrushSettings& brush, const glm::vec3& move, const glm::vec3& mirror)
    {
        float r = std::max(brush.Radius, 1e-6f), r2 = r * r;
        glm::vec3 view = brush.ViewDir * mirror;
        std::vector<std::pair<uint32_t, float>> hit; // vertex, falloff
        glm::vec3 areaN(0.0f), areaC(0.0f);
        float areaW = 0.0f;
        std::vector<uint32_t> near;
        VertsNear(c, r, near);
        for (uint32_t v : near)
        {
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
            if (brush.Texture && brush.Type != Brush::SnakeHook) f *= brush.Texture->At(p, c, r, view);
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
            GridMoved(hit[i].first);
        }
        for (auto& [v, f] : hit) NoteEdges(v);
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
            glm::vec2 ux = TriUVs[t][i], uy = TriUVs[t][(i + 1) % 3], uz = TriUVs[t][(i + 2) % 3], um = (ux + uy) * 0.5f;
            KillTri(t);
            AddTri(x, m, z, mat, { ux, um, uz });
            AddTri(m, y, z, mat, { um, uy, uz });
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
        // Corner UVs: each triangle around a or b moves its corner halfway along the collapsing edge in its own UV
        // island (known from a removed triangle with the same UV there; across a seam it stays put).
        std::vector<std::pair<glm::vec2, glm::vec2>> edgeUVs; // (at a, at b) in the removed triangles
        for (uint32_t t : shared)
        {
            glm::vec2 ua(0.0f), ub(0.0f);
            for (int k = 0; k < 3; ++k)
            {
                if (Tris[t][k] == a) ua = TriUVs[t][k];
                if (Tris[t][k] == b) ub = TriUVs[t][k];
            }
            edgeUVs.push_back({ ua, ub });
        }
        for (uint32_t t : shared) KillTri(t);
        auto sameUV = [](glm::vec2 p, glm::vec2 q) { return std::abs(p.x - q.x) < 1e-6f && std::abs(p.y - q.y) < 1e-6f; };
        for (int side = 0; side < 2; ++side)
            for (uint32_t t : VertTris[side ? b : a])
                for (int k = 0; k < 3; ++k)
                    if (Tris[t][k] == (side ? b : a))
                        for (auto& [ua, ub] : edgeUVs)
                            if (sameUV(TriUVs[t][k], side ? ub : ua))
                            {
                                TriUVs[t][k] += ((side ? ua : ub) - (side ? ub : ua)) * 0.5f;
                                break;
                            }
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
        GridMoved(a);
        NoteEdges(a);
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
            // The triangles the brush's sphere touches (each has a corner within an edge's length of it).
            std::vector<uint32_t> candidates;
            if (reach2 < 1e35f)
            {
                std::vector<uint32_t> near;
                VertsNear(center, reach + m_MaxEdge, near);
                uint32_t stamp = NextStamp();
                for (uint32_t v : near)
                    for (uint32_t t : VertTris[v])
                        if (m_TriStamp[t] != stamp)
                        {
                            m_TriStamp[t] = stamp;
                            candidates.push_back(t);
                        }
                std::sort(candidates.begin(), candidates.end());
            }
            else
            {
                candidates.resize(Tris.size());
                std::iota(candidates.begin(), candidates.end(), 0u);
            }
            for (uint32_t t : candidates)
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
        m_GridOk = false;
        UpdateNormals();
    }

    // ---- Voxel remesh

    namespace
    {
        // Eigenvalues and eigenvectors (columns of `vectors`) of a symmetric 3x3 matrix (cyclic Jacobi).
        void SymmetricEigen(glm::dmat3 a, glm::dvec3& values, glm::dmat3& vectors)
        {
            vectors = glm::dmat3(1.0);
            for (int sweep = 0; sweep < 12; ++sweep)
            {
                double off = a[0][1] * a[0][1] + a[0][2] * a[0][2] + a[1][2] * a[1][2];
                if (off < 1e-24) break;
                for (int p = 0; p < 2; ++p)
                    for (int q = p + 1; q < 3; ++q)
                    {
                        if (std::abs(a[p][q]) < 1e-30) continue;
                        double theta = (a[q][q] - a[p][p]) / (2.0 * a[p][q]);
                        double t = (theta >= 0 ? 1.0 : -1.0) / (std::abs(theta) + std::sqrt(theta * theta + 1.0));
                        double c = 1.0 / std::sqrt(t * t + 1.0), s = t * c;
                        glm::dmat3 r(1.0);
                        r[p][p] = c;
                        r[q][q] = c;
                        r[q][p] = s;  // column q, row p
                        r[p][q] = -s; // column p, row q
                        a = glm::transpose(r) * a * r;
                        vectors = vectors * r;
                    }
            }
            values = glm::dvec3(a[0][0], a[1][1], a[2][2]);
        }

        // The point nearest all the planes (point, normal) in the least squares sense, starting from their points'
        // middle and moving only along directions the planes pin down (so flat patches stay put, edges and corners
        // snap). `rank`: how many directions that was.
        glm::vec3 SolvePlanes(const std::vector<glm::vec3>& points, const std::vector<glm::vec3>& normals, int& rank)
        {
            glm::dvec3 c(0.0);
            for (const glm::vec3& p : points) c += glm::dvec3(p);
            c /= (double)std::max<size_t>(points.size(), 1);
            glm::dmat3 ata(0.0);
            glm::dvec3 rhs(0.0);
            for (size_t i = 0; i < points.size(); ++i)
            {
                glm::dvec3 n(normals[i]);
                for (int col = 0; col < 3; ++col)
                    for (int row = 0; row < 3; ++row) ata[col][row] += n[row] * n[col];
                rhs += n * glm::dot(n, glm::dvec3(points[i]) - c);
            }
            glm::dvec3 w;
            glm::dmat3 v;
            SymmetricEigen(ata, w, v);
            double top = std::max({ w.x, w.y, w.z });
            glm::dvec3 x = c;
            rank = 0;
            for (int k = 0; k < 3; ++k)
                if (w[k] > top * 0.1 && w[k] > 1e-12)
                {
                    x += v[k] * (glm::dot(v[k], rhs) / w[k]);
                    ++rank;
                }
            return glm::vec3(x);
        }

        struct LineHit
        {
            float X;
            int Sign;    // +1 entering, -1 leaving
            glm::vec3 N; // the surface's normal there
            bool operator<(const LineHit& o) const { return X < o.X; }
        };
    }

    EditableMesh VoxelRemesh(const EditableMesh& m, float voxelSize, int smooth, bool sharp)
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
        // Crossings along grid lines of each axis.
        std::vector<std::vector<LineHit>> lines[3];
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
                        lines[a][(size_t)k * dim[b] + j].push_back({ x, n[a] < 0.0f ? 1 : -1, glm::normalize(n) });
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
                    while (h0 < l.size() && l[h0].X < x) winding += l[h0++].Sign;
                    inside[node(i, j, k)] = winding != 0;
                }
            }
        // Where the surface crosses a grid edge from node (i,j,k) along axis a, and its normal there.
        auto crossing = [&](int a, int i, int j, int k, glm::vec3& normal) {
            glm::ivec3 id(i, j, k);
            int b = other[a][0], c = other[a][1];
            const auto& l = lines[a][(size_t)id[c] * dim[b] + id[b]];
            glm::vec3 p = pos(i, j, k);
            float lo = p[a], hi = lo + h;
            for (const LineHit& hit : l)
                if (hit.X >= lo && hit.X <= hi)
                {
                    p[a] = hit.X;
                    normal = hit.N;
                    return p;
                }
            p[a] = lo + h * 0.5f;
            normal = glm::vec3(0.0f);
            return p;
        };
        std::vector<uint8_t> feature; // per new vertex: on an edge or corner (kept out of the relaxing)
        std::vector<glm::vec3> cellPoints, cellNormals;
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
                    cellPoints.clear();
                    cellNormals.clear();
                    for (int a = 0; a < 3; ++a)
                        for (int e = 0; e < 4; ++e)
                        {
                            glm::ivec3 s(i, j, k);
                            s[other[a][0]] += e & 1;
                            s[other[a][1]] += e >> 1;
                            glm::ivec3 t = s;
                            t[a] += 1;
                            if (inside[node(s.x, s.y, s.z)] == inside[node(t.x, t.y, t.z)]) continue;
                            glm::vec3 n;
                            glm::vec3 p = crossing(a, s.x, s.y, s.z, n);
                            sum += p;
                            ++count;
                            if (glm::dot(n, n) > 0.0f)
                            {
                                cellPoints.push_back(p);
                                cellNormals.push_back(n);
                            }
                        }
                    glm::vec3 at = count ? sum / (float)count : pos(i, j, k) + glm::vec3(h * 0.5f);
                    int rank = 1;
                    if (sharp && !cellPoints.empty())
                    {
                        glm::vec3 solved = SolvePlanes(cellPoints, cellNormals, rank);
                        // (kept near its cell: planes nearly parallel can put the meeting point far away)
                        glm::vec3 lo = pos(i, j, k) - glm::vec3(h * 0.5f), hi = pos(i, j, k) + glm::vec3(h * 1.5f);
                        if (glm::all(glm::greaterThanEqual(solved, lo)) && glm::all(glm::lessThanEqual(solved, hi))) at = solved;
                        else rank = 1;
                    }
                    cellVertex[cellIndex(i, j, k)] = out.AddVertex(at);
                    feature.push_back(rank >= 2 ? 1 : 0);
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
                    if (nb[v].empty() || (sharp && feature[v])) continue;
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
