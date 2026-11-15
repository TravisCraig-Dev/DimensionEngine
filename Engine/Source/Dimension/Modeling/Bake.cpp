#include "Dimension/Modeling/Bake.h"
#include <glm/gtc/constants.hpp>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <cmath>
#include <thread>
#include <unordered_map>

namespace Dimension::Modeling::Bake
{
    namespace
    {
        // A triangle of the surface rays look for: corner positions, shading normals and curvature.
        struct Tri
        {
            glm::vec3 P[3];
            glm::vec3 N[3];
            float K[3];
        };

        struct Hit
        {
            float T = 0.0f, U = 0.0f, V = 0.0f; // barycentrics: corner 1 U, corner 2 V
            uint32_t Tri = 0;
        };

        // A bounding volume hierarchy over triangles (median splits on the longest axis).
        class TriBVH
        {
        public:
            std::vector<Tri> Tris;

            void Build()
            {
                m_Nodes.clear();
                m_Order.resize(Tris.size());
                for (uint32_t i = 0; i < Tris.size(); ++i) m_Order[i] = i;
                m_Centers.resize(Tris.size());
                for (size_t i = 0; i < Tris.size(); ++i) m_Centers[i] = (Tris[i].P[0] + Tris[i].P[1] + Tris[i].P[2]) / 3.0f;
                if (Tris.empty()) return;
                m_Nodes.push_back({});
                BuildNode(0, 0, (uint32_t)Tris.size());
                // Leaves refer to triangles in m_Order: store them in that order.
                std::vector<Tri> ordered(Tris.size());
                for (size_t i = 0; i < Tris.size(); ++i) ordered[i] = Tris[m_Order[i]];
                Tris = std::move(ordered);
                // What the ray test needs, as plain floats: a corner and the two edges from it.
                m_Fast.resize(Tris.size() * 9);
                for (size_t i = 0; i < Tris.size(); ++i)
                {
                    const Tri& t = Tris[i];
                    float* f = &m_Fast[i * 9];
                    for (int k = 0; k < 3; ++k)
                    {
                        f[k] = t.P[0][k];
                        f[3 + k] = t.P[1][k] - t.P[0][k];
                        f[6 + k] = t.P[2][k] - t.P[0][k];
                    }
                }
            }

