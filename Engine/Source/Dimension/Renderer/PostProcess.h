#pragma once
#include <glm/glm.hpp>
#include <cstdint>

namespace Dimension
{
    class Scene;

    // The post-processing a camera gets (Unity's volume framework): the scene's settings, then each Post Process
    // Volume that reaches the camera blended over them in Priority order. A volume reaches a camera when its layer is
    // in the camera's culling mask and, unless Global, the camera is inside its box or within Blend Distance of it
    // (fading in over that distance); its Weight scales it. Only the values a volume overrides change.
    struct PostProcessValues
    {
        float Exposure = 1.0f;
        float BloomIntensity = 1.0f;
        float Vignette = 0.0f;
        float Saturation = 1.0f;          // 0 grey, 1 as is, above 1 more vivid
        float Contrast = 1.0f;            // around mid grey
        glm::vec3 ColorFilter{ 1.0f };    // multiplies the image (linear)
    };

    namespace PostProcess
    {
        PostProcessValues Evaluate(Scene& scene, const glm::vec3& cameraPosition, uint32_t cullingMask);
    }
}
