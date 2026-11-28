#include "Dimension/Image/ImageFilters.h"
#include "Dimension/Image/ImageGame.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <thread>
#include <type_traits>

namespace Dimension::Imaging
{
    namespace
    {
        template <typename F>
        void ParallelRows(int rows, size_t pixelsPerRow, F&& body)
        {
            unsigned threads = std::max(1u, std::thread::hardware_concurrency());
            if (rows <= 0) return;
            if (threads == 1 || (size_t)rows * pixelsPerRow < 32 * 1024)
            {
                body(0, rows - 1);
                return;
            }
            threads = std::min<unsigned>(threads, (unsigned)rows);
            std::vector<std::thread> pool;
            int per = (rows + (int)threads - 1) / (int)threads;
            for (int start = 0; start < rows; start += per)
            {
                int end = std::min(rows - 1, start + per - 1);
                pool.emplace_back([&body, start, end] { body(start, end); });
            }
            for (auto& t : pool) t.join();
        }

        glm::vec4 Premul(uint32_t p)
        {
            glm::vec4 c = Unpack(p);
            return glm::vec4(glm::vec3(c) * c.a, c.a);
        }
        uint32_t Straight(const glm::vec4& c) { return c.a > 1e-6f ? Pack(glm::vec4(glm::clamp(glm::vec3(c) / c.a, 0.0f, 1.0f), c.a)) : 0u; }

        float ToLinear(float v) { return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f); }
        float ToSrgb(float v) { return v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(std::max(v, 0.0f), 1.0f / 2.4f) - 0.055f; }

        glm::vec3 ToHsl(const glm::vec3& c)
        {
            float mx = std::max({ c.r, c.g, c.b }), mn = std::min({ c.r, c.g, c.b });
            float l = (mx + mn) * 0.5f, d = mx - mn;
            if (d < 1e-6f) return { 0.0f, 0.0f, l };
            float s = l > 0.5f ? d / (2.0f - mx - mn) : d / (mx + mn);
            float h;
            if (mx == c.r) h = (c.g - c.b) / d + (c.g < c.b ? 6.0f : 0.0f);
            else if (mx == c.g) h = (c.b - c.r) / d + 2.0f;
            else h = (c.r - c.g) / d + 4.0f;
            return { h / 6.0f, s, l };
        }
        glm::vec3 FromHsl(const glm::vec3& hsl)
        {
            float h = hsl.x - std::floor(hsl.x), s = hsl.y, l = hsl.z;
            if (s <= 0.0f) return glm::vec3(l);
            float q = l < 0.5f ? l * (1.0f + s) : l + s - l * s, p = 2.0f * l - q;
            auto hue = [&](float t) {
                t -= std::floor(t);
                if (t < 1.0f / 6.0f) return p + (q - p) * 6.0f * t;
                if (t < 0.5f) return q;
                if (t < 2.0f / 3.0f) return p + (q - p) * (2.0f / 3.0f - t) * 6.0f;
                return p;
            };
            return { hue(h + 1.0f / 3.0f), hue(h), hue(h - 1.0f / 3.0f) };
        }

        // Box blur passes along rows then columns of any summable type (edges repeated).
        template <typename T>
        void BoxPass(std::vector<T>& v, int w, int h, int r)
        {
            if (r <= 0) return;
            std::vector<T> tmp(v.size());
            const float inv = 1.0f / float(2 * r + 1);
            ParallelRows(h, (size_t)w * 4, [&](int y0, int y1) {
                for (int y = y0; y <= y1; ++y)
                {
                    const T* row = v.data() + (size_t)y * w;
                    T sum = T(0);
                    for (int k = -r; k <= r; ++k) sum += row[std::clamp(k, 0, w - 1)];
                    for (int x = 0; x < w; ++x)
                    {
                        tmp[(size_t)y * w + x] = sum * inv;
                        sum += row[std::min(x + r + 1, w - 1)] - row[std::max(x - r, 0)];
                    }
                }
            });
            ParallelRows(w, (size_t)h * 4, [&](int x0, int x1) {
                for (int x = x0; x <= x1; ++x)
                {
                    T sum = T(0);
                    for (int k = -r; k <= r; ++k) sum += tmp[(size_t)std::clamp(k, 0, h - 1) * w + x];
                    for (int y = 0; y < h; ++y)
                    {
                        v[(size_t)y * w + x] = sum * inv;
                        sum += tmp[(size_t)std::min(y + r + 1, h - 1) * w + x] - tmp[(size_t)std::max(y - r, 0) * w + x];
                    }
                }
            });
        }
        BoxBlurHook s_BoxBlurHook = nullptr;

        // Three box passes: close to a Gaussian of `sigma`.
        template <typename T>
        void GaussPasses(std::vector<T>& v, int w, int h, float sigma)
        {
            if (sigma < 0.3f) return;
            float ideal = std::sqrt(12.0f * sigma * sigma / 3.0f + 1.0f);
            int r = std::max(1, (int)std::lround((ideal - 1.0f) * 0.5f));
            if constexpr (std::is_same_v<T, glm::vec4>)
                if (s_BoxBlurHook && (size_t)w * h >= 256 * 256 && s_BoxBlurHook(v, w, h, r, 3)) return;
            for (int k = 0; k < 3; ++k) BoxPass(v, w, h, r);
        }

        std::vector<glm::vec4> ToPremul(const Bitmap& b)
        {
            std::vector<glm::vec4> v(b.Pixels.size());
            for (size_t i = 0; i < v.size(); ++i) v[i] = Premul(b.Pixels[i]);
            return v;
        }

        // `filtered` (premultiplied) mixed into `image` by the selection.
        Bitmap Blend(const Bitmap& image, const std::vector<glm::vec4>& filtered, const Mask* sel)
        {
            Bitmap out = image;
            for (size_t i = 0; i < out.Pixels.size(); ++i)
            {
                float s = sel ? sel->Values[i] / 255.0f : 1.0f;
                if (s <= 0.0f) continue;
                out.Pixels[i] = Straight(s >= 1.0f ? filtered[i] : glm::mix(Premul(image.Pixels[i]), filtered[i], s));
            }
            return out;
        }

        glm::vec4 SampleBilinear(const std::vector<glm::vec4>& v, int w, int h, glm::vec2 p)
        {
            p -= 0.5f;
            int x0 = (int)std::floor(p.x), y0 = (int)std::floor(p.y);
            glm::vec2 t = p - glm::vec2(x0, y0);
            auto at = [&](int x, int y) { return v[(size_t)std::clamp(y, 0, h - 1) * w + std::clamp(x, 0, w - 1)]; };
            return glm::mix(glm::mix(at(x0, y0), at(x0 + 1, y0), t.x), glm::mix(at(x0, y0 + 1), at(x0 + 1, y0 + 1), t.x), t.y);
        }

