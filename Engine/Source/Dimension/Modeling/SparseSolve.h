#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

// Sparse symmetric linear systems (unwrapping, automatic weights): conjugate gradients, and a direct envelope Cholesky
// factorization for solving the same matrix many times.
namespace Dimension::Modeling::Sparse
{
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

    // A sparse symmetric positive definite system factored once (Cholesky, L L^T) and then solved directly as often as
    // needed: the unknowns reordered by reverse Cuthill-McKee so each row's nonzeros lie close to the diagonal, and
    // only that envelope stored. Much faster than iterating on large, badly conditioned charts.
    class EnvelopeCholesky
    {
    public:
        // False when the matrix isn't positive definite or the envelope would be too big (use an iterative solve).
        bool Factor(const SymmetricSystem& A, size_t maxEntries = 60000000)
        {
            size_t n = A.Rows.size();
            m_N = n;
            if (n == 0) return false;
            // Reverse Cuthill-McKee: breadth first from a far-out vertex, neighbours by rising degree, reversed.
            std::vector<uint32_t> order;
            order.reserve(n);
            std::vector<uint8_t> seen(n, 0);
            std::vector<uint32_t> level(n, 0);
            auto degree = [&](uint32_t v) { return A.Rows[v].size(); };
            auto bfs = [&](uint32_t start, std::vector<uint32_t>& out, std::vector<uint8_t>& mark) {
                size_t begin = out.size();
                out.push_back(start);
                mark[start] = 1;
                level[start] = 0;
                std::vector<uint32_t> next;
                for (size_t q = begin; q < out.size(); ++q)
                {
                    uint32_t v = out[q];
                    next.clear();
                    for (auto& [c, w] : A.Rows[v])
                        if (c != v && !mark[c])
                        {
                            mark[c] = 1;
                            level[c] = level[v] + 1;
                            next.push_back(c);
                        }
                    std::sort(next.begin(), next.end(), [&](uint32_t a, uint32_t b) { return degree(a) < degree(b); });
                    out.insert(out.end(), next.begin(), next.end());
                }
            };
            for (uint32_t s = 0; s < n; ++s)
            {
                if (seen[s]) continue;
                // A pseudo-peripheral start: the last-reached vertex of a breadth-first pass, twice.
                uint32_t start = s;
                for (int pass = 0; pass < 2; ++pass)
                {
                    std::vector<uint32_t> tmp;
                    std::vector<uint8_t> mark = seen;
                    bfs(start, tmp, mark);
                    uint32_t far = tmp.back();
                    for (uint32_t v : tmp)
                        if (level[v] == level[far] && degree(v) < degree(far)) far = v;
                    start = far;
                }
                bfs(start, order, seen);
            }
            std::reverse(order.begin(), order.end());
            m_Perm = order;
            std::vector<uint32_t> inv(n);
            for (size_t i = 0; i < n; ++i) inv[m_Perm[i]] = (uint32_t)i;
            // The envelope: each row from its first nonzero to the diagonal.
            m_First.assign(n, 0);
            m_Start.assign(n + 1, 0);
            for (size_t i = 0; i < n; ++i)
            {
                uint32_t first = (uint32_t)i;
                for (auto& [c, w] : A.Rows[m_Perm[i]]) first = std::min(first, inv[c]);
                m_First[i] = first;
                m_Start[i + 1] = m_Start[i] + (i - first + 1);
                if (m_Start[i + 1] > maxEntries) return false;
            }
            m_L.assign(m_Start[n], 0.0);
            double* L = m_L.data();
            for (size_t i = 0; i < n; ++i)
                for (auto& [c, w] : A.Rows[m_Perm[i]])
                    if (uint32_t j = inv[c]; j <= i) L[m_Start[i] + (j - m_First[i])] += w;
            double largest = 0.0;
            for (size_t i = 0; i < n; ++i) largest = std::max(largest, std::abs(L[m_Start[i + 1] - 1]));
            for (size_t i = 0; i < n; ++i)
            {
                size_t fi = m_First[i];
                double* Li = L + m_Start[i] - fi; // Li[k]: row i, column k
                for (size_t j = fi; j < i; ++j)
                {
                    size_t fj = m_First[j];
                    const double* Lj = L + m_Start[j] - fj;
                    double sum = Li[j];
                    for (size_t k = std::max(fi, fj); k < j; ++k) sum -= Li[k] * Lj[k];
                    Li[j] = sum / Lj[j];
                }
                double d = Li[i];
                for (size_t k = fi; k < i; ++k) d -= Li[k] * Li[k];
                if (!(d > largest * 1e-14)) return false;
                Li[i] = std::sqrt(d);
            }
            return true;
        }

        void Solve(const std::vector<double>& b, std::vector<double>& x) const
        {
            size_t n = m_N;
            std::vector<double> y(n);
            const double* L = m_L.data();
            for (size_t i = 0; i < n; ++i)
            {
                size_t fi = m_First[i];
                const double* Li = L + m_Start[i] - fi;
                double sum = b[m_Perm[i]];
                for (size_t k = fi; k < i; ++k) sum -= Li[k] * y[k];
                y[i] = sum / Li[i];
            }
            for (size_t i = n; i-- > 0;)
            {
                size_t fi = m_First[i];
                const double* Li = L + m_Start[i] - fi;
                y[i] /= Li[i];
                double yi = y[i];
                for (size_t k = fi; k < i; ++k) y[k] -= Li[k] * yi;
            }
            x.resize(n);
            for (size_t i = 0; i < n; ++i) x[m_Perm[i]] = y[i];
        }

    private:
        size_t m_N = 0;
        std::vector<uint32_t> m_Perm, m_First;
        std::vector<size_t> m_Start;
        std::vector<double> m_L;
    };
}