            // Nearest hit within maxT (`any`: the first found, for occlusion).
            // (Plain floats and pointers throughout: this runs millions of times per bake, and must not crawl in
            // debug builds either.)
            bool Raycast(const glm::vec3& o, const glm::vec3& d, float maxT, Hit& hit, bool any = false) const
            {
                if (m_Nodes.empty()) return false;
                const float ox = o.x, oy = o.y, oz = o.z, dx = d.x, dy = d.y, dz = d.z;
                const float ix = 1.0f / (dx > 1e-12f || dx < -1e-12f ? dx : 1e-12f), iy = 1.0f / (dy > 1e-12f || dy < -1e-12f ? dy : 1e-12f),
                            iz = 1.0f / (dz > 1e-12f || dz < -1e-12f ? dz : 1e-12f);
                const Node* nodes = m_Nodes.data();
                const float* fast = m_Fast.data();
                // The entry distance of a ray into a node's box (or a miss).
                auto enter = [&](const Node& n, float limit) {
                    float a0 = (n.Min.x - ox) * ix, a1 = (n.Max.x - ox) * ix;
                    float b0 = (n.Min.y - oy) * iy, b1 = (n.Max.y - oy) * iy;
                    float c0 = (n.Min.z - oz) * iz, c1 = (n.Max.z - oz) * iz;
                    float lo = a0 < a1 ? a0 : a1, hi = a0 < a1 ? a1 : a0;
                    float blo = b0 < b1 ? b0 : b1, bhi = b0 < b1 ? b1 : b0;
                    float clo = c0 < c1 ? c0 : c1, chi = c0 < c1 ? c1 : c0;
                    if (blo > lo) lo = blo;
                    if (clo > lo) lo = clo;
                    if (bhi < hi) hi = bhi;
                    if (chi < hi) hi = chi;
                    if (lo < 0.0f) lo = 0.0f;
                    if (limit < hi) hi = limit;
                    return lo <= hi ? lo : -1.0f;
                };
                uint32_t stack[64];
                int sp = 0;
                bool found = false;
                float best = maxT;
                if (enter(nodes[0], best) >= 0.0f) stack[sp++] = 0;
                while (sp > 0)
                {
                    const Node& n = nodes[stack[--sp]];
                    if (n.Count == 0)
                    {
                        // Both children that the ray enters, the nearer visited first (so far ones get cut short).
                        float tl = enter(nodes[n.Left], best), tr = enter(nodes[n.Left + 1], best);
                        if (sp + 2 > 64) continue;
                        if (tl >= 0.0f && tr >= 0.0f)
                        {
                            bool leftFirst = tl <= tr;
                            stack[sp++] = leftFirst ? n.Left + 1 : n.Left;
                            stack[sp++] = leftFirst ? n.Left : n.Left + 1;
                        }
                        else if (tl >= 0.0f) stack[sp++] = n.Left;
                        else if (tr >= 0.0f) stack[sp++] = n.Left + 1;
                        continue;
                    }
                    for (uint32_t i = n.Left; i < n.Left + n.Count; ++i)
                    {
                        const float* f = fast + (size_t)i * 9;
                        float e1x = f[3], e1y = f[4], e1z = f[5], e2x = f[6], e2y = f[7], e2z = f[8];
                        float px = dy * e2z - dz * e2y, py = dz * e2x - dx * e2z, pz = dx * e2y - dy * e2x;
                        float det = e1x * px + e1y * py + e1z * pz;
                        if (det > -1e-14f && det < 1e-14f) continue;
                        float invDet = 1.0f / det;
                        float sx = ox - f[0], sy = oy - f[1], sz = oz - f[2];
                        float u = (sx * px + sy * py + sz * pz) * invDet;
                        if (u < 0.0f || u > 1.0f) continue;
                        float qx = sy * e1z - sz * e1y, qy = sz * e1x - sx * e1z, qz = sx * e1y - sy * e1x;
                        float v = (dx * qx + dy * qy + dz * qz) * invDet;
                        if (v < 0.0f || u + v > 1.0f) continue;
                        float tt = (e2x * qx + e2y * qy + e2z * qz) * invDet;
                        if (tt <= 0.0f || tt >= best) continue;
                        best = tt;
                        hit = { tt, u, v, i };
                        found = true;
                        if (any) return true;
                    }
                }
                return found;
            }

        private:
            struct Node
            {
                glm::vec3 Min, Max;
                uint32_t Left = 0, Count = 0;
            };
            std::vector<Node> m_Nodes;
            std::vector<float> m_Fast; // per triangle: P0, P1 - P0, P2 - P0
            std::vector<uint32_t> m_Order;
            std::vector<glm::vec3> m_Centers;

            void BuildNode(uint32_t node, uint32_t first, uint32_t count)
            {
                glm::vec3 mn(1e30f), mx(-1e30f), cmn(1e30f), cmx(-1e30f);
                for (uint32_t i = first; i < first + count; ++i)
                {
                    const Tri& t = Tris[m_Order[i]];
                    for (const glm::vec3& p : t.P)
                    {
                        mn = glm::min(mn, p);
                        mx = glm::max(mx, p);
                    }
                    cmn = glm::min(cmn, m_Centers[m_Order[i]]);
                    cmx = glm::max(cmx, m_Centers[m_Order[i]]);
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
                std::nth_element(m_Order.begin() + first, m_Order.begin() + mid, m_Order.begin() + first + count,
                                 [&](uint32_t a, uint32_t b) { return m_Centers[a][axis] < m_Centers[b][axis]; });
                uint32_t left = (uint32_t)m_Nodes.size();
                m_Nodes.push_back({});
                m_Nodes.push_back({});
                m_Nodes[node].Left = left;
                m_Nodes[node].Count = 0;
                BuildNode(left, first, mid - first);
                BuildNode(left + 1, mid, first + count - mid);
            }
        };

