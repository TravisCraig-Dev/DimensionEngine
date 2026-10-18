#pragma once
#include "Dimension/Core/Guid.h"
#include "Dimension/ECS/Entity.h"
#include "Dimension/Renderer/Font.h"
#include "Dimension/Particles/ParticleSystem.h"
#include "Dimension/Animation/AnimatorController.h"
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/quaternion.hpp>
#include <nlohmann/json.hpp>
#include <memory>
#include <string>
#include <vector>

namespace Dimension
{
    class ScriptableEntity;

    // ---- Core (every entity has these) -------------------------------------

    struct IDComponent
    {
        Guid ID;
    };

    struct TagComponent
    {
        std::string Name = "GameObject";
        std::string Tag = "Untagged";
        bool Active = true;
    };

    struct TransformComponent
    {
        glm::vec3 Position{ 0.0f };
        glm::vec3 Rotation{ 0.0f }; // Euler angles, radians (XYZ)
        glm::vec3 Scale{ 1.0f };

        // Computed each frame by the transform system (not serialized).
        glm::mat4 World{ 1.0f };
        bool ActiveInHierarchy = true;

        glm::quat GetRotationQuat() const { return glm::quat(Rotation); }
        glm::mat4 GetLocalMatrix() const
        {
            return glm::translate(glm::mat4(1.0f), Position) * glm::toMat4(GetRotationQuat()) * glm::scale(glm::mat4(1.0f), Scale);
        }
        glm::vec3 GetWorldPosition() const { return glm::vec3(World[3]); }
        glm::vec3 Forward() const { return glm::normalize(glm::vec3(World * glm::vec4(0, 0, -1, 0))); }
        glm::vec3 Right() const { return glm::normalize(glm::vec3(World * glm::vec4(1, 0, 0, 0))); }
        glm::vec3 Up() const { return glm::normalize(glm::vec3(World * glm::vec4(0, 1, 0, 0))); }
    };

    struct HierarchyComponent
    {
        Entity Parent = Entity::Null();
        std::vector<Entity> Children;
    };

    // Links an entity to the prefab asset it was instantiated from (see Scene/Prefab.h).
    struct PrefabLinkComponent
    {
        AssetHandle Prefab = AssetHandle::None();
        uint64_t SourceId = 0;                 // guid of the corresponding entity inside the prefab file
        bool IsRoot = false;
        std::vector<std::string> Overrides;    // "Component.Field", "+Component" (added), "-Component" (removed)

        bool HasOverride(const std::string& key) const
        {
            for (auto& o : Overrides)
                if (o == key) return true;
            return false;
        }
        void AddOverride(const std::string& key)
        {
            if (!HasOverride(key)) Overrides.push_back(key);
        }
    };

    // ---- Rendering ----------------------------------------------------------

    enum class ProjectionType : int32_t { Perspective = 0, Orthographic = 1 };
    enum class ClearMode : int32_t { SolidColor = 0, Skybox = 1 };

    struct CameraComponent
    {
        ProjectionType Projection = ProjectionType::Perspective;
        float FieldOfView = 60.0f; // degrees, vertical
        float OrthographicSize = 5.0f; // half height in world units
        float NearClip = 0.1f;
        float FarClip = 1000.0f;
        bool Primary = true;
        ClearMode Clear = ClearMode::SolidColor;
        glm::vec4 ClearColor{ 0.19f, 0.30f, 0.47f, 1.0f };

        glm::mat4 GetProjection(float aspect) const;
    };

    struct SpriteRendererComponent
    {
        glm::vec4 Color{ 1.0f };
        AssetHandle Texture = AssetHandle::None();
        glm::vec2 Tiling{ 1.0f };
        int32_t SortingOrder = 0;
        bool FlipX = false;
        bool FlipY = false;
        // Sprite sheet support: the texture is a grid of frames, Frame picks one (row-major from top-left).
        int32_t SheetColumns = 1;
        int32_t SheetRows = 1;
        int32_t Frame = 0;
    };

    // Plays a frame range of the SpriteRenderer's sheet while the scene runs.
    struct SpriteAnimatorComponent
    {
        float FramesPerSecond = 10.0f;
        int32_t FirstFrame = 0;
        int32_t FrameCount = 1;
        bool Loop = true;
        bool Playing = true;
        float Time = 0.0f; // runtime only
    };

