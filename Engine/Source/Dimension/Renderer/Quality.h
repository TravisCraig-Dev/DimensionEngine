#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace Dimension
{
    struct SceneSettings;

    // A quality level (like Unity's Quality Settings): caps on what a scene's settings ask of the renderer, so a heavy
    // scene can be edited or played on a slower machine without changing the scene. A level only ever lowers: a scene
    // without SSR stays without it at Ultra.
    struct QualityLevel
    {
        std::string Name;
        int MaxMSAA = 8;                 // samples per pixel at most (1 = off)
        bool AmbientOcclusion = true;    // SSAO
        bool ScreenSpaceReflections = true;
        bool ScreenSpaceGI = true;
        bool Bloom = true;
        bool DepthOfField = true;
        bool MotionBlur = true;
        bool CloudShadows = true;
        int MaxRaySteps = 64;            // SSR steps, volumetric fog and cloud steps at most
        int MaxGIRays = 8;               // SSGI rays per pixel at most
        int ShadowResolution = 2048;     // one shadow cascade's size in texels (512 .. 4096)
        float ShadowDistance = 1.0f;     // multiplies each light's Shadow Distance (shorter: sharper, cheaper)
        float RenderScale = 1.0f;        // the scene drawn at this fraction of the view's size (0.5 .. 1), then stretched
    };

    namespace Quality
    {
        // Low, Medium, High, Ultra (Ultra caps nothing).
        std::vector<QualityLevel> DefaultLevels();
        // The project's levels (Project Settings > Quality; the defaults when it has none).
        const std::vector<QualityLevel>& Levels();
        int Current();                  // index into Levels()
        void SetCurrent(int index);     // clamped
        const QualityLevel& Active();
        // A scene's settings with the active level's caps applied (what the renderer draws with).
        SceneSettings Apply(const SceneSettings& settings);
        // The view size the scene is drawn at for a viewport of `width` x `height` (render scale, at least 1 pixel).
        void ScaledSize(uint32_t width, uint32_t height, uint32_t& outWidth, uint32_t& outHeight);
    }
}