        // Per vertex of a render mesh: curvature (convex > 0, concave < 0) from how normals spread along its edges,
        // vertices at the same place counted as one (so hard edges count too), scaled to -1..1 by the mesh's own range.
        std::vector<float> VertexCurvature(const MeshData& m)
        {
            size_t n = m.Vertices.size();
            std::unordered_map<uint64_t, uint32_t> weld;
            std::vector<uint32_t> id(n);
            std::vector<glm::vec3> pos;
            auto bits = [](float f) {
                uint32_t u;
                std::memcpy(&u, &f, 4);
                return (uint64_t)u;
            };
            for (size_t v = 0; v < n; ++v)
            {
                const glm::vec3& p = m.Vertices[v].Position;
                uint64_t key = (bits(p.x) * 73856093ull) ^ (bits(p.y) * 19349663ull) ^ (bits(p.z) * 83492791ull);
                // (a hash collision would only merge two far vertices' curvature: checked below)
                auto [it, added] = weld.try_emplace(key, (uint32_t)pos.size());
                if (!added && pos[it->second] != p)
                {
                    id[v] = (uint32_t)pos.size();
                    pos.push_back(p);
                    continue;
                }
                if (added) pos.push_back(p);
                id[v] = it->second;
            }
            std::vector<glm::vec3> normal(pos.size(), glm::vec3(0.0f));
            std::vector<std::pair<uint32_t, uint32_t>> edges;
            for (size_t i = 0; i + 2 < m.Indices.size(); i += 3)
            {
                uint32_t a = id[m.Indices[i]], b = id[m.Indices[i + 1]], c = id[m.Indices[i + 2]];
                glm::vec3 fn = glm::cross(pos[b] - pos[a], pos[c] - pos[a]);
                normal[a] += fn;
                normal[b] += fn;
                normal[c] += fn;
                edges.push_back({ std::min(a, b), std::max(a, b) });
                edges.push_back({ std::min(b, c), std::max(b, c) });
                edges.push_back({ std::min(c, a), std::max(c, a) });
            }
            for (auto& nn : normal)
                if (glm::length(nn) > 1e-20f) nn = glm::normalize(nn);
            std::sort(edges.begin(), edges.end());
            edges.erase(std::unique(edges.begin(), edges.end()), edges.end());
            std::vector<float> sum(pos.size(), 0.0f), count(pos.size(), 0.0f);
            double lengthSum = 0.0;
            for (auto [a, b] : edges)
            {
                glm::vec3 d = pos[b] - pos[a];
                float l2 = glm::dot(d, d);
                if (l2 < 1e-20f) continue;
                float k = glm::dot(normal[b] - normal[a], d) / l2;
                sum[a] += k;
                sum[b] += k;
                count[a] += 1.0f;
                count[b] += 1.0f;
                lengthSum += std::sqrt(l2);
            }
            float avgLen = edges.empty() ? 1.0f : (float)(lengthSum / (double)edges.size());
            std::vector<float> welded(pos.size(), 0.0f), magnitudes;
            for (size_t v = 0; v < pos.size(); ++v)
            {
                welded[v] = count[v] > 0.0f ? sum[v] / count[v] * avgLen : 0.0f;
                magnitudes.push_back(std::abs(welded[v]));
            }
            float scale = 1e-6f;
            if (!magnitudes.empty())
            {
                size_t at = std::min(magnitudes.size() - 1, (size_t)(magnitudes.size() * 0.95));
                std::nth_element(magnitudes.begin(), magnitudes.begin() + at, magnitudes.end());
                scale = std::max(magnitudes[at], 1e-6f);
            }
            std::vector<float> out(n);
            for (size_t v = 0; v < n; ++v) out[v] = std::clamp(welded[id[v]] / scale, -1.0f, 1.0f);
            return out;
        }

