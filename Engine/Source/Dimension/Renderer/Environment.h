#pragma once
#include <glm/glm.hpp>
#include <array>
#include <cstdint>

namespace Dimension
{
    // Equirectangular (latitude/longitude) environment maps. Must match DirToEquirect in Common.glsl:
    // u = 0.5 at -Z (forward), increasing towards +X; v = 0 at +Y (up).
    namespace Environment
    {
        glm::vec2 DirectionToEquirect(const glm::vec3& direction);
        glm::vec3 EquirectToDirection(const glm::vec2& uv);

        // Diffuse irradiance of an HDR equirect image (linear RGBA floats) as 9 spherical-harmonic
        // coefficients, cosine-convolved and divided by pi: a constant environment of radiance L gives L.
        using SH9 = std::array<glm::vec3, 9>;
        SH9 ComputeIrradianceSH(const float* rgba, uint32_t width, uint32_t height);
        glm::vec3 EvaluateSH(const SH9& sh, const glm::vec3& normal);
    }
}
