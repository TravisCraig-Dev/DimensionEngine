#include "Dimension/Image/ImageDocument.h"
#include "Dimension/Image/ImageFilters.h"
#include <nlohmann/json.hpp>
#include <stb_image.h>
#include <stb_image_write.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <thread>
#include <unordered_map>

namespace Dimension::Imaging
{
    namespace
    {
        constexpr const char* kNames[] = { "Normal",      "Dissolve",     "Darken",     "Multiply",    "Color Burn", "Linear Burn",
                                           "Darker Color", "Lighten",     "Screen",     "Color Dodge", "Linear Dodge (Add)",
                                           "Lighter Color", "Overlay",    "Soft Light", "Hard Light",  "Vivid Light", "Linear Light",
                                           "Pin Light",   "Hard Mix",     "Difference", "Exclusion",   "Subtract",    "Divide",
                                           "Hue",         "Saturation",   "Color",      "Luminosity",  "Pass Through" };
        static_assert(std::size(kNames) == (size_t)LayerBlend::Count);

        // The non-separable modes' helpers (the W3C compositing spec, which follows Photoshop's).
        float Lum(const glm::vec3& c) { return 0.3f * c.r + 0.59f * c.g + 0.11f * c.b; }
        glm::vec3 ClipColor(glm::vec3 c)
        {
            float l = Lum(c);
            float n = std::min({ c.r, c.g, c.b }), x = std::max({ c.r, c.g, c.b });
            if (n < 0.0f) c = l + (c - l) * l / std::max(l - n, 1e-6f);
            if (x > 1.0f) c = l + (c - l) * (1.0f - l) / std::max(x - l, 1e-6f);
            return c;
        }
        glm::vec3 SetLum(const glm::vec3& c, float l) { return ClipColor(c + (l - Lum(c))); }
        float Sat(const glm::vec3& c) { return std::max({ c.r, c.g, c.b }) - std::min({ c.r, c.g, c.b }); }
        glm::vec3 SetSat(glm::vec3 c, float s)
        {
            float* v[3] = { &c.r, &c.g, &c.b };
            std::sort(v, v + 3, [](float* a, float* b) { return *a < *b; });
            float& mn = *v[0];
            float& mid = *v[1];
            float& mx = *v[2];
            if (mx > mn)
            {
                mid = (mid - mn) * s / (mx - mn);
                mx = s;
            }
            else
                mid = mx = 0.0f;
            mn = 0.0f;
            return c;
        }

        float ColorDodge(float b, float s) { return b <= 0.0f ? 0.0f : s >= 1.0f ? 1.0f : std::min(1.0f, b / (1.0f - s)); }
        float ColorBurn(float b, float s) { return b >= 1.0f ? 1.0f : s <= 0.0f ? 0.0f : 1.0f - std::min(1.0f, (1.0f - b) / s); }
        float HardLight(float b, float s) { return s <= 0.5f ? b * 2.0f * s : 1.0f - (1.0f - b) * (1.0f - (2.0f * s - 1.0f)); }
        float SoftLight(float b, float s)
        {
            if (s <= 0.5f) return b - (1.0f - 2.0f * s) * b * (1.0f - b);
            float d = b <= 0.25f ? ((16.0f * b - 12.0f) * b + 4.0f) * b : std::sqrt(b);
            return b + (2.0f * s - 1.0f) * (d - b);
        }
        float Separable(LayerBlend mode, float b, float s)
        {
            switch (mode)
            {
            case LayerBlend::Darken: return std::min(b, s);
            case LayerBlend::Multiply: return b * s;
            case LayerBlend::ColorBurn: return ColorBurn(b, s);
            case LayerBlend::LinearBurn: return std::max(0.0f, b + s - 1.0f);
            case LayerBlend::Lighten: return std::max(b, s);
            case LayerBlend::Screen: return b + s - b * s;
            case LayerBlend::ColorDodge: return ColorDodge(b, s);
            case LayerBlend::LinearDodge: return std::min(1.0f, b + s);
            case LayerBlend::Overlay: return HardLight(s, b);
            case LayerBlend::SoftLight: return SoftLight(b, s);
            case LayerBlend::HardLight: return HardLight(b, s);
            case LayerBlend::VividLight: return s <= 0.5f ? ColorBurn(b, 2.0f * s) : ColorDodge(b, 2.0f * (s - 0.5f));
            case LayerBlend::LinearLight: return std::clamp(b + 2.0f * s - 1.0f, 0.0f, 1.0f);
            case LayerBlend::PinLight: return s <= 0.5f ? std::min(b, 2.0f * s) : std::max(b, 2.0f * s - 1.0f);
            case LayerBlend::HardMix: return b + s >= 1.0f ? 1.0f : 0.0f;
            case LayerBlend::Difference: return std::abs(b - s);
            case LayerBlend::Exclusion: return b + s - 2.0f * b * s;
            case LayerBlend::Subtract: return std::max(0.0f, b - s);
            case LayerBlend::Divide: return s <= 0.0f ? (b <= 0.0f ? 0.0f : 1.0f) : std::min(1.0f, b / s);
            default: return s;
            }
        }

        // Dissolve's per-pixel threshold (stable for a pixel, so the pattern does not crawl as things change).
        float Hash(uint32_t x, uint32_t y)
        {
            uint32_t h = x * 374761393u + y * 668265263u;
            h = (h ^ (h >> 13)) * 1274126177u;
            h ^= h >> 16;
            return (h & 0xFFFFFF) / float(0x1000000);
        }