        TriBVH MakeBVH(const MeshData& m, const glm::mat4& transform)
        {
            TriBVH bvh;
            std::vector<float> curvature = VertexCurvature(m);
            glm::mat3 normalMatrix = glm::transpose(glm::inverse(glm::mat3(transform)));
            for (size_t i = 0; i + 2 < m.Indices.size(); i += 3)
            {
                Tri t;
                for (int j = 0; j < 3; ++j)
                {
                    const Vertex& v = m.Vertices[m.Indices[i + j]];
                    t.P[j] = glm::vec3(transform * glm::vec4(v.Position, 1.0f));
                    glm::vec3 n = normalMatrix * v.Normal;
                    t.N[j] = glm::length(n) > 1e-20f ? glm::normalize(n) : glm::vec3(0, 1, 0);
                    t.K[j] = curvature[m.Indices[i + j]];
                }
                bvh.Tris.push_back(t);
            }
            bvh.Build();
            return bvh;
        }

        // A covered texel: which low triangle and where in it.
        struct Texel
        {
            uint32_t Index; // y * width + x
            uint32_t Tri;   // first index of the triangle in low.Indices
            float B[3];
        };

        float RadicalInverse(uint32_t bits)
        {
            bits = (bits << 16u) | (bits >> 16u);
            bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
            bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
            bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
            bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
            return (float)bits * 2.3283064365386963e-10f;
        }

        uint32_t Hash(uint32_t x)
        {
            x ^= x >> 16;
            x *= 0x7FEB352Du;
            x ^= x >> 15;
            x *= 0x846CA68Bu;
            x ^= x >> 16;
            return x;
        }

        uint32_t Gray(float v)
        {
            uint32_t b = (uint32_t)(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
            return b | (b << 8) | (b << 16) | 0xFF000000u;
        }
    }

    void Dilate(Paint::Image& image, std::vector<uint8_t> covered, int iterations)
    {
        int w = (int)image.Width, h = (int)image.Height;
        for (int it = 0; it < iterations; ++it)
        {
            std::vector<uint8_t> next = covered;
            std::vector<uint32_t> pixels = image.Pixels;
            bool any = false;
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                    size_t i = (size_t)y * w + x;
                    if (covered[i]) continue;
                    glm::vec4 sum(0.0f);
                    float n = 0.0f;
                    for (int dy = -1; dy <= 1; ++dy)
                        for (int dx = -1; dx <= 1; ++dx)
                        {
                            int sx = x + dx, sy = y + dy;
                            if (sx < 0 || sy < 0 || sx >= w || sy >= h) continue;
                            size_t j = (size_t)sy * w + sx;
                            if (!covered[j]) continue;
                            sum += UnpackRGBA8(image.Pixels[j]);
                            n += 1.0f;
                        }
                    if (n == 0.0f) continue;
                    pixels[i] = PackRGBA8(sum / n);
                    next[i] = 1;
                    any = true;
                }
            image.Pixels = std::move(pixels);
            covered = std::move(next);
            if (!any) break;
        }
    }

