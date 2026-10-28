#include "Dimension/Physics/Water.h"
#include "Dimension/Scene/Scene.h"
#include "Dimension/Physics/Physics3D.h"
#include <glm/gtc/constants.hpp>
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <mutex>
#include <tuple>

namespace Dimension::Water
{
    namespace
    {
        constexpr float kGravity = 9.81f;
        constexpr int kWaves = 3;

        struct Wave
        {
            glm::vec2 Direction;
            float K, Omega, Amplitude, Q;
        };

        // The main wave and two shorter, lower ones turned to either side, so the pattern does not repeat at once.
        void MakeWaves(const WaterVolumeComponent& water, Wave out[kWaves])
        {
            const float turn[kWaves] = { 0.0f, 25.0f, -35.0f }, length[kWaves] = { 1.0f, 0.61f, 0.37f }, height[kWaves] = { 1.0f, 0.45f, 0.25f };
            float base = std::max(water.WaveLength, 0.1f), steep = std::clamp(water.Steepness, 0.0f, 1.0f);
            for (int i = 0; i < kWaves; ++i)
            {
                float a = glm::radians(water.WaveDirection + turn[i]);
                Wave& w = out[i];
                w.Direction = { std::cos(a), std::sin(a) };
                w.K = glm::two_pi<float>() / (base * length[i]);
                w.Omega = std::sqrt(kGravity * w.K); // deep water
                w.Amplitude = std::max(water.WaveHeight, 0.0f) * 0.5f * height[i];
                // Q * k * a summed over the waves stays below 1, so crests sharpen without looping over.
                w.Q = w.K * w.Amplitude > 1e-6f ? steep / (w.K * w.Amplitude * kWaves) : 0.0f;
            }
        }

        // How much the waves bunch the surface together at a rest point (the Jacobian of the horizontal motion: 1 =
        // undisturbed, smaller where crests squeeze it), as foam 0-1.
        float CrestFoamWith(const Wave waves[kWaves], glm::vec2 rest, float time)
        {
            float jxx = 1.0f, jzz = 1.0f, jxz = 0.0f;
            for (int i = 0; i < kWaves; ++i)
            {
                const Wave& w = waves[i];
                if (w.Amplitude <= 0.0f) continue;
                float s = std::sin(w.K * glm::dot(w.Direction, rest) - w.Omega * time), qak = w.Q * w.Amplitude * w.K;
                jxx -= qak * w.Direction.x * w.Direction.x * s;
                jzz -= qak * w.Direction.y * w.Direction.y * s;
                jxz -= qak * w.Direction.x * w.Direction.y * s;
            }
            float det = jxx * jzz - jxz * jxz;
            return std::clamp((0.88f - det) / 0.35f, 0.0f, 1.0f);
        }

        float ShoreFoamFromDepth(const WaterVolumeComponent& water, float depth)
        {
            return water.ShoreFoam > 0.0f ? std::clamp(1.0f - depth / water.ShoreFoam, 0.0f, 1.0f) : 0.0f;
        }

        // The ground's depth under a world point on the surface: a ray down to the first thing not floating.
        float DepthBelow(Scene& scene, const glm::vec3& p, float reach)
        {
            auto& reg = scene.GetRegistry();
            for (const RaycastHit& h : Physics3DWorld::RaycastAll(scene, p + glm::vec3(0.0f, 0.05f, 0.0f), { 0.0f, -1.0f, 0.0f }, reach + 0.05f))
            {
                const auto* rb = reg.TryGet<RigidbodyComponent>(h.Hit);
                if (rb && rb->Type == BodyType::Dynamic) continue;
                return std::max(h.Distance - 0.05f, 0.0f);
            }
            return reach;
        }

        glm::vec3 DisplaceWith(const Wave waves[kWaves], glm::vec2 rest, float time, glm::vec3* normal, glm::vec3* velocity)
        {
            glm::vec3 p(rest.x, 0.0f, rest.y), n(0.0f, 1.0f, 0.0f), v(0.0f);
            for (int i = 0; i < kWaves; ++i)
            {
                const Wave& w = waves[i];
                if (w.Amplitude <= 0.0f) continue;
                float theta = w.K * glm::dot(w.Direction, rest) - w.Omega * time;
                float c = std::cos(theta), s = std::sin(theta), qa = w.Q * w.Amplitude;
                p.x += qa * w.Direction.x * c;
                p.z += qa * w.Direction.y * c;
                p.y += w.Amplitude * s;
                float ka = w.K * w.Amplitude;
                n.x -= w.Direction.x * ka * c;
                n.z -= w.Direction.y * ka * c;
                n.y -= w.Q * ka * s;
                v.x += qa * w.Direction.x * w.Omega * s;
                v.z += qa * w.Direction.y * w.Omega * s;
                v.y -= w.Amplitude * w.Omega * c;
            }
            if (normal) *normal = glm::normalize(n);
            if (velocity) *velocity = v;
            return p;
        }

