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
            .Field("Active", &TagComponent::Active);
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
            .Field("End Color", &PS::EndColor).As(FieldType::Color4).Tooltip("Multiplied with the start color over the lifetime")
            .Field("End Size", &PS::EndSize).Range(0.0f, 100.0f).Speed(0.01f).Tooltip("Size multiplier at the end of the lifetime")
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
            .Field("Color", &UIImageComponent::Color).As(FieldType::Color4);
        Register<UITextComponent>("UI Text", "UI")
            .Field("Text", &UITextComponent::Text)
            .Field("Font", &UITextComponent::Font).As(FieldType::Font)
            .Field("Font Size", &UITextComponent::FontSize).Range(1.0f, 1000.0f).Speed(0.5f)
            .Field("Color", &UITextComponent::Color).As(FieldType::Color4)
            .Field("Alignment", &UITextComponent::Align).Enum({ "Left", "Center", "Right" })
            .Field("Vertical Alignment", &UITextComponent::VerticalAlign).Enum({ "Top", "Middle", "Bottom" })
            .Field("Wrap", &UITextComponent::Wrap);
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
            .Field("Shadow Bias", &DirectionalLightComponent::ShadowBias).Range(0.0f, 0.05f).Speed(0.0001f);

        Register<PointLightComponent>("Point Light", "Rendering")
            .Field("Color", &PointLightComponent::Color).As(FieldType::Color3)
            .Field("Intensity", &PointLightComponent::Intensity).Range(0.0f, 1000.0f).Speed(0.05f)
            .Field("Range", &PointLightComponent::Range).Range(0.01f, 1000.0f).Speed(0.1f);

        Register<AudioSourceComponent>("Audio Source", "Audio")
            .Field("Clip", &AudioSourceComponent::Clip).As(FieldType::Audio)
            .Field("Volume", &AudioSourceComponent::Volume).Range(0.0f, 1.0f).Speed(0.01f)
            .Field("Pitch", &AudioSourceComponent::Pitch).Range(0.1f, 3.0f).Speed(0.01f)
            .Field("Loop", &AudioSourceComponent::Loop)
            .Field("Play On Awake", &AudioSourceComponent::PlayOnAwake)
            .Field("Spatial", &AudioSourceComponent::Spatial)
            .Field("Min Distance", &AudioSourceComponent::MinDistance).Range(0.01f, 1000.0f)
            .Field("Max Distance", &AudioSourceComponent::MaxDistance).Range(0.01f, 10000.0f);

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
            .Field("Bounciness", &BoxColliderComponent::Restitution).Range(0.0f, 1.0f).Speed(0.01f);
        Register<CapsuleColliderComponent>("Capsule Collider", "Physics")
            .Field("Offset", &CapsuleColliderComponent::Offset)
            .Field("Radius", &CapsuleColliderComponent::Radius).Range(0.001f, 1000.0f).Speed(0.01f)
            .Field("Height", &CapsuleColliderComponent::Height).Range(0.001f, 1000.0f).Speed(0.01f)
            .Field("Friction", &CapsuleColliderComponent::Friction).Range(0.0f, 1.0f).Speed(0.01f)
            .Field("Bounciness", &CapsuleColliderComponent::Restitution).Range(0.0f, 1.0f).Speed(0.01f);
        Register<SphereColliderComponent>("Sphere Collider", "Physics")
            .Field("Offset", &SphereColliderComponent::Offset)
            .Field("Radius", &SphereColliderComponent::Radius).Range(0.001f, 1000.0f).Speed(0.01f)
            .Field("Friction", &SphereColliderComponent::Friction).Range(0.0f, 1.0f).Speed(0.01f)
            .Field("Bounciness", &SphereColliderComponent::Restitution).Range(0.0f, 1.0f).Speed(0.01f);
        BuiltinCount() = Infos().size();
    }
}
