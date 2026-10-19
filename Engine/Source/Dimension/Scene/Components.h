#pragma once
#include "Dimension/Scene/Tilemap.h"
#include "Dimension/Scene/LineRenderer.h"
#include "Dimension/Core/Guid.h"
#include "Dimension/ECS/Entity.h"
#include "Dimension/Renderer/Font.h"
#include "Dimension/Particles/ParticleSystem.h"
#include "Dimension/Animation/AnimatorController.h"
#include <glm/glm.hpp>
#include <algorithm>
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
        int32_t Layer = 0; // physics layer (0-31), see Project Settings > Physics Layers
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

    // Tilemap: see Tilemap.h.

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
        int32_t ShadowCascades = 4;   // 1-4 cascades split over the shadow distance
        bool DebugCascades = false;   // tint surfaces by cascade (red, green, blue, yellow)
    };

    struct PointLightComponent
    {
        glm::vec3 Color{ 1.0f };
        float Intensity = 1.0f;
        float Range = 10.0f;
        bool CastShadows = false;  // up to 4 point lights cast shadows (cube shadow maps)
        float ShadowBias = 0.05f;  // world units
    };

    // Cone light shining along the entity's forward (-Z) axis, like Unity's Spot Light.
    struct SpotLightComponent
    {
        glm::vec3 Color{ 1.0f };
        float Intensity = 1.0f;
        float Range = 10.0f;
        float SpotAngle = 45.0f;      // full outer cone angle, degrees
        float InnerSpotAngle = 30.0f; // full angle of the fully lit core, degrees
        bool CastShadows = false;     // shares the 4 shadow slots with point lights
        float ShadowBias = 0.05f;     // world units
    };

    // Plays skeletal animation clips of the entity's skinned Mesh Renderer (like Unity's Animator/Animation).
    // Playback of one extra Animator layer (runtime only); mirrors the base layer's fields on AnimatorComponent.
    struct AnimatorLayerPlayback
    {
        AnimatorRuntime Runtime;
        std::string Clip;
        float Time = 0.0f;
        bool Loop = true;
        bool Playing = true;
        float StateSpeed = 1.0f;
        std::string CurrentState;
        int TreeState = -1;
        std::string PreviousClip;
        int PreviousTreeState = -1;
        float PreviousTime = 0.0f;
        float FadeElapsed = 0.0f, FadeDuration = 0.0f;
        float Weight = -1.0f; // < 0: use the layer's weight from the controller
    };

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
        // Extra layers are 1-based (layer 0 is the base). Weight changes take effect immediately.
        void SetLayerWeight(int layer, float weight)
        {
            if (layer < 1) return;
            if ((int)Layers.size() < layer) Layers.resize(layer);
            Layers[layer - 1].Weight = std::clamp(weight, 0.0f, 1.0f);
        }
        std::vector<AnimatorLayerPlayback> Layers; // runtime, one per extra controller layer
        void ResetTrigger(const std::string& name) { Runtime.Triggers.erase(name); }

        // Runtime state (not serialized).
        float Time = 0.0f;
        int TreeState = -1;        // controller state index when playing a blend tree (time is the state machine's)
        std::string PreviousClip;  // crossfade source
        int PreviousTreeState = -1;
        float PreviousTime = 0.0f; // seconds for clips, normalized time for blend trees
        float FadeElapsed = 0.0f, FadeDuration = 0.0f;
        std::vector<glm::mat4> SkinMatrices;

        // Switches clips, blending from the current pose over `fadeSeconds`.
        void CrossFade(const std::string& clip, float fadeSeconds = 0.25f)
        {
            if (clip == Clip && TreeState < 0) return;
            PreviousClip = Clip;
            PreviousTreeState = TreeState;
            PreviousTime = TreeState >= 0 ? Runtime.StateTime : Time;
            TreeState = -1;
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
        glm::vec2 ClipMin{ 0.0f }, ClipMax{ 0.0f }; // visible region (scroll views clip their content)
        float ComputedScale = 1.0f;
    };

    enum class UIImageType : int32_t { Simple = 0, Sliced = 1 };

    struct UIImageComponent
    {
        AssetHandle Texture = AssetHandle::None();
        glm::vec4 Color{ 1.0f };
        UIImageType Type = UIImageType::Simple;
        glm::vec4 Border{ 0.0f };   // 9-slice borders in texture pixels: left, top, right, bottom
        float BorderScale = 1.0f;   // canvas units per border pixel
    };

    // Horizontal slider: drag anywhere on the rect to set Value.
    struct UISliderComponent
    {
        float MinValue = 0.0f;
        float MaxValue = 1.0f;
        float Value = 0.5f;
        bool WholeNumbers = false;
        bool Interactable = true;
        glm::vec4 BackgroundColor{ 0.22f, 0.22f, 0.25f, 1.0f };
        glm::vec4 FillColor{ 0.25f, 0.55f, 0.95f, 1.0f };
        glm::vec4 HandleColor{ 1.0f };
        bool Dragging = false; // runtime
    };

    // Checkbox with an optional label; clicking flips IsOn.
    struct UIToggleComponent
    {
        bool IsOn = false;
        std::string Label = "Toggle";
        float FontSize = 28.0f;
        bool Interactable = true;
        glm::vec4 BoxColor{ 1.0f };
        glm::vec4 CheckColor{ 0.2f, 0.5f, 0.95f, 1.0f };
        glm::vec4 LabelColor{ 1.0f };
        bool Pressed = false; // runtime
    };

    // Single-line text entry: click to focus, type, Enter submits.
    struct UIInputFieldComponent
    {
        std::string Text;
        std::string Placeholder = "Enter text...";
        int32_t CharacterLimit = 0; // 0 = unlimited
        float FontSize = 28.0f;
        bool Interactable = true;
        glm::vec4 BackgroundColor{ 1.0f };
        glm::vec4 TextColor{ 0.1f, 0.1f, 0.12f, 1.0f };
        glm::vec4 PlaceholderColor{ 0.55f, 0.55f, 0.58f, 1.0f };
        bool Focused = false;   // runtime
        float CaretTime = 0.0f; // runtime
        // Runtime editing state: byte offsets into Text (always on UTF-8 boundaries).
        int32_t Caret = 0;
        int32_t SelectionAnchor = -1; // -1 = no selection
        bool Selecting = false;       // mouse drag in progress
        float ScrollX = 0.0f;         // pixels the text is scrolled left to keep the caret visible
    };

    enum class UILayoutDirection : int32_t { Vertical = 0, Horizontal = 1 };

    // Arranges child UI Rects in a row or column (like Vertical/Horizontal Layout Group).
    struct UILayoutGroupComponent
    {
        UILayoutDirection Direction = UILayoutDirection::Vertical;
        float Spacing = 8.0f;
        glm::vec4 Padding{ 8.0f };  // left, top, right, bottom
        bool StretchChildren = true; // children fill the cross axis
    };

    // Clips its children to its rect and scrolls its first child (the content) vertically.
    // A scrollbar on the right edge appears while the content is taller than the view.
    struct UIScrollViewComponent
    {
        float ScrollSpeed = 60.0f;  // canvas units per wheel notch
        bool Scrollbar = true;
        float ScrollbarWidth = 10.0f; // canvas units
        glm::vec4 ScrollbarTrackColor{ 1.0f, 1.0f, 1.0f, 0.12f };
        glm::vec4 ScrollbarHandleColor{ 1.0f, 1.0f, 1.0f, 0.55f };

        // Runtime (not serialized)
        float ScrollPosition = 0.0f; // canvas units scrolled down
        float MaxScroll = 0.0f;      // set by the layout pass
        bool DraggingScrollbar = false;
        float DragOffset = 0.0f;     // pointer offset inside the handle (pixels)
    };

    // Closed: shows the selected option. Clicking opens a list over all other UI; picking an item sets Value.
    struct UIDropdownComponent
    {
        std::vector<std::string> Options{ "Option A", "Option B", "Option C" };
        int32_t Value = 0; // selected option index
        float FontSize = 28.0f;
        int32_t MaxVisibleItems = 6; // the list scrolls beyond this
        bool Interactable = true;
        glm::vec4 BackgroundColor{ 1.0f };
        glm::vec4 TextColor{ 0.1f, 0.1f, 0.12f, 1.0f };
        glm::vec4 ListColor{ 0.97f, 0.97f, 0.98f, 1.0f };
        glm::vec4 HighlightColor{ 0.25f, 0.55f, 0.95f, 1.0f };

        // Runtime (not serialized)
        bool Open = false;
        bool Pressed = false;
        int32_t HoveredItem = -1;
        float ListScroll = 0.0f; // first visible item
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

    // Captures the surroundings at its position; meshes inside its box reflect that instead of the sky (Unity's
    // Reflection Probe). Baked when first rendered, when moved or resized, on request, or every frame (Realtime).
    struct ReflectionProbeComponent
    {
        glm::vec3 Size{ 10.0f, 5.0f, 10.0f }; // influence box, centered on the probe
        int32_t Resolution = 1;               // 0: 64, 1: 128, 2: 256, 3: 512 pixels per cube face
        float Intensity = 1.0f;
        bool BoxProjection = true;            // parallax-correct reflections against the box (rooms, courtyards)
        bool Realtime = false;                // re-capture every frame (six extra scene renders)
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
        std::string Output;          // mixer group name; empty = Master
        bool Stream = false;         // decode while playing (music) instead of loading the whole clip
        bool Mute = false;
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
        bool IsTrigger = false; // sensor: reports OnTriggerEnter/Exit instead of colliding
    };

    struct CapsuleColliderComponent // Y-axis capsule like Unity's default
    {
        glm::vec3 Offset{ 0.0f };
        float Radius = 0.5f;
        float Height = 2.0f; // total height including the caps
        float Friction = 0.6f;
        float Restitution = 0.0f;
        bool IsTrigger = false;
    };

    struct SphereColliderComponent
    {
        glm::vec3 Offset{ 0.0f };
        float Radius = 0.5f;
        float Friction = 0.6f;
        float Restitution = 0.0f;
        bool IsTrigger = false;
    };

    // Collider from a mesh: exact triangles (static/kinematic bodies) or its convex hull (any body).
    struct MeshColliderComponent
    {
        AssetHandle Mesh = AssetHandle::None(); // None = the entity's Mesh Renderer mesh
        bool Convex = false;                    // dynamic bodies always use the convex hull
        float Friction = 0.6f;
        float Restitution = 0.0f;
        bool IsTrigger = false;
    };

    // Capsule character moved by scripts (Unity's CharacterController), on Jolt's CharacterVirtual:
    // collides and slides, walks up steps and slopes, pushes rigidbodies. Not simulated by itself.
    struct CharacterControllerComponent
    {
        float Height = 2.0f;
        float Radius = 0.5f;
        glm::vec3 Center{ 0.0f };   // capsule center relative to the entity
        float SlopeLimit = 45.0f;   // degrees
        float StepOffset = 0.3f;
        float SkinWidth = 0.02f;
        float Mass = 70.0f;
        float PushStrength = 100.0f; // max force on rigidbodies
        bool RideMovingPlatforms = true; // standing on a moving body carries the character along

        // Runtime (not serialized)
        bool IsGrounded = false;
        glm::vec3 Velocity{ 0.0f };   // actual velocity of the last move
        float VerticalSpeed = 0.0f;   // SimpleMove's gravity accumulator (set it to jump)
        glm::vec3 GroundVelocity{ 0.0f }; // velocity of the moving platform under the character (last move)
        std::shared_ptr<void> Runtime; // JPH::CharacterVirtual
    };

    // ---- Joints ------------------------------------------------------------------

    enum class JointType : int32_t { Fixed = 0, Hinge = 1, Point = 2, Distance = 3 };

    // Connects this entity's Rigidbody to another Rigidbody, or to the world when Connected Body is empty.
    struct JointComponent
    {
        JointType Type = JointType::Hinge;
        Entity ConnectedBody = Entity::Null();
        glm::vec3 Anchor{ 0.0f };          // pivot in this entity's local space
        glm::vec3 Axis{ 0.0f, 0.0f, 1.0f }; // hinge axis in local space
        glm::vec3 ConnectedAnchor{ 0.0f }; // distance joint: point on the connected body (local), or world point
        bool UseLimits = false;
        float MinAngle = -45.0f;           // hinge limits, degrees
        float MaxAngle = 45.0f;
        float MinDistance = 0.0f;          // distance joint; 0 = the initial distance
        float MaxDistance = 0.0f;
        bool EnableCollision = false;      // let the two connected bodies collide with each other
        bool UseMotor = false;             // hinge: drive toward Motor Speed
        float MotorSpeed = 90.0f;          // degrees per second (can be changed while playing)
        float MotorMaxTorque = 1000.0f;    // N*m
        float BreakForce = 0.0f;           // N; the joint breaks above this (0 = unbreakable)
        float BreakTorque = 0.0f;          // N*m; 0 = unbreakable

        // Runtime (not serialized)
        bool Broken = false;
        float CurrentForce = 0.0f, CurrentTorque = 0.0f; // reaction during the last physics step
    };

    enum class Joint2DType : int32_t { Fixed = 0, Hinge = 1, Distance = 2 };

    // 2D joint between this entity's Rigidbody 2D and another (or the world).
    struct Joint2DComponent
    {
        Joint2DType Type = Joint2DType::Hinge;
        Entity ConnectedBody = Entity::Null();
        glm::vec2 Anchor{ 0.0f };          // pivot in this entity's local space
        glm::vec2 ConnectedAnchor{ 0.0f }; // distance joint: point on the connected body (local), or world point
        bool UseLimits = false;
        float LowerAngle = -45.0f;         // hinge limits, degrees
        float UpperAngle = 45.0f;
        float Distance = 0.0f;             // 0 = the initial distance
        bool CollideConnected = false;
        bool UseMotor = false;             // hinge: drive toward Motor Speed
        float MotorSpeed = 90.0f;          // degrees per second (can be changed while playing)
        float MotorMaxTorque = 100.0f;     // N*m
        float BreakForce = 0.0f;           // N; 0 = unbreakable
        float BreakTorque = 0.0f;          // N*m; 0 = unbreakable

        // Runtime (not serialized)
        bool Broken = false;
        float CurrentForce = 0.0f, CurrentTorque = 0.0f;
    };
}