        // `cs` at alpha `as` laid over the premultiplied backdrop `d` with the mode.
        inline void BlendPixel(glm::vec4& d, const glm::vec3& cs, float as, LayerBlend mode, uint32_t x, uint32_t y)
        {
            if (mode == LayerBlend::Dissolve) as = Hash(x, y) < as ? 1.0f : 0.0f;
            if (as <= 0.0f) return;
            float ab = d.a;
            glm::vec3 mixed = cs;
            if (ab > 0.0f && mode != LayerBlend::Normal && mode != LayerBlend::Dissolve && mode != LayerBlend::PassThrough)
            {
                glm::vec3 cb = glm::vec3(d) / ab;
                mixed = (1.0f - ab) * cs + ab * glm::clamp(BlendColors(mode, cb, cs), 0.0f, 1.0f);
            }
            d = glm::vec4(as * mixed + (1.0f - as) * glm::vec3(d), as + ab * (1.0f - as));
        }

        inline float MaskAt(const Layer& l, size_t i) { return l.LayerMask && l.MaskEnabled ? l.LayerMask->Values[i] * (1.0f / 255.0f) : 1.0f; }

        // Rows [y0, y1] split over the machine's threads when there are enough of them to be worth it.
        template <typename F>
        void ParallelRows(int y0, int y1, size_t pixelsPerRow, F&& rows)
        {
            int count = y1 - y0 + 1;
            if (count <= 0) return;
            unsigned threads = std::max(1u, std::thread::hardware_concurrency());
            size_t work = (size_t)count * pixelsPerRow;
            if (threads == 1 || work < 64 * 1024)
            {
                rows(y0, y1);
                return;
            }
            threads = std::min<unsigned>(threads, (unsigned)count);
            std::vector<std::thread> pool;
            int per = (count + (int)threads - 1) / (int)threads;
            for (int start = y0; start <= y1; start += per)
            {
                int end = std::min(y1, start + per - 1);
                pool.emplace_back([&rows, start, end] { rows(start, end); });
            }
            for (auto& t : pool) t.join();
        }

        // One row's compositing of a group's (or the document's) contents into premultiplied colors.
        struct RowCompositor
        {
            const Document& Doc;
            std::unordered_map<uint32_t, std::vector<int>> Kids;
            int X0 = 0, Width = 0;
            std::vector<PreparedAdjustment> Prepared;                 // (adjustment layers)
            std::vector<std::shared_ptr<const StyledLayer>> Styles;   // (layers with effects)

            void Children(uint32_t parent, glm::vec4* row, int y) const
            {
                auto it = Kids.find(parent);
                if (it == Kids.end()) return;
                for (int index : it->second) Draw(index, row, y);
            }

            void Draw(int index, glm::vec4* row, int y) const
            {
                const Imaging::Layer& l = Doc.Layers[(size_t)index];
                if (!l.Visible || l.Opacity <= 0.0f) return;
                const size_t base = (size_t)y * Doc.Width + X0;
                if (l.IsAdjustment())
                {
                    // What is under it (so far, in its group) changed, faded by its opacity and mask, mixed by its mode.
                    const PreparedAdjustment& pa = Prepared[(size_t)index];
                    const bool plain = l.Blend == LayerBlend::Normal || l.Blend == LayerBlend::PassThrough || l.Blend == LayerBlend::Dissolve;
                    for (int i = 0; i < Width; ++i)
                    {
                        glm::vec4& d = row[i];
                        float f = l.Opacity * MaskAt(l, base + i);
                        if (d.a <= 0.0f || f <= 0.0f) continue;
                        glm::vec3 cb = glm::vec3(d) / d.a, cs = Adjust(pa, cb);
                        glm::vec3 mixed = plain ? cs : glm::clamp(BlendColors(l.Blend, cb, cs), 0.0f, 1.0f);
                        d = glm::vec4(glm::mix(cb, mixed, std::min(f, 1.0f)) * d.a, d.a);
                    }
                    return;
                }
                if (l.IsGroup())
                {
                    if (l.Blend == LayerBlend::PassThrough)
                    {
                        std::vector<glm::vec4> before(row, row + Width);
                        Children(l.Id, row, y);
                        for (int i = 0; i < Width; ++i)
                            if (float f = l.Opacity * MaskAt(l, base + i); f < 1.0f) row[i] = glm::mix(before[(size_t)i], row[i], f);
                        return;
                    }
                    std::vector<glm::vec4> group((size_t)Width, glm::vec4(0.0f));
                    Children(l.Id, group.data(), y);
                    for (int i = 0; i < Width; ++i)
                    {
                        const glm::vec4& g = group[(size_t)i];
                        if (g.a <= 0.0f) continue;
                        BlendPixel(row[i], glm::vec3(g) / g.a, g.a * l.Opacity * MaskAt(l, base + i), l.Blend, (uint32_t)(X0 + i), (uint32_t)y);
                    }
                    return;
                }
                if (!l.Pixels || l.Pixels->Width != Doc.Width || l.Pixels->Height != Doc.Height) return;
                // Effects: shadows, glows and the outside stroke first; then the pixels with the overlays on them.
                const StyledLayer* style = Styles.empty() ? nullptr : Styles[(size_t)index].get();
                if (style)
                    for (const StyledLayer::Under& u : style->Below)
                        for (int i = 0; i < Width; ++i)
                            if (uint8_t a = u.Alpha[base + i])
                                BlendPixel(row[i], u.Color, a / 255.0f * u.Opacity * l.Opacity, u.Mode, (uint32_t)(X0 + i), (uint32_t)y);
                const uint32_t* src = (style ? style->Content.Pixels.data() : l.Pixels->Pixels.data()) + base;
                for (int i = 0; i < Width; ++i)
                {
                    uint32_t p = src[i];
                    if ((p >> 24) == 0) continue;
                    glm::vec4 s = Unpack(p);
                    BlendPixel(row[i], glm::vec3(s), s.a * l.Opacity * MaskAt(l, base + i), l.Blend, (uint32_t)(X0 + i), (uint32_t)y);
                }
            }

