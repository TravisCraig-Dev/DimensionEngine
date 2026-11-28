#include "Dimension/Image/ImageVector.h"
#include "Dimension/Image/ImageFilters.h"
#include "Dimension/Asset/AssetManager.h"
#include <nlohmann/json.hpp>
#include <glm/gtc/constants.hpp>
#include <stb_truetype.h>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <map>
#include <mutex>
#include <thread>

namespace Dimension::Imaging
{
    namespace
    {
        using Contour = std::vector<glm::vec2>;

        // Closed contours filled by the nonzero rule (as fonts are): `sub` rows per pixel, exact coverage across.
        std::vector<float> Fill(uint32_t width, uint32_t height, const std::vector<Contour>& contours, bool antialias)
        {
            std::vector<float> cov((size_t)width * height, 0.0f);
            struct Edge
            {
                glm::vec2 A, B;
                int Dir;
            };
            std::vector<Edge> edges;
            float minY = 1e30f, maxY = -1e30f;
            for (const Contour& c : contours)
                for (size_t i = 0; i < c.size(); ++i)
                {
                    glm::vec2 a = c[i], b = c[(i + 1) % c.size()];
                    if (a.y == b.y) continue;
                    edges.push_back({ a, b, b.y > a.y ? 1 : -1 });
                    minY = std::min({ minY, a.y, b.y });
                    maxY = std::max({ maxY, a.y, b.y });
                }
            if (edges.empty()) return cov;
            const int sub = antialias ? 5 : 1;
            const int y0 = std::max(0, (int)std::floor(minY)), y1 = std::min((int)height - 1, (int)std::ceil(maxY));
            if (y0 > y1) return cov;
            const int rows = y1 - y0 + 1;
            unsigned threads = std::clamp(std::thread::hardware_concurrency(), 1u, 16u);
            auto body = [&](int r0, int r1) {
                std::vector<std::pair<float, int>> xs;
                for (int y = y0 + r0; y <= y0 + r1; ++y)
                {
                    float* row = cov.data() + (size_t)y * width;
                    for (int k = 0; k < sub; ++k)
                    {
                        float sy = y + (k + 0.5f) / sub;
                        xs.clear();
                        for (const Edge& e : edges)
                        {
                            if ((e.A.y <= sy) == (e.B.y <= sy)) continue;
                            xs.push_back({ e.A.x + (sy - e.A.y) / (e.B.y - e.A.y) * (e.B.x - e.A.x), e.Dir });
                        }
                        std::sort(xs.begin(), xs.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
                        int winding = 0;
                        for (size_t i = 0; i + 1 < xs.size(); ++i)
                        {
                            winding += xs[i].second;
                            if (winding == 0) continue;
                            float xa = std::max(0.0f, xs[i].first), xb = std::min((float)width, xs[i + 1].first);
                            if (xb <= xa) continue;
                            if (!antialias)
                            {
                                int a = (int)std::ceil(xa - 0.5f), b = (int)std::floor(xb - 0.5f);
                                for (int x = std::max(a, 0); x <= std::min(b, (int)width - 1); ++x) row[x] = 1.0f;
                                continue;
                            }
                            for (int x = (int)std::floor(xa); x < (int)std::ceil(xb) && x < (int)width; ++x)
                                row[x] += std::max(0.0f, std::min(xb, x + 1.0f) - std::max(xa, (float)x)) / sub;
                        }
                    }
                    for (uint32_t x = 0; x < width; ++x) row[x] = std::min(row[x], 1.0f);
                }
            };
            if (threads == 1 || (size_t)rows * width < 32 * 1024) body(0, rows - 1);
            else
            {
                std::vector<std::thread> pool;
                int per = (rows + (int)threads - 1) / (int)threads;
                for (int s = 0; s < rows; s += per) pool.emplace_back(body, s, std::min(rows - 1, s + per - 1));
                for (auto& t : pool) t.join();
            }
            return cov;
        }

        glm::vec2 Rotate(const glm::vec2& p, float degrees)
        {
            float a = glm::radians(degrees), c = std::cos(a), s = std::sin(a);
            return { p.x * c - p.y * s, p.x * s + p.y * c };
        }

        // Paint `color` by `alpha` over `out` (straight).
        void Over(VectorBitmap& out, size_t i, const glm::vec4& color, float alpha)
        {
            float sa = color.a * std::clamp(alpha, 0.0f, 1.0f);
            if (sa <= 0.0f) return;
            glm::vec4 d = Unpack(out.Pixels[i]);
            float a = sa + d.a * (1.0f - sa);
            glm::vec3 c = (glm::vec3(color) * sa + glm::vec3(d) * d.a * (1.0f - sa)) / a;
            out.Pixels[i] = Pack(glm::vec4(c, a));
        }

        // Font files, read once.
        const std::vector<uint8_t>* FontBytes(const std::string& path)
        {
            static std::mutex mutex;
            static std::map<std::string, std::vector<uint8_t>> cache;
            std::lock_guard lock(mutex);
            auto it = cache.find(path);
            if (it != cache.end()) return it->second.empty() ? nullptr : &it->second;
            std::vector<uint8_t> bytes;
            if (std::ifstream f{ fs::path(path), std::ios::binary }) bytes.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
            auto& slot = cache[path] = std::move(bytes);
            return slot.empty() ? nullptr : &slot;
        }

        // The font: its file, else the built-in Roboto.
        bool OpenFont(const std::string& path, stbtt_fontinfo& info)
        {
            const uint8_t* data = nullptr;
            if (!path.empty())
                if (const std::vector<uint8_t>* bytes = FontBytes(path)) data = bytes->data();
            if (!data)
            {
                size_t size = 0;
                data = AssetManager::GetDefaultFontData(&size);
            }
            return data && stbtt_InitFont(&info, data, stbtt_GetFontOffsetForIndex(data, 0)) != 0;
        }

        // The next character of UTF-8 text (bad bytes count as themselves).
        uint32_t NextCodepoint(const std::string& s, size_t& i)
        {
            unsigned char c = (unsigned char)s[i];
            int n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
            if (i + n > s.size()) n = 1;
            uint32_t cp = n == 1 ? c : c & (0xFF >> (n + 1));
            for (int k = 1; k < n; ++k) cp = (cp << 6) | ((unsigned char)s[i + k] & 0x3F);
            i += n;
            return cp;
        }

        nlohmann::json Vec(const glm::vec2& v) { return nlohmann::json::array({ v.x, v.y }); }
        nlohmann::json Vec(const glm::vec4& v) { return nlohmann::json::array({ v.x, v.y, v.z, v.w }); }
        glm::vec2 Vec2(const nlohmann::json& j, glm::vec2 f) { return j.is_array() && j.size() >= 2 ? glm::vec2(j[0].get<float>(), j[1].get<float>()) : f; }
        glm::vec4 Vec4(const nlohmann::json& j, glm::vec4 f)
        {
            return j.is_array() && j.size() >= 4 ? glm::vec4(j[0].get<float>(), j[1].get<float>(), j[2].get<float>(), j[3].get<float>()) : f;
        }
    }

