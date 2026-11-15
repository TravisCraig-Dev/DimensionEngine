#pragma once
#include "Dimension/Modeling/Paint.h"
#include "Dimension/Renderer/Mesh.h"

// Texture baking (Blender's Bake, "selected to active"): maps for a low-detail mesh's UV layout, read from a detailed
// mesh over it (or from itself): a tangent-space normal map, ambient occlusion and curvature.
namespace Dimension::Modeling::Bake
{
    struct Settings
    {
        uint32_t Width = 1024, Height = 1024;
        bool Normal = true;           // needs a detailed source
        bool AmbientOcclusion = true;
        bool Curvature = true;
        float CageExtrusion = 0.05f;  // rays start this far out along the low mesh's normal and look this far in past it (model units)
        int AOSamples = 32;
        float AODistance = 0.5f;      // occluders farther than this don't darken (model units)
        int Margin = 8;               // texels the maps spread past the UV islands' edges
        int Threads = 0;              // 0: all cores
    };

    struct Result
    {
        Paint::Image Normal, AmbientOcclusion, Curvature; // the ones asked for
        size_t Texels = 0;  // texels covered by the UV layout
        size_t Missed = 0;  // of those, where the cage rays found no detailed surface (the low mesh's own surface used)
    };

    // `low`: the mesh being textured, as it renders (UVs, normals and tangents as the renderer uses them). `high`: the
    // detailed source placed by `highToLow` into low's space, or null to bake low from itself.
    Result BakeMaps(const MeshData& low, const MeshData* high, const glm::mat4& highToLow, const Settings& settings);

    // Spreads covered texels (`covered` per texel) outwards by `iterations` texels into the uncovered ones around them.
    void Dilate(Paint::Image& image, std::vector<uint8_t> covered, int iterations);
}