    Result BakeMaps(const MeshData& low, const MeshData* high, const glm::mat4& highToLow, const Settings& s)
    {
        Result result;
        uint32_t W = std::clamp(s.Width, 1u, 8192u), H = std::clamp(s.Height, 1u, 8192u);
        bool normal = s.Normal && high != nullptr;
        if (normal) result.Normal = Paint::Image::Solid(W, H, PackRGBA8(glm::vec4(0.5f, 0.5f, 1.0f, 1.0f)));
        if (s.AmbientOcclusion) result.AmbientOcclusion = Paint::Image::Solid(W, H, 0xFFFFFFFFu);
        if (s.Curvature) result.Curvature = Paint::Image::Solid(W, H, Gray(0.5f));
        if (!normal && !s.AmbientOcclusion && !s.Curvature) return result;

        TriBVH source = high ? MakeBVH(*high, highToLow) : MakeBVH(low, glm::mat4(1.0f));
        std::vector<float> lowCurvature = high ? std::vector<float>() : VertexCurvature(low);
        glm::vec3 mn(1e30f), mx(-1e30f);
        for (const Vertex& v : low.Vertices)
        {
            mn = glm::min(mn, v.Position);
            mx = glm::max(mx, v.Position);
        }
        float eps = std::max(glm::length(mx - mn), 1e-3f) * 1e-4f;

        // The low mesh's UV layout: which triangle covers each texel (texel centers inside it).
        std::vector<int64_t> owner((size_t)W * H, -1);
        std::vector<Texel> texels;
        for (size_t i = 0; i + 2 < low.Indices.size(); i += 3)
        {
            glm::vec2 t[3];
            for (int j = 0; j < 3; ++j)
            {
                glm::vec2 uv = low.Vertices[low.Indices[i + j]].UV;
                t[j] = glm::vec2(uv.x * W, uv.y * H);
            }
            float area = (t[1].x - t[0].x) * (t[2].y - t[0].y) - (t[1].y - t[0].y) * (t[2].x - t[0].x);
            if (std::abs(area) < 1e-10f) continue;
            int x0 = std::max(0, (int)std::floor(std::min({ t[0].x, t[1].x, t[2].x }))), x1 = std::min((int)W - 1, (int)std::ceil(std::max({ t[0].x, t[1].x, t[2].x })));
            int y0 = std::max(0, (int)std::floor(std::min({ t[0].y, t[1].y, t[2].y }))), y1 = std::min((int)H - 1, (int)std::ceil(std::max({ t[0].y, t[1].y, t[2].y })));
            for (int y = y0; y <= y1; ++y)
                for (int x = x0; x <= x1; ++x)
                {
                    glm::vec2 p((float)x + 0.5f, (float)y + 0.5f);
                    float b[3];
                    bool inside = true;
                    for (int j = 0; j < 3; ++j)
                    {
                        const glm::vec2 &e0 = t[(j + 1) % 3], &e1 = t[(j + 2) % 3];
                        b[j] = ((e1.x - e0.x) * (p.y - e0.y) - (e1.y - e0.y) * (p.x - e0.x)) / area;
                        if (b[j] < -1e-6f) inside = false;
                    }
                    if (!inside) continue;
                    size_t index = (size_t)y * W + x;
                    if (owner[index] >= 0) texels[(size_t)owner[index]] = { (uint32_t)index, (uint32_t)i, { b[0], b[1], b[2] } };
                    else
                    {
                        owner[index] = (int64_t)texels.size();
                        texels.push_back({ (uint32_t)index, (uint32_t)i, { b[0], b[1], b[2] } });
                    }
                }
        }
        result.Texels = texels.size();

        std::atomic<size_t> next{ 0 }, missed{ 0 };
        int samples = std::max(1, s.AOSamples);
        auto work = [&]() {
            Hit hit;
            for (;;)
            {
                size_t start = next.fetch_add(256);
                if (start >= texels.size()) break;
                size_t end = std::min(texels.size(), start + 256);
                for (size_t k = start; k < end; ++k)
                {
                    const Texel& tx = texels[k];
                    const Vertex* v[3] = { &low.Vertices[low.Indices[tx.Tri]], &low.Vertices[low.Indices[tx.Tri + 1]], &low.Vertices[low.Indices[tx.Tri + 2]] };
                    glm::vec3 p = v[0]->Position * tx.B[0] + v[1]->Position * tx.B[1] + v[2]->Position * tx.B[2];
                    glm::vec3 n = v[0]->Normal * tx.B[0] + v[1]->Normal * tx.B[1] + v[2]->Normal * tx.B[2];
                    n = glm::length(n) > 1e-20f ? glm::normalize(n) : glm::vec3(0, 1, 0);
                    glm::vec4 tan = v[0]->Tangent * tx.B[0] + v[1]->Tangent * tx.B[1] + v[2]->Tangent * tx.B[2];
                    // The surface sampled: the detailed one found along the cage ray, else this one.
                    glm::vec3 sp = p, sn = n;
                    float curvature = 0.0f;
                    if (high)
                    {
                        glm::vec3 from = p + n * s.CageExtrusion;
                        if (source.Raycast(from, -n, s.CageExtrusion * 2.0f, hit))
                        {
                            const Tri& t = source.Tris[hit.Tri];
                            float w0 = 1.0f - hit.U - hit.V;
                            sp = from - n * hit.T;
                            sn = t.N[0] * w0 + t.N[1] * hit.U + t.N[2] * hit.V;
                            sn = glm::length(sn) > 1e-20f ? glm::normalize(sn) : n;
                            curvature = t.K[0] * w0 + t.K[1] * hit.U + t.K[2] * hit.V;
                        }
                        else
                            missed.fetch_add(1, std::memory_order_relaxed);
                    }
                    else
                        curvature = lowCurvature[low.Indices[tx.Tri]] * tx.B[0] + lowCurvature[low.Indices[tx.Tri + 1]] * tx.B[1] +
                                    lowCurvature[low.Indices[tx.Tri + 2]] * tx.B[2];
                    if (normal)
                    {
                        // Tangent space as the shader builds it: T against the normal, B = cross(N, T) * w (up the image).
                        glm::vec3 T = glm::vec3(tan) - n * glm::dot(n, glm::vec3(tan));
                        glm::vec3 encoded(0.0f, 0.0f, 1.0f);
                        if (glm::length(T) > 1e-12f && std::abs(tan.w) > 1e-6f)
                        {
                            T = glm::normalize(T);
                            glm::vec3 B = glm::cross(n, T) * (tan.w < 0.0f ? -1.0f : 1.0f);
                            encoded = glm::vec3(glm::dot(sn, T), glm::dot(sn, B), glm::dot(sn, n));
                        }
                        result.Normal.Pixels[tx.Index] = PackRGBA8(glm::vec4(encoded * 0.5f + 0.5f, 1.0f));
                    }
                    if (s.AmbientOcclusion)
                    {
                        glm::vec3 up = std::abs(sn.z) < 0.9f ? glm::vec3(0, 0, 1) : glm::vec3(1, 0, 0);
                        glm::vec3 tx1 = glm::normalize(glm::cross(up, sn)), ty1 = glm::cross(sn, tx1);
                        float rotation = (float)(Hash(tx.Index) & 0xFFFF) / 65536.0f;
                        int occluded = 0;
                        glm::vec3 origin = sp + sn * eps * 10.0f;
                        for (int i = 0; i < samples; ++i)
                        {
                            // Cosine-weighted directions (Hammersley points, turned per texel).
                            float u1 = ((float)i + 0.5f) / (float)samples, u2 = std::fmod(RadicalInverse((uint32_t)i) + rotation, 1.0f);
                            float r = std::sqrt(u1), phi = glm::two_pi<float>() * u2;
                            glm::vec3 d = tx1 * (r * std::cos(phi)) + ty1 * (r * std::sin(phi)) + sn * std::sqrt(std::max(0.0f, 1.0f - u1));
                            if (source.Raycast(origin, d, s.AODistance, hit, true)) ++occluded;
                        }
                        result.AmbientOcclusion.Pixels[tx.Index] = Gray(1.0f - (float)occluded / (float)samples);
                    }
                    if (s.Curvature) result.Curvature.Pixels[tx.Index] = Gray(0.5f + 0.5f * curvature);
                }
            }
        };
        int threads = s.Threads > 0 ? s.Threads : (int)std::max(1u, std::thread::hardware_concurrency());
        std::vector<std::thread> pool;
        for (int i = 1; i < threads; ++i) pool.emplace_back(work);
        work();
        for (auto& t : pool) t.join();
        result.Missed = missed.load();

        std::vector<uint8_t> covered((size_t)W * H, 0);
        for (const Texel& t : texels) covered[t.Index] = 1;
        if (normal) Dilate(result.Normal, covered, s.Margin);
        if (s.AmbientOcclusion) Dilate(result.AmbientOcclusion, covered, s.Margin);
        if (s.Curvature) Dilate(result.Curvature, covered, s.Margin);
        return result;
    }
}
