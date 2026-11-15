#include "Dimension/Modeling/UVTools.h"
#include <glm/gtc/constants.hpp>
#include <algorithm>
#include <cmath>
#include <map>
#include <numeric>

namespace Dimension::Modeling::UV
{
    namespace
    {
        constexpr float kSameUV = 1e-5f;
        constexpr double kPi = 3.14159265358979323846;

        struct UnionFind
        {
            std::vector<uint32_t> Parent;
            explicit UnionFind(size_t n) : Parent(n) { std::iota(Parent.begin(), Parent.end(), 0u); }
            uint32_t Find(uint32_t x)
            {
                while (Parent[x] != x)
                {
                    Parent[x] = Parent[Parent[x]];
                    x = Parent[x];
                }
                return x;
            }
            void Join(uint32_t a, uint32_t b)
            {
                a = Find(a);
                b = Find(b);
                if (a != b) Parent[b] = a;
            }
        };

        int CornerOf(const Face& f, uint32_t v)
        {
            for (size_t c = 0; c < f.Verts.size(); ++c)
                if (f.Verts[c] == v) return (int)c;
            return -1;
        }

        glm::vec2 UVAt(const Face& f, size_t c) { return c < f.UVs.size() ? f.UVs[c] : glm::vec2(0.0f); }
        void SetUV(Face& f, size_t c, glm::vec2 uv)
        {
            if (f.UVs.size() != f.Verts.size()) f.UVs.resize(f.Verts.size(), glm::vec2(0.0f));
            f.UVs[c] = uv;
        }
        bool SameUV(glm::vec2 a, glm::vec2 b) { return std::abs(a.x - b.x) <= kSameUV && std::abs(a.y - b.y) <= kSameUV; }

        // A tangent frame for projecting along `n` with the engine's UV orientation: seen from the front (looking
        // along -n) u points right and v down. Up is +Y unless n is nearly vertical (then -Z for faces up, +Z down).
        void ProjectionFrame(glm::vec3 n, glm::vec3& right, glm::vec3& up)
        {
            n = glm::normalize(n);
            glm::vec3 ref = std::abs(n.y) > 0.9f ? glm::vec3(0, 0, n.y > 0.0f ? -1.0f : 1.0f) : glm::vec3(0, 1, 0);
            right = glm::normalize(glm::cross(ref, n));
            up = glm::cross(n, right);
        }

        // ---- Charts: faces cut apart into flat pieces, their corners joined into the pieces' vertices.

        struct Chart
        {
            std::vector<uint32_t> Faces;                // mesh faces
            std::vector<std::vector<uint32_t>> Corners; // per chart face: chart vertex per corner
            std::vector<glm::dvec3> Pos;                // per chart vertex (model space)
            std::vector<glm::dvec2> UV;                 // per chart vertex: the solution (v up while solving)
            std::vector<uint8_t> Pinned;
            std::vector<glm::dvec2> PinUV;              // where pinned vertices go (v up)
            std::vector<glm::uvec3> Tris;               // chart vertices
            size_t PinCount = 0;
        };

        // `byUVs`: faces join across edges where their UVs agree (islands); else across edges that are not seams.
        std::vector<Chart> BuildCharts(const EditableMesh& m, const std::vector<uint32_t>& faces, bool byUVs)
        {
            const Topology& t = m.Topo();
            std::vector<int32_t> local(m.FaceCount(), -1);
            for (size_t i = 0; i < faces.size(); ++i) local[faces[i]] = (int32_t)i;
            std::vector<uint32_t> offset(faces.size() + 1, 0);
            for (size_t i = 0; i < faces.size(); ++i) offset[i + 1] = offset[i] + (uint32_t)m.Faces[faces[i]].Verts.size();
            UnionFind corners(offset.back()), parts(faces.size());
            for (size_t i = 0; i < faces.size(); ++i)
            {
                uint32_t f = faces[i];
                const Face& face = m.Faces[f];
                size_t n = face.Verts.size();
                for (size_t c = 0; c < n; ++c)
                {
                    uint32_t e = t.FaceEdges[f][c];
                    if (t.EdgeFaces[e].size() != 2) continue;
                    uint32_t g = t.EdgeFaces[e][0] == f ? t.EdgeFaces[e][1] : t.EdgeFaces[e][0];
                    if (g == f || local[g] < 0 || (size_t)local[g] < i) continue;
                    const Face& other = m.Faces[g];
                    size_t c1 = (c + 1) % n;
                    int ga = CornerOf(other, face.Verts[c]), gb = CornerOf(other, face.Verts[c1]);
                    if (ga < 0 || gb < 0) continue;
                    if (byUVs)
                    {
                        if (!SameUV(UVAt(face, c), UVAt(other, ga)) || !SameUV(UVAt(face, c1), UVAt(other, gb))) continue;
                    }
                    else if (m.GetEdge(t.Edges[e]).Seam)
                        continue;
                    uint32_t j = (uint32_t)local[g];
                    corners.Join(offset[i] + (uint32_t)c, offset[j] + (uint32_t)ga);
                    corners.Join(offset[i] + (uint32_t)c1, offset[j] + (uint32_t)gb);
                    parts.Join((uint32_t)i, j);
                }
            }
            std::map<uint32_t, size_t> chartOf;
            std::vector<Chart> charts;
            std::vector<int64_t> vertOf(offset.back(), -1); // corner root -> chart vertex
            for (size_t i = 0; i < faces.size(); ++i)
            {
                uint32_t root = parts.Find((uint32_t)i);
                auto [it, added] = chartOf.try_emplace(root, charts.size());
                if (added) charts.emplace_back();
                Chart& ch = charts[it->second];
                const Face& face = m.Faces[faces[i]];
                ch.Faces.push_back(faces[i]);
                std::vector<uint32_t> cv(face.Verts.size());
                for (size_t c = 0; c < face.Verts.size(); ++c)
                {
                    uint32_t r = corners.Find(offset[i] + (uint32_t)c);
                    if (vertOf[r] < 0)
                    {
                        vertOf[r] = (int64_t)ch.Pos.size();
                        ch.Pos.push_back(glm::dvec3(m.Positions[face.Verts[c]]));
                        ch.Pinned.push_back(0);
                        ch.PinUV.push_back(glm::dvec2(0.0));
                    }
                    cv[c] = (uint32_t)vertOf[r];
                    if (face.CornerFlags(c) & UVPinned)
                    {
                        if (!ch.Pinned[cv[c]]) ++ch.PinCount;
                        ch.Pinned[cv[c]] = 1;
                        glm::vec2 uv = UVAt(face, c);
                        ch.PinUV[cv[c]] = glm::dvec2(uv.x, -uv.y);
                    }
                }
                std::vector<uint32_t> tri;
                m.TriangulateFace(faces[i], tri);
                for (size_t k = 0; k + 2 < tri.size(); k += 3) ch.Tris.push_back({ cv[tri[k]], cv[tri[k + 1]], cv[tri[k + 2]] });
                ch.Corners.push_back(std::move(cv));
            }
            for (Chart& ch : charts) ch.UV.assign(ch.Pos.size(), glm::dvec2(0.0));
            return charts;
        }

        // A triangle laid flat in its own plane: corner 0 at the origin, corner 1 along +x, corner 2 above (y > 0).
        bool LocalTriangle(const glm::dvec3& p0, const glm::dvec3& p1, const glm::dvec3& p2, glm::dvec2 z[3], double& area)
        {
            glm::dvec3 e1 = p1 - p0, e2 = p2 - p0;
            glm::dvec3 n = glm::cross(e1, e2);
            double nl = glm::length(n), l1 = glm::length(e1);
            area = nl * 0.5;
            if (nl < 1e-14 || l1 < 1e-12) return false;
            glm::dvec3 x = e1 / l1, y = glm::cross(n / nl, x);
            z[0] = glm::dvec2(0.0);
            z[1] = glm::dvec2(l1, 0.0);
            z[2] = glm::dvec2(glm::dot(e2, x), glm::dot(e2, y));
            return true;
        }

        double Area2(const glm::dvec2& a, const glm::dvec2& b, const glm::dvec2& c) { return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x); }