            // The layer by itself: its pixels (or contents) with its mask, at full opacity and Normal.
            void Alone(const Imaging::Layer& l, glm::vec4* row, int y) const
            {
                const size_t base = (size_t)y * Doc.Width + X0;
                if (l.IsGroup()) Children(l.Id, row, y);
                else if (l.IsRaster() && l.Pixels && l.Pixels->Width == Doc.Width && l.Pixels->Height == Doc.Height)
                    for (int i = 0; i < Width; ++i)
                    {
                        glm::vec4 s = Unpack(l.Pixels->Pixels[base + i]);
                        row[i] = glm::vec4(glm::vec3(s) * s.a, s.a);
                    }
                for (int i = 0; i < Width; ++i) row[i] *= MaskAt(l, base + i);
            }
        };

        void WriteU32(std::vector<uint8_t>& out, uint32_t v)
        {
            for (int i = 0; i < 4; ++i) out.push_back((uint8_t)(v >> (8 * i)));
        }
        bool ReadU32(const std::vector<uint8_t>& in, size_t& at, uint32_t& v)
        {
            if (at + 4 > in.size()) return false;
            v = 0;
            for (int i = 0; i < 4; ++i) v |= (uint32_t)in[at + i] << (8 * i);
            at += 4;
            return true;
        }
        void AppendBytes(void* context, void* data, int size)
        {
            auto* out = (std::vector<uint8_t>*)context;
            out->insert(out->end(), (uint8_t*)data, (uint8_t*)data + size);
        }
        // A PNG blob of `channels` (1 or 4) into a canvas-sized buffer (copied from the top-left should sizes differ).
        template <typename T>
        void DecodeInto(const uint8_t* data, size_t size, int channels, uint32_t width, uint32_t height, std::vector<T>& out)
        {
            int w = 0, h = 0, c = 0;
            stbi_uc* pixels = stbi_load_from_memory(data, (int)size, &w, &h, &c, channels);
            if (!pixels) return;
            for (int y = 0; y < std::min(h, (int)height); ++y)
                std::memcpy((uint8_t*)out.data() + (size_t)y * width * channels, pixels + (size_t)y * w * channels,
                            (size_t)std::min(w, (int)width) * channels);
            stbi_image_free(pixels);
        }
    }

    const char* LayerBlendName(LayerBlend mode) { return (size_t)mode < std::size(kNames) ? kNames[(size_t)mode] : "Normal"; }

    LayerBlend LayerBlendFromName(const std::string& name)
    {
        for (size_t i = 0; i < std::size(kNames); ++i)
            if (name == kNames[i]) return (LayerBlend)i;
        return LayerBlend::Normal;
    }

    bool LayerBlendStartsGroup(LayerBlend mode)
    {
        switch (mode)
        {
        case LayerBlend::Darken:
        case LayerBlend::Lighten:
        case LayerBlend::Overlay:
        case LayerBlend::Difference:
        case LayerBlend::Hue: return true;
        default: return false;
        }
    }

    glm::vec3 BlendColors(LayerBlend mode, const glm::vec3& b, const glm::vec3& s)
    {
        switch (mode)
        {
        case LayerBlend::Normal:
        case LayerBlend::Dissolve:
        case LayerBlend::PassThrough: return s;
        case LayerBlend::DarkerColor: return Lum(s) < Lum(b) ? s : b;
        case LayerBlend::LighterColor: return Lum(s) > Lum(b) ? s : b;
        case LayerBlend::Hue: return SetLum(SetSat(s, Sat(b)), Lum(b));
        case LayerBlend::Saturation: return SetLum(SetSat(b, Sat(s)), Lum(b));
        case LayerBlend::Color: return SetLum(s, Lum(b));
        case LayerBlend::Luminosity: return SetLum(b, Lum(s));
        default: return { Separable(mode, b.r, s.r), Separable(mode, b.g, s.g), Separable(mode, b.b, s.b) };
        }
    }

    glm::vec4 Unpack(uint32_t c) { return glm::vec4(c & 0xFF, (c >> 8) & 0xFF, (c >> 16) & 0xFF, c >> 24) * (1.0f / 255.0f); }