        float TopOf(const WaterVolumeComponent& water) { return water.Size.y * 0.5f; }
        int ResolutionOf(const WaterVolumeComponent& water) { return std::clamp(water.SurfaceResolution, 2, 256); }
    }

    bool HasWaves(const WaterVolumeComponent& water) { return water.WaveHeight > 0.0f; }

    glm::vec3 Displace(const WaterVolumeComponent& water, glm::vec2 rest, float time, glm::vec3* normal)
    {
        Wave waves[kWaves];
        MakeWaves(water, waves);
        glm::vec3 p = DisplaceWith(waves, rest, time, normal, nullptr);
        p.y += TopOf(water);
        return p;
    }

    SurfacePoint Sample(const WaterVolumeComponent& water, glm::vec2 xz, float time)
    {
        SurfacePoint out;
        out.Height = TopOf(water);
        if (!HasWaves(water)) return out;
        Wave waves[kWaves];
        MakeWaves(water, waves);
        // Find the resting point whose circle passes over xz (a few fixed-point steps converge for Q k a < 1).
        glm::vec2 rest = xz;
        for (int i = 0; i < 4; ++i)
        {
            glm::vec3 p = DisplaceWith(waves, rest, time, nullptr, nullptr);
            rest += xz - glm::vec2(p.x, p.z);
        }
        glm::vec3 p = DisplaceWith(waves, rest, time, &out.Normal, &out.Velocity);
        out.Height += p.y;
        return out;
    }

    namespace
    {
        struct Grid
        {
            std::vector<Vertex> Vertices;
            Ref<Mesh> GpuMesh;
        };
        std::mutex s_Mutex;
        std::map<std::tuple<float, float, float, int>, Grid> s_Grids;
        Ref<Mesh> s_OceanMesh; // released with the grids (before the GPU goes away)

        MeshData BuildGrid(const WaterVolumeComponent& water)
        {
            int n = ResolutionOf(water);
            glm::vec2 size = glm::max(glm::vec2(water.Size.x, water.Size.z), glm::vec2(0.01f));
            std::vector<Vertex> vertices;
            std::vector<uint32_t> indices;
            vertices.reserve(size_t(n + 1) * (n + 1));
            for (int j = 0; j <= n; ++j)
                for (int i = 0; i <= n; ++i)
                {
                    float u = (float)i / n, v = (float)j / n;
                    Vertex vert;
                    vert.Position = { (u - 0.5f) * size.x, TopOf(water), (0.5f - v) * size.y }; // +U along +X, +V along -Z: faces +Y
                    vert.Normal = { 0.0f, 1.0f, 0.0f };
                    vert.UV = { u * size.x * 0.1f, (1.0f - v) * size.y * 0.1f }; // a texture tiles every 10 m
                    vert.Tangent = { 1.0f, 0.0f, 0.0f, 1.0f };
                    vertices.push_back(vert);
                }
            auto at = [n](int i, int j) { return (uint32_t)(j * (n + 1) + i); };
            for (int j = 0; j < n; ++j)
                for (int i = 0; i < n; ++i)
                    indices.insert(indices.end(), { at(i, j), at(i + 1, j), at(i + 1, j + 1), at(i, j), at(i + 1, j + 1), at(i, j + 1) });
            return MeshData::FromArrays(std::move(vertices), std::move(indices), "Water");
        }

        Grid& GridFor(const WaterVolumeComponent& water, bool gpu)
        {
            Grid& g = s_Grids[std::make_tuple(water.Size.x, water.Size.y, water.Size.z, ResolutionOf(water))];
            if (g.Vertices.empty()) g.Vertices = BuildGrid(water).Vertices;
            if (gpu && !g.GpuMesh) g.GpuMesh = MakeRef<Mesh>(BuildGrid(water));
            return g;
        }
    }

    void Bounds(const WaterVolumeComponent& water, const glm::vec3& center, const glm::vec3& scale, glm::vec3& lo, glm::vec3& hi)
    {
        glm::vec3 half = water.Size * glm::max(glm::abs(scale), glm::vec3(1e-4f)) * 0.5f;
        if (water.Ocean) half.x = half.z = 1e7f;
        lo = center - half;
        hi = center + half;
    }

    namespace
    {
        constexpr int kOceanRings = 96, kOceanSegments = 192;
        constexpr float kOceanSpread = 2.4f; // ring k sits at radius * (k / rings)^spread