        // The chart's average facing (area weighted), or its largest face's when they cancel out (closed shapes).
        glm::dvec3 ChartNormal(const Chart& ch)
        {
            glm::dvec3 sum(0.0), largest(0.0, 1.0, 0.0);
            double best = -1.0;
            for (const glm::uvec3& t : ch.Tris)
            {
                glm::dvec3 n = glm::cross(ch.Pos[t.y] - ch.Pos[t.x], ch.Pos[t.z] - ch.Pos[t.x]);
                sum += n;
                double l = glm::length(n);
                if (l > best)
                {
                    best = l;
                    largest = n;
                }
            }
            double total = 0.0;
            for (const glm::uvec3& t : ch.Tris) total += glm::length(glm::cross(ch.Pos[t.y] - ch.Pos[t.x], ch.Pos[t.z] - ch.Pos[t.x]));
            if (glm::length(sum) > total * 0.1 && glm::length(sum) > 1e-14) return glm::normalize(sum);
            return best > 1e-14 ? glm::normalize(largest) : glm::dvec3(0, 1, 0);
        }

        // ---- Sparse least squares (CGLS with Jacobi column scaling)

        struct LeastSquares
        {
            size_t Cols = 0;
            std::vector<uint32_t> RowStart{ 0 };
            std::vector<uint32_t> Col;
            std::vector<double> Val, Rhs;
            void Add(uint32_t col, double v)
            {
                Col.push_back(col);
                Val.push_back(v);
            }
            void EndRow(double rhs)
            {
                Rhs.push_back(rhs);
                RowStart.push_back((uint32_t)Col.size());
            }
            size_t Rows() const { return Rhs.size(); }

            // x: the start on input, the solution on output.
            void Solve(std::vector<double>& x, int maxIterations, double tolerance) const
            {
                size_t rows = Rows();
                std::vector<double> scale(Cols, 0.0);
                for (size_t k = 0; k < Col.size(); ++k) scale[Col[k]] += Val[k] * Val[k];
                for (double& s : scale) s = s > 1e-30 ? 1.0 / std::sqrt(s) : 1.0;
                // Solve for y with x = scale * y.
                std::vector<double> y(Cols), r(rows), s(Cols), p(Cols), q(rows);
                for (size_t i = 0; i < Cols; ++i) y[i] = x[i] / scale[i];
                auto mulA = [&](const std::vector<double>& in, std::vector<double>& out) {
                    for (size_t r0 = 0; r0 < rows; ++r0)
                    {
                        double acc = 0.0;
                        for (uint32_t k = RowStart[r0]; k < RowStart[r0 + 1]; ++k) acc += Val[k] * scale[Col[k]] * in[Col[k]];
                        out[r0] = acc;
                    }
                };
                auto mulAT = [&](const std::vector<double>& in, std::vector<double>& out) {
                    std::fill(out.begin(), out.end(), 0.0);
                    for (size_t r0 = 0; r0 < rows; ++r0)
                        for (uint32_t k = RowStart[r0]; k < RowStart[r0 + 1]; ++k) out[Col[k]] += Val[k] * scale[Col[k]] * in[r0];
                };
                mulA(y, r);
                for (size_t i = 0; i < rows; ++i) r[i] = Rhs[i] - r[i];
                mulAT(r, s);
                p = s;
                double gamma = 0.0;
                for (double v : s) gamma += v * v;
                double target = std::max(gamma * tolerance * tolerance, 1e-30);
                for (int it = 0; it < maxIterations && gamma > target; ++it)
                {
                    mulA(p, q);
                    double qq = 0.0;
                    for (double v : q) qq += v * v;
                    if (qq < 1e-300) break;
                    double alpha = gamma / qq;
                    for (size_t i = 0; i < Cols; ++i) y[i] += alpha * p[i];
                    for (size_t i = 0; i < rows; ++i) r[i] -= alpha * q[i];
                    mulAT(r, s);
                    double next = 0.0;
                    for (double v : s) next += v * v;
                    double beta = next / gamma;
                    gamma = next;
                    for (size_t i = 0; i < Cols; ++i) p[i] = s[i] + beta * p[i];
                }
                for (size_t i = 0; i < Cols; ++i) x[i] = y[i] * scale[i];
            }
        };

        // A sparse symmetric system solved by conjugate gradients with a Jacobi preconditioner.
        struct SymmetricSystem
        {
            std::vector<std::vector<std::pair<uint32_t, double>>> Rows;
            void Add(uint32_t i, uint32_t j, double v)
            {
                for (auto& [c, w] : Rows[i])
                    if (c == j)
                    {
                        w += v;
                        return;
                    }
                Rows[i].push_back({ j, v });
            }
            void Solve(const std::vector<double>& b, std::vector<double>& x, int maxIterations, double tolerance) const
            {
                size_t n = Rows.size();
                std::vector<double> diag(n, 1.0), r(n), z(n), p(n), q(n);
                for (size_t i = 0; i < n; ++i)
                    for (auto& [c, w] : Rows[i])
                        if (c == i && std::abs(w) > 1e-30) diag[i] = w;
                auto mul = [&](const std::vector<double>& in, std::vector<double>& out) {
                    for (size_t i = 0; i < n; ++i)
                    {
                        double acc = 0.0;
                        for (auto& [c, w] : Rows[i]) acc += w * in[c];
                        out[i] = acc;
                    }
                };
                mul(x, r);
                double bb = 0.0;
                for (size_t i = 0; i < n; ++i)
                {
                    r[i] = b[i] - r[i];
                    bb += b[i] * b[i];
                    z[i] = r[i] / diag[i];
                }
                p = z;
                double rz = 0.0;
                for (size_t i = 0; i < n; ++i) rz += r[i] * z[i];
                double target = std::max(bb * tolerance * tolerance, 1e-30);
                for (int it = 0; it < maxIterations; ++it)
                {
                    double rr = 0.0;
                    for (double v : r) rr += v * v;
                    if (rr <= target) break;
                    mul(p, q);
                    double pq = 0.0;
                    for (size_t i = 0; i < n; ++i) pq += p[i] * q[i];
                    if (std::abs(pq) < 1e-300) break;
                    double alpha = rz / pq;
                    for (size_t i = 0; i < n; ++i)
                    {
                        x[i] += alpha * p[i];
                        r[i] -= alpha * q[i];
                        z[i] = r[i] / diag[i];
                    }
                    double next = 0.0;
                    for (size_t i = 0; i < n; ++i) next += r[i] * z[i];
                    double beta = next / rz;
                    rz = next;
                    for (size_t i = 0; i < n; ++i) p[i] = z[i] + beta * p[i];
                }
            }
        };

        // ---- Flattening

        // Starting UVs: the chart projected along its average facing; pins: the user's, or the two vertices farthest
        // apart along the projection's longer side. Returns the fixed vertices.
        std::vector<uint8_t> StartChart(Chart& ch, bool usePins)
        {
            glm::dvec3 n = ChartNormal(ch);
            glm::vec3 right, up;
            ProjectionFrame(glm::vec3(n), right, up);
            glm::dvec3 r(right), u(up);
            for (size_t v = 0; v < ch.Pos.size(); ++v) ch.UV[v] = glm::dvec2(glm::dot(ch.Pos[v], r), glm::dot(ch.Pos[v], u));
            std::vector<uint8_t> fixed(ch.Pos.size(), 0);
            if (usePins && ch.PinCount >= 2)
            {
                for (size_t v = 0; v < ch.Pos.size(); ++v)
                    if (ch.Pinned[v])
                    {
                        fixed[v] = 1;
                        ch.UV[v] = ch.PinUV[v];
                    }
                return fixed;
            }
            if (ch.Pos.size() < 2) return fixed;
            glm::dvec2 mn(1e300), mx(-1e300);
            for (const auto& p : ch.UV)
            {
                mn = glm::min(mn, p);
                mx = glm::max(mx, p);
            }
            int axis = (mx.x - mn.x) >= (mx.y - mn.y) ? 0 : 1;
            size_t a = 0, b = 0;
            for (size_t v = 0; v < ch.UV.size(); ++v)
            {
                if (ch.UV[v][axis] < ch.UV[a][axis]) a = v;
                if (ch.UV[v][axis] > ch.UV[b][axis]) b = v;
            }
            if (a == b || glm::length(ch.UV[a] - ch.UV[b]) < 1e-12)
            {
                // Degenerate projection: the two vertices farthest apart in space.
                double best = -1.0;
                for (size_t v = 1; v < ch.Pos.size(); ++v)
                    if (double d = glm::length(ch.Pos[v] - ch.Pos[0]); d > best)
                    {
                        best = d;
                        b = v;
                    }
                a = 0;
                ch.UV[a] = glm::dvec2(0.0);
                ch.UV[b] = glm::dvec2(std::max(best, 1e-6), 0.0);
            }
            fixed[a] = fixed[b] = 1;
            return fixed;
        }

