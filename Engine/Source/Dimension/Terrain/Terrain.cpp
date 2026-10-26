#include "Dimension/Terrain/Terrain.h"
#include "Dimension/Scene/Components.h"
#include "Dimension/Particles/ParticleSystem.h"
#include "Dimension/Asset/AssetManager.h"
#include "Dimension/Renderer/Texture.h"
#include "Dimension/Core/Log.h"
#include <stb_image.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>

namespace Dimension::Terrain
{
    namespace
    {
        // A heightmap image as 0-1 grayscale (16-bit images keep their precision).
        struct HeightImage
        {
            int Width = 0, Height = 0;
            std::vector<float> Values;
        };

        const HeightImage* LoadHeightmap(AssetHandle handle)
        {
            static std::unordered_map<uint64_t, std::pair<fs::file_time_type, HeightImage>> cache;
            fs::path path = AssetManager::GetAbsolutePath(handle);
            std::error_code ec;
            if (path.empty() || !fs::exists(path, ec)) return nullptr;
            auto stamp = fs::last_write_time(path, ec);
            auto& entry = cache[(uint64_t)handle];
            if (!entry.second.Values.empty() && entry.first == stamp) return &entry.second;
            int w = 0, h = 0, channels = 0;
            stbi_us* pixels = stbi_load_16(path.string().c_str(), &w, &h, &channels, 1);
            if (!pixels)
            {
                Log::Warn("Terrain heightmap '{}' could not be read", path.filename().string());
                return nullptr;
            }
            HeightImage image{ w, h, std::vector<float>((size_t)w * h) };
            for (size_t i = 0; i < image.Values.size(); ++i) image.Values[i] = pixels[i] / 65535.0f;
            stbi_image_free(pixels);
            entry = { stamp, std::move(image) };
            return &entry.second;
        }

        float SampleImage(const HeightImage& image, float u, float v)
        {
            float x = std::clamp(u, 0.0f, 1.0f) * (image.Width - 1), y = std::clamp(v, 0.0f, 1.0f) * (image.Height - 1);
            int x0 = (int)x, y0 = (int)y, x1 = std::min(x0 + 1, image.Width - 1), y1 = std::min(y0 + 1, image.Height - 1);
            float fx = x - x0, fy = y - y0;
            auto at = [&](int px, int py) { return image.Values[(size_t)py * image.Width + px]; };
            return glm::mix(glm::mix(at(x0, y0), at(x1, y0), fx), glm::mix(at(x0, y1), at(x1, y1), fx), fy);
        }

        float Smooth(float a, float b, float x)
        {
            // 0 at a, 1 at b (either way round).
            float d = b - a;
            if (std::abs(d) < 1e-6f) d = d < 0.0f ? -1e-6f : 1e-6f;
            float t = std::clamp((x - a) / d, 0.0f, 1.0f);
            return t * t * (3.0f - 2.0f * t);
        }
    }

    int SampleCount(const TerrainComponent& terrain)
    {
        int n = std::clamp(terrain.Resolution, 4, 1024);
        return n + (n & 1);
    }

