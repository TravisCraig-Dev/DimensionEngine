#include "Dimension/Image/ImageEdit.h"
#include "Dimension/Image/ImagePaint.h"
#include <algorithm>
#include <cmath>
#include <deque>
#include <thread>

namespace Dimension::Imaging
{
    namespace
    {
        template <typename F>
        void ParallelRows(int rows, size_t pixelsPerRow, F&& body)
        {
            unsigned threads = std::max(1u, std::thread::hardware_concurrency());
            if (threads == 1 || (size_t)rows * pixelsPerRow < 64 * 1024)
            {
                body(0, rows - 1);
                return;
            }
            threads = std::min<unsigned>(threads, (unsigned)std::max(rows, 1));
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
        uint32_t Straight(const glm::vec4& c) { return c.a > 1e-6f ? Pack(glm::vec4(glm::vec3(c) / c.a, c.a)) : 0u; }

        // A blur of float values (w x h), three box passes (close to a Gaussian), edges clamped.
        void BoxBlur(std::vector<float>& v, int w, int h, int r)
        {
            if (r <= 0) return;
            std::vector<float> tmp(v.size());
            auto pass = [&](std::vector<float>& in, std::vector<float>& out, bool horizontal) {
                const int n = horizontal ? w : h, lines = horizontal ? h : w;
                for (int line = 0; line < lines; ++line)
                {
                    auto at = [&](int i) -> float& {
                        i = std::clamp(i, 0, n - 1);
                        return horizontal ? in[(size_t)line * w + i] : in[(size_t)i * w + line];
                    };
                    float sum = 0.0f;
                    for (int k = -r; k <= r; ++k) sum += at(k);
                    for (int i = 0; i < n; ++i)
                    {
                        (horizontal ? out[(size_t)line * w + i] : out[(size_t)i * w + line]) = sum / float(2 * r + 1);
                        sum += at(i + r + 1) - at(i - r);
                    }
                }
            };
            for (int k = 0; k < 3; ++k)
            {
                pass(v, tmp, true);
                pass(tmp, v, false);
            }
        }

        // The largest (or smallest) value within `r` along rows, then columns (a square window).
        void MaxFilter(std::vector<uint8_t>& v, int w, int h, int r, bool grow)
        {
            if (r <= 0) return;
            std::vector<uint8_t> tmp(v.size());
            auto pass = [&](const std::vector<uint8_t>& in, std::vector<uint8_t>& out, bool horizontal) {
                const int n = horizontal ? w : h, lines = horizontal ? h : w;
                std::deque<int> q;
                for (int line = 0; line < lines; ++line)
                {
                    auto at = [&](int i) { return horizontal ? in[(size_t)line * w + i] : in[(size_t)i * w + line]; };
                    auto better = [&](uint8_t a, uint8_t b) { return grow ? a >= b : a <= b; };
                    q.clear();
                    int next = 0;
                    for (int i = 0; i < n; ++i)
                    {
                        for (; next <= std::min(n - 1, i + r); ++next)
                        {
                            while (!q.empty() && better(at(next), at(q.back()))) q.pop_back();
                            q.push_back(next);
                        }
                        while (q.front() < i - r) q.pop_front();
                        (horizontal ? out[(size_t)line * w + i] : out[(size_t)i * w + line]) = at(q.front());
                    }
                }
            };
            pass(v, tmp, true);
            pass(tmp, v, false);
        }

        // The rows' even-odd coverage of a polygon: `sub` rows per pixel, exact across.
        Mask Rasterize(uint32_t width, uint32_t height, const std::vector<glm::vec2>& pts, bool antialias)
        {
            Mask m = Mask::Solid(width, height, 0);
            if (pts.size() < 3) return m;
            const int sub = antialias ? 4 : 1;
            float minY = pts[0].y, maxY = pts[0].y;
            for (const glm::vec2& p : pts) minY = std::min(minY, p.y), maxY = std::max(maxY, p.y);
            int y0 = std::max(0, (int)std::floor(minY)), y1 = std::min((int)height - 1, (int)std::ceil(maxY));
            if (y0 > y1) return m;
            ParallelRows(y1 - y0 + 1, width, [&](int r0, int r1) {
                std::vector<float> cov(width);
                std::vector<float> xs;
                for (int y = y0 + r0; y <= y0 + r1; ++y)
                {
                    std::fill(cov.begin(), cov.end(), 0.0f);
                    for (int k = 0; k < sub; ++k)
                    {
                        float sy = y + (k + 0.5f) / sub;
                        xs.clear();
                        for (size_t i = 0; i < pts.size(); ++i)
                        {
                            glm::vec2 a = pts[i], b = pts[(i + 1) % pts.size()];
                            if ((a.y <= sy) == (b.y <= sy)) continue;
                            xs.push_back(a.x + (sy - a.y) / (b.y - a.y) * (b.x - a.x));
                        }
                        std::sort(xs.begin(), xs.end());
                        for (size_t i = 0; i + 1 < xs.size(); i += 2)
                        {
                            float xa = std::max(0.0f, xs[i]), xb = std::min((float)width, xs[i + 1]);
                            if (!antialias)
                            {
                                // Whole pixels whose centers are inside.
                                int a = (int)std::ceil(xa - 0.5f), b = (int)std::floor(xb - 0.5f);
                                for (int x = std::max(a, 0); x <= std::min(b, (int)width - 1); ++x) cov[(size_t)x] = 1.0f;
                                continue;
                            }
                            for (int x = (int)std::floor(xa); x < (int)std::ceil(xb) && x < (int)width; ++x)
                                cov[(size_t)x] += std::max(0.0f, std::min(xb, x + 1.0f) - std::max(xa, (float)x)) / sub;
                        }
                    }
                    for (uint32_t x = 0; x < width; ++x)
                        m.Values[(size_t)y * width + x] = (uint8_t)std::lround(std::clamp(cov[x], 0.0f, 1.0f) * 255.0f);
                }
            });
            return m;
        }

        // One axis of a resample: each output sample a tent-weighted average of the inputs around its center (the tent
        // as wide as one output sample when shrinking, so every input counts).
        struct Taps
        {
            std::vector<int> Start;
            std::vector<std::vector<float>> Weights;
        };
        Taps MakeTaps(int in, int out, Resample method)
        {
            Taps t;
            t.Start.resize((size_t)out);
            t.Weights.resize((size_t)out);
            const float scale = (float)in / out;
            for (int i = 0; i < out; ++i)
            {
                float c = (i + 0.5f) * scale - 0.5f;
                if (method == Resample::Nearest)
                {
                    t.Start[(size_t)i] = std::clamp((int)std::floor((i + 0.5f) * scale), 0, in - 1);
                    t.Weights[(size_t)i] = { 1.0f };
                    continue;
                }
                float r = std::max(1.0f, scale);
                int a = (int)std::floor(c - r) + 1, b = (int)std::ceil(c + r) - 1;
                float sum = 0.0f;
                std::vector<float> w;
                for (int x = a; x <= b; ++x)
                {
                    float wt = std::max(0.0f, 1.0f - std::abs(x - c) / r);
                    w.push_back(wt);
                    sum += wt;
                }
                for (float& v : w) v /= std::max(sum, 1e-6f);
                t.Start[(size_t)i] = a;
                t.Weights[(size_t)i] = std::move(w);
            }
            return t;
        }

        template <typename T, typename Get, typename Put>
        void Resample2D(int inW, int inH, int outW, int outH, Resample method, Get get, Put put)
        {
            Taps tx = MakeTaps(inW, outW, method), ty = MakeTaps(inH, outH, method);
            std::vector<T> mid((size_t)outW * inH);
            for (int y = 0; y < inH; ++y)
                for (int x = 0; x < outW; ++x)
                {
                    T sum = T(0);
                    const auto& w = tx.Weights[(size_t)x];
                    for (size_t k = 0; k < w.size(); ++k) sum += get(std::clamp(tx.Start[(size_t)x] + (int)k, 0, inW - 1), y) * w[k];
                    mid[(size_t)y * outW + x] = sum;
                }
            for (int y = 0; y < outH; ++y)
                for (int x = 0; x < outW; ++x)
                {
                    T sum = T(0);
                    const auto& w = ty.Weights[(size_t)y];
                    for (size_t k = 0; k < w.size(); ++k) sum += mid[(size_t)std::clamp(ty.Start[(size_t)y] + (int)k, 0, inH - 1) * outW + x] * w[k];
                    put(x, y, sum);
                }
        }
    }

