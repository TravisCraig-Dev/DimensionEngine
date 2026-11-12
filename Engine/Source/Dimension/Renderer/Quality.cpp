#include "Dimension/Renderer/Quality.h"
#include "Dimension/Project/Project.h"
#include "Dimension/Scene/Scene.h"
#include <algorithm>
#include <cmath>

namespace Dimension::Quality
{
    namespace
    {
        int s_Current = -1; // -1: the project's default
    }

    std::vector<QualityLevel> DefaultLevels()
    {
        QualityLevel low;
        low.Name = "Low";
        low.MaxMSAA = 1;
        low.AmbientOcclusion = low.ScreenSpaceReflections = low.ScreenSpaceGI = false;
        low.Bloom = low.DepthOfField = low.MotionBlur = low.CloudShadows = false;
        low.MaxRaySteps = 12;
        low.MaxGIRays = 1;
        low.ShadowResolution = 1024;
        low.ShadowDistance = 0.5f;
        low.RenderScale = 0.75f;

        QualityLevel medium;
        medium.Name = "Medium";
        medium.MaxMSAA = 2;
        medium.ScreenSpaceReflections = medium.ScreenSpaceGI = false;
        medium.MotionBlur = false;
        medium.MaxRaySteps = 24;
        medium.MaxGIRays = 2;
        medium.ShadowResolution = 1024;
        medium.ShadowDistance = 0.75f;

        QualityLevel high;
        high.Name = "High";
        high.MaxMSAA = 4;
        high.ScreenSpaceGI = false;
        high.MaxRaySteps = 48;
        high.MaxGIRays = 4;

        QualityLevel ultra;
        ultra.Name = "Ultra";
        ultra.MaxRaySteps = 1 << 20; // (the scene's own)
        ultra.MaxGIRays = 1 << 20;
        return { low, medium, high, ultra };
    }

    const std::vector<QualityLevel>& Levels()
    {
        static const std::vector<QualityLevel> defaults = DefaultLevels();
        const auto& levels = Project::GetConfig().QualityLevels;
        return levels.empty() ? defaults : levels;
    }

    int Current()
    {
        int count = (int)Levels().size();
        int index = s_Current >= 0 ? s_Current : Project::GetConfig().DefaultQuality;
        return std::clamp(index, 0, std::max(count - 1, 0));
    }

    void SetCurrent(int index) { s_Current = std::clamp(index, 0, std::max((int)Levels().size() - 1, 0)); }

    const QualityLevel& Active()
    {
        static const QualityLevel none{ "Ultra" };
        const auto& levels = Levels();
        return levels.empty() ? none : levels[Current()];
    }

    SceneSettings Apply(const SceneSettings& settings)
    {
        const QualityLevel& q = Active();
        SceneSettings s = settings;
        s.MSAA = std::min(s.MSAA, std::max(q.MaxMSAA, 1));
        s.SSAO &= q.AmbientOcclusion;
        s.SSR &= q.ScreenSpaceReflections;
        s.SSGI &= q.ScreenSpaceGI;
        s.Bloom &= q.Bloom;
        s.DepthOfField &= q.DepthOfField;
        s.MotionBlur &= q.MotionBlur;
        s.CloudShadows &= q.CloudShadows;
        int steps = std::max(q.MaxRaySteps, 1);
        s.SSRSteps = std::min(s.SSRSteps, steps);
        s.FogSteps = std::min(s.FogSteps, steps);
        s.CloudSteps = std::min(s.CloudSteps, steps);
        s.SSGIRays = std::min(s.SSGIRays, std::max(q.MaxGIRays, 1));
        return s;
    }

    void ScaledSize(uint32_t width, uint32_t height, uint32_t& outWidth, uint32_t& outHeight)
    {
        float scale = std::clamp(Active().RenderScale, 0.25f, 1.0f);
        outWidth = std::max<uint32_t>(1, (uint32_t)std::lround(width * scale));
        outHeight = std::max<uint32_t>(1, (uint32_t)std::lround(height * scale));
    }
}