    const char* ShapeKindName(ShapeKind kind)
    {
        static constexpr const char* kNames[] = { "Rectangle", "Rounded Rectangle", "Ellipse", "Polygon", "Star", "Line" };
        return (size_t)kind < std::size(kNames) ? kNames[(size_t)kind] : "Shape";
    }

    // ---- Shapes

    std::vector<glm::vec2> ShapeOutline(const ShapeData& s)
    {
        std::vector<glm::vec2> local;
        const glm::vec2 half = glm::abs(s.Size) * 0.5f;
        switch (s.Kind)
        {
        case ShapeKind::RoundedRectangle:
        {
            float r = std::clamp(s.Radius, 0.0f, std::min(half.x, half.y));
            const glm::vec2 centers[4] = { { half.x - r, -half.y + r }, { half.x - r, half.y - r }, { -half.x + r, half.y - r }, { -half.x + r, -half.y + r } };
            const int steps = std::clamp((int)(r * 0.5f), 4, 32);
            for (int c = 0; c < 4; ++c)
                for (int k = 0; k <= steps; ++k)
                {
                    float a = glm::radians(-90.0f + 90.0f * c) + k * glm::half_pi<float>() / steps;
                    local.push_back(centers[c] + glm::vec2(std::cos(a), std::sin(a)) * r);
                }
            break;
        }
        case ShapeKind::Ellipse:
        {
            const int n = std::clamp((int)((half.x + half.y) * 1.5f), 24, 720);
            for (int k = 0; k < n; ++k)
            {
                float a = k * glm::two_pi<float>() / n;
                local.push_back({ std::cos(a) * half.x, std::sin(a) * half.y });
            }
            break;
        }
        case ShapeKind::Polygon:
        case ShapeKind::Star:
        {
            const int n = std::clamp(s.Sides, 3, 100);
            const bool star = s.Kind == ShapeKind::Star;
            const int count = star ? n * 2 : n;
            for (int k = 0; k < count; ++k)
            {
                float a = -glm::half_pi<float>() + k * glm::two_pi<float>() / count;
                float r = star && (k % 2) ? 1.0f - std::clamp(s.Inset, 0.0f, 0.95f) : 1.0f;
                local.push_back({ std::cos(a) * half.x * r, std::sin(a) * half.y * r });
            }
            break;
        }
        default: // Rectangle, Line (its length along x, its weight across)
            local = { { -half.x, -half.y }, { half.x, -half.y }, { half.x, half.y }, { -half.x, half.y } };
            break;
        }
        for (glm::vec2& p : local) p = s.Center + Rotate(p, s.Rotation);
        return local;
    }

