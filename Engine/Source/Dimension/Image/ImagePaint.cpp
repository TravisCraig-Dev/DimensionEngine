#include "Dimension/Image/ImagePaint.h"
#include <algorithm>
#include <glm/gtc/constants.hpp>
#include <cmath>

namespace Dimension::Imaging
{
    namespace
    {
        // `color` laid over `under` by `amount` (0..1), as a stroke paints (straight alpha).
        uint32_t PaintOver(uint32_t under, const glm::vec4& color, float amount, bool erase, bool lockAlpha)
        {
            glm::vec4 d = Unpack(under);
            if (erase)
            {
                if (lockAlpha) return under;
                d.a *= 1.0f - amount;
                return Pack(d);
            }
            float sa = color.a * amount;
            if (lockAlpha) return Pack(glm::vec4(glm::mix(glm::vec3(d), glm::vec3(color), sa), d.a));
            float a = sa + d.a * (1.0f - sa);
            if (a <= 0.0f) return 0u;
            glm::vec3 c = (glm::vec3(color) * sa + glm::vec3(d) * d.a * (1.0f - sa)) / a;
            return Pack(glm::vec4(c, a));
        }

        // How much dodging and burning touch a tone.
        float ToneWeight(ToneRange range, float l)
        {
            switch (range)
            {
            case ToneRange::Shadows: return (1.0f - l) * (1.0f - l);
            case ToneRange::Highlights: return l * l;
            default: return 1.0f - std::abs(2.0f * l - 1.0f) * 0.7f;
            }
        }

        glm::vec4 Premultiply(uint32_t p)
        {
            glm::vec4 c = Unpack(p);
            return glm::vec4(glm::vec3(c) * c.a, c.a);
        }
        uint32_t Straighten(const glm::vec4& c) { return c.a > 0.0f ? Pack(glm::vec4(glm::vec3(c) / c.a, c.a)) : 0u; }

