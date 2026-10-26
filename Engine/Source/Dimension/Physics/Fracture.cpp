#include "Dimension/Physics/Fracture.h"
#include <algorithm>
#include <cmath>
#include <numeric>

namespace Dimension::Fracture
{
    namespace
    {
        struct Tri
        {
            Vertex V[3];
            int Sub; // source sub-mesh index, or the cap sub-mesh
        };

        uint32_t Hash(uint32_t x)
        {
            x ^= x >> 16;
            x *= 0x7feb352dU;
            x ^= x >> 15;
            x *= 0x846ca68bU;
            x ^= x >> 16;
            return x;
        }
        float Random01(uint32_t& state)
        {
            state = Hash(state + 0x9e3779b9U);
            return (state >> 8) * (1.0f / 16777216.0f);
        }

        Vertex Lerp(const Vertex& a, const Vertex& b, float t)
        {
            Vertex v;
            v.Position = glm::mix(a.Position, b.Position, t);
            v.Normal = glm::mix(a.Normal, b.Normal, t);
            float len = glm::length(v.Normal);
            v.Normal = len > 1e-8f ? v.Normal / len : a.Normal;
            v.UV = glm::mix(a.UV, b.UV, t);
            v.Tangent = glm::mix(a.Tangent, b.Tangent, t);
            v.Tangent.w = a.Tangent.w;
            return v;
        }

        // Keeps the part of the triangles where dot(n, p) <= d, closing the cut with cap triangles (facing +n, so
        // outward) filled from each loop of cut edges.
        void Clip(std::vector<Tri>& tris, const glm::vec3& n, float d, int capSub, float tolerance)
        {
            std::vector<Tri> out;
            out.reserve(tris.size());
            std::vector<std::pair<glm::vec3, glm::vec3>> segments;
            for (const Tri& t : tris)
            {
                float dist[3];
                int inside = 0;
                for (int k = 0; k < 3; ++k)
                {
                    dist[k] = glm::dot(n, t.V[k].Position) - d;
                    inside += dist[k] <= 0.0f;
                }
                if (inside == 3)
                {
                    out.push_back(t);
                    continue;
                }
                if (inside == 0) continue;
                // Sutherland-Hodgman against one plane: the inside polygon (3 or 4 corners), then a fan.
                Vertex poly[4];
                int count = 0;
                glm::vec3 cut[2];
                int cuts = 0;
                for (int k = 0; k < 3; ++k)
                {
                    const Vertex &a = t.V[k], &b = t.V[(k + 1) % 3];
                    bool ina = dist[k] <= 0.0f, inb = dist[(k + 1) % 3] <= 0.0f;
                    if (ina) poly[count++] = a;
                    if (ina != inb)
                    {
                        float s = dist[k] / (dist[k] - dist[(k + 1) % 3]);
                        Vertex v = Lerp(a, b, s);
                        poly[count++] = v;
                        if (cuts < 2) cut[cuts++] = v.Position;
                    }
                }
                for (int k = 1; k + 1 < count; ++k) out.push_back({ { poly[0], poly[k], poly[k + 1] }, t.Sub });
                if (cuts == 2 && glm::distance(cut[0], cut[1]) > tolerance) segments.push_back({ cut[0], cut[1] });
            }
            tris = std::move(out);

            // Chain the cut edges into loops (endpoints match within the tolerance) and fan each from its center.
            glm::vec3 axisU = glm::normalize(glm::cross(n, std::abs(n.y) < 0.9f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0)));
            glm::vec3 axisV = glm::cross(n, axisU);
            std::vector<bool> used(segments.size(), false);
            for (size_t first = 0; first < segments.size(); ++first)
            {
                if (used[first]) continue;
                used[first] = true;
                std::vector<glm::vec3> loop = { segments[first].first, segments[first].second };
                for (bool grew = true; grew;)
                {
                    grew = false;
                    for (size_t s = 0; s < segments.size(); ++s)
                    {
                        if (used[s]) continue;
                        const glm::vec3& end = loop.back();
                        if (glm::distance(segments[s].first, end) <= tolerance) loop.push_back(segments[s].second);
                        else if (glm::distance(segments[s].second, end) <= tolerance) loop.push_back(segments[s].first);
                        else continue;
                        used[s] = true;
                        grew = true;
                    }
                }
                if (glm::distance(loop.front(), loop.back()) <= tolerance) loop.pop_back();
                if (loop.size() < 3) continue;
                glm::vec3 center = std::accumulate(loop.begin(), loop.end(), glm::vec3(0.0f)) / (float)loop.size();
                auto capVertex = [&](const glm::vec3& p) {
                    Vertex v;
                    v.Position = p;
                    v.Normal = n;
                    v.UV = { glm::dot(p, axisU), glm::dot(p, axisV) }; // one repeat per unit
                    v.Tangent = glm::vec4(axisU, 1.0f);
                    return v;
                };
                for (size_t k = 0; k < loop.size(); ++k)
                {
                    glm::vec3 a = loop[k], b = loop[(k + 1) % loop.size()];
                    if (glm::dot(glm::cross(a - center, b - center), n) < 0.0f) std::swap(a, b);
                    tris.push_back({ { capVertex(center), capVertex(a), capVertex(b) }, capSub });
                }
            }
        }