    // ---- Selections

    Mask SelectRect(uint32_t width, uint32_t height, const glm::vec2& a, const glm::vec2& b, bool ellipse, bool antialias)
    {
        glm::vec2 lo = glm::min(a, b), hi = glm::max(a, b);
        if (ellipse)
        {
            std::vector<glm::vec2> pts;
            glm::vec2 c = (lo + hi) * 0.5f, r = (hi - lo) * 0.5f;
            const int n = std::clamp((int)((r.x + r.y) * 1.5f), 32, 1024);
            for (int i = 0; i < n; ++i)
            {
                float t = i * 6.2831853f / n;
                pts.push_back(c + glm::vec2(std::cos(t), std::sin(t)) * r);
            }
            return Rasterize(width, height, pts, antialias);
        }
        if (!antialias) lo = glm::round(lo), hi = glm::round(hi);
        return Rasterize(width, height, { lo, { hi.x, lo.y }, hi, { lo.x, hi.y } }, antialias);
    }

    Mask SelectPolygon(uint32_t width, uint32_t height, const std::vector<glm::vec2>& points, bool antialias)
    {
        return Rasterize(width, height, points, antialias);
    }

    Mask SelectColor(const Bitmap& image, glm::ivec2 at, float tolerance, bool contiguous)
    {
        Mask m;
        m.Width = image.Width;
        m.Height = image.Height;
        m.Values = MatchRegion(image, at, tolerance, contiguous);
        for (uint8_t& v : m.Values) v = v ? 255 : 0;
        return m;
    }

