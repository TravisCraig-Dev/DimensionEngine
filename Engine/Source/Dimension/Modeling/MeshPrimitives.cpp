#include "Dimension/Modeling/EditableMesh.h"
#include <glm/gtc/constants.hpp>
#include <map>

namespace Dimension::Modeling::Primitives
{
    namespace
    {
        constexpr float kTau = glm::two_pi<float>();

        EditableMesh Finish(EditableMesh m, const char* name)
        {
            m.Name = name;
            for (auto& f : m.VertFlags) f = Selected;
            for (auto& f : m.Faces) f.Flags = Selected;
            m.Changed();
            for (EdgeKey k : m.Topo().Edges) m.EditEdge(k).Flags = Selected;
            return m;
        }

        // A ring of `n` vertices around Y at height y.
        uint32_t Ring(EditableMesh& m, uint32_t n, float radius, float y)
        {
            uint32_t first = m.VertexCount();
            for (uint32_t i = 0; i < n; ++i)
            {
                float a = kTau * (float)i / (float)n;
                m.AddVertex({ std::sin(a) * radius, y, std::cos(a) * radius });
            }
            return first;
        }

        // A cap n-gon over a ring (facing up or down), UVs a disc inside `rect` (min xy, max zw).
        void Cap(EditableMesh& m, uint32_t ring, uint32_t n, bool up, glm::vec4 rect)
        {
            std::vector<uint32_t> verts;
            std::vector<glm::vec2> uvs;
            glm::vec2 center((rect.x + rect.z) * 0.5f, (rect.y + rect.w) * 0.5f), half((rect.z - rect.x) * 0.5f, (rect.w - rect.y) * 0.5f);
            for (uint32_t k = 0; k < n; ++k)
            {
                uint32_t i = up ? k : (n - k) % n;
                float a = kTau * (float)i / (float)n;
                verts.push_back(ring + i);
                uvs.push_back(center + glm::vec2(std::sin(a) * half.x, (up ? -1.0f : 1.0f) * std::cos(a) * half.y));
            }
            m.AddFace(std::move(verts), std::move(uvs));
        }
    }

    EditableMesh Plane(float size)
    {
        EditableMesh m;
        float h = size * 0.5f;
        m.AddVertex({ -h, 0, h });
        m.AddVertex({ h, 0, h });
        m.AddVertex({ h, 0, -h });
        m.AddVertex({ -h, 0, -h });
        m.AddFace({ 0, 1, 2, 3 }, { { 0, 1 }, { 1, 1 }, { 1, 0 }, { 0, 0 } });
        return Finish(std::move(m), "Plane");
    }

    EditableMesh Cube(float size)
    {
        EditableMesh m;
        float h = size * 0.5f;
        for (int i = 0; i < 8; ++i) m.AddVertex({ i & 1 ? h : -h, i & 2 ? h : -h, i & 4 ? h : -h });
        // Each face counter-clockwise seen from outside; corners start bottom-left (UV 0,1).
        const uint32_t faces[6][4] = {
            { 4, 5, 7, 6 }, // +Z
            { 1, 0, 2, 3 }, // -Z
            { 5, 1, 3, 7 }, // +X
            { 0, 4, 6, 2 }, // -X
            { 6, 7, 3, 2 }, // +Y
            { 0, 1, 5, 4 }, // -Y
        };
        for (const auto& f : faces) m.AddFace({ f[0], f[1], f[2], f[3] }, { { 0, 1 }, { 1, 1 }, { 1, 0 }, { 0, 0 } });
        return Finish(std::move(m), "Cube");
    }

    EditableMesh Circle(uint32_t vertices, float radius, bool fill)
    {
        EditableMesh m;
        uint32_t n = std::max(3u, vertices);
        uint32_t ring = Ring(m, n, radius, 0.0f);
        if (fill) Cap(m, ring, n, true, { 0, 0, 1, 1 });
        else
            for (uint32_t i = 0; i < n; ++i) m.AddLooseEdge(ring + i, ring + (i + 1) % n);
        return Finish(std::move(m), "Circle");
    }