        // Volume and its centroid (signed tetrahedra from the origin).
        float VolumeOf(const std::vector<Tri>& tris, glm::vec3& centroid)
        {
            float volume = 0.0f;
            glm::vec3 weighted(0.0f), mean(0.0f);
            for (const Tri& t : tris)
            {
                const glm::vec3 &a = t.V[0].Position, &b = t.V[1].Position, &c = t.V[2].Position;
                float v = glm::dot(a, glm::cross(b, c)) / 6.0f;
                volume += v;
                weighted += v * (a + b + c) * 0.25f;
                mean += a + b + c;
            }
            centroid = std::abs(volume) > 1e-12f ? weighted / volume : mean / std::max(1.0f, tris.size() * 3.0f);
            return volume;
        }
    }

    float Volume(const MeshData& mesh)
    {
        float volume = 0.0f;
        for (size_t i = 0; i + 2 < mesh.Indices.size(); i += 3)
        {
            const glm::vec3 &a = mesh.Vertices[mesh.Indices[i]].Position, &b = mesh.Vertices[mesh.Indices[i + 1]].Position,
                            &c = mesh.Vertices[mesh.Indices[i + 2]].Position;
            volume += glm::dot(a, glm::cross(b, c)) / 6.0f;
        }
        return volume;
    }

    std::vector<Piece> Shatter(const MeshData& mesh, int count, uint32_t seed, int32_t capSlot, const Focus* focus)
    {
        std::vector<Piece> pieces;
        if (mesh.Vertices.empty() || mesh.Indices.size() < 3) return pieces;
        // The source as a triangle soup, each triangle knowing its sub-mesh.
        std::vector<Tri> source;
        std::vector<SubMesh> subs = mesh.SubMeshes;
        if (subs.empty()) subs.push_back({ 0, (uint32_t)mesh.Indices.size(), -1 });
        int capSub = (int)subs.size();
        for (int s = 0; s < capSub; ++s)
            for (uint32_t i = subs[s].IndexOffset; i + 2 < subs[s].IndexOffset + subs[s].IndexCount && i + 2 < mesh.Indices.size(); i += 3)
                source.push_back({ { mesh.Vertices[mesh.Indices[i]], mesh.Vertices[mesh.Indices[i + 1]], mesh.Vertices[mesh.Indices[i + 2]] }, s });
        glm::vec3 lo(1e30f), hi(-1e30f);
        for (const Vertex& v : mesh.Vertices)
        {
            lo = glm::min(lo, v.Position);
            hi = glm::max(hi, v.Position);
        }
        float size = std::max(glm::length(hi - lo), 1e-4f);
        float tolerance = size * 1e-5f;

        // Seeds inside the mesh: between its centroid and random points on its surface (picked by area).
        glm::vec3 centroid;
        VolumeOf(source, centroid);
        std::vector<float> area(source.size());
        float total = 0.0f;
        for (size_t i = 0; i < source.size(); ++i)
        {
            total += 0.5f * glm::length(glm::cross(source[i].V[1].Position - source[i].V[0].Position, source[i].V[2].Position - source[i].V[0].Position));
            area[i] = total;
        }
        count = std::clamp(count, 1, 256);
        uint32_t state = Hash(seed * 2654435761u + 17u);
        std::vector<glm::vec3> seeds;
        // Focused seeds: around the point, pulled a little toward the centroid so a point on the surface still seeds
        // cells inside.
        int focused = focus ? (int)std::lround(std::clamp(focus->Amount, 0.0f, 1.0f) * count) : 0;
        for (int i = 0; i < focused; ++i)
        {
            glm::vec3 dir;
            do dir = glm::vec3(Random01(state), Random01(state), Random01(state)) * 2.0f - 1.0f;
            while (glm::dot(dir, dir) > 1.0f);
            glm::vec3 p = focus->Point + dir * (std::max(focus->Radius, 0.01f) * size * 0.5f);
            seeds.push_back(glm::mix(p, centroid, 0.2f));
        }
        for (int i = focused; i < count; ++i)
        {
            float pick = Random01(state) * total;
            size_t t = std::min((size_t)(std::lower_bound(area.begin(), area.end(), pick) - area.begin()), source.size() - 1);
            float u = Random01(state), v = Random01(state);
            if (u + v > 1.0f) { u = 1.0f - u; v = 1.0f - v; }
            const Tri& tri = source[t];
            glm::vec3 onSurface = tri.V[0].Position + (tri.V[1].Position - tri.V[0].Position) * u + (tri.V[2].Position - tri.V[0].Position) * v;
            seeds.push_back(glm::mix(centroid, onSurface, 0.15f + 0.75f * Random01(state)));
        }

        for (size_t i = 0; i < seeds.size(); ++i)
        {
            std::vector<Tri> cell = source;
            // Nearest neighbours first: their planes cut the most away early.
            std::vector<size_t> order(seeds.size());
            std::iota(order.begin(), order.end(), size_t(0));
            std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return glm::distance(seeds[a], seeds[i]) < glm::distance(seeds[b], seeds[i]); });
            for (size_t j : order)
            {
                if (j == i || cell.empty()) continue;
                glm::vec3 delta = seeds[j] - seeds[i];
                float len = glm::length(delta);
                if (len < tolerance) continue; // the same seed twice: the first one keeps the cell
                glm::vec3 n = delta / len;
                float d = glm::dot(n, (seeds[i] + seeds[j]) * 0.5f);
                bool outside = false;
                for (const Tri& t : cell)
                    for (const Vertex& v : t.V) outside |= glm::dot(n, v.Position) > d;
                if (outside) Clip(cell, n, d, capSub, tolerance);
            }
            if (cell.empty()) continue;
            Piece piece;
            piece.Volume = VolumeOf(cell, piece.Center);
            if (piece.Volume <= size * size * size * 1e-7f) continue; // slivers
            // One sub-mesh per source sub-mesh with triangles here, then the caps.
            MeshData& out = piece.Mesh;
            out.Name = mesh.Name + " Piece";
            for (int s = 0; s <= capSub; ++s)
            {
                // Source parts keep their slot (or their index, as the renderer reads a missing slot).
                SubMesh sub{ (uint32_t)out.Indices.size(), 0, s < capSub ? (subs[s].MaterialSlot >= 0 ? subs[s].MaterialSlot : s) : capSlot };
                for (const Tri& t : cell)
                {
                    if (t.Sub != s) continue;
                    for (const Vertex& v : t.V)
                    {
                        out.Indices.push_back((uint32_t)out.Vertices.size());
                        out.Vertices.push_back(v);
                        out.Vertices.back().Position -= piece.Center;
                    }
                }
                sub.IndexCount = (uint32_t)out.Indices.size() - sub.IndexOffset;
                if (sub.IndexCount > 0) out.SubMeshes.push_back(sub);
            }
            pieces.push_back(std::move(piece));
        }
        return pieces;
    }
}