    Mask SelectColorRange(const Bitmap& image, const glm::vec4& color, float fuzziness)
    {
        Mask m = Mask::Solid(image.Width, image.Height, 0);
        const uint32_t target = Pack(glm::vec4(glm::vec3(color), 1.0f));
        const float fuzz = std::max(fuzziness, 0.5f);
        for (size_t i = 0; i < image.Pixels.size(); ++i)
        {
            uint32_t p = image.Pixels[i];
            int d = 0;
            for (int s = 0; s < 24; s += 8) d = std::max(d, std::abs((int)((p >> s) & 0xFF) - (int)((target >> s) & 0xFF)));
            float v = std::clamp(1.0f - d / fuzz, 0.0f, 1.0f) * ((p >> 24) / 255.0f);
            m.Values[i] = (uint8_t)std::lround(v * 255.0f);
        }
        return m;
    }

    Mask SelectAlpha(const Bitmap& image)
    {
        Mask m = Mask::Solid(image.Width, image.Height, 0);
        for (size_t i = 0; i < image.Pixels.size(); ++i) m.Values[i] = (uint8_t)(image.Pixels[i] >> 24);
        return m;
    }

    void Combine(Mask& sel, const Mask& shape, SelectOp op)
    {
        if (op == SelectOp::Replace || sel.Values.size() != shape.Values.size())
        {
            sel = shape;
            return;
        }
        for (size_t i = 0; i < sel.Values.size(); ++i)
        {
            int a = sel.Values[i], b = shape.Values[i];
            switch (op)
            {
            case SelectOp::Add: sel.Values[i] = (uint8_t)std::max(a, b); break;
            case SelectOp::Subtract: sel.Values[i] = (uint8_t)std::max(0, a - b); break;
            case SelectOp::Intersect: sel.Values[i] = (uint8_t)std::min(a, b); break;
            default: break;
            }
        }
    }

    void InvertSelection(Mask& sel)
    {
        for (uint8_t& v : sel.Values) v = (uint8_t)(255 - v);
    }

    void Feather(Mask& sel, float radius)
    {
        int r = (int)std::lround(radius / 1.7f);
        if (r <= 0) return;
        std::vector<float> v(sel.Values.begin(), sel.Values.end());
        BoxBlur(v, (int)sel.Width, (int)sel.Height, r);
        for (size_t i = 0; i < v.size(); ++i) sel.Values[i] = (uint8_t)std::lround(std::clamp(v[i], 0.0f, 255.0f));
    }

    void Expand(Mask& sel, int pixels)
    {
        if (pixels == 0) return;
        MaxFilter(sel.Values, (int)sel.Width, (int)sel.Height, std::abs(pixels), pixels > 0);
    }

    Mask Border(const Mask& sel, int width)
    {
        Mask outer = sel, inner = sel;
        int half = std::max(1, width / 2);
        Expand(outer, half);
        Expand(inner, -(width - half));
        for (size_t i = 0; i < outer.Values.size(); ++i) outer.Values[i] = (uint8_t)std::max(0, outer.Values[i] - inner.Values[i]);
        return outer;
    }