        void SolveConformal(Chart& ch, const std::vector<uint8_t>& fixed)
        {
            size_t n = ch.Pos.size();
            std::vector<int64_t> index(n, -1);
            size_t freeCount = 0;
            for (size_t v = 0; v < n; ++v)
                if (!fixed[v]) index[v] = (int64_t)freeCount++;
            if (freeCount == 0) return;
            LeastSquares ls;
            ls.Cols = freeCount * 2;
            for (const glm::uvec3& t : ch.Tris)
            {
                glm::dvec2 z[3];
                double area;
                if (!LocalTriangle(ch.Pos[t.x], ch.Pos[t.y], ch.Pos[t.z], z, area)) continue;
                double s = 1.0 / std::sqrt(2.0 * area);
                uint32_t id[3] = { t.x, t.y, t.z };
                // sum_j W_j U_j = 0 with W_j = z_k - z_l (j, k, l in turn): U is a similarity of z on the triangle.
                glm::dvec2 w[3] = { z[1] - z[2], z[2] - z[0], z[0] - z[1] };
                for (int row = 0; row < 2; ++row)
                {
                    double rhs = 0.0;
                    for (int j = 0; j < 3; ++j)
                    {
                        // real: wr u - wi v; imaginary: wi u + wr v
                        double cu = (row == 0 ? w[j].x : w[j].y) * s, cv = (row == 0 ? -w[j].y : w[j].x) * s;
                        if (fixed[id[j]])
                            rhs -= cu * ch.UV[id[j]].x + cv * ch.UV[id[j]].y;
                        else
                        {
                            ls.Add((uint32_t)index[id[j]], cu);
                            ls.Add((uint32_t)(index[id[j]] + freeCount), cv);
                        }
                    }
                    ls.EndRow(rhs);
                }
            }
            std::vector<double> x(freeCount * 2);
            for (size_t v = 0; v < n; ++v)
                if (index[v] >= 0)
                {
                    x[(size_t)index[v]] = ch.UV[v].x;
                    x[(size_t)index[v] + freeCount] = ch.UV[v].y;
                }
            ls.Solve(x, (int)std::clamp<size_t>(freeCount * 8, 200, 20000), 1e-10);
            for (size_t v = 0; v < n; ++v)
                if (index[v] >= 0) ch.UV[v] = glm::dvec2(x[(size_t)index[v]], x[(size_t)index[v] + freeCount]);
        }

        // As-rigid-as-possible: alternately the closest rotation of each triangle onto its true shape, and the UVs best
        // fitting those rotated triangles (cotangent weights). `fixed`: vertices that stay (at least one).
        void SolveRigid(Chart& ch, std::vector<uint8_t> fixed, int iterations)
        {
            size_t n = ch.Pos.size();
            if (std::find(fixed.begin(), fixed.end(), 1) == fixed.end() && n > 0) fixed[0] = 1;
            struct Tri
            {
                uint32_t V[3];
                glm::dvec2 Z[3];
                double Cot[3]; // weight of the edge opposite each corner
            };
            std::vector<Tri> tris;
            for (const glm::uvec3& t : ch.Tris)
            {
                Tri tri{ { t.x, t.y, t.z }, {}, {} };
                double area;
                if (!LocalTriangle(ch.Pos[t.x], ch.Pos[t.y], ch.Pos[t.z], tri.Z, area)) continue;
                for (int k = 0; k < 3; ++k)
                {
                    glm::dvec2 a = tri.Z[(k + 1) % 3] - tri.Z[k], b = tri.Z[(k + 2) % 3] - tri.Z[k];
                    double cr = std::abs(a.x * b.y - a.y * b.x);
                    tri.Cot[k] = std::clamp(glm::dot(a, b) / std::max(cr, 1e-30), 1e-3, 1e3) * 0.5;
                }
                tris.push_back(tri);
            }
            if (tris.empty()) return;
            std::vector<int64_t> index(n, -1);
            size_t freeCount = 0;
            for (size_t v = 0; v < n; ++v)
                if (!fixed[v]) index[v] = (int64_t)freeCount++;
            if (freeCount == 0) return;
            SymmetricSystem L;
            L.Rows.resize(freeCount);
            for (const Tri& t : tris)
                for (int k = 0; k < 3; ++k)
                {
                    uint32_t i = t.V[(k + 1) % 3], j = t.V[(k + 2) % 3];
                    double w = t.Cot[k];
                    if (index[i] >= 0)
                    {
                        L.Add((uint32_t)index[i], (uint32_t)index[i], w);
                        if (index[j] >= 0) L.Add((uint32_t)index[i], (uint32_t)index[j], -w);
                    }
                    if (index[j] >= 0)
                    {
                        L.Add((uint32_t)index[j], (uint32_t)index[j], w);
                        if (index[i] >= 0) L.Add((uint32_t)index[j], (uint32_t)index[i], -w);
                    }
                }
            std::vector<double> bu(freeCount), bv(freeCount), xu(freeCount), xv(freeCount);
            for (int it = 0; it < iterations; ++it)
            {
                std::fill(bu.begin(), bu.end(), 0.0);
                std::fill(bv.begin(), bv.end(), 0.0);
                for (const Tri& t : tris)
                {
                    // Local step: the rotation best taking the true triangle onto its UVs.
                    double a = 0, b = 0, c = 0, d = 0;
                    for (int k = 0; k < 3; ++k)
                    {
                        uint32_t i = (k + 1) % 3, j = (k + 2) % 3;
                        glm::dvec2 du = ch.UV[t.V[i]] - ch.UV[t.V[j]], dz = t.Z[i] - t.Z[j];
                        a += t.Cot[k] * du.x * dz.x;
                        b += t.Cot[k] * du.x * dz.y;
                        c += t.Cot[k] * du.y * dz.x;
                        d += t.Cot[k] * du.y * dz.y;
                    }
                    double angle = std::atan2(c - b, a + d), cs = std::cos(angle), sn = std::sin(angle);
                    // A triangle flipped in UV space would pick a reflection; the rotation keeps it from staying flipped.
                    for (int k = 0; k < 3; ++k)
                    {
                        uint32_t i = (k + 1) % 3, j = (k + 2) % 3;
                        glm::dvec2 dz = t.Z[i] - t.Z[j];
                        glm::dvec2 rz(cs * dz.x - sn * dz.y, sn * dz.x + cs * dz.y);
                        double w = t.Cot[k];
                        auto add = [&](uint32_t v, uint32_t other, glm::dvec2 g) {
                            if (index[v] < 0) return;
                            size_t row = (size_t)index[v];
                            bu[row] += w * g.x;
                            bv[row] += w * g.y;
                            if (index[other] < 0)
                            {
                                bu[row] += w * ch.UV[other].x;
                                bv[row] += w * ch.UV[other].y;
                            }
                        };
                        add(t.V[i], t.V[j], rz);
                        add(t.V[j], t.V[i], -rz);
                    }
                }
                for (size_t v = 0; v < n; ++v)
                    if (index[v] >= 0)
                    {
                        xu[(size_t)index[v]] = ch.UV[v].x;
                        xv[(size_t)index[v]] = ch.UV[v].y;
                    }
                int inner = (int)std::clamp<size_t>(freeCount * 2, 50, 2000);
                L.Solve(bu, xu, inner, 1e-9);
                L.Solve(bv, xv, inner, 1e-9);
                for (size_t v = 0; v < n; ++v)
                    if (index[v] >= 0) ch.UV[v] = glm::dvec2(xu[(size_t)index[v]], xv[(size_t)index[v]]);
            }
        }

        double SurfaceArea(const Chart& ch)
        {
            double a = 0.0;
            for (const glm::uvec3& t : ch.Tris) a += glm::length(glm::cross(ch.Pos[t.y] - ch.Pos[t.x], ch.Pos[t.z] - ch.Pos[t.x])) * 0.5;
            return a;
        }

