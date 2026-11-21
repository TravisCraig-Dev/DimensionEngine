#include "Dimension/Terrain/Terrain.h"
#include "Dimension/Scene/Components.h"
#include "Dimension/Particles/ParticleSystem.h"
#include "Dimension/Asset/AssetManager.h"
#include "Dimension/Renderer/Texture.h"
#include "Dimension/Core/Log.h"
#include "Dimension/Scene/Scene.h"
#include "Dimension/Physics/Physics3D.h"
#include "Dimension/Navigation/NavMesh.h"
#include <stb_image.h>
#include <algorithm>
#include <climits>
#include <functional>
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

        // A sample's weights as RGBA8 (4 layers).
        uint32_t PackWeights(const float* w)
        {
            auto byte = [](float x) { return (uint32_t)std::clamp((int)std::lround(x * 255.0f), 0, 255); };
            return byte(w[0]) | byte(w[1]) << 8 | byte(w[2]) << 16 | byte(w[3]) << 24;
        }

        // The splat maps (SplatMapCount * count) from the edits: a byte per layer per sample (older scenes stored 3 or
        // 6: their first layers).
        std::vector<uint32_t> EditedLayers(const TerrainComponent& t, size_t count)
        {
            size_t maps = (size_t)SplatMapCount(t);
            if (t.EditLayers.size() == count * maps) return t.EditLayers;
            if (t.PaintedLayers.empty() || count == 0) return {};
            std::vector<uint8_t> bytes = Decode(t.PaintedLayers);
            size_t stride = bytes.size() / count;
            if (stride == 0 || stride > (size_t)kMaxLayers || bytes.size() != count * stride) return {};
            std::vector<uint32_t> s(count * maps, 0u);
            int layers = LayerCount(t);
            for (size_t i = 0; i < count; ++i)
                for (size_t k = 0; k < stride && (int)k < layers; ++k) s[(k / 4) * count + i] |= (uint32_t)bytes[i * stride + k] << (k % 4 * 8);
            return s;
        }
    }

    int LayerCount(const TerrainComponent& terrain) { return std::clamp((int)terrain.Layers.size(), 1, kMaxLayers); }
    int SplatMapCount(const TerrainComponent& terrain) { return (LayerCount(terrain) + 3) / 4; }

    float LayerWeight(const TerrainComponent& terrain, size_t sample, int layer)
    {
        size_t count = terrain.EditLayers.size() / (size_t)SplatMapCount(terrain);
        if (layer < 0 || layer >= LayerCount(terrain) || sample >= count) return 0.0f;
        return (terrain.EditLayers[(size_t)(layer / 4) * count + sample] >> (layer % 4 * 8) & 255) / 255.0f;
    }

    void PaintSample(TerrainComponent& terrain, size_t sample, int layer, float amount)
    {
        int layers = LayerCount(terrain), maps = SplatMapCount(terrain);
        size_t count = terrain.EditLayers.size() / (size_t)maps;
        if (layer < 0 || layer >= layers || sample >= count) return;
        float w[kMaxLayers] = {};
        for (int k = 0; k < layers; ++k) w[k] = LayerWeight(terrain, sample, k);
        for (int k = 0; k < layers; ++k) w[k] += ((k == layer ? 1.0f : 0.0f) - w[k]) * amount;
        for (int m = 0; m < maps; ++m) terrain.EditLayers[(size_t)m * count + sample] = PackWeights(w + m * 4);
    }

    std::vector<uint8_t> HoleMap(const TerrainComponent& terrain)
    {
        int n = SampleCount(terrain);
        size_t count = (size_t)n * n;
        if (terrain.EditHoles.size() == count)
            return std::any_of(terrain.EditHoles.begin(), terrain.EditHoles.end(), [](uint8_t h) { return h != 0; }) ? terrain.EditHoles : std::vector<uint8_t>();
        if (terrain.PaintedHoles.empty()) return {};
        std::vector<uint8_t> bits = Decode(terrain.PaintedHoles);
        if (bits.size() != (count + 7) / 8) return {}; // cut at another resolution
        std::vector<uint8_t> holes(count);
        for (size_t i = 0; i < count; ++i) holes[i] = bits[i / 8] >> (i % 8) & 1;
        return holes;
    }

    bool IsHoleAt(const TerrainComponent& terrain, const std::vector<uint8_t>& holes, float x, float z)
    {
        int n = SampleCount(terrain);
        if (holes.size() != (size_t)n * n) return false;
        glm::vec2 size = glm::max(terrain.Size, glm::vec2(0.01f));
        float fx = (x / size.x + 0.5f) * (n - 1), fz = (z / size.y + 0.5f) * (n - 1);
        if (fx < 0.0f || fz < 0.0f || fx > n - 1 || fz > n - 1) return false;
        int c0 = std::min((int)fx, n - 2), r0 = std::min((int)fz, n - 2);
        auto at = [&](int c, int r) { return holes[(size_t)r * n + c] != 0; };
        // The triangle the point is in (split as in HeightAt).
        if (fx - c0 >= fz - r0) return at(c0, r0) || at(c0 + 1, r0) || at(c0 + 1, r0 + 1);
        return at(c0, r0) || at(c0, r0 + 1) || at(c0 + 1, r0 + 1);
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

    namespace
    {
        // The surface's vertex at sample (c, r): its normal from the heights around it. No tangent: the terrain shader
        // makes its own frame for the layers' normal maps.
        Vertex SurfaceVertex(const std::vector<float>& heights, int n, const glm::vec2& size, const glm::vec2& step, int c, int r)
        {
            auto h = [&](int col, int row) { return heights[(size_t)std::clamp(row, 0, n - 1) * n + std::clamp(col, 0, n - 1)]; };
            Vertex v;
            v.Position = { -size.x * 0.5f + c * step.x, h(c, r), -size.y * 0.5f + r * step.y };
            v.Normal = glm::normalize(glm::vec3((h(c - 1, r) - h(c + 1, r)) / (2.0f * step.x), 1.0f, (h(c, r - 1) - h(c, r + 1)) / (2.0f * step.y)));
            v.UV = { c / (float)(n - 1), r / (float)(n - 1) };
            v.Tangent = glm::vec4(0.0f);
            return v;
        }
    }

    MeshData BuildMesh(const TerrainComponent& terrain, const std::vector<float>& heights)
    {
        int n = SampleCount(terrain);
        glm::vec2 size = glm::max(terrain.Size, glm::vec2(0.01f));
        glm::vec2 step = size / (float)(n - 1);
        std::vector<Vertex> vertices((size_t)n * n);
        for (int r = 0; r < n; ++r)
            for (int c = 0; c < n; ++c) vertices[(size_t)r * n + c] = SurfaceVertex(heights, n, size, step, c, r);
        std::vector<uint32_t> indices;
        indices.reserve((size_t)(n - 1) * (n - 1) * 6);
        std::vector<uint8_t> holes = HoleMap(terrain);
        auto solid = [&](uint32_t i, uint32_t j, uint32_t k) { return holes.empty() || !(holes[i] | holes[j] | holes[k]); };
        for (int r = 0; r + 1 < n; ++r)
            for (int c = 0; c + 1 < n; ++c)
            {
                uint32_t a = r * n + c, b = a + 1, d = a + n, e = d + 1;
                // Counter-clockwise seen from above, split along (c, r) -> (c + 1, r + 1).
                if (solid(a, e, b)) indices.insert(indices.end(), { a, e, b });
                if (solid(a, d, e)) indices.insert(indices.end(), { a, d, e });
            }
        // (not FromArrays: its tangents took most of the time, and nothing reads a terrain's)
        MeshData mesh;
        mesh.Name = "Terrain";
        mesh.Vertices = std::move(vertices);
        mesh.Indices = std::move(indices);
        mesh.SubMeshes.push_back({ 0, (uint32_t)mesh.Indices.size(), -1 });
        return mesh;
    }

    std::vector<uint32_t> BuildSplat(const TerrainComponent& terrain, const MeshData& mesh)
    {
        if (std::vector<uint32_t> painted = EditedLayers(terrain, mesh.Vertices.size()); !painted.empty()) return painted;
        size_t count = mesh.Vertices.size();
        int layers = LayerCount(terrain);
        std::vector<uint32_t> splat(count * (size_t)SplatMapCount(terrain), 0u);
        bool rules = false;
        for (int k = 1; k < layers && k < (int)terrain.Layers.size(); ++k) rules |= terrain.Layers[k].Rule;
        if (rules)
        {
            // Layer rules: the first layer everywhere, then each layer with a rule over those before it, as much as the
            // ground's height and slope sit in its ranges (fading past them), broken up by its noise.
            auto within = [](float x, float lo, float hi, float fade, float bottom, float top) {
                float a = lo <= bottom ? 1.0f : Smooth(lo - fade, lo + fade, x);
                float b = hi >= top ? 1.0f : 1.0f - Smooth(hi - fade, hi + fade, x);
                return a * b;
            };
            float w[kMaxLayers];
            for (size_t i = 0; i < count; ++i)
            {
                const Vertex& v = mesh.Vertices[i];
                float height = terrain.Height > 0.0f ? v.Position.y / terrain.Height : 0.0f;
                float slope = glm::degrees(std::acos(std::clamp(v.Normal.y, -1.0f, 1.0f)));
                std::fill(w, w + kMaxLayers, 0.0f);
                w[0] = 1.0f;
                for (int k = 1; k < layers && k < (int)terrain.Layers.size(); ++k)
                {
                    const TerrainLayer& rule = terrain.Layers[k];
                    if (!rule.Rule) continue;
                    float cover = within(height, rule.HeightRange.x, rule.HeightRange.y, std::max(rule.HeightFade, 1e-4f), 0.0f, 1.0f) *
                                  within(slope, rule.SlopeRange.x, rule.SlopeRange.y, std::max(rule.SlopeFade, 1e-3f), 0.0f, 90.0f);
                    if (cover > 0.0f && rule.Noise > 0.0f)
                    {
                        float scale = std::max(rule.NoiseScale, 0.1f);
                        float n = 0.5f + 0.5f * ParticleSystems::GradientNoise({ v.Position.x / scale, (float)k * 17.31f + (float)terrain.Seed * 0.37f, v.Position.z / scale });
                        cover = std::clamp(cover * (1.0f + (n * 2.0f - 1.0f) * 2.0f * rule.Noise), 0.0f, 1.0f);
                    }
                    if (cover <= 0.0f) continue;
                    for (int j = 0; j < k; ++j) w[j] *= 1.0f - cover;
                    w[k] = cover;
                }
                for (int m = 0; m < SplatMapCount(terrain); ++m) splat[(size_t)m * count + i] = PackWeights(w + m * 4);
            }
            return splat;
        }
        float slopeAt = std::cos(glm::radians(std::clamp(terrain.RockSlope, 1.0f, 89.0f)));
        for (size_t i = 0; i < count; ++i)
        {
            // The first three layers by slope and height (with fewer, the missing ones fall to the first).
            const Vertex& v = mesh.Vertices[i];
            float up = v.Normal.y;
            float rock = layers > 1 ? Smooth(slopeAt + 0.06f, slopeAt - 0.06f, up) : 0.0f;
            float height = terrain.Height > 0.0f ? v.Position.y / terrain.Height : 0.0f;
            float snow = layers > 2 ? Smooth(terrain.SnowHeight - 0.04f, terrain.SnowHeight + 0.04f, height) * (1.0f - rock * 0.7f) : 0.0f;
            float grass = std::max(0.0f, 1.0f - rock - snow);
            float sum = std::max(grass + rock + snow, 1e-4f);
            float w[4] = { grass / sum, rock / sum, snow / sum, 0.0f };
            splat[i] = PackWeights(w);
        }
        return splat;
    }

    namespace
    {
        uint64_t Fnv(const void* p, size_t n)
        {
            uint64_t h = 1469598103934665603ull;
            const auto* b = static_cast<const uint8_t*>(p);
            for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 1099511628211ull;
            return h;
        }

        // The hash of a sculpted or painted data string. They run to megabytes and the renderer asks for the key every
        // frame, so a big one's hash is remembered with a copy of what was hashed: comparing with the copy is many
        // times faster than hashing again, and stays right however the string was changed (an undo can overwrite it in
        // place, keeping its buffer and size).
        uint64_t BlobHash(const std::string& s)
        {
            if (s.size() < 65536) return Fnv(s.data(), s.size());
            struct Memo
            {
                std::string Data;
                uint64_t Hash = 0;
            };
            static std::mutex mutex;
            static std::unordered_map<const std::string*, Memo> memo; // by the string's address (the component's field)
            std::lock_guard lock(mutex);
            if (auto it = memo.find(&s); it != memo.end() && it->second.Data == s) return it->second.Hash;
            if (memo.size() >= 24) memo.clear(); // fields of components since moved or destroyed
            uint64_t h = Fnv(s.data(), s.size());
            memo[&s] = { s, h };
            return h;
        }
    }

    uint64_t SettingsKey(const TerrainComponent& t)
    {
        uint64_t h = BaseKey(t);
        const auto* b = reinterpret_cast<const uint8_t*>(&t.EditRevision);
        for (size_t i = 0; i < sizeof(t.EditRevision); ++i) h = (h ^ b[i]) * 1099511628211ull;
        return h;
    }

    bool ChangedSince(const TerrainComponent& terrain, uint64_t revision, glm::ivec4& samples)
    {
        samples = glm::ivec4(INT_MAX, INT_MAX, INT_MIN, INT_MIN);
        if (revision > terrain.EditRevision) return false;
        uint64_t next = revision + 1;
        for (const TerrainComponent::DirtyRect& d : terrain.EditLog)
        {
            if (d.Revision < next) continue;
            if (d.Revision != next) return false; // an edit in between that was not logged
            samples = glm::ivec4(glm::min(glm::ivec2(samples), glm::ivec2(d.Samples)), glm::max(glm::ivec2(samples.z, samples.w), glm::ivec2(d.Samples.z, d.Samples.w)));
            ++next;
        }
        return next == terrain.EditRevision + 1;
    }

    uint64_t BaseKey(const TerrainComponent& t)
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
        int layers = LayerCount(t);
        mix(&layers, sizeof(layers));
        for (const TerrainLayer& l : t.Layers)
            if (l.Rule)
            {
                float rule[8] = { l.HeightRange.x, l.HeightRange.y, l.SlopeRange.x, l.SlopeRange.y, l.HeightFade, l.SlopeFade, l.Noise, l.NoiseScale };
                mix(rule, sizeof(rule));
            }
            else
                mix("-", 1);
        auto mixString = [&](const std::string& s) {
            size_t size = s.size();
            mix(&size, sizeof(size));
            uint64_t blob = BlobHash(s);
            mix(&blob, sizeof(blob));
        };
        mixString(t.SculptedHeights);
        mixString(t.PaintedLayers);
        mixString(t.PaintedHoles);
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

    // The samples a level keeps between a chunk's first and last (every `stride`-th, and the last).
    static void LevelLines(int first, int last, int stride, std::vector<int>& out)
    {
        out.clear();
        for (int i = first; i < last; i += stride) out.push_back(i);
        out.push_back(last);
    }

    namespace
    {
        // A chunk's samples: columns c0-c1 and rows r0-r1 (inclusive; chunk (cx, cz) of `across` a side).
        struct ChunkSpan
        {
            int C0, C1, R0, R1;
        };
        ChunkSpan SpanOf(int cx, int cz, int n)
        {
            int quads = n - 1, per = kChunkQuads;
            return { cx * per, std::min(cx * per + per, quads), cz * per, std::min(cz * per + per, quads) };
        }

        // Its lowest and highest sample, and how far each level strays from the full surface: the most any sample is
        // off the level's triangles there.
        void MeasureChunk(const std::vector<float>& heights, int n, const ChunkSpan& s, Chunk& chunk, float& low, float& high)
        {
            auto h = [&](int c, int r) { return heights[(size_t)r * n + c]; };
            low = high = h(s.C0, s.R0);
            for (int r = s.R0; r <= s.R1; ++r)
                for (int c = s.C0; c <= s.C1; ++c)
                {
                    low = std::min(low, h(c, r));
                    high = std::max(high, h(c, r));
                }
            std::vector<int> cols, rows;
            chunk.Error[0] = 0.0f;
            for (int level = 1; level < chunk.Levels; ++level)
            {
                LevelLines(s.C0, s.C1, 1 << level, cols);
                LevelLines(s.R0, s.R1, 1 << level, rows);
                float error = chunk.Error[level - 1];
                for (size_t j = 0; j + 1 < rows.size(); ++j)
                    for (size_t i = 0; i + 1 < cols.size(); ++i)
                    {
                        int ca = cols[i], cb = cols[i + 1], ra = rows[j], rb = rows[j + 1];
                        float a = h(ca, ra), b = h(cb, ra), d = h(ca, rb), e = h(cb, rb);
                        for (int r = ra; r <= rb; ++r)
                            for (int c = ca; c <= cb; ++c)
                            {
                                float tx = (c - ca) / (float)(cb - ca), tz = (r - ra) / (float)(rb - ra);
                                // the same split as the full surface: (ca, ra) -> (cb, rb)
                                float at = tx >= tz ? a + (b - a) * tx + (e - b) * tz : a + (d - a) * tz + (e - d) * tx;
                                error = std::max(error, std::abs(h(c, r) - at));
                            }
                    }
                chunk.Error[level] = error;
            }
            for (int level = chunk.Levels; level < kChunkLevels; ++level) chunk.Error[level] = chunk.Error[chunk.Levels - 1];
        }

        // Skirts on the edges a chunk shares: two neighbours stray from the full surface by at most their errors, so
        // the gap between them is at most the sum; a skirt that deep on each side covers it whichever is higher.
        float SkirtDepth(const std::vector<Chunk>& chunks, int across, int cx, int cz, const glm::vec2& step)
        {
            auto worst = [&](int x, int z) {
                if (x < 0 || z < 0 || x >= across || z >= across) return 0.0f;
                return chunks[(size_t)z * across + x].Error[kChunkLevels - 1];
            };
            float depth = chunks[(size_t)cz * across + cx].Error[kChunkLevels - 1] +
                          std::max({ worst(cx - 1, cz), worst(cx + 1, cz), worst(cx, cz - 1), worst(cx, cz + 1) });
            return depth > 0.0f ? depth + 0.05f * std::max(step.x, step.y) : 0.0f;
        }

        // An edge of a chunk (0 west, 1 east, 2 south, 3 north): whether a neighbour shares it, and its samples.
        struct ChunkEdge
        {
            bool Shared;
            bool AlongX; // the edge runs along X (a row of samples) or along Z (a column)
            int Fixed;   // that row or column
            int From, To;
        };
        ChunkEdge EdgeOf(const ChunkSpan& s, int cx, int cz, int across, int e)
        {
            switch (e)
            {
            case 0: return { cx > 0, false, s.C0, s.R0, s.R1 };
            case 1: return { cx + 1 < across, false, s.C1, s.R0, s.R1 };
            case 2: return { cz > 0, true, s.R0, s.C0, s.C1 };
            default: return { cz + 1 < across, true, s.R1, s.C0, s.C1 };
            }
        }
        uint32_t EdgeSample(const ChunkEdge& edge, int n, int k) { return edge.AlongX ? (uint32_t)(edge.Fixed * n + k) : (uint32_t)(k * n + edge.Fixed); }

        std::mutex s_UploadMutex;
        std::vector<std::function<void(VkCommandBuffer)>> s_Uploads;
        void QueueUpload(std::function<void(VkCommandBuffer)> upload)
        {
            std::lock_guard lock(s_UploadMutex);
            s_Uploads.push_back(std::move(upload));
        }
    }

    void FlushUploads(VkCommandBuffer cmd)
    {
        std::vector<std::function<void(VkCommandBuffer)>> uploads;
        {
            std::lock_guard lock(s_UploadMutex);
            uploads.swap(s_Uploads);
        }
        for (auto& upload : uploads) upload(cmd);
    }

    void BuildChunks(const TerrainComponent& terrain, const std::vector<float>& heights, std::vector<Vertex>& vertices,
                     std::vector<uint32_t>& indices, std::vector<Chunk>& chunks)
    {
        std::vector<uint8_t> holes = HoleMap(terrain);
        int n = SampleCount(terrain);
        int quads = n - 1, per = kChunkQuads, across = (quads + per - 1) / per;
        auto solid = [&](uint32_t i, uint32_t j, uint32_t k) { return holes.empty() || !(holes[i] | holes[j] | holes[k]); };
        chunks.assign((size_t)across * across, Chunk{});
        std::vector<float> low(chunks.size()), high(chunks.size());
        std::vector<int> cols, rows;
        for (int cz = 0; cz < across; ++cz)
            for (int cx = 0; cx < across; ++cx)
            {
                size_t index = (size_t)cz * across + cx;
                ChunkSpan s = SpanOf(cx, cz, n);
                bool hole = false;
                for (int r = s.R0; r <= s.R1 && !holes.empty(); ++r)
                    for (int c = s.C0; c <= s.C1; ++c) hole |= holes[(size_t)r * n + c] != 0;
                chunks[index].Levels = hole ? 1 : kChunkLevels;
                MeasureChunk(heights, n, s, chunks[index], low[index], high[index]);
            }
        glm::vec2 size = glm::max(terrain.Size, glm::vec2(0.01f));
        glm::vec2 step = size / (float)(n - 1);
        for (int cz = 0; cz < across; ++cz)
            for (int cx = 0; cx < across; ++cx)
            {
                size_t index = (size_t)cz * across + cx;
                Chunk& chunk = chunks[index];
                ChunkSpan s = SpanOf(cx, cz, n);
                int c0 = s.C0, c1 = s.C1, r0 = s.R0, r1 = s.R1;
                float depth = SkirtDepth(chunks, across, cx, cz, step);
                chunk.Depth = depth;
                // The edges (west, east, south, north) and, for each shared one, the first of its skirt's bottom vertices.
                struct Edge
                {
                    bool Shared;
                    bool AlongX;
                    int Fixed;
                    uint32_t Bottom = 0;
                };
                Edge edges[4];
                for (int e = 0; e < 4; ++e)
                {
                    ChunkEdge ce = EdgeOf(s, cx, cz, across, e);
                    edges[e] = { ce.Shared, ce.AlongX, ce.Fixed };
                }
                for (int e = 0; e < 4; ++e)
                {
                    Edge& edge = edges[e];
                    if (!edge.Shared || depth <= 0.0f) continue;
                    edge.Bottom = (uint32_t)vertices.size();
                    chunk.Skirt[e] = edge.Bottom;
                    int from = edge.AlongX ? c0 : r0, to = edge.AlongX ? c1 : r1;
                    for (int i = from; i <= to; ++i)
                    {
                        Vertex v = edge.AlongX ? vertices[(size_t)edge.Fixed * n + i] : vertices[(size_t)i * n + edge.Fixed];
                        v.Position.y -= depth;
                        vertices.push_back(v);
                    }
                }
                chunk.Min = { -size.x * 0.5f + c0 * step.x, low[index] - depth, -size.y * 0.5f + r0 * step.y };
                chunk.Max = { -size.x * 0.5f + c1 * step.x, high[index], -size.y * 0.5f + r1 * step.y };
                for (int level = 0; level < kChunkLevels; ++level)
                {
                    if (level >= chunk.Levels)
                    {
                        chunk.First[level] = chunk.First[chunk.Levels - 1];
                        chunk.Count[level] = chunk.Count[chunk.Levels - 1];
                        continue;
                    }
                    chunk.First[level] = (uint32_t)indices.size();
                    LevelLines(c0, c1, 1 << level, cols);
                    LevelLines(r0, r1, 1 << level, rows);
                    for (size_t j = 0; j + 1 < rows.size(); ++j)
                        for (size_t i = 0; i + 1 < cols.size(); ++i)
                        {
                            uint32_t a = rows[j] * n + cols[i], b = rows[j] * n + cols[i + 1];
                            uint32_t d = rows[j + 1] * n + cols[i], e = rows[j + 1] * n + cols[i + 1];
                            // Counter-clockwise seen from above, split along (c, r) -> (c + 1, r + 1) as BuildMesh.
                            if (solid(a, e, b)) indices.insert(indices.end(), { a, e, b });
                            if (solid(a, d, e)) indices.insert(indices.end(), { a, d, e });
                        }
                    for (const Edge& edge : edges)
                    {
                        if (!edge.Shared || depth <= 0.0f) continue;
                        const std::vector<int>& along = edge.AlongX ? cols : rows;
                        int from = edge.AlongX ? c0 : r0;
                        auto top = [&](int k) { return edge.AlongX ? (uint32_t)(edge.Fixed * n + k) : (uint32_t)(k * n + edge.Fixed); };
                        for (size_t i = 0; i + 1 < along.size(); ++i)
                        {
                            uint32_t tp = top(along[i]), tq = top(along[i + 1]);
                            uint32_t bp = edge.Bottom + (uint32_t)(along[i] - from), bq = edge.Bottom + (uint32_t)(along[i + 1] - from);
                            // both faces: it is seen from whichever neighbour sits lower
                            indices.insert(indices.end(), { tp, bp, bq, tp, bq, tq, tp, bq, bp, tp, tq, bq });
                        }
                    }
                    chunk.Count[level] = (uint32_t)indices.size() - chunk.First[level];
                }
            }
    }

    const Runtime* GetRuntime(const TerrainComponent& terrain) { return GetRuntime(terrain, SettingsKey(terrain)); }

    namespace
    {
        // Brings a runtime up to the terrain's edits that touched only `samples` (and its edges' neighbours): the
        // vertices there, the chunks over them (errors, bounds, skirts) and the splat maps. False when it cannot (the
        // whole is built again).
        bool UpdateRuntime(const TerrainComponent& terrain, Runtime& rt, glm::ivec4 samples)
        {
            int n = SampleCount(terrain);
            size_t count = (size_t)n * n;
            int maps = SplatMapCount(terrain);
            const std::vector<float>& heights = terrain.EditHeights;
            if (heights.size() != count || terrain.EditLayers.size() != count * (size_t)maps || (int)rt.Splats.size() != maps) return false;
            if (samples.x > samples.z || samples.y > samples.w) return true; // nothing changed
            int quads = n - 1, per = kChunkQuads, across = (quads + per - 1) / per;
            if ((int)rt.Chunks.size() != across * across) return false;
            glm::vec2 size = glm::max(terrain.Size, glm::vec2(0.01f));
            glm::vec2 step = size / (float)(n - 1);
            // Normals read a sample's neighbours: the ring around the changed samples moves too.
            int c0 = std::max(samples.x - 1, 0), r0 = std::max(samples.y - 1, 0), c1 = std::min(samples.z + 1, n - 1), r1 = std::min(samples.w + 1, n - 1);
            Ref<Mesh> mesh = rt.Surface;
            for (int r = r0; r <= r1; ++r)
            {
                std::vector<Vertex> row;
                row.reserve((size_t)(c1 - c0 + 1));
                for (int c = c0; c <= c1; ++c) row.push_back(SurfaceVertex(heights, n, size, step, c, r));
                uint32_t first = (uint32_t)(r * n + c0);
                QueueUpload([mesh, first, row = std::move(row)](VkCommandBuffer cmd) { mesh->UpdateVertices(cmd, first, row.data(), (uint32_t)row.size()); });
            }
            // The chunks over them: measured again; their skirts and their neighbours' (which hang by their errors).
            int x0 = std::max(c0 - 1, 0) / per, x1 = std::min(c1 / per, across - 1), z0 = std::max(r0 - 1, 0) / per, z1 = std::min(r1 / per, across - 1);
            std::vector<float> lows(rt.Chunks.size()), highs(rt.Chunks.size());
            for (size_t i = 0; i < rt.Chunks.size(); ++i)
            {
                lows[i] = rt.Chunks[i].Min.y + rt.Chunks[i].Depth;
                highs[i] = rt.Chunks[i].Max.y;
            }
            for (int cz = z0; cz <= z1; ++cz)
                for (int cx = x0; cx <= x1; ++cx)
                {
                    size_t i = (size_t)cz * across + cx;
                    MeasureChunk(heights, n, SpanOf(cx, cz, n), rt.Chunks[i], lows[i], highs[i]);
                }
            for (int cz = std::max(z0 - 1, 0); cz <= std::min(z1 + 1, across - 1); ++cz)
                for (int cx = std::max(x0 - 1, 0); cx <= std::min(x1 + 1, across - 1); ++cx)
                {
                    size_t i = (size_t)cz * across + cx;
                    Chunk& chunk = rt.Chunks[i];
                    ChunkSpan s = SpanOf(cx, cz, n);
                    float depth = SkirtDepth(rt.Chunks, across, cx, cz, step);
                    bool touched = cx >= x0 && cx <= x1 && cz >= z0 && cz <= z1;
                    if (!touched && depth == chunk.Depth) continue;
                    for (int e = 0; e < 4; ++e)
                    {
                        ChunkEdge edge = EdgeOf(s, cx, cz, across, e);
                        if (!edge.Shared) continue;
                        if (chunk.Skirt[e] == 0) // built without a skirt (flat): it has nothing to hang now
                        {
                            if (depth > 0.0f) return false;
                            continue;
                        }
                        std::vector<Vertex> skirt;
                        for (int k = edge.From; k <= edge.To; ++k)
                        {
                            uint32_t top = EdgeSample(edge, n, k);
                            Vertex v = SurfaceVertex(heights, n, size, step, (int)(top % n), (int)(top / n));
                            v.Position.y -= depth;
                            skirt.push_back(v);
                        }
                        uint32_t first = chunk.Skirt[e];
                        QueueUpload([mesh, first, skirt = std::move(skirt)](VkCommandBuffer cmd) { mesh->UpdateVertices(cmd, first, skirt.data(), (uint32_t)skirt.size()); });
                    }
                    chunk.Depth = depth;
                    chunk.Min.y = lows[i] - depth;
                    chunk.Max.y = highs[i];
                }
            // The splat maps over the changed samples (painted while digging).
            uint32_t w = (uint32_t)(samples.z - samples.x + 1), hgt = (uint32_t)(samples.w - samples.y + 1);
            for (int m = 0; m < maps; ++m)
            {
                std::vector<uint32_t> texels((size_t)w * hgt);
                for (uint32_t y = 0; y < hgt; ++y)
                    for (uint32_t x = 0; x < w; ++x)
                        texels[(size_t)y * w + x] = terrain.EditLayers[(size_t)m * count + (size_t)(samples.y + y) * n + (samples.x + x)];
                Ref<Texture2D> splat = rt.Splats[m];
                uint32_t sx = (uint32_t)samples.x, sy = (uint32_t)samples.y;
                QueueUpload([splat, sx, sy, w, hgt, texels = std::move(texels)](VkCommandBuffer cmd) { splat->UpdateRegion(cmd, sx, sy, w, hgt, texels.data()); });
            }
            return true;
        }

        // Which runtime each terrain drew last (by component), to bring up to date in place.
        std::unordered_map<const TerrainComponent*, uint64_t>& LastRuntime()
        {
            static std::unordered_map<const TerrainComponent*, uint64_t> last;
            return last;
        }
    }

    const Runtime* GetRuntime(const TerrainComponent& terrain, uint64_t key)
    {
        auto& runtimes = Runtimes();
        if (auto it = runtimes.find(key); it != runtimes.end()) return &it->second;
        // The one this terrain drew last, if only craters (logged edits) came since: moved to the new key, updated there.
        uint64_t base = BaseKey(terrain);
        auto& last = LastRuntime();
        if (auto l = last.find(&terrain); l != last.end())
            if (auto old = runtimes.find(l->second); old != runtimes.end() && old->second.BaseKey == base)
            {
                glm::ivec4 samples;
                if (ChangedSince(terrain, old->second.Revision, samples))
                {
                    Runtime rt = std::move(old->second);
                    runtimes.erase(old);
                    if (UpdateRuntime(terrain, rt, samples))
                    {
                        rt.Key = key;
                        rt.Revision = terrain.EditRevision;
                        l->second = key;
                        return &(runtimes[key] = std::move(rt));
                    }
                }
            }
        if (runtimes.size() > 16)
        {
            runtimes.clear(); // edited settings leave old builds behind; drop them now and then
            last.clear();
        }
        std::vector<float> heights = BuildHeights(terrain);
        MeshData data = BuildMesh(terrain, heights);
        std::vector<uint32_t> splat = BuildSplat(terrain, data); // (one per grid sample: before the skirts are added)
        int n = SampleCount(terrain);
        Runtime rt;
        rt.Key = key;
        rt.BaseKey = base;
        rt.Revision = terrain.EditRevision;
        last[&terrain] = key;
        std::vector<uint32_t> indices;
        BuildChunks(terrain, heights, data.Vertices, indices, rt.Chunks);
        if (indices.empty()) indices = { 0, 0, 0 }; // all holes: one degenerate triangle keeps the mesh valid
        rt.Surface = MakeRef<Mesh>(std::move(data.Vertices), std::move(indices), "Terrain");
        TextureSettings settings;
        settings.SRGB = false;
        settings.GenerateMips = true;
        for (int m = 0; m < SplatMapCount(terrain); ++m)
            rt.Splats.push_back(MakeRef<Texture2D>(splat.data() + (size_t)m * n * n, (uint32_t)n, (uint32_t)n, settings, "TerrainSplat"));
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
        if (terrain.EditLayers.size() != (size_t)n * n * SplatMapCount(terrain))
        {
            MeshData mesh = BuildMesh(terrain, terrain.EditHeights);
            terrain.EditLayers = BuildSplat(terrain, mesh);
        }
        if (terrain.EditHoles.size() != (size_t)n * n)
        {
            terrain.EditHoles = HoleMap(terrain);
            terrain.EditHoles.resize((size_t)n * n, 0);
        }
    }

    float StampValue(const StampSettings& stamp, const glm::vec2& offset, float radius)
    {
        const HeightImage* image = stamp.Image.IsValid() ? LoadHeightmap(stamp.Image) : nullptr;
        if (!image || radius <= 0.0f) return -1.0f;
        // Into the stamp's turned square: u to the right, v toward +Z (the image's top).
        float a = glm::radians(stamp.Rotation), c = std::cos(a), s = std::sin(a);
        glm::vec2 local(c * offset.x + s * offset.y, -s * offset.x + c * offset.y);
        glm::vec2 uv = local / (2.0f * radius) + 0.5f;
        if (uv.x < 0.0f || uv.y < 0.0f || uv.x > 1.0f || uv.y > 1.0f) return -1.0f;
        return SampleImage(*image, uv.x, 1.0f - uv.y);
    }

    bool ApplyBrush(TerrainComponent& terrain, BrushMode mode, const glm::vec2& at, float radius, float strength, float dt, float flattenHeight,
                    const StampSettings* stamp, int paintLayer)
    {
        if (mode == BrushMode::Erode)
        {
            // Rain on the brush: drops in proportion to its area, strength and time.
            BeginEdit(terrain);
            glm::vec2 size = glm::max(terrain.Size, glm::vec2(0.01f));
            if (std::abs(at.x) > size.x * 0.5f + radius || std::abs(at.y) > size.y * 0.5f + radius) return false;
            glm::vec2 cell = size / (float)(SampleCount(terrain) - 1);
            float area = glm::pi<float>() * radius * radius / std::max(cell.x * cell.y, 1e-6f);
            int droplets = std::max(1, (int)(area * std::clamp(strength, 0.0f, 1.0f) * dt * 4.0f));
            Erode(terrain, at, radius, droplets, (uint32_t)terrain.EditRevision * 7919u + 13u);
            return true;
        }
        if (mode == BrushMode::Stamp && (!stamp || StampValue(*stamp, glm::vec2(0.0f), std::max(radius, 1e-3f)) < 0.0f)) return false;
        BeginEdit(terrain);
        int n = SampleCount(terrain);
        glm::vec2 size = glm::max(terrain.Size, glm::vec2(0.01f));
        glm::vec2 cell = size / (float)(n - 1);
        radius = std::max(radius, std::min(cell.x, cell.y));
        float reach = mode == BrushMode::Stamp ? radius * 1.4143f : radius; // the stamp's square reaches its corners
        if (std::abs(at.x) > size.x * 0.5f + reach || std::abs(at.y) > size.y * 0.5f + reach) return false;
        int c0 = std::max(0, (int)std::floor((at.x - reach) / cell.x + (n - 1) * 0.5f)), c1 = std::min(n - 1, (int)std::ceil((at.x + reach) / cell.x + (n - 1) * 0.5f));
        int r0 = std::max(0, (int)std::floor((at.y - reach) / cell.y + (n - 1) * 0.5f)), r1 = std::min(n - 1, (int)std::ceil((at.y + reach) / cell.y + (n - 1) * 0.5f));
        std::vector<float> before = mode == BrushMode::Smooth ? terrain.EditHeights : std::vector<float>();
        float rate = std::clamp(strength, 0.0f, 1.0f) * dt;
        for (int r = r0; r <= r1; ++r)
            for (int c = c0; c <= c1; ++c)
            {
                glm::vec2 p(-size.x * 0.5f + c * cell.x, -size.y * 0.5f + r * cell.y);
                size_t i = (size_t)r * n + c;
                float& h = terrain.EditHeights[i];
                if (mode == BrushMode::Stamp)
                {
                    // Toward the image's shape over the base height, one way only.
                    float v = StampValue(*stamp, p - at, radius);
                    if (v <= 0.0f) continue;
                    float target = flattenHeight + (stamp->Carve ? -v : v) * stamp->Height;
                    if (stamp->Carve ? target < h : target > h) h += (target - h) * std::min(rate * 8.0f, 1.0f);
                    h = std::clamp(h, 0.0f, terrain.Height);
                    continue;
                }
                float d = glm::length(p - at) / radius;
                if (d >= 1.0f) continue;
                float falloff = 1.0f - Smooth(0.0f, 1.0f, d); // soft edge
                if (IsHoleMode(mode))
                {
                    terrain.EditHoles[i] = mode == BrushMode::DigHole ? 1 : 0; // holes are all or nothing
                    continue;
                }
                if (IsDetailMode(mode))
                {
                    // Detail density moves toward full (or, erasing, toward none for both kinds).
                    float amount = std::min(rate * 4.0f * falloff, 1.0f);
                    for (int k = 0; k < 2; ++k)
                    {
                        bool target = (mode == BrushMode::PlaceTrees && k == 0) || (mode == BrushMode::PlaceGrass && k == 1);
                        if (mode != BrushMode::EraseDetails && !target) continue;
                        uint8_t& density = terrain.EditDetails[i * 2 + k];
                        float v = density / 255.0f;
                        v += ((target ? 1.0f : 0.0f) - v) * amount;
                        int next = std::clamp((int)std::lround(v * 255.0f), 0, 255);
                        if (next == density && amount > 0.0f) next = std::clamp(density + (target ? 1 : -1), 0, 255); // rounding would stall it
                        density = (uint8_t)next;
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
                    // Painting: move this sample's weights toward the layer.
                    PaintSample(terrain, i, paintLayer, std::min(rate * 4.0f * falloff, 1.0f));
                    break;
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
        size_t count = (size_t)n * n;
        if (terrain.EditLayers.size() == count * (size_t)SplatMapCount(terrain))
        {
            // A byte per layer per sample, up to the last layer painted anywhere (at least three).
            int layers = LayerCount(terrain), stride = std::min(3, layers);
            for (int k = layers - 1; k >= 3 && stride == std::min(3, layers); --k)
                for (size_t i = 0; i < count; ++i)
                    if (terrain.EditLayers[(size_t)(k / 4) * count + i] >> (k % 4 * 8) & 255)
                    {
                        stride = k + 1;
                        break;
                    }
            std::vector<uint8_t> bytes(count * (size_t)stride);
            for (size_t i = 0; i < count; ++i)
                for (int k = 0; k < stride; ++k) bytes[i * stride + k] = (uint8_t)(terrain.EditLayers[(size_t)(k / 4) * count + i] >> (k % 4 * 8) & 255);
            terrain.PaintedLayers = Encode(bytes.data(), bytes.size());
        }
        if (terrain.EditHoles.size() == count)
        {
            std::vector<uint8_t> bits((count + 7) / 8, 0);
            bool any = false;
            for (size_t i = 0; i < count; ++i)
                if (terrain.EditHoles[i])
                {
                    bits[i / 8] |= (uint8_t)(1u << (i % 8));
                    any = true;
                }
            terrain.PaintedHoles = any ? Encode(bits.data(), bits.size()) : std::string();
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
        terrain.EditHoles.clear();
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

    namespace
    {
        // The meters a rectangle of samples covers (lo inclusive, hi exclusive): the details placed there.
        void SampleArea(const TerrainComponent& terrain, const glm::ivec4& samples, glm::vec2& lo, glm::vec2& hi)
        {
            int n = SampleCount(terrain);
            glm::vec2 size = glm::max(terrain.Size, glm::vec2(0.01f));
            glm::vec2 step = size / (float)(n - 1);
            lo = -size * 0.5f + glm::vec2(samples.x, samples.y) * step;
            hi = -size * 0.5f + glm::vec2(samples.z, samples.w) * step;
        }
    }

    // The details, all of them or (with `only`) just those placed over that rectangle of samples.
    static DetailInstances BuildDetailsIn(const TerrainComponent& terrain, const std::vector<float>& heights, const glm::ivec4* only);

    DetailInstances BuildDetails(const TerrainComponent& terrain, const std::vector<float>& heights) { return BuildDetailsIn(terrain, heights, nullptr); }

    static DetailInstances BuildDetailsIn(const TerrainComponent& terrain, const std::vector<float>& heights, const glm::ivec4* only)
    {
        DetailInstances out;
        std::vector<uint8_t> map = DetailMap(terrain);
        // Layers that grow details by themselves: per sample, the layers' weights times their Trees and Grass, kept
        // where more than what was painted.
        bool grows = false;
        for (int k = 0; k < LayerCount(terrain) && k < (int)terrain.Layers.size(); ++k) grows |= terrain.Layers[k].Trees > 0.0f || terrain.Layers[k].Grass > 0.0f;
        if (map.empty() && !grows) return out;
        int n = SampleCount(terrain);
        if (map.empty()) map.assign((size_t)n * n * 2, 0);
        if (grows && heights.size() == (size_t)n * n)
        {
            size_t count = (size_t)n * n;
            // The layers' weights: painted (or being edited) as they are, else laid out by their rules over the mesh.
            std::vector<uint32_t> splat = EditedLayers(terrain, count);
            if (splat.empty()) splat = BuildSplat(terrain, BuildMesh(terrain, heights));
            // (with `only`, the samples it reads: the rectangle and one more all round)
            int c0 = 0, c1 = n - 1, r0 = 0, r1 = n - 1;
            if (only) c0 = std::max(only->x - 1, 0), c1 = std::min(only->z + 1, n - 1), r0 = std::max(only->y - 1, 0), r1 = std::min(only->w + 1, n - 1);
            if (splat.size() == count * (size_t)SplatMapCount(terrain))
                for (int row = r0; row <= r1; ++row)
                    for (int col = c0; col <= c1; ++col)
                {
                    size_t i = (size_t)row * n + col;
                    float trees = 0.0f, grass = 0.0f;
                    for (int k = 0; k < LayerCount(terrain) && k < (int)terrain.Layers.size(); ++k)
                    {
                        float w = (splat[(size_t)(k / 4) * count + i] >> (k % 4 * 8) & 255) / 255.0f;
                        trees += w * terrain.Layers[k].Trees;
                        grass += w * terrain.Layers[k].Grass;
                    }
                    map[i * 2] = std::max(map[i * 2], (uint8_t)std::clamp((int)std::lround(trees * 255.0f), 0, 255));
                    map[i * 2 + 1] = std::max(map[i * 2 + 1], (uint8_t)std::clamp((int)std::lround(grass * 255.0f), 0, 255));
                }
        }
        std::vector<uint8_t> holes = HoleMap(terrain);
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
            // With `only`: the cells that can place one over the rectangle (each lands somewhere in its own cell).
            int i0 = 0, i1 = cx - 1, j0 = 0, j1 = cz - 1;
            glm::vec2 lo(0.0f), hi(0.0f);
            if (only)
            {
                SampleArea(terrain, *only, lo, hi);
                i0 = std::max(0, (int)std::floor((lo.x + size.x * 0.5f) / cell)), i1 = std::min(cx - 1, (int)std::floor((hi.x + size.x * 0.5f) / cell));
                j0 = std::max(0, (int)std::floor((lo.y + size.y * 0.5f) / cell)), j1 = std::min(cz - 1, (int)std::floor((hi.y + size.y * 0.5f) / cell));
            }
            for (int j = j0; j <= j1; ++j)
                for (int i = i0; i <= i1; ++i)
                {
                    uint32_t state = Hash((uint32_t)i * 73856093u ^ (uint32_t)j * 19349663u ^ (uint32_t)(kind + 1) * 83492791u ^ (uint32_t)terrain.Seed * 2654435761u);
                    float x = -size.x * 0.5f + (i + Random01(state)) * cell, z = -size.y * 0.5f + (j + Random01(state)) * cell;
                    if (only && (x < lo.x || x >= hi.x || z < lo.y || z >= hi.y)) continue;
                    float chance = Random01(state);
                    if (std::abs(x) > size.x * 0.5f || std::abs(z) > size.y * 0.5f || chance >= densityAt(x, z, kind)) continue;
                    if (upAt(x, z) < minUp || IsHoleAt(terrain, holes, x, z)) continue;
                    float yaw = Random01(state) * glm::two_pi<float>();
                    float s = glm::mix(scale.x, scale.y, Random01(state));
                    glm::mat4 m = glm::rotate(glm::translate(glm::mat4(1.0f), glm::vec3(x, HeightAt(terrain, heights, x, z), z)), yaw, glm::vec3(0, 1, 0));
                    list.push_back(glm::scale(m, glm::vec3(std::max(s, 0.01f))));
                }
        }
        return out;
    }

    const DetailInstances& GetDetails(const TerrainComponent& terrain) { return GetDetails(terrain, SettingsKey(terrain)); }

    const DetailInstances& GetDetails(const TerrainComponent& terrain, uint64_t key)
    {
        // The detail settings on top of the terrain's (whose edit counter covers its edited holes and details too).
        auto withDetails = [&](uint64_t h) {
            auto mix = [&](const void* p, size_t size) {
                const auto* b = static_cast<const uint8_t*>(p);
                for (size_t i = 0; i < size; ++i) h = (h ^ b[i]) * 1099511628211ull;
            };
            for (const float* f : { &terrain.TreeDensity, &terrain.TreeScale.x, &terrain.TreeScale.y, &terrain.TreeMaxSlope, &terrain.GrassDensity,
                                    &terrain.GrassScale.x, &terrain.GrassScale.y, &terrain.GrassMaxSlope })
                mix(f, sizeof(float));
            uint64_t painted = BlobHash(terrain.PaintedDetails);
            mix(&painted, sizeof(painted));
            for (const TerrainLayer& l : terrain.Layers)
            {
                mix(&l.Trees, sizeof(float));
                mix(&l.Grass, sizeof(float));
            }
            return h;
        };
        key = withDetails(key);
        struct Entry
        {
            DetailInstances Details;
            uint64_t Base = 0, Revision = 0;
        };
        static std::mutex mutex;
        static std::unordered_map<uint64_t, Entry> cache;
        static std::unordered_map<const TerrainComponent*, uint64_t> last; // the entry each terrain used last
        std::lock_guard lock(mutex);
        if (auto it = cache.find(key); it != cache.end()) return it->second.Details;
        int n = SampleCount(terrain);
        bool edited = terrain.EditHeights.size() == (size_t)n * n;
        uint64_t base = withDetails(BaseKey(terrain));
        // Since this terrain's last details only craters (logged edits) came: the details over them placed again.
        if (auto l = last.find(&terrain); l != last.end() && edited)
            if (auto old = cache.find(l->second); old != cache.end() && old->second.Base == base)
            {
                glm::ivec4 samples;
                if (ChangedSince(terrain, old->second.Revision, samples))
                {
                    Entry entry = std::move(old->second);
                    cache.erase(old);
                    if (samples.x <= samples.z && samples.y <= samples.w)
                    {
                        // Two samples more all round: a detail's height, slope and density read the samples about it.
                        samples = glm::ivec4(glm::max(glm::ivec2(samples) - 2, glm::ivec2(0)), glm::min(glm::ivec2(samples.z, samples.w) + 2, glm::ivec2(n - 1)));
                        glm::vec2 lo, hi;
                        SampleArea(terrain, samples, lo, hi);
                        auto inside = [&](const glm::mat4& m) { return m[3].x >= lo.x && m[3].x < hi.x && m[3].z >= lo.y && m[3].z < hi.y; };
                        for (auto* list : { &entry.Details.Trees, &entry.Details.Grass })
                            list->erase(std::remove_if(list->begin(), list->end(), inside), list->end());
                        DetailInstances added = BuildDetailsIn(terrain, terrain.EditHeights, &samples);
                        entry.Details.Trees.insert(entry.Details.Trees.end(), added.Trees.begin(), added.Trees.end());
                        entry.Details.Grass.insert(entry.Details.Grass.end(), added.Grass.begin(), added.Grass.end());
                    }
                    entry.Revision = terrain.EditRevision;
                    l->second = key;
                    return (cache[key] = std::move(entry)).Details;
                }
            }
        if (cache.size() > 16)
        {
            cache.clear();
            last.clear();
        }
        Entry entry;
        entry.Base = base;
        entry.Revision = terrain.EditRevision;
        entry.Details = BuildDetails(terrain, edited ? terrain.EditHeights : BuildHeights(terrain));
        last[&terrain] = key;
        return (cache[key] = std::move(entry)).Details;
    }

    namespace
    {
        // Calls fn with the terrain's heights without copying them: the edited heights, or the generated grid from a
        // cache (held locked while fn runs, so fn must not call back into it).
        template<typename Fn>
        void WithHeights(const TerrainComponent& terrain, Fn&& fn)
        {
            int n = SampleCount(terrain);
            if (terrain.EditHeights.size() == (size_t)n * n) { fn(terrain.EditHeights); return; }
            uint64_t key = SettingsKey(terrain);
            static std::mutex mutex;
            static std::unordered_map<uint64_t, std::vector<float>> cache;
            std::lock_guard lock(mutex);
            auto it = cache.find(key);
            if (it == cache.end())
            {
                if (cache.size() > 8) cache.clear();
                it = cache.emplace(key, BuildHeights(terrain)).first;
            }
            fn(it->second);
        }
    }

    bool Sample(const TerrainComponent& terrain, const glm::mat4& world, const glm::vec3& point, glm::vec3& ground, glm::vec3& normal)
    {
        glm::mat4 inv = glm::inverse(world);
        glm::vec3 local = glm::vec3(inv * glm::vec4(point, 1.0f));
        glm::vec2 half = glm::max(terrain.Size, glm::vec2(0.01f)) * 0.5f;
        if (std::abs(local.x) > half.x || std::abs(local.z) > half.y) return false;
        // From the height slopes around the point, over about a sample's spacing. The heights are read in place: a
        // copy of the grid per call (tens of MB on a big terrain) made scripts sampling every frame crawl.
        float h = std::max(terrain.Size.x, terrain.Size.y) / (float)std::max(SampleCount(terrain) - 1, 1) * 0.5f;
        float dx = 0.0f, dz = 0.0f;
        WithHeights(terrain, [&](const std::vector<float>& heights) {
            local.y = HeightAt(terrain, heights, local.x, local.z);
            dx = HeightAt(terrain, heights, local.x + h, local.z) - HeightAt(terrain, heights, local.x - h, local.z);
            dz = HeightAt(terrain, heights, local.x, local.z + h) - HeightAt(terrain, heights, local.x, local.z - h);
        });
        ground = glm::vec3(world * glm::vec4(local, 1.0f));
        normal = glm::normalize(glm::transpose(glm::mat3(inv)) * glm::vec3(-dx, 2.0f * h, -dz));
        return true;
    }

    void PrepareSampling(TerrainComponent& terrain)
    {
        int n = SampleCount(terrain);
        if (terrain.EditHeights.size() != (size_t)n * n) terrain.EditHeights = GetHeights(terrain);
    }

    std::vector<float> GetHeights(const TerrainComponent& terrain)
    {
        std::vector<float> copy;
        WithHeights(terrain, [&](const std::vector<float>& heights) { copy = heights; });
        return copy;
    }

    void ClearEdits(TerrainComponent& terrain)
    {
        terrain.SculptedHeights.clear();
        terrain.PaintedLayers.clear();
        terrain.PaintedHoles.clear();
        terrain.EditHeights.clear();
        terrain.EditLayers.clear();
        terrain.EditHoles.clear();
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

    std::vector<glm::vec3> PathCurve(const std::vector<glm::vec3>& points, int perSegment)
    {
        if (points.size() < 2) return points;
        std::vector<glm::vec3> curve;
        size_t n = points.size();
        for (size_t i = 0; i + 1 < n; ++i)
        {
            const glm::vec3& p0 = points[i > 0 ? i - 1 : 0];
            const glm::vec3& p1 = points[i];
            const glm::vec3& p2 = points[i + 1];
            const glm::vec3& p3 = points[std::min(i + 2, n - 1)];
            for (int k = 0; k < perSegment; ++k)
            {
                float t = (float)k / perSegment, t2 = t * t, t3 = t2 * t;
                curve.push_back(0.5f * (2.0f * p1 + (p2 - p0) * t + (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * t2 + (3.0f * p1 - p0 - 3.0f * p2 + p3) * t3));
            }
        }
        curve.push_back(points.back());
        return curve;
    }

    int ApplyPath(TerrainComponent& terrain, const glm::mat4& terrainWorld, const TerrainPathComponent& path, const glm::mat4& pathWorld)
    {
        int n = SampleCount(terrain);
        if (terrain.EditHeights.size() != (size_t)n * n || terrain.EditLayers.size() != (size_t)n * n * SplatMapCount(terrain)) return 0;
        // The curve in the terrain's space.
        glm::mat4 toTerrain = glm::inverse(terrainWorld) * pathWorld;
        std::vector<glm::vec3> curve = PathCurve(path.Points);
        for (glm::vec3& p : curve) p = glm::vec3(toTerrain * glm::vec4(p, 1.0f));
        if (curve.size() < 2) return 0;
        float half = std::max(path.Width, 0.0f) * 0.5f, falloff = std::max(path.Falloff, 0.0f), reach = half + falloff;
        glm::vec2 lo(1e30f), hi(-1e30f);
        for (const glm::vec3& p : curve) { lo = glm::min(lo, glm::vec2(p.x, p.z)); hi = glm::max(hi, glm::vec2(p.x, p.z)); }
        glm::vec2 size = glm::max(terrain.Size, glm::vec2(0.01f));
        glm::vec2 cell = size / (float)(n - 1);
        int c0 = std::max(0, (int)std::floor((lo.x - reach) / cell.x + (n - 1) * 0.5f)), c1 = std::min(n - 1, (int)std::ceil((hi.x + reach) / cell.x + (n - 1) * 0.5f));
        int r0 = std::max(0, (int)std::floor((lo.y - reach) / cell.y + (n - 1) * 0.5f)), r1 = std::min(n - 1, (int)std::ceil((hi.y + reach) / cell.y + (n - 1) * 0.5f));
        int changed = 0;
        for (int r = r0; r <= r1; ++r)
            for (int c = c0; c <= c1; ++c)
            {
                glm::vec2 p(-size.x * 0.5f + c * cell.x, -size.y * 0.5f + r * cell.y);
                // The nearest point of the curve (across XZ) and its height.
                float best = 1e30f, height = 0.0f;
                for (size_t k = 0; k + 1 < curve.size(); ++k)
                {
                    glm::vec2 a(curve[k].x, curve[k].z), b(curve[k + 1].x, curve[k + 1].z);
                    glm::vec2 ab = b - a;
                    float t = glm::dot(ab, ab) > 1e-12f ? std::clamp(glm::dot(p - a, ab) / glm::dot(ab, ab), 0.0f, 1.0f) : 0.0f;
                    float d = glm::distance(p, a + ab * t);
                    if (d < best)
                    {
                        best = d;
                        height = glm::mix(curve[k].y, curve[k + 1].y, t);
                    }
                }
                if (best >= reach) continue;
                size_t i = (size_t)r * n + c;
                float& h = terrain.EditHeights[i];
                // Full effect across the width, easing out over the falloff.
                float w = best <= half ? 1.0f : 1.0f - Smooth(half, reach, best);
                if (path.Mode == TerrainPathMode::Road)
                    h = glm::mix(h, height, w);
                else
                {
                    // River: a rounded channel below the curve, banks lowered to the curve's height.
                    float inside = half > 0.0f ? std::clamp(best / half, 0.0f, 1.0f) : 1.0f;
                    float bed = height - std::max(path.Depth, 0.0f) * (1.0f - inside * inside);
                    float target = best <= half ? bed : std::min(h, height);
                    h = glm::mix(h, std::min(h, target), w);
                }
                h = std::clamp(h, 0.0f, terrain.Height);
                if (path.PaintLayer >= 0 && path.PaintLayer < LayerCount(terrain))
                    PaintSample(terrain, i, path.PaintLayer, best <= half ? 1.0f : 1.0f - Smooth(half, half + falloff * 0.5f, best));
                ++changed;
            }
        ++terrain.EditRevision;
        return changed;
    }

    int Crater(TerrainComponent& terrain, const glm::mat4& world, const glm::vec3& point, float radius, float depth, int paintLayer)
    {
        int n = SampleCount(terrain);
        if (terrain.EditHeights.size() != (size_t)n * n || terrain.EditLayers.size() != (size_t)n * n * SplatMapCount(terrain) || radius <= 0.0f) return 0;
        // In the terrain's space (meters along its axes).
        glm::vec3 local = glm::vec3(glm::inverse(world) * glm::vec4(point, 1.0f));
        glm::vec3 scale(glm::length(glm::vec3(world[0])), glm::length(glm::vec3(world[1])), glm::length(glm::vec3(world[2])));
        float r = radius / std::max(0.5f * (scale.x + scale.z), 1e-4f), d = depth / std::max(scale.y, 1e-4f);
        float reach = r * 1.8f;
        glm::vec2 size = glm::max(terrain.Size, glm::vec2(0.01f));
        glm::vec2 cell = size / (float)(n - 1);
        int c0 = std::max(0, (int)std::floor((local.x - reach) / cell.x + (n - 1) * 0.5f)), c1 = std::min(n - 1, (int)std::ceil((local.x + reach) / cell.x + (n - 1) * 0.5f));
        int r0 = std::max(0, (int)std::floor((local.z - reach) / cell.y + (n - 1) * 0.5f)), r1 = std::min(n - 1, (int)std::ceil((local.z + reach) / cell.y + (n - 1) * 0.5f));
        int changed = 0;
        for (int row = r0; row <= r1; ++row)
            for (int col = c0; col <= c1; ++col)
            {
                glm::vec2 p(-size.x * 0.5f + col * cell.x, -size.y * 0.5f + row * cell.y);
                float dist = glm::distance(p, glm::vec2(local.x, local.z));
                if (dist >= reach) continue;
                size_t i = (size_t)row * n + col;
                // A bowl (deepest in the middle, level with the ground at Radius) and a rim of thrown-up earth
                // around it.
                float bowl = dist < r ? -d * (1.0f - (dist / r) * (dist / r)) : 0.0f;
                float rimT = (dist - r) / (0.3f * r);
                float rim = 0.25f * d * std::exp(-rimT * rimT);
                float& h = terrain.EditHeights[i];
                h = std::clamp(h + bowl + rim, 0.0f, terrain.Height);
                // Scorched ground, and no grass or trees left in it.
                if (paintLayer >= 0 && paintLayer < LayerCount(terrain)) PaintSample(terrain, i, paintLayer, 1.0f - Smooth(0.6f * r, 1.3f * r, dist));
                if (dist < r * 1.1f && terrain.EditDetails.size() == (size_t)n * n * 2) terrain.EditDetails[i * 2] = terrain.EditDetails[i * 2 + 1] = 0;
                ++changed;
            }
        ++terrain.EditRevision;
        // What it touched, so the mesh, details and collider can update just there.
        terrain.EditLog.push_back({ terrain.EditRevision, glm::ivec4(c0, r0, c1, r1) });
        if (terrain.EditLog.size() > 64) terrain.EditLog.erase(terrain.EditLog.begin());
        return changed;
    }

    int DigCrater(Scene& scene, const glm::vec3& point, float radius, float depth, int paintLayer)
    {
        int changed = 0;
        std::vector<std::pair<Entity, glm::ivec4>> dug; // and the samples changed
        for (auto [e, terrain, t] : scene.GetRegistry().ViewOf<TerrainComponent, TransformComponent>())
        {
            if (!t.ActiveInHierarchy) continue;
            glm::vec3 local = glm::vec3(glm::inverse(t.World) * glm::vec4(point, 1.0f));
            glm::vec2 half = glm::max(terrain.Size, glm::vec2(0.01f)) * 0.5f + radius * 2.0f;
            if (std::abs(local.x) > half.x || std::abs(local.z) > half.y) continue;
            // Only the terrain the point is on (not another far under or over it, as a wide one around a valley's).
            glm::vec3 ground, normal;
            if (Sample(terrain, t.World, point, ground, normal) && std::abs(ground.y - point.y) > radius + depth + 1.0f) continue;
            BeginEdit(terrain);
            int n = Crater(terrain, t.World, point, radius, depth, paintLayer);
            // While playing the edit buffers stay the terrain's (the scene is not saved): encoding them back after
            // every crater took longer than the rest together, and decoding them again for the next.
            if (!scene.IsRunning()) CommitEdit(terrain);
            if (n > 0) dug.push_back({ e, terrain.EditLog.empty() ? glm::ivec4(0, 0, -1, -1) : terrain.EditLog.back().Samples });
            changed += n;
        }
        // The collider follows the new ground (the mesh redraws by itself: its settings changed).
        if (Physics3DWorld* physics = scene.GetPhysics3D())
            for (auto& [e, samples] : dug)
                if (!physics->RefreshTerrain(e, samples)) physics->RefreshShape(e);
        // Navigation meshes over it: the ground there sampled again (a bowl too steep to walk out of becomes a hole).
        if (!dug.empty())
        {
            glm::vec3 reach(radius * 2.0f, std::max(depth, radius) * 2.0f, radius * 2.0f);
            Navigation::UpdateArea(scene, point - reach, point + reach);
        }
        return changed;
    }

    void Erode(TerrainComponent& terrain, const glm::vec2& at, float radius, int droplets, uint32_t seed)
    {
        int n = SampleCount(terrain);
        std::vector<float>& h = terrain.EditHeights;
        if (h.size() != (size_t)n * n) return;
        glm::vec2 size = glm::max(terrain.Size, glm::vec2(0.01f));
        glm::vec2 cell = size / (float)(n - 1);
        float cellSize = 0.5f * (cell.x + cell.y);
        // Sample space: x = column, y = row (fractional).
        glm::vec2 center((at.x / size.x + 0.5f) * (n - 1), (at.y / size.y + 0.5f) * (n - 1));
        float r = radius / cellSize;
        auto heightAndGradient = [&](const glm::vec2& p, glm::vec2& gradient) {
            int x = std::clamp((int)p.x, 0, n - 2), y = std::clamp((int)p.y, 0, n - 2);
            float fx = p.x - x, fy = p.y - y;
            float h00 = h[(size_t)y * n + x], h10 = h[(size_t)y * n + x + 1], h01 = h[(size_t)(y + 1) * n + x], h11 = h[(size_t)(y + 1) * n + x + 1];
            gradient = { (h10 - h00) * (1 - fy) + (h11 - h01) * fy, (h01 - h00) * (1 - fx) + (h11 - h10) * fx }; // meters per sample
            return h00 * (1 - fx) * (1 - fy) + h10 * fx * (1 - fy) + h01 * (1 - fx) * fy + h11 * fx * fy;
        };
        // Adds (or takes, with a negative amount) material at a point, shared by its four samples.
        auto deposit = [&](const glm::vec2& p, float amount) {
            int x = std::clamp((int)p.x, 0, n - 2), y = std::clamp((int)p.y, 0, n - 2);
            float fx = p.x - x, fy = p.y - y;
            h[(size_t)y * n + x] += amount * (1 - fx) * (1 - fy);
            h[(size_t)y * n + x + 1] += amount * fx * (1 - fy);
            h[(size_t)(y + 1) * n + x] += amount * (1 - fx) * fy;
            h[(size_t)(y + 1) * n + x + 1] += amount * fx * fy;
        };
        constexpr float kInertia = 0.1f, kCapacity = 4.0f, kMinSlope = 0.01f, kErode = 0.3f, kDeposit = 0.3f, kEvaporate = 0.02f, kGravity = 4.0f;
        constexpr int kLifetime = 40;
        uint32_t state = Hash(seed * 2654435761u + 0x51ed27u);
        for (int d = 0; d < droplets; ++d)
        {
            float angle = Random01(state) * glm::two_pi<float>(), dist = std::sqrt(Random01(state)) * r;
            glm::vec2 pos = center + glm::vec2(std::cos(angle), std::sin(angle)) * dist;
            glm::vec2 dir(0.0f);
            float speed = 1.0f, water = 1.0f, sediment = 0.0f;
            for (int step = 0; step < kLifetime; ++step)
            {
                if (pos.x < 0.0f || pos.y < 0.0f || pos.x > n - 1.001f || pos.y > n - 1.001f) break;
                glm::vec2 gradient;
                float height = heightAndGradient(pos, gradient);
                dir = dir * kInertia - gradient * (1.0f - kInertia);
                float len = glm::length(dir);
                if (len < 1e-6f)
                {
                    deposit(pos, sediment); // flat: it soaks in where it stands
                    break;
                }
                dir /= len;
                glm::vec2 next = pos + dir;
                // Off the terrain or well past the brush: the drop leaves what it carries where it is.
                if (next.x < 0.0f || next.y < 0.0f || next.x > n - 1.001f || next.y > n - 1.001f || glm::length(next - center) > r * 1.5f + 2.0f)
                {
                    deposit(pos, sediment);
                    break;
                }
                glm::vec2 unused;
                float delta = heightAndGradient(next, unused) - height; // meters
                float capacity = std::max(-delta / cellSize, kMinSlope) * speed * water * kCapacity * cellSize;
                if (sediment > capacity || delta > 0.0f)
                {
                    // Uphill: fill the step behind it (as much as it carries); slowing: drop part of the excess.
                    float amount = delta > 0.0f ? std::min(delta, sediment) : (sediment - capacity) * kDeposit;
                    sediment -= amount;
                    deposit(pos, amount);
                }
                else
                {
                    // Take what it can carry, never digging deeper than the drop to the next point.
                    float amount = std::min((capacity - sediment) * kErode, -delta);
                    deposit(pos, -amount);
                    sediment += amount;
                }
                speed = std::sqrt(std::max(speed * speed - delta / cellSize * kGravity, 0.0f));
                water *= 1.0f - kEvaporate;
                pos = next;
                if (step == kLifetime - 1) deposit(pos, sediment);
            }
        }
        for (float& v : h) v = std::clamp(v, 0.0f, terrain.Height);
        ++terrain.EditRevision;
    }
}