    struct MeshRendererComponent
    {
        AssetHandle Mesh = AssetHandle::None();
        AssetHandle Material = AssetHandle::None(); // when set, overrides the simple fields below
        glm::vec4 Color{ 1.0f };
        AssetHandle Texture = AssetHandle::None();
        glm::vec2 Tiling{ 1.0f };
        float Smoothness = 0.5f;
        float Emission = 0.0f;
        bool CastShadows = true;
    };

    struct DirectionalLightComponent
    {
        glm::vec3 Color{ 1.0f, 0.96f, 0.84f };
        float Intensity = 1.0f;
        bool CastShadows = true;
        float ShadowDistance = 40.0f; // world units from the camera covered by the shadow map
        float ShadowBias = 0.0015f;
    };

    struct PointLightComponent
    {
        glm::vec3 Color{ 1.0f };
        float Intensity = 1.0f;
        float Range = 10.0f;
    };

    // Plays skeletal animation clips of the entity's skinned Mesh Renderer (like Unity's Animator/Animation).
    struct AnimatorComponent
    {
        AssetHandle Controller = AssetHandle::None(); // optional state machine; when set it drives Clip/Loop
        std::string Clip;          // clip name inside the mesh asset
        float Speed = 1.0f;
        bool Loop = true;
        bool Playing = true;

        // State machine (runtime only).
        AnimatorRuntime Runtime;
        float StateSpeed = 1.0f;
        std::string CurrentState;

        void SetFloat(const std::string& name, float v) { Runtime.Parameters[name] = v; }
        void SetInteger(const std::string& name, int v) { Runtime.Parameters[name] = (float)v; }
        void SetBool(const std::string& name, bool v) { Runtime.Parameters[name] = v ? 1.0f : 0.0f; }
        void SetTrigger(const std::string& name) { Runtime.Triggers.insert(name); }
        void ResetTrigger(const std::string& name) { Runtime.Triggers.erase(name); }

        // Runtime state (not serialized).
        float Time = 0.0f;
        std::string PreviousClip;  // crossfade source
        float PreviousTime = 0.0f;
        float FadeElapsed = 0.0f, FadeDuration = 0.0f;
        std::vector<glm::mat4> SkinMatrices;

        // Switches clips, blending from the current pose over `fadeSeconds`.
        void CrossFade(const std::string& clip, float fadeSeconds = 0.25f)
        {
            if (clip == Clip) return;
            PreviousClip = Clip;
            PreviousTime = Time;
            Clip = clip;
            Time = 0.0f;
            FadeElapsed = 0.0f;
            FadeDuration = fadeSeconds;
            Playing = true;
        }
    };

    // World-space text (like TextMeshPro 3D). Drawn through the sprite batcher.
    struct TextRendererComponent
    {
        std::string Text = "Hello World";
        AssetHandle Font = AssetHandle::None(); // None = built-in font
        glm::vec4 Color{ 1.0f };
        float FontSize = 0.5f;                  // world units per line
        TextAlign Align = TextAlign::Center;
        int32_t SortingOrder = 0;
    };

    // ---- UI (screen space) --------------------------------------------------------
    // Coordinates are in canvas units with a top-left origin and y pointing down.

    enum class CanvasScaleMode : int32_t { ConstantPixelSize = 0, ScaleWithScreenSize = 1 };

    struct CanvasComponent
    {
        CanvasScaleMode ScaleMode = CanvasScaleMode::ScaleWithScreenSize;
        glm::vec2 ReferenceResolution{ 1920.0f, 1080.0f };
        int32_t SortingOrder = 0;
    };

    // Unity-style RectTransform: anchors are fractions of the parent rect (0,0 = top-left).
    struct UIRectComponent
    {
        glm::vec2 AnchorMin{ 0.5f };
        glm::vec2 AnchorMax{ 0.5f };
        glm::vec2 Pivot{ 0.5f };
        glm::vec2 Position{ 0.0f }; // offset of the pivot from the anchor reference point
        glm::vec2 Size{ 160.0f, 40.0f }; // added to the anchor span (equals the size when anchors coincide)

        // Computed by the UI layout pass (screen pixels), not serialized.
        glm::vec2 ComputedMin{ 0.0f }, ComputedMax{ 0.0f };
        float ComputedScale = 1.0f;
    };

    struct UIImageComponent
    {
        AssetHandle Texture = AssetHandle::None();
        glm::vec4 Color{ 1.0f };
    };

    struct UITextComponent
    {
        std::string Text = "New Text";
        AssetHandle Font = AssetHandle::None();
        float FontSize = 32.0f;
        glm::vec4 Color{ 0.12f, 0.12f, 0.12f, 1.0f };
        TextAlign Align = TextAlign::Center;
        TextVAlign VerticalAlign = TextVAlign::Middle;
        bool Wrap = true;
    };