    bool IsEmpty(const Mask& sel)
    {
        return std::all_of(sel.Values.begin(), sel.Values.end(), [](uint8_t v) { return v == 0; });
    }

    Rect Bounds(const Mask& sel, uint8_t threshold)
    {
        Rect r;
        for (uint32_t y = 0; y < sel.Height; ++y)
        {
            const uint8_t* row = sel.Values.data() + (size_t)y * sel.Width;
            for (uint32_t x = 0; x < sel.Width; ++x)
                if (row[x] >= threshold)
                {
                    r.Add((int)x, (int)y);
                    // (the rest of the row only matters for its last pixel)
                    for (uint32_t e = sel.Width; e-- > x;)
                        if (row[e] >= threshold)
                        {
                            r.Add((int)e, (int)y);
                            break;
                        }
                    break;
                }
        }
        return r;
    }

    std::vector<Segment> Outline(const Mask& sel, size_t limit)
    {
        std::vector<Segment> out;
        const int w = (int)sel.Width, h = (int)sel.Height;
        auto in = [&](int x, int y) { return x >= 0 && y >= 0 && x < w && y < h && sel.Values[(size_t)y * w + x] >= 128; };
        // Horizontal edges (between rows y-1 and y), merged along x; then vertical ones, merged along y.
        for (int y = 0; y <= h && out.size() < limit; ++y)
        {
            int start = -1;
            for (int x = 0; x <= w; ++x)
            {
                bool edge = x < w && in(x, y) != in(x, y - 1);
                if (edge && start < 0) start = x;
                if (!edge && start >= 0)
                {
                    out.push_back({ { start, y }, { x, y } });
                    start = -1;
                }
            }
        }
        for (int x = 0; x <= w && out.size() < limit; ++x)
        {
            int start = -1;
            for (int y = 0; y <= h; ++y)
            {
                bool edge = y < h && in(x, y) != in(x - 1, y);
                if (edge && start < 0) start = y;
                if (!edge && start >= 0)
                {
                    out.push_back({ { x, start }, { x, y } });
                    start = -1;
                }
            }
        }
        if (out.size() > limit) out.resize(limit);
        return out;
    }

    // ---- Pixels and selections

    Rect ContentBounds(const Bitmap& image)
    {
        Rect r;
        for (uint32_t y = 0; y < image.Height; ++y)
            for (uint32_t x = 0; x < image.Width; ++x)
                if (image.At(x, y) >> 24) r.Add((int)x, (int)y);
        return r;
    }

    Bitmap CopyPixels(const Bitmap& image, const Mask* sel, Rect& bounds)
    {
        bounds = sel ? Bounds(*sel) : ContentBounds(image);
        if (bounds.Empty()) return Bitmap();
        Bitmap out = Bitmap::Solid((uint32_t)(bounds.X1 - bounds.X0 + 1), (uint32_t)(bounds.Y1 - bounds.Y0 + 1), 0u);
        for (int y = bounds.Y0; y <= bounds.Y1; ++y)
            for (int x = bounds.X0; x <= bounds.X1; ++x)
            {
                uint32_t p = image.At((uint32_t)x, (uint32_t)y);
                if (sel)
                {
                    uint32_t a = ((p >> 24) * sel->At((uint32_t)x, (uint32_t)y) + 127) / 255;
                    p = (p & 0x00FFFFFFu) | (a << 24);
                }
                out.At((uint32_t)(x - bounds.X0), (uint32_t)(y - bounds.Y0)) = (p >> 24) ? p : 0u;
            }
        return out;
    }

    void Blit(Bitmap& image, const Bitmap& piece, glm::ivec2 at)
    {
        for (uint32_t y = 0; y < piece.Height; ++y)
            for (uint32_t x = 0; x < piece.Width; ++x)
            {
                int dx = at.x + (int)x, dy = at.y + (int)y;
                if (dx < 0 || dy < 0 || dx >= (int)image.Width || dy >= (int)image.Height) continue;
                glm::vec4 s = Premul(piece.At(x, y));
                if (s.a <= 0.0f) continue;
                uint32_t& d = image.At((uint32_t)dx, (uint32_t)dy);
                d = Straight(s + Premul(d) * (1.0f - s.a));
            }
    }

