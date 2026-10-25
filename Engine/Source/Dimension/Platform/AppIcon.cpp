#include "Dimension/Platform/AppIcon.h"
#include "Dimension/Core/Log.h"
#include <stb_image.h>
#include <stb_image_write.h>
#include <glm/glm.hpp>
#include <algorithm>
#include <cmath>
#include <cstring>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
// Wide resource types (RT_ICON / RT_GROUP_ICON follow the narrow character set in this build).
#define DIM_RT_ICON MAKEINTRESOURCEW(3)
#define DIM_RT_GROUP_ICON MAKEINTRESOURCEW(14)
#endif

namespace Dimension::AppIcon
{
    namespace
    {
        std::vector<uint8_t> EncodePng(const Image& image)
        {
            std::vector<uint8_t> out;
            stbi_write_png_to_func([](void* ctx, void* data, int size) {
                auto* v = static_cast<std::vector<uint8_t>*>(ctx);
                v->insert(v->end(), (uint8_t*)data, (uint8_t*)data + size);
            }, &out, image.Width, image.Height, 4, image.Rgba.data(), image.Width * 4);
            return out;
        }

        template <class T> void Put(std::vector<uint8_t>& out, T value)
        {
            uint8_t bytes[sizeof(T)];
            std::memcpy(bytes, &value, sizeof(T));
            out.insert(out.end(), bytes, bytes + sizeof(T));
        }

        // Directory entry fields shared by .ico files and RT_GROUP_ICON resources (256 is stored as 0).
        void PutEntryHeader(std::vector<uint8_t>& out, int size, size_t bytes)
        {
            Put<uint8_t>(out, (uint8_t)(size >= 256 ? 0 : size));
            Put<uint8_t>(out, (uint8_t)(size >= 256 ? 0 : size));
            Put<uint8_t>(out, 0);  // palette colors
            Put<uint8_t>(out, 0);  // reserved
            Put<uint16_t>(out, 1); // planes
            Put<uint16_t>(out, 32); // bits per pixel
            Put<uint32_t>(out, (uint32_t)bytes);
        }
    }

    std::optional<Image> Load(const fs::path& path)
    {
        int w = 0, h = 0, channels = 0;
        stbi_uc* pixels = stbi_load(path.string().c_str(), &w, &h, &channels, 4);
        if (!pixels)
        {
            Log::Warn("Could not load icon image '{}': {}", path.string(), stbi_failure_reason());
            return std::nullopt;
        }
        Image image;
        image.Width = w;
        image.Height = h;
        image.Rgba.assign(pixels, pixels + (size_t)w * h * 4);
        stbi_image_free(pixels);
        return image;
    }

    Image Resize(const Image& source, int size)
    {
        Image out;
        out.Width = out.Height = size;
        out.Rgba.assign((size_t)size * size * 4, 0);
        if (source.Width <= 0 || source.Height <= 0) return out;
        // Destination rectangle of the fitted image.
        float scale = (float)size / (float)std::max(source.Width, source.Height);
        float dw = source.Width * scale, dh = source.Height * scale;
        float ox = (size - dw) * 0.5f, oy = (size - dh) * 0.5f;
        auto texel = [&](int x, int y) {
            x = std::clamp(x, 0, source.Width - 1);
            y = std::clamp(y, 0, source.Height - 1);
            const uint8_t* p = &source.Rgba[((size_t)y * source.Width + x) * 4];
            float a = p[3] / 255.0f;
            return glm::vec4(p[0] / 255.0f * a, p[1] / 255.0f * a, p[2] / 255.0f * a, a); // premultiplied
        };
        auto bilinear = [&](float u, float v) { // source pixel coordinates (pixel centers at +0.5)
            float x = u - 0.5f, y = v - 0.5f;
            int x0 = (int)std::floor(x), y0 = (int)std::floor(y);
            float fx = x - x0, fy = y - y0;
            return glm::mix(glm::mix(texel(x0, y0), texel(x0 + 1, y0), fx), glm::mix(texel(x0, y0 + 1), texel(x0 + 1, y0 + 1), fx), fy);
        };
        // Supersample each destination pixel enough to cover the source pixels it spans (box filter when shrinking).
        int samples = std::clamp((int)std::ceil(1.0f / scale), 1, 16);
        for (int y = 0; y < size; ++y)
            for (int x = 0; x < size; ++x)
            {
                glm::vec4 sum(0.0f);
                for (int sy = 0; sy < samples; ++sy)
                    for (int sx = 0; sx < samples; ++sx)
                    {
                        float px = x + (sx + 0.5f) / samples, py = y + (sy + 0.5f) / samples;
                        if (px < ox || py < oy || px > ox + dw || py > oy + dh) continue; // outside: transparent
                        sum += bilinear((px - ox) / scale, (py - oy) / scale);
                    }
                sum /= (float)(samples * samples);
                uint8_t* d = &out.Rgba[((size_t)y * size + x) * 4];
                float a = sum.a;
                glm::vec3 c = a > 1e-5f ? glm::vec3(sum) / a : glm::vec3(0.0f);
                for (int i = 0; i < 3; ++i) d[i] = (uint8_t)std::lround(std::clamp(c[i], 0.0f, 1.0f) * 255.0f);
                d[3] = (uint8_t)std::lround(std::clamp(a, 0.0f, 1.0f) * 255.0f);
            }
        return out;
    }

