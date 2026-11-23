#include "Dimension/Platform/SplashScreen.h"
#include "Dimension/Asset/AssetManager.h"
#include "embedded/kSplashLogoPng.h"
#include <algorithm>
#include <cmath>
#include <mutex>
#include <stb_image.h>
#include <stb_truetype.h>

// The splash screen drawn in software (Linux shows it in an X11 window): the same layout and colors as the Windows
// one (SplashScreenWin32.cpp), text from the built-in Roboto through stb_truetype.
namespace Dimension::SplashScreen
{
    namespace
    {
        struct Rgb { uint8_t R, G, B; };
        constexpr Rgb kBackground{ 30, 33, 41 }, kAccent{ 58, 121, 187 }, kTitle{ 236, 238, 242 }, kSubtitle{ 160, 168, 182 },
                      kStatus{ 190, 196, 206 }, kTrack{ 48, 53, 64 };

        struct Canvas
        {
            AppIcon::Image& Image;
            void Blend(int x, int y, Rgb c, int alpha) // alpha 0..255
            {
                if (x < 0 || y < 0 || x >= Image.Width || y >= Image.Height || alpha <= 0) return;
                uint8_t* p = &Image.Rgba[((size_t)y * Image.Width + x) * 4];
                p[0] = (uint8_t)((p[0] * (255 - alpha) + c.R * alpha + 127) / 255);
                p[1] = (uint8_t)((p[1] * (255 - alpha) + c.G * alpha + 127) / 255);
                p[2] = (uint8_t)((p[2] * (255 - alpha) + c.B * alpha + 127) / 255);
            }
            void Fill(int x0, int y0, int x1, int y1, Rgb c)
            {
                x0 = std::max(x0, 0), y0 = std::max(y0, 0), x1 = std::min(x1, Image.Width), y1 = std::min(y1, Image.Height);
                for (int y = y0; y < y1; ++y)
                    for (int x = x0; x < x1; ++x)
                    {
                        uint8_t* p = &Image.Rgba[((size_t)y * Image.Width + x) * 4];
                        p[0] = c.R, p[1] = c.G, p[2] = c.B, p[3] = 255;
                    }
            }
        };

        struct Resources
        {
            stbtt_fontinfo Font{};
            bool FontOk = false;
            std::optional<AppIcon::Image> Logo;
        };
        const Resources& Shared()
        {
            static Resources r;
            static std::once_flag once;
            std::call_once(once, [] {
                size_t size = 0;
                const uint8_t* font = AssetManager::GetDefaultFontData(&size);
                r.FontOk = font && stbtt_InitFont(&r.Font, font, stbtt_GetFontOffsetForIndex(font, 0)) != 0;
                int w = 0, h = 0, channels = 0;
                if (stbi_uc* px = stbi_load_from_memory(kSplashLogoPng, (int)kSplashLogoPng_size, &w, &h, &channels, 4))
                {
                    AppIcon::Image logo;
                    logo.Width = w, logo.Height = h;
                    logo.Rgba.assign(px, px + (size_t)w * h * 4);
                    stbi_image_free(px);
                    r.Logo = std::move(logo);
                }
            });
            return r;
        }

        std::vector<int> Codepoints(const std::string& text)
        {
            std::vector<int> out;
            for (size_t i = 0; i < text.size();)
            {
                unsigned char c = (unsigned char)text[i];
                int n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
                int cp = n == 1 ? (c < 0x80 ? c : 0xFFFD) : c & (0x3F >> (n - 1));
                for (int k = 1; k < n; ++k)
                    cp = i + k < text.size() ? (cp << 6) | ((unsigned char)text[i + k] & 0x3F) : 0xFFFD;
                out.push_back(cp);
                i += n;
            }
            return out;
        }