    Rect ClearPixels(Bitmap& image, const Mask* sel)
    {
        Rect changed;
        for (uint32_t y = 0; y < image.Height; ++y)
            for (uint32_t x = 0; x < image.Width; ++x)
            {
                uint32_t& p = image.At(x, y);
                uint32_t s = sel ? sel->At(x, y) : 255u;
                if (!s || !(p >> 24)) continue;
                uint32_t a = ((p >> 24) * (255 - s) + 127) / 255;
                p = a ? (p & 0x00FFFFFFu) | (a << 24) : 0u;
                changed.Add((int)x, (int)y);
            }
        return changed;
    }

    Rect FillMask(Mask& mask, const Mask* sel, uint8_t value)
    {
        Rect changed;
        for (uint32_t y = 0; y < mask.Height; ++y)
            for (uint32_t x = 0; x < mask.Width; ++x)
            {
                size_t i = (size_t)y * mask.Width + x;
                float s = sel ? sel->Values[i] / 255.0f : 1.0f;
                uint8_t v = (uint8_t)std::lround(mask.Values[i] * (1.0f - s) + value * s);
                if (v != mask.Values[i]) mask.Values[i] = v, changed.Add((int)x, (int)y);
            }
        return changed;
    }

    Rect FillSelection(Document& doc, int layer, const glm::vec4& color, const Mask* sel, bool mask)
    {
        if (layer < 0 || layer >= (int)doc.Layers.size()) return {};
        Layer& l = doc.Layers[(size_t)layer];
        if (mask)
        {
            if (!l.LayerMask) return {};
            return FillMask(l.EditMask(), sel, (uint8_t)std::lround(Luminance(glm::vec3(color)) * 255.0f));
        }
        if (l.IsGroup() || !l.Pixels) return {};
        Rect changed;
        Bitmap& px = l.Edit();
        for (uint32_t y = 0; y < doc.Height; ++y)
            for (uint32_t x = 0; x < doc.Width; ++x)
            {
                float s = (sel ? sel->At(x, y) / 255.0f : 1.0f) * color.a;
                if (s <= 0.0f) continue;
                uint32_t& p = px.At(x, y);
                glm::vec4 d = Unpack(p);
                uint32_t before = p;
                if (l.LockTransparency) p = Pack(glm::vec4(glm::mix(glm::vec3(d), glm::vec3(color), s), d.a));
                else p = Straight(glm::vec4(glm::vec3(color) * s, s) + Premul(p) * (1.0f - s));
                if (p != before) changed.Add((int)x, (int)y);
            }
        return changed;
    }

    // ---- Transforms

    glm::mat3 Homography(const glm::vec2 from[4], const glm::vec2 to[4])
    {
        // h11..h32 (h33 = 1) from the 8 equations x' (h31 x + h32 y + 1) = h11 x + h12 y + h13, likewise y'.
        double a[8][9] = {};
        for (int i = 0; i < 4; ++i)
        {
            double x = from[i].x, y = from[i].y, u = to[i].x, v = to[i].y;
            double r0[9] = { x, y, 1, 0, 0, 0, -u * x, -u * y, u };
            double r1[9] = { 0, 0, 0, x, y, 1, -v * x, -v * y, v };
            std::copy(r0, r0 + 9, a[i * 2]);
            std::copy(r1, r1 + 9, a[i * 2 + 1]);
        }
        for (int c = 0; c < 8; ++c)
        {
            int pivot = c;
            for (int r = c + 1; r < 8; ++r)
                if (std::abs(a[r][c]) > std::abs(a[pivot][c])) pivot = r;
            if (std::abs(a[pivot][c]) < 1e-12) return glm::mat3(1.0f);
            if (pivot != c)
                for (int k = 0; k < 9; ++k) std::swap(a[c][k], a[pivot][k]);
            for (int r = 0; r < 8; ++r)
            {
                if (r == c) continue;
                double f = a[r][c] / a[c][c];
                for (int k = c; k < 9; ++k) a[r][k] -= f * a[c][k];
            }
        }
        double h[8];
        for (int i = 0; i < 8; ++i) h[i] = a[i][8] / a[i][i];
        // (glm is column-major: m[column][row])
        glm::mat3 m;
        m[0] = glm::vec3((float)h[0], (float)h[3], (float)h[6]);
        m[1] = glm::vec3((float)h[1], (float)h[4], (float)h[7]);
        m[2] = glm::vec3((float)h[2], (float)h[5], 1.0f);
        return m;
    }

