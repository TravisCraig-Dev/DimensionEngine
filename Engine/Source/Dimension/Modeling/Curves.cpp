#include "Dimension/Modeling/Curves.h"
#include "Dimension/Core/Log.h"
#include <stb_truetype.h>
#include <glm/gtc/constants.hpp>
#include <algorithm>
#include <cmath>

namespace Dimension::Modeling
{
    // ---- Flat shapes

    namespace
    {
        float Cross(glm::vec2 o, glm::vec2 a, glm::vec2 b) { return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x); }

        float SignedArea(const std::vector<glm::vec2>& ring)
        {
            float a = 0.0f;
            for (size_t i = 0; i < ring.size(); ++i)
            {
                glm::vec2 p = ring[i], q = ring[(i + 1) % ring.size()];
                a += p.x * q.y - q.x * p.y;
            }
            return a * 0.5f;
        }

        bool InsideRing(glm::vec2 p, const std::vector<glm::vec2>& ring)
        {
            bool in = false;
            for (size_t i = 0, j = ring.size() - 1; i < ring.size(); j = i++)
                if ((ring[i].y > p.y) != (ring[j].y > p.y) && p.x < (ring[j].x - ring[i].x) * (p.y - ring[i].y) / (ring[j].y - ring[i].y) + ring[i].x) in = !in;
            return in;
        }

        bool InTriangle(glm::vec2 p, glm::vec2 a, glm::vec2 b, glm::vec2 c)
        {
            // (points on the edges count: an ear touching another corner would cut the polygon apart)
            return Cross(a, b, p) >= -1e-12f && Cross(b, c, p) >= -1e-12f && Cross(c, a, p) >= -1e-12f;
        }