        // One line of text in the box from `left` to `right` whose top is `top` (`size`: the em height in pixels,
        // like a GDI font height), ending in an ellipsis when it does not fit.
        void Text(Canvas& canvas, const std::string& utf8, float size, Rgb color, int left, int top, int right)
        {
            const Resources& r = Shared();
            if (!r.FontOk || utf8.empty()) return;
            const stbtt_fontinfo& f = r.Font;
            float scale = stbtt_ScaleForMappingEmToPixels(&f, size);
            int ascent = 0, descent = 0, gap = 0;
            stbtt_GetFontVMetrics(&f, &ascent, &descent, &gap);
            float baseline = top + ascent * scale;
            auto advance = [&](int cp, int next) {
                int adv = 0, lsb = 0;
                stbtt_GetCodepointHMetrics(&f, cp, &adv, &lsb);
                return (adv + (next ? stbtt_GetCodepointKernAdvance(&f, cp, next) : 0)) * scale;
            };
            auto width = [&](const std::vector<int>& cps) {
                float w = 0.0f;
                for (size_t i = 0; i < cps.size(); ++i) w += advance(cps[i], i + 1 < cps.size() ? cps[i + 1] : 0);
                return w;
            };
            std::vector<int> cps = Codepoints(utf8);
            if (width(cps) > right - left)
            {
                while (!cps.empty() && (cps.push_back(0x2026), width(cps) > right - left)) cps.pop_back(), cps.pop_back();
                if (cps.empty()) return;
            }
            float x = (float)left;
            std::vector<uint8_t> glyph;
            for (size_t i = 0; i < cps.size(); ++i)
            {
                int cp = cps[i];
                float shift = x - std::floor(x);
                int x0, y0, x1, y1;
                stbtt_GetCodepointBitmapBoxSubpixel(&f, cp, scale, scale, shift, 0.0f, &x0, &y0, &x1, &y1);
                int w = x1 - x0, h = y1 - y0;
                if (w > 0 && h > 0)
                {
                    glyph.assign((size_t)w * h, 0);
                    stbtt_MakeCodepointBitmapSubpixel(&f, glyph.data(), w, h, w, scale, scale, shift, 0.0f, cp);
                    int ox = (int)std::floor(x) + x0, oy = (int)std::lround(baseline) + y0;
                    for (int y = 0; y < h; ++y)
                        for (int gx = 0; gx < w; ++gx) canvas.Blend(ox + gx, oy + y, color, glyph[(size_t)y * w + gx]);
                }
                x += advance(cp, i + 1 < cps.size() ? cps[i + 1] : 0);
            }
        }
    }

    AppIcon::Image Background(float scale)
    {
        using namespace Layout;
        scale = std::clamp(scale, 0.5f, 4.0f);
        auto px = [&](float v) { return (int)std::lround(v * scale); };
        AppIcon::Image image;
        image.Width = px((float)Width), image.Height = px((float)Height);
        image.Rgba.assign((size_t)image.Width * image.Height * 4, 255);
        Canvas canvas{ image };
        canvas.Fill(0, 0, image.Width, image.Height, kBackground);
        if (const auto& logo = Shared().Logo)
        {
            int lw = px((float)LogoWidth), lh = (int)std::lround((float)lw * logo->Height / logo->Width);
            AppIcon::Image sized = AppIcon::Resize(*logo, lw, lh);
            int ox = px((float)LogoX), oy = px((float)LogoY);
            for (int y = 0; y < sized.Height; ++y)
                for (int x = 0; x < sized.Width; ++x)
                {
                    const uint8_t* s = &sized.Rgba[((size_t)y * sized.Width + x) * 4];
                    canvas.Blend(ox + x, oy + y, { s[0], s[1], s[2] }, s[3]);
                }
        }
        canvas.Fill(0, 0, image.Width, px((float)AccentHeight), kAccent); // accent line (the editor's default accent)
        return image;
    }

    AppIcon::Image Render(float scale, const std::string& title, const std::string& subtitle, const std::string& status,
                          float phase)
    {
        using namespace Layout;
        scale = std::clamp(scale, 0.5f, 4.0f);
        auto px = [&](float v) { return (int)std::lround(v * scale); };
        AppIcon::Image image = Background(scale);
        Canvas canvas{ image };
        int left = px((float)TextLeft), right = px((float)TextRight);
        Text(canvas, title, TitleSize * scale, kTitle, left, px((float)TitleTop), right);
        Text(canvas, subtitle, SubtitleSize * scale, kSubtitle, left, px((float)SubtitleTop), right);

        // Status and a moving bar (loading has no known length: a segment sweeping across).
        int barTop = px((float)BarTop), barH = px((float)BarHeight);
        Text(canvas, status.empty() ? "Loading..." : status, StatusSize * scale, kStatus, left, px((float)StatusTop), right);
        canvas.Fill(left, barTop, right, barTop + barH, kTrack);
        phase = phase - std::floor(phase);
        int span = right - left, seg = span / 4;
        int x0 = left + (int)((span + seg) * phase) - seg;
        canvas.Fill(std::max(x0, left), barTop, std::min(x0 + seg, right), barTop + barH, kAccent);
        return image;
    }
}