    glm::vec2 Apply(const glm::mat3& m, const glm::vec2& p)
    {
        glm::vec3 q = m * glm::vec3(p, 1.0f);
        return std::abs(q.z) > 1e-9f ? glm::vec2(q) / q.z : glm::vec2(1e9f);
    }

    Bitmap TransformPixels(const Bitmap& image, const glm::mat3& m, const Mask* sel, Resample method)
    {
        const int w = (int)image.Width, h = (int)image.Height;
        // What stays: everything but the selected pixels (nothing, without a selection).
        Bitmap out = sel ? image : Bitmap::Solid(image.Width, image.Height, 0u);
        if (sel) ClearPixels(out, sel);
        const glm::mat3 inv = glm::inverse(m);
        auto tap = [&](int x, int y) {
            if (x < 0 || y < 0 || x >= w || y >= h) return glm::vec4(0.0f);
            glm::vec4 c = Premul(image.Pixels[(size_t)y * w + x]);
            return sel ? c * (sel->Values[(size_t)y * w + x] / 255.0f) : c;
        };
        ParallelRows(h, (size_t)w * 4, [&](int y0, int y1) {
            for (int y = y0; y <= y1; ++y)
                for (int x = 0; x < w; ++x)
                {
                    glm::vec2 s = Apply(inv, glm::vec2(x + 0.5f, y + 0.5f));
                    if (s.x < -1.0f || s.y < -1.0f || s.x > w + 1.0f || s.y > h + 1.0f) continue;
                    glm::vec4 c;
                    if (method == Resample::Nearest) c = tap((int)std::floor(s.x), (int)std::floor(s.y));
                    else
                    {
                        glm::vec2 f = s - 0.5f;
                        int x0 = (int)std::floor(f.x), y0p = (int)std::floor(f.y);
                        glm::vec2 t = f - glm::vec2(x0, y0p);
                        c = glm::mix(glm::mix(tap(x0, y0p), tap(x0 + 1, y0p), t.x), glm::mix(tap(x0, y0p + 1), tap(x0 + 1, y0p + 1), t.x), t.y);
                    }
                    if (c.a <= 1e-6f) continue;
                    uint32_t& d = out.Pixels[(size_t)y * w + x];
                    d = Straight(c + Premul(d) * (1.0f - c.a));
                }
        });
        return out;
    }

    Mask TransformMask(const Mask& mask, const glm::mat3& m, uint8_t outside, Resample method)
    {
        const int w = (int)mask.Width, h = (int)mask.Height;
        Mask out = Mask::Solid(mask.Width, mask.Height, outside);
        const glm::mat3 inv = glm::inverse(m);
        auto tap = [&](int x, int y) -> float {
            if (x < 0 || y < 0 || x >= w || y >= h) return outside;
            return mask.Values[(size_t)y * w + x];
        };
        ParallelRows(h, (size_t)w * 4, [&](int y0, int y1) {
            for (int y = y0; y <= y1; ++y)
                for (int x = 0; x < w; ++x)
                {
                    glm::vec2 s = Apply(inv, glm::vec2(x + 0.5f, y + 0.5f));
                    float v;
                    if (method == Resample::Nearest) v = tap((int)std::floor(s.x), (int)std::floor(s.y));
                    else
                    {
                        glm::vec2 f = s - 0.5f;
                        int x0 = (int)std::floor(f.x), yy = (int)std::floor(f.y);
                        glm::vec2 t = f - glm::vec2(x0, yy);
                        v = glm::mix(glm::mix(tap(x0, yy), tap(x0 + 1, yy), t.x), glm::mix(tap(x0, yy + 1), tap(x0 + 1, yy + 1), t.x), t.y);
                    }
                    out.Values[(size_t)y * w + x] = (uint8_t)std::lround(std::clamp(v, 0.0f, 255.0f));
                }
        });
        return out;
    }

    Bitmap ResampleBitmap(const Bitmap& image, uint32_t width, uint32_t height, Resample method)
    {
        Bitmap out = Bitmap::Solid(std::max(width, 1u), std::max(height, 1u), 0u);
        if (image.Empty()) return out;
        Resample2D<glm::vec4>((int)image.Width, (int)image.Height, (int)out.Width, (int)out.Height, method,
                              [&](int x, int y) { return Premul(image.At((uint32_t)x, (uint32_t)y)); },
                              [&](int x, int y, const glm::vec4& c) { out.At((uint32_t)x, (uint32_t)y) = Straight(c); });
        return out;
    }