        // Ring k's vertices, around: index 1 + (k - 1) * segments + s (vertex 0 is the center).
        uint32_t OceanIndex(int ring, int segment) { return ring == 0 ? 0u : (uint32_t)(1 + (ring - 1) * kOceanSegments + segment % kOceanSegments); }
    }

    Ref<Mesh> OceanMesh()
    {
        std::lock_guard lock(s_Mutex);
        Ref<Mesh>& mesh = s_OceanMesh;
        if (mesh) return mesh;
        std::vector<Vertex> vertices(1 + (size_t)kOceanRings * kOceanSegments);
        auto place = [](int ring, int segment) {
            float r = std::pow((float)ring / kOceanRings, kOceanSpread), a = glm::two_pi<float>() * segment / kOceanSegments;
            return glm::vec3(r * std::cos(a), 0.0f, r * std::sin(a));
        };
        vertices[0] = { glm::vec3(0.0f), { 0, 1, 0 }, { 0, 0 }, { 1, 0, 0, 1 } };
        for (int k = 1; k <= kOceanRings; ++k)
            for (int s = 0; s < kOceanSegments; ++s) vertices[OceanIndex(k, s)] = { place(k, s), { 0, 1, 0 }, { 0, 0 }, { 1, 0, 0, 1 } };
        std::vector<uint32_t> indices;
        // Triangles facing up (+Y), whatever order the corners come in.
        auto tri = [&](uint32_t a, uint32_t b, uint32_t c) {
            glm::vec3 n = glm::cross(vertices[b].Position - vertices[a].Position, vertices[c].Position - vertices[a].Position);
            if (n.y < 0.0f) std::swap(b, c);
            indices.insert(indices.end(), { a, b, c });
        };
        for (int s = 0; s < kOceanSegments; ++s) tri(0, OceanIndex(1, s), OceanIndex(1, s + 1));
        for (int k = 1; k < kOceanRings; ++k)
            for (int s = 0; s < kOceanSegments; ++s)
            {
                tri(OceanIndex(k, s), OceanIndex(k, s + 1), OceanIndex(k + 1, s));
                tri(OceanIndex(k, s + 1), OceanIndex(k + 1, s + 1), OceanIndex(k + 1, s));
            }
        mesh = MakeRef<Mesh>(MeshData::FromArrays(std::move(vertices), std::move(indices), "Ocean"));
        return mesh;
    }

    float CrestFoam(const WaterVolumeComponent& water, glm::vec2 rest, float time)
    {
        Wave waves[kWaves];
        MakeWaves(water, waves);
        return HasWaves(water) ? CrestFoamWith(waves, rest, time) : 0.0f;
    }

    bool ShoreFoamAt(Scene& scene, Entity e, const glm::vec3& p, int& budget, float& foam)
    {
        foam = 0.0f;
        auto* water = scene.GetRegistry().TryGet<WaterVolumeComponent>(e);
        if (!water || water->ShoreFoam <= 0.0f || !scene.GetPhysics3D()) return true;
        // Cells in tiles of 32 x 32, the last tile looked up kept (neighbouring points share it).
        int cx = (int)std::floor(p.x), cz = (int)std::floor(p.z);
        int tx = cx >> 5, tz = cz >> 5;
        int64_t key = ((int64_t)tx << 32) ^ (int64_t)(uint32_t)tz;
        if (water->ShoreLastOwner != water || water->ShoreLastKey != key || !water->ShoreLastTile)
        {
            auto it = water->ShoreTiles.find(key);
            if (it == water->ShoreTiles.end()) it = water->ShoreTiles.emplace(key, std::vector<float>(1024, std::numeric_limits<float>::quiet_NaN())).first;
            water->ShoreLastOwner = water;
            water->ShoreLastKey = key;
            water->ShoreLastTile = &it->second; // map nodes do not move
        }
        float& depth = (*water->ShoreLastTile)[(size_t)(cz & 31) * 32 + (cx & 31)];
        if (std::isnan(depth))
        {
            if (budget <= 0) return false;
            --budget;
            depth = DepthBelow(scene, glm::vec3(cx + 0.5f, p.y, cz + 0.5f), water->ShoreFoam * 2.0f);
        }
        foam = ShoreFoamFromDepth(*water, depth);
        return true;
    }

