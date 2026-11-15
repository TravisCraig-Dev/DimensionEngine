#include "Dimension/Modeling/Paint.h"
#include <stb_image.h>
#include <stb_image_write.h>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace Dimension::Modeling::Paint
{
    namespace
    {
        std::vector<glm::vec3> Mirrors(uint8_t symmetry)
        {
            std::vector<glm::vec3> out;
            for (int bits = 0; bits < 8; ++bits)
                if ((bits & ~symmetry) == 0) out.push_back(glm::vec3(bits & 1 ? -1.0f : 1.0f, bits & 2 ? -1.0f : 1.0f, bits & 4 ? -1.0f : 1.0f));
            return out;
        }
    }

    // ---- Images

    Image Image::Solid(uint32_t width, uint32_t height, uint32_t rgba)
    {
        Image img;
        img.Width = std::max(width, 1u);
        img.Height = std::max(height, 1u);
        img.Pixels.assign((size_t)img.Width * img.Height, rgba);
        return img;
    }

    glm::vec4 Image::Sample(glm::vec2 uv) const
    {
        if (Pixels.empty()) return glm::vec4(1.0f);
        float x = uv.x * Width - 0.5f, y = uv.y * Height - 0.5f;
        int x0 = (int)std::floor(x), y0 = (int)std::floor(y);
        float fx = x - x0, fy = y - y0;
        auto px = [&](int a, int b) {
            a = ((a % (int)Width) + (int)Width) % (int)Width;
            b = ((b % (int)Height) + (int)Height) % (int)Height;
            return UnpackRGBA8(At((uint32_t)a, (uint32_t)b));
        };
        return glm::mix(glm::mix(px(x0, y0), px(x0 + 1, y0), fx), glm::mix(px(x0, y0 + 1), px(x0 + 1, y0 + 1), fx), fy);
    }

    bool Image::Load(const fs::path& path, Image& out)
    {
        int w = 0, h = 0, channels = 0;
        stbi_uc* data = stbi_load(path.string().c_str(), &w, &h, &channels, 4);
        if (!data) return false;
        out.Width = (uint32_t)w;
        out.Height = (uint32_t)h;
        out.Pixels.resize((size_t)w * h);
        std::memcpy(out.Pixels.data(), data, out.Pixels.size() * 4);
        stbi_image_free(data);
        return true;
    }

    bool Image::CanSave(const fs::path& path)
    {
        std::string ext = path.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](char c) { return (char)std::tolower((unsigned char)c); });
        return ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga" || ext == ".bmp";
    }

    bool Image::Save(const fs::path& path) const
    {
        if (Pixels.empty()) return false;
        std::string ext = path.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](char c) { return (char)std::tolower((unsigned char)c); });
        std::string file = path.string();
        if (ext == ".jpg" || ext == ".jpeg") return stbi_write_jpg(file.c_str(), (int)Width, (int)Height, 4, Pixels.data(), 95) != 0;
        if (ext == ".tga") return stbi_write_tga(file.c_str(), (int)Width, (int)Height, 4, Pixels.data()) != 0;
        if (ext == ".bmp") return stbi_write_bmp(file.c_str(), (int)Width, (int)Height, 4, Pixels.data()) != 0;
        return SavePng(path);
    }

    bool Image::SavePng(const fs::path& path) const
    {
        if (Pixels.empty()) return false;
        return stbi_write_png(path.string().c_str(), (int)Width, (int)Height, 4, Pixels.data(), (int)Width * 4) != 0;
    }

    // ---- Blending

    const char* BlendName(Blend b)
    {
        static const char* names[] = { "Mix", "Add", "Subtract", "Multiply", "Lighten", "Darken", "Erase Alpha", "Blur", "Clone" };
        return names[std::clamp((int)b, 0, (int)Blend::Count - 1)];
    }

    glm::vec4 BlendColor(const glm::vec4& dst, const glm::vec4& src, Blend mode, float weight)
    {
        float w = std::clamp(weight, 0.0f, 1.0f) * src.a;
        glm::vec3 d(dst), s(src), target = d;
        switch (mode)
        {
        case Blend::Mix: target = s; break;
        case Blend::Add: target = d + s; break;
        case Blend::Subtract: target = d - s; break;
        case Blend::Multiply: target = d * s; break;
        case Blend::Lighten: target = glm::max(d, s); break;
        case Blend::Darken: target = glm::min(d, s); break;
        case Blend::Erase: return glm::vec4(d, std::max(0.0f, dst.a - w));
        default: break;
        }
        glm::vec3 rgb = glm::clamp(glm::mix(d, target, w), 0.0f, 1.0f);
        float a = mode == Blend::Mix ? dst.a + (1.0f - dst.a) * w : dst.a;
        return glm::vec4(rgb, a);
    }

    glm::vec4 BlendColorLinear(const glm::vec4& dst, const glm::vec4& src, Blend mode, float weight)
    {
        auto lin = [](const glm::vec4& c) { return glm::vec4(SrgbToLinear(c.r), SrgbToLinear(c.g), SrgbToLinear(c.b), c.a); };
        glm::vec4 out = BlendColor(lin(dst), lin(src), mode, weight);
        return glm::vec4(LinearToSrgb(out.r), LinearToSrgb(out.g), LinearToSrgb(out.b), out.a);
    }

    float BrushWeight(const Brush& brush, float d)
    {
        if (d >= brush.Radius || brush.Radius <= 0.0f) return 0.0f;
        float t = d / brush.Radius, h = std::clamp(brush.Hardness, 0.0f, 0.999f);
        t = t <= h ? 0.0f : (t - h) / (1.0f - h);
        return (1.0f - t * t * (3.0f - 2.0f * t)) * std::clamp(brush.Strength, 0.0f, 1.0f);
    }

    // ---- Vertex paint

    size_t PaintVertices(EditableMesh& m, const glm::vec3& center, const Brush& brush)
    {
        size_t changed = 0;
        const Topology& t = m.Topo();
        std::vector<glm::vec3> faceNormal(m.FaceCount());
        for (uint32_t f = 0; f < m.FaceCount(); ++f) faceNormal[f] = m.FaceNormal(f);
        // Blur: each vertex's average color (over its corners) first.
        std::vector<glm::vec4> average;
        if (brush.Mode == Blend::Blur)
        {
            average.assign(m.VertexCount(), glm::vec4(0.0f));
            std::vector<float> count(m.VertexCount(), 0.0f);
            for (const Face& face : m.Faces)
                for (size_t c = 0; c < face.Verts.size(); ++c)
                {
                    average[face.Verts[c]] += UnpackRGBA8(face.ColorAt(c));
                    count[face.Verts[c]] += 1.0f;
                }
            for (uint32_t v = 0; v < m.VertexCount(); ++v)
                if (count[v] > 0.0f) average[v] /= count[v];
        }
        for (const glm::vec3& s : Mirrors(brush.Symmetry))
        {
            glm::vec3 c = center * s, view = brush.ViewDir * s;
            for (uint32_t v = 0; v < m.VertexCount(); ++v)
            {
                float w = BrushWeight(brush, glm::length(m.Positions[v] - c));
                if (w <= 0.0f || m.IsVertHidden(v)) continue;
                glm::vec4 neighbours(0.0f);
                if (brush.Mode == Blend::Blur)
                {
                    float n = 0.0f;
                    for (uint32_t e : t.VertEdges[v])
                    {
                        uint32_t u = EdgeA(t.Edges[e]) == v ? EdgeB(t.Edges[e]) : EdgeA(t.Edges[e]);
                        neighbours += average[u];
                        n += 1.0f;
                    }
                    if (n == 0.0f) continue;
                    neighbours /= n;
                }
                for (uint32_t f : t.VertFaces[v])
                {
                    Face& face = m.Faces[f];
                    if (face.Flags & Hidden) continue;
                    if (brush.FrontFacesOnly && glm::dot(faceNormal[f], view) > 0.0f) continue;
                    for (size_t k = 0; k < face.Verts.size(); ++k)
                    {
                        if (face.Verts[k] != v) continue;
                        glm::vec4 before = UnpackRGBA8(face.ColorAt(k));
                        float wk = brush.Texture ? w * brush.Texture->At(m.Positions[v], c, brush.Radius, view) : w;
                        glm::vec4 color = brush.Color;
                        Blend mode = brush.Mode;
                        if (mode == Blend::Clone)
                        {
                            if (!brush.CloneSource || brush.CloneSource->Empty()) continue;
                            glm::vec2 uv = k < face.UVs.size() ? face.UVs[k] : glm::vec2(0.0f);
                            color = brush.CloneSource->Sample(uv + brush.CloneOffset);
                            mode = Blend::Mix;
                        }
                        glm::vec4 after = mode == Blend::Blur ? glm::mix(before, neighbours, std::min(1.0f, wk))
                                          : brush.Linear ? BlendColorLinear(before, color, mode, wk)
                                                         : BlendColor(before, color, mode, wk);
                        uint32_t packed = PackRGBA8(after);
                        if (packed != face.ColorAt(k))
                        {
                            face.SetColor(k, packed);
                            ++changed;
                        }
                    }
                }
            }
        }
        return changed;
    }

    void FillVertices(EditableMesh& m, const std::vector<uint32_t>& faces, uint32_t rgba)
    {
        for (uint32_t f : faces)
            for (size_t c = 0; c < m.Faces[f].Verts.size(); ++c) m.Faces[f].SetColor(c, rgba);
    }

    // ---- Texture paint

    Rect PaintTexture(const EditableMesh& m, Image& image, const glm::vec3& center, const Brush& brush)
    {
        Rect changed;
        if (image.Empty()) return changed;
        const float W = (float)image.Width, H = (float)image.Height;
        // Blur (and cloning from this image) read the image as it was before this dab.
        Image before;
        if (brush.Mode == Blend::Blur || (brush.Mode == Blend::Clone && brush.CloneSource == &image)) before = image;
        const Image* cloneFrom = brush.CloneSource == &image ? &before : brush.CloneSource;
        std::vector<uint32_t> tri;
        for (const glm::vec3& s : Mirrors(brush.Symmetry))
        {
            glm::vec3 c = center * s, view = brush.ViewDir * s;
            float r = brush.Radius;
            for (uint32_t f = 0; f < m.FaceCount(); ++f)
            {
                const Face& face = m.Faces[f];
                if (face.Flags & Hidden) continue;
                // Quick reject: the face's bounds against the brush's sphere.
                glm::vec3 lo(1e30f), hi(-1e30f);
                for (uint32_t v : face.Verts)
                {
                    lo = glm::min(lo, m.Positions[v]);
                    hi = glm::max(hi, m.Positions[v]);
                }
                glm::vec3 q = glm::clamp(c, lo, hi) - c;
                if (glm::dot(q, q) > r * r) continue;
                if (brush.FrontFacesOnly && glm::dot(m.FaceNormal(f), view) > 0.0f) continue;
                tri.clear();
                m.TriangulateFace(f, tri);
                for (size_t k = 0; k + 2 < tri.size(); k += 3)
                {
                    glm::vec3 p[3];
                    glm::vec2 t[3];
                    for (int j = 0; j < 3; ++j)
                    {
                        p[j] = m.Positions[face.Verts[tri[k + j]]];
                        glm::vec2 uv = tri[k + j] < face.UVs.size() ? face.UVs[tri[k + j]] : glm::vec2(0.0f);
                        t[j] = glm::vec2(uv.x * W, uv.y * H);
                    }
                    float area = (t[1].x - t[0].x) * (t[2].y - t[0].y) - (t[1].y - t[0].y) * (t[2].x - t[0].x);
                    if (std::abs(area) < 1e-8f) continue;
                    // Texels of the triangle's UV bounds (a pixel wider), each mapped back onto the surface.
                    int x0 = (int)std::floor(std::min({ t[0].x, t[1].x, t[2].x })) - 1, x1 = (int)std::ceil(std::max({ t[0].x, t[1].x, t[2].x })) + 1;
                    int y0 = (int)std::floor(std::min({ t[0].y, t[1].y, t[2].y })) - 1, y1 = (int)std::ceil(std::max({ t[0].y, t[1].y, t[2].y })) + 1;
                    float edgeLen[3];
                    for (int j = 0; j < 3; ++j) edgeLen[j] = std::max(glm::length(t[(j + 2) % 3] - t[(j + 1) % 3]), 1e-6f);
                    for (int y = y0; y <= y1; ++y)
                        for (int x = x0; x <= x1; ++x)
                        {
                            glm::vec2 pt((float)x + 0.5f, (float)y + 0.5f);
                            // Barycentrics; corner j's weight is its opposite edge's signed area.
                            float b[3];
                            bool inside = true;
                            for (int j = 0; j < 3; ++j)
                            {
                                const glm::vec2 &e0 = t[(j + 1) % 3], &e1 = t[(j + 2) % 3];
                                float a = (e1.x - e0.x) * (pt.y - e0.y) - (e1.y - e0.y) * (pt.x - e0.x);
                                b[j] = a / area;
                                // Within about a pixel outside the edge still counts (seams).
                                if (a / area * std::abs(area) / edgeLen[j] < -0.75f) inside = false;
                            }
                            if (!inside) continue;
                            float sum = 0.0f;
                            for (float& v : b)
                            {
                                v = std::max(v, 0.0f);
                                sum += v;
                            }
                            if (sum <= 0.0f) continue;
                            glm::vec3 point = (p[0] * b[0] + p[1] * b[1] + p[2] * b[2]) / sum;
                            float w = BrushWeight(brush, glm::length(point - c));
                            if (w > 0.0f && brush.Texture) w *= brush.Texture->At(point, c, r, view);
                            if (w <= 0.0f) continue;
                            int px = ((x % (int)image.Width) + (int)image.Width) % (int)image.Width;
                            int py = ((y % (int)image.Height) + (int)image.Height) % (int)image.Height;
                            uint32_t& dst = image.At((uint32_t)px, (uint32_t)py);
                            glm::vec4 old = UnpackRGBA8(dst), next;
                            if (brush.Mode == Blend::Blur)
                            {
                                glm::vec4 avg(0.0f);
                                for (int dy = -1; dy <= 1; ++dy)
                                    for (int dx = -1; dx <= 1; ++dx)
                                    {
                                        int sx = std::clamp(px + dx, 0, (int)image.Width - 1), sy = std::clamp(py + dy, 0, (int)image.Height - 1);
                                        avg += UnpackRGBA8(before.At((uint32_t)sx, (uint32_t)sy));
                                    }
                                next = glm::mix(old, avg / 9.0f, std::min(1.0f, w));
                            }
                            else if (brush.Mode == Blend::Clone)
                            {
                                if (!cloneFrom || cloneFrom->Empty()) continue;
                                glm::vec2 uv(((float)px + 0.5f) / W, ((float)py + 0.5f) / H);
                                glm::vec4 src = cloneFrom->Sample(uv + brush.CloneOffset);
                                next = brush.Linear ? BlendColorLinear(old, src, Blend::Mix, w) : BlendColor(old, src, Blend::Mix, w);
                            }
                            else
                                next = brush.Linear ? BlendColorLinear(old, brush.Color, brush.Mode, w) : BlendColor(old, brush.Color, brush.Mode, w);
                            uint32_t packed = PackRGBA8(next);
                            if (packed != dst)
                            {
                                dst = packed;
                                changed.Add(px, py);
                            }
                        }
                }
            }
        }
        return changed;
    }

    Rect PaintImage(Image& image, const glm::vec2& center, float radius, const Brush& brush)
    {
        Rect changed;
        if (image.Empty() || radius <= 0.0f) return changed;
        const float W = (float)image.Width, H = (float)image.Height;
        Image before;
        if (brush.Mode == Blend::Blur || (brush.Mode == Blend::Clone && brush.CloneSource == &image)) before = image;
        const Image* cloneFrom = brush.CloneSource == &image ? &before : brush.CloneSource;
        Brush flat = brush;
        flat.Radius = radius;
        int x0 = (int)std::floor(center.x - radius), x1 = (int)std::ceil(center.x + radius);
        int y0 = (int)std::floor(center.y - radius), y1 = (int)std::ceil(center.y + radius);
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x)
            {
                glm::vec2 d((float)x + 0.5f - center.x, (float)y + 0.5f - center.y);
                float w = BrushWeight(flat, glm::length(d));
                if (w <= 0.0f) continue;
                int px = ((x % (int)image.Width) + (int)image.Width) % (int)image.Width;
                int py = ((y % (int)image.Height) + (int)image.Height) % (int)image.Height;
                glm::vec2 uv(((float)px + 0.5f) / W, ((float)py + 0.5f) / H);
                if (brush.Texture)
                {
                    const Sculpt::BrushTexture& t = *brush.Texture;
                    glm::vec2 q = t.Mapping == 1 ? glm::vec2((float)x + 0.5f, (float)y + 0.5f) / glm::vec2(W, H) / std::max(t.Scale, 1e-6f)
                                                 : d / (2.0f * radius);
                    float a = glm::radians(t.Angle), cs = std::cos(a), sn = std::sin(a);
                    q = glm::vec2(cs * q.x - sn * q.y, sn * q.x + cs * q.y);
                    w *= t.Sample(t.Mapping == 1 ? q : q + 0.5f);
                    if (w <= 0.0f) continue;
                }
                uint32_t& dst = image.At((uint32_t)px, (uint32_t)py);
                glm::vec4 old = UnpackRGBA8(dst), next;
                if (brush.Mode == Blend::Blur)
                {
                    glm::vec4 avg(0.0f);
                    for (int dy = -1; dy <= 1; ++dy)
                        for (int dx = -1; dx <= 1; ++dx)
                        {
                            int sx = std::clamp(px + dx, 0, (int)image.Width - 1), sy = std::clamp(py + dy, 0, (int)image.Height - 1);
                            avg += UnpackRGBA8(before.At((uint32_t)sx, (uint32_t)sy));
                        }
                    next = glm::mix(old, avg / 9.0f, std::min(1.0f, w));
                }
                else if (brush.Mode == Blend::Clone)
                {
                    if (!cloneFrom || cloneFrom->Empty()) continue;
                    glm::vec4 src = cloneFrom->Sample(uv + brush.CloneOffset);
                    next = brush.Linear ? BlendColorLinear(old, src, Blend::Mix, w) : BlendColor(old, src, Blend::Mix, w);
                }
                else
                    next = brush.Linear ? BlendColorLinear(old, brush.Color, brush.Mode, w) : BlendColor(old, brush.Color, brush.Mode, w);
                uint32_t packed = PackRGBA8(next);
                if (packed != dst)
                {
                    dst = packed;
                    changed.Add(px, py);
                }
            }
        return changed;
    }

    bool SurfaceAt(const EditableMesh& m, uint32_t f, const glm::vec3& point, glm::vec2& uv, glm::vec4& color)
    {
        if (f >= m.FaceCount()) return false;
        const Face& face = m.Faces[f];
        std::vector<uint32_t> tri;
        m.TriangulateFace(f, tri);
        float best = 1e30f;
        bool found = false;
        for (size_t k = 0; k + 2 < tri.size(); k += 3)
        {
            glm::vec3 a = m.Positions[face.Verts[tri[k]]], b = m.Positions[face.Verts[tri[k + 1]]], c = m.Positions[face.Verts[tri[k + 2]]];
            glm::vec3 n = glm::cross(b - a, c - a);
            float nn = glm::dot(n, n);
            if (nn < 1e-20f) continue;
            float w0 = glm::dot(glm::cross(c - b, point - b), n) / nn, w1 = glm::dot(glm::cross(a - c, point - c), n) / nn, w2 = 1.0f - w0 - w1;
            float outside = std::max({ -w0, -w1, -w2, 0.0f });
            if (outside >= best) continue;
            best = outside;
            found = true;
            glm::vec3 w = glm::max(glm::vec3(w0, w1, w2), 0.0f);
            w /= std::max(w.x + w.y + w.z, 1e-9f);
            auto uvAt = [&](uint32_t i) { return i < face.UVs.size() ? face.UVs[i] : glm::vec2(0.0f); };
            uv = uvAt(tri[k]) * w.x + uvAt(tri[k + 1]) * w.y + uvAt(tri[k + 2]) * w.z;
            color = UnpackRGBA8(face.ColorAt(tri[k])) * w.x + UnpackRGBA8(face.ColorAt(tri[k + 1])) * w.y + UnpackRGBA8(face.ColorAt(tri[k + 2])) * w.z;
        }
        return found;
    }
}
