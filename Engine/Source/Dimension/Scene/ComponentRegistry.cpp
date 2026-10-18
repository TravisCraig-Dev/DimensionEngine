#include "Dimension/Scene/ComponentRegistry.h"

namespace Dimension
{
    std::deque<ComponentInfo>& ComponentRegistry::Infos()
    {
        static std::deque<ComponentInfo> infos;
        return infos;
    }

    size_t& ComponentRegistry::BuiltinCount()
    {
        static size_t count = 0;
        return count;
    }

    void ComponentRegistry::RemoveGameComponents()
    {
        Infos().resize(BuiltinCount());
    }

    const ComponentInfo* ComponentRegistry::Find(const std::string& name)
    {
        for (auto& i : Infos())
            if (i.Name == name) return &i;
        return nullptr;
    }

    void ComponentRegistry::RegisterBuiltins()
    {
        if (!Infos().empty()) return;

        // Core. Tag and Transform are drawn specially by the inspector; ID/Hierarchy are internal.
        Register<TagComponent>("Tag", "Core", true, false)
            .Field("Name", &TagComponent::Name)
            .Field("Tag", &TagComponent::Tag)
            .Field("Active", &TagComponent::Active)
            .Field("Layer", &TagComponent::Layer).Range(0, 31);
        Register<TransformComponent>("Transform", "Core", false, false)
            .Field("Position", &TransformComponent::Position)
            .Field("Rotation", &TransformComponent::Rotation).As(FieldType::Angles).Speed(0.5f)
            .Field("Scale", &TransformComponent::Scale);

        // Serialized with extra keys by the scene serializer; edited through the prefab header in the inspector.
        Register<PrefabLinkComponent>("Prefab Link", "Core", true, false).Field("Prefab", &PrefabLinkComponent::Prefab).As(FieldType::Prefab);

        Register<CameraComponent>("Camera", "Rendering")
            .Field("Projection", &CameraComponent::Projection).Enum({ "Perspective", "Orthographic" })
            .Field("Field Of View", &CameraComponent::FieldOfView).Range(1.0f, 179.0f).Speed(0.5f)
            .Field("Orthographic Size", &CameraComponent::OrthographicSize).Range(0.01f, 10000.0f)
            .Field("Near Clip", &CameraComponent::NearClip).Range(0.001f, 10000.0f).Speed(0.01f)
            .Field("Far Clip", &CameraComponent::FarClip).Range(0.01f, 100000.0f).Speed(1.0f)
            .Field("Primary", &CameraComponent::Primary)
            .Field("Clear Flags", &CameraComponent::Clear).Enum({ "Solid Color", "Skybox" })
            .Field("Background", &CameraComponent::ClearColor).As(FieldType::Color4);

        Register<SpriteRendererComponent>("Sprite Renderer", "Rendering")
            .Field("Sprite", &SpriteRendererComponent::Texture).As(FieldType::Texture)
            .Field("Color", &SpriteRendererComponent::Color).As(FieldType::Color4)
            .Field("Tiling", &SpriteRendererComponent::Tiling)
            .Field("Sorting Order", &SpriteRendererComponent::SortingOrder)
            .Field("Flip X", &SpriteRendererComponent::FlipX)
            .Field("Flip Y", &SpriteRendererComponent::FlipY)
            .Field("Sheet Columns", &SpriteRendererComponent::SheetColumns).Range(1, 256)
            .Field("Sheet Rows", &SpriteRendererComponent::SheetRows).Range(1, 256)
            .Field("Frame", &SpriteRendererComponent::Frame).Range(0, 65535);

        Register<SpriteAnimatorComponent>("Sprite Animator", "Rendering")
            .Field("Frames Per Second", &SpriteAnimatorComponent::FramesPerSecond).Range(0.0f, 240.0f).Speed(0.1f)
            .Field("First Frame", &SpriteAnimatorComponent::FirstFrame).Range(0, 65535)
            .Field("Frame Count", &SpriteAnimatorComponent::FrameCount).Range(1, 65535)
            .Field("Loop", &SpriteAnimatorComponent::Loop)
            .Field("Playing", &SpriteAnimatorComponent::Playing);

        Register<MeshRendererComponent>("Mesh Renderer", "Rendering")
            .Field("Mesh", &MeshRendererComponent::Mesh).As(FieldType::Mesh)
            .Field("Material", &MeshRendererComponent::Material).As(FieldType::Material)
                .Tooltip("PBR material asset. When empty, the simple fields below are used.")
            .Field("Albedo", &MeshRendererComponent::Texture).As(FieldType::Texture)
            .Field("Color", &MeshRendererComponent::Color).As(FieldType::Color4)
            .Field("Tiling", &MeshRendererComponent::Tiling)
            .Field("Smoothness", &MeshRendererComponent::Smoothness).Range(0.0f, 1.0f).Speed(0.01f)
            .Field("Emission", &MeshRendererComponent::Emission).Range(0.0f, 100.0f).Speed(0.05f)
            .Field("Cast Shadows", &MeshRendererComponent::CastShadows);

        Register<AnimatorComponent>("Animator", "Animation")
            .Field("Controller", &AnimatorComponent::Controller).As(FieldType::AnimatorController)
                .Tooltip("State machine asset (.controller). When set, it chooses the clip.")
            .Field("Clip", &AnimatorComponent::Clip)
            .Field("Speed", &AnimatorComponent::Speed).Range(-10.0f, 10.0f).Speed(0.01f)
            .Field("Loop", &AnimatorComponent::Loop)
            .Field("Playing", &AnimatorComponent::Playing);

        using PS = ParticleSystemComponent;
        Register<PS>("Particle System", "Effects")
            .Field("Duration", &PS::Duration).Range(0.05f, 1000.0f).Speed(0.05f)
            .Field("Looping", &PS::Looping)
            .Field("Play On Awake", &PS::PlayOnAwake)
            .Field("Start Lifetime", &PS::StartLifetime).Speed(0.01f).Tooltip("(min, max) seconds")
            .Field("Start Speed", &PS::StartSpeed).Speed(0.01f).Tooltip("(min, max) units per second")
            .Field("Start Size", &PS::StartSize).Speed(0.01f).Tooltip("(min, max) world units")
            .Field("Start Rotation", &PS::StartRotation).Speed(1.0f).Tooltip("(min, max) degrees")
            .Field("Angular Velocity", &PS::AngularVelocity).Speed(1.0f).Tooltip("(min, max) degrees per second")
            .Field("Start Color A", &PS::StartColorA).As(FieldType::Color4)
            .Field("Start Color B", &PS::StartColorB).As(FieldType::Color4).Tooltip("Each particle picks a color between A and B")
            .Field("Gravity Modifier", &PS::GravityModifier).Speed(0.01f)
            .Field("Drag", &PS::Drag).Range(0.0f, 100.0f).Speed(0.01f)
            .Field("Max Particles", &PS::MaxParticles).Range(1, 100000)
            .Field("Simulation Space", &PS::SimulationSpace).Enum({ "Local", "World" })
            .Field("Rate Over Time", &PS::RateOverTime).Range(0.0f, 100000.0f).Speed(0.5f)
            .Field("Burst Count", &PS::BurstCount).Range(0, 100000)
            .Field("Shape", &PS::Shape).Enum({ "Cone", "Sphere", "Box", "Point" })
            .Field("Shape Angle", &PS::ShapeAngle).Range(0.0f, 90.0f).Speed(0.5f)
            .Field("Shape Radius", &PS::ShapeRadius).Range(0.0f, 1000.0f).Speed(0.01f)
            .Field("Box Size", &PS::BoxSize).Speed(0.01f)
            .Field("Color Over Lifetime", &PS::ColorOverLifetime).Tooltip("Multiplied with the start color (normalized age 0-1)")
            .Field("Size Over Lifetime", &PS::SizeOverLifetime).Tooltip("Multiplies the start size")
            .Field("Speed Over Lifetime", &PS::SpeedOverLifetime).Tooltip("Multiplies the velocity")
            .Field("Collision", &PS::Collision).Enum({ "None", "Plane", "World" }).Tooltip("World collides with 3D physics colliders (play mode)")
            .Field("Collision Plane Height", &PS::CollisionPlaneHeight).Speed(0.01f)
            .Field("Bounce", &PS::Bounce).Range(0.0f, 2.0f).Speed(0.01f)
            .Field("Collision Dampen", &PS::CollisionDampen).Range(0.0f, 1.0f).Speed(0.01f)
            .Field("Lifetime Loss", &PS::LifetimeLoss).Range(0.0f, 1.0f).Speed(0.01f).Tooltip("Fraction of the lifetime lost per collision")
            .Field("Collision Radius", &PS::CollisionRadius).Range(0.0f, 10.0f).Speed(0.005f)
            .Field("Sub Emitter", &PS::SubEmitter).Tooltip("Another Particle System that bursts at this system's particles")
            .Field("Sub Emitter Trigger", &PS::SubEmitterTrigger).Enum({ "Birth", "Collision", "Death" })
            .Field("Sub Emit Count", &PS::SubEmitCount).Range(0, 10000)
            .Field("Trails", &PS::Trails)
            .Field("Trail Lifetime", &PS::TrailLifetime).Range(0.01f, 10.0f).Speed(0.01f)
            .Field("Trail Width", &PS::TrailWidth).Range(0.0f, 10.0f).Speed(0.01f).Tooltip("Multiplier of the particle size")
            .Field("Texture", &PS::Texture).As(FieldType::Texture)
            .Field("Blend", &PS::Blend).Enum({ "Alpha", "Additive" })
            .Field("Sorting Order", &PS::SortingOrder);

        Register<TextRendererComponent>("Text (3D)", "Rendering")
            .Field("Text", &TextRendererComponent::Text)
            .Field("Font", &TextRendererComponent::Font).As(FieldType::Font)
            .Field("Color", &TextRendererComponent::Color).As(FieldType::Color4)
            .Field("Font Size", &TextRendererComponent::FontSize).Range(0.001f, 1000.0f).Speed(0.01f)
            .Field("Alignment", &TextRendererComponent::Align).Enum({ "Left", "Center", "Right" })
            .Field("Sorting Order", &TextRendererComponent::SortingOrder);

        Register<CanvasComponent>("Canvas", "UI")
            .Field("Scale Mode", &CanvasComponent::ScaleMode).Enum({ "Constant Pixel Size", "Scale With Screen Size" })
            .Field("Reference Resolution", &CanvasComponent::ReferenceResolution).Speed(1.0f)
            .Field("Sorting Order", &CanvasComponent::SortingOrder);
        Register<UIRectComponent>("UI Rect", "UI")
            .Field("Anchor Min", &UIRectComponent::AnchorMin).Range(0.0f, 1.0f).Speed(0.005f)
            .Field("Anchor Max", &UIRectComponent::AnchorMax).Range(0.0f, 1.0f).Speed(0.005f)
            .Field("Pivot", &UIRectComponent::Pivot).Range(0.0f, 1.0f).Speed(0.005f)
            .Field("Position", &UIRectComponent::Position).Speed(1.0f)
            .Field("Size", &UIRectComponent::Size).Speed(1.0f);
        Register<UIImageComponent>("UI Image", "UI")
            .Field("Image", &UIImageComponent::Texture).As(FieldType::Texture)
            .Field("Color", &UIImageComponent::Color).As(FieldType::Color4)
            .Field("Image Type", &UIImageComponent::Type).Enum({ "Simple", "Sliced" })
            .Field("Border", &UIImageComponent::Border).Speed(0.5f).Tooltip("9-slice border in texture pixels: left, top, right, bottom")
            .Field("Border Scale", &UIImageComponent::BorderScale).Range(0.01f, 100.0f).Speed(0.01f);
        Register<UITextComponent>("UI Text", "UI")
            .Field("Text", &UITextComponent::Text)
            .Field("Font", &UITextComponent::Font).As(FieldType::Font)
            .Field("Font Size", &UITextComponent::FontSize).Range(1.0f, 1000.0f).Speed(0.5f)
            .Field("Color", &UITextComponent::Color).As(FieldType::Color4)
            .Field("Alignment", &UITextComponent::Align).Enum({ "Left", "Center", "Right" })
            .Field("Vertical Alignment", &UITextComponent::VerticalAlign).Enum({ "Top", "Middle", "Bottom" })
            .Field("Wrap", &UITextComponent::Wrap);
        Register<TilemapComponent>("Tilemap", "Rendering")
            .Field("Tileset", &TilemapComponent::Tileset).As(FieldType::Texture).Tooltip("Sprite sheet; paint with Window > Tile Palette")
            .Field("Columns", &TilemapComponent::Columns).Range(1, 256)
            .Field("Rows", &TilemapComponent::Rows).Range(1, 256)
            .Field("Cell Size", &TilemapComponent::CellSize).Range(0.01f, 100.0f).Speed(0.01f)
            .Field("Color", &TilemapComponent::Color).As(FieldType::Color4)
            .Field("Sorting Order", &TilemapComponent::SortingOrder)
            .Field("Collider", &TilemapComponent::Collider).Tooltip("Static Box2D collider covering every tile")
            .Field("Friction", &TilemapComponent::Friction).Range(0.0f, 1.0f).Speed(0.01f)
            .Field("Data", &TilemapComponent::Data).HideInInspector();
        Register<UISliderComponent>("UI Slider", "UI")
            .Field("Min Value", &UISliderComponent::MinValue).Speed(0.1f)
            .Field("Max Value", &UISliderComponent::MaxValue).Speed(0.1f)
            .Field("Value", &UISliderComponent::Value).Speed(0.01f)
            .Field("Whole Numbers", &UISliderComponent::WholeNumbers)
            .Field("Interactable", &UISliderComponent::Interactable)
            .Field("Background Color", &UISliderComponent::BackgroundColor).As(FieldType::Color4)
            .Field("Fill Color", &UISliderComponent::FillColor).As(FieldType::Color4)
            .Field("Handle Color", &UISliderComponent::HandleColor).As(FieldType::Color4);
        Register<UIToggleComponent>("UI Toggle", "UI")
            .Field("Is On", &UIToggleComponent::IsOn)
            .Field("Label", &UIToggleComponent::Label)
            .Field("Font Size", &UIToggleComponent::FontSize).Range(1.0f, 500.0f).Speed(0.5f)
            .Field("Interactable", &UIToggleComponent::Interactable)
            .Field("Box Color", &UIToggleComponent::BoxColor).As(FieldType::Color4)
            .Field("Check Color", &UIToggleComponent::CheckColor).As(FieldType::Color4)
            .Field("Label Color", &UIToggleComponent::LabelColor).As(FieldType::Color4);
        Register<UIInputFieldComponent>("UI Input Field", "UI")
            .Field("Text", &UIInputFieldComponent::Text)
            .Field("Placeholder", &UIInputFieldComponent::Placeholder)
            .Field("Character Limit", &UIInputFieldComponent::CharacterLimit).Range(0, 100000)
            .Field("Font Size", &UIInputFieldComponent::FontSize).Range(1.0f, 500.0f).Speed(0.5f)
            .Field("Interactable", &UIInputFieldComponent::Interactable)
            .Field("Background Color", &UIInputFieldComponent::BackgroundColor).As(FieldType::Color4)
            .Field("Text Color", &UIInputFieldComponent::TextColor).As(FieldType::Color4)
            .Field("Placeholder Color", &UIInputFieldComponent::PlaceholderColor).As(FieldType::Color4);
        Register<UILayoutGroupComponent>("UI Layout Group", "UI")
            .Field("Direction", &UILayoutGroupComponent::Direction).Enum({ "Vertical", "Horizontal" })
            .Field("Spacing", &UILayoutGroupComponent::Spacing).Speed(0.5f)
            .Field("Padding", &UILayoutGroupComponent::Padding).Speed(0.5f).Tooltip("left, top, right, bottom")
            .Field("Stretch Children", &UILayoutGroupComponent::StretchChildren);
        Register<UIScrollViewComponent>("UI Scroll View", "UI")
            .Field("Scroll Speed", &UIScrollViewComponent::ScrollSpeed).Range(1.0f, 10000.0f).Speed(1.0f);
        Register<UIButtonComponent>("UI Button", "UI")
            .Field("Normal Color", &UIButtonComponent::NormalColor).As(FieldType::Color4)
            .Field("Hover Color", &UIButtonComponent::HoverColor).As(FieldType::Color4)
            .Field("Pressed Color", &UIButtonComponent::PressedColor).As(FieldType::Color4)
            .Field("Interactable", &UIButtonComponent::Interactable);

        Register<DirectionalLightComponent>("Directional Light", "Rendering")
            .Field("Color", &DirectionalLightComponent::Color).As(FieldType::Color3)
            .Field("Intensity", &DirectionalLightComponent::Intensity).Range(0.0f, 100.0f).Speed(0.01f)
            .Field("Cast Shadows", &DirectionalLightComponent::CastShadows)
            .Field("Shadow Distance", &DirectionalLightComponent::ShadowDistance).Range(1.0f, 1000.0f).Speed(0.5f)
            .Field("Shadow Bias", &DirectionalLightComponent::ShadowBias).Range(0.0f, 0.05f).Speed(0.0001f)
            .Field("Shadow Cascades", &DirectionalLightComponent::ShadowCascades).Range(1, 4)
            .Field("Debug Cascades", &DirectionalLightComponent::DebugCascades);

        Register<PointLightComponent>("Point Light", "Rendering")
            .Field("Color", &PointLightComponent::Color).As(FieldType::Color3)
            .Field("Intensity", &PointLightComponent::Intensity).Range(0.0f, 1000.0f).Speed(0.05f)
            .Field("Range", &PointLightComponent::Range).Range(0.01f, 1000.0f).Speed(0.1f)
            .Field("Cast Shadows", &PointLightComponent::CastShadows)
            .Field("Shadow Bias", &PointLightComponent::ShadowBias).Range(0.0f, 1.0f).Speed(0.001f);

        Register<SpotLightComponent>("Spot Light", "Rendering")
            .Field("Color", &SpotLightComponent::Color).As(FieldType::Color3)
            .Field("Intensity", &SpotLightComponent::Intensity).Range(0.0f, 1000.0f).Speed(0.05f)
            .Field("Range", &SpotLightComponent::Range).Range(0.01f, 1000.0f).Speed(0.1f)
            .Field("Spot Angle", &SpotLightComponent::SpotAngle).Range(1.0f, 179.0f).Speed(0.5f)
            .Field("Inner Spot Angle", &SpotLightComponent::InnerSpotAngle).Range(0.0f, 179.0f).Speed(0.5f)
            .Field("Cast Shadows", &SpotLightComponent::CastShadows)
            .Field("Shadow Bias", &SpotLightComponent::ShadowBias).Range(0.0f, 1.0f).Speed(0.001f);

        Register<AudioSourceComponent>("Audio Source", "Audio")
            .Field("Clip", &AudioSourceComponent::Clip).As(FieldType::Audio)
            .Field("Volume", &AudioSourceComponent::Volume).Range(0.0f, 1.0f).Speed(0.01f)
            .Field("Pitch", &AudioSourceComponent::Pitch).Range(0.1f, 3.0f).Speed(0.01f)
            .Field("Loop", &AudioSourceComponent::Loop)
            .Field("Play On Awake", &AudioSourceComponent::PlayOnAwake)
            .Field("Spatial", &AudioSourceComponent::Spatial)
            .Field("Min Distance", &AudioSourceComponent::MinDistance).Range(0.01f, 1000.0f)
            .Field("Max Distance", &AudioSourceComponent::MaxDistance).Range(0.01f, 10000.0f)
            .Field("Output", &AudioSourceComponent::Output).Tooltip("Audio Mixer group (Window > Audio Mixer); empty = Master")
            .Field("Stream", &AudioSourceComponent::Stream).Tooltip("Stream from disk while playing (music, long clips)")
            .Field("Mute", &AudioSourceComponent::Mute);

        // Script fields are drawn/serialized specially (class picker + per-class fields).
        Register<ScriptComponent>("Script", "Scripting"); // custom-serialized list of scripts (see SceneSerializer)

        Register<Rigidbody2DComponent>("Rigidbody 2D", "Physics 2D")
            .Field("Body Type", &Rigidbody2DComponent::Type).Enum({ "Static", "Dynamic", "Kinematic" })
            .Field("Gravity Scale", &Rigidbody2DComponent::GravityScale).Speed(0.01f)
            .Field("Linear Damping", &Rigidbody2DComponent::LinearDamping).Range(0.0f, 100.0f).Speed(0.01f)
            .Field("Fixed Rotation", &Rigidbody2DComponent::FixedRotation);
        Register<BoxCollider2DComponent>("Box Collider 2D", "Physics 2D")
            .Field("Offset", &BoxCollider2DComponent::Offset)
            .Field("Size", &BoxCollider2DComponent::Size)
            .Field("Density", &BoxCollider2DComponent::Density).Range(0.0f, 1000.0f).Speed(0.01f)
            .Field("Friction", &BoxCollider2DComponent::Friction).Range(0.0f, 1.0f).Speed(0.01f)
            .Field("Bounciness", &BoxCollider2DComponent::Restitution).Range(0.0f, 1.0f).Speed(0.01f)
            .Field("Is Trigger", &BoxCollider2DComponent::IsTrigger);
        Register<CircleCollider2DComponent>("Circle Collider 2D", "Physics 2D")
            .Field("Offset", &CircleCollider2DComponent::Offset)
            .Field("Radius", &CircleCollider2DComponent::Radius).Range(0.001f, 1000.0f).Speed(0.01f)
            .Field("Density", &CircleCollider2DComponent::Density).Range(0.0f, 1000.0f).Speed(0.01f)
            .Field("Friction", &CircleCollider2DComponent::Friction).Range(0.0f, 1.0f).Speed(0.01f)
            .Field("Bounciness", &CircleCollider2DComponent::Restitution).Range(0.0f, 1.0f).Speed(0.01f)
            .Field("Is Trigger", &CircleCollider2DComponent::IsTrigger);

        Register<RigidbodyComponent>("Rigidbody", "Physics")
            .Field("Body Type", &RigidbodyComponent::Type).Enum({ "Static", "Dynamic", "Kinematic" })
            .Field("Mass", &RigidbodyComponent::Mass).Range(0.001f, 100000.0f).Speed(0.05f)
            .Field("Linear Damping", &RigidbodyComponent::LinearDamping).Range(0.0f, 100.0f).Speed(0.01f)
            .Field("Angular Damping", &RigidbodyComponent::AngularDamping).Range(0.0f, 100.0f).Speed(0.01f)
            .Field("Use Gravity", &RigidbodyComponent::UseGravity);
        Register<BoxColliderComponent>("Box Collider", "Physics")
            .Field("Offset", &BoxColliderComponent::Offset)
            .Field("Size", &BoxColliderComponent::Size)
            .Field("Friction", &BoxColliderComponent::Friction).Range(0.0f, 1.0f).Speed(0.01f)
            .Field("Bounciness", &BoxColliderComponent::Restitution).Range(0.0f, 1.0f).Speed(0.01f)
            .Field("Is Trigger", &BoxColliderComponent::IsTrigger);
        Register<CapsuleColliderComponent>("Capsule Collider", "Physics")
            .Field("Offset", &CapsuleColliderComponent::Offset)
            .Field("Radius", &CapsuleColliderComponent::Radius).Range(0.001f, 1000.0f).Speed(0.01f)
            .Field("Height", &CapsuleColliderComponent::Height).Range(0.001f, 1000.0f).Speed(0.01f)
            .Field("Friction", &CapsuleColliderComponent::Friction).Range(0.0f, 1.0f).Speed(0.01f)
            .Field("Bounciness", &CapsuleColliderComponent::Restitution).Range(0.0f, 1.0f).Speed(0.01f)
            .Field("Is Trigger", &CapsuleColliderComponent::IsTrigger);
        Register<SphereColliderComponent>("Sphere Collider", "Physics")
            .Field("Offset", &SphereColliderComponent::Offset)
            .Field("Radius", &SphereColliderComponent::Radius).Range(0.001f, 1000.0f).Speed(0.01f)
            .Field("Friction", &SphereColliderComponent::Friction).Range(0.0f, 1.0f).Speed(0.01f)
            .Field("Bounciness", &SphereColliderComponent::Restitution).Range(0.0f, 1.0f).Speed(0.01f)
            .Field("Is Trigger", &SphereColliderComponent::IsTrigger);
        Register<JointComponent>("Joint", "Physics")
            .Field("Type", &JointComponent::Type).Enum({ "Fixed", "Hinge", "Point", "Distance" })
            .Field("Connected Body", &JointComponent::ConnectedBody).Tooltip("Empty = attached to the world")
            .Field("Anchor", &JointComponent::Anchor).Speed(0.01f)
            .Field("Axis", &JointComponent::Axis).Speed(0.01f).Tooltip("Hinge axis (local space)")
            .Field("Connected Anchor", &JointComponent::ConnectedAnchor).Speed(0.01f).Tooltip("Distance joint: local point on the connected body, or a world point")
            .Field("Use Limits", &JointComponent::UseLimits)
            .Field("Min Angle", &JointComponent::MinAngle).Range(-180.0f, 0.0f).Speed(0.5f)
            .Field("Max Angle", &JointComponent::MaxAngle).Range(0.0f, 180.0f).Speed(0.5f)
            .Field("Min Distance", &JointComponent::MinDistance).Range(0.0f, 10000.0f).Speed(0.01f)
            .Field("Max Distance", &JointComponent::MaxDistance).Range(0.0f, 10000.0f).Speed(0.01f)
            .Field("Enable Collision", &JointComponent::EnableCollision);
        Register<MeshColliderComponent>("Mesh Collider", "Physics")
            .Field("Mesh", &MeshColliderComponent::Mesh).As(FieldType::Mesh).Tooltip("None = the Mesh Renderer's mesh")
            .Field("Convex", &MeshColliderComponent::Convex).Tooltip("Use the convex hull (always on for dynamic bodies)")
            .Field("Friction", &MeshColliderComponent::Friction).Range(0.0f, 1.0f).Speed(0.01f)
            .Field("Bounciness", &MeshColliderComponent::Restitution).Range(0.0f, 1.0f).Speed(0.01f)
            .Field("Is Trigger", &MeshColliderComponent::IsTrigger);
        Register<CharacterControllerComponent>("Character Controller", "Physics")
            .Field("Height", &CharacterControllerComponent::Height).Range(0.1f, 100.0f).Speed(0.01f)
            .Field("Radius", &CharacterControllerComponent::Radius).Range(0.01f, 50.0f).Speed(0.01f)
            .Field("Center", &CharacterControllerComponent::Center).Speed(0.01f)
            .Field("Slope Limit", &CharacterControllerComponent::SlopeLimit).Range(0.0f, 89.0f).Speed(0.5f)
            .Field("Step Offset", &CharacterControllerComponent::StepOffset).Range(0.0f, 10.0f).Speed(0.01f)
            .Field("Skin Width", &CharacterControllerComponent::SkinWidth).Range(0.001f, 1.0f).Speed(0.001f)
            .Field("Mass", &CharacterControllerComponent::Mass).Range(0.1f, 10000.0f).Speed(0.5f)
            .Field("Push Strength", &CharacterControllerComponent::PushStrength).Range(0.0f, 100000.0f).Speed(1.0f);
        Register<Joint2DComponent>("Joint 2D", "Physics 2D")
            .Field("Type", &Joint2DComponent::Type).Enum({ "Fixed", "Hinge", "Distance" })
            .Field("Connected Body", &Joint2DComponent::ConnectedBody).Tooltip("Empty = attached to the world")
            .Field("Anchor", &Joint2DComponent::Anchor).Speed(0.01f)
            .Field("Connected Anchor", &Joint2DComponent::ConnectedAnchor).Speed(0.01f)
            .Field("Use Limits", &Joint2DComponent::UseLimits)
            .Field("Lower Angle", &Joint2DComponent::LowerAngle).Range(-180.0f, 180.0f).Speed(0.5f)
            .Field("Upper Angle", &Joint2DComponent::UpperAngle).Range(-180.0f, 180.0f).Speed(0.5f)
            .Field("Distance", &Joint2DComponent::Distance).Range(0.0f, 10000.0f).Speed(0.01f)
            .Field("Collide Connected", &Joint2DComponent::CollideConnected);
        BuiltinCount() = Infos().size();
    }
}
