#pragma once
#include "Dimension/Core/Base.h"
#include <cstdint>
#include <optional>
#include <vector>

namespace Dimension
{
    // Application icons: the engine's executables embed a default icon (resource "GLFW_ICON", which GLFW also uses
    // for windows). Build replaces it in the game's executable with the project's icon, and the player sets the
    // window icon from it at run time.
    namespace AppIcon
    {
        struct Image
        {
            int Width = 0, Height = 0;
            std::vector<uint8_t> Rgba; // 8-bit RGBA, rows top to bottom
        };

        // Sizes written to icon files and executables (the shell picks the best match).
        inline constexpr int kSizes[] = { 16, 24, 32, 48, 64, 256 };

        std::optional<Image> Load(const fs::path& path); // PNG, JPEG, BMP, TGA, ...
        // Fits the image into a size x size square (keeping its aspect ratio, transparent margins), filtering with
        // premultiplied alpha so edges do not darken.
        Image Resize(const Image& source, int size);
        // An .ico file holding every size in kSizes as PNG-compressed images.
        std::vector<uint8_t> BuildIco(const Image& source);
        // Replaces the GLFW_ICON icon group of an executable (one that is not running). Windows only.
        bool WriteToExecutable(const fs::path& exe, const Image& source);
    }
}
