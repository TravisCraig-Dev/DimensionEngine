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
#include <mutex>
#include <unordered_map>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>

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

    namespace
    {
        const char* kBase64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

        std::string Encode(const uint8_t* data, size_t size)
        {
            std::string out;
            out.reserve((size + 2) / 3 * 4);
            for (size_t i = 0; i < size; i += 3)
            {
                uint32_t v = (uint32_t)data[i] << 16 | (i + 1 < size ? (uint32_t)data[i + 1] << 8 : 0u) | (i + 2 < size ? data[i + 2] : 0u);
                out += kBase64[v >> 18 & 63];
                out += kBase64[v >> 12 & 63];
                out += i + 1 < size ? kBase64[v >> 6 & 63] : '=';
                out += i + 2 < size ? kBase64[v & 63] : '=';
            }
            return out;
        }

        std::vector<uint8_t> Decode(const std::string& text)
        {
            int table[256];
            std::fill(std::begin(table), std::end(table), -1);
            for (int i = 0; i < 64; ++i) table[(uint8_t)kBase64[i]] = i;
            std::vector<uint8_t> out;
            out.reserve(text.size() / 4 * 3);
            uint32_t acc = 0;
            int bits = 0;
            for (char c : text)
            {
                int v = table[(uint8_t)c];
                if (v < 0) continue; // padding, whitespace
                acc = acc << 6 | (uint32_t)v;
                bits += 6;
                if (bits >= 8)
                {
                    bits -= 8;
                    out.push_back((uint8_t)(acc >> bits & 0xFF));
                }
            }
            return out;
        }

        // The sculpted heights (meters) when the edits match the resolution; empty otherwise.
        std::vector<float> EditedHeights(const TerrainComponent& t, int n)
        {
            if (t.EditHeights.size() == (size_t)n * n) return t.EditHeights;
            if (t.SculptedHeights.empty()) return {};
            std::vector<uint8_t> bytes = Decode(t.SculptedHeights);
            if (bytes.size() != (size_t)n * n * 2) return {};
            std::vector<float> h((size_t)n * n);
            for (size_t i = 0; i < h.size(); ++i) h[i] = (bytes[i * 2] | bytes[i * 2 + 1] << 8) / 65535.0f * t.Height;
            return h;
        }

        std::vector<uint32_t> EditedLayers(const TerrainComponent& t, size_t count)
        {
            if (t.EditLayers.size() == count) return t.EditLayers;
            if (t.PaintedLayers.empty()) return {};
            std::vector<uint8_t> bytes = Decode(t.PaintedLayers);
            if (bytes.size() != count * 3) return {};
            std::vector<uint32_t> s(count);
            for (size_t i = 0; i < count; ++i) s[i] = bytes[i * 3] | bytes[i * 3 + 1] << 8 | bytes[i * 3 + 2] << 16 | 0xFF000000u;
            return s;
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
        if (std::vector<float> edited = EditedHeights(terrain, n); !edited.empty()) return edited;
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
        if (std::vector<uint32_t> painted = EditedLayers(terrain, mesh.Vertices.size()); !painted.empty()) return painted;
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
        auto mixString = [&](const std::string& s) {
            size_t size = s.size();
            mix(&size, sizeof(size));
            mix(s.data(), s.size());
        };
        mixString(t.SculptedHeights);
        mixString(t.PaintedLayers);
        mix(&t.EditRevision, sizeof(t.EditRevision));
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

    std::vector<uint8_t> DetailMap(const TerrainComponent& terrain)
    {
        int n = SampleCount(terrain);
        size_t count = (size_t)n * n * 2;
        if (terrain.EditDetails.size() == count) return terrain.EditDetails;
        if (terrain.PaintedDetails.empty()) return {};
        std::vector<uint8_t> bytes = Decode(terrain.PaintedDetails);
        if (bytes.size() != count) return {}; // painted at another resolution
        return bytes;
    }

    void BeginEdit(TerrainComponent& terrain)
    {
        int n = SampleCount(terrain);
        if (terrain.EditHeights.size() != (size_t)n * n) terrain.EditHeights = BuildHeights(terrain);
        if (terrain.EditDetails.size() != (size_t)n * n * 2)
        {
            terrain.EditDetails = DetailMap(terrain);
            terrain.EditDetails.resize((size_t)n * n * 2, 0);
        }
        if (terrain.EditLayers.size() != (size_t)n * n)
        {
            MeshData mesh = BuildMesh(terrain, terrain.EditHeights);
            terrain.EditLayers = BuildSplat(terrain, mesh);
        }
    }

    bool ApplyBrush(TerrainComponent& terrain, BrushMode mode, const glm::vec2& at, float radius, float strength, float dt, float flattenHeight)
    {
        BeginEdit(terrain);
        int n = SampleCount(terrain);
        glm::vec2 size = glm::max(terrain.Size, glm::vec2(0.01f));
        if (std::abs(at.x) > size.x * 0.5f + radius || std::abs(at.y) > size.y * 0.5f + radius) return false;
        glm::vec2 cell = size / (float)(n - 1);
        radius = std::max(radius, std::min(cell.x, cell.y));
        int c0 = std::max(0, (int)std::floor((at.x - radius) / cell.x + (n - 1) * 0.5f)), c1 = std::min(n - 1, (int)std::ceil((at.x + radius) / cell.x + (n - 1) * 0.5f));
        int r0 = std::max(0, (int)std::floor((at.y - radius) / cell.y + (n - 1) * 0.5f)), r1 = std::min(n - 1, (int)std::ceil((at.y + radius) / cell.y + (n - 1) * 0.5f));
        std::vector<float> before = mode == BrushMode::Smooth ? terrain.EditHeights : std::vector<float>();
        float rate = std::clamp(strength, 0.0f, 1.0f) * dt;
        for (int r = r0; r <= r1; ++r)
            for (int c = c0; c <= c1; ++c)
            {
                glm::vec2 p(-size.x * 0.5f + c * cell.x, -size.y * 0.5f + r * cell.y);
                float d = glm::length(p - at) / radius;
                if (d >= 1.0f) continue;
                float falloff = 1.0f - Smooth(0.0f, 1.0f, d); // soft edge
                size_t i = (size_t)r * n + c;
                float& h = terrain.EditHeights[i];
                if (IsDetailMode(mode))
                {
                    // Detail density moves toward full (or, erasing, toward none for both kinds).
                    float amount = std::min(rate * 4.0f * falloff, 1.0f);
                    for (int k = 0; k < 2; ++k)
                    {
                        bool target = (mode == BrushMode::PlaceTrees && k == 0) || (mode == BrushMode::PlaceGrass && k == 1);
                        if (mode != BrushMode::EraseDetails && !target) continue;
                        uint8_t& d = terrain.EditDetails[i * 2 + k];
                        float v = d / 255.0f;
                        v += ((target ? 1.0f : 0.0f) - v) * amount;
                        int next = std::clamp((int)std::lround(v * 255.0f), 0, 255);
                        if (next == d && amount > 0.0f) next = std::clamp(d + (target ? 1 : -1), 0, 255); // rounding would stall it
                        d = (uint8_t)next;
                    }
                    continue;
                }
                switch (mode)
                {
                case BrushMode::Raise: h += terrain.Height * 0.5f * rate * falloff; break;
                case BrushMode::Lower: h -= terrain.Height * 0.5f * rate * falloff; break;
                case BrushMode::Flatten: h += (flattenHeight - h) * std::min(rate * 6.0f * falloff, 1.0f); break;
                case BrushMode::Smooth:
                {
                    float sum = 0.0f;
                    int count = 0;
                    for (int dr = -1; dr <= 1; ++dr)
                        for (int dc = -1; dc <= 1; ++dc)
                        {
                            int rr = std::clamp(r + dr, 0, n - 1), cc = std::clamp(c + dc, 0, n - 1);
                            sum += before[(size_t)rr * n + cc];
                            ++count;
                        }
                    h += (sum / count - h) * std::min(rate * 8.0f * falloff, 1.0f);
                    break;
                }
                default:
                {
                    // Painting: move this sample's weights toward the layer.
                    int layer = (int)mode - (int)BrushMode::PaintGrass;
                    uint32_t& s = terrain.EditLayers[i];
                    float w[3] = { (s & 255) / 255.0f, (s >> 8 & 255) / 255.0f, (s >> 16 & 255) / 255.0f };
                    float amount = std::min(rate * 4.0f * falloff, 1.0f);
                    for (int k = 0; k < 3; ++k) w[k] += ((k == layer ? 1.0f : 0.0f) - w[k]) * amount;
                    auto byte = [](float x) { return (uint32_t)std::clamp((int)std::lround(x * 255.0f), 0, 255); };
                    s = byte(w[0]) | byte(w[1]) << 8 | byte(w[2]) << 16 | 0xFF000000u;
                    break;
                }
                }
                h = std::clamp(h, 0.0f, terrain.Height);
            }
        ++terrain.EditRevision;
        return true;
    }

    void CommitEdit(TerrainComponent& terrain)
    {
        int n = SampleCount(terrain);
        if (terrain.EditHeights.size() == (size_t)n * n)
        {
            std::vector<uint8_t> bytes((size_t)n * n * 2);
            for (size_t i = 0; i < terrain.EditHeights.size(); ++i)
            {
                uint16_t v = (uint16_t)std::clamp((int)std::lround(terrain.EditHeights[i] / std::max(terrain.Height, 1e-4f) * 65535.0f), 0, 65535);
                bytes[i * 2] = (uint8_t)(v & 0xFF);
                bytes[i * 2 + 1] = (uint8_t)(v >> 8);
            }
            terrain.SculptedHeights = Encode(bytes.data(), bytes.size());
        }
        if (terrain.EditLayers.size() == (size_t)n * n)
        {
            std::vector<uint8_t> bytes((size_t)n * n * 3);
            for (size_t i = 0; i < terrain.EditLayers.size(); ++i)
            {
                bytes[i * 3] = (uint8_t)(terrain.EditLayers[i] & 255);
                bytes[i * 3 + 1] = (uint8_t)(terrain.EditLayers[i] >> 8 & 255);
                bytes[i * 3 + 2] = (uint8_t)(terrain.EditLayers[i] >> 16 & 255);
            }
            terrain.PaintedLayers = Encode(bytes.data(), bytes.size());
        }
        if (terrain.EditDetails.size() == (size_t)n * n * 2)
        {
            bool any = std::any_of(terrain.EditDetails.begin(), terrain.EditDetails.end(), [](uint8_t d) { return d != 0; });
            terrain.PaintedDetails = any ? Encode(terrain.EditDetails.data(), terrain.EditDetails.size()) : std::string();
        }
        // The serialized edits are now the source; the stored heights round to 16 bits.
        terrain.EditHeights.clear();
        terrain.EditLayers.clear();
        terrain.EditDetails.clear();
        ++terrain.EditRevision;
    }

    void ClearDetails(TerrainComponent& terrain)
    {
        terrain.PaintedDetails.clear();
        terrain.EditDetails.clear();
        ++terrain.EditRevision;
    }

    namespace
    {
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
    }

    DetailInstances BuildDetails(const TerrainComponent& terrain, const std::vector<float>& heights)
    {
        DetailInstances out;
        std::vector<uint8_t> map = DetailMap(terrain);
        if (map.empty()) return out;
        int n = SampleCount(terrain);
        glm::vec2 size = glm::max(terrain.Size, glm::vec2(0.01f));
        glm::vec2 step = size / (float)(n - 1);
        auto densityAt = [&](float x, float z, int kind) {
            float fx = std::clamp((x / size.x + 0.5f) * (n - 1), 0.0f, (float)(n - 1)), fz = std::clamp((z / size.y + 0.5f) * (n - 1), 0.0f, (float)(n - 1));
            int c0 = std::min((int)fx, n - 2), r0 = std::min((int)fz, n - 2);
            float tx = fx - c0, tz = fz - r0;
            auto at = [&](int c, int r) { return map[((size_t)r * n + c) * 2 + kind] / 255.0f; };
            return (at(c0, r0) * (1 - tx) + at(c0 + 1, r0) * tx) * (1 - tz) + (at(c0, r0 + 1) * (1 - tx) + at(c0 + 1, r0 + 1) * tx) * tz;
        };
        auto upAt = [&](float x, float z) {
            float dx = (HeightAt(terrain, heights, x + step.x, z) - HeightAt(terrain, heights, x - step.x, z)) / (2.0f * step.x);
            float dz = (HeightAt(terrain, heights, x, z + step.y) - HeightAt(terrain, heights, x, z - step.y)) / (2.0f * step.y);
            return 1.0f / std::sqrt(1.0f + dx * dx + dz * dz); // the normal's y
        };
        for (int kind = 0; kind < 2; ++kind)
        {
            float density = kind == 0 ? terrain.TreeDensity : terrain.GrassDensity;
            glm::vec2 scale = kind == 0 ? terrain.TreeScale : terrain.GrassScale;
            float maxSlope = kind == 0 ? terrain.TreeMaxSlope : terrain.GrassMaxSlope;
            if (density <= 0.0f) continue;
            constexpr float kMaxCells = 60000.0f; // per kind: denser settings spread the same count wider
            float cell = std::max(std::sqrt(100.0f / density), std::sqrt(size.x * size.y / kMaxCells));
            int cx = std::max(1, (int)std::ceil(size.x / cell)), cz = std::max(1, (int)std::ceil(size.y / cell));
            float minUp = std::cos(glm::radians(std::clamp(maxSlope, 0.0f, 90.0f)));
            auto& list = kind == 0 ? out.Trees : out.Grass;
            for (int j = 0; j < cz; ++j)
                for (int i = 0; i < cx; ++i)
                {
                    uint32_t state = Hash((uint32_t)i * 73856093u ^ (uint32_t)j * 19349663u ^ (uint32_t)(kind + 1) * 83492791u ^ (uint32_t)terrain.Seed * 2654435761u);
                    float x = -size.x * 0.5f + (i + Random01(state)) * cell, z = -size.y * 0.5f + (j + Random01(state)) * cell;
                    float chance = Random01(state);
                    if (std::abs(x) > size.x * 0.5f || std::abs(z) > size.y * 0.5f || chance >= densityAt(x, z, kind)) continue;
                    if (upAt(x, z) < minUp) continue;
                    float yaw = Random01(state) * glm::two_pi<float>();
                    float s = glm::mix(scale.x, scale.y, Random01(state));
                    glm::mat4 m = glm::rotate(glm::translate(glm::mat4(1.0f), glm::vec3(x, HeightAt(terrain, heights, x, z), z)), yaw, glm::vec3(0, 1, 0));
                    list.push_back(glm::scale(m, glm::vec3(std::max(s, 0.01f))));
                }
        }
        return out;
    }

    const DetailInstances& GetDetails(const TerrainComponent& terrain)
    {
        uint64_t key = SettingsKey(terrain);
        auto mix = [&](const void* p, size_t size) {
            const auto* b = static_cast<const uint8_t*>(p);
            for (size_t i = 0; i < size; ++i) key = (key ^ b[i]) * 1099511628211ull;
        };
        for (const float* f : { &terrain.TreeDensity, &terrain.TreeScale.x, &terrain.TreeScale.y, &terrain.TreeMaxSlope, &terrain.GrassDensity,
                                &terrain.GrassScale.x, &terrain.GrassScale.y, &terrain.GrassMaxSlope })
            mix(f, sizeof(float));
        mix(terrain.PaintedDetails.data(), terrain.PaintedDetails.size());
        static std::mutex mutex;
        static std::unordered_map<uint64_t, DetailInstances> cache;
        std::lock_guard lock(mutex);
        if (auto it = cache.find(key); it != cache.end()) return it->second;
        if (cache.size() > 16) cache.clear();
        return cache[key] = BuildDetails(terrain, terrain.EditHeights.size() == (size_t)SampleCount(terrain) * SampleCount(terrain) ? terrain.EditHeights : BuildHeights(terrain));
    }

    void ClearEdits(TerrainComponent& terrain)
    {
        terrain.SculptedHeights.clear();
        terrain.PaintedLayers.clear();
        terrain.EditHeights.clear();
        terrain.EditLayers.clear();
        ++terrain.EditRevision;
    }

    bool Raycast(const TerrainComponent& terrain, const glm::mat4& world, const glm::vec3& origin, const glm::vec3& direction, glm::vec3& hit)
    {
        // March in the object's space, then bisect the crossing.
        glm::mat4 inv = glm::inverse(world);
        glm::vec3 o = glm::vec3(inv * glm::vec4(origin, 1.0f)), d = glm::vec3(inv * glm::vec4(direction, 0.0f));
        float len = glm::length(d);
        if (len < 1e-8f) return false;
        d /= len;
        std::vector<float> heights = BuildHeights(terrain);
        glm::vec2 half = glm::max(terrain.Size, glm::vec2(0.01f)) * 0.5f;
        float reach = glm::length(glm::vec3(half.x, terrain.Height, half.y)) * 2.0f + glm::length(o);
        float step = std::max(std::min(terrain.Size.x, terrain.Size.y) / (float)SampleCount(terrain) * 0.5f, 0.01f);
        auto above = [&](const glm::vec3& p) { return p.y - HeightAt(terrain, heights, p.x, p.z); };
        float prevT = 0.0f, prev = above(o);
        for (float t = step; t < reach; t += step)
        {
            glm::vec3 p = o + d * t;
            bool inside = std::abs(p.x) <= half.x && std::abs(p.z) <= half.y;
            float a = above(p);
            if (inside && prev > 0.0f && a <= 0.0f)
            {
                float lo = prevT, hi = t;
                for (int k = 0; k < 20; ++k)
                {
                    float mid = 0.5f * (lo + hi);
                    if (above(o + d * mid) > 0.0f) lo = mid;
                    else hi = mid;
                }
                hit = glm::vec3(world * glm::vec4(o + d * hi, 1.0f));
                return true;
            }
            prevT = t;
            prev = a;
        }
        return false;
    }
}