    std::vector<uint8_t> BuildIco(const Image& source)
    {
        std::vector<std::vector<uint8_t>> images;
        for (int s : kSizes) images.push_back(EncodePng(Resize(source, s)));
        std::vector<uint8_t> out;
        Put<uint16_t>(out, 0); // reserved
        Put<uint16_t>(out, 1); // type: icon
        Put<uint16_t>(out, (uint16_t)images.size());
        uint32_t offset = 6 + 16 * (uint32_t)images.size();
        for (size_t i = 0; i < images.size(); ++i)
        {
            PutEntryHeader(out, kSizes[i], images[i].size());
            Put<uint32_t>(out, offset);
            offset += (uint32_t)images[i].size();
        }
        for (auto& img : images) out.insert(out.end(), img.begin(), img.end());
        return out;
    }

    bool WriteToExecutable(const fs::path& exe, const Image& source)
    {
#ifdef _WIN32
        const WORD lang = MAKELANGID(LANG_NEUTRAL, SUBLANG_NEUTRAL); // Engine/Resources/Dimension.rc uses this language

        // Icon ids the executable's current group uses, so they can be dropped.
        std::vector<WORD> oldIds;
        if (HMODULE module = LoadLibraryExW(exe.wstring().c_str(), nullptr, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE))
        {
            if (HRSRC res = FindResourceExW(module, DIM_RT_GROUP_ICON, L"GLFW_ICON", lang))
                if (HGLOBAL data = LoadResource(module, res))
                {
                    auto* bytes = (const uint8_t*)LockResource(data);
                    DWORD size = SizeofResource(module, res);
                    uint16_t count = size >= 6 ? *(const uint16_t*)(bytes + 4) : 0;
                    for (uint16_t i = 0; i < count && 6u + 14u * (i + 1u) <= size; ++i)
                        oldIds.push_back(*(const uint16_t*)(bytes + 6 + 14 * i + 12));
                }
            FreeLibrary(module);
        }

        HANDLE update = BeginUpdateResourceW(exe.wstring().c_str(), FALSE);
        if (!update)
        {
            Log::Error("Could not open '{}' to set its icon (error {})", exe.string(), GetLastError());
            return false;
        }
        for (WORD id : oldIds) UpdateResourceW(update, DIM_RT_ICON, MAKEINTRESOURCEW(id), lang, nullptr, 0);

        std::vector<uint8_t> group;
        Put<uint16_t>(group, 0);
        Put<uint16_t>(group, 1);
        Put<uint16_t>(group, (uint16_t)std::size(kSizes));
        bool ok = true;
        WORD id = 1;
        for (int s : kSizes)
        {
            std::vector<uint8_t> png = EncodePng(Resize(source, s));
            ok &= UpdateResourceW(update, DIM_RT_ICON, MAKEINTRESOURCEW(id), lang, png.data(), (DWORD)png.size()) != FALSE;
            PutEntryHeader(group, s, png.size());
            Put<uint16_t>(group, id);
            ++id;
        }
        ok &= UpdateResourceW(update, DIM_RT_GROUP_ICON, L"GLFW_ICON", lang, group.data(), (DWORD)group.size()) != FALSE;
        if (!EndUpdateResourceW(update, ok ? FALSE : TRUE) || !ok)
        {
            Log::Error("Writing the icon into '{}' failed (error {})", exe.string(), GetLastError());
            return false;
        }
        return true;
#else
        (void)exe;
        (void)source;
        return false;
#endif
    }
}