    // Tints the entity's UI Image by state and calls OnClick() on its scripts when clicked.
    struct UIButtonComponent
    {
        glm::vec4 NormalColor{ 1.0f };
        glm::vec4 HoverColor{ 0.92f, 0.92f, 0.92f, 1.0f };
        glm::vec4 PressedColor{ 0.75f, 0.75f, 0.75f, 1.0f };
        bool Interactable = true;
        bool Hovered = false, Pressed = false; // runtime state
    };

    // ---- Audio -------------------------------------------------------------------

    struct AudioSourceComponent
    {
        AssetHandle Clip = AssetHandle::None();
        float Volume = 1.0f;
        float Pitch = 1.0f;
        bool Loop = false;
        bool PlayOnAwake = true;
        bool Spatial = false;
        float MinDistance = 1.0f;
        float MaxDistance = 50.0f;
        std::shared_ptr<void> Runtime; // ma_sound, runtime only
    };

    // ---- Scripting -----------------------------------------------------------

    // One script attached to an entity.
    struct ScriptInstance
    {
        std::string ClassName;
        nlohmann::json Fields = nlohmann::json::object(); // serialized public fields
        bool Enabled = true;                               // disabled scripts get no update callbacks
        std::shared_ptr<ScriptableEntity> Instance;       // runtime only
    };

    // Ordered list of scripts on an entity (like several MonoBehaviours on a GameObject).
    struct ScriptComponent
    {
        std::vector<ScriptInstance> Scripts;

        ScriptInstance& Add(const std::string& className)
        {
            auto& s = Scripts.emplace_back();
            s.ClassName = className;
            return s;
        }
        ScriptInstance* Find(const std::string& className)
        {
            for (auto& s : Scripts)
                if (s.ClassName == className) return &s;
            return nullptr;
        }
        // Live instances (copied, so callbacks can safely modify the scene).
        std::vector<std::shared_ptr<ScriptableEntity>> LiveInstances(bool enabledOnly) const
        {
            std::vector<std::shared_ptr<ScriptableEntity>> out;
            for (auto& s : Scripts)
                if (s.Instance && (!enabledOnly || s.Enabled)) out.push_back(s.Instance);
            return out;
        }
    };

    // ---- Physics 2D ------------------------------------------------------------

    enum class BodyType : int32_t { Static = 0, Dynamic = 1, Kinematic = 2 };

    struct Rigidbody2DComponent
    {
        BodyType Type = BodyType::Dynamic;
        float GravityScale = 1.0f;
        float LinearDamping = 0.0f;
        bool FixedRotation = false;
        uint64_t RuntimeBody = 0; // b2BodyId packed, runtime only
    };

    struct BoxCollider2DComponent
    {
        glm::vec2 Offset{ 0.0f };
        glm::vec2 Size{ 1.0f };
        float Density = 1.0f;
        float Friction = 0.6f;
        float Restitution = 0.0f;
        bool IsTrigger = false;
    };

    struct CircleCollider2DComponent
    {
        glm::vec2 Offset{ 0.0f };
        float Radius = 0.5f;
        float Density = 1.0f;
        float Friction = 0.6f;
        float Restitution = 0.0f;
        bool IsTrigger = false;
    };

    // ---- Physics 3D ------------------------------------------------------------

    struct RigidbodyComponent
    {
        BodyType Type = BodyType::Dynamic;
        float Mass = 1.0f;
        float LinearDamping = 0.05f;
        float AngularDamping = 0.05f;
        bool UseGravity = true;
        uint32_t RuntimeBody = 0xFFFFFFFF; // JPH::BodyID, runtime only
    };

    struct BoxColliderComponent
    {
        glm::vec3 Offset{ 0.0f };
        glm::vec3 Size{ 1.0f };
        float Friction = 0.6f;
        float Restitution = 0.0f;
    };

    struct CapsuleColliderComponent // Y-axis capsule like Unity's default
    {
        glm::vec3 Offset{ 0.0f };
        float Radius = 0.5f;
        float Height = 2.0f; // total height including the caps
        float Friction = 0.6f;
        float Restitution = 0.0f;
    };

    struct SphereColliderComponent
    {
        glm::vec3 Offset{ 0.0f };
        float Radius = 0.5f;
        float Friction = 0.6f;
        float Restitution = 0.0f;
    };
}