    Mask ResampleMask(const Mask& mask, uint32_t width, uint32_t height, Resample method)
    {
        Mask out = Mask::Solid(std::max(width, 1u), std::max(height, 1u), 0);
        if (mask.Values.empty()) return out;
        Resample2D<float>((int)mask.Width, (int)mask.Height, (int)out.Width, (int)out.Height, method,
                          [&](int x, int y) { return (float)mask.At((uint32_t)x, (uint32_t)y); },
                          [&](int x, int y, float v) { out.Values[(size_t)y * out.Width + x] = (uint8_t)std::lround(std::clamp(v, 0.0f, 255.0f)); });
        return out;
    }

    // ---- The canvas

    namespace
    {
        // Each layer's pixels and mask, and the selection, through `pixels` and `mask` (new buffers).
        template <typename P, typename M>
        void EachBuffer(Document& doc, P pixels, M mask)
        {
            for (Layer& l : doc.Layers)
            {
                if (l.Pixels) l.Pixels = std::make_shared<Bitmap>(pixels(*l.Pixels));
                if (l.LayerMask) l.LayerMask = std::make_shared<Mask>(mask(*l.LayerMask, (uint8_t)255));
            }
            if (doc.Selection) doc.Selection = std::make_shared<Mask>(mask(*doc.Selection, (uint8_t)0));
        }
    }

    namespace
    {
        // Text and shape layers' settings through a change of the canvas: `point` maps positions, `scale` sizes,
        // `turn` adds to rotations (degrees).
        template <typename P>
        void MoveVectors(Document& doc, P point, glm::vec2 scale, float turn)
        {
            for (Layer& l : doc.Layers)
            {
                if (l.Kind == LayerKind::Shape)
                {
                    l.Shape.Center = point(l.Shape.Center);
                    l.Shape.Size *= scale;
                    float s = (scale.x + scale.y) * 0.5f;
                    l.Shape.StrokeWidth *= s;
                    l.Shape.Radius *= s;
                    l.Shape.Rotation += turn;
                }
                else if (l.Kind == LayerKind::Text)
                {
                    l.Text.Position = point(l.Text.Position);
                    l.Text.Size *= scale.y;
                    l.Text.Tracking *= scale.x;
                    l.Text.Rotation += turn;
                }
                l.VectorKey = 0;
            }
        }
    }

    void Reframe(Document& doc, uint32_t width, uint32_t height, glm::ivec2 offset)
    {
        MoveVectors(doc, [&](glm::vec2 p) { return p + glm::vec2(offset); }, glm::vec2(1.0f), 0.0f);
        width = std::max(width, 1u);
        height = std::max(height, 1u);
        const uint32_t ow = doc.Width, oh = doc.Height;
        auto place = [&](auto& out, const auto& in, auto get, auto set) {
            for (uint32_t y = 0; y < oh; ++y)
                for (uint32_t x = 0; x < ow; ++x)
                {
                    int dx = (int)x + offset.x, dy = (int)y + offset.y;
                    if (dx >= 0 && dy >= 0 && dx < (int)width && dy < (int)height) set(out, (uint32_t)dx, (uint32_t)dy, get(in, x, y));
                }
        };
        EachBuffer(
            doc,
            [&](const Bitmap& in) {
                Bitmap out = Bitmap::Solid(width, height, 0u);
                place(out, in, [](const Bitmap& b, uint32_t x, uint32_t y) { return b.At(x, y); }, [](Bitmap& b, uint32_t x, uint32_t y, uint32_t v) { b.At(x, y) = v; });
                return out;
            },
            [&](const Mask& in, uint8_t fill) {
                Mask out = Mask::Solid(width, height, fill);
                place(out, in, [](const Mask& m, uint32_t x, uint32_t y) { return m.At(x, y); },
                      [](Mask& m, uint32_t x, uint32_t y, uint8_t v) { m.Values[(size_t)y * m.Width + x] = v; });
                return out;
            });
        doc.Width = width;
        doc.Height = height;
    }

