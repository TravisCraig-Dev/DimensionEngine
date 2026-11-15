#pragma once
#include "Dimension/Modeling/EditableMesh.h"

// Painting (Blender's Vertex Paint and Texture Paint): brushes that color a mesh's corners, or the texels of an image
// laid on it through its UVs, wherever the surface lies inside the brush's sphere. Colors are RGBA8 with sRGB color,
// as images and vertex colors store them; blending happens on those values.
namespace Dimension::Modeling::Paint
{
    // An RGBA8 image (row 0 at the top, red in the lowest byte).
    struct Image
    {
        uint32_t Width = 0, Height = 0;
        std::vector<uint32_t> Pixels;

        static Image Solid(uint32_t width, uint32_t height, uint32_t rgba);
        bool Empty() const { return Pixels.empty(); }
        uint32_t& At(uint32_t x, uint32_t y) { return Pixels[(size_t)y * Width + x]; }
        uint32_t At(uint32_t x, uint32_t y) const { return Pixels[(size_t)y * Width + x]; }
        // The color at a UV (bilinear; (0, 0) the top-left corner, repeating).
        glm::vec4 Sample(glm::vec2 uv) const;
        static bool Load(const fs::path& path, Image& out);
        bool SavePng(const fs::path& path) const;
    };

    enum class Blend
    {
        Mix = 0,  // towards the color
        Add,
        Subtract,
        Multiply,
        Lighten,
        Darken,
        Erase,    // alpha down (texture paint)
        Blur,     // averages with the neighbours
        Count
    };
    const char* BlendName(Blend b);
    // `dst` blended with `src` by `weight` (0..1; the source's alpha scales it too).
    glm::vec4 BlendColor(const glm::vec4& dst, const glm::vec4& src, Blend mode, float weight);

    struct Brush
    {
        glm::vec4 Color{ 1.0f };  // 0..1, sRGB, with alpha
        float Radius = 0.1f;      // model units
        float Strength = 1.0f;    // 0..1
        float Hardness = 0.0f;    // 0: a soft edge all the way in, 1: full strength up to the rim
        Blend Mode = Blend::Mix;
        bool FrontFacesOnly = true;
        glm::vec3 ViewDir{ 0, 0, -1 }; // the camera's looking direction (model space)
        uint8_t Symmetry = 0;          // bits: 1 X, 2 Y, 4 Z (the model's own planes)
    };
    // The brush's weight at a distance (0 outside it).
    float BrushWeight(const Brush& brush, float distance);

    // Changed pixels' bounds.
    struct Rect
    {
        int X0 = 1 << 30, Y0 = 1 << 30, X1 = -1, Y1 = -1; // inclusive
        bool Empty() const { return X1 < X0; }
        void Add(int x, int y)
        {
            X0 = std::min(X0, x);
            Y0 = std::min(Y0, y);
            X1 = std::max(X1, x);
            Y1 = std::max(Y1, y);
        }
        void Add(const Rect& r)
        {
            if (r.Empty()) return;
            Add(r.X0, r.Y0);
            Add(r.X1, r.Y1);
        }
    };

    // Vertex Paint: the corners of vertices inside the brush (hidden faces left alone). Returns how many changed.
    size_t PaintVertices(EditableMesh& m, const glm::vec3& center, const Brush& brush);
    // Texture Paint: the texels whose point on the surface (through the faces' UVs) is inside the brush, a little past
    // each face's UV edges so seams don't show gaps.
    Rect PaintTexture(const EditableMesh& m, Image& image, const glm::vec3& center, const Brush& brush);
    // Every corner of `faces` one color.
    void FillVertices(EditableMesh& m, const std::vector<uint32_t>& faces, uint32_t rgba);
    // The UV and vertex color at a point on a face (barycentric over its triangles).
    bool SurfaceAt(const EditableMesh& m, uint32_t face, const glm::vec3& point, glm::vec2& uv, glm::vec4& color);
}
