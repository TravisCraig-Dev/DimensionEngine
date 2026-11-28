#include "Dimension/Image/ImageGame.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

namespace Dimension::Imaging
{
    namespace
    {
        glm::vec3 Hex(uint32_t rgb) { return glm::vec3((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF) / 255.0f; }
        // (weighted for how the eye sees the channels)
        float Distance(const glm::vec3& a, const glm::vec3& b)
        {
            glm::vec3 d = a - b;
            return 0.30f * d.r * d.r + 0.59f * d.g * d.g + 0.11f * d.b * d.b;
        }
    }

    Bitmap OffsetWrap(const Bitmap& image, glm::ivec2 by)
    {
        Bitmap out = image;
        const int w = (int)image.Width, h = (int)image.Height;
        if (w == 0 || h == 0) return out;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
            {
                int sx = ((x - by.x) % w + w) % w, sy = ((y - by.y) % h + h) % h;
                out.Pixels[(size_t)y * w + x] = image.Pixels[(size_t)sy * w + sx];
            }
        return out;
    }

    Bitmap MakeSeamless(const Bitmap& image, float blend)
    {
        const int w = (int)image.Width, h = (int)image.Height;
        if (w < 2 || h < 2) return image;
        // The half-moved copy has its seams in the middle and clean edges; the original has clean middles. Each pixel
        // takes the copy near the edges (where the original's seams are) and the original elsewhere.
        Bitmap shifted = OffsetWrap(image, { w / 2, h / 2 });
        Bitmap out = image;
        const float band = std::clamp(blend, 0.01f, 1.0f) * 0.5f;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
            {
                // 0 at an edge, 1 from `band` (a fraction of the size) in.
                float ex = std::min(x + 0.5f, w - x - 0.5f) / (w * band), ey = std::min(y + 0.5f, h - y - 0.5f) / (h * band);
                float t = std::clamp(std::min(ex, ey), 0.0f, 1.0f);
                t = t * t * (3.0f - 2.0f * t);
                size_t i = (size_t)y * w + x;
                glm::vec4 a = Unpack(image.Pixels[i]), b = Unpack(shifted.Pixels[i]);
                glm::vec4 pa(glm::vec3(a) * a.a, a.a), pb(glm::vec3(b) * b.a, b.a);
                glm::vec4 m = glm::mix(pb, pa, t);
                out.Pixels[i] = m.a > 0.0f ? Pack(glm::vec4(glm::vec3(m) / m.a, m.a)) : 0u;
            }
        return out;
    }

    Bitmap NormalMap(const Bitmap& height, float strength, bool invertY, bool wrap)
    {
        const int w = (int)height.Width, h = (int)height.Height;
        Bitmap out = Bitmap::Solid(height.Width, height.Height, 0u);
        std::vector<float> hgt((size_t)w * h);
        for (size_t i = 0; i < hgt.size(); ++i)
        {
            glm::vec4 c = Unpack(height.Pixels[i]);
            hgt[i] = Luminance(glm::vec3(c)) * c.a;
        }
        auto at = [&](int x, int y) {
            if (wrap) x = (x % w + w) % w, y = (y % h + h) % h;
            else x = std::clamp(x, 0, w - 1), y = std::clamp(y, 0, h - 1);
            return hgt[(size_t)y * w + x];
        };
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
            {
                // Sobel slopes (height rising to the right and downwards).
                float dx = (at(x + 1, y - 1) + 2.0f * at(x + 1, y) + at(x + 1, y + 1)) - (at(x - 1, y - 1) + 2.0f * at(x - 1, y) + at(x - 1, y + 1));
                float dy = (at(x - 1, y + 1) + 2.0f * at(x, y + 1) + at(x + 1, y + 1)) - (at(x - 1, y - 1) + 2.0f * at(x, y - 1) + at(x + 1, y - 1));
                // Image rows go down; OpenGL normal maps have green up, so a height rising downwards tilts the normal up.
                glm::vec3 n = glm::normalize(glm::vec3(-dx * strength, dy * strength * (invertY ? -1.0f : 1.0f), 1.0f));
                out.Pixels[(size_t)y * w + x] = Pack(glm::vec4(n * 0.5f + 0.5f, 1.0f));
            }
        return out;
    }

