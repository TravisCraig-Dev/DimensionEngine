#include "Dimension/Renderer/ColrV1.h"
#include <stb_truetype.h>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace Dimension::ColrV1
{
    namespace
    {
        // Reads within the table (zero past its end).
        struct Reader
        {
            const uint8_t* Base;
            size_t Size;
            bool In(size_t at, size_t bytes) const { return at + bytes <= Size; }
            uint8_t U8(size_t at) const { return In(at, 1) ? Base[at] : 0; }
            uint16_t U16(size_t at) const { return In(at, 2) ? (uint16_t)(Base[at] << 8 | Base[at + 1]) : 0; }
            int16_t S16(size_t at) const { return (int16_t)U16(at); }
            uint32_t U24(size_t at) const { return In(at, 3) ? (uint32_t)Base[at] << 16 | (uint32_t)Base[at + 1] << 8 | Base[at + 2] : 0; }
            uint32_t U32(size_t at) const { return In(at, 4) ? (uint32_t)Base[at] << 24 | (uint32_t)Base[at + 1] << 16 | (uint32_t)Base[at + 2] << 8 | Base[at + 3] : 0; }
            float F2Dot14(size_t at) const { return S16(at) / 16384.0f; }
            float Fixed(size_t at) const { return (int32_t)U32(at) / 65536.0f; }
        };

        // x' = xx x + xy y + dx, y' = yx x + yy y + dy.
        struct Affine
        {
            float xx = 1, yx = 0, xy = 0, yy = 1, dx = 0, dy = 0;
            Affine operator*(const Affine& b) const // this after b
            {
                return { xx * b.xx + xy * b.yx, yx * b.xx + yy * b.yx, xx * b.xy + xy * b.yy, yx * b.xy + yy * b.yy,
                         xx * b.dx + xy * b.dy + dx, yx * b.dx + yy * b.dy + dy };
            }
            void Apply(float x, float y, float& ox, float& oy) const
            {
                ox = xx * x + xy * y + dx;
                oy = yx * x + yy * y + dy;
            }
            Affine Inverse() const
            {
                float det = xx * yy - xy * yx;
                if (std::abs(det) < 1e-12f) return {};
                float i = 1.0f / det;
                Affine r{ yy * i, -yx * i, -xy * i, xx * i, 0, 0 };
                r.dx = -(r.xx * dx + r.xy * dy);
                r.dy = -(r.yx * dx + r.yy * dy);
                return r;
            }
            static Affine Translate(float x, float y) { return { 1, 0, 0, 1, x, y }; }
            static Affine Scale(float x, float y) { return { x, 0, 0, y, 0, 0 }; }
            static Affine Around(const Affine& m, float cx, float cy) { return Translate(cx, cy) * m * Translate(-cx, -cy); }
        };

        struct Color
        {
            float r = 0, g = 0, b = 0, a = 0; // premultiplied
        };

        // An image of the glyph's box (premultiplied), or a coverage mask.
        struct Canvas
        {
            int W = 0, H = 0;
            std::vector<Color> Px;
            void Init(int w, int h) { W = w, H = h, Px.assign(size_t(w) * h, Color{}); }
        };

        struct Context
        {
            Reader R;
            const Table* T;
            const stbtt_fontinfo* Font;
            const std::vector<uint32_t>* Palette;
            Affine ToPixels; // font units -> the image's pixels (y down)
            int W, H;
            int Depth = 0;
        };

        // A palette color (0xFFFF: the text's color, white) times alpha, as straight RGBA. Blending and gradients work on
        // the sRGB values, as browsers draw color fonts.
        void PaletteColor(const Context& c, uint16_t index, float alpha, float out[4])
        {
            uint32_t rgba = index < c.Palette->size() ? (*c.Palette)[index] : 0xFFFFFFFFu;
            out[0] = (rgba & 255) / 255.0f;
            out[1] = (rgba >> 8 & 255) / 255.0f;
            out[2] = (rgba >> 16 & 255) / 255.0f;
            out[3] = (rgba >> 24) / 255.0f * std::clamp(alpha, 0.0f, 1.0f);
        }

        // A glyph outline's coverage (0-1 per pixel) under `m` (font units -> pixels): flattened, then filled by the
        // nonzero rule with 4 sub-rows per pixel and exact coverage along each.
        void Coverage(const Context& c, uint16_t glyph, const Affine& m, std::vector<float>& cov)
        {
            cov.assign(size_t(c.W) * c.H, 0.0f);
            stbtt_vertex* v = nullptr;
            int n = stbtt_GetGlyphShape(c.Font, glyph, &v);
            if (n <= 0 || !v) return;
            struct Edge { float x0, y0, x1, y1; int dir; };
            std::vector<Edge> edges;
            float px = 0, py = 0, sx = 0, sy = 0;
            auto line = [&](float x0, float y0, float x1, float y1) {
                if (y0 == y1) return;
                if (y0 < y1) edges.push_back({ x0, y0, x1, y1, 1 });
                else edges.push_back({ x1, y1, x0, y0, -1 });
            };
            for (int i = 0; i < n; ++i)
            {
                float x, y;
                m.Apply(v[i].x, v[i].y, x, y);
                switch (v[i].type)
                {
                case STBTT_vmove:
                    if (i > 0) line(px, py, sx, sy); // close the last contour
                    sx = px = x, sy = py = y;
                    break;
                case STBTT_vline: line(px, py, x, y); px = x, py = y; break;
                case STBTT_vcurve:
                {
                    float cx, cy;
                    m.Apply(v[i].cx, v[i].cy, cx, cy);
                    int steps = std::clamp((int)(std::hypot(cx - px, cy - py) + std::hypot(x - cx, y - cy)) / 2, 2, 32);
                    float lx = px, ly = py;
                    for (int s = 1; s <= steps; ++s)
                    {
                        float t = (float)s / steps, u = 1 - t;
                        float qx = u * u * px + 2 * u * t * cx + t * t * x, qy = u * u * py + 2 * u * t * cy + t * t * y;
                        line(lx, ly, qx, qy);
                        lx = qx, ly = qy;
                    }
                    px = x, py = y;
                    break;
                }
                case STBTT_vcubic:
                {
                    float c1x, c1y, c2x, c2y;
                    m.Apply(v[i].cx, v[i].cy, c1x, c1y);
                    m.Apply(v[i].cx1, v[i].cy1, c2x, c2y);
                    int steps = std::clamp((int)(std::hypot(c1x - px, c1y - py) + std::hypot(c2x - c1x, c2y - c1y) + std::hypot(x - c2x, y - c2y)) / 2, 2, 48);
                    float lx = px, ly = py;
                    for (int s = 1; s <= steps; ++s)
                    {
                        float t = (float)s / steps, u = 1 - t;
                        float qx = u * u * u * px + 3 * u * u * t * c1x + 3 * u * t * t * c2x + t * t * t * x;
                        float qy = u * u * u * py + 3 * u * u * t * c1y + 3 * u * t * t * c2y + t * t * t * y;
                        line(lx, ly, qx, qy);
                        lx = qx, ly = qy;
                    }
                    px = x, py = y;
                    break;
                }
                }
            }
            line(px, py, sx, sy);
            stbtt_FreeShape(c.Font, v);

            constexpr int kSub = 4;
            std::vector<std::pair<float, int>> crossings;
            for (int row = 0; row < c.H; ++row)
                for (int s = 0; s < kSub; ++s)
                {
                    float y = row + (s + 0.5f) / kSub;
                    crossings.clear();
                    for (const Edge& e : edges)
                        if (y >= e.y0 && y < e.y1) crossings.push_back({ e.x0 + (y - e.y0) / (e.y1 - e.y0) * (e.x1 - e.x0), e.dir });
                    if (crossings.size() < 2) continue;
                    std::sort(crossings.begin(), crossings.end());
                    int winding = 0;
                    for (size_t k = 0; k + 1 < crossings.size(); ++k)
                    {
                        winding += crossings[k].second;
                        if (winding == 0) continue;
                        // Covered from a to b on this sub-row: whole pixels inside, fractions at the ends.
                        float a = std::clamp(crossings[k].first, 0.0f, (float)c.W), b = std::clamp(crossings[k + 1].first, 0.0f, (float)c.W);
                        if (b <= a) continue;
                        float* out = &cov[size_t(row) * c.W];
                        int ia = (int)a, ib = (int)b;
                        if (ia == ib) { out[std::min(ia, c.W - 1)] += (b - a) / kSub; continue; }
                        out[ia] += (ia + 1 - a) / kSub;
                        for (int x = ia + 1; x < ib && x < c.W; ++x) out[x] += 1.0f / kSub;
                        if (ib < c.W) out[ib] += (b - ib) / kSub;
                    }
                }
            for (float& f : cov) f = std::min(f, 1.0f);
        }

        // A color line's color at t (its extend mode applied), straight RGBA.
        void ColorAt(const Context& c, size_t line, bool var, float t, float out[4])
        {
            uint8_t extend = c.R.U8(line);
            uint16_t count = c.R.U16(line + 1);
            size_t stride = var ? 10 : 6;
            if (count == 0)
            {
                out[0] = out[1] = out[2] = out[3] = 0.0f;
                return;
            }
            if (extend == 1) t -= std::floor(t);                                 // repeat
            else if (extend == 2) t = 1.0f - std::abs(std::fmod(std::abs(t), 2.0f) - 1.0f); // reflect
            // Stops are in order of offset (as fonts store them); before the first and after the last they hold.
            float lastOffset = 0.0f, last[4] = { 0, 0, 0, 0 };
            for (uint16_t i = 0; i < count; ++i)
            {
                size_t at = line + 3 + i * stride;
                float offset = c.R.F2Dot14(at), color[4];
                PaletteColor(c, c.R.U16(at + 2), c.R.F2Dot14(at + 4), color);
                if (i == 0 && t <= offset)
                {
                    std::copy(color, color + 4, out);
                    return;
                }
                if (i > 0 && t <= offset)
                {
                    float f = offset > lastOffset ? (t - lastOffset) / (offset - lastOffset) : 1.0f;
                    // Interpolated premultiplied, as the spec asks.
                    float pa[4] = { last[0] * last[3], last[1] * last[3], last[2] * last[3], last[3] };
                    float pb[4] = { color[0] * color[3], color[1] * color[3], color[2] * color[3], color[3] };
                    float a = pa[3] + (pb[3] - pa[3]) * f;
                    for (int k = 0; k < 3; ++k) out[k] = a > 1e-6f ? (pa[k] + (pb[k] - pa[k]) * f) / a : 0.0f;
                    out[3] = a;
                    return;
                }
                lastOffset = offset;
                std::copy(color, color + 4, last);
            }
            std::copy(last, last + 4, out);
        }

        // Fills `canvas` (where `mask` covers, all of it without one) with a paint, source-over what is there.
        void Paint(Context& c, size_t paint, const Affine& m, const std::vector<float>* mask, Canvas& canvas);

        void Fill(Context& c, const Affine& m, const std::vector<float>* mask, Canvas& canvas, const auto& colorAt)
        {
            Affine toFont = (c.ToPixels * m).Inverse();
            for (int y = 0; y < c.H; ++y)
                for (int x = 0; x < c.W; ++x)
                {
                    size_t i = size_t(y) * c.W + x;
                    float cover = mask ? (*mask)[i] : 1.0f;
                    if (cover <= 0.0f) continue;
                    float fx, fy, col[4];
                    toFont.Apply(x + 0.5f, y + 0.5f, fx, fy);
                    colorAt(fx, fy, col);
                    float a = col[3] * cover;
                    Color& d = canvas.Px[i];
                    d.r = col[0] * a + d.r * (1 - a);
                    d.g = col[1] * a + d.g * (1 - a);
                    d.b = col[2] * a + d.b * (1 - a);
                    d.a = a + d.a * (1 - a);
                }
        }

        void Composite(Canvas& dst, const Canvas& src, const Canvas& backdrop, uint8_t mode)
        {
            for (size_t i = 0; i < dst.Px.size(); ++i)
            {
                const Color& s = src.Px[i];
                const Color& b = backdrop.Px[i];
                Color r;
                auto mix = [&](float fs, float fb) { r = { s.r * fs + b.r * fb, s.g * fs + b.g * fb, s.b * fs + b.b * fb, s.a * fs + b.a * fb }; };
                switch (mode)
                {
                case 0: break;                              // clear
                case 1: r = s; break;                       // src
                case 2: r = b; break;                       // dest
                case 4: mix(1 - b.a, 1); break;             // dest over
                case 5: mix(b.a, 0); break;                 // src in
                case 6: mix(0, s.a); break;                 // dest in
                case 7: mix(1 - b.a, 0); break;             // src out
                case 8: mix(0, 1 - s.a); break;             // dest out
                case 9: mix(b.a, 1 - s.a); break;           // src atop
                case 10: mix(1 - b.a, s.a); break;          // dest atop
                case 11: mix(1 - b.a, 1 - s.a); break;      // xor
                case 12: mix(1, 1); r.a = std::min(r.a, 1.0f); break; // plus
                default: mix(1, 1 - s.a); break;            // src over (and the blend modes, approximated)
                }
                Color& d = dst.Px[i];
                d.r = r.r + d.r * (1 - r.a);
                d.g = r.g + d.g * (1 - r.a);
                d.b = r.b + d.b * (1 - r.a);
                d.a = r.a + d.a * (1 - r.a);
            }
        }

        void Paint(Context& c, size_t p, const Affine& m, const std::vector<float>* mask, Canvas& canvas)
        {
            if (!p || p >= c.R.Size || c.Depth > 64) return;
            ++c.Depth;
            const Reader& R = c.R;
            uint8_t format = R.U8(p);
            auto child = [&](size_t at) { uint32_t o = R.U24(at); return o ? p + o : 0; };
            switch (format)
            {
            case 1: // layers, bottom first
            {
                uint8_t count = R.U8(p + 1);
                uint32_t first = R.U32(p + 2);
                uint32_t layers = R.U32(c.T->LayerList);
                for (uint32_t i = first; i < first + count && i < layers; ++i)
                    Paint(c, c.T->LayerList + R.U32(c.T->LayerList + 4 + 4 * size_t(i)), m, mask, canvas);
                break;
            }
            case 2: case 3: // solid
            {
                float col[4];
                PaletteColor(c, R.U16(p + 1), R.F2Dot14(p + 3), col);
                Fill(c, m, mask, canvas, [&](float, float, float* out) { std::copy(col, col + 4, out); });
                break;
            }
            case 4: case 5: // linear gradient: along p0 -> p1, its lines parallel to p0 -> p2
            {
                size_t line = child(p + 1);
                float x0 = R.S16(p + 4), y0 = R.S16(p + 6), x1 = R.S16(p + 8), y1 = R.S16(p + 10), x2 = R.S16(p + 12), y2 = R.S16(p + 14);
                float nx = -(y2 - y0), ny = x2 - x0; // perpendicular to p0 -> p2
                float dx = x1 - x0, dy = y1 - y0;
                float nn = nx * nx + ny * ny;
                float ex = dx, ey = dy;
                if (nn > 1e-9f)
                {
                    float k = (dx * nx + dy * ny) / nn;
                    ex = nx * k, ey = ny * k;
                }
                float ee = std::max(ex * ex + ey * ey, 1e-9f);
                bool var = format == 5;
                Fill(c, m, mask, canvas, [&](float fx, float fy, float* out) { ColorAt(c, line, var, ((fx - x0) * ex + (fy - y0) * ey) / ee, out); });
                break;
            }
            case 6: case 7: // radial gradient: circles from (c0, r0) to (c1, r1), the largest t with a positive radius
            {
                size_t line = child(p + 1);
                float cx0 = R.S16(p + 4), cy0 = R.S16(p + 6), r0 = R.U16(p + 8), cx1 = R.S16(p + 10), cy1 = R.S16(p + 12), r1 = R.U16(p + 14);
                float cdx = cx1 - cx0, cdy = cy1 - cy0, dr = r1 - r0;
                float a = cdx * cdx + cdy * cdy - dr * dr;
                bool var = format == 7;
                Fill(c, m, mask, canvas, [&](float fx, float fy, float* out) {
                    float pdx = fx - cx0, pdy = fy - cy0;
                    float b = pdx * cdx + pdy * cdy + r0 * dr, cc = pdx * pdx + pdy * pdy - r0 * r0;
                    float t;
                    if (std::abs(a) < 1e-6f) t = std::abs(b) > 1e-6f ? cc / (2.0f * b) : 0.0f;
                    else
                    {
                        float disc = b * b - a * cc;
                        if (disc < 0.0f)
                        {
                            out[0] = out[1] = out[2] = out[3] = 0.0f;
                            return;
                        }
                        float s = std::sqrt(disc), ta = (b + s) / a, tb = (b - s) / a;
                        t = std::max(ta, tb);
                        if (r0 + t * dr < 0.0f) t = std::min(ta, tb);
                        if (r0 + t * dr < 0.0f)
                        {
                            out[0] = out[1] = out[2] = out[3] = 0.0f;
                            return;
                        }
                    }
                    ColorAt(c, line, var, t, out);
                });
                break;
            }
            case 8: case 9: // sweep gradient: counter-clockwise from the start angle to the end angle
            {
                size_t line = child(p + 1);
                float cx = R.S16(p + 4), cy = R.S16(p + 6);
                float start = R.F2Dot14(p + 8) * 180.0f, end = R.F2Dot14(p + 10) * 180.0f; // degrees, 180 per 1.0
                bool var = format == 9;
                Fill(c, m, mask, canvas, [&](float fx, float fy, float* out) {
                    float angle = std::atan2(fy - cy, fx - cx) * 57.29578f;
                    if (angle < 0.0f) angle += 360.0f;
                    ColorAt(c, line, var, end != start ? (angle - start) / (end - start) : 0.0f, out);
                });
                break;
            }
            case 10: // a glyph's outline clips the paint
            {
                std::vector<float> cov;
                Coverage(c, R.U16(p + 4), c.ToPixels * m, cov);
                if (mask)
                    for (size_t i = 0; i < cov.size(); ++i) cov[i] *= (*mask)[i];
                Paint(c, child(p + 1), m, &cov, canvas);
                break;
            }
            case 11: // another base glyph's paint
                Paint(c, c.T->FindPaint(R.U16(p + 1)), m, mask, canvas);
                break;
            case 12: case 13: // affine transform
            {
                size_t t = child(p + 4);
                Affine a{ R.Fixed(t), R.Fixed(t + 4), R.Fixed(t + 8), R.Fixed(t + 12), R.Fixed(t + 16), R.Fixed(t + 20) };
                Paint(c, child(p + 1), m * a, mask, canvas);
                break;
            }
            case 14: case 15: Paint(c, child(p + 1), m * Affine::Translate(R.S16(p + 4), R.S16(p + 6)), mask, canvas); break;
            case 16: case 17: Paint(c, child(p + 1), m * Affine::Scale(R.F2Dot14(p + 4), R.F2Dot14(p + 6)), mask, canvas); break;
            case 18: case 19:
                Paint(c, child(p + 1), m * Affine::Around(Affine::Scale(R.F2Dot14(p + 4), R.F2Dot14(p + 6)), R.S16(p + 8), R.S16(p + 10)), mask, canvas);
                break;
            case 20: case 21: Paint(c, child(p + 1), m * Affine::Scale(R.F2Dot14(p + 4), R.F2Dot14(p + 4)), mask, canvas); break;
            case 22: case 23:
                Paint(c, child(p + 1), m * Affine::Around(Affine::Scale(R.F2Dot14(p + 4), R.F2Dot14(p + 4)), R.S16(p + 6), R.S16(p + 8)), mask, canvas);
                break;
            case 24: case 25: case 26: case 27: // rotate (around a center)
            {
                float a = R.F2Dot14(p + 4) * 3.14159265f, cs = std::cos(a), sn = std::sin(a);
                Affine r{ cs, sn, -sn, cs, 0, 0 };
                if (format >= 26) r = Affine::Around(r, R.S16(p + 6), R.S16(p + 8));
                Paint(c, child(p + 1), m * r, mask, canvas);
                break;
            }
            case 28: case 29: case 30: case 31: // skew (around a center)
            {
                float ax = R.F2Dot14(p + 4) * 3.14159265f, ay = R.F2Dot14(p + 6) * 3.14159265f;
                Affine s{ 1, std::tan(ay), -std::tan(ax), 1, 0, 0 };
                if (format >= 30) s = Affine::Around(s, R.S16(p + 8), R.S16(p + 10));
                Paint(c, child(p + 1), m * s, mask, canvas);
                break;
            }
            case 32: // composite: the source onto the backdrop, then that over the canvas
            {
                Canvas source, backdrop;
                source.Init(c.W, c.H);
                backdrop.Init(c.W, c.H);
                Paint(c, child(p + 1), m, mask, source);
                Paint(c, child(p + 5), m, mask, backdrop);
                Composite(canvas, source, backdrop, R.U8(p + 4));
                break;
            }
            default: break;
            }
            --c.Depth;
        }
    }

    bool Table::Load(const uint8_t* colr, size_t size)
    {
        Reader r{ colr, size };
        if (size < 34 || r.U16(0) < 1) return false;
        Colr = colr;
        Size = size;
        BaseList = r.U32(14);
        LayerList = r.U32(18);
        ClipList = r.U32(22);
        if (BaseList >= size) BaseList = 0;
        if (LayerList >= size) LayerList = 0;
        if (ClipList >= size) ClipList = 0;
        return BaseList != 0;
    }

    uint32_t Table::FindPaint(uint16_t glyph) const
    {
        if (!BaseList) return 0;
        Reader r{ Colr, Size };
        uint32_t lo = 0, hi = r.U32(BaseList);
        while (lo < hi)
        {
            uint32_t mid = (lo + hi) / 2;
            size_t rec = BaseList + 4 + size_t(mid) * 6;
            uint16_t g = r.U16(rec);
            if (g == glyph)
            {
                uint32_t offset = r.U32(rec + 2);
                return offset ? BaseList + offset : 0;
            }
            if (g < glyph) lo = mid + 1;
            else hi = mid;
        }
        return 0;
    }

    bool Table::ClipBox(uint16_t glyph, float& xMin, float& yMin, float& xMax, float& yMax) const
    {
        if (!ClipList) return false;
        Reader r{ Colr, Size };
        uint32_t count = r.U32(ClipList + 1);
        for (uint32_t i = 0; i < count; ++i) // (sorted ranges: a binary search would do; a few thousand entries at most)
        {
            size_t rec = ClipList + 5 + size_t(i) * 7;
            uint16_t first = r.U16(rec), last = r.U16(rec + 2);
            if (glyph < first) return false;
            if (glyph > last) continue;
            size_t box = ClipList + r.U24(rec + 4);
            xMin = r.S16(box + 1), yMin = r.S16(box + 3), xMax = r.S16(box + 5), yMax = r.S16(box + 7);
            return true;
        }
        return false;
    }

    bool Render(const Table& table, const stbtt_fontinfo& font, float scale, uint16_t glyph, const std::vector<uint32_t>& palette,
                std::vector<uint32_t>& rgba, int& x0, int& y0, int& width, int& height)
    {
        uint32_t paint = table.FindPaint(glyph);
        if (!paint) return false;
        // The box: the clip list's, else the glyph's advance and the font's ascent / descent.
        float xMin, yMin, xMax, yMax;
        if (!table.ClipBox(glyph, xMin, yMin, xMax, yMax))
        {
            int advance, bearing, ascent, descent, gap;
            stbtt_GetGlyphHMetrics(&font, glyph, &advance, &bearing);
            stbtt_GetFontVMetrics(&font, &ascent, &descent, &gap);
            xMin = 0, xMax = (float)advance, yMin = (float)descent, yMax = (float)ascent;
        }
        x0 = (int)std::floor(xMin * scale) - 1;
        y0 = (int)std::floor(-yMax * scale) - 1;
        int x1 = (int)std::ceil(xMax * scale) + 1, y1 = (int)std::ceil(-yMin * scale) + 1;
        width = x1 - x0, height = y1 - y0;
        if (width <= 2 || height <= 2 || width > 1024 || height > 1024) return false;

        Context c{ Reader{ table.Colr, table.Size }, &table, &font, &palette, {}, width, height };
        c.ToPixels = { scale, 0, 0, -scale, (float)-x0, (float)-y0 }; // font units (y up) -> the image (y down)
        Canvas canvas;
        canvas.Init(width, height);
        Paint(c, paint, Affine{}, nullptr, canvas);
        // Straight alpha, and nothing on the border pixels (filtering, mips).
        rgba.assign(size_t(width) * height, 0u);
        bool any = false;
        auto toByte = [](float v) { return (uint32_t)std::clamp((int)std::lround(v * 255.0f), 0, 255); };
        for (int y = 1; y + 1 < height; ++y)
            for (int x = 1; x + 1 < width; ++x)
            {
                const Color& p = canvas.Px[size_t(y) * width + x];
                if (p.a <= 1e-4f) continue;
                float inv = 1.0f / p.a;
                rgba[size_t(y) * width + x] = toByte(p.r * inv) | toByte(p.g * inv) << 8 | toByte(p.b * inv) << 16 | toByte(std::min(p.a, 1.0f)) << 24;
                any = true;
            }
        return any;
    }

    bool RenderCodepoint(const uint8_t* data, size_t size, uint32_t codepoint, float pixelHeight, std::vector<uint32_t>& rgba, int& width, int& height)
    {
        stbtt_fontinfo info;
        int start = stbtt_GetFontOffsetForIndex(data, 0);
        if (start < 0 || !stbtt_InitFont(&info, data, start)) return false;
        Reader file{ data, size };
        uint32_t colr = 0, colrLength = 0, cpal = 0;
        uint16_t tables = file.U16(start + 4);
        for (uint16_t i = 0; i < tables; ++i)
        {
            size_t rec = start + 12 + 16 * size_t(i);
            if (!file.In(rec, 16)) return false;
            if (!std::memcmp(data + rec, "COLR", 4)) colr = file.U32(rec + 8), colrLength = file.U32(rec + 12);
            if (!std::memcmp(data + rec, "CPAL", 4)) cpal = file.U32(rec + 8);
        }
        Table table;
        if (!colr || !cpal || colr + colrLength > size || !table.Load(data + colr, colrLength)) return false;
        std::vector<uint32_t> palette;
        uint16_t entries = file.U16(cpal + 2);
        uint32_t records = file.U32(cpal + 8);
        uint16_t first = file.U16(cpal + 12);
        for (uint16_t i = 0; i < entries; ++i)
        {
            size_t at = cpal + records + 4 * (size_t(first) + i);
            if (!file.In(at, 4)) break;
            palette.push_back((uint32_t)data[at + 2] | (uint32_t)data[at + 1] << 8 | (uint32_t)data[at] << 16 | (uint32_t)data[at + 3] << 24);
        }
        int glyph = stbtt_FindGlyphIndex(&info, (int)codepoint);
        int x0, y0;
        return glyph > 0 && Render(table, info, stbtt_ScaleForPixelHeight(&info, pixelHeight), (uint16_t)glyph, palette, rgba, x0, y0, width, height);
    }
}
