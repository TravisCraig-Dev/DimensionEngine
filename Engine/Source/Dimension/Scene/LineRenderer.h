#pragma once
#include "Dimension/Core/Guid.h"
#include "Dimension/Core/Curve.h"
#include <glm/glm.hpp>
#include <cstdint>
#include <vector>

namespace Dimension
{
    class Scene;

    enum class LineAlignment : int32_t { View = 0, TransformZ = 1 };
    enum class LineTextureMode : int32_t { Stretch = 0, Tile = 1 };
    enum class LineBlend : int32_t { Alpha = 0, Additive = 1 };

    // Polyline drawn as a camera-facing ribbon (Unity's LineRenderer).
    struct LineRendererComponent
    {
        std::vector<glm::vec3> Positions{ { 0.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f } };
        bool UseWorldSpace = false; // otherwise positions are in the entity's local space
        bool Loop = false;          // connect the last point back to the first
        float Width = 0.1f;
        Curve WidthCurve = Curve::Constant(1.0f); // multiplier along the line (0 = start)
        Gradient ColorGradient = Gradient::Constant(glm::vec4(1.0f));
        LineAlignment Alignment = LineAlignment::View;
        AssetHandle Texture = AssetHandle::None();                     // none = solid white
        LineTextureMode TextureMode = LineTextureMode::Stretch;
        LineBlend Blend = LineBlend::Alpha;
        int32_t SortingOrder = 0;
    };

    // Ribbon left behind a moving object (Unity's TrailRenderer). Points are recorded in world space while playing.
    struct TrailRendererComponent
    {
        float Time = 1.0f;              // seconds a point lives
        float MinVertexDistance = 0.1f; // world distance moved before a new point is added
        float Width = 0.25f;
        Curve WidthCurve = Curve::Linear(1.0f, 0.0f); // 0 = head (newest), 1 = tail
        Gradient ColorGradient = Gradient::Linear(glm::vec4(1.0f), glm::vec4(1.0f, 1.0f, 1.0f, 0.0f));
        bool Emitting = true;
        AssetHandle Texture = AssetHandle::None();
        LineTextureMode TextureMode = LineTextureMode::Stretch;
        LineBlend Blend = LineBlend::Alpha;
        int32_t SortingOrder = 0;

        // Runtime state (not serialized): oldest first.
        struct Point { glm::vec3 Position; float Time; };
        std::vector<Point> Points;
        void Clear() { Points.clear(); }
    };

    // One point of a ribbon after width and color are resolved.
    struct RibbonPoint
    {
        glm::vec3 Position;
        float Width;
        glm::vec4 Color;
    };

    // Four corners of one ribbon segment, in the sprite batcher's corner order
    // (start-left, start-right, end-right, end-left). Adjacent segments share their edge.
    struct RibbonQuad
    {
        glm::vec3 P[4];
        glm::vec4 C[4];
        glm::vec2 UV[4];
    };

    struct RibbonParams
    {
        bool FaceCamera = true;           // false: face along Normal (TransformZ alignment)
        bool Orthographic = false;        // face camera direction instead of camera position
        glm::vec3 CameraPosition{ 0.0f };
        glm::vec3 CameraForward{ 0.0f, 0.0f, -1.0f };
        glm::vec3 Normal{ 0.0f, 0.0f, 1.0f };
        bool Loop = false;
        LineTextureMode TextureMode = LineTextureMode::Stretch;
    };

    // Expands a polyline into joined quads. Points closer than 1e-5 to the previous one are skipped.
    std::vector<RibbonQuad> BuildRibbon(const std::vector<RibbonPoint>& points, const RibbonParams& params);

    namespace TrailRenderers
    {
        // Ages out old points and records new ones (play mode only).
        void Update(Scene& scene, float time);
        void ClearAll(Scene& scene);
    }
}
