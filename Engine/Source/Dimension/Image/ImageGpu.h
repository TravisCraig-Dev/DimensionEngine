#pragma once
#include "Dimension/Image/ImageDocument.h"
#include <string>

// Image Mode on the GPU (compute shaders): compositing a document (every layer kind, blend mode, group, mask,
// adjustment and effect, with the CPU's math: ImageComposite.comp) and the blurs most filters are built on
// (ImageBoxBlur.comp). Layers' pixels stay on the GPU between composites (uploaded when a layer changes; Touch sends
// what a brush changed in place). Everything falls back to the CPU where there is no GPU (headless tools and tests).
namespace Dimension::Imaging::Gpu
{
    bool Available();
    // The document composited into `out` (made canvas-sized when it is not) inside `area` (inclusive; empty = all);
    // false: not done (no GPU, groups nested too deep), and the CPU should.
    bool Composite(const Document& doc, Bitmap& out, Rect area = {});
    // Bytes of a layer's pixels (bytesPerPixel 4) or mask (1) changed in place in `area` (a brush): sent again.
    void Touch(const void* data, Rect area, uint32_t width, uint32_t bytesPerPixel);
    // Everything on the GPU let go (an image closed; before the device goes).
    void Release();
    // The milliseconds the last composite took (on the GPU, with its uploads and the read back).
    double LastMilliseconds();
    // Box blur passes on the GPU (the hook ImageFilters' blurs take: SetBoxBlurHook).
    bool BoxBlur(std::vector<glm::vec4>& colors, int width, int height, int radius, int passes);

    // The GPU's results against the CPU's (every blend mode, groups, masks, adjustments, effects, text and shapes; a
    // part of the canvas; the blur), and how long each takes on a big picture. `ok`: they match.
    std::string SelfCheck(bool& ok);
}