    uint32_t Pack(const glm::vec4& c)
    {
        auto q = [](float v) { return (uint32_t)std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f); };
        return q(c.r) | (q(c.g) << 8) | (q(c.b) << 16) | (q(c.a) << 24);
    }

    float Luminance(const glm::vec3& c) { return 0.299f * c.r + 0.587f * c.g + 0.114f * c.b; }

    Mask Mask::Solid(uint32_t width, uint32_t height, uint8_t value)
    {
        Mask m;
        m.Width = width;
        m.Height = height;
        m.Values.assign((size_t)width * height, value);
        return m;
    }

    Bitmap& Layer::Edit()
    {
        if (!Pixels) Pixels = std::make_shared<Bitmap>();
        else if (Pixels.use_count() > 1) Pixels = std::make_shared<Bitmap>(*Pixels);
        return const_cast<Bitmap&>(*Pixels);
    }

    Mask& Layer::EditMask()
    {
        if (!LayerMask) LayerMask = std::make_shared<Mask>();
        else if (LayerMask.use_count() > 1) LayerMask = std::make_shared<Mask>(*LayerMask);
        return const_cast<Mask&>(*LayerMask);
    }

    Document Document::Create(uint32_t width, uint32_t height, const glm::vec4& background)
    {
        Document doc;
        doc.Width = std::max(width, 1u);
        doc.Height = std::max(height, 1u);
        Layer bg = doc.MakeLayer("Background");
        bg.Pixels = std::make_shared<Bitmap>(Bitmap::Solid(doc.Width, doc.Height, background.a > 0.0f ? Pack(background) : 0u));
        doc.Layers.push_back(std::move(bg));
        return doc;
    }

    Document Document::FromBitmap(Bitmap image)
    {
        Document doc;
        doc.Width = image.Width;
        doc.Height = image.Height;
        Layer bg;
        bg.Id = doc.NextId++;
        bg.Name = "Background";
        bg.Pixels = std::make_shared<Bitmap>(std::move(image));
        doc.Layers.push_back(std::move(bg));
        return doc;
    }

    // ---- The tree

    int Document::IndexOf(uint32_t id) const
    {
        for (size_t i = 0; i < Layers.size(); ++i)
            if (Layers[i].Id == id) return (int)i;
        return -1;
    }

    std::vector<int> Document::Children(uint32_t parent) const
    {
        std::vector<int> out;
        for (size_t i = 0; i < Layers.size(); ++i)
            if (Layers[i].Parent == parent) out.push_back((int)i);
        return out;
    }

    bool Document::IsInside(int index, uint32_t group) const
    {
        if (index < 0 || index >= (int)Layers.size() || group == 0) return false;
        uint32_t p = Layers[(size_t)index].Parent;
        for (int guard = 0; p != 0 && guard < 1000; ++guard)
        {
            if (p == group) return true;
            int i = IndexOf(p);
            if (i < 0) return false;
            p = Layers[(size_t)i].Parent;
        }
        return false;
    }

    int Document::Depth(int index) const
    {
        int depth = 0;
        for (uint32_t p = index >= 0 && index < (int)Layers.size() ? Layers[(size_t)index].Parent : 0; p != 0 && depth < 1000; ++depth)
        {
            int i = IndexOf(p);
            if (i < 0) break;
            p = Layers[(size_t)i].Parent;
        }
        return depth;
    }

    bool Document::ShownInTree(int index) const
    {
        for (int guard = 0; index >= 0 && guard < 1000; ++guard)
        {
            if (!Layers[(size_t)index].Visible) return false;
            uint32_t p = Layers[(size_t)index].Parent;
            if (p == 0) return true;
            index = IndexOf(p);
        }
        return index >= 0;
    }

    int Document::Below(int index) const
    {
        if (index < 0 || index >= (int)Layers.size()) return -1;
        int found = -1;
        for (int i = 0; i < index; ++i)
            if (Layers[(size_t)i].Parent == Layers[(size_t)index].Parent) found = i;
        return found;
    }

    int Document::Above(int index) const
    {
        if (index < 0 || index >= (int)Layers.size()) return -1;
        for (int i = index + 1; i < (int)Layers.size(); ++i)
            if (Layers[(size_t)i].Parent == Layers[(size_t)index].Parent) return i;
        return -1;
    }

    Layer Document::MakeLayer(const std::string& name)
    {
        Layer l;
        l.Id = NextId++;
        l.Name = name;
        l.Pixels = std::make_shared<Bitmap>(Bitmap::Solid(Width, Height, 0u));
        return l;
    }

    int Document::AddLayer(const std::string& name, int index, uint32_t parent)
    {
        index = std::clamp(index, 0, (int)Layers.size());
        Layer l = MakeLayer(name);
        l.Parent = parent;
        Layers.insert(Layers.begin() + index, std::move(l));
        return index;
    }

    int Document::AddAdjustment(const Imaging::Adjustment& adjustment, int index, uint32_t parent)
    {
        index = std::clamp(index, 0, (int)Layers.size());
        Layer l;
        l.Id = NextId++;
        l.Kind = LayerKind::Adjustment;
        l.Name = UniqueLayerName(AdjustmentName(adjustment.Type) + std::string(" 1"));
        l.Parent = parent;
        l.Adjust = adjustment;
        l.LayerMask = std::make_shared<Mask>(Mask::Solid(Width, Height, 255));
        Layers.insert(Layers.begin() + index, std::move(l));
        return index;
    }

    int Document::AddText(const TextData& text, int index, uint32_t parent)
    {
        index = std::clamp(index, 0, (int)Layers.size());
        Layer l;
        l.Id = NextId++;
        l.Kind = LayerKind::Text;
        std::string name = text.Text.substr(0, std::min(text.Text.find('\n'), (size_t)30));
        l.Name = UniqueLayerName(name.empty() ? "Text" : name);
        l.Parent = parent;
        l.Text = text;
        Layers.insert(Layers.begin() + index, std::move(l));
        return index;
    }

    int Document::AddShape(const ShapeData& shape, int index, uint32_t parent)
    {
        index = std::clamp(index, 0, (int)Layers.size());
        Layer l;
        l.Id = NextId++;
        l.Kind = LayerKind::Shape;
        l.Name = UniqueLayerName(ShapeKindName(shape.Kind) + std::string(" 1"));
        l.Parent = parent;
        l.Shape = shape;
        Layers.insert(Layers.begin() + index, std::move(l));
        return index;
    }

    void Document::Rasterize(int index)
    {
        if (index < 0 || index >= (int)Layers.size() || !Layers[(size_t)index].IsVector()) return;
        RefreshVectors();
        Layers[(size_t)index].Kind = LayerKind::Pixels;
        Layers[(size_t)index].VectorKey = 0;
    }

    void Document::PrepareStyles() const
    {
        RefreshVectors();
        for (const Layer& l : Layers)
        {
            if (!l.IsRaster() || !l.Pixels || !l.Effects.Any()) continue;
            const void* maskKey = l.MaskEnabled ? l.LayerMask.get() : nullptr;
            if (!l.Styled || l.Styled->Pixels.get() != l.Pixels.get() || l.Styled->MaskKey.get() != maskKey || l.Styled->Key != l.Effects.Hash())
                l.Styled = RenderEffects(l, Width, Height);
        }
    }

    void Document::RefreshVectors() const
    {
        for (const Layer& l : Layers)
        {
            if (!l.IsVector()) continue;
            uint64_t key = 1469598103934665603ull;
            std::string settings = (l.Kind == LayerKind::Text ? ToJson(l.Text) : ToJson(l.Shape)).dump();
            for (unsigned char c : settings) key = (key ^ c) * 1099511628211ull;
            key ^= ((uint64_t)Width << 32) ^ Height;
            if (l.Pixels && l.VectorKey == key) continue;
            l.Pixels = std::make_shared<Bitmap>(l.Kind == LayerKind::Text ? RenderText(l.Text, Width, Height) : RenderShape(l.Shape, Width, Height));
            l.VectorKey = key;
        }
    }

    int Document::GroupLayer(int index)
    {
        if (index < 0 || index >= (int)Layers.size()) return -1;
        Layer group;
        group.Id = NextId++;
        group.Kind = LayerKind::Group;
        group.Name = UniqueLayerName("Group 1");
        group.Blend = LayerBlend::PassThrough;
        group.Parent = Layers[(size_t)index].Parent;
        Layers[(size_t)index].Parent = group.Id;
        Layers.insert(Layers.begin() + index + 1, std::move(group)); // (in the layer's place among its siblings)
        return index + 1;
    }

    int Document::Ungroup(int index)
    {
        if (index < 0 || index >= (int)Layers.size() || !Layers[(size_t)index].IsGroup()) return -1;
        const uint32_t id = Layers[(size_t)index].Id, parent = Layers[(size_t)index].Parent;
        std::vector<Layer> kids;
        int before = 0;
        for (int i = (int)Layers.size() - 1; i >= 0; --i)
            if (Layers[(size_t)i].Parent == id)
            {
                kids.insert(kids.begin(), std::move(Layers[(size_t)i]));
                kids.front().Parent = parent;
                if (i < index) ++before;
                Layers.erase(Layers.begin() + i);
            }
        int at = index - before;
        Layers.erase(Layers.begin() + at); // (the group)
        int count = (int)kids.size();
        Layers.insert(Layers.begin() + at, std::make_move_iterator(kids.begin()), std::make_move_iterator(kids.end()));
        return count > 0 ? at + count - 1 : -1;
    }

    int Document::DuplicateLayer(int index)
    {
        if (index < 0 || index >= (int)Layers.size()) return -1;
        Layer copy = Layers[(size_t)index]; // (shares the pixels until one is painted)
        const uint32_t original = copy.Id;
        copy.Id = NextId++;
        copy.Name = UniqueLayerName(Layers[(size_t)index].Name + " copy");
        std::vector<Layer> inside;
        if (copy.IsGroup())
        {
            std::unordered_map<uint32_t, uint32_t> ids = { { original, copy.Id } };
            for (size_t i = 0; i < Layers.size(); ++i)
                if (IsInside((int)i, original)) inside.push_back(Layers[i]);
            for (Layer& l : inside) ids[l.Id] = NextId++;
            for (Layer& l : inside)
            {
                l.Id = ids[l.Id];
                l.Parent = ids[l.Parent];
            }
        }
        Layers.insert(Layers.begin() + index + 1, std::move(copy));
        Layers.insert(Layers.begin() + index + 2, inside.begin(), inside.end());
        return index + 1;
    }

    void Document::RemoveLayer(int index)
    {
        if (index < 0 || index >= (int)Layers.size()) return;
        const uint32_t id = Layers[(size_t)index].Id;
        // (found first: erasing breaks the parent chains IsInside follows)
        std::vector<uint8_t> gone(Layers.size(), 0);
        for (int i = 0; i < (int)Layers.size(); ++i) gone[(size_t)i] = i == index || IsInside(i, id);
        for (int i = (int)Layers.size() - 1; i >= 0; --i)
            if (gone[(size_t)i]) Layers.erase(Layers.begin() + i);
    }

    void Document::MoveLayer(int from, int to)
    {
        if (from < 0 || from >= (int)Layers.size()) return;
        to = std::clamp(to, 0, (int)Layers.size() - 1);
        if (from == to) return;
        Layer l = std::move(Layers[(size_t)from]);
        Layers.erase(Layers.begin() + from);
        Layers.insert(Layers.begin() + to, std::move(l));
    }

    int Document::MoveInto(int index, uint32_t parent, int over)
    {
        if (index < 0 || index >= (int)Layers.size()) return -1;
        const uint32_t id = Layers[(size_t)index].Id;
        if (parent == id) return -1;
        if (parent != 0)
        {
            int p = IndexOf(parent);
            if (p < 0 || !Layers[(size_t)p].IsGroup() || IsInside(p, id)) return -1;
        }
        const uint32_t overId = over >= 0 && over < (int)Layers.size() ? Layers[(size_t)over].Id : 0;
        if (overId == id) return index;
        Layer l = std::move(Layers[(size_t)index]);
        Layers.erase(Layers.begin() + index);
        l.Parent = parent;
        int at;
        if (overId != 0) at = IndexOf(overId) + 1;
        else
        {
            // At the bottom: before the parent's first child (or before the group itself; at the start at the top level).
            std::vector<int> kids = Children(parent);
            at = !kids.empty() ? kids.front() : parent != 0 ? IndexOf(parent) : 0;
        }
        at = std::clamp(at, 0, (int)Layers.size());
        Layers.insert(Layers.begin() + at, std::move(l));
        return at;
    }

    int Document::Step(int index, int direction)
    {
        if (index < 0 || index >= (int)Layers.size()) return index;
        std::vector<int> siblings = Children(Layers[(size_t)index].Parent);
        int pos = (int)(std::find(siblings.begin(), siblings.end(), index) - siblings.begin());
        int target = pos + (direction > 0 ? 1 : -1);
        if (target < 0 || target >= (int)siblings.size()) return index;
        const uint32_t parent = Layers[(size_t)index].Parent;
        if (direction > 0) return MoveInto(index, parent, siblings[(size_t)target]);
        return MoveInto(index, parent, target - 1 >= 0 ? siblings[(size_t)target - 1] : -1);
    }

    int Document::MergeDown(int index)
    {
        int lowerIndex = Below(index);
        if (lowerIndex < 0 || Layers[(size_t)index].IsGroup() || !Layers[(size_t)lowerIndex].HasPixels()) return index;
        // The two alone: the lower one (its mask applied) as the backdrop at full strength; it keeps its opacity and mode.
        Document pair;
        pair.Width = Width;
        pair.Height = Height;
        Layer base = Layers[(size_t)lowerIndex];
        base.Opacity = 1.0f;
        base.Blend = LayerBlend::Normal;
        base.Visible = true;
        base.Parent = 0;
        Layer upper = Layers[(size_t)index];
        upper.Parent = 0;
        pair.Layers.push_back(std::move(base));
        if (upper.Visible) pair.Layers.push_back(std::move(upper));
        Layer& lower = Layers[(size_t)lowerIndex];
        lower.Pixels = std::make_shared<Bitmap>(pair.Flatten());
        lower.LayerMask.reset();
        lower.Effects = LayerEffects(); // (baked in)
        lower.Styled.reset();
        Layers.erase(Layers.begin() + index);
        return lowerIndex;
    }

    int Document::MergeGroup(int index)
    {
        if (index < 0 || index >= (int)Layers.size() || !Layers[(size_t)index].IsGroup()) return index;
        Bitmap pixels = Render(index);
        const uint32_t id = Layers[(size_t)index].Id;
        Layer& g = Layers[(size_t)index];
        g.Kind = LayerKind::Pixels;
        g.Pixels = std::make_shared<Bitmap>(std::move(pixels));
        g.LayerMask.reset();
        if (g.Blend == LayerBlend::PassThrough) g.Blend = LayerBlend::Normal;
        g.Effects = LayerEffects();
        std::vector<uint8_t> gone(Layers.size(), 0);
        for (int i = 0; i < (int)Layers.size(); ++i) gone[(size_t)i] = IsInside(i, id);
        for (int i = (int)Layers.size() - 1; i >= 0; --i)
            if (gone[(size_t)i]) Layers.erase(Layers.begin() + i);
        return IndexOf(id);
    }

    std::string Document::UniqueLayerName(const std::string& base) const
    {
        auto taken = [&](const std::string& n) { return std::any_of(Layers.begin(), Layers.end(), [&](const Layer& l) { return l.Name == n; }); };
        if (!taken(base)) return base;
        // "Layer 1" -> "Layer 2": a number at the end counts on.
        std::string stem = base;
        int start = 2;
        if (size_t sp = base.find_last_of(' '); sp != std::string::npos && sp + 1 < base.size() &&
                                                 base.find_first_not_of("0123456789", sp + 1) == std::string::npos)
        {
            stem = base.substr(0, sp);
            start = std::stoi(base.substr(sp + 1)) + 1;
        }
        for (int i = start;; ++i)
            if (std::string n = stem + " " + std::to_string(i); !taken(n)) return n;
    }

    // ---- Masks

    void Document::AddMask(int index, bool revealAll)
    {
        if (index < 0 || index >= (int)Layers.size()) return;
        Layers[(size_t)index].LayerMask = std::make_shared<Mask>(Mask::Solid(Width, Height, revealAll ? 255 : 0));
        Layers[(size_t)index].MaskEnabled = true;
    }

    void Document::RemoveMask(int index)
    {
        if (index >= 0 && index < (int)Layers.size()) Layers[(size_t)index].LayerMask.reset();
    }

    void Document::ApplyMask(int index)
    {
        if (index < 0 || index >= (int)Layers.size()) return;
        Layer& l = Layers[(size_t)index];
        if (!l.LayerMask || l.IsGroup()) return;
        if (l.MaskEnabled)
        {
            Bitmap& px = l.Edit();
            for (size_t i = 0; i < px.Pixels.size(); ++i)
            {
                uint32_t p = px.Pixels[i];
                uint32_t a = ((p >> 24) * l.LayerMask->Values[i] + 127) / 255;
                px.Pixels[i] = (p & 0x00FFFFFFu) | (a << 24);
            }
        }
        l.LayerMask.reset();
    }

    void Document::InvertMask(int index)
    {
        if (index < 0 || index >= (int)Layers.size() || !Layers[(size_t)index].LayerMask) return;
        for (uint8_t& v : Layers[(size_t)index].EditMask().Values) v = (uint8_t)(255 - v);
    }

    // ---- Compositing

    void Document::CompositeRows(Bitmap& out, const Rect& r, uint32_t root, bool includeRoot) const
    {
        PrepareStyles();
        RowCompositor rc{ *this, {}, r.X0, r.X1 - r.X0 + 1, {}, {} };
        for (size_t i = 0; i < Layers.size(); ++i) rc.Kids[Layers[i].Parent].push_back((int)i);
        // Adjustments made ready; effects (rendered by PrepareStyles, kept with the layer until it changes).
        rc.Prepared.resize(Layers.size());
        for (size_t i = 0; i < Layers.size(); ++i)
        {
            const Layer& l = Layers[i];
            if (l.IsAdjustment()) rc.Prepared[i] = Prepare(l.Adjust);
            if (l.IsRaster() && l.Pixels && l.Effects.Any())
            {
                if (rc.Styles.empty()) rc.Styles.resize(Layers.size());
                rc.Styles[i] = l.Styled;
            }
        }
        const int rootIndex = includeRoot ? IndexOf(root) : -1;
        ParallelRows(r.Y0, r.Y1, (size_t)rc.Width * std::max<size_t>(1, Layers.size()), [&](int y0, int y1) {
            std::vector<glm::vec4> row((size_t)rc.Width);
            for (int y = y0; y <= y1; ++y)
            {
                std::fill(row.begin(), row.end(), glm::vec4(0.0f));
                if (rootIndex >= 0) rc.Alone(Layers[(size_t)rootIndex], row.data(), y);
                else rc.Children(root, row.data(), y);
                uint32_t* dst = out.Pixels.data() + (size_t)y * Width + r.X0;
                for (int i = 0; i < rc.Width; ++i)
                {
                    const glm::vec4& d = row[(size_t)i];
                    dst[i] = d.a > 0.0f ? Pack(glm::vec4(glm::vec3(d) / d.a, d.a)) : 0u;
                }
            }
        });
    }

    void Document::Composite(Bitmap& out, Rect area) const
    {
        if (out.Width != Width || out.Height != Height || out.Pixels.size() != (size_t)Width * Height)
        {
            out = Bitmap::Solid(Width, Height, 0u);
            area = {};
        }
        if (area.Empty()) area = Rect{ 0, 0, (int)Width - 1, (int)Height - 1 };
        area.X0 = std::max(area.X0, 0);
        area.Y0 = std::max(area.Y0, 0);
        area.X1 = std::min(area.X1, (int)Width - 1);
        area.Y1 = std::min(area.Y1, (int)Height - 1);
        if (area.Empty()) return;
        CompositeRows(out, area, 0, false);
    }

    Bitmap Document::Flatten() const
    {
        Bitmap out;
        Composite(out);
        return out;
    }

    Bitmap Document::Render(int index) const
    {
        Bitmap out = Bitmap::Solid(Width, Height, 0u);
        if (index < 0 || index >= (int)Layers.size()) return out;
        CompositeRows(out, Rect{ 0, 0, (int)Width - 1, (int)Height - 1 }, Layers[(size_t)index].Id, true);
        return out;
    }

    size_t Document::MemoryBytes(std::vector<const void*>* seen) const
    {
        size_t bytes = 0;
        auto count = [&](const void* p, size_t size) {
            if (!p) return;
            if (seen)
            {
                if (std::find(seen->begin(), seen->end(), p) != seen->end()) return;
                seen->push_back(p);
            }
            bytes += size;
        };
        for (const Layer& l : Layers)
        {
            if (l.Pixels) count(l.Pixels.get(), l.Pixels->Pixels.size() * 4);
            if (l.LayerMask) count(l.LayerMask.get(), l.LayerMask->Values.size());
        }
        if (Selection) count(Selection.get(), Selection->Values.size());
        return bytes;
    }

    // ---- Files

    std::vector<uint8_t> Document::Serialize() const
    {
        nlohmann::json j;
        j["width"] = Width;
        j["height"] = Height;
        if (!Export.empty()) j["export"] = Export;
        auto& layers = j["layers"] = nlohmann::json::array();
        for (const Layer& l : Layers)
        {
            nlohmann::json jl = { { "id", l.Id },           { "name", l.Name },     { "visible", l.Visible },
                                  { "opacity", l.Opacity }, { "blend", LayerBlendName(l.Blend) } };
            if (l.Parent) jl["parent"] = l.Parent;
            if (l.IsGroup())
            {
                jl["group"] = true;
                jl["expanded"] = l.Expanded;
            }
            if (l.IsAdjustment()) jl["adjustment"] = ToJson(l.Adjust);
            if (l.Kind == LayerKind::Text) jl["text"] = ToJson(l.Text);
            if (l.Kind == LayerKind::Shape) jl["shape"] = ToJson(l.Shape);
            if (l.Effects.Any()) jl["effects"] = ToJson(l.Effects);
            if (l.LockTransparency) jl["lockTransparency"] = true;
            if (l.LockPixels) jl["lockPixels"] = true;
            if (l.LockAll) jl["lockAll"] = true;
            if (l.LayerMask)
            {
                jl["mask"] = true;
                jl["maskEnabled"] = l.MaskEnabled;
            }
            layers.push_back(std::move(jl));
        }
        std::string header = j.dump();
        std::vector<uint8_t> out = { 'D', 'I', 'M', 'G' };
        WriteU32(out, 2);
        WriteU32(out, (uint32_t)header.size());
        out.insert(out.end(), header.begin(), header.end());
        auto blob = [&](const void* data, int channels) {
            std::vector<uint8_t> png;
            stbi_write_png_to_func(AppendBytes, &png, (int)Width, (int)Height, channels, data, (int)Width * channels);
            WriteU32(out, (uint32_t)png.size());
            out.insert(out.end(), png.begin(), png.end());
        };
        for (const Layer& l : Layers)
        {
            if (l.Kind == LayerKind::Pixels)
            {
                Bitmap empty;
                if (!l.Pixels || l.Pixels->Width != Width || l.Pixels->Height != Height) empty = Bitmap::Solid(Width, Height, 0u);
                blob(empty.Empty() ? l.Pixels->Pixels.data() : empty.Pixels.data(), 4);
            }
            if (l.LayerMask) blob(l.LayerMask->Values.data(), 1);
        }
        return out;
    }

    bool Document::Deserialize(const std::vector<uint8_t>& in, Document& out, bool decode)
    {
        if (in.size() < 12 || std::memcmp(in.data(), "DIMG", 4) != 0) return false;
        size_t at = 4;
        uint32_t version = 0, headerSize = 0;
        if (!ReadU32(in, at, version) || version < 1 || !ReadU32(in, at, headerSize) || at + headerSize > in.size()) return false;
        nlohmann::json j = nlohmann::json::parse(in.begin() + at, in.begin() + at + headerSize, nullptr, false);
        at += headerSize;
        if (j.is_discarded() || !j.is_object()) return false;
        Document doc;
        doc.Width = j.value("width", 0u);
        doc.Height = j.value("height", 0u);
        doc.Export = j.value("export", std::string());
        if (!doc.Valid() || doc.Width > 32768 || doc.Height > 32768) return false;
        // The next blob (a layer's pixels or mask), or false when the file ends early.
        auto next = [&](const uint8_t*& data, uint32_t& size) {
            if (!ReadU32(in, at, size) || at + size > in.size()) return false;
            data = in.data() + at;
            at += size;
            return true;
        };
        for (const auto& jl : j.value("layers", nlohmann::json::array()))
        {
            Layer l;
            l.Id = jl.value("id", 0u);
            l.Parent = jl.value("parent", 0u);
            l.Kind = jl.value("group", false)      ? LayerKind::Group
                     : jl.contains("adjustment") ? LayerKind::Adjustment
                     : jl.contains("text")       ? LayerKind::Text
                     : jl.contains("shape")      ? LayerKind::Shape
                                                 : LayerKind::Pixels;
            if (l.Kind == LayerKind::Text) l.Text = TextFromJson(jl["text"]);
            if (l.Kind == LayerKind::Shape) l.Shape = ShapeFromJson(jl["shape"]);
            if (l.IsAdjustment()) l.Adjust = AdjustmentFromJson(jl["adjustment"]);
            if (jl.contains("effects")) l.Effects = EffectsFromJson(jl["effects"]);
            l.Expanded = jl.value("expanded", true);
            l.Name = jl.value("name", std::string("Layer"));
            l.Visible = jl.value("visible", true);
            l.Opacity = std::clamp(jl.value("opacity", 1.0f), 0.0f, 1.0f);
            l.Blend = LayerBlendFromName(jl.value("blend", std::string("Normal")));
            l.LockTransparency = jl.value("lockTransparency", false);
            l.LockPixels = jl.value("lockPixels", false);
            l.LockAll = jl.value("lockAll", false);
            l.MaskEnabled = jl.value("maskEnabled", true);
            const uint8_t* data = nullptr;
            uint32_t size = 0;
            if (l.Kind == LayerKind::Pixels)
            {
                if (!next(data, size)) return false;
                auto pixels = std::make_shared<Bitmap>(Bitmap::Solid(doc.Width, doc.Height, 0u));
                if (decode) DecodeInto(data, size, 4, doc.Width, doc.Height, pixels->Pixels);
                l.Pixels = std::move(pixels);
            }
            if (jl.value("mask", false))
            {
                if (!next(data, size)) return false;
                auto mask = std::make_shared<Mask>(Mask::Solid(doc.Width, doc.Height, 255));
                if (decode) DecodeInto(data, size, 1, doc.Width, doc.Height, mask->Values);
                l.LayerMask = std::move(mask);
            }
            doc.Layers.push_back(std::move(l));
        }
        // Ids (version 1 files have none): unique and non-zero; parents that are not groups mean the top level.
        uint32_t maxId = 0;
        for (const Layer& l : doc.Layers) maxId = std::max(maxId, l.Id);
        std::vector<uint32_t> used;
        for (Layer& l : doc.Layers)
        {
            if (l.Id == 0 || std::find(used.begin(), used.end(), l.Id) != used.end()) l.Id = ++maxId;
            used.push_back(l.Id);
        }
        for (Layer& l : doc.Layers)
            if (int p = l.Parent ? doc.IndexOf(l.Parent) : -1; l.Parent && (p < 0 || !doc.Layers[(size_t)p].IsGroup() || doc.Layers[(size_t)p].Id == l.Id))
                l.Parent = 0;
        doc.NextId = maxId + 1;
        out = std::move(doc);
        return true;
    }

    bool Document::Save(const fs::path& file) const
    {
        std::vector<uint8_t> bytes = Serialize();
        fs::path temp = file;
        temp += ".tmp";
        {
            std::ofstream f(temp, std::ios::binary | std::ios::trunc);
            if (!f) return false;
            f.write((const char*)bytes.data(), (std::streamsize)bytes.size());
            if (!f) return false;
        }
        std::error_code ec;
        fs::rename(temp, file, ec);
        if (ec)
        {
            fs::remove(temp, ec);
            return false;
        }
        return true;
    }

    bool Document::Load(const fs::path& file, Document& out, bool pixels)
    {
        std::ifstream f(file, std::ios::binary);
        if (!f) return false;
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        return Deserialize(bytes, out, pixels);
    }

    fs::path Document::ExportPath(const fs::path& file) const
    {
        if (Export.empty())
        {
            fs::path p = file;
            return p.replace_extension(".png");
        }
        return file.parent_path() / fs::path(Export);
    }
}