        double UVArea(const Chart& ch)
        {
            double a = 0.0;
            for (const glm::uvec3& t : ch.Tris) a += Area2(ch.UV[t.x], ch.UV[t.y], ch.UV[t.z]) * 0.5;
            return a;
        }

        // Scales the chart's UVs about their center so its UV area matches `target`; a chart that came out mirrored is
        // flipped back.
        void ScaleChart(Chart& ch, double target)
        {
            double area = UVArea(ch);
            if (area < 0.0)
            {
                for (auto& uv : ch.UV) uv.x = -uv.x;
                area = -area;
            }
            if (area < 1e-20 || target <= 0.0) return;
            glm::dvec2 c(0.0);
            for (const auto& uv : ch.UV) c += uv;
            c /= (double)std::max<size_t>(ch.UV.size(), 1);
            double s = std::sqrt(target / area);
            for (auto& uv : ch.UV) uv = c + (uv - c) * s;
        }

        void WriteChart(EditableMesh& m, const Chart& ch)
        {
            for (size_t i = 0; i < ch.Faces.size(); ++i)
            {
                Face& face = m.Faces[ch.Faces[i]];
                for (size_t c = 0; c < face.Verts.size(); ++c)
                {
                    glm::dvec2 uv = ch.UV[ch.Corners[i][c]];
                    SetUV(face, c, glm::vec2((float)uv.x, (float)-uv.y));
                }
            }
        }

        // ---- Packing