    std::vector<float> BuildHeights(const TerrainComponent& terrain)
    {
        int n = SampleCount(terrain);
        std::vector<float> heights((size_t)n * n, 0.0f);
        const HeightImage* image = terrain.Heightmap.IsValid() ? LoadHeightmap(terrain.Heightmap) : nullptr;
        glm::vec2 size = glm::max(terrain.Size, glm::vec2(0.01f));
        float seedOffset = (float)(terrain.Seed % 10007) * 17.137f;
        for (int row = 0; row < n; ++row)
            for (int col = 0; col < n; ++col)
            {
                float u = col / (float)(n - 1), v = row / (float)(n - 1);
                float h;
                if (image)
                    h = SampleImage(*image, u, 1.0f - v); // image top = far (+Z) edge
                else
                {
                    // Fractal noise: octaves of gradient noise, each finer and fainter.
                    glm::vec2 p((u - 0.5f) * size.x, (v - 0.5f) * size.y);
                    float frequency = std::max(terrain.NoiseScale, 1e-5f), amplitude = 1.0f, sum = 0.0f, norm = 0.0f;
                    for (int o = 0; o < std::clamp(terrain.Octaves, 1, 10); ++o)
                    {
                        sum += ParticleSystems::GradientNoise(glm::vec3(p.x * frequency, seedOffset + o * 31.7f, p.y * frequency)) * amplitude;
                        norm += amplitude;
                        amplitude *= std::clamp(terrain.Roughness, 0.0f, 1.0f);
                        frequency *= 2.0f;
                    }
                    h = std::clamp(sum / std::max(norm, 1e-6f) * 1.4f * 0.5f + 0.5f, 0.0f, 1.0f);
                }
                // A flat area in the middle (for building on), blending out to the full terrain.
                if (terrain.FlatCenter > 0.0f)
                {
                    float r = glm::length(glm::vec2(u - 0.5f, v - 0.5f)) * 2.0f;
                    float keep = Smooth(terrain.FlatCenter, terrain.FlatCenter + 0.25f, r);
                    h = glm::mix(terrain.FlatHeight, h, keep);
                }
                heights[(size_t)row * n + col] = h * terrain.Height;
            }
        return heights;
    }

    float HeightAt(const TerrainComponent& terrain, const std::vector<float>& heights, float x, float z)
    {
        int n = SampleCount(terrain);
        if ((int)heights.size() < n * n) return 0.0f;
        glm::vec2 size = glm::max(terrain.Size, glm::vec2(0.01f));
        float fx = std::clamp((x / size.x + 0.5f) * (n - 1), 0.0f, (float)(n - 1)), fz = std::clamp((z / size.y + 0.5f) * (n - 1), 0.0f, (float)(n - 1));
        int c0 = std::min((int)fx, n - 2), r0 = std::min((int)fz, n - 2);
        float tx = fx - c0, tz = fz - r0;
        auto at = [&](int c, int r) { return heights[(size_t)r * n + c]; };
        // The same triangle split as the mesh and the height field (diagonal from (c0, r0) to (c0 + 1, r0 + 1)).
        if (tx >= tz) return at(c0, r0) + (at(c0 + 1, r0) - at(c0, r0)) * tx + (at(c0 + 1, r0 + 1) - at(c0 + 1, r0)) * tz;
        return at(c0, r0) + (at(c0, r0 + 1) - at(c0, r0)) * tz + (at(c0 + 1, r0 + 1) - at(c0, r0 + 1)) * tx;
    }

    MeshData BuildMesh(const TerrainComponent& terrain, const std::vector<float>& heights)
    {
        int n = SampleCount(terrain);
        glm::vec2 size = glm::max(terrain.Size, glm::vec2(0.01f));
        glm::vec2 step = size / (float)(n - 1);
        std::vector<Vertex> vertices((size_t)n * n);
        auto h = [&](int c, int r) { return heights[(size_t)std::clamp(r, 0, n - 1) * n + std::clamp(c, 0, n - 1)]; };
        for (int r = 0; r < n; ++r)
            for (int c = 0; c < n; ++c)
            {
                Vertex& v = vertices[(size_t)r * n + c];
                v.Position = { -size.x * 0.5f + c * step.x, h(c, r), -size.y * 0.5f + r * step.y };
                v.Normal = glm::normalize(glm::vec3((h(c - 1, r) - h(c + 1, r)) / (2.0f * step.x), 1.0f, (h(c, r - 1) - h(c, r + 1)) / (2.0f * step.y)));
                v.UV = { c / (float)(n - 1), r / (float)(n - 1) };
                v.Tangent = glm::vec4(0.0f);
            }
        std::vector<uint32_t> indices;
        indices.reserve((size_t)(n - 1) * (n - 1) * 6);
        for (int r = 0; r + 1 < n; ++r)
            for (int c = 0; c + 1 < n; ++c)
            {
                uint32_t a = r * n + c, b = a + 1, d = a + n, e = d + 1;
                // Counter-clockwise seen from above, split along (c, r) -> (c + 1, r + 1).
                indices.insert(indices.end(), { a, e, b, a, d, e });
            }
        MeshData mesh = MeshData::FromArrays(std::move(vertices), std::move(indices), "Terrain");
        return mesh;
    }