    std::array<glm::vec2, 4> ShapeCorners(const ShapeData& s)
    {
        const glm::vec2 h = glm::abs(s.Size) * 0.5f;
        return { s.Center + Rotate({ -h.x, -h.y }, s.Rotation), s.Center + Rotate({ h.x, -h.y }, s.Rotation), s.Center + Rotate({ h.x, h.y }, s.Rotation),
                 s.Center + Rotate({ -h.x, h.y }, s.Rotation) };
    }

    VectorBitmap RenderShape(const ShapeData& s, uint32_t width, uint32_t height)
    {
        VectorBitmap out = VectorBitmap::Solid(width, height, 0u);
        std::vector<float> cov = Fill(width, height, { ShapeOutline(s) }, true);
        const size_t n = cov.size();
        if (s.Fill)
            for (size_t i = 0; i < n; ++i)
                if (cov[i] > 0.0f) Over(out, i, s.FillColor, cov[i]);
        if (s.Stroke && s.StrokeWidth > 0.0f && s.Kind != ShapeKind::Line)
        {
            // The distance to the edge from each side (pixel centers), the stroke by it.
            std::vector<uint8_t> inside(n), outside(n);
            for (size_t i = 0; i < n; ++i) inside[i] = cov[i] >= 0.5f, outside[i] = !inside[i];
            std::vector<float> toInside = DistanceField(inside, (int)width, (int)height), toOutside = DistanceField(outside, (int)width, (int)height);
            const float w = s.StrokeWidth;
            const float in = s.StrokeAlign == 0 ? w : s.StrokeAlign == 1 ? w * 0.5f : 0.0f;
            const float outW = s.StrokeAlign == 2 ? w : s.StrokeAlign == 1 ? w * 0.5f : 0.0f;
            for (size_t i = 0; i < n; ++i)
            {
                float a = 0.0f;
                // (a pixel whose center is d from the edge is covered by a band of width w by about w - d + 1/2; the
                // distance between pixel centers across the edge is d + 1/2)
                if (inside[i]) a = in > 0.0f ? std::clamp(in - std::sqrt(toOutside[i]) + 1.0f, 0.0f, 1.0f) * cov[i] : 0.0f;
                else a = outW > 0.0f ? std::clamp(outW - std::sqrt(toInside[i]) + 1.0f, 0.0f, 1.0f) : 0.0f;
                // (the antialiased rim between: the part the fill leaves)
                if (inside[i] && outW > 0.0f) a = std::max(a, 1.0f - cov[i]);
                if (!inside[i] && in > 0.0f) a = std::max(a, cov[i]);
                if (a > 0.0f) Over(out, i, s.StrokeColor, a);
            }
        }
        return out;
    }