    void ResizeImage(Document& doc, uint32_t width, uint32_t height, Resample method)
    {
        width = std::max(width, 1u);
        height = std::max(height, 1u);
        const glm::vec2 scale((float)width / doc.Width, (float)height / doc.Height);
        MoveVectors(doc, [&](glm::vec2 p) { return p * scale; }, scale, 0.0f);
        EachBuffer(
            doc, [&](const Bitmap& in) { return ResampleBitmap(in, width, height, method); },
            [&](const Mask& in, uint8_t) { return ResampleMask(in, width, height, method); });
        doc.Width = width;
        doc.Height = height;
    }

    void RotateCanvas(Document& doc, int quarters)
    {
        quarters = ((quarters % 4) + 4) % 4;
        if (quarters == 0) return;
        const uint32_t ow = doc.Width, oh = doc.Height;
        const uint32_t nw = quarters % 2 ? oh : ow, nh = quarters % 2 ? ow : oh;
        MoveVectors(
            doc,
            [&](glm::vec2 p) {
                return quarters == 1 ? glm::vec2((float)oh - p.y, p.x) : quarters == 2 ? glm::vec2((float)ow - p.x, (float)oh - p.y) : glm::vec2(p.y, (float)ow - p.x);
            },
            glm::vec2(1.0f), 90.0f * quarters);
        // Where the new pixel (x, y) comes from.
        auto source = [&](uint32_t x, uint32_t y) -> glm::uvec2 {
            switch (quarters)
            {
            case 1: return { y, oh - 1 - x };          // clockwise
            case 2: return { ow - 1 - x, oh - 1 - y };
            default: return { ow - 1 - y, x };         // counterclockwise
            }
        };
        EachBuffer(
            doc,
            [&](const Bitmap& in) {
                Bitmap out = Bitmap::Solid(nw, nh, 0u);
                for (uint32_t y = 0; y < nh; ++y)
                    for (uint32_t x = 0; x < nw; ++x)
                    {
                        glm::uvec2 s = source(x, y);
                        out.At(x, y) = in.At(s.x, s.y);
                    }
                return out;
            },
            [&](const Mask& in, uint8_t) {
                Mask out = Mask::Solid(nw, nh, 0);
                for (uint32_t y = 0; y < nh; ++y)
                    for (uint32_t x = 0; x < nw; ++x)
                    {
                        glm::uvec2 s = source(x, y);
                        out.Values[(size_t)y * nw + x] = in.At(s.x, s.y);
                    }
                return out;
            });
        doc.Width = nw;
        doc.Height = nh;
    }

    void FlipCanvas(Document& doc, bool horizontal)
    {
        const uint32_t w = doc.Width, h = doc.Height;
        // Text cannot be mirrored as text: it becomes pixels; shapes mirror their settings.
        for (int i = 0; i < (int)doc.Layers.size(); ++i)
            if (doc.Layers[(size_t)i].Kind == LayerKind::Text) doc.Rasterize(i);
        MoveVectors(doc, [&](glm::vec2 p) { return horizontal ? glm::vec2((float)w - p.x, p.y) : glm::vec2(p.x, (float)h - p.y); }, glm::vec2(1.0f), 0.0f);
        for (Layer& l : doc.Layers)
            if (l.Kind == LayerKind::Shape)
            {
                l.Shape.Rotation = -l.Shape.Rotation;
                if (!horizontal && (l.Shape.Kind == ShapeKind::Polygon || l.Shape.Kind == ShapeKind::Star)) l.Shape.Rotation += 180.0f;
            }
        auto source = [&](uint32_t x, uint32_t y) { return horizontal ? glm::uvec2(w - 1 - x, y) : glm::uvec2(x, h - 1 - y); };
        EachBuffer(
            doc,
            [&](const Bitmap& in) {
                Bitmap out = Bitmap::Solid(w, h, 0u);
                for (uint32_t y = 0; y < h; ++y)
                    for (uint32_t x = 0; x < w; ++x)
                    {
                        glm::uvec2 s = source(x, y);
                        out.At(x, y) = in.At(s.x, s.y);
                    }
                return out;
            },
            [&](const Mask& in, uint8_t) {
                Mask out = Mask::Solid(w, h, 0);
                for (uint32_t y = 0; y < h; ++y)
                    for (uint32_t x = 0; x < w; ++x)
                    {
                        glm::uvec2 s = source(x, y);
                        out.Values[(size_t)y * w + x] = in.At(s.x, s.y);
                    }
                return out;
            });
    }
}
