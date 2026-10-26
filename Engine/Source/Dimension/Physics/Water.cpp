#include "Dimension/Physics/Water.h"
#include "Dimension/Scene/Scene.h"
#include <glm/gtc/constants.hpp>
#include <algorithm>
#include <cmath>
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
            if (!HasWaves(water))
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
            for (size_t i = 0; i < rest.size(); ++i)
            {
                glm::vec3 p = DisplaceWith(waves, glm::vec2(rest[i].Position.x, rest[i].Position.z), time, &water.SimulatedNormals[i], nullptr);
                p.y += top;
                water.SimulatedPositions[i] = p;
            }
        }
    }

    void ReleaseMeshes()
    {
        std::lock_guard lock(s_Mutex);
        s_Grids.clear();
    }
}