    // ---- Text

    glm::vec2 TextToCanvas(const TextData& t, const glm::vec2& local) { return t.Position + Rotate(local, t.Rotation); }
    glm::vec2 CanvasToText(const TextData& t, const glm::vec2& canvas) { return Rotate(canvas - t.Position, -t.Rotation); }

    TextLayout LayoutText(const TextData& t)
    {
        TextLayout layout;
        stbtt_fontinfo info;
        if (!OpenFont(t.Font, info)) return layout;
        const float scale = stbtt_ScaleForMappingEmToPixels(&info, std::max(t.Size, 1.0f));
        int ascent = 0, descent = 0, gap = 0;
        stbtt_GetFontVMetrics(&info, &ascent, &descent, &gap);
        layout.Ascent = ascent * scale;
        layout.Descent = -descent * scale;
        layout.LineHeight = t.Size * t.Leading;
        size_t start = 0;
        float baseline = 0.0f;
        for (;;)
        {
            size_t end = t.Text.find('\n', start);
            if (end == std::string::npos) end = t.Text.size();
            TextLayout::Line line;
            line.Start = start;
            line.End = end;
            line.Baseline = baseline;
            float x = 0.0f;
            uint32_t prev = 0;
            for (size_t i = start; i < end;)
            {
                size_t at = i;
                uint32_t cp = NextCodepoint(t.Text, i);
                if (prev) x += stbtt_GetCodepointKernAdvance(&info, (int)prev, (int)cp) * scale;
                line.Carets.push_back(x);
                line.Offsets.push_back(at);
                int advance = 0, bearing = 0;
                stbtt_GetCodepointHMetrics(&info, (int)cp, &advance, &bearing);
                x += advance * scale + t.Tracking;
                prev = cp;
            }
            line.Carets.push_back(x);
            line.Offsets.push_back(end);
            line.Width = x;
            line.X = t.Align == 1 ? -x * 0.5f : t.Align == 2 ? -x : 0.0f;
            layout.Lines.push_back(std::move(line));
            if (end >= t.Text.size()) break;
            start = end + 1;
            baseline += layout.LineHeight;
        }
        layout.Min = glm::vec2(1e30f, -layout.Ascent);
        layout.Max = glm::vec2(-1e30f, baseline + layout.Descent);
        for (const auto& l : layout.Lines)
        {
            layout.Min.x = std::min(layout.Min.x, l.X);
            layout.Max.x = std::max(layout.Max.x, l.X + l.Width);
        }
        if (layout.Max.x < layout.Min.x) layout.Min.x = layout.Max.x = 0.0f;
        return layout;
    }

    std::array<glm::vec2, 4> TextCorners(const TextData& t, const TextLayout& l)
    {
        return { TextToCanvas(t, l.Min), TextToCanvas(t, { l.Max.x, l.Min.y }), TextToCanvas(t, l.Max), TextToCanvas(t, { l.Min.x, l.Max.y }) };
    }