    std::vector<uint32_t> BuildSplat(const TerrainComponent& terrain, const MeshData& mesh)
    {
        std::vector<uint32_t> splat(mesh.Vertices.size());
        float slopeAt = std::cos(glm::radians(std::clamp(terrain.RockSlope, 1.0f, 89.0f)));
        for (size_t i = 0; i < mesh.Vertices.size(); ++i)
        {
            const Vertex& v = mesh.Vertices[i];
            float up = v.Normal.y;
            float rock = Smooth(slopeAt + 0.06f, slopeAt - 0.06f, up);
            float height = terrain.Height > 0.0f ? v.Position.y / terrain.Height : 0.0f;
            float snow = Smooth(terrain.SnowHeight - 0.04f, terrain.SnowHeight + 0.04f, height) * (1.0f - rock * 0.7f);
            float grass = std::max(0.0f, 1.0f - rock - snow);
            float sum = std::max(grass + rock + snow, 1e-4f);
            auto byte = [](float x) { return (uint32_t)std::clamp((int)std::lround(x * 255.0f), 0, 255); };
            splat[i] = byte(grass / sum) | byte(rock / sum) << 8 | byte(snow / sum) << 16 | 0xFF000000u;
        }
        return splat;
    }

    uint64_t SettingsKey(const TerrainComponent& t)
    {
        uint64_t h = 1469598103934665603ull;
        auto mix = [&](const void* p, size_t n) {
            const auto* b = static_cast<const uint8_t*>(p);
            for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 1099511628211ull;
        };
        mix(&t.Size, sizeof(t.Size));
        mix(&t.Height, sizeof(t.Height));
        mix(&t.Resolution, sizeof(t.Resolution));
        uint64_t map = (uint64_t)t.Heightmap;
        mix(&map, sizeof(map));
        mix(&t.Seed, sizeof(t.Seed));
        mix(&t.NoiseScale, sizeof(t.NoiseScale));
        mix(&t.Octaves, sizeof(t.Octaves));
        mix(&t.Roughness, sizeof(t.Roughness));
        mix(&t.FlatCenter, sizeof(t.FlatCenter));
        mix(&t.FlatHeight, sizeof(t.FlatHeight));
        mix(&t.RockSlope, sizeof(t.RockSlope));
        mix(&t.SnowHeight, sizeof(t.SnowHeight));
        if (t.Heightmap.IsValid())
        {
            std::error_code ec;
            auto stamp = fs::last_write_time(AssetManager::GetAbsolutePath(t.Heightmap), ec).time_since_epoch().count();
            mix(&stamp, sizeof(stamp));
        }
        return h;
    }

    namespace
    {
        std::unordered_map<uint64_t, Runtime>& Runtimes()
        {
            static std::unordered_map<uint64_t, Runtime> runtimes;
            return runtimes;
        }
    }

    const Runtime* GetRuntime(const TerrainComponent& terrain)
    {
        uint64_t key = SettingsKey(terrain);
        auto& runtimes = Runtimes();
        if (auto it = runtimes.find(key); it != runtimes.end()) return &it->second;
        if (runtimes.size() > 16) runtimes.clear(); // edited settings leave old builds behind; drop them now and then
        MeshData data = BuildMesh(terrain, BuildHeights(terrain));
        std::vector<uint32_t> splat = BuildSplat(terrain, data);
        int n = SampleCount(terrain);
        Runtime rt;
        rt.Key = key;
        rt.Surface = MakeRef<Mesh>(std::move(data.Vertices), std::move(data.Indices), "Terrain");
        TextureSettings settings;
        settings.SRGB = false;
        settings.GenerateMips = true;
        rt.Splat = MakeRef<Texture2D>(splat.data(), (uint32_t)n, (uint32_t)n, settings, "TerrainSplat");
        return &(runtimes[key] = std::move(rt));
    }

    void ReleaseRuntimes() { Runtimes().clear(); }
}