    EditableMesh Grid(uint32_t xs, uint32_t ys, float size)
    {
        EditableMesh m;
        xs = std::max(1u, xs);
        ys = std::max(1u, ys);
        float h = size * 0.5f;
        for (uint32_t y = 0; y <= ys; ++y)
            for (uint32_t x = 0; x <= xs; ++x) m.AddVertex({ -h + size * x / xs, 0.0f, h - size * y / ys });
        auto at = [&](uint32_t x, uint32_t y) { return y * (xs + 1) + x; };
        auto uv = [&](uint32_t x, uint32_t y) { return glm::vec2((float)x / xs, 1.0f - (float)y / ys); };
        for (uint32_t y = 0; y < ys; ++y)
            for (uint32_t x = 0; x < xs; ++x)
                m.AddFace({ at(x, y), at(x + 1, y), at(x + 1, y + 1), at(x, y + 1) }, { uv(x, y), uv(x + 1, y), uv(x + 1, y + 1), uv(x, y + 1) });
        return Finish(std::move(m), "Grid");
    }

    EditableMesh UVSphere(uint32_t segments, uint32_t rings, float radius)
    {
        EditableMesh m;
        uint32_t s = std::max(3u, segments), r = std::max(2u, rings);
        uint32_t top = m.AddVertex({ 0, radius, 0 });
        for (uint32_t ring = 1; ring < r; ++ring)
        {
            float phi = glm::pi<float>() * (float)ring / (float)r;
            Ring(m, s, radius * std::sin(phi), radius * std::cos(phi));
        }
        uint32_t bottom = m.AddVertex({ 0, -radius, 0 });
        auto at = [&](uint32_t ring, uint32_t seg) { return 1 + (ring - 1) * s + seg % s; };
        auto uv = [&](uint32_t ring, uint32_t seg) { return glm::vec2((float)seg / s, (float)ring / r); };
        for (uint32_t seg = 0; seg < s; ++seg)
        {
            m.AddFace({ top, at(1, seg), at(1, seg + 1) }, { { (seg + 0.5f) / s, 0.0f }, uv(1, seg), uv(1, seg + 1) });
            for (uint32_t ring = 1; ring + 1 < r; ++ring)
                m.AddFace({ at(ring, seg), at(ring + 1, seg), at(ring + 1, seg + 1), at(ring, seg + 1) },
                          { uv(ring, seg), uv(ring + 1, seg), uv(ring + 1, seg + 1), uv(ring, seg + 1) });
            m.AddFace({ at(r - 1, seg), bottom, at(r - 1, seg + 1) }, { uv(r - 1, seg), { (seg + 0.5f) / s, 1.0f }, uv(r - 1, seg + 1) });
        }
        for (auto& f : m.Faces) f.Smooth = true;
        return Finish(std::move(m), "Sphere");
    }

    EditableMesh IcoSphere(uint32_t subdivisions, float radius)
    {
        EditableMesh m;
        const float t = (1.0f + std::sqrt(5.0f)) * 0.5f;
        std::vector<glm::vec3> p = { { -1, t, 0 }, { 1, t, 0 }, { -1, -t, 0 }, { 1, -t, 0 }, { 0, -1, t }, { 0, 1, t },
                                     { 0, -1, -t }, { 0, 1, -t }, { t, 0, -1 }, { t, 0, 1 }, { -t, 0, -1 }, { -t, 0, 1 } };
        std::vector<glm::uvec3> tris = { { 0, 11, 5 }, { 0, 5, 1 }, { 0, 1, 7 }, { 0, 7, 10 }, { 0, 10, 11 }, { 1, 5, 9 }, { 5, 11, 4 },
                                         { 11, 10, 2 }, { 10, 7, 6 }, { 7, 1, 8 }, { 3, 9, 4 }, { 3, 4, 2 }, { 3, 2, 6 }, { 3, 6, 8 },
                                         { 3, 8, 9 }, { 4, 9, 5 }, { 2, 4, 11 }, { 6, 2, 10 }, { 8, 6, 7 }, { 9, 8, 1 } };
        for (auto& v : p) v = glm::normalize(v);
        for (uint32_t level = 1; level < std::max(1u, subdivisions); ++level)
        {
            std::map<std::pair<uint32_t, uint32_t>, uint32_t> mid;
            auto midpoint = [&](uint32_t a, uint32_t b) {
                auto key = std::minmax(a, b);
                auto [it, inserted] = mid.try_emplace(key, (uint32_t)p.size());
                if (inserted) p.push_back(glm::normalize(p[a] + p[b]));
                return it->second;
            };
            std::vector<glm::uvec3> next;
            for (auto& tr : tris)
            {
                uint32_t a = midpoint(tr.x, tr.y), b = midpoint(tr.y, tr.z), c = midpoint(tr.z, tr.x);
                next.insert(next.end(), { { tr.x, a, c }, { tr.y, b, a }, { tr.z, c, b }, { a, b, c } });
            }
            tris = std::move(next);
        }
        for (auto& v : p) m.AddVertex(v * radius);
        auto sphereUV = [](const glm::vec3& d) {
            return glm::vec2(0.5f + std::atan2(d.x, d.z) / kTau, std::acos(std::clamp(d.y, -1.0f, 1.0f)) / glm::pi<float>());
        };
        for (auto& tr : tris)
        {
            glm::vec2 uv[3] = { sphereUV(p[tr.x]), sphereUV(p[tr.y]), sphereUV(p[tr.z]) };
            // Triangles across the seam: wrap the low side over.
            float maxU = std::max({ uv[0].x, uv[1].x, uv[2].x });
            for (auto& u : uv)
                if (maxU - u.x > 0.5f) u.x += 1.0f;
            // Pole vertices take the middle of the other two's U.
            for (int i = 0; i < 3; ++i)
                if (std::abs(p[tr[i]].y) > 0.9999f) uv[i].x = (uv[(i + 1) % 3].x + uv[(i + 2) % 3].x) * 0.5f;
            m.AddFace({ tr.x, tr.y, tr.z }, { uv[0], uv[1], uv[2] }, 0, true);
        }
        return Finish(std::move(m), "Icosphere");
    }