        float Hash01(uint32_t x, uint32_t y, uint32_t seed)
        {
            uint32_t h = x * 374761393u + y * 668265263u + seed * 2246822519u;
            h = (h ^ (h >> 13)) * 1274126177u;
            h ^= h >> 16;
            return ((h & 0xFFFFFF) + 0.5f) / float(0x1000000);
        }

        void HashJson(uint64_t& h, const std::string& s)
        {
            for (unsigned char c : s) h = (h ^ c) * 1099511628211ull;
        }

        nlohmann::json Vec(const glm::vec3& v) { return nlohmann::json::array({ v.x, v.y, v.z }); }
        glm::vec3 Vec(const nlohmann::json& j, const glm::vec3& fallback)
        {
            if (!j.is_array() || j.size() < 3) return fallback;
            return { j[0].get<float>(), j[1].get<float>(), j[2].get<float>() };
        }
    }

    void SetBoxBlurHook(BoxBlurHook hook) { s_BoxBlurHook = hook; }

    const char* AdjustmentName(AdjustmentType type)
    {
        static constexpr const char* kNames[] = { "Brightness/Contrast", "Levels",      "Curves", "Exposure",  "Hue/Saturation", "Color Balance",
                                                  "Vibrance",            "Black & White", "Photo Filter", "Invert", "Posterize",     "Threshold",
                                                  "Gradient Map" };
        static_assert(std::size(kNames) == (size_t)AdjustmentType::Count);
        return (size_t)type < std::size(kNames) ? kNames[(size_t)type] : "Adjustment";
    }

    const char* FilterName(FilterType type)
    {
        static constexpr const char* kNames[] = { "Gaussian Blur", "Box Blur", "Motion Blur", "Unsharp Mask", "Sharpen", "Add Noise",
                                                  "Median",        "High Pass", "Mosaic",     "Emboss",       "Find Edges",
                                                  "Offset",        "Make Seamless", "Normal Map", "Reduce Colors" };
        static_assert(std::size(kNames) == (size_t)FilterType::Count);
        return (size_t)type < std::size(kNames) ? kNames[(size_t)type] : "Filter";
    }

    // ---- Adjustments

    float CurveAt(const std::vector<glm::vec2>& pts, float x)
    {
        if (pts.empty()) return x;
        if (pts.size() == 1 || x <= pts.front().x) return std::clamp(pts.front().y, 0.0f, 1.0f);
        if (x >= pts.back().x) return std::clamp(pts.back().y, 0.0f, 1.0f);
        // Fritsch-Carlson: slopes that keep each piece monotone.
        const size_t n = pts.size();
        std::vector<float> d(n - 1), m(n);
        for (size_t i = 0; i + 1 < n; ++i) d[i] = (pts[i + 1].y - pts[i].y) / std::max(pts[i + 1].x - pts[i].x, 1e-6f);
        m[0] = d[0];
        m[n - 1] = d[n - 2];
        for (size_t i = 1; i + 1 < n; ++i) m[i] = d[i - 1] * d[i] <= 0.0f ? 0.0f : (d[i - 1] + d[i]) * 0.5f;
        for (size_t i = 0; i + 1 < n; ++i)
        {
            if (std::abs(d[i]) < 1e-9f)
            {
                m[i] = m[i + 1] = 0.0f;
                continue;
            }
            float a = m[i] / d[i], b = m[i + 1] / d[i], s = a * a + b * b;
            if (s > 9.0f)
            {
                float t = 3.0f / std::sqrt(s);
                m[i] = t * a * d[i];
                m[i + 1] = t * b * d[i];
            }
        }
        size_t k = 0;
        while (k + 2 < n && x > pts[k + 1].x) ++k;
        float hgt = pts[k + 1].x - pts[k].x, t = (x - pts[k].x) / std::max(hgt, 1e-6f);
        float t2 = t * t, t3 = t2 * t;
        float y = (2 * t3 - 3 * t2 + 1) * pts[k].y + (t3 - 2 * t2 + t) * hgt * m[k] + (-2 * t3 + 3 * t2) * pts[k + 1].y + (t3 - t2) * hgt * m[k + 1];
        return std::clamp(y, 0.0f, 1.0f);
    }

    PreparedAdjustment Prepare(const Adjustment& a)
    {
        PreparedAdjustment p;
        p.Settings = &a;
        auto levels = [&](int ch, float v) {
            float range = std::max(a.InWhite[(size_t)ch] - a.InBlack[(size_t)ch], 1e-4f);
            v = std::clamp((v - a.InBlack[(size_t)ch]) / range, 0.0f, 1.0f);
            v = std::pow(v, 1.0f / std::max(a.Gamma[(size_t)ch], 0.01f));
            return a.OutBlack[(size_t)ch] + v * (a.OutWhite[(size_t)ch] - a.OutBlack[(size_t)ch]);
        };
        std::function<float(int, float)> f;
        switch (a.Type)
        {
        case AdjustmentType::BrightnessContrast:
        {
            float c = a.Contrast / 100.0f, factor = c >= 0.0f ? 1.0f + c * 2.0f : 1.0f + c;
            f = [=](int, float v) { return (v - 0.5f) * factor + 0.5f + a.Brightness / 250.0f; };
            break;
        }
        case AdjustmentType::Levels: f = [=](int ch, float v) { return levels(ch + 1, levels(0, v)); }; break;
        case AdjustmentType::Curves: f = [&](int ch, float v) { return CurveAt(a.Curve[(size_t)ch + 1], CurveAt(a.Curve[0], v)); }; break;
        case AdjustmentType::Exposure:
            f = [=](int, float v) {
                float l = ToLinear(v) * std::exp2(a.Exposure) + a.Offset;
                l = std::pow(std::max(l, 0.0f), 1.0f / std::max(a.ExposureGamma, 0.01f));
                return ToSrgb(l);
            };
            break;
        case AdjustmentType::Invert: f = [](int, float v) { return 1.0f - v; }; break;
        case AdjustmentType::Posterize:
        {
            float n = (float)std::max(a.Levels, 2) - 1.0f;
            f = [=](int, float v) { return std::round(v * n) / n; };
            break;
        }
        default: break;
        }
        if (f)
        {
            p.UsesLut = true;
            for (int ch = 0; ch < 3; ++ch)
                for (int i = 0; i < 256; ++i) p.Lut[(size_t)ch][(size_t)i] = std::clamp(f(ch, i / 255.0f), 0.0f, 1.0f);
        }
        return p;
    }

    glm::vec3 Adjust(const PreparedAdjustment& p, const glm::vec3& color)
    {
        const Adjustment& a = *p.Settings;
        glm::vec3 c = glm::clamp(color, 0.0f, 1.0f);
        if (p.UsesLut)
        {
            glm::vec3 out;
            for (int ch = 0; ch < 3; ++ch)
            {
                float x = c[ch] * 255.0f;
                int i = std::min((int)x, 254);
                out[ch] = glm::mix(p.Lut[(size_t)ch][(size_t)i], p.Lut[(size_t)ch][(size_t)i + 1], x - i);
            }
            return out;
        }
        const float l = Luminance(c);
        switch (a.Type)
        {
        case AdjustmentType::HueSaturation:
        {
            glm::vec3 hsl = ToHsl(c);
            if (a.Colorize) hsl = { a.Hue / 360.0f, std::clamp(a.Saturation / 100.0f, 0.0f, 1.0f), l };
            else
            {
                hsl.x += a.Hue / 360.0f;
                hsl.y = std::clamp(hsl.y * (1.0f + a.Saturation / 100.0f), 0.0f, 1.0f);
            }
            c = FromHsl(hsl);
            if (a.Lightness > 0.0f) c += (1.0f - c) * (a.Lightness / 100.0f);
            else if (a.Lightness < 0.0f) c *= 1.0f + a.Lightness / 100.0f;
            return glm::clamp(c, 0.0f, 1.0f);
        }
        case AdjustmentType::ColorBalance:
        {
            auto smooth = [](float e0, float e1, float x) {
                float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
                return t * t * (3.0f - 2.0f * t);
            };
            float ws = 1.0f - smooth(0.0f, 0.5f, l), wh = smooth(0.5f, 1.0f, l), wm = std::max(0.0f, 1.0f - ws - wh);
            glm::vec3 shift = (a.Balance[0] * ws + a.Balance[1] * wm + a.Balance[2] * wh) / 100.0f * 0.4f;
            c = glm::clamp(c + shift, 0.0f, 1.0f);
            if (a.PreserveLuminosity) c = glm::clamp(c + (l - Luminance(c)), 0.0f, 1.0f);
            return c;
        }
        case AdjustmentType::Vibrance:
        {
            float sat = std::max({ c.r, c.g, c.b }) - std::min({ c.r, c.g, c.b });
            c = glm::vec3(l) + (c - l) * (1.0f + a.Vibrance / 100.0f * (1.0f - sat));
            c = glm::vec3(l) + (c - l) * (1.0f + a.VibranceSaturation / 100.0f);
            return glm::clamp(c, 0.0f, 1.0f);
        }
        case AdjustmentType::BlackWhite:
        {
            // The gray from how much of each hue there is: min + (mid - min) x the secondary's weight + (max - mid) x the
            // primary's (red, yellow, green, cyan, blue, magenta weights).
            int idx[3] = { 0, 1, 2 };
            std::sort(idx, idx + 3, [&](int x, int y) { return c[x] > c[y]; });
            float mx = c[idx[0]], md = c[idx[1]], mn = c[idx[2]];
            static constexpr int kPrimary[3] = { 0, 2, 4 }; // red, green, blue
            int secondary = (idx[0] == 0 && idx[1] == 1) || (idx[0] == 1 && idx[1] == 0) ? 1  // yellow
                            : (idx[0] == 1 && idx[1] == 2) || (idx[0] == 2 && idx[1] == 1) ? 3 // cyan
                                                                                          : 5; // magenta
            float gray = mn + (md - mn) * a.Weights[(size_t)secondary] / 100.0f + (mx - md) * a.Weights[(size_t)kPrimary[idx[0]]] / 100.0f;
            gray = std::clamp(gray, 0.0f, 1.0f);
            if (!a.Tint) return glm::vec3(gray);
            glm::vec3 hsl = ToHsl(a.TintColor);
            return glm::clamp(FromHsl({ hsl.x, hsl.y, gray }), 0.0f, 1.0f);
        }
        case AdjustmentType::PhotoFilter:
        {
            glm::vec3 filtered = glm::mix(c, c * a.FilterColor * 1.6f, a.Density);
            if (a.PreserveLuminosity) filtered += l - Luminance(filtered);
            return glm::clamp(filtered, 0.0f, 1.0f);
        }
        case AdjustmentType::Threshold: return glm::vec3(l >= a.ThresholdLevel ? 1.0f : 0.0f);
        case AdjustmentType::GradientMap: return glm::mix(a.MapFrom, a.MapTo, l);
        default: return c;
        }
    }

    Bitmap ApplyAdjustment(const Bitmap& image, const Adjustment& adjustment, const Mask* sel)
    {
        PreparedAdjustment p = Prepare(adjustment);
        Bitmap out = image;
        ParallelRows((int)image.Height, image.Width, [&](int y0, int y1) {
            for (int y = y0; y <= y1; ++y)
                for (uint32_t x = 0; x < image.Width; ++x)
                {
                    size_t i = (size_t)y * image.Width + x;
                    float s = sel ? sel->Values[i] / 255.0f : 1.0f;
                    if (s <= 0.0f || (image.Pixels[i] >> 24) == 0) continue;
                    glm::vec4 c = Unpack(image.Pixels[i]);
                    glm::vec3 adjusted = Adjust(p, glm::vec3(c));
                    out.Pixels[i] = Pack(glm::vec4(glm::mix(glm::vec3(c), adjusted, s), c.a));
                }
        });
        return out;
    }

    std::array<uint32_t, 256> Histogram(const Bitmap& image, int channel)
    {
        std::array<uint32_t, 256> h{};
        for (uint32_t p : image.Pixels)
        {
            if ((p >> 24) == 0) continue;
            glm::vec4 c = Unpack(p);
            float v = channel == 0 ? Luminance(glm::vec3(c)) : c[channel - 1];
            h[(size_t)std::clamp((int)std::lround(v * 255.0f), 0, 255)]++;
        }
        return h;
    }

    // ---- Filters

    Bitmap GaussianBlur(const Bitmap& image, float radius)
    {
        std::vector<glm::vec4> v = ToPremul(image);
        GaussPasses(v, (int)image.Width, (int)image.Height, radius);
        Bitmap out = image;
        for (size_t i = 0; i < v.size(); ++i) out.Pixels[i] = Straight(v[i]);
        return out;
    }

    Bitmap ApplyFilter(const Bitmap& image, const FilterSettings& f, const Mask* sel)
    {
        const int w = (int)image.Width, h = (int)image.Height;
        if (w == 0 || h == 0) return image;
        // The game art tools (ImageGame.cpp), blended by the selection like the rest.
        auto blended = [&](const Bitmap& changed) { return Blend(image, ToPremul(changed), sel); };
        switch (f.Type)
        {
        case FilterType::Offset: return blended(OffsetWrap(image, f.Shift));
        case FilterType::MakeSeamless: return blended(MakeSeamless(image, std::clamp(f.Amount, 0.05f, 1.0f)));
        case FilterType::NormalMap: return blended(NormalMap(image, f.Amount, f.InvertY, f.Wrap));
        case FilterType::ReduceColors:
        {
            std::vector<glm::vec3> palette = f.Palette.empty() ? MedianCut(image, std::max(2, f.Colors)) : f.Palette;
            return Quantize(image, palette, (Dither)f.DitherKind, f.HardAlpha, sel);
        }
        default: break;
        }
        std::vector<glm::vec4> src = ToPremul(image), out = src;
        auto blurred = [&](float sigma) {
            std::vector<glm::vec4> b = src;
            GaussPasses(b, w, h, sigma);
            return b;
        };
        switch (f.Type)
        {
        case FilterType::GaussianBlur: out = blurred(f.Radius); break;
        case FilterType::BoxBlur: BoxPass(out, w, h, std::max(1, (int)std::lround(f.Radius))); break;
        case FilterType::MotionBlur:
        {
            const float a = glm::radians(f.Angle);
            const glm::vec2 dir(std::cos(a), -std::sin(a));
            const int taps = std::max(2, (int)std::lround(f.Distance));
            ParallelRows(h, (size_t)w * taps, [&](int y0, int y1) {
                for (int y = y0; y <= y1; ++y)
                    for (int x = 0; x < w; ++x)
                    {
                        glm::vec4 sum(0.0f);
                        for (int k = 0; k < taps; ++k)
                        {
                            float t = (k / float(taps - 1) - 0.5f) * f.Distance;
                            sum += SampleBilinear(src, w, h, glm::vec2(x + 0.5f, y + 0.5f) + dir * t);
                        }
                        out[(size_t)y * w + x] = sum / float(taps);
                    }
            });
            break;
        }
        case FilterType::UnsharpMask:
        case FilterType::Sharpen:
        {
            const float radius = f.Type == FilterType::Sharpen ? 1.0f : f.Radius;
            const float amount = f.Type == FilterType::Sharpen ? 0.6f : f.Amount;
            const float threshold = f.Type == FilterType::Sharpen ? 0.0f : f.Threshold / 255.0f;
            std::vector<glm::vec4> b = blurred(radius);
            for (size_t i = 0; i < out.size(); ++i)
            {
                glm::vec4 c = src[i], diff = c - b[i];
                if (std::max({ std::abs(diff.r), std::abs(diff.g), std::abs(diff.b) }) < threshold) continue;
                glm::vec3 rgb = glm::clamp(glm::vec3(c) + glm::vec3(diff) * amount, 0.0f, c.a);
                out[i] = glm::vec4(rgb, c.a);
            }
            break;
        }
        case FilterType::AddNoise:
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                    size_t i = (size_t)y * w + x;
                    glm::vec4 c = src[i];
                    if (c.a <= 0.0f) continue;
                    auto noise = [&](uint32_t channel) {
                        float u1 = Hash01((uint32_t)x, (uint32_t)y, f.Seed * 3 + channel), u2 = Hash01((uint32_t)x, (uint32_t)y, f.Seed * 3 + channel + 101);
                        if (!f.Gaussian) return (u1 - 0.5f) * 2.0f;
                        return std::sqrt(-2.0f * std::log(u1)) * std::cos(6.2831853f * u2) * 0.5f;
                    };
                    glm::vec3 n = f.Monochrome ? glm::vec3(noise(0)) : glm::vec3(noise(0), noise(1), noise(2));
                    glm::vec3 straight = glm::vec3(c) / c.a + n * (f.Amount * 0.25f);
                    out[i] = glm::vec4(glm::clamp(straight, 0.0f, 1.0f) * c.a, c.a);
                }
            break;
        case FilterType::Median:
        {
            const int r = std::clamp((int)std::lround(f.Radius), 1, 50);
            ParallelRows(h, (size_t)w * 256, [&](int y0, int y1) {
                std::array<std::array<int, 256>, 4> hist;
                for (int y = y0; y <= y1; ++y)
                {
                    for (auto& hh : hist) hh.fill(0);
                    auto addColumn = [&](int x, int sign) {
                        x = std::clamp(x, 0, w - 1);
                        for (int k = -r; k <= r; ++k)
                        {
                            uint32_t p = image.Pixels[(size_t)std::clamp(y + k, 0, h - 1) * w + x];
                            for (int ch = 0; ch < 4; ++ch) hist[(size_t)ch][(p >> (8 * ch)) & 0xFF] += sign;
                        }
                    };
                    for (int k = -r; k <= r; ++k) addColumn(k, 1);
                    const int half = (2 * r + 1) * (2 * r + 1) / 2;
                    for (int x = 0; x < w; ++x)
                    {
                        uint32_t px = 0;
                        for (int ch = 0; ch < 4; ++ch)
                        {
                            int count = 0, v = 0;
                            for (; v < 255; ++v)
                                if ((count += hist[(size_t)ch][(size_t)v]) > half) break;
                            px |= (uint32_t)v << (8 * ch);
                        }
                        out[(size_t)y * w + x] = Premul(px);
                        addColumn(x - r, -1);
                        addColumn(x + r + 1, 1);
                    }
                }
            });
            break;
        }
        case FilterType::HighPass:
        {
            std::vector<glm::vec4> b = blurred(f.Radius);
            for (size_t i = 0; i < out.size(); ++i)
            {
                glm::vec4 c = src[i];
                if (c.a <= 0.0f) continue;
                glm::vec3 cs = glm::vec3(c) / c.a, bs = b[i].a > 0.0f ? glm::vec3(b[i]) / b[i].a : cs;
                out[i] = glm::vec4(glm::clamp(cs - bs + 0.5f, 0.0f, 1.0f) * c.a, c.a);
            }
            break;
        }
        case FilterType::Mosaic:
        {
            const int cell = std::max(2, (int)std::lround(f.Radius));
            for (int cy = 0; cy < h; cy += cell)
                for (int cx = 0; cx < w; cx += cell)
                {
                    glm::vec4 sum(0.0f);
                    int n = 0;
                    for (int y = cy; y < std::min(h, cy + cell); ++y)
                        for (int x = cx; x < std::min(w, cx + cell); ++x) sum += src[(size_t)y * w + x], ++n;
                    sum /= (float)n;
                    for (int y = cy; y < std::min(h, cy + cell); ++y)
                        for (int x = cx; x < std::min(w, cx + cell); ++x) out[(size_t)y * w + x] = sum;
                }
            break;
        }
        case FilterType::Emboss:
        {
            const float a = glm::radians(f.Angle);
            const glm::vec2 o = glm::vec2(std::cos(a), -std::sin(a)) * std::max(1.0f, f.Distance);
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                    size_t i = (size_t)y * w + x;
                    glm::vec4 c = src[i];
                    if (c.a <= 0.0f) continue;
                    glm::vec2 p(x + 0.5f, y + 0.5f);
                    glm::vec4 lit = SampleBilinear(src, w, h, p + o), dark = SampleBilinear(src, w, h, p - o);
                    float v = 0.5f + (Luminance(glm::vec3(lit)) - Luminance(glm::vec3(dark))) * f.Amount;
                    out[i] = glm::vec4(glm::vec3(std::clamp(v, 0.0f, 1.0f)) * c.a, c.a);
                }
            break;
        }
        case FilterType::FindEdges:
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                    size_t i = (size_t)y * w + x;
                    glm::vec4 c = src[i];
                    if (c.a <= 0.0f) continue;
                    auto at = [&](int dx, int dy) {
                        glm::vec4 s = src[(size_t)std::clamp(y + dy, 0, h - 1) * w + std::clamp(x + dx, 0, w - 1)];
                        return s.a > 0.0f ? glm::vec3(s) / s.a : glm::vec3(1.0f);
                    };
                    glm::vec3 gx = at(1, -1) + 2.0f * at(1, 0) + at(1, 1) - at(-1, -1) - 2.0f * at(-1, 0) - at(-1, 1);
                    glm::vec3 gy = at(-1, 1) + 2.0f * at(0, 1) + at(1, 1) - at(-1, -1) - 2.0f * at(0, -1) - at(1, -1);
                    glm::vec3 mag = glm::sqrt(gx * gx + gy * gy) * 0.5f;
                    out[i] = glm::vec4(glm::clamp(1.0f - mag, 0.0f, 1.0f) * c.a, c.a);
                }
            break;
        default: break;
        }
        return Blend(image, out, sel);
    }

    Mask ApplyToMask(const Mask& mask, const std::function<Bitmap(const Bitmap&)>& change)
    {
        Bitmap gray = Bitmap::Solid(mask.Width, mask.Height, 0u);
        for (size_t i = 0; i < mask.Values.size(); ++i)
        {
            uint32_t v = mask.Values[i];
            gray.Pixels[i] = v | (v << 8) | (v << 16) | 0xFF000000u;
        }
        Bitmap changed = change(gray);
        Mask out = mask;
        for (size_t i = 0; i < out.Values.size() && i < changed.Pixels.size(); ++i)
            out.Values[i] = (uint8_t)std::lround(Luminance(glm::vec3(Unpack(changed.Pixels[i]))) * 255.0f);
        return out;
    }

    // ---- Layer effects

    bool LayerEffects::Any() const
    {
        return DropShadow.Enabled || InnerShadow.Enabled || OuterGlow.Enabled || InnerGlow.Enabled || Stroke.Enabled || ColorOverlay.Enabled ||
               GradientOverlay.Enabled;
    }

    uint64_t LayerEffects::Hash() const
    {
        uint64_t h = 1469598103934665603ull;
        HashJson(h, ToJson(*this).dump());
        return h;
    }

    std::vector<float> DistanceField(const std::vector<uint8_t>& inside, int w, int h)
    {
        // Felzenszwalb and Huttenlocher's squared distance transform, by rows then columns.
        const float inf = 1e20f;
        std::vector<float> d(inside.size());
        for (size_t i = 0; i < d.size(); ++i) d[i] = inside[i] ? 0.0f : inf;
        auto transform = [&](std::vector<float>& f, int n) {
            std::vector<float> out((size_t)n);
            std::vector<double> z((size_t)n + 1);
            std::vector<int> v((size_t)n);
            auto meet = [&](int q, int p) {
                return (((double)f[(size_t)q] + (double)q * q) - ((double)f[(size_t)p] + (double)p * p)) / (2.0 * (q - p));
            };
            int k = 0;
            v[0] = 0;
            z[0] = -1e30;
            z[1] = 1e30;
            for (int q = 1; q < n; ++q)
            {
                double s = meet(q, v[(size_t)k]);
                while (s <= z[(size_t)k])
                {
                    --k;
                    s = meet(q, v[(size_t)k]);
                }
                ++k;
                v[(size_t)k] = q;
                z[(size_t)k] = s;
                z[(size_t)k + 1] = 1e30;
            }
            k = 0;
            for (int q = 0; q < n; ++q)
            {
                while (z[(size_t)k + 1] < q) ++k;
                float dq = (float)(q - v[(size_t)k]);
                out[(size_t)q] = std::min(dq * dq + f[(size_t)v[(size_t)k]], inf);
            }
            f = std::move(out);
        };
        ParallelRows(h, (size_t)w * 8, [&](int y0, int y1) {
            std::vector<float> row((size_t)w);
            for (int y = y0; y <= y1; ++y)
            {
                std::copy(d.begin() + (ptrdiff_t)y * w, d.begin() + (ptrdiff_t)(y + 1) * w, row.begin());
                transform(row, w);
                std::copy(row.begin(), row.end(), d.begin() + (ptrdiff_t)y * w);
            }
        });
        ParallelRows(w, (size_t)h * 8, [&](int x0, int x1) {
            std::vector<float> col((size_t)h);
            for (int x = x0; x <= x1; ++x)
            {
                for (int y = 0; y < h; ++y) col[(size_t)y] = d[(size_t)y * w + x];
                transform(col, h);
                for (int y = 0; y < h; ++y) d[(size_t)y * w + x] = col[(size_t)y];
            }
        });
        return d;
    }

    std::shared_ptr<const StyledLayer> RenderEffects(const Layer& layer, uint32_t width, uint32_t height)
    {
        auto out = std::make_shared<StyledLayer>();
        out->Pixels = layer.Pixels;
        if (layer.MaskEnabled) out->MaskKey = layer.LayerMask;
        out->Key = layer.Effects.Hash();
        if (!layer.IsRaster() || !layer.Pixels) return out;
        const int w = (int)width, h = (int)height;
        const size_t n = (size_t)w * h;
        const Bitmap& px = layer.Get();
        const LayerEffects& fx = layer.Effects;
        // The content's alpha (the mask counts: effects follow what shows).
        std::vector<float> A(n);
        for (size_t i = 0; i < n; ++i)
            A[i] = (px.Pixels[i] >> 24) / 255.0f * (layer.LayerMask && layer.MaskEnabled ? layer.LayerMask->Values[i] / 255.0f : 1.0f);
        auto distanceTo = [&](const std::vector<float>& a, bool toInside) {
            std::vector<uint8_t> set(n);
            for (size_t i = 0; i < n; ++i) set[i] = toInside ? a[i] >= 0.5f : a[i] < 0.5f;
            return DistanceField(set, w, h);
        };
        // Grown by r pixels (round corners), antialiased; shrunk likewise.
        auto grow = [&](const std::vector<float>& a, float r) {
            if (r <= 0.0f) return a;
            std::vector<float> d = distanceTo(a, true), g(n);
            for (size_t i = 0; i < n; ++i) g[i] = std::max(a[i], std::clamp(r + 0.5f - std::sqrt(d[i]), 0.0f, 1.0f));
            return g;
        };
        auto shrink = [&](const std::vector<float>& a, float r) {
            if (r <= 0.0f) return a;
            std::vector<float> d = distanceTo(a, false), g(n);
            for (size_t i = 0; i < n; ++i) g[i] = std::min(a[i], std::clamp(std::sqrt(d[i]) - r + 0.5f, 0.0f, 1.0f));
            return g;
        };
        auto blur = [&](std::vector<float> a, float size) {
            GaussPasses(a, w, h, size * 0.4f);
            return a;
        };
        auto shift = [&](const std::vector<float>& a, float dx, float dy, float outside) {
            std::vector<float> s(n, outside);
            int ix = (int)std::lround(dx), iy = (int)std::lround(dy);
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                    int sx = x - ix, sy = y - iy;
                    if (sx >= 0 && sy >= 0 && sx < w && sy < h) s[(size_t)y * w + x] = a[(size_t)sy * w + sx];
                }
            return s;
        };
        auto alpha8 = [&](const std::vector<float>& a) {
            std::vector<uint8_t> v(n);
            for (size_t i = 0; i < n; ++i) v[i] = (uint8_t)std::lround(std::clamp(a[i], 0.0f, 1.0f) * 255.0f);
            return v;
        };
        auto offset = [](const ShadowEffect& s) {
            float a = glm::radians(s.Angle);
            return glm::vec2(-std::cos(a), std::sin(a)) * s.Distance; // (away from the light; screen y points down)
        };

        // Under the content, bottom first.
        if (fx.DropShadow.Enabled)
        {
            const ShadowEffect& s = fx.DropShadow;
            std::vector<float> a = blur(grow(A, s.Spread * s.Size), s.Size * (1.0f - s.Spread));
            glm::vec2 o = offset(s);
            out->Below.push_back({ s.Color, s.Mode, s.Opacity, alpha8(shift(a, o.x, o.y, 0.0f)) });
        }
        if (fx.OuterGlow.Enabled)
        {
            const GlowEffect& g = fx.OuterGlow;
            out->Below.push_back({ g.Color, g.Mode, g.Opacity, alpha8(blur(grow(A, g.Spread * g.Size), g.Size * (1.0f - g.Spread))) });
        }
        if (fx.Stroke.Enabled && fx.Stroke.Position != 1)
        {
            const StrokeEffect& s = fx.Stroke;
            out->Below.push_back({ s.Color, LayerBlend::Normal, s.Opacity, alpha8(grow(A, s.Position == 2 ? s.Size * 0.5f : s.Size)) });
        }

        // On the content: gradient overlay, color overlay, inner glow, inner shadow, the inside of the stroke.
        std::vector<float> innerGlow, innerShadow, insideStroke;
        if (fx.InnerGlow.Enabled)
        {
            const GlowEffect& g = fx.InnerGlow;
            std::vector<float> core = blur(shrink(A, g.Spread * g.Size), g.Size * (1.0f - g.Spread));
            innerGlow.resize(n);
            for (size_t i = 0; i < n; ++i) innerGlow[i] = std::clamp(1.0f - core[i], 0.0f, 1.0f);
        }
        if (fx.InnerShadow.Enabled)
        {
            const ShadowEffect& s = fx.InnerShadow;
            std::vector<float> outside(n);
            for (size_t i = 0; i < n; ++i) outside[i] = 1.0f - A[i];
            glm::vec2 o = offset(s);
            innerShadow = blur(grow(shift(outside, o.x, o.y, 1.0f), s.Spread * s.Size), s.Size * (1.0f - s.Spread));
        }
        if (fx.Stroke.Enabled && fx.Stroke.Position != 0)
        {
            std::vector<float> core = shrink(A, fx.Stroke.Position == 2 ? fx.Stroke.Size * 0.5f : fx.Stroke.Size);
            insideStroke.resize(n);
            for (size_t i = 0; i < n; ++i) insideStroke[i] = A[i] > 0.0f ? std::clamp(1.0f - core[i] / A[i], 0.0f, 1.0f) : 0.0f;
        }
        Rect bounds;
        if (fx.GradientOverlay.Enabled)
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                    if (A[(size_t)y * w + x] > 0.0f) bounds.Add(x, y);
        const glm::vec2 center = bounds.Empty() ? glm::vec2(0.0f) : glm::vec2((bounds.X0 + bounds.X1 + 1) * 0.5f, (bounds.Y0 + bounds.Y1 + 1) * 0.5f);
        const float ga = glm::radians(fx.GradientOverlay.Angle);
        const glm::vec2 gdir(std::cos(ga), -std::sin(ga));
        const float extent = bounds.Empty() ? 1.0f
                                            : std::max(1.0f, std::abs((bounds.X1 - bounds.X0 + 1) * gdir.x) + std::abs((bounds.Y1 - bounds.Y0 + 1) * gdir.y));
        auto lay = [](glm::vec3 cb, const glm::vec3& color, LayerBlend mode, float amount) {
            return glm::mix(cb, glm::clamp(BlendColors(mode, cb, color), 0.0f, 1.0f), std::clamp(amount, 0.0f, 1.0f));
        };
        out->Content = px;
        ParallelRows(h, (size_t)w * 4, [&](int y0, int y1) {
            for (int y = y0; y <= y1; ++y)
                for (int x = 0; x < w; ++x)
                {
                    size_t i = (size_t)y * w + x;
                    uint32_t p = px.Pixels[i];
                    if ((p >> 24) == 0) continue;
                    glm::vec4 c = Unpack(p);
                    glm::vec3 cb(c);
                    if (fx.GradientOverlay.Enabled)
                    {
                        const GradientEffect& g = fx.GradientOverlay;
                        float t = std::clamp(0.5f + glm::dot(glm::vec2(x + 0.5f, y + 0.5f) - center, gdir) / extent, 0.0f, 1.0f);
                        if (g.Reverse) t = 1.0f - t;
                        cb = lay(cb, glm::mix(g.From, g.To, t), g.Mode, g.Opacity);
                    }
                    if (fx.ColorOverlay.Enabled) cb = lay(cb, fx.ColorOverlay.Color, fx.ColorOverlay.Mode, fx.ColorOverlay.Opacity);
                    if (!innerGlow.empty()) cb = lay(cb, fx.InnerGlow.Color, fx.InnerGlow.Mode, fx.InnerGlow.Opacity * innerGlow[i]);
                    if (!innerShadow.empty()) cb = lay(cb, fx.InnerShadow.Color, fx.InnerShadow.Mode, fx.InnerShadow.Opacity * innerShadow[i]);
                    if (!insideStroke.empty()) cb = lay(cb, fx.Stroke.Color, LayerBlend::Normal, fx.Stroke.Opacity * insideStroke[i]);
                    out->Content.Pixels[i] = Pack(glm::vec4(cb, c.a));
                }
        });
        return out;
    }

    // ---- The file

    nlohmann::json ToJson(const Adjustment& a)
    {
        nlohmann::json j;
        j["type"] = AdjustmentName(a.Type);
        switch (a.Type)
        {
        case AdjustmentType::BrightnessContrast: j["brightness"] = a.Brightness, j["contrast"] = a.Contrast; break;
        case AdjustmentType::Levels:
            j["inBlack"] = a.InBlack, j["inWhite"] = a.InWhite, j["gamma"] = a.Gamma, j["outBlack"] = a.OutBlack, j["outWhite"] = a.OutWhite;
            break;
        case AdjustmentType::Curves:
        {
            auto& curves = j["curves"] = nlohmann::json::array();
            for (const auto& c : a.Curve)
            {
                nlohmann::json pts = nlohmann::json::array();
                for (const glm::vec2& p : c) pts.push_back({ p.x, p.y });
                curves.push_back(pts);
            }
            break;
        }
        case AdjustmentType::Exposure: j["exposure"] = a.Exposure, j["offset"] = a.Offset, j["gamma"] = a.ExposureGamma; break;
        case AdjustmentType::HueSaturation: j["hue"] = a.Hue, j["saturation"] = a.Saturation, j["lightness"] = a.Lightness, j["colorize"] = a.Colorize; break;
        case AdjustmentType::ColorBalance:
            j["shadows"] = Vec(a.Balance[0]), j["midtones"] = Vec(a.Balance[1]), j["highlights"] = Vec(a.Balance[2]);
            j["preserveLuminosity"] = a.PreserveLuminosity;
            break;
        case AdjustmentType::Vibrance: j["vibrance"] = a.Vibrance, j["saturation"] = a.VibranceSaturation; break;
        case AdjustmentType::BlackWhite: j["weights"] = a.Weights, j["tint"] = a.Tint, j["tintColor"] = Vec(a.TintColor); break;
        case AdjustmentType::PhotoFilter: j["color"] = Vec(a.FilterColor), j["density"] = a.Density, j["preserveLuminosity"] = a.PreserveLuminosity; break;
        case AdjustmentType::Posterize: j["levels"] = a.Levels; break;
        case AdjustmentType::Threshold: j["level"] = a.ThresholdLevel; break;
        case AdjustmentType::GradientMap: j["from"] = Vec(a.MapFrom), j["to"] = Vec(a.MapTo); break;
        default: break;
        }
        return j;
    }

    Adjustment AdjustmentFromJson(const nlohmann::json& j)
    {
        Adjustment a;
        std::string type = j.value("type", std::string());
        for (int t = 0; t < (int)AdjustmentType::Count; ++t)
            if (type == AdjustmentName((AdjustmentType)t)) a.Type = (AdjustmentType)t;
        auto arr4 = [&](const char* key, std::array<float, 4>& v) {
            if (j.contains(key) && j[key].is_array() && j[key].size() == 4)
                for (size_t i = 0; i < 4; ++i) v[i] = j[key][i].get<float>();
        };
        a.Brightness = j.value("brightness", 0.0f);
        a.Contrast = j.value("contrast", 0.0f);
        arr4("inBlack", a.InBlack);
        arr4("inWhite", a.InWhite);
        arr4("outBlack", a.OutBlack);
        arr4("outWhite", a.OutWhite);
        if (a.Type == AdjustmentType::Levels) arr4("gamma", a.Gamma);
        if (j.contains("curves") && j["curves"].is_array())
            for (size_t c = 0; c < std::min<size_t>(4, j["curves"].size()); ++c)
            {
                std::vector<glm::vec2> pts;
                for (const auto& p : j["curves"][c])
                    if (p.is_array() && p.size() == 2) pts.push_back({ p[0].get<float>(), p[1].get<float>() });
                if (pts.size() >= 2) a.Curve[c] = pts;
            }
        a.Exposure = j.value("exposure", 0.0f);
        a.Offset = j.value("offset", 0.0f);
        if (a.Type == AdjustmentType::Exposure) a.ExposureGamma = j.value("gamma", 1.0f);
        a.Hue = j.value("hue", 0.0f);
        a.Saturation = j.value("saturation", 0.0f);
        a.Lightness = j.value("lightness", 0.0f);
        a.Colorize = j.value("colorize", false);
        if (j.contains("shadows")) a.Balance[0] = Vec(j["shadows"], glm::vec3(0.0f));
        if (j.contains("midtones")) a.Balance[1] = Vec(j["midtones"], glm::vec3(0.0f));
        if (j.contains("highlights")) a.Balance[2] = Vec(j["highlights"], glm::vec3(0.0f));
        a.PreserveLuminosity = j.value("preserveLuminosity", true);
        a.Vibrance = j.value("vibrance", 0.0f);
        if (a.Type == AdjustmentType::Vibrance) a.VibranceSaturation = j.value("saturation", 0.0f);
        if (j.contains("weights") && j["weights"].is_array() && j["weights"].size() == 6)
            for (size_t i = 0; i < 6; ++i) a.Weights[i] = j["weights"][i].get<float>();
        a.Tint = j.value("tint", false);
        if (j.contains("tintColor")) a.TintColor = Vec(j["tintColor"], a.TintColor);
        if (j.contains("color")) a.FilterColor = Vec(j["color"], a.FilterColor);
        a.Density = j.value("density", 0.25f);
        a.Levels = j.value("levels", 4);
        a.ThresholdLevel = j.value("level", 0.5f);
        if (j.contains("from")) a.MapFrom = Vec(j["from"], a.MapFrom);
        if (j.contains("to")) a.MapTo = Vec(j["to"], a.MapTo);
        return a;
    }

    namespace
    {
        nlohmann::json ShadowJson(const ShadowEffect& s)
        {
            return { { "enabled", s.Enabled },     { "color", Vec(s.Color) }, { "opacity", s.Opacity }, { "mode", LayerBlendName(s.Mode) },
                     { "angle", s.Angle },         { "distance", s.Distance }, { "spread", s.Spread },   { "size", s.Size } };
        }
        ShadowEffect ShadowFrom(const nlohmann::json& j, ShadowEffect s)
        {
            if (!j.is_object()) return s;
            s.Enabled = j.value("enabled", false);
            s.Color = Vec(j.value("color", nlohmann::json()), s.Color);
            s.Opacity = j.value("opacity", s.Opacity);
            s.Mode = LayerBlendFromName(j.value("mode", std::string(LayerBlendName(s.Mode))));
            s.Angle = j.value("angle", s.Angle);
            s.Distance = j.value("distance", s.Distance);
            s.Spread = j.value("spread", s.Spread);
            s.Size = j.value("size", s.Size);
            return s;
        }
        nlohmann::json GlowJson(const GlowEffect& g)
        {
            return { { "enabled", g.Enabled }, { "color", Vec(g.Color) }, { "opacity", g.Opacity }, { "mode", LayerBlendName(g.Mode) },
                     { "spread", g.Spread },   { "size", g.Size } };
        }
        GlowEffect GlowFrom(const nlohmann::json& j, GlowEffect g)
        {
            if (!j.is_object()) return g;
            g.Enabled = j.value("enabled", false);
            g.Color = Vec(j.value("color", nlohmann::json()), g.Color);
            g.Opacity = j.value("opacity", g.Opacity);
            g.Mode = LayerBlendFromName(j.value("mode", std::string(LayerBlendName(g.Mode))));
            g.Spread = j.value("spread", g.Spread);
            g.Size = j.value("size", g.Size);
            return g;
        }
    }

    nlohmann::json ToJson(const LayerEffects& e)
    {
        nlohmann::json j;
        j["dropShadow"] = ShadowJson(e.DropShadow);
        j["innerShadow"] = ShadowJson(e.InnerShadow);
        j["outerGlow"] = GlowJson(e.OuterGlow);
        j["innerGlow"] = GlowJson(e.InnerGlow);
        j["stroke"] = { { "enabled", e.Stroke.Enabled }, { "color", Vec(e.Stroke.Color) }, { "opacity", e.Stroke.Opacity }, { "size", e.Stroke.Size },
                        { "position", e.Stroke.Position } };
        j["colorOverlay"] = { { "enabled", e.ColorOverlay.Enabled }, { "color", Vec(e.ColorOverlay.Color) }, { "opacity", e.ColorOverlay.Opacity },
                              { "mode", LayerBlendName(e.ColorOverlay.Mode) } };
        const GradientEffect& g = e.GradientOverlay;
        j["gradientOverlay"] = { { "enabled", g.Enabled }, { "from", Vec(g.From) }, { "to", Vec(g.To) }, { "opacity", g.Opacity },
                                 { "mode", LayerBlendName(g.Mode) }, { "angle", g.Angle }, { "reverse", g.Reverse } };
        return j;
    }

    LayerEffects EffectsFromJson(const nlohmann::json& j)
    {
        LayerEffects e;
        if (!j.is_object()) return e;
        e.DropShadow = ShadowFrom(j.value("dropShadow", nlohmann::json()), e.DropShadow);
        e.InnerShadow = ShadowFrom(j.value("innerShadow", nlohmann::json()), e.InnerShadow);
        e.OuterGlow = GlowFrom(j.value("outerGlow", nlohmann::json()), e.OuterGlow);
        e.InnerGlow = GlowFrom(j.value("innerGlow", nlohmann::json()), e.InnerGlow);
        if (auto s = j.value("stroke", nlohmann::json()); s.is_object())
        {
            e.Stroke.Enabled = s.value("enabled", false);
            e.Stroke.Color = Vec(s.value("color", nlohmann::json()), e.Stroke.Color);
            e.Stroke.Opacity = s.value("opacity", 1.0f);
            e.Stroke.Size = s.value("size", 3.0f);
            e.Stroke.Position = s.value("position", 0);
        }
        if (auto c = j.value("colorOverlay", nlohmann::json()); c.is_object())
        {
            e.ColorOverlay.Enabled = c.value("enabled", false);
            e.ColorOverlay.Color = Vec(c.value("color", nlohmann::json()), e.ColorOverlay.Color);
            e.ColorOverlay.Opacity = c.value("opacity", 1.0f);
            e.ColorOverlay.Mode = LayerBlendFromName(c.value("mode", std::string("Normal")));
        }
        if (auto g = j.value("gradientOverlay", nlohmann::json()); g.is_object())
        {
            GradientEffect& o = e.GradientOverlay;
            o.Enabled = g.value("enabled", false);
            o.From = Vec(g.value("from", nlohmann::json()), o.From);
            o.To = Vec(g.value("to", nlohmann::json()), o.To);
            o.Opacity = g.value("opacity", 1.0f);
            o.Mode = LayerBlendFromName(g.value("mode", std::string("Normal")));
            o.Angle = g.value("angle", 90.0f);
            o.Reverse = g.value("reverse", false);
        }
        return e;
    }
}
