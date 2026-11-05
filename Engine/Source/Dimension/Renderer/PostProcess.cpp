#include "Dimension/Renderer/PostProcess.h"
#include "Dimension/Scene/Scene.h"
#include <algorithm>
#include <vector>

namespace Dimension
{
    namespace PostProcess
    {
        PostProcessValues Evaluate(Scene& scene, const glm::vec3& cameraPosition, uint32_t cullingMask)
        {
            const SceneSettings& settings = scene.GetSettings();
            PostProcessValues v;
            v.Exposure = settings.Exposure;
            v.BloomIntensity = settings.BloomIntensity;
            v.Vignette = settings.Vignette;

            auto& reg = scene.GetRegistry();
            std::vector<std::pair<float, Entity>> volumes;
            for (auto [e, vol, t] : reg.ViewOf<PostProcessVolumeComponent, TransformComponent>())
            {
                if (!t.ActiveInHierarchy || vol.Weight <= 0.0f) continue;
                int layer = reg.Get<TagComponent>(e).Layer;
                if (layer >= 0 && layer < 32 && !(cullingMask & (1u << layer))) continue;
                volumes.emplace_back(vol.Priority, e);
            }
            std::stable_sort(volumes.begin(), volumes.end(), [](auto& a, auto& b) { return a.first < b.first; });
            for (auto& [priority, e] : volumes)
            {
                const auto& vol = reg.Get<PostProcessVolumeComponent>(e);
                float w = std::clamp(vol.Weight, 0.0f, 1.0f);
                if (!vol.Global)
                {
                    // Distance from the box (in the object's space, its scale applied), fading over Blend Distance.
                    const glm::mat4& world = reg.Get<TransformComponent>(e).World;
                    glm::vec3 scale(glm::length(glm::vec3(world[0])), glm::length(glm::vec3(world[1])), glm::length(glm::vec3(world[2])));
                    glm::vec3 local = glm::vec3(glm::inverse(world) * glm::vec4(cameraPosition, 1.0f));
                    glm::vec3 outside = glm::max(glm::abs(local) - glm::abs(vol.Size) * 0.5f, glm::vec3(0.0f)) * scale;
                    float d = glm::length(outside);
                    if (d > 0.0f) w *= vol.BlendDistance > 0.0f ? std::clamp(1.0f - d / vol.BlendDistance, 0.0f, 1.0f) : 0.0f;
                }
                if (w <= 0.0f) continue;
                auto blend = [w](float& value, bool on, float target) { if (on) value += (target - value) * w; };
                blend(v.Exposure, vol.OverrideExposure, vol.Exposure);
                blend(v.BloomIntensity, vol.OverrideBloom, vol.BloomIntensity);
                blend(v.Vignette, vol.OverrideVignette, vol.Vignette);
                blend(v.Saturation, vol.OverrideSaturation, vol.Saturation);
                blend(v.Contrast, vol.OverrideContrast, vol.Contrast);
                if (vol.OverrideColorFilter) v.ColorFilter += (vol.ColorFilter - v.ColorFilter) * w;
            }
            return v;
        }
    }
}