    VectorBitmap RenderText(const TextData& t, uint32_t width, uint32_t height)
    {
        VectorBitmap out = VectorBitmap::Solid(width, height, 0u);
        stbtt_fontinfo info;
        if (!OpenFont(t.Font, info)) return out;
        const TextLayout layout = LayoutText(t);
        const float scale = stbtt_ScaleForMappingEmToPixels(&info, std::max(t.Size, 1.0f));
        const float shear = t.FauxItalic ? 0.21f : 0.0f;
        // Every glyph's outline, its curves flattened, into the canvas.
        std::vector<Contour> contours;
        for (const auto& line : layout.Lines)
        {
            size_t k = 0;
            for (size_t i = line.Start; i < line.End; ++k)
            {
                uint32_t cp = NextCodepoint(t.Text, i);
                stbtt_vertex* v = nullptr;
                int count = stbtt_GetCodepointShape(&info, (int)cp, &v);
                const float penX = line.X + line.Carets[k];
                auto place = [&](float fx, float fy) {
                    glm::vec2 local(penX + fx * scale, line.Baseline - fy * scale);
                    local.x -= (local.y - line.Baseline) * shear;
                    return TextToCanvas(t, local);
                };
                Contour c;
                glm::vec2 last(0.0f);
                const int steps = std::clamp((int)(t.Size / 6.0f), 4, 16);
                for (int j = 0; j < count; ++j)
                {
                    const stbtt_vertex& e = v[j];
                    glm::vec2 p((float)e.x, (float)e.y);
                    if (e.type == STBTT_vmove)
                    {
                        if (c.size() >= 3) contours.push_back(std::move(c));
                        c.clear();
                        c.push_back(place(p.x, p.y));
                    }
                    else if (e.type == STBTT_vline)
                        c.push_back(place(p.x, p.y));
                    else if (e.type == STBTT_vcurve)
                    {
                        glm::vec2 ctl((float)e.cx, (float)e.cy);
                        for (int s = 1; s <= steps; ++s)
                        {
                            float u = s / (float)steps;
                            glm::vec2 q = (1 - u) * (1 - u) * last + 2 * (1 - u) * u * ctl + u * u * p;
                            c.push_back(place(q.x, q.y));
                        }
                    }
                    else if (e.type == STBTT_vcubic)
                    {
                        glm::vec2 c0((float)e.cx, (float)e.cy), c1((float)e.cx1, (float)e.cy1);
                        for (int s = 1; s <= steps; ++s)
                        {
                            float u = s / (float)steps, w = 1 - u;
                            glm::vec2 q = w * w * w * last + 3 * w * w * u * c0 + 3 * w * u * u * c1 + u * u * u * p;
                            c.push_back(place(q.x, q.y));
                        }
                    }
                    last = p;
                }
                if (c.size() >= 3) contours.push_back(std::move(c));
                if (v) stbtt_FreeShape(&info, v);
            }
        }
        std::vector<float> cov = Fill(width, height, contours, t.Antialias);
        if (t.FauxBold)
        {
            // Thicker by a 30th of the size all round.
            std::vector<uint8_t> inside(cov.size());
            for (size_t i = 0; i < cov.size(); ++i) inside[i] = cov[i] >= 0.5f;
            std::vector<float> d = DistanceField(inside, (int)width, (int)height);
            const float r = std::max(0.5f, t.Size / 30.0f);
            for (size_t i = 0; i < cov.size(); ++i) cov[i] = std::max(cov[i], std::clamp(r + 0.5f - std::sqrt(d[i]), 0.0f, 1.0f));
        }
        for (size_t i = 0; i < cov.size(); ++i)
            if (cov[i] > 0.0f) Over(out, i, t.Color, t.Antialias ? cov[i] : (cov[i] >= 0.5f ? 1.0f : 0.0f));
        return out;
    }

    const std::vector<FontFile>& FindFonts(const fs::path& assets)
    {
        static std::mutex mutex;
        static std::map<std::string, std::vector<FontFile>> cache;
        std::lock_guard lock(mutex);
        auto& list = cache[assets.generic_string()];
        if (!list.empty()) return list;
        list.push_back({ "Roboto (built in)", "" });
        auto scan = [&](const fs::path& dir, const char* suffix) {
            std::error_code ec;
            if (!fs::is_directory(dir, ec)) return;
            for (auto it = fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec); !ec && it != fs::recursive_directory_iterator();
                 it.increment(ec))
            {
                std::string ext = it->path().extension().string();
                std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return (char)std::tolower(c); });
                if (ext == ".ttf" || ext == ".otf") list.push_back({ it->path().stem().string() + suffix, it->path().string() });
            }
        };
        scan(assets, " (project)");