        std::vector<glm::vec2> ConvexHull(std::vector<glm::vec2> pts)
        {
            std::sort(pts.begin(), pts.end(), [](glm::vec2 a, glm::vec2 b) { return a.x < b.x || (a.x == b.x && a.y < b.y); });
            pts.erase(std::unique(pts.begin(), pts.end()), pts.end());
            if (pts.size() < 3) return pts;
            std::vector<glm::vec2> h(pts.size() * 2);
            size_t k = 0;
            auto cross = [](glm::vec2 o, glm::vec2 a, glm::vec2 b) { return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x); };
            for (size_t i = 0; i < pts.size(); ++i)
            {
                while (k >= 2 && cross(h[k - 2], h[k - 1], pts[i]) <= 0.0f) --k;
                h[k++] = pts[i];
            }
            for (size_t i = pts.size() - 1, t = k + 1; i > 0; --i)
            {
                while (k >= t && cross(h[k - 2], h[k - 1], pts[i - 1]) <= 0.0f) --k;
                h[k++] = pts[i - 1];
            }
            h.resize(k - 1);
            return h;
        }

        struct PackRect
        {
            size_t Id;
            float W, H, X = 0.0f, Y = 0.0f;
        };

        // Skyline bottom-left packing in a strip `width` wide; returns the height used.
        float SkylinePack(std::vector<PackRect>& rects, float width)
        {
            struct Seg
            {
                float X, Y, W;
            };
            std::vector<Seg> sky{ { 0.0f, 0.0f, width } };
            float top = 0.0f;
            for (PackRect& r : rects)
            {
                float bestY = 1e30f, bestX = 0.0f;
                for (size_t i = 0; i < sky.size(); ++i)
                {
                    float x = sky[i].X;
                    if (x + r.W > width + 1e-5f * width) break;
                    float y = 0.0f;
                    for (size_t j = i; j < sky.size() && sky[j].X < x + r.W - 1e-7f; ++j) y = std::max(y, sky[j].Y);
                    if (y + r.H < bestY + r.H - 1e-7f || (std::abs(y - bestY) <= 1e-7f && x < bestX))
                    {
                        bestY = y;
                        bestX = x;
                    }
                }
                if (bestY >= 1e29f)
                {
                    bestX = 0.0f;
                    bestY = top;
                }
                r.X = bestX;
                r.Y = bestY;
                top = std::max(top, bestY + r.H);
                std::vector<Seg> next;
                float x0 = bestX, x1 = bestX + r.W;
                for (const Seg& s : sky)
                {
                    float s1 = s.X + s.W;
                    if (s1 <= x0 || s.X >= x1)
                    {
                        next.push_back(s);
                        continue;
                    }
                    if (s.X < x0) next.push_back({ s.X, s.Y, x0 - s.X });
                    if (s1 > x1) next.push_back({ x1, s.Y, s1 - x1 });
                }
                next.push_back({ x0, bestY + r.H, r.W });
                std::sort(next.begin(), next.end(), [](const Seg& a, const Seg& b) { return a.X < b.X; });
                sky.clear();
                for (const Seg& s : next)
                    if (!sky.empty() && std::abs(sky.back().Y - s.Y) < 1e-7f) sky.back().W = s.X + s.W - sky.back().X;
                    else sky.push_back(s);
            }
            return top;
        }

        // Packs groups of faces (each moved as one piece) into 0..1.
        void PackGroups(EditableMesh& m, const std::vector<std::vector<uint32_t>>& groups, float margin, bool rotate)
        {
            if (groups.empty()) return;
            std::vector<glm::vec2> sizes(groups.size());
            float area = 0.0f, maxSide = 0.0f;
            for (size_t g = 0; g < groups.size(); ++g)
            {
                std::vector<glm::vec2> pts;
                for (uint32_t f : groups[g])
                    for (size_t c = 0; c < m.Faces[f].Verts.size(); ++c) pts.push_back(UVAt(m.Faces[f], c));
                float angle = 0.0f;
                if (rotate)
                {
                    std::vector<glm::vec2> hull = ConvexHull(pts);
                    float bestArea = 1e30f;
                    for (size_t i = 0; i < hull.size(); ++i)
                    {
                        glm::vec2 e = hull[(i + 1) % hull.size()] - hull[i];
                        if (glm::dot(e, e) < 1e-20f) continue;
                        float a = -std::atan2(e.y, e.x), cs = std::cos(a), sn = std::sin(a);
                        glm::vec2 mn(1e30f), mx(-1e30f);
                        for (glm::vec2 p : hull)
                        {
                            glm::vec2 q(cs * p.x - sn * p.y, sn * p.x + cs * p.y);
                            mn = glm::min(mn, q);
                            mx = glm::max(mx, q);
                        }
                        glm::vec2 ext = mx - mn;
                        float boxArea = ext.x * ext.y;
                        if (boxArea < bestArea - 1e-12f)
                        {
                            bestArea = boxArea;
                            angle = ext.y > ext.x ? a + glm::half_pi<float>() : a; // lying down
                        }
                    }
                }
                float cs = std::cos(angle), sn = std::sin(angle);
                glm::vec2 mn(1e30f), mx(-1e30f);
                for (uint32_t f : groups[g])
                    for (size_t c = 0; c < m.Faces[f].Verts.size(); ++c)
                    {
                        glm::vec2 p = UVAt(m.Faces[f], c), q(cs * p.x - sn * p.y, sn * p.x + cs * p.y);
                        SetUV(m.Faces[f], c, q);
                        mn = glm::min(mn, q);
                        mx = glm::max(mx, q);
                    }
                for (uint32_t f : groups[g])
                    for (size_t c = 0; c < m.Faces[f].Verts.size(); ++c) SetUV(m.Faces[f], c, UVAt(m.Faces[f], c) - mn);
                sizes[g] = glm::max(mx - mn, glm::vec2(1e-6f));
                area += sizes[g].x * sizes[g].y;
                maxSide = std::max({ maxSide, sizes[g].x, sizes[g].y });
            }
            std::vector<size_t> order(groups.size());
            std::iota(order.begin(), order.end(), size_t(0));
            std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
                return sizes[a].y > sizes[b].y || (sizes[a].y == sizes[b].y && sizes[a].x > sizes[b].x);
            });
            float side = std::max(std::sqrt(area), maxSide);
            std::vector<PackRect> best;
            float pad = 0.0f;
            for (int pass = 0; pass < 3; ++pass)
            {
                pad = margin * 0.5f * side;
                float padded = 0.0f, widest = 0.0f;
                for (size_t g : order)
                {
                    padded += (sizes[g].x + 2 * pad) * (sizes[g].y + 2 * pad);
                    widest = std::max(widest, sizes[g].x + 2 * pad);
                }
                float bestSide = 1e30f;
                for (float k : { 1.0f, 1.05f, 1.1f, 1.2f, 1.3f, 1.45f, 1.6f, 1.8f, 2.0f })
                {
                    float w = std::max(widest, std::sqrt(padded) * k);
                    std::vector<PackRect> rects;
                    for (size_t g : order) rects.push_back({ g, sizes[g].x + 2 * pad, sizes[g].y + 2 * pad });
                    float h = SkylinePack(rects, w);
                    float used = 0.0f;
                    for (const PackRect& r : rects) used = std::max(used, r.X + r.W);
                    float s = std::max(used, h);
                    if (s < bestSide)
                    {
                        bestSide = s;
                        best = std::move(rects);
                    }
                }
                if (std::abs(bestSide - side) < side * 1e-3f) break;
                side = bestSide;
            }
            float usedSide = 0.0f;
            for (const PackRect& r : best) usedSide = std::max({ usedSide, r.X + r.W, r.Y + r.H });
            side = std::max(usedSide, 1e-9f);
            for (const PackRect& r : best)
                for (uint32_t f : groups[r.Id])
                    for (size_t c = 0; c < m.Faces[f].Verts.size(); ++c)
                        SetUV(m.Faces[f], c, (UVAt(m.Faces[f], c) + glm::vec2(r.X + pad, r.Y + pad)) / side);
        }

        std::vector<std::vector<uint32_t>> GroupsOf(const std::vector<Chart>& charts)
        {
            std::vector<std::vector<uint32_t>> g;
            for (const Chart& ch : charts) g.push_back(ch.Faces);
            return g;
        }

        // The faces' vertices' bounds (model space).
        void FaceBounds(const EditableMesh& m, const std::vector<uint32_t>& faces, glm::vec3& mn, glm::vec3& mx)
        {
            mn = glm::vec3(1e30f);
            mx = glm::vec3(-1e30f);
            for (uint32_t f : faces)
                for (uint32_t v : m.Faces[f].Verts)
                {
                    mn = glm::min(mn, m.Positions[v]);
                    mx = glm::max(mx, m.Positions[v]);
                }
            if (faces.empty()) mn = mx = glm::vec3(0.0f);
        }

        // Axis frames for wrapping: `along` the axis, `a` and `b` across it (right-handed with along).
        void AxisFrame(int axis, glm::vec3& along, glm::vec3& a, glm::vec3& b)
        {
            axis = std::clamp(axis, 0, 2);
            along = glm::vec3(0.0f);
            along[axis] = 1.0f;
            // Y: a = Z (the front), b = X; u = 0.5 at the front and grows to the right seen from outside, the wrap at the back.
            a = glm::vec3(0.0f);
            b = glm::vec3(0.0f);
            a[(axis + 1) % 3] = 1.0f;
            b[(axis + 2) % 3] = 1.0f;
        }

        // A face whose u values run across the wrap (from ~1 back to ~0) gets the small ones moved past 1.
        void FixWrap(Face& face)
        {
            float mn = 1e30f, mx = -1e30f;
            for (const glm::vec2& uv : face.UVs)
            {
                mn = std::min(mn, uv.x);
                mx = std::max(mx, uv.x);
            }
            if (mx - mn <= 0.5f) return;
            for (glm::vec2& uv : face.UVs)
                if (uv.x < 0.5f) uv.x += 1.0f;
        }
    }

    std::vector<uint32_t> SelectedFaces(const EditableMesh& m)
    {
        std::vector<uint32_t> out;
        for (uint32_t f = 0; f < m.FaceCount(); ++f)
            if ((m.Faces[f].Flags & Selected) && !(m.Faces[f].Flags & Hidden)) out.push_back(f);
        return out;
    }

    std::vector<std::vector<uint32_t>> Islands(const EditableMesh& m, const std::vector<uint32_t>& faces)
    {
        std::vector<Chart> charts = BuildCharts(m, faces, true);
        return GroupsOf(charts);
    }

    // ---- Unwrapping and projections

    int Unwrap(EditableMesh& m, const std::vector<uint32_t>& faces, const UnwrapSettings& settings)
    {
        if (faces.empty()) return 0;
        std::vector<Chart> charts = BuildCharts(m, faces, false);
        std::vector<std::vector<uint32_t>> toPack;
        for (Chart& ch : charts)
        {
            if (!settings.UsePins) ch.PinCount = 0;
            std::vector<uint8_t> fixed = StartChart(ch, settings.UsePins);
            SolveConformal(ch, fixed);
            bool pinned = settings.UsePins && ch.PinCount >= 2;
            if (!pinned)
            {
                ScaleChart(ch, SurfaceArea(ch));
                if (settings.Method == UnwrapMethod::AngleBased)
                {
                    // Free of the conformal pins: one vertex holds it in place while it settles.
                    std::vector<uint8_t> one(ch.Pos.size(), 0);
                    if (!one.empty()) one[0] = 1;
                    SolveRigid(ch, one, settings.Iterations);
                    ScaleChart(ch, std::abs(UVArea(ch)));
                }
            }
            else if (settings.Method == UnwrapMethod::AngleBased)
                SolveRigid(ch, fixed, settings.Iterations);
            if (settings.UsePins && ch.PinCount == 1)
            {
                // One pin: the chart moves so that vertex lands on it.
                for (size_t v = 0; v < ch.Pos.size(); ++v)
                    if (ch.Pinned[v])
                    {
                        glm::dvec2 d = ch.PinUV[v] - ch.UV[v];
                        for (auto& uv : ch.UV) uv += d;
                    }
            }
            WriteChart(m, ch);
            if (!pinned && !(settings.UsePins && ch.PinCount == 1)) toPack.push_back(ch.Faces);
        }
        if (settings.Pack) PackGroups(m, toPack, settings.Margin, true);
        m.Changed();
        return (int)charts.size();
    }

    int SmartProject(EditableMesh& m, const std::vector<uint32_t>& faces, float angleLimit, float margin)
    {
        if (faces.empty()) return 0;
        float limit = std::cos(glm::radians(std::clamp(angleLimit, 1.0f, 89.0f)));
        std::vector<glm::vec3> normal(faces.size());
        std::vector<float> area(faces.size());
        for (size_t i = 0; i < faces.size(); ++i)
        {
            normal[i] = m.FaceNormal(faces[i]);
            area[i] = m.FaceArea(faces[i]);
        }
        // Projection directions: the largest faces not yet covered by one.
        std::vector<size_t> bySize(faces.size());
        std::iota(bySize.begin(), bySize.end(), size_t(0));
        std::sort(bySize.begin(), bySize.end(), [&](size_t a, size_t b) { return area[a] > area[b]; });
        std::vector<glm::vec3> dirs;
        for (size_t i : bySize)
        {
            if (normal[i] == glm::vec3(0.0f)) continue;
            bool covered = false;
            for (const glm::vec3& d : dirs)
                if (glm::dot(d, normal[i]) >= limit)
                {
                    covered = true;
                    break;
                }
            if (!covered) dirs.push_back(normal[i]);
        }
        if (dirs.empty()) dirs.push_back(glm::vec3(0, 1, 0));
        std::vector<int> dirOf(faces.size(), 0);
        for (int pass = 0; pass < 2; ++pass)
        {
            for (size_t i = 0; i < faces.size(); ++i)
            {
                float best = -2.0f;
                for (size_t d = 0; d < dirs.size(); ++d)
                    if (float dot = glm::dot(dirs[d], normal[i]); dot > best)
                    {
                        best = dot;
                        dirOf[i] = (int)d;
                    }
            }
            if (pass == 1) break;
            // Each direction moves to the mean of its faces'.
            std::vector<glm::vec3> sum(dirs.size(), glm::vec3(0.0f));
            for (size_t i = 0; i < faces.size(); ++i) sum[(size_t)dirOf[i]] += normal[i] * area[i];
            for (size_t d = 0; d < dirs.size(); ++d)
                if (glm::length(sum[d]) > 1e-12f) dirs[d] = glm::normalize(sum[d]);
        }
        // Charts: faces of the same direction joined across edges.
        const Topology& t = m.Topo();
        std::vector<int32_t> local(m.FaceCount(), -1);
        for (size_t i = 0; i < faces.size(); ++i) local[faces[i]] = (int32_t)i;
        UnionFind parts(faces.size());
        for (size_t i = 0; i < faces.size(); ++i)
            for (uint32_t e : t.FaceEdges[faces[i]])
                for (uint32_t g : t.EdgeFaces[e])
                    if (int32_t j = local[g]; j >= 0 && (size_t)j != i && dirOf[(size_t)j] == dirOf[i]) parts.Join((uint32_t)i, (uint32_t)j);
        std::map<uint32_t, std::vector<uint32_t>> groups;
        for (size_t i = 0; i < faces.size(); ++i)
        {
            groups[parts.Find((uint32_t)i)].push_back(faces[i]);
            glm::vec3 right, up;
            ProjectionFrame(dirs[(size_t)dirOf[i]], right, up);
            Face& face = m.Faces[faces[i]];
            for (size_t c = 0; c < face.Verts.size(); ++c)
            {
                glm::vec3 p = m.Positions[face.Verts[c]];
                SetUV(face, c, glm::vec2(glm::dot(p, right), -glm::dot(p, up)));
            }
        }
        std::vector<std::vector<uint32_t>> list;
        for (auto& [k, g] : groups) list.push_back(std::move(g));
        PackGroups(m, list, margin, true);
        m.Changed();
        return (int)list.size();
    }

    void CubeProject(EditableMesh& m, const std::vector<uint32_t>& faces, float cubeSize)
    {
        glm::vec3 mn, mx;
        FaceBounds(m, faces, mn, mx);
        glm::vec3 center = (mn + mx) * 0.5f;
        float size = std::max(cubeSize, 1e-6f);
        for (uint32_t f : faces)
        {
            glm::vec3 n = m.FaceNormal(f);
            int axis = std::abs(n.x) >= std::abs(n.y) && std::abs(n.x) >= std::abs(n.z) ? 0 : std::abs(n.y) >= std::abs(n.z) ? 1 : 2;
            glm::vec3 dir(0.0f);
            dir[axis] = n[axis] >= 0.0f ? 1.0f : -1.0f;
            glm::vec3 right, up;
            ProjectionFrame(dir, right, up);
            Face& face = m.Faces[f];
            for (size_t c = 0; c < face.Verts.size(); ++c)
            {
                glm::vec3 p = m.Positions[face.Verts[c]] - center;
                SetUV(face, c, glm::vec2(glm::dot(p, right), -glm::dot(p, up)) / size + 0.5f);
            }
        }
        m.Changed();
    }

    void CylinderProject(EditableMesh& m, const std::vector<uint32_t>& faces, int axis)
    {
        glm::vec3 mn, mx, along, a, b;
        FaceBounds(m, faces, mn, mx);
        AxisFrame(axis, along, a, b);
        glm::vec3 center = (mn + mx) * 0.5f;
        float lo = glm::dot(mn, along), hi = glm::dot(mx, along), height = std::max(hi - lo, 1e-6f);
        for (uint32_t f : faces)
        {
            Face& face = m.Faces[f];
            for (size_t c = 0; c < face.Verts.size(); ++c)
            {
                glm::vec3 p = m.Positions[face.Verts[c]] - center;
                float u = std::atan2(glm::dot(p, b), glm::dot(p, a)) / (2.0f * glm::pi<float>()) + 0.5f;
                float v = (hi - glm::dot(m.Positions[face.Verts[c]], along)) / height;
                SetUV(face, c, glm::vec2(u, v));
            }
            FixWrap(face);
        }
        m.Changed();
    }

    void SphereProject(EditableMesh& m, const std::vector<uint32_t>& faces, int axis)
    {
        glm::vec3 mn, mx, along, a, b;
        FaceBounds(m, faces, mn, mx);
        AxisFrame(axis, along, a, b);
        glm::vec3 center = (mn + mx) * 0.5f;
        for (uint32_t f : faces)
        {
            Face& face = m.Faces[f];
            std::vector<uint8_t> pole(face.Verts.size(), 0);
            for (size_t c = 0; c < face.Verts.size(); ++c)
            {
                glm::vec3 p = m.Positions[face.Verts[c]] - center;
                float len = glm::length(p);
                float across = std::sqrt(glm::dot(p, a) * glm::dot(p, a) + glm::dot(p, b) * glm::dot(p, b));
                float u = std::atan2(glm::dot(p, b), glm::dot(p, a)) / (2.0f * glm::pi<float>()) + 0.5f;
                float v = len > 1e-9f ? std::acos(std::clamp(glm::dot(p, along) / len, -1.0f, 1.0f)) / glm::pi<float>() : 0.5f;
                pole[c] = across < len * 1e-4f;
                SetUV(face, c, glm::vec2(u, v));
            }
            // Corners on a pole take the face's other corners' u (else they fan out across the whole width).
            float sum = 0.0f;
            int count = 0;
            FixWrap(face);
            for (size_t c = 0; c < face.Verts.size(); ++c)
                if (!pole[c])
                {
                    sum += face.UVs[c].x;
                    ++count;
                }
            if (count)
                for (size_t c = 0; c < face.Verts.size(); ++c)
                    if (pole[c]) face.UVs[c].x = sum / (float)count;
        }
        m.Changed();
    }

    void ProjectFromView(EditableMesh& m, const std::vector<uint32_t>& faces, const glm::mat4& modelToClip, float aspect, bool scaleToBounds)
    {
        glm::vec2 mn(1e30f), mx(-1e30f);
        for (uint32_t f : faces)
        {
            Face& face = m.Faces[f];
            for (size_t c = 0; c < face.Verts.size(); ++c)
            {
                glm::vec4 clip = modelToClip * glm::vec4(m.Positions[face.Verts[c]], 1.0f);
                float w = std::abs(clip.w) > 1e-9f ? clip.w : 1e-9f;
                glm::vec2 uv(clip.x / w * std::max(aspect, 1e-6f) * 0.5f + 0.5f, 0.5f - clip.y / w * 0.5f);
                SetUV(face, c, uv);
                mn = glm::min(mn, uv);
                mx = glm::max(mx, uv);
            }
        }
        if (scaleToBounds && !faces.empty())
        {
            glm::vec2 ext = glm::max(mx - mn, glm::vec2(1e-9f));
            for (uint32_t f : faces)
                for (glm::vec2& uv : m.Faces[f].UVs) uv = (uv - mn) / ext;
        }
        m.Changed();
    }

    void Reset(EditableMesh& m, const std::vector<uint32_t>& faces)
    {
        for (uint32_t f : faces)
        {
            Face& face = m.Faces[f];
            size_t n = face.Verts.size();
            face.UVs.resize(n);
            if (n == 4)
            {
                face.UVs = { { 0, 1 }, { 1, 1 }, { 1, 0 }, { 0, 0 } };
                continue;
            }
            for (size_t c = 0; c < n; ++c)
            {
                // Counter-clockwise from the bottom (v down), the first edge along the bottom.
                float a = -glm::half_pi<float>() - glm::pi<float>() / (float)n + 2.0f * glm::pi<float>() * (float)c / (float)n;
                face.UVs[c] = glm::vec2(0.5f + 0.5f * std::cos(a), 0.5f - 0.5f * std::sin(a));
            }
        }
        m.Changed();
    }

    // ---- Islands

    void PackIslands(EditableMesh& m, const std::vector<uint32_t>& faces, float margin, bool rotate)
    {
        PackGroups(m, Islands(m, faces), margin, rotate);
        m.Changed();
    }

    void AverageIslandScale(EditableMesh& m, const std::vector<uint32_t>& faces)
    {
        std::vector<Chart> charts = BuildCharts(m, faces, true);
        double total3 = 0.0, total2 = 0.0;
        std::vector<double> a3(charts.size()), a2(charts.size());
        for (size_t i = 0; i < charts.size(); ++i)
        {
            Chart& ch = charts[i];
            for (size_t k = 0; k < ch.Faces.size(); ++k)
            {
                const Face& face = m.Faces[ch.Faces[k]];
                for (size_t c = 0; c < face.Verts.size(); ++c)
                {
                    glm::vec2 uv = UVAt(face, c);
                    ch.UV[ch.Corners[k][c]] = glm::dvec2(uv.x, -uv.y);
                }
            }
            a3[i] = SurfaceArea(ch);
            a2[i] = std::abs(UVArea(ch));
            total3 += a3[i];
            total2 += a2[i];
        }
        if (total3 <= 0.0 || total2 <= 0.0) return;
        double density = total2 / total3;
        for (size_t i = 0; i < charts.size(); ++i)
        {
            if (a2[i] < 1e-20) continue;
            Chart& ch = charts[i];
            glm::dvec2 mn(1e300), mx(-1e300);
            for (const auto& uv : ch.UV)
            {
                mn = glm::min(mn, uv);
                mx = glm::max(mx, uv);
            }
            glm::dvec2 c = (mn + mx) * 0.5;
            double s = std::sqrt(a3[i] * density / a2[i]);
            for (auto& uv : ch.UV) uv = c + (uv - c) * s;
            WriteChart(m, ch);
        }
        m.Changed();
    }

    size_t SeamsFromIslands(EditableMesh& m, const std::vector<uint32_t>& faces)
    {
        const Topology& t = m.Topo();
        std::vector<uint8_t> in(m.FaceCount(), 0);
        for (uint32_t f : faces) in[f] = 1;
        size_t count = 0;
        for (size_t e = 0; e < t.Edges.size(); ++e)
        {
            if (t.EdgeFaces[e].size() != 2) continue;
            uint32_t f = t.EdgeFaces[e][0], g = t.EdgeFaces[e][1];
            if (!in[f] || !in[g] || f == g) continue;
            uint32_t a = EdgeA(t.Edges[e]), b = EdgeB(t.Edges[e]);
            const Face &ff = m.Faces[f], &gf = m.Faces[g];
            int fa = CornerOf(ff, a), fb = CornerOf(ff, b), ga = CornerOf(gf, a), gb = CornerOf(gf, b);
            if (fa < 0 || fb < 0 || ga < 0 || gb < 0) continue;
            bool joined = SameUV(UVAt(ff, (size_t)fa), UVAt(gf, (size_t)ga)) && SameUV(UVAt(ff, (size_t)fb), UVAt(gf, (size_t)gb));
            if (!joined && !m.GetEdge(t.Edges[e]).Seam)
            {
                m.EditEdge(t.Edges[e]).Seam = true;
                ++count;
            }
        }
        return count;
    }

    void Relax(EditableMesh& m, const std::vector<uint32_t>& faces, int iterations)
    {
        std::vector<Chart> charts = BuildCharts(m, faces, true);
        bool anySelected = !SelectedCorners(m, faces).empty();
        for (Chart& ch : charts)
        {
            bool selected = !anySelected;
            for (size_t k = 0; k < ch.Faces.size() && !selected; ++k)
                for (size_t c = 0; c < ch.Corners[k].size(); ++c)
                    if (m.Faces[ch.Faces[k]].CornerFlags(c) & UVSelected) selected = true;
            if (!selected) continue;
            for (size_t k = 0; k < ch.Faces.size(); ++k)
            {
                const Face& face = m.Faces[ch.Faces[k]];
                for (size_t c = 0; c < face.Verts.size(); ++c)
                {
                    glm::vec2 uv = UVAt(face, c);
                    ch.UV[ch.Corners[k][c]] = glm::dvec2(uv.x, -uv.y);
                }
            }
            double before = UVArea(ch);
            if (std::abs(before) < 1e-20) continue;
            glm::dvec2 center(0.0);
            for (const auto& uv : ch.UV) center += uv;
            center /= (double)ch.UV.size();
            std::vector<uint8_t> fixed(ch.Pos.size(), 0);
            for (size_t v = 0; v < ch.Pos.size(); ++v) fixed[v] = ch.Pinned[v];
            bool pinned = ch.PinCount > 0;
            if (before < 0.0)
                for (auto& uv : ch.UV) uv.x = 2.0 * center.x - uv.x; // (mirrored islands relax mirrored)
            // Work at the surface's own scale, then back to the island's size.
            double scale = std::sqrt(SurfaceArea(ch) / std::abs(before));
            if (!pinned)
                for (auto& uv : ch.UV) uv = center + (uv - center) * scale;
            SolveRigid(ch, fixed, iterations);
            if (!pinned)
            {
                ScaleChart(ch, std::abs(before));
                glm::dvec2 now(0.0);
                for (const auto& uv : ch.UV) now += uv;
                now /= (double)ch.UV.size();
                for (auto& uv : ch.UV) uv += center - now;
            }
            if (before < 0.0)
                for (auto& uv : ch.UV) uv.x = 2.0 * center.x - uv.x;
            WriteChart(m, ch);
        }
        m.Changed();
    }

    std::vector<float> FaceStretch(const EditableMesh& m, const std::vector<uint32_t>& faces, StretchMode mode)
    {
        std::vector<float> out(faces.size(), 0.0f);
        double total3 = 0.0, total2 = 0.0;
        std::vector<double> f3(faces.size(), 0.0), f2(faces.size(), 0.0);
        std::vector<uint32_t> tri;
        for (size_t i = 0; i < faces.size(); ++i)
        {
            const Face& face = m.Faces[faces[i]];
            tri.clear();
            m.TriangulateFace(faces[i], tri);
            double angleError = 0.0, weight = 0.0;
            bool flipped = false;
            for (size_t k = 0; k + 2 < tri.size(); k += 3)
            {
                glm::dvec3 p[3];
                glm::dvec2 q[3];
                for (int j = 0; j < 3; ++j)
                {
                    p[j] = glm::dvec3(m.Positions[face.Verts[tri[k + j]]]);
                    glm::vec2 uv = UVAt(face, tri[k + j]);
                    q[j] = glm::dvec2(uv.x, -uv.y);
                }
                double a3 = glm::length(glm::cross(p[1] - p[0], p[2] - p[0])) * 0.5, a2 = Area2(q[0], q[1], q[2]) * 0.5;
                f3[i] += a3;
                f2[i] += std::abs(a2);
                if (a2 < -1e-14) flipped = true;
                if (a3 < 1e-14) continue;
                for (int j = 0; j < 3; ++j)
                {
                    glm::dvec3 e1 = p[(j + 1) % 3] - p[j], e2 = p[(j + 2) % 3] - p[j];
                    glm::dvec2 g1 = q[(j + 1) % 3] - q[j], g2 = q[(j + 2) % 3] - q[j];
                    double l3 = glm::length(e1) * glm::length(e2), l2 = glm::length(g1) * glm::length(g2);
                    double ang3 = std::acos(std::clamp(glm::dot(e1, e2) / std::max(l3, 1e-30), -1.0, 1.0));
                    double ang2 = l2 > 1e-30 ? std::acos(std::clamp(glm::dot(g1, g2) / l2, -1.0, 1.0)) : 0.0;
                    angleError += std::abs(ang3 - ang2) * a3;
                }
                weight += a3;
            }
            total3 += f3[i];
            total2 += f2[i];
            if (flipped) out[i] = 1.0f;
            else if (mode == StretchMode::Angle && weight > 0.0) out[i] = (float)std::clamp(angleError / weight / (kPi * 0.5), 0.0, 1.0);
        }
        if (mode == StretchMode::Area && total3 > 0.0 && total2 > 0.0)
        {
            double density = total2 / total3;
            for (size_t i = 0; i < faces.size(); ++i)
            {
                if (out[i] >= 1.0f) continue;
                if (f3[i] < 1e-14) continue;
                double r = f2[i] / (f3[i] * density);
                out[i] = r <= 1e-12 ? 1.0f : (float)std::clamp(1.0 - std::min(r, 1.0 / r), 0.0, 1.0);
            }
        }
        return out;
    }

    // ---- Selection

    bool IsSelected(const EditableMesh& m, Corner c) { return m.Faces[c.Face].CornerFlags(c.Index) & UVSelected; }
    bool IsPinned(const EditableMesh& m, Corner c) { return m.Faces[c.Face].CornerFlags(c.Index) & UVPinned; }

    std::vector<Corner> SelectedCorners(const EditableMesh& m, const std::vector<uint32_t>& faces)
    {
        std::vector<Corner> out;
        for (uint32_t f : faces)
            for (uint32_t c = 0; c < m.Faces[f].Verts.size(); ++c)
                if (m.Faces[f].CornerFlags(c) & UVSelected) out.push_back({ f, c });
        return out;
    }

    void SelectAll(EditableMesh& m, const std::vector<uint32_t>& faces, bool select)
    {
        for (uint32_t f : faces)
            for (size_t c = 0; c < m.Faces[f].Verts.size(); ++c) m.Faces[f].SetCornerFlag(c, UVSelected, select);
    }

    void InvertSelection(EditableMesh& m, const std::vector<uint32_t>& faces)
    {
        for (uint32_t f : faces)
            for (size_t c = 0; c < m.Faces[f].Verts.size(); ++c)
                m.Faces[f].SetCornerFlag(c, UVSelected, !(m.Faces[f].CornerFlags(c) & UVSelected));
    }

    void SelectCorner(EditableMesh& m, const std::vector<uint32_t>& faces, Corner corner, bool select, Sticky sticky)
    {
        const Face& src = m.Faces[corner.Face];
        uint32_t v = src.Verts[corner.Index];
        glm::vec2 uv = UVAt(src, corner.Index);
        m.Faces[corner.Face].SetCornerFlag(corner.Index, UVSelected, select);
        if (sticky == Sticky::Disabled) return;
        for (uint32_t f : faces)
        {
            Face& face = m.Faces[f];
            for (size_t c = 0; c < face.Verts.size(); ++c)
                if (face.Verts[c] == v && (sticky == Sticky::Vertex || SameUV(UVAt(face, c), uv))) face.SetCornerFlag(c, UVSelected, select);
        }
    }

    void ExpandSticky(EditableMesh& m, const std::vector<uint32_t>& faces, Sticky sticky)
    {
        if (sticky == Sticky::Disabled) return;
        std::multimap<uint32_t, glm::vec2> chosen; // vertex -> selected UV there
        for (uint32_t f : faces)
            for (size_t c = 0; c < m.Faces[f].Verts.size(); ++c)
                if (m.Faces[f].CornerFlags(c) & UVSelected) chosen.insert({ m.Faces[f].Verts[c], UVAt(m.Faces[f], c) });
        for (uint32_t f : faces)
        {
            Face& face = m.Faces[f];
            for (size_t c = 0; c < face.Verts.size(); ++c)
            {
                if (face.CornerFlags(c) & UVSelected) continue;
                auto [lo, hi] = chosen.equal_range(face.Verts[c]);
                for (auto it = lo; it != hi; ++it)
                    if (sticky == Sticky::Vertex || SameUV(it->second, UVAt(face, c)))
                    {
                        face.SetCornerFlag(c, UVSelected, true);
                        break;
                    }
            }
        }
    }

    void SelectLinked(EditableMesh& m, const std::vector<uint32_t>& faces, const Corner* seed, bool select)
    {
        for (const std::vector<uint32_t>& island : Islands(m, faces))
        {
            bool hit = false;
            for (uint32_t f : island)
            {
                if (seed) hit |= seed->Face == f;
                else
                    for (size_t c = 0; c < m.Faces[f].Verts.size() && !hit; ++c) hit = m.Faces[f].CornerFlags(c) & UVSelected;
                if (hit) break;
            }
            if (hit) SelectAll(m, island, select);
        }
    }

    void SplitSelection(EditableMesh& m, const std::vector<uint32_t>& faces)
    {
        for (uint32_t f : faces)
        {
            Face& face = m.Faces[f];
            bool all = !face.Verts.empty();
            for (size_t c = 0; c < face.Verts.size(); ++c) all &= (face.CornerFlags(c) & UVSelected) != 0;
            if (!all)
                for (size_t c = 0; c < face.Verts.size(); ++c) face.SetCornerFlag(c, UVSelected, false);
        }
    }

    void SetPinned(EditableMesh& m, const std::vector<uint32_t>& faces, bool pinned)
    {
        for (const Corner& c : SelectedCorners(m, faces)) m.Faces[c.Face].SetCornerFlag(c.Index, UVPinned, pinned);
    }

    // ---- Changing the selected UVs

    bool SelectionCenter(const EditableMesh& m, const std::vector<uint32_t>& faces, glm::vec2& center)
    {
        glm::vec2 mn(1e30f), mx(-1e30f);
        bool any = false;
        for (const Corner& c : SelectedCorners(m, faces))
        {
            glm::vec2 uv = UVAt(m.Faces[c.Face], c.Index);
            mn = glm::min(mn, uv);
            mx = glm::max(mx, uv);
            any = true;
        }
        if (any) center = (mn + mx) * 0.5f;
        return any;
    }

    void TransformSelected(EditableMesh& m, const std::vector<uint32_t>& faces, const glm::mat3& transform)
    {
        for (const Corner& c : SelectedCorners(m, faces))
        {
            Face& face = m.Faces[c.Face];
            glm::vec3 p = transform * glm::vec3(UVAt(face, c.Index), 1.0f);
            SetUV(face, c.Index, glm::vec2(p));
        }
    }

    void MirrorSelected(EditableMesh& m, const std::vector<uint32_t>& faces, int axis)
    {
        glm::vec2 center;
        if (!SelectionCenter(m, faces, center)) return;
        for (const Corner& c : SelectedCorners(m, faces))
        {
            Face& face = m.Faces[c.Face];
            glm::vec2 uv = UVAt(face, c.Index);
            uv[axis & 1] = 2.0f * center[axis & 1] - uv[axis & 1];
            SetUV(face, c.Index, uv);
        }
    }

    void WeldSelected(EditableMesh& m, const std::vector<uint32_t>& faces)
    {
        std::vector<Corner> sel = SelectedCorners(m, faces);
        if (sel.empty()) return;
        glm::vec2 sum(0.0f);
        for (const Corner& c : sel) sum += UVAt(m.Faces[c.Face], c.Index);
        glm::vec2 center = sum / (float)sel.size();
        for (const Corner& c : sel) SetUV(m.Faces[c.Face], c.Index, center);
    }

    size_t MergeByDistance(EditableMesh& m, const std::vector<uint32_t>& faces, float distance)
    {
        std::vector<Corner> sel = SelectedCorners(m, faces);
        if (sel.empty()) return 0;
        std::vector<glm::vec2> uv(sel.size());
        for (size_t i = 0; i < sel.size(); ++i) uv[i] = UVAt(m.Faces[sel[i].Face], sel[i].Index);
        std::vector<size_t> order(sel.size());
        std::iota(order.begin(), order.end(), size_t(0));
        std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return uv[a].x < uv[b].x; });
        UnionFind uf(sel.size());
        for (size_t i = 0; i < order.size(); ++i)
            for (size_t j = i + 1; j < order.size() && uv[order[j]].x - uv[order[i]].x <= distance; ++j)
                if (glm::length(uv[order[j]] - uv[order[i]]) <= distance) uf.Join((uint32_t)order[i], (uint32_t)order[j]);
        // Points before and after (distinct UV positions).
        auto countPoints = [&](const std::vector<glm::vec2>& pts) {
            std::vector<glm::vec2> p = pts;
            std::sort(p.begin(), p.end(), [](glm::vec2 a, glm::vec2 b) { return a.x < b.x || (a.x == b.x && a.y < b.y); });
            return (size_t)(std::unique(p.begin(), p.end(), [](glm::vec2 a, glm::vec2 b) { return SameUV(a, b); }) - p.begin());
        };
        size_t before = countPoints(uv);
        std::map<uint32_t, std::pair<glm::vec2, int>> sums;
        for (size_t i = 0; i < sel.size(); ++i)
        {
            auto& s = sums[uf.Find((uint32_t)i)];
            s.first += uv[i];
            ++s.second;
        }
        for (size_t i = 0; i < sel.size(); ++i)
        {
            auto& s = sums[uf.Find((uint32_t)i)];
            uv[i] = s.first / (float)s.second;
            SetUV(m.Faces[sel[i].Face], sel[i].Index, uv[i]);
        }
        return before - countPoints(uv);
    }

    void AlignSelected(EditableMesh& m, const std::vector<uint32_t>& faces, int axis)
    {
        std::vector<Corner> sel = SelectedCorners(m, faces);
        if (sel.empty()) return;
        float sum = 0.0f;
        for (const Corner& c : sel) sum += UVAt(m.Faces[c.Face], c.Index)[axis & 1];
        float value = sum / (float)sel.size();
        for (const Corner& c : sel)
        {
            glm::vec2 uv = UVAt(m.Faces[c.Face], c.Index);
            uv[axis & 1] = value;
            SetUV(m.Faces[c.Face], c.Index, uv);
        }
    }
}
