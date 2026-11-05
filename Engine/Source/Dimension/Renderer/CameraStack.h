#pragma once
#include "Dimension/Core/Base.h"
#include "Dimension/ECS/Entity.h"
#include "Dimension/UI/UI.h"
#include "Dimension/Renderer/Vulkan.h"
#include <glm/glm.hpp>
#include <unordered_map>
#include <vector>

namespace Dimension
{
    class Scene;
    class SceneRenderer;
    class SceneRenderTarget;

    // The scene's other cameras drawn over the primary one (Unity's camera stacking by viewport): every active camera
    // other than the primary whose Viewport Rect is smaller than the whole screen (picture in picture, split screen,
    // minimaps) renders into its own image, in Depth order (higher on top), and is drawn over the primary camera's
    // view under the UI. Cameras covering the whole screen stay off unless primary, as before. Cameras with a Target
    // Texture draw into that Render Texture instead (first, so the others show it the same frame). Each has its own
    // renderer (its history, occlusion and probe captures), kept while the camera is.
    class CameraStack
    {
    public:
        CameraStack();
        ~CameraStack();
        CameraStack(const CameraStack&) = delete;
        CameraStack& operator=(const CameraStack&) = delete;

        // Renders the extra cameras for a screen of `screen` pixels; `out` gets the quads that show them (for
        // SceneRenderOptions::Underlays of the primary camera's render).
        void Render(VkCommandBuffer cmd, Scene& scene, glm::uvec2 screen, std::vector<UI::DrawCommand>& out);
        // The extra cameras Render would draw, in drawing order, with their pixel rectangles (min, max).
        static std::vector<std::pair<Entity, glm::vec4>> Cameras(Scene& scene, glm::uvec2 screen);
        void Clear();

    private:
        struct View
        {
            Scope<SceneRenderer> Renderer;
            Scope<SceneRenderTarget> Target;
            Ref<class Texture2D> Texture; // a Render Texture it draws into
            uint64_t Frame = 0;
            uint32_t Requests = 0;        // Manual Render: the Camera.Render() count it last drew for
        };
        std::unordered_map<uint64_t, View> m_Views;
        uint64_t m_Frame = 0;
    };
}