#ifdef _WIN32
        scan("C:/Windows/Fonts", "");
        if (const char* local = std::getenv("LOCALAPPDATA")) scan(fs::path(local) / "Microsoft/Windows/Fonts", "");
#else
        scan("/usr/share/fonts", "");
        scan("/usr/local/share/fonts", "");
        if (const char* home = std::getenv("HOME")) scan(fs::path(home) / ".fonts", ""), scan(fs::path(home) / ".local/share/fonts", "");
        scan("/System/Library/Fonts", "");
        scan("/Library/Fonts", "");
#endif
        std::sort(list.begin() + 1, list.end(), [](const FontFile& a, const FontFile& b) { return a.Name < b.Name; });
        return list;
    }

    // ---- The file

    nlohmann::json ToJson(const ShapeData& s)
    {
        return { { "kind", ShapeKindName(s.Kind) }, { "center", Vec(s.Center) }, { "size", Vec(s.Size) },     { "rotation", s.Rotation },
                 { "radius", s.Radius },          { "sides", s.Sides },         { "inset", s.Inset },          { "fill", s.Fill },
                 { "fillColor", Vec(s.FillColor) }, { "stroke", s.Stroke },     { "strokeColor", Vec(s.StrokeColor) },
                 { "strokeWidth", s.StrokeWidth }, { "strokeAlign", s.StrokeAlign } };
    }

    ShapeData ShapeFromJson(const nlohmann::json& j)
    {
        ShapeData s;
        if (!j.is_object()) return s;
        std::string kind = j.value("kind", std::string("Rectangle"));
        for (int k = 0; k < (int)ShapeKind::Count; ++k)
            if (kind == ShapeKindName((ShapeKind)k)) s.Kind = (ShapeKind)k;
        s.Center = Vec2(j.value("center", nlohmann::json()), s.Center);
        s.Size = Vec2(j.value("size", nlohmann::json()), s.Size);
        s.Rotation = j.value("rotation", 0.0f);
        s.Radius = j.value("radius", s.Radius);
        s.Sides = j.value("sides", s.Sides);
        s.Inset = j.value("inset", s.Inset);
        s.Fill = j.value("fill", true);
        s.FillColor = Vec4(j.value("fillColor", nlohmann::json()), s.FillColor);
        s.Stroke = j.value("stroke", false);
        s.StrokeColor = Vec4(j.value("strokeColor", nlohmann::json()), s.StrokeColor);
        s.StrokeWidth = j.value("strokeWidth", s.StrokeWidth);
        s.StrokeAlign = j.value("strokeAlign", 0);
        return s;
    }

    nlohmann::json ToJson(const TextData& t)
    {
        return { { "text", t.Text },         { "font", t.Font },         { "size", t.Size },         { "color", Vec(t.Color) },
                 { "align", t.Align },       { "leading", t.Leading },   { "tracking", t.Tracking }, { "position", Vec(t.Position) },
                 { "rotation", t.Rotation }, { "antialias", t.Antialias }, { "fauxBold", t.FauxBold }, { "fauxItalic", t.FauxItalic } };
    }

    TextData TextFromJson(const nlohmann::json& j)
    {
        TextData t;
        if (!j.is_object()) return t;
        t.Text = j.value("text", t.Text);
        t.Font = j.value("font", std::string());
        t.Size = j.value("size", t.Size);
        t.Color = Vec4(j.value("color", nlohmann::json()), t.Color);
        t.Align = j.value("align", 0);
        t.Leading = j.value("leading", 1.2f);
        t.Tracking = j.value("tracking", 0.0f);
        t.Position = Vec2(j.value("position", nlohmann::json()), t.Position);
        t.Rotation = j.value("rotation", 0.0f);
        t.Antialias = j.value("antialias", true);
        t.FauxBold = j.value("fauxBold", false);
        t.FauxItalic = j.value("fauxItalic", false);
        return t;
    }
}