    const std::vector<Palette>& BuiltInPalettes()
    {
        static const std::vector<Palette> palettes = [] {
            auto make = [](const char* name, std::initializer_list<uint32_t> rgb) {
                Palette p;
                p.Name = name;
                for (uint32_t c : rgb) p.Colors.push_back(Hex(c));
                return p;
            };
            return std::vector<Palette>{
                make("PICO-8", { 0x000000, 0x1D2B53, 0x7E2553, 0x008751, 0xAB5236, 0x5F574F, 0xC2C3C7, 0xFFF1E8, 0xFF004D, 0xFFA300, 0xFFEC27,
                                 0x00E436, 0x29ADFF, 0x83769C, 0xFF77A8, 0xFFCCAA }),
                make("Sweetie 16", { 0x1A1C2C, 0x5D275D, 0xB13E53, 0xEF7D57, 0xFFCD75, 0xA7F070, 0x38B764, 0x257179, 0x29366F, 0x3B5DC9, 0x41A6F6,
                                     0x73EFF7, 0xF4F4F4, 0x94B0C2, 0x566C86, 0x333C57 }),
                make("Game Boy", { 0x0F380F, 0x306230, 0x8BAC0F, 0x9BBC0F }),
                make("Grayscale 4", { 0x000000, 0x555555, 0xAAAAAA, 0xFFFFFF }),
                make("Grayscale 8", { 0x000000, 0x242424, 0x494949, 0x6D6D6D, 0x929292, 0xB6B6B6, 0xDBDBDB, 0xFFFFFF }),
                make("Basic 16", { 0x000000, 0xFFFFFF, 0x808080, 0xC0C0C0, 0x800000, 0xFF0000, 0x808000, 0xFFFF00, 0x008000, 0x00FF00, 0x008080,
                                   0x00FFFF, 0x000080, 0x0000FF, 0x800080, 0xFF00FF }),
            };
        }();
        return palettes;
    }

    bool LoadPalette(const fs::path& file, Palette& out)
    {
        std::ifstream f(file);
        if (!f) return false;
        Palette p;
        p.Name = file.stem().string();
        p.File = file.string();
        std::string ext = file.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        std::string line;
        int lineNo = 0;
        while (std::getline(f, line))
        {
            ++lineNo;
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (ext == ".hex")
            {
                std::string hex = line;
                if (!hex.empty() && hex[0] == '#') hex.erase(0, 1);
                if (hex.size() >= 6 && hex.find_first_not_of("0123456789abcdefABCDEF") >= 6) p.Colors.push_back(Hex((uint32_t)std::stoul(hex.substr(0, 6), nullptr, 16)));
                continue;
            }
            if (ext == ".pal" && lineNo <= 3) continue; // (JASC-PAL, 0100, the count)
            if (ext == ".gpl" && line.rfind("Name:", 0) == 0)
            {
                p.Name = line.substr(5);
                p.Name.erase(0, p.Name.find_first_not_of(' '));
                continue;
            }
            if (line.empty() || line[0] == '#' || !std::isdigit((unsigned char)line[line.find_first_not_of(" \t") == std::string::npos ? 0 : line.find_first_not_of(" \t")]))
                continue;
            std::istringstream s(line);
            int r = 0, g = 0, b = 0;
            if (s >> r >> g >> b) p.Colors.push_back(glm::vec3(std::clamp(r, 0, 255), std::clamp(g, 0, 255), std::clamp(b, 0, 255)) / 255.0f);
        }
        if (p.Colors.empty()) return false;
        out = std::move(p);
        return true;
    }

    bool SavePalette(const fs::path& file, const Palette& palette)
    {
        std::error_code ec;
        fs::create_directories(file.parent_path(), ec);
        std::ofstream f(file, std::ios::trunc);
        if (!f) return false;
        f << "GIMP Palette\nName: " << palette.Name << "\nColumns: 8\n#\n";
        for (const glm::vec3& c : palette.Colors)
        {
            glm::ivec3 v = glm::ivec3(glm::round(glm::clamp(c, 0.0f, 1.0f) * 255.0f));
            char hex[8];
            std::snprintf(hex, sizeof(hex), "%02X%02X%02X", v.r, v.g, v.b);
            f << v.r << ' ' << v.g << ' ' << v.b << '\t' << hex << '\n';
        }
        return (bool)f;
    }

