#include "Dimension/Renderer/Environment.h"
#include <glm/gtc/constants.hpp>
#include <algorithm>
#include <cmath>

namespace Dimension::Environment
{
    glm::vec2 DirectionToEquirect(const glm::vec3& d)
    {
        glm::vec3 n = glm::normalize(d);
        float phi = std::atan2(n.x, -n.z);
        float theta = std::acos(std::clamp(n.y, -1.0f, 1.0f));
        return { phi / glm::two_pi<float>() + 0.5f, theta / glm::pi<float>() };
    }

    glm::vec3 EquirectToDirection(const glm::vec2& uv)
    {
        float phi = (uv.x - 0.5f) * glm::two_pi<float>();
        float theta = uv.y * glm::pi<float>();
        float s = std::sin(theta);
        return { std::sin(phi) * s, std::cos(theta), -std::cos(phi) * s };
    }

    namespace
    {
        // Real SH basis (bands 0-2).
        void Basis(const glm::vec3& n, float out[9])
        {
            out[0] = 0.282095f;
            out[1] = 0.488603f * n.y;
            out[2] = 0.488603f * n.z;
            out[3] = 0.488603f * n.x;
            out[4] = 1.092548f * n.x * n.y;
            out[5] = 1.092548f * n.y * n.z;
            out[6] = 0.315392f * (3.0f * n.z * n.z - 1.0f);
            out[7] = 1.092548f * n.x * n.z;
            out[8] = 0.546274f * (n.x * n.x - n.y * n.y);
        }
    }

    SH9 ComputeIrradianceSH(const float* rgba, uint32_t width, uint32_t height)
    {
        SH9 sh{};
        if (!rgba || width == 0 || height == 0) return sh;
        // A coarse grid is plenty for 9 coefficients.
        uint32_t stepX = std::max(1u, width / 128), stepY = std::max(1u, height / 64);
        float weightSum = 0.0f;
        for (uint32_t y = 0; y < height; y += stepY)
        {
            float v = (y + 0.5f) / height;
            float solidAngle = std::sin(v * glm::pi<float>()); // equirect texels shrink towards the poles
            for (uint32_t x = 0; x < width; x += stepX)
            {
                glm::vec3 dir = EquirectToDirection({ (x + 0.5f) / width, v });
                const float* p = rgba + (size_t(y) * width + x) * 4;
                glm::vec3 radiance(p[0], p[1], p[2]);
                float b[9];
                Basis(dir, b);
                for (int i = 0; i < 9; ++i) sh[i] += radiance * b[i] * solidAngle;
                weightSum += solidAngle;
            }
        }
        float norm = 4.0f * glm::pi<float>() / std::max(weightSum, 1e-6f);
        // Cosine-lobe convolution (Ramamoorthi & Hanrahan) divided by pi, so the result is outgoing radiance for albedo 1.
        const float band[9] = { 1.0f, 2.0f / 3.0f, 2.0f / 3.0f, 2.0f / 3.0f, 0.25f, 0.25f, 0.25f, 0.25f, 0.25f };
        for (int i = 0; i < 9; ++i) sh[i] *= norm * band[i];
        return sh;
    }

    glm::vec3 EvaluateSH(const SH9& sh, const glm::vec3& normal)
    {
        float b[9];
        Basis(glm::normalize(normal), b);
        glm::vec3 r(0.0f);
        for (int i = 0; i < 9; ++i) r += sh[i] * b[i];
        return glm::max(r, glm::vec3(0.0f));
    }
}