        // A box blur (radius r) of an RGB image w x h, edges clamped.
        std::vector<glm::vec3> BoxBlur(const std::vector<glm::vec3>& in, int w, int h, int r)
        {
            std::vector<glm::vec3> tmp(in.size()), out(in.size());
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                    glm::vec3 sum(0.0f);
                    for (int k = -r; k <= r; ++k) sum += in[(size_t)y * w + std::clamp(x + k, 0, w - 1)];
                    tmp[(size_t)y * w + x] = sum / float(2 * r + 1);
                }
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                    glm::vec3 sum(0.0f);
                    for (int k = -r; k <= r; ++k) sum += tmp[(size_t)std::clamp(y + k, 0, h - 1) * w + x];
                    out[(size_t)y * w + x] = sum / float(2 * r + 1);
                }
            return out;
        }

        float Noise(int x, int y)
        {
            uint32_t h = (uint32_t)x * 73856093u ^ (uint32_t)y * 19349663u;
            h = (h ^ (h >> 13)) * 1274126177u;
            return ((h >> 8) & 0xFFFF) / 65535.0f - 0.5f;
        }
    }

    float DabWeight(const BrushSettings& brush, const glm::vec2& offset)
    {
        // Into the tip's own frame: its long side along x, the short side stretched to a circle.
        glm::vec2 o = offset;
        float roundness = std::clamp(brush.Roundness, 0.05f, 1.0f);
        if (roundness < 1.0f || brush.Angle != 0.0f)
        {
            float a = glm::radians(brush.Angle);
            float c = std::cos(a), s = std::sin(a);
            o = glm::vec2(o.x * c - o.y * s, o.x * s + o.y * c); // (screen y points down: counterclockwise on screen)
            o.y /= roundness;
        }
        return DabWeight(brush, glm::length(o));
    }

    float DabWeight(const BrushSettings& brush, float distance)
    {
        float r = std::max(brush.Size * 0.5f, 0.5f);
        if (brush.Aliased) return distance <= r + 1e-4f ? 1.0f : 0.0f;
        if (distance >= r + 0.5f) return 0.0f;
        float edge = std::clamp(r - distance + 0.5f, 0.0f, 1.0f); // (antialiased rim)
        float h = std::clamp(brush.Hardness, 0.0f, 1.0f);
        float d = distance / r;
        float w = 1.0f;
        if (d > h)
        {
            float t = std::clamp((d - h) / std::max(1.0f - h, 1e-4f), 0.0f, 1.0f);
            w = 1.0f - t * t * (3.0f - 2.0f * t);
        }
        return std::min(w, edge);
    }

    Stroke::Stroke(Document& doc, int layer, const BrushSettings& brush, bool mask) : m_Doc(doc), m_Layer(layer), m_Mask(mask), m_Brush(brush)
    {
        if (m_Brush.Erase) m_Brush.Tool = BrushTool::Erase;
        if (layer < 0 || layer >= (int)doc.Layers.size()) return;
        Layer& l = doc.Layers[(size_t)layer];
        if (mask)
        {
            if (!l.LayerMask || (m_Brush.Tool != BrushTool::Paint && m_Brush.Tool != BrushTool::Erase)) return;
            m_OriginalMask = l.LayerMask;
            l.EditMask(); // (its own copy now; the original stays as the stroke's starting point)
        }
        else
        {
            if (!l.HasPixels()) return;
            m_Original = l.Pixels;
            l.Edit();
        }
        m_Coverage.assign((size_t)doc.Width * doc.Height, 0.0f);
    }

    Stroke::~Stroke() { Finish(); }

    void Stroke::To(const glm::vec2& p)
    {
        if (!m_Original && !m_OriginalMask) return;
        if (!m_Started)
        {
            m_Started = true;
            m_Last = p;
            m_Carry = 0.0f;
            Dab(p);
            return;
        }
        float step = std::max(m_Brush.Aliased ? 1.0f : 0.5f, m_Brush.Size * std::clamp(m_Brush.Roundness, 0.05f, 1.0f) * m_Brush.Spacing);
        if (m_Brush.Tool == BrushTool::Smudge) step = std::min(step, std::max(1.0f, m_Brush.Size * 0.05f)); // (smooth drags)
        glm::vec2 delta = p - m_Last;
        float length = glm::length(delta);
        if (length <= 0.0f) return;
        glm::vec2 dir = delta / length;
        float walked = step - m_Carry; // where the next dab falls along this segment
        while (walked <= length)
        {
            Dab(m_Last + dir * walked);
            walked += step;
        }
        m_Carry = length - (walked - step);
        m_Last = p;
    }

    void Stroke::Dab(const glm::vec2& center)
    {
        // Mirrors (the tools that paint by coverage; smudging and cloning stay single).
        const BrushTool t = m_Brush.Tool;
        const bool mirrors = m_Brush.Symmetry != 0 && t != BrushTool::Smudge && t != BrushTool::Clone && t != BrushTool::Heal;
        const glm::vec2 c = m_Brush.Aliased ? glm::floor(center) + 0.5f : center;
        DabAt(c);
        if (mirrors)
        {
            const glm::vec2 m = 2.0f * m_Brush.SymmetryCenter - c;
            if (m_Brush.Symmetry & 1) DabAt({ m.x, c.y });
            if (m_Brush.Symmetry & 2) DabAt({ c.x, m.y });
            if ((m_Brush.Symmetry & 3) == 3) DabAt(m);
            return;
        }
        // The pixel-perfect pencil: a pixel that only turned a corner between two diagonal neighbours goes back.
        if (m_Brush.PixelPerfect && m_Brush.Aliased && m_Brush.Size <= 1.5f && !m_Mask)
        {
            glm::ivec2 p = glm::ivec2(glm::floor(center));
            if (!m_Trail.empty() && m_Trail.back() == p) return;
            m_Trail.push_back(p);
            if (m_Trail.size() >= 3)
            {
                glm::ivec2 a = m_Trail[m_Trail.size() - 3], b = m_Trail[m_Trail.size() - 2], d = m_Trail.back();
                glm::ivec2 ab = glm::abs(b - a), bd = glm::abs(d - b), ad = glm::abs(d - a);
                if (ab.x + ab.y == 1 && bd.x + bd.y == 1 && ad.x == 1 && ad.y == 1)
                {
                    const int w = (int)m_Doc.Width;
                    if (b.x >= 0 && b.y >= 0 && b.x < w && b.y < (int)m_Doc.Height)
                    {
                        size_t i = (size_t)b.y * w + b.x;
                        m_Doc.Layers[(size_t)m_Layer].Edit().Pixels[i] = m_Original->Pixels[i];
                        m_Coverage[i] = 0.0f;
                        m_Changed.Add(b.x, b.y);
                    }
                    m_Trail.erase(m_Trail.end() - 2);
                }
            }
        }
    }

    void Stroke::DabAt(const glm::vec2& center)
    {
        Layer& layer = m_Doc.Layers[(size_t)m_Layer];
        const int w = (int)m_Doc.Width, h = (int)m_Doc.Height;
        const float r = std::max(m_Brush.Size * 0.5f, 0.5f) + 1.0f;
        // (the pencil's dabs come centered on a pixel: its pixels are those whose centers fall in the brush)
        const glm::vec2 c = center;
        const int x0 = std::max(0, (int)std::floor(c.x - r)), x1 = std::min(w - 1, (int)std::ceil(c.x + r));
        const int y0 = std::max(0, (int)std::floor(c.y - r)), y1 = std::min(h - 1, (int)std::ceil(c.y + r));
        if (x0 > x1 || y0 > y1) return;
        const float flow = std::clamp(m_Brush.Flow, 0.0f, 1.0f), opacity = std::clamp(m_Brush.Opacity, 0.0f, 1.0f);
        const BrushTool tool = m_Brush.Tool;

        // Masks: painting with the color's gray, erasing towards black.
        if (m_Mask)
        {
            Mask& mask = layer.EditMask();
            const float target = tool == BrushTool::Erase ? 0.0f : Luminance(glm::vec3(m_Brush.Color)) * 255.0f;
            for (int y = y0; y <= y1; ++y)
                for (int x = x0; x <= x1; ++x)
                {
                    float weight = DabWeight(m_Brush, glm::vec2(x + 0.5f, y + 0.5f) - c) * flow;
                    if (weight <= 0.0f) continue;
                    size_t i = (size_t)y * w + x;
                    float& cov = m_Coverage[i];
                    float next = m_Brush.Aliased ? std::max(cov, weight) : cov + (1.0f - cov) * weight;
                    if (next <= cov) continue;
                    cov = next;
                    float a = cov * opacity * Sel(i);
                    mask.Values[i] = (uint8_t)std::lround(m_OriginalMask->Values[i] * (1.0f - a) + target * a);
                    m_Changed.Add(x, y);
                    m_Bounds.Add(x, y);
                }
            return;
        }

        Bitmap& pixels = layer.Edit();
        const bool lock = layer.LockTransparency;

        // Smudge, Blur and Sharpen: on the pixels as they are now (a copy of the dab's box, so it reads unchanged ones).
        if (tool == BrushTool::Smudge || tool == BrushTool::Blur || tool == BrushTool::Sharpen)
        {
            const int bw = x1 - x0 + 1, bh = y1 - y0 + 1;
            std::vector<glm::vec4> box((size_t)bw * bh);
            for (int y = y0; y <= y1; ++y)
                for (int x = x0; x <= x1; ++x) box[(size_t)(y - y0) * bw + (x - x0)] = Premultiply(pixels.Pixels[(size_t)y * w + x]);
            auto at = [&](int x, int y) { return box[(size_t)(std::clamp(y, y0, y1) - y0) * bw + (std::clamp(x, x0, x1) - x0)]; };
            // Smudge's held colors, by offset from the dab's center pixel.
            const int cx = (int)std::floor(c.x), cy = (int)std::floor(c.y);
            const int half = (int)std::ceil(r) + 1;
            if (tool == BrushTool::Smudge && !m_HasCarried)
            {
                m_CarriedSide = half * 2 + 1;
                m_Carried.assign((size_t)m_CarriedSide * m_CarriedSide, glm::vec4(0.0f));
                for (int dy = -half; dy <= half; ++dy)
                    for (int dx = -half; dx <= half; ++dx)
                    {
                        int x = std::clamp(cx + dx, 0, w - 1), y = std::clamp(cy + dy, 0, h - 1);
                        m_Carried[(size_t)(dy + half) * m_CarriedSide + (dx + half)] = Premultiply(pixels.Pixels[(size_t)y * w + x]);
                    }
                m_HasCarried = true;
                return; // (the first dab only picks the colors up)
            }
            for (int y = y0; y <= y1; ++y)
                for (int x = x0; x <= x1; ++x)
                {
                    float weight = DabWeight(m_Brush, glm::vec2(x + 0.5f, y + 0.5f) - c) * flow * opacity * Sel((size_t)y * w + x);
                    if (weight <= 0.0f) continue;
                    glm::vec4 cur = at(x, y), next = cur;
                    if (tool == BrushTool::Smudge)
                    {
                        int dx = x - cx, dy = y - cy;
                        if (std::abs(dx) > half || std::abs(dy) > half) continue;
                        glm::vec4& held = m_Carried[(size_t)(dy + half) * m_CarriedSide + (dx + half)];
                        next = glm::mix(cur, held, weight);
                        held = glm::mix(held, next, 0.5f); // (the brush picks up some of what it smears into)
                    }
                    else
                    {
                        glm::vec4 avg(0.0f);
                        for (int ky = -1; ky <= 1; ++ky)
                            for (int kx = -1; kx <= 1; ++kx) avg += at(x + kx, y + ky);
                        avg /= 9.0f;
                        if (tool == BrushTool::Blur) next = glm::mix(cur, avg, weight);
                        else next = glm::clamp(cur + (cur - avg) * weight * 1.5f, glm::vec4(0.0f), glm::vec4(std::max(cur.a, 1e-6f)));
                    }
                    if (lock) next = cur.a > 0.0f && next.a > 0.0f ? next * (cur.a / next.a) : cur;
                    size_t i = (size_t)y * w + x;
                    uint32_t value = Straighten(next);
                    if (value == pixels.Pixels[i]) continue;
                    pixels.Pixels[i] = value;
                    m_Changed.Add(x, y);
                    m_Bounds.Add(x, y);
                }
            return;
        }

        const Bitmap& original = *m_Original;
        const glm::vec2 offset = m_Brush.SourceOffset;
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x)
            {
                float weight = DabWeight(m_Brush, glm::vec2(x + 0.5f, y + 0.5f) - c) * flow;
                if (weight <= 0.0f) continue;
                size_t i = (size_t)y * w + x;
                float& cov = m_Coverage[i];
                float next = m_Brush.Aliased ? std::max(cov, weight) : cov + (1.0f - cov) * weight;
                if (next <= cov) continue;
                const float a = next * opacity * Sel(i);
                const uint32_t under = original.Pixels[i];
                uint32_t value = under;
                switch (tool)
                {
                case BrushTool::Paint: value = PaintOver(under, m_Brush.Color, a, false, lock); break;
                case BrushTool::Erase: value = PaintOver(under, m_Brush.Color, a, true, lock); break;
                case BrushTool::Clone:
                case BrushTool::Heal:
                {
                    if (!m_Brush.Source) continue;
                    int sx = (int)std::floor(x + 0.5f + offset.x), sy = (int)std::floor(y + 0.5f + offset.y);
                    if (sx < 0 || sy < 0 || sx >= (int)m_Brush.Source->Width || sy >= (int)m_Brush.Source->Height) continue;
                    value = PaintOver(under, Unpack(m_Brush.Source->At((uint32_t)sx, (uint32_t)sy)), a, false, lock);
                    break;
                }
                case BrushTool::Dodge:
                case BrushTool::Burn:
                case BrushTool::Sponge:
                {
                    glm::vec4 d = Unpack(under);
                    glm::vec3 col(d);
                    float l = Luminance(col);
                    if (tool == BrushTool::Dodge) col += (1.0f - col) * a * ToneWeight(m_Brush.Range, l);
                    else if (tool == BrushTool::Burn) col -= col * a * ToneWeight(m_Brush.Range, l);
                    else if (m_Brush.Saturate) col = glm::clamp(glm::vec3(l) + (col - l) * (1.0f + a), 0.0f, 1.0f);
                    else col = glm::mix(col, glm::vec3(l), a);
                    value = Pack(glm::vec4(col, d.a));
                    break;
                }
                default: break;
                }
                cov = next;
                pixels.Pixels[i] = value;
                m_Changed.Add(x, y);
                m_Bounds.Add(x, y);
            }
    }

    void Stroke::Finish()
    {
        if (m_Finished) return;
        m_Finished = true;
        if (m_Brush.Tool == BrushTool::Heal && !m_Mask && m_Original && m_Brush.Source && !m_Bounds.Empty()) Heal();
    }

    void Stroke::Heal()
    {
        // The cloned colors' details over the destination's own colors: C - blur(C) + blur(D), laid on by coverage.
        const int w = (int)m_Doc.Width, h = (int)m_Doc.Height;
        const int radius = std::clamp((int)std::lround(m_Brush.Size * 0.3f), 2, 64);
        const int bx0 = std::max(0, m_Bounds.X0 - radius * 2), by0 = std::max(0, m_Bounds.Y0 - radius * 2);
        const int bx1 = std::min(w - 1, m_Bounds.X1 + radius * 2), by1 = std::min(h - 1, m_Bounds.Y1 + radius * 2);
        const int bw = bx1 - bx0 + 1, bh = by1 - by0 + 1;
        const Bitmap& original = *m_Original;
        const Bitmap& source = *m_Brush.Source;
        std::vector<glm::vec3> dst((size_t)bw * bh), src((size_t)bw * bh);
        std::vector<float> srcAlpha((size_t)bw * bh, 0.0f);
        for (int y = by0; y <= by1; ++y)
            for (int x = bx0; x <= bx1; ++x)
            {
                size_t k = (size_t)(y - by0) * bw + (x - bx0);
                glm::vec4 d = Unpack(original.At((uint32_t)x, (uint32_t)y));
                dst[k] = glm::vec3(d);
                int sx = (int)std::floor(x + 0.5f + m_Brush.SourceOffset.x), sy = (int)std::floor(y + 0.5f + m_Brush.SourceOffset.y);
                if (sx >= 0 && sy >= 0 && sx < (int)source.Width && sy < (int)source.Height)
                {
                    glm::vec4 s = Unpack(source.At((uint32_t)sx, (uint32_t)sy));
                    src[k] = glm::vec3(s);
                    srcAlpha[k] = s.a;
                }
                else
                    src[k] = dst[k];
            }
        std::vector<glm::vec3> blurDst = BoxBlur(dst, bw, bh, radius), blurSrc = BoxBlur(src, bw, bh, radius);
        Bitmap& pixels = m_Doc.Layers[(size_t)m_Layer].Edit();
        const bool lock = m_Doc.Layers[(size_t)m_Layer].LockTransparency;
        const float opacity = std::clamp(m_Brush.Opacity, 0.0f, 1.0f);
        for (int y = m_Bounds.Y0; y <= m_Bounds.Y1; ++y)
            for (int x = m_Bounds.X0; x <= m_Bounds.X1; ++x)
            {
                size_t i = (size_t)y * w + x;
                if (m_Coverage[i] <= 0.0f) continue;
                size_t k = (size_t)(y - by0) * bw + (x - bx0);
                glm::vec3 healed = glm::clamp(src[k] - blurSrc[k] + blurDst[k], 0.0f, 1.0f);
                pixels.Pixels[i] = PaintOver(original.Pixels[i], glm::vec4(healed, srcAlpha[k]), m_Coverage[i] * opacity * Sel(i), false, lock);
                m_Changed.Add(x, y);
            }
    }

    Rect Stroke::TakeChanged()
    {
        Rect r = m_Changed;
        m_Changed = {};
        return r;
    }

    std::vector<uint8_t> MatchRegion(const Bitmap& source, glm::ivec2 at, float tolerance, bool contiguous)
    {
        const int w = (int)source.Width, h = (int)source.Height;
        std::vector<uint8_t> mask((size_t)w * h, 0);
        if (at.x < 0 || at.y < 0 || at.x >= w || at.y >= h) return mask;
        const uint32_t seed = source.At((uint32_t)at.x, (uint32_t)at.y);
        const int tol = (int)std::lround(std::clamp(tolerance, 0.0f, 255.0f));
        auto matches = [&](uint32_t p) {
            // Fully transparent pixels all count as one color (whatever their RGB).
            if ((p >> 24) == 0 && (seed >> 24) == 0) return true;
            for (int s = 0; s < 32; s += 8)
                if (std::abs((int)((p >> s) & 0xFF) - (int)((seed >> s) & 0xFF)) > tol) return false;
            return true;
        };
        if (!contiguous)
        {
            for (size_t i = 0; i < mask.size(); ++i) mask[i] = matches(source.Pixels[i]) ? 1 : 0;
            return mask;
        }
        // Scanline flood fill (4-connected).
        std::vector<glm::ivec2> stack = { at };
        while (!stack.empty())
        {
            glm::ivec2 p = stack.back();
            stack.pop_back();
            int x = p.x, y = p.y;
            if (mask[(size_t)y * w + x] || !matches(source.Pixels[(size_t)y * w + x])) continue;
            int left = x, right = x;
            while (left > 0 && !mask[(size_t)y * w + left - 1] && matches(source.Pixels[(size_t)y * w + left - 1])) --left;
            while (right < w - 1 && !mask[(size_t)y * w + right + 1] && matches(source.Pixels[(size_t)y * w + right + 1])) ++right;
            for (int i = left; i <= right; ++i) mask[(size_t)y * w + i] = 1;
            for (int ny : { y - 1, y + 1 })
            {
                if (ny < 0 || ny >= h) continue;
                bool run = false;
                for (int i = left; i <= right; ++i)
                {
                    size_t k = (size_t)ny * w + i;
                    bool open = !mask[k] && matches(source.Pixels[k]);
                    if (open && !run) stack.push_back({ i, ny });
                    run = open;
                }
            }
        }
        return mask;
    }

    Rect Fill(Document& doc, int layer, glm::ivec2 at, const FillSettings& fill)
    {
        Rect changed;
        if (layer < 0 || layer >= (int)doc.Layers.size() || !doc.Layers[(size_t)layer].HasPixels()) return changed;
        Layer& l = doc.Layers[(size_t)layer];
        std::vector<uint8_t> mask;
        if (fill.SampleAllLayers) mask = MatchRegion(doc.Flatten(), at, fill.Tolerance, fill.Contiguous);
        else mask = MatchRegion(l.Get(), at, fill.Tolerance, fill.Contiguous);
        Bitmap& pixels = l.Edit();
        const float amount = std::clamp(fill.Opacity, 0.0f, 1.0f);
        for (int y = 0; y < (int)doc.Height; ++y)
            for (int x = 0; x < (int)doc.Width; ++x)
            {
                size_t i = (size_t)y * doc.Width + x;
                float sel = fill.Selection ? fill.Selection->Values[i] / 255.0f : 1.0f;
                if (!mask[i] || sel <= 0.0f) continue;
                uint32_t before = pixels.Pixels[i];
                pixels.Pixels[i] = PaintOver(before, fill.Color, amount * sel, false, l.LockTransparency);
                if (pixels.Pixels[i] != before) changed.Add(x, y);
            }
        return changed;
    }

    const char* GradientShapeName(GradientShape shape)
    {
        static constexpr const char* kNames[] = { "Linear", "Radial", "Angle", "Reflected", "Diamond" };
        return (size_t)shape < std::size(kNames) ? kNames[(size_t)shape] : "Linear";
    }

    float GradientAt(GradientShape shape, const glm::vec2& a, const glm::vec2& b, const glm::vec2& p)
    {
        glm::vec2 axis = b - a;
        float len = glm::length(axis);
        if (len < 1e-4f) return 0.0f;
        glm::vec2 dir = axis / len, d = p - a;
        switch (shape)
        {
        case GradientShape::Radial: return std::clamp(glm::length(d) / len, 0.0f, 1.0f);
        case GradientShape::Angle:
        {
            // Around `a`, starting along the drag and going clockwise on screen.
            float ang = std::atan2(d.x * dir.y - d.y * dir.x, glm::dot(d, dir));
            return ang < 0.0f ? -ang / glm::two_pi<float>() : 1.0f - ang / glm::two_pi<float>();
        }
        case GradientShape::Reflected: return std::clamp(std::abs(glm::dot(d, dir)) / len, 0.0f, 1.0f);
        case GradientShape::Diamond:
        {
            glm::vec2 perp(-dir.y, dir.x);
            return std::clamp((std::abs(glm::dot(d, dir)) + std::abs(glm::dot(d, perp))) / len, 0.0f, 1.0f);
        }
        default: return std::clamp(glm::dot(d, dir) / len, 0.0f, 1.0f);
        }
    }

    Rect DrawGradient(Document& doc, int layer, const glm::vec2& a, const glm::vec2& b, const GradientSettings& g, bool mask)
    {
        Rect changed;
        if (layer < 0 || layer >= (int)doc.Layers.size()) return changed;
        Layer& l = doc.Layers[(size_t)layer];
        if (mask ? !l.LayerMask : l.IsGroup() || !l.Pixels) return changed;
        const glm::vec4 from = g.Reverse ? g.To : g.From, to = g.Reverse ? g.From : g.To;
        const glm::vec4 pf(glm::vec3(from) * from.a, from.a), pt(glm::vec3(to) * to.a, to.a);
        const float amount = std::clamp(g.Opacity, 0.0f, 1.0f);
        const int w = (int)doc.Width, h = (int)doc.Height;
        Bitmap* pixels = mask ? nullptr : &l.Edit();
        Mask* values = mask ? &l.EditMask() : nullptr;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
            {
                float t = GradientAt(g.Shape, a, b, glm::vec2(x + 0.5f, y + 0.5f));
                glm::vec4 c = glm::mix(pf, pt, t); // (mixed premultiplied: towards transparent keeps the color)
                glm::vec4 color = c.a > 0.0f ? glm::vec4(glm::vec3(c) / c.a, c.a) : glm::vec4(glm::vec3(from), 0.0f);
                if (g.Dither) color = glm::vec4(glm::vec3(color) + Noise(x, y) / 255.0f, color.a);
                size_t i = (size_t)y * w + x;
                const float sel = g.Selection ? g.Selection->Values[i] / 255.0f : 1.0f;
                if (sel <= 0.0f) continue;
                if (values)
                {
                    float target = Luminance(glm::vec3(color)) * 255.0f;
                    uint8_t v = (uint8_t)std::lround(std::clamp(values->Values[i] * (1.0f - amount * sel) + target * amount * sel, 0.0f, 255.0f));
                    if (v != values->Values[i]) values->Values[i] = v, changed.Add(x, y);
                    continue;
                }
                uint32_t before = pixels->Pixels[i];
                pixels->Pixels[i] = PaintOver(before, color, amount * sel, false, l.LockTransparency);
                if (pixels->Pixels[i] != before) changed.Add(x, y);
            }
        return changed;
    }

    glm::vec4 SampleColor(const Bitmap& image, glm::ivec2 at, int radius)
    {
        glm::vec4 sum(0.0f);
        float alpha = 0.0f;
        int count = 0;
        for (int y = at.y - radius; y <= at.y + radius; ++y)
            for (int x = at.x - radius; x <= at.x + radius; ++x)
            {
                if (x < 0 || y < 0 || x >= (int)image.Width || y >= (int)image.Height) continue;
                glm::vec4 c = Unpack(image.At((uint32_t)x, (uint32_t)y));
                sum += glm::vec4(glm::vec3(c) * c.a, 0.0f);
                alpha += c.a;
                ++count;
            }
        if (count == 0) return glm::vec4(0.0f);
        glm::vec3 color = alpha > 0.0f ? glm::vec3(sum) / alpha : glm::vec3(0.0f);
        return glm::vec4(color, alpha / count);
    }
}