    EditableMesh Cone(uint32_t vertices, float radius1, float radius2, float depth, bool caps)
    {
        EditableMesh m;
        uint32_t n = std::max(3u, vertices);
        float h = depth * 0.5f;
        uint32_t bottom = Ring(m, n, radius1, -h);
        bool apex = radius2 <= 0.0f;
        uint32_t top = apex ? m.AddVertex({ 0, h, 0 }) : Ring(m, n, radius2, h);
        // Sides in the lower half of the UV square, caps in the upper half.
        for (uint32_t i = 0; i < n; ++i)
        {
            uint32_t j = (i + 1) % n;
            float u0 = (float)i / n, u1 = (float)(i + 1) / n;
            if (apex) m.AddFace({ bottom + i, bottom + j, top }, { { u0, 1.0f }, { u1, 1.0f }, { (u0 + u1) * 0.5f, 0.5f } });
            else m.AddFace({ bottom + i, bottom + j, top + j, top + i }, { { u0, 1.0f }, { u1, 1.0f }, { u1, 0.5f }, { u0, 0.5f } });
        }
        if (caps)
        {
            if (radius1 > 0.0f) Cap(m, bottom, n, false, { 0.5f, 0.0f, 1.0f, 0.5f });
            if (!apex) Cap(m, top, n, true, { 0.0f, 0.0f, 0.5f, 0.5f });
        }
        return Finish(std::move(m), apex ? "Cone" : "Cylinder");
    }

    EditableMesh Cylinder(uint32_t vertices, float radius, float depth, bool caps) { return Cone(vertices, radius, radius, depth, caps); }

    EditableMesh Torus(uint32_t majorSegments, uint32_t minorSegments, float majorRadius, float minorRadius)
    {
        EditableMesh m;
        uint32_t ma = std::max(3u, majorSegments), mi = std::max(3u, minorSegments);
        for (uint32_t i = 0; i < ma; ++i)
        {
            float a = kTau * (float)i / ma;
            glm::vec3 dir(std::sin(a), 0.0f, std::cos(a));
            for (uint32_t j = 0; j < mi; ++j)
            {
                float b = kTau * (float)j / mi;
                m.AddVertex(dir * (majorRadius + minorRadius * std::cos(b)) + glm::vec3(0, minorRadius * std::sin(b), 0));
            }
        }
        auto at = [&](uint32_t i, uint32_t j) { return (i % ma) * mi + j % mi; };
        auto uv = [&](uint32_t i, uint32_t j) { return glm::vec2((float)i / ma, 1.0f - (float)j / mi); };
        for (uint32_t i = 0; i < ma; ++i)
            for (uint32_t j = 0; j < mi; ++j)
                m.AddFace({ at(i, j), at(i + 1, j), at(i + 1, j + 1), at(i, j + 1) }, { uv(i, j), uv(i + 1, j), uv(i + 1, j + 1), uv(i, j + 1) }, 0, true);
        return Finish(std::move(m), "Torus");
    }
}
