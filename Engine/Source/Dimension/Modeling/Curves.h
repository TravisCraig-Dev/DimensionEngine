#pragma once
#include "Dimension/Modeling/EditableMesh.h"
#include <functional>

// Curves and text (Blender's curve and text objects) turned into geometry, and the flat shapes both use.
namespace Dimension::Modeling
{
    namespace Shapes
    {
        // Triangles filling a polygon with holes: rings[0] is the outline, the rest holes inside it (either winding).
        // Indices into the rings' points taken in order (ring 0's, then ring 1's...).
        std::vector<uint32_t> Triangulate(const std::vector<std::vector<glm::vec2>>& rings);
        // Closed outlines in the XY plane filled (contours inside others are holes, inside those solid again...) and,
        // with `depth`, made solid: front at +depth/2 facing +Z, back at -depth/2, flat sides.
        EditableMesh Fill(const std::vector<std::vector<glm::vec2>>& contours, float depth);
    }

    namespace CurveOps
    {
        // Points along a curve (`resolution` per span; the last one left out on cyclic curves), with the radius and tilt
        // there.
        std::vector<glm::vec3> Sample(const Curve& curve, int resolution, std::vector<float>* radius = nullptr, std::vector<float>* tilt = nullptr);
        // The geometry of curves: tubes (bevel), ribbons (extrude) or filled shapes (fill).
        EditableMesh ToMesh(const std::vector<Curve>& curves, const CurveSettings& settings);
        // A curve through points (all smooth).
        Curve Make(const std::vector<glm::vec3>& points, bool cyclic);
    }

    namespace TextOps
    {
        // Where text models get font files from (set by the asset manager: 0 is the built-in font).
        using FontResolver = std::function<bool(uint64_t asset, std::vector<uint8_t>& bytes)>;
        void SetFontResolver(FontResolver resolver);
        // Letters from a TrueType/OpenType font's outlines (the first line's baseline on y = 0).
        EditableMesh ToMesh(const TextSettings& text, const uint8_t* font, size_t size);
        EditableMesh ToMesh(const TextSettings& text); // the font through the resolver
    }
}