    std::vector<glm::vec3> MedianCut(const Bitmap& image, int count)
    {
        std::vector<glm::vec3> colors;
        const size_t step = std::max<size_t>(1, image.Pixels.size() / 200000); // (a sample of big pictures)
        for (size_t i = 0; i < image.Pixels.size(); i += step)
            if ((image.Pixels[i] >> 24) >= 128) colors.push_back(glm::vec3(Unpack(image.Pixels[i])));
        if (colors.empty()) return {};
        struct Box
        {
            size_t Begin, End;
        };
        std::vector<Box> boxes = { { 0, colors.size() } };
        while ((int)boxes.size() < count)
        {
            // The box with the widest range splits at its median along that channel.
            int best = -1, channel = 0;
            float widest = 0.0f;
            for (size_t b = 0; b < boxes.size(); ++b)
            {
                if (boxes[b].End - boxes[b].Begin < 2) continue;
                glm::vec3 lo(1.0f), hi(0.0f);
                for (size_t i = boxes[b].Begin; i < boxes[b].End; ++i) lo = glm::min(lo, colors[i]), hi = glm::max(hi, colors[i]);
                glm::vec3 range = hi - lo;
                for (int c = 0; c < 3; ++c)
                    if (range[c] > widest) widest = range[c], best = (int)b, channel = c;
            }
            if (best < 0 || widest <= 0.0f) break;
            Box box = boxes[(size_t)best];
            size_t mid = box.Begin + (box.End - box.Begin) / 2;
            std::nth_element(colors.begin() + (ptrdiff_t)box.Begin, colors.begin() + (ptrdiff_t)mid, colors.begin() + (ptrdiff_t)box.End,
                             [channel](const glm::vec3& a, const glm::vec3& b) { return a[channel] < b[channel]; });
            boxes[(size_t)best] = { box.Begin, mid };
            boxes.push_back({ mid, box.End });
        }
        std::vector<glm::vec3> out;
        for (const Box& b : boxes)
        {
            glm::vec3 sum(0.0f);
            for (size_t i = b.Begin; i < b.End; ++i) sum += colors[i];
            out.push_back(sum / float(std::max<size_t>(1, b.End - b.Begin)));
        }
        std::sort(out.begin(), out.end(), [](const glm::vec3& a, const glm::vec3& b) { return Luminance(a) < Luminance(b); });
        return out;
    }

    const char* DitherName(Dither dither)
    {
        static constexpr const char* kNames[] = { "None", "Floyd-Steinberg", "Ordered (Bayer)" };
        return (size_t)dither < std::size(kNames) ? kNames[(size_t)dither] : "None";
    }

    int NearestColor(const std::vector<glm::vec3>& palette, const glm::vec3& color)
    {
        int best = 0;
        float bestD = 1e30f;
        for (size_t i = 0; i < palette.size(); ++i)
            if (float d = Distance(palette[i], color); d < bestD) bestD = d, best = (int)i;
        return best;
    }

    Bitmap Quantize(const Bitmap& image, const std::vector<glm::vec3>& palette, Dither dither, bool hardAlpha, const Mask* sel)
    {
        Bitmap out = image;
        if (palette.empty()) return out;
        const int w = (int)image.Width, h = (int)image.Height;
        static constexpr int kBayer[4][4] = { { 0, 8, 2, 10 }, { 12, 4, 14, 6 }, { 3, 11, 1, 9 }, { 15, 7, 13, 5 } };
        // (how far apart the palette's colors are, roughly: the ordered pattern's strength)
        // (kept under half the gap between the palette's colors, so colors already in it stay as they are)
        const float spread = 0.6f / std::max(2.0f, std::cbrt((float)palette.size()));
        std::vector<glm::vec3> error(dither == Dither::FloydSteinberg ? (size_t)w * h : 0, glm::vec3(0.0f));
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
            {
                size_t i = (size_t)y * w + x;
                float s = sel ? sel->Values[i] / 255.0f : 1.0f;
                if (s <= 0.0f) continue;
                glm::vec4 c = Unpack(image.Pixels[i]);
                if (hardAlpha) c.a = c.a >= 0.5f ? 1.0f : 0.0f;
                if (c.a <= 0.0f)
                {
                    out.Pixels[i] = s >= 1.0f ? 0u : image.Pixels[i];
                    continue;
                }
                glm::vec3 want = glm::vec3(c);
                if (dither == Dither::FloydSteinberg) want = glm::clamp(want + error[i], 0.0f, 1.0f);
                else if (dither == Dither::Ordered) want = glm::clamp(want + (kBayer[y & 3][x & 3] / 16.0f - 0.5f) * spread, 0.0f, 1.0f);
                glm::vec3 got = palette[(size_t)NearestColor(palette, want)];
                if (dither == Dither::FloydSteinberg)
                {
                    glm::vec3 e = want - got;
                    auto push = [&](int dx, int dy, float k) {
                        int nx = x + dx, ny = y + dy;
                        if (nx >= 0 && nx < w && ny < h) error[(size_t)ny * w + nx] += e * k;
                    };
                    push(1, 0, 7.0f / 16.0f);
                    push(-1, 1, 3.0f / 16.0f);
                    push(0, 1, 5.0f / 16.0f);
                    push(1, 1, 1.0f / 16.0f);
                }
                glm::vec4 result(got, c.a);
                out.Pixels[i] = s >= 1.0f ? Pack(result) : Pack(glm::mix(Unpack(image.Pixels[i]), result, s));
            }
        return out;
    }
}