    void OceanVertices(const WaterVolumeComponent& water, glm::vec2 center, float radius, float time, std::vector<Vertex>& out)
    {
        Wave waves[kWaves];
        MakeWaves(water, waves);
        bool moving = HasWaves(water);
        float top = TopOf(water);
        // The waves die down between 6 and 18 wave lengths away.
        float fadeStart = std::max(water.WaveLength, 0.1f) * 6.0f, fadeEnd = std::max(water.WaveLength, 0.1f) * 18.0f;
        out.resize(1 + (size_t)kOceanRings * kOceanSegments);
        for (int k = 0; k <= kOceanRings; ++k)
        {
            float r = radius * std::pow((float)k / kOceanRings, kOceanSpread);
            float strength = moving ? std::clamp((fadeEnd - r) / (fadeEnd - fadeStart), 0.0f, 1.0f) : 0.0f;
            for (int s = 0; s < (k == 0 ? 1 : kOceanSegments); ++s)
            {
                float a = glm::two_pi<float>() * s / kOceanSegments;
                glm::vec2 rest = center + glm::vec2(std::cos(a), std::sin(a)) * r;
                Vertex& v = out[OceanIndex(k, s)];
                v.Position = glm::vec3(rest.x, top, rest.y);
                v.Normal = { 0.0f, 1.0f, 0.0f };
                float foam = 0.0f;
                if (strength > 0.0f)
                {
                    glm::vec3 n;
                    glm::vec3 p = DisplaceWith(waves, rest, time, &n, nullptr);
                    v.Position = glm::vec3(rest.x, top, rest.y) + (p - glm::vec3(rest.x, 0.0f, rest.y)) * strength;
                    v.Normal = glm::normalize(glm::mix(glm::vec3(0.0f, 1.0f, 0.0f), n, strength));
                    foam = CrestFoamWith(waves, rest, time) * std::max(water.Foam, 0.0f) * strength;
                }
                v.Normal *= 1.0f + std::min(foam, 1.0f); // the length carries the foam to the shader
                v.UV = rest * 0.1f; // a texture tiles every 10 m
                v.Tangent = { 1.0f, 0.0f, 0.0f, 1.0f };
            }
        }
    }

    Ref<Mesh> SurfaceMesh(const WaterVolumeComponent& water)
    {
        std::lock_guard lock(s_Mutex);
        return GridFor(water, true).GpuMesh;
    }

    const std::vector<Vertex>& SurfaceVertices(const WaterVolumeComponent& water)
    {
        std::lock_guard lock(s_Mutex);
        return GridFor(water, false).Vertices;
    }

    void Update(Scene& scene, float time)
    {
        for (auto [e, water] : scene.GetRegistry().ViewOf<WaterVolumeComponent>())
        {
            if (!scene.GetPhysics3D()) water.ShoreDepths.clear(); // editing: the ground may move; measured again in play
            // Shore depths under the grid's points, once while playing (the ground does not move).
            bool shore = water.ShoreFoam > 0.0f && scene.GetPhysics3D() && !water.Ocean;
            if (shore && water.ShoreDepths.empty())
            {
                const std::vector<Vertex>& rest = SurfaceVertices(water);
                const glm::mat4& world = scene.GetRegistry().Get<TransformComponent>(e).World;
                water.ShoreDepths.resize(rest.size());
                for (size_t i = 0; i < rest.size(); ++i)
                    water.ShoreDepths[i] = DepthBelow(scene, glm::vec3(world * glm::vec4(rest[i].Position, 1.0f)), water.ShoreFoam * 2.0f);
            }
            if ((!HasWaves(water) && !shore) || water.Ocean)
            {
                water.SimulatedPositions.clear();
                water.SimulatedNormals.clear();
                continue;
            }
            const std::vector<Vertex>& rest = SurfaceVertices(water);
            Wave waves[kWaves];
            MakeWaves(water, waves);
            water.SimulatedPositions.resize(rest.size());
            water.SimulatedNormals.resize(rest.size());
            float top = TopOf(water);
            bool crests = HasWaves(water) && water.Foam > 0.0f;
            for (size_t i = 0; i < rest.size(); ++i)
            {
                glm::vec2 r(rest[i].Position.x, rest[i].Position.z);
                glm::vec3 p = DisplaceWith(waves, r, time, &water.SimulatedNormals[i], nullptr);
                p.y += top;
                water.SimulatedPositions[i] = p;
                float foam = crests ? CrestFoamWith(waves, r, time) * water.Foam : 0.0f;
                if (shore && i < water.ShoreDepths.size()) foam = std::max(foam, ShoreFoamFromDepth(water, water.ShoreDepths[i]));
                water.SimulatedNormals[i] *= 1.0f + std::min(foam, 1.0f); // the length carries the foam to the shader
            }
        }
    }

    void ReleaseMeshes()
    {
        std::lock_guard lock(s_Mutex);
        s_Grids.clear();
        s_OceanMesh.reset();
    }
}