        // Without repeated neighbours (and the closing repeat).
        std::vector<glm::vec2> Clean(std::vector<glm::vec2> ring)
        {
            std::vector<glm::vec2> out;
            for (const glm::vec2& p : ring)
                if (out.empty() || glm::length(p - out.back()) > 1e-7f) out.push_back(p);
            while (out.size() > 1 && glm::length(out.front() - out.back()) <= 1e-7f) out.pop_back();
            // Points on a straight run between their neighbours go (ear clipping can't cut at them, and caps without
            // them wouldn't meet the sides).
            for (bool removed = true; removed && out.size() > 3;)
            {
                removed = false;
                for (size_t i = 0; i < out.size() && out.size() > 3; ++i)
                {
                    glm::vec2 a = out[(i + out.size() - 1) % out.size()], b = out[i], c = out[(i + 1) % out.size()];
                    if (std::abs(Cross(a, b, c)) <= 1e-6f * glm::length(b - a) * glm::length(c - b) + 1e-12f)
                    {
                        out.erase(out.begin() + (std::ptrdiff_t)i);
                        removed = true;
                        --i;
                    }
                }
            }
            return out;
        }
    }

    std::vector<uint32_t> Shapes::Triangulate(const std::vector<std::vector<glm::vec2>>& rings)
    {
        std::vector<uint32_t> out;
        if (rings.empty() || rings[0].size() < 3) return out;
        std::vector<glm::vec2> pts;
        std::vector<std::vector<uint32_t>> idx;
        for (const auto& ring : rings)
        {
            std::vector<uint32_t> r;
            for (const glm::vec2& p : ring)
            {
                r.push_back((uint32_t)pts.size());
                pts.push_back(p);
            }
            idx.push_back(std::move(r));
        }
        // The outline counter-clockwise, holes clockwise.
        if (SignedArea(rings[0]) < 0.0f) std::reverse(idx[0].begin(), idx[0].end());
        for (size_t h = 1; h < rings.size(); ++h)
            if (SignedArea(rings[h]) > 0.0f) std::reverse(idx[h].begin(), idx[h].end());
        // Holes joined to the outline by bridges, rightmost holes first (Eberly's method).
        std::vector<uint32_t> poly = idx[0];
        std::vector<size_t> holes;
        for (size_t h = 1; h < idx.size(); ++h)
            if (idx[h].size() >= 3) holes.push_back(h);
        auto maxX = [&](size_t h) {
            float m = -1e30f;
            for (uint32_t i : idx[h]) m = std::max(m, pts[i].x);
            return m;
        };
        std::sort(holes.begin(), holes.end(), [&](size_t a, size_t b) { return maxX(a) > maxX(b); });
        for (size_t h : holes)
        {
            const auto& hole = idx[h];
            size_t mi = 0;
            for (size_t i = 1; i < hole.size(); ++i)
                if (pts[hole[i]].x > pts[hole[mi]].x) mi = i;
            glm::vec2 m = pts[hole[mi]];
            // The nearest outline edge to the right of M along +x.
            float bestX = 1e30f;
            size_t bestEdge = SIZE_MAX;
            for (size_t i = 0; i < poly.size(); ++i)
            {
                glm::vec2 a = pts[poly[i]], b = pts[poly[(i + 1) % poly.size()]];
                if ((a.y > m.y) == (b.y > m.y) && a.y != m.y && b.y != m.y) continue;
                if (a.y == b.y) continue;
                float t = (m.y - a.y) / (b.y - a.y);
                if (t < 0.0f || t > 1.0f) continue;
                float x = a.x + (b.x - a.x) * t;
                if (x >= m.x && x < bestX)
                {
                    bestX = x;
                    bestEdge = i;
                }
            }
            if (bestEdge == SIZE_MAX) continue; // (not inside the outline)
            glm::vec2 hit(bestX, m.y);
            size_t pi = bestEdge, pj = (bestEdge + 1) % poly.size();
            size_t visible = pts[poly[pi]].x > pts[poly[pj]].x ? pi : pj;
            // A reflex outline vertex inside triangle (M, hit, P) blocks the view: take the one at the smallest angle.
            glm::vec2 p = pts[poly[visible]];
            float bestAngle = 1e30f;
            for (size_t i = 0; i < poly.size(); ++i)
            {
                if (i == visible) continue;
                glm::vec2 q = pts[poly[i]];
                glm::vec2 prev = pts[poly[(i + poly.size() - 1) % poly.size()]], next = pts[poly[(i + 1) % poly.size()]];
                if (Cross(prev, q, next) >= 0.0f) continue; // convex
                bool inside = Cross(m, hit, q) >= 0.0f && Cross(hit, p, q) >= 0.0f && Cross(p, m, q) >= 0.0f;
                if (!inside && (Cross(m, hit, q) <= 0.0f && Cross(hit, p, q) <= 0.0f && Cross(p, m, q) <= 0.0f)) inside = true;
                if (!inside) continue;
                float angle = std::atan2(std::abs(q.y - m.y), q.x - m.x);
                if (angle < bestAngle)
                {
                    bestAngle = angle;
                    visible = i;
                }
            }
            std::vector<uint32_t> merged(poly.begin(), poly.begin() + (std::ptrdiff_t)visible + 1);
            for (size_t k = 0; k <= hole.size(); ++k) merged.push_back(hole[(mi + k) % hole.size()]);
            merged.push_back(poly[visible]);
            merged.insert(merged.end(), poly.begin() + (std::ptrdiff_t)visible + 1, poly.end());
            poly = std::move(merged);
        }
        // Ear clipping.
        std::vector<uint32_t> v = poly;
        int guard = (int)(v.size() * v.size()) + 16;
        while (v.size() > 3 && guard-- > 0)
        {
            size_t n = v.size(), ear = SIZE_MAX;
            for (size_t i = 0; i < n && ear == SIZE_MAX; ++i)
            {
                glm::vec2 a = pts[v[(i + n - 1) % n]], b = pts[v[i]], c = pts[v[(i + 1) % n]];
                if (Cross(a, b, c) <= 1e-12f) continue;
                bool empty = true;
                for (size_t k = 0; k < n && empty; ++k)
                {
                    glm::vec2 q = pts[v[k]];
                    if (q == a || q == b || q == c) continue;
                    if (InTriangle(q, a, b, c)) empty = false;
                }
                if (empty) ear = i;
            }
            if (ear == SIZE_MAX)
            {
                // Nothing clean (degenerate input): drop the flattest corner and go on.
                size_t flat = 0;
                float best = 1e30f;
                for (size_t i = 0; i < n; ++i)
                {
                    float c = std::abs(Cross(pts[v[(i + n - 1) % n]], pts[v[i]], pts[v[(i + 1) % n]]));
                    if (c < best)
                    {
                        best = c;
                        flat = i;
                    }
                }
                if (Cross(pts[v[(flat + n - 1) % n]], pts[v[flat]], pts[v[(flat + 1) % n]]) > 0.0f)
                    out.insert(out.end(), { v[(flat + n - 1) % n], v[flat], v[(flat + 1) % n] });
                v.erase(v.begin() + (std::ptrdiff_t)flat);
                continue;
            }
            out.insert(out.end(), { v[(ear + n - 1) % n], v[ear], v[(ear + 1) % n] });
            v.erase(v.begin() + (std::ptrdiff_t)ear);
        }
        if (v.size() == 3 && Cross(pts[v[0]], pts[v[1]], pts[v[2]]) > 0.0f) out.insert(out.end(), { v[0], v[1], v[2] });
        return out;
    }

    EditableMesh Shapes::Fill(const std::vector<std::vector<glm::vec2>>& input, float depth, float bevel)
    {
        EditableMesh m;
        std::vector<std::vector<glm::vec2>> contours;
        for (const auto& c : input)
        {
            std::vector<glm::vec2> clean = Clean(c);
            if (clean.size() >= 3 && std::abs(SignedArea(clean)) > 1e-12f) contours.push_back(std::move(clean));
        }
        // Nesting: how many others each contour lies in (even: solid outline, odd: hole).
        size_t n = contours.size();
        std::vector<int> level(n, 0), parent(n, -1);
        for (size_t i = 0; i < n; ++i)
        {
            float parentArea = 1e30f;
            for (size_t j = 0; j < n; ++j)
                if (i != j && InsideRing(contours[i][0], contours[j]))
                {
                    ++level[i];
                    float a = std::abs(SignedArea(contours[j]));
                    if (a < parentArea)
                    {
                        parentArea = a;
                        parent[i] = (int)j;
                    }
                }
        }
        glm::vec2 mn(1e30f), mx(-1e30f);
        for (const auto& c : contours)
            for (const glm::vec2& p : c)
            {
                mn = glm::min(mn, p);
                mx = glm::max(mx, p);
            }
        glm::vec2 extent = glm::max(mx - mn, glm::vec2(1e-6f));
        float scale = std::max(extent.x, extent.y);
        auto capUV = [&](glm::vec2 p) { return glm::vec2((p.x - mn.x) / scale, 1.0f - (p.y - mn.y) / scale); };
        float h = depth * 0.5f;
        float b = depth > 0.0f ? std::clamp(bevel, 0.0f, h * 0.98f) : 0.0f;
        for (size_t i = 0; i < n; ++i)
        {
            if (level[i] % 2) continue;
            // The outline counter-clockwise, its holes clockwise: the solid is on every ring's left.
            std::vector<std::vector<glm::vec2>> rings = { contours[i] };
            for (size_t j = 0; j < n; ++j)
                if (parent[j] == (int)i && level[j] % 2) rings.push_back(contours[j]);
            for (size_t r = 0; r < rings.size(); ++r)
                if ((SignedArea(rings[r]) > 0.0f) != (r == 0)) std::reverse(rings[r].begin(), rings[r].end());
            // The bevel: the rings moved into the solid by `b` (mitred corners, not more than three times `b`).
            std::vector<std::vector<glm::vec2>> inset = rings;
            if (b > 0.0f)
                for (size_t r = 0; r < rings.size(); ++r)
                {
                    const auto& ring = rings[r];
                    size_t c = ring.size();
                    for (size_t k = 0; k < c; ++k)
                    {
                        glm::vec2 e0 = ring[k] - ring[(k + c - 1) % c], e1 = ring[(k + 1) % c] - ring[k];
                        glm::vec2 n0 = glm::length(e0) > 1e-12f ? glm::normalize(glm::vec2(-e0.y, e0.x)) : glm::vec2(0.0f);
                        glm::vec2 n1 = glm::length(e1) > 1e-12f ? glm::normalize(glm::vec2(-e1.y, e1.x)) : glm::vec2(0.0f);
                        glm::vec2 off = (n0 + n1) / std::max(1.0f + glm::dot(n0, n1), 0.1f);
                        if (glm::length(off) > 3.0f) off = glm::normalize(off) * 3.0f;
                        inset[r][k] = ring[k] + off * b;
                    }
                }
            // Layers from front to back: the caps on the (inset) rings, the sides between.
            struct Layer
            {
                const std::vector<std::vector<glm::vec2>>* Rings;
                float Z;
            };
            std::vector<Layer> layers;
            if (depth <= 0.0f) layers = { { &rings, 0.0f } };
            else if (b > 0.0f) layers = { { &inset, h }, { &rings, h - b }, { &rings, -(h - b) }, { &inset, -h } };
            else layers = { { &rings, h }, { &rings, -h } };
            std::vector<std::vector<std::vector<uint32_t>>> ids(layers.size());
            for (size_t L = 0; L < layers.size(); ++L)
                for (const auto& ring : *layers[L].Rings)
                {
                    ids[L].emplace_back();
                    for (const glm::vec2& p : ring) ids[L].back().push_back(m.AddVertex({ p.x, p.y, layers[L].Z }));
                }
            // Caps (triangulated on the cap layer's own rings).
            std::vector<uint32_t> tris = Triangulate(*layers.front().Rings);
            std::vector<glm::vec2> flat;
            std::vector<uint32_t> front, back;
            for (size_t r = 0; r < layers.front().Rings->size(); ++r)
            {
                const auto& ring = (*layers.front().Rings)[r];
                flat.insert(flat.end(), ring.begin(), ring.end());
                front.insert(front.end(), ids.front()[r].begin(), ids.front()[r].end());
                back.insert(back.end(), ids.back()[r].begin(), ids.back()[r].end());
            }
            for (size_t k = 0; k + 2 < tris.size(); k += 3)
            {
                uint32_t a = tris[k], bb = tris[k + 1], c = tris[k + 2];
                m.AddFace({ front[a], front[bb], front[c] }, { capUV(flat[a]), capUV(flat[bb]), capUV(flat[c]) });
                if (layers.size() > 1) m.AddFace({ back[c], back[bb], back[a] }, { capUV(flat[c]), capUV(flat[bb]), capUV(flat[a]) });
            }
            // Sides (and chamfers): quads between neighbouring layers, facing out of the solid.
            for (size_t L = 0; L + 1 < layers.size(); ++L)
                for (size_t r = 0; r < rings.size(); ++r)
                {
                    const auto& ring = rings[r];
                    float along = 0.0f, perimeter = 0.0f;
                    for (size_t k = 0; k < ring.size(); ++k) perimeter += glm::length(ring[(k + 1) % ring.size()] - ring[k]);
                    float v0 = (float)L / (float)(layers.size() - 1), v1 = (float)(L + 1) / (float)(layers.size() - 1);
                    for (size_t k = 0; k < ring.size(); ++k)
                    {
                        size_t k1 = (k + 1) % ring.size();
                        float len = glm::length(ring[k1] - ring[k]);
                        float u0 = along / std::max(perimeter, 1e-9f), u1 = (along + len) / std::max(perimeter, 1e-9f);
                        along += len;
                        m.AddFace({ ids[L][r][k], ids[L + 1][r][k], ids[L + 1][r][k1], ids[L][r][k1] }, { { u0, v0 }, { u0, v1 }, { u1, v1 }, { u1, v0 } });
                    }
                }
        }
        m.Changed();
        return m;
    }

    // ---- Curves

    Curve CurveOps::Make(const std::vector<glm::vec3>& points, bool cyclic)
    {
        Curve c;
        c.Cyclic = cyclic;
        for (const glm::vec3& p : points)
        {
            CurvePoint cp;
            cp.Position = p;
            c.Points.push_back(cp);
        }
        return c;
    }

    void CurveOps::Handles(const Curve& curve, size_t i, glm::vec3& in, glm::vec3& out)
    {
        const auto& p = curve.Points;
        size_t n = p.size();
        const CurvePoint& c = p[i];
        if (c.HandleType != 0 && !c.Sharp)
        {
            in = c.Position + c.HandleIn;
            out = c.Position + c.HandleOut;
            return;
        }
        // Auto: along the line from the previous point to the next, a third of each span long.
        bool cyclic = curve.Cyclic && n >= 3;
        bool first = !cyclic && i == 0, last = !cyclic && i + 1 == n;
        glm::vec3 prev = first ? c.Position : p[(i + n - 1) % n].Position, next = last ? c.Position : p[(i + 1) % n].Position;
        if (c.Sharp)
        {
            in = c.Position + (prev - c.Position) / 3.0f;
            out = c.Position + (next - c.Position) / 3.0f;
            return;
        }
        glm::vec3 t = next - prev;
        float tl = glm::length(t);
        t = tl > 1e-12f ? t / tl : glm::vec3(0.0f);
        in = c.Position - t * (glm::length(c.Position - prev) / 3.0f);
        out = c.Position + t * (glm::length(next - c.Position) / 3.0f);
    }

    std::vector<glm::vec3> CurveOps::Sample(const Curve& curve, int resolution, std::vector<float>* radius, std::vector<float>* tilt)
    {
        std::vector<glm::vec3> out;
        const auto& p = curve.Points;
        size_t n = p.size();
        if (radius) radius->clear();
        if (tilt) tilt->clear();
        if (n == 0) return out;
        if (n == 1)
        {
            out.push_back(p[0].Position);
            if (radius) radius->push_back(p[0].Radius);
            if (tilt) tilt->push_back(p[0].Tilt);
            return out;
        }
        int res = std::max(resolution, 1);
        bool cyclic = curve.Cyclic && n >= 3;
        auto at = [&](int i) -> const CurvePoint& { return p[(size_t)((i % (int)n + (int)n) % (int)n)]; };
        // Auto handles: along the line from the previous point to the next, a third of each span long.
        auto handles = [&](int i, glm::vec3& in, glm::vec3& outH) { Handles(curve, (size_t)((i % (int)n + (int)n) % (int)n), in, outH); };
        int spans = cyclic ? (int)n : (int)n - 1;
        for (int s = 0; s < spans; ++s)
        {
            glm::vec3 in0, out0, in1, out1;
            handles(s, in0, out0);
            handles(s + 1, in1, out1);
            glm::vec3 p0 = at(s).Position, p3 = at(s + 1).Position;
            for (int k = 0; k < res; ++k)
            {
                float t = (float)k / (float)res, u = 1.0f - t;
                out.push_back(p0 * (u * u * u) + out0 * (3 * u * u * t) + in1 * (3 * u * t * t) + p3 * (t * t * t));
                if (radius) radius->push_back(glm::mix(at(s).Radius, at(s + 1).Radius, t));
                if (tilt) tilt->push_back(glm::mix(at(s).Tilt, at(s + 1).Tilt, t));
            }
        }
        if (!cyclic)
        {
            out.push_back(p.back().Position);
            if (radius) radius->push_back(p.back().Radius);
            if (tilt) tilt->push_back(p.back().Tilt);
        }
        return out;
    }

    namespace
    {
        // Rotation-minimizing frames (double reflection, Wang et al. 2008): per sample a normal; closed loops spread
        // the leftover twist evenly.
        void Frames(const std::vector<glm::vec3>& x, bool cyclic, std::vector<glm::vec3>& tangent, std::vector<glm::vec3>& normal)
        {
            size_t n = x.size();
            tangent.assign(n, glm::vec3(0, 0, 1));
            normal.assign(n, glm::vec3(1, 0, 0));
            if (n < 2) return;
            for (size_t i = 0; i < n; ++i)
            {
                glm::vec3 a = cyclic ? x[(i + n - 1) % n] : x[i == 0 ? 0 : i - 1], b = cyclic ? x[(i + 1) % n] : x[std::min(i + 1, n - 1)];
                glm::vec3 t = b - a;
                if (glm::length(t) > 1e-12f) tangent[i] = glm::normalize(t);
                else if (i) tangent[i] = tangent[i - 1];
            }
            glm::vec3 up = std::abs(tangent[0].z) < 0.99f ? glm::vec3(0, 0, 1) : glm::vec3(0, 1, 0);
            normal[0] = glm::normalize(up - tangent[0] * glm::dot(up, tangent[0]));
            size_t steps = cyclic ? n : n - 1;
            glm::vec3 r = normal[0];
            for (size_t i = 0; i < steps; ++i)
            {
                size_t j = (i + 1) % n;
                glm::vec3 v1 = x[j] - x[i];
                float c1 = glm::dot(v1, v1);
                glm::vec3 rL = r, tL = tangent[i];
                if (c1 > 1e-20f)
                {
                    rL = r - (2.0f / c1) * glm::dot(v1, r) * v1;
                    tL = tangent[i] - (2.0f / c1) * glm::dot(v1, tangent[i]) * v1;
                }
                glm::vec3 v2 = tangent[j] - tL;
                float c2 = glm::dot(v2, v2);
                r = c2 > 1e-20f ? rL - (2.0f / c2) * glm::dot(v2, rL) * v2 : rL;
                r = glm::normalize(r - tangent[j] * glm::dot(r, tangent[j]));
                if (j != 0) normal[j] = r;
            }
            if (cyclic)
            {
                // The transported normal back at the start, against the start's: spread the difference.
                float angle = std::atan2(glm::dot(glm::cross(r, normal[0]), tangent[0]), glm::dot(r, normal[0]));
                for (size_t i = 1; i < n; ++i)
                {
                    float a = angle * (float)i / (float)n;
                    glm::vec3 t = tangent[i], v = normal[i];
                    normal[i] = v * std::cos(a) + glm::cross(t, v) * std::sin(a) + t * glm::dot(t, v) * (1.0f - std::cos(a));
                }
            }
        }

        // Turns every face over when the first one faces the wrong way (against `outward` at its center).
        void Orient(EditableMesh& m, uint32_t firstFace, const glm::vec3& outward)
        {
            if (firstFace >= m.FaceCount()) return;
            if (glm::dot(m.FaceNormal(firstFace), outward) >= 0.0f) return;
            for (uint32_t f = firstFace; f < m.FaceCount(); ++f) m.Faces[f].Reverse();
        }
    }

    EditableMesh CurveOps::ToMesh(const std::vector<Curve>& curves, const CurveSettings& s)
    {
        EditableMesh m;
        // Filled shapes: the closed curves in the XY plane.
        if (s.Fill)
        {
            // The closed curves' own plane (best fit, Newell's method over all of them; XY when they're flat in it).
            std::vector<std::vector<glm::vec3>> loops;
            glm::vec3 normal(0.0f), center(0.0f);
            int count = 0;
            for (const Curve& c : curves)
            {
                if (!c.Cyclic || c.Points.size() < 3) continue;
                loops.push_back(Sample(c, s.Resolution));
                const auto& l = loops.back();
                for (size_t i = 0; i < l.size(); ++i)
                {
                    const glm::vec3 &a = l[i], &b = l[(i + 1) % l.size()];
                    normal += glm::vec3((a.y - b.y) * (a.z + b.z), (a.z - b.z) * (a.x + b.x), (a.x - b.x) * (a.y + b.y));
                    center += a;
                    ++count;
                }
            }
            if (!loops.empty())
            {
                center /= (float)std::max(count, 1);
                glm::vec3 n = glm::length(normal) > 1e-12f ? glm::normalize(normal) : glm::vec3(0, 0, 1);
                if (glm::dot(n, glm::vec3(0, 0, 1)) < 0.0f && std::abs(n.z) > 0.999f) n = -n; // (flat in XY: face +Z)
                glm::vec3 ref = std::abs(n.x) < 0.9f ? glm::vec3(1, 0, 0) : glm::vec3(0, 1, 0);
                glm::vec3 ax = glm::normalize(ref - n * glm::dot(ref, n)), ay = glm::cross(n, ax); // (ax x ay = n; X stays X in the XY plane)
                std::vector<std::vector<glm::vec2>> contours;
                for (const auto& l : loops)
                {
                    std::vector<glm::vec2> ring;
                    for (const glm::vec3& p : l) ring.push_back({ glm::dot(p - center, ax), glm::dot(p - center, ay) });
                    contours.push_back(std::move(ring));
                }
                EditableMesh filled = Shapes::Fill(contours, s.Extrude * 2.0f, s.BevelDepth);
                for (auto& p : filled.Positions) p = center + ax * p.x + ay * p.y + n * p.z;
                filled.Changed();
                m.Append(filled);
            }
        }
        for (const Curve& c : curves)
        {
            if (c.Points.size() < 2 || (s.Fill && c.Cyclic && c.Points.size() >= 3)) continue;
            bool cyclic = c.Cyclic && c.Points.size() >= 3;
            std::vector<float> radius, tilt;
            std::vector<glm::vec3> x = Sample(c, s.Resolution, &radius, &tilt), t, nrm;
            Frames(x, cyclic, t, nrm);
            for (size_t i = 0; i < x.size(); ++i)
                if (tilt[i] != 0.0f)
                {
                    glm::vec3 v = nrm[i], a = t[i];
                    nrm[i] = v * std::cos(tilt[i]) + glm::cross(a, v) * std::sin(tilt[i]);
                }
            size_t n = x.size();
            std::vector<float> along(n, 0.0f);
            for (size_t i = 1; i < n; ++i) along[i] = along[i - 1] + glm::length(x[i] - x[i - 1]);
            float total = std::max(along.back() + (cyclic ? glm::length(x[0] - x.back()) : 0.0f), 1e-9f);
            size_t rows = n;
            size_t spans = cyclic ? n : n - 1; // (closed loops join the last ring back to the first)
            uint32_t firstFace = m.FaceCount();
            if (s.BevelDepth > 0.0f)
            {
                int sides = std::max(s.BevelResolution, 3);
                std::vector<std::vector<uint32_t>> ring(rows);
                for (size_t r = 0; r < rows; ++r)
                {
                    size_t i = r;
                    glm::vec3 b = glm::cross(t[i], nrm[i]);
                    for (int k = 0; k < sides; ++k)
                    {
                        float a = glm::two_pi<float>() * (float)k / (float)sides;
                        ring[r].push_back(m.AddVertex(x[i] + (nrm[i] * std::cos(a) + b * std::sin(a)) * (s.BevelDepth * radius[i])));
                    }
                }
                for (size_t r = 0; r < spans; ++r)
                {
                    size_t r1 = (r + 1) % n;
                    float v0 = along[r] / total, v1 = r + 1 < n ? along[r + 1] / total : 1.0f;
                    for (int k = 0; k < sides; ++k)
                    {
                        int k1 = (k + 1) % sides;
                        float u0 = (float)k / (float)sides, u1 = (float)(k + 1) / (float)sides;
                        m.AddFace({ ring[r][(size_t)k], ring[r][(size_t)k1], ring[r1][(size_t)k1], ring[r1][(size_t)k] }, { { u0, v0 }, { u1, v0 }, { u1, v1 }, { u0, v1 } },
                                  0, s.Smooth);
                    }
                }
                if (firstFace < m.FaceCount()) Orient(m, firstFace, m.FaceCenter(firstFace) - x[0]); // (outwards from the curve)
                if (!cyclic && s.Caps)
                {
                    uint32_t cap0 = m.FaceCount();
                    m.AddFace(ring[0]);
                    Orient(m, cap0, -t[0]);
                    std::vector<uint32_t> last = ring[rows - 1];
                    uint32_t cap1 = m.FaceCount();
                    m.AddFace(last);
                    Orient(m, cap1, t[n - 1]);
                }
            }
            else if (s.Extrude > 0.0f)
            {
                std::vector<uint32_t> left(rows), right(rows);
                for (size_t r = 0; r < rows; ++r)
                {
                    size_t i = r;
                    left[r] = m.AddVertex(x[i] - nrm[i] * (s.Extrude * radius[i]));
                    right[r] = m.AddVertex(x[i] + nrm[i] * (s.Extrude * radius[i]));
                }
                for (size_t r = 0; r < spans; ++r)
                {
                    size_t r1 = (r + 1) % n;
                    float v0 = along[r] / total, v1 = r + 1 < n ? along[r + 1] / total : 1.0f;
                    m.AddFace({ left[r], right[r], right[r1], left[r1] }, { { 0, v0 }, { 1, v0 }, { 1, v1 }, { 0, v1 } }, 0, s.Smooth);
                }
            }
            else
            {
                // Just the line (wire edges).
                std::vector<uint32_t> ids;
                for (size_t i = 0; i < n; ++i) ids.push_back(m.AddVertex(x[i]));
                for (size_t i = 0; i + 1 < n; ++i) m.AddLooseEdge(ids[i], ids[i + 1]);
                if (cyclic) m.AddLooseEdge(ids.back(), ids.front());
            }
        }
        m.Changed();
        return m;
    }

    // ---- Text

    namespace
    {
        TextOps::FontResolver s_FontResolver;

        // Decodes UTF-8 (bad bytes become '?').
        std::vector<uint32_t> Codepoints(const std::string& text)
        {
            std::vector<uint32_t> out;
            for (size_t i = 0; i < text.size();)
            {
                unsigned char c = (unsigned char)text[i];
                uint32_t cp = '?';
                int len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
                if (len == 1) cp = c < 0x80 ? c : '?';
                else if (i + (size_t)len <= text.size())
                {
                    cp = c & (0x7F >> len);
                    for (int k = 1; k < len; ++k) cp = (cp << 6) | ((unsigned char)text[i + (size_t)k] & 0x3F);
                }
                out.push_back(cp);
                i += (size_t)len;
            }
            return out;
        }
    }

    void TextOps::SetFontResolver(FontResolver resolver) { s_FontResolver = std::move(resolver); }

    EditableMesh TextOps::ToMesh(const TextSettings& text)
    {
        std::vector<uint8_t> bytes;
        if (!s_FontResolver || !s_FontResolver(text.Font, bytes) || bytes.empty()) return EditableMesh();
        return ToMesh(text, bytes.data(), bytes.size());
    }

    EditableMesh TextOps::ToMesh(const TextSettings& text, const uint8_t* font, size_t size)
    {
        EditableMesh m;
        stbtt_fontinfo info;
        if (!font || size == 0 || !stbtt_InitFont(&info, font, stbtt_GetFontOffsetForIndex(font, 0))) return m;
        int ascent = 0, descent = 0, gap = 0;
        stbtt_GetFontVMetrics(&info, &ascent, &descent, &gap);
        float scale = text.Size / (float)std::max(ascent - descent, 1);
        int steps = std::clamp(text.Resolution, 1, 32);
        // Lines of glyph outlines, laid out left to right.
        struct Line
        {
            std::vector<std::vector<std::vector<glm::vec2>>> Glyphs; // per letter, its outlines
            float Width = 0.0f;
        };
        std::vector<Line> lines(1);
        float pen = 0.0f;
        int prevGlyph = 0;
        for (uint32_t cp : Codepoints(text.Body))
        {
            if (cp == '\n')
            {
                lines.back().Width = pen;
                lines.emplace_back();
                pen = 0.0f;
                prevGlyph = 0;
                continue;
            }
            int glyph = stbtt_FindGlyphIndex(&info, (int)cp);
            if (prevGlyph) pen += (float)stbtt_GetGlyphKernAdvance(&info, prevGlyph, glyph) * scale;
            stbtt_vertex* v = nullptr;
            int count = stbtt_GetGlyphShape(&info, glyph, &v);
            std::vector<glm::vec2>* contour = nullptr;
            lines.back().Glyphs.emplace_back();
            auto& outlines = lines.back().Glyphs.back();
            glm::vec2 at(0.0f);
            auto point = [&](float x, float y) { return glm::vec2(pen + x * scale, y * scale); };
            for (int i = 0; i < count; ++i)
            {
                glm::vec2 p = point(v[i].x, v[i].y);
                switch (v[i].type)
                {
                case STBTT_vmove:
                    outlines.emplace_back();
                    contour = &outlines.back();
                    contour->push_back(p);
                    break;
                case STBTT_vline:
                    if (contour) contour->push_back(p);
                    break;
                case STBTT_vcurve:
                {
                    glm::vec2 c = point(v[i].cx, v[i].cy);
                    for (int k = 1; k <= steps && contour; ++k)
                    {
                        float t = (float)k / (float)steps, u = 1.0f - t;
                        contour->push_back(at * (u * u) + c * (2 * u * t) + p * (t * t));
                    }
                    break;
                }
                case STBTT_vcubic:
                {
                    glm::vec2 c0 = point(v[i].cx, v[i].cy), c1 = point(v[i].cx1, v[i].cy1);
                    for (int k = 1; k <= steps && contour; ++k)
                    {
                        float t = (float)k / (float)steps, u = 1.0f - t;
                        contour->push_back(at * (u * u * u) + c0 * (3 * u * u * t) + c1 * (3 * u * t * t) + p * (t * t * t));
                    }
                    break;
                }
                default: break;
                }
                at = p;
            }
            if (v) stbtt_FreeShape(&info, v);
            int advance = 0, bearing = 0;
            stbtt_GetGlyphHMetrics(&info, glyph, &advance, &bearing);
            pen += (float)advance * scale + text.Spacing * text.Size;
            prevGlyph = glyph;
        }
        lines.back().Width = pen;
        // Each letter filled on its own (so letters that overlap, as in tight script fonts, don't cut holes in each other).
        for (size_t l = 0; l < lines.size(); ++l)
        {
            float shift = text.Align == 1 ? -lines[l].Width * 0.5f : text.Align == 2 ? -lines[l].Width : 0.0f;
            float y = -(float)l * text.LineSpacing * text.Size;
            for (auto& glyph : lines[l].Glyphs)
            {
                for (auto& c : glyph)
                    for (glm::vec2& p : c) p += glm::vec2(shift, y);
                m.Append(Shapes::Fill(glyph, text.Depth, text.Bevel));
            }
        }
        m.Changed();
        return m;
    }
}
