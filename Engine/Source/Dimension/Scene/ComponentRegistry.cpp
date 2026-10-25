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

    std::vector<const ComponentInfo*> ComponentRegistry::GetDisplayOrder(Scene& scene, Entity entity)
    {
        std::vector<const ComponentInfo*> list;
        if (!scene.IsValid(entity)) return list;
        const auto* tag = scene.GetRegistry().TryGet<TagComponent>(entity);
        std::vector<std::pair<size_t, const ComponentInfo*>> keyed;
        size_t index = 0;
        for (auto& info : Infos())
        {
            ++index;
            if (info.Hidden || !info.Has(scene, entity)) continue;
            size_t key = 1'000'000 + index; // unlisted: after listed ones, in registration order
            if (!info.Removable) key = index;
            else if (tag)
                if (auto it = std::find(tag->ComponentOrder.begin(), tag->ComponentOrder.end(), info.Name); it != tag->ComponentOrder.end())
                    key = 1000 + (size_t)(it - tag->ComponentOrder.begin());
            keyed.emplace_back(key, &info);
        }
        std::stable_sort(keyed.begin(), keyed.end(), [](auto& a, auto& b) { return a.first < b.first; });
        for (auto& [k, info] : keyed) list.push_back(info);
        return list;
    }

    bool ComponentRegistry::MoveComponent(Scene& scene, Entity entity, const std::string& name, int delta)
    {
        auto* tag = scene.IsValid(entity) ? scene.GetRegistry().TryGet<TagComponent>(entity) : nullptr;
        if (!tag || delta == 0) return false;
        auto list = GetDisplayOrder(scene, entity);
        auto it = std::find_if(list.begin(), list.end(), [&](const ComponentInfo* i) { return i->Name == name; });
        if (it == list.end() || !(*it)->Removable) return false;
        int from = (int)(it - list.begin());
        int to = std::clamp(from + delta, 0, (int)list.size() - 1);
        while (to < from && !list[to]->Removable) ++to; // never above Transform
        if (to == from) return false;
        const ComponentInfo* moving = list[from];
        list.erase(list.begin() + from);
        list.insert(list.begin() + to, moving);
        tag->ComponentOrder.clear();
        for (auto* i : list)
            if (i->Removable) tag->ComponentOrder.push_back(i->Name);
        return true;
    }

    void ComponentRegistry::RegisterBuiltins()
    {
        if (!Infos().empty()) return;

        // Core. Tag and Transform are drawn specially by the inspector; ID/Hierarchy are internal.
        Register<TagComponent>("Tag", "Core", true, false)
            .Field("Name", &TagComponent::Name)
            .Field("Tag", &TagComponent::Tag)
            .Field("Active", &TagComponent::Active)
            .Field("Layer", &TagComponent::Layer).Range(0, 31)
            .Field("Component Order", &TagComponent::ComponentOrder).HideInInspector();
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
            .Field("Materials", &MeshRendererComponent::Materials)
                .Tooltip("Per-slot overrides: slot N replaces the model's material N (or sub-mesh N). Empty slots use Material or the model's own.")
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
            .Field("Playing", &AnimatorComponent::Playing)
            .Field("Apply Root Motion", &AnimatorComponent::ApplyRootMotion)
                .Tooltip("Moves the object by the animation's root movement (walk cycles that travel). With a Character Controller it moves the character.")
            .Field("Root Motion Joint", &AnimatorComponent::RootMotionJoint).Tooltip("Empty = the first root joint (e.g. Hips)");

        Register<IKTargetComponent>("IK Target", "Animation")
            .Field("Animator", &IKTargetComponent::Animator).Tooltip("Object with the Animator. None = the nearest parent with one.")
            .Field("Mode", &IKTargetComponent::Mode).Enum({ "Reach", "Look At" })
                .Tooltip("Reach: pull the tip joint to this object (hands, feet). Look At: turn the tip joint to aim at it (heads, eyes, turrets).")
            .Field("Tip", &IKTargetComponent::Tip).Tooltip("Joint pulled to (or aimed at) this object's position")
            .Field("Chain Length", &IKTargetComponent::ChainLength).Range(1, 32)
                .Tooltip("Reach: joints above the tip that turn (2 solves exactly; other lengths iterate). Look At: joints sharing the turn, the tip included.")
            .Field("Hint", &IKTargetComponent::Hint).Tooltip("Reach: optional object a two-joint chain bends toward (elbow or knee direction)")
            .Field("Aim Axis", &IKTargetComponent::AimAxis).Speed(0.01f).Tooltip("Look At: the tip joint's own axis that points at the target (+Z = 0,0,1)")
            .Field("Max Angle", &IKTargetComponent::MaxAngle).Range(0.0f, 180.0f).Speed(0.5f).Tooltip("Look At: largest turn away from the animation (degrees)")
            .Field("Weight", &IKTargetComponent::Weight).Range(0.0f, 1.0f).Speed(0.01f)
            .Field("Layer", &IKTargetComponent::Layer).Range(-1, 32)
                .Tooltip("Applied after this Animator layer (0 = base layer); its weight scales with that layer's. -1 = after all layers.");

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
            .Field("Velocity Over Lifetime", &PS::VelocityOverLifetime).Tooltip("Adds a velocity (not accumulated) and an orbit")
            .Field("Lifetime Velocity", &PS::LifetimeVelocity).Speed(0.05f).Tooltip("Units per second, scaled by Velocity Curve")
            .Field("Velocity Space", &PS::VelocitySpace).Enum({ "Local", "World" })
            .Field("Velocity Curve", &PS::VelocityCurve).Tooltip("Multiplies Lifetime Velocity over each particle's life")
            .Field("Orbital Speed", &PS::OrbitalSpeed).Speed(1.0f).Tooltip("Degrees per second around the emitter's Y axis")
            .Field("Force Over Lifetime", &PS::ForceOverLifetime).Tooltip("Accelerates particles (wind, thrust)")
            .Field("Force", &PS::Force).Speed(0.05f).Tooltip("Units per second squared, scaled by Force Curve")
            .Field("Force Space", &PS::ForceSpace).Enum({ "Local", "World" })
            .Field("Force Curve", &PS::ForceCurve).Tooltip("Multiplies Force over each particle's life")
            .Field("Collision", &PS::Collision).Enum({ "None", "Plane", "World" }).Tooltip("World collides with 3D physics colliders (play mode)")
            .Field("Collision Plane Height", &PS::CollisionPlaneHeight).Speed(0.01f)
            .Field("Bounce", &PS::Bounce).Range(0.0f, 2.0f).Speed(0.01f)
            .Field("Collision Dampen", &PS::CollisionDampen).Range(0.0f, 1.0f).Speed(0.01f)
            .Field("Lifetime Loss", &PS::LifetimeLoss).Range(0.0f, 1.0f).Speed(0.01f).Tooltip("Fraction of the lifetime lost per collision")
            .Field("Collision Radius", &PS::CollisionRadius).Range(0.0f, 10.0f).Speed(0.005f)
            .Field("Sub Emitter", &PS::SubEmitter).Tooltip("Another Particle System that bursts at this system's particles")
            .Field("Sub Emitter Trigger", &PS::SubEmitterTrigger).Enum({ "Birth", "Collision", "Death" })
            .Field("Sub Emit Count", &PS::SubEmitCount).Range(0, 10000)
            .Field("Texture Sheet", &PS::TextureSheet).Tooltip("Play the texture as a flipbook (a grid of frames) over each particle's life")
            .Field("Sheet Columns", &PS::SheetColumns).Range(1, 64)
            .Field("Sheet Rows", &PS::SheetRows).Range(1, 64)
            .Field("Sheet Frames", &PS::SheetFrames).Range(0, 4096).Tooltip("0 = every cell")
            .Field("Sheet Cycles", &PS::SheetCycles).Range(0.0f, 100.0f).Speed(0.05f).Tooltip("Times the animation plays over a lifetime")
            .Field("Sheet Random Start", &PS::SheetRandomStart)
            .Field("Noise", &PS::Noise).Tooltip("Swirling curl-noise drift")
            .Field("Noise Strength", &PS::NoiseStrength).Range(0.0f, 1000.0f).Speed(0.02f).Tooltip("Units per second")
            .Field("Noise Frequency", &PS::NoiseFrequency).Range(0.001f, 100.0f).Speed(0.01f).Tooltip("Noise features per unit")
            .Field("Noise Scroll Speed", &PS::NoiseScrollSpeed).Range(0.0f, 100.0f).Speed(0.01f)
            .Field("Trails", &PS::Trails)
            .Field("Trail Lifetime", &PS::TrailLifetime).Range(0.01f, 10.0f).Speed(0.01f)
            .Field("Trail Width", &PS::TrailWidth).Range(0.0f, 10.0f).Speed(0.01f).Tooltip("Multiplier of the particle size")
            .Field("Texture", &PS::Texture).As(FieldType::Texture)
            .Field("Blend", &PS::Blend).Enum({ "Alpha", "Additive" })
            .Field("Sorting Order", &PS::SortingOrder);

        Register<LineRendererComponent>("Line Renderer", "Effects")
            .Field("Positions", &LineRendererComponent::Positions)
            .Field("Use World Space", &LineRendererComponent::UseWorldSpace).Tooltip("Off: positions are relative to this object")
            .Field("Loop", &LineRendererComponent::Loop).Tooltip("Connect the last point back to the first")
            .Field("Width", &LineRendererComponent::Width).Range(0.0f, 1000.0f).Speed(0.005f)
            .Field("Width Curve", &LineRendererComponent::WidthCurve).Tooltip("Width multiplier from the start (0) to the end (1)")
            .Field("Color", &LineRendererComponent::ColorGradient).Tooltip("Color from the start (0) to the end (1)")
            .Field("Alignment", &LineRendererComponent::Alignment).Enum({ "View", "Transform Z" })
                .Tooltip("View: faces the camera. Transform Z: lies flat, facing this object's Z axis")
            .Field("Texture", &LineRendererComponent::Texture).As(FieldType::Texture)
            .Field("Texture Mode", &LineRendererComponent::TextureMode).Enum({ "Stretch", "Tile" }).Tooltip("Tile repeats the texture every world unit")
            .Field("Corner Vertices", &LineRendererComponent::CornerVertices).Range(0, 32).Tooltip("Rounded joins (0 = mitered corners)")
            .Field("End Cap Vertices", &LineRendererComponent::EndCapVertices).Range(0, 32).Tooltip("Rounded ends (0 = square)")
            .Field("Blend", &LineRendererComponent::Blend).Enum({ "Alpha", "Additive" })
            .Field("Sorting Order", &LineRendererComponent::SortingOrder);

        Register<TrailRendererComponent>("Trail Renderer", "Effects")
            .Field("Time", &TrailRendererComponent::Time).Range(0.01f, 60.0f).Speed(0.01f).Tooltip("Seconds each trail point lives")
            .Field("Min Vertex Distance", &TrailRendererComponent::MinVertexDistance).Range(0.001f, 100.0f).Speed(0.005f)
            .Field("Width", &TrailRendererComponent::Width).Range(0.0f, 1000.0f).Speed(0.005f)
            .Field("Width Curve", &TrailRendererComponent::WidthCurve).Tooltip("Width multiplier from the head (0) to the tail (1)")
            .Field("Color", &TrailRendererComponent::ColorGradient).Tooltip("Color from the head (0) to the tail (1)")
            .Field("Emitting", &TrailRendererComponent::Emitting)
            .Field("Texture", &TrailRendererComponent::Texture).As(FieldType::Texture)
            .Field("Texture Mode", &TrailRendererComponent::TextureMode).Enum({ "Stretch", "Tile" })
            .Field("Corner Vertices", &TrailRendererComponent::CornerVertices).Range(0, 32).Tooltip("Rounded joins (0 = mitered corners)")
            .Field("End Cap Vertices", &TrailRendererComponent::EndCapVertices).Range(0, 32).Tooltip("Rounded ends (0 = square)")
            .Field("Blend", &TrailRendererComponent::Blend).Enum({ "Alpha", "Additive" })
            .Field("Sorting Order", &TrailRendererComponent::SortingOrder);

        Register<TextRendererComponent>("Text (3D)", "Rendering")
            .Field("Text", &TextRendererComponent::Text)
            .Field("Font", &TextRendererComponent::Font).As(FieldType::Font)
            .Field("Color", &TextRendererComponent::Color).As(FieldType::Color4)
            .Field("Font Size", &TextRendererComponent::FontSize).Range(0.001f, 1000.0f).Speed(0.01f)
            .Field("Alignment", &TextRendererComponent::Align).Enum({ "Left", "Center", "Right" })
            .Field("Sorting Order", &TextRendererComponent::SortingOrder)
            .Field("Rich Text", &TextRendererComponent::RichText).Tooltip("Style with tags: <b>, <i>, <u>, <s>, <color=#ff8000>, <size=0.8>, <br>");

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
            .Field("Wrap", &UITextComponent::Wrap)
            .Field("Rich Text", &UITextComponent::RichText)
                .Tooltip("Style with tags: <b>bold</b>, <i>italic</i>, <u>underline</u>, <s>strike</s>, <color=#ff8000>color</color> (or a name), <size=40>size</size> or <size=150%>, and <br> for a line break");
        Register<ReflectionProbeComponent>("Reflection Probe", "Rendering")
            .Field("Size", &ReflectionProbeComponent::Size).Speed(0.05f).Tooltip("Influence box: surfaces inside reflect this probe")
            .Field("Resolution", &ReflectionProbeComponent::Resolution).Enum({ "64", "128", "256", "512" })
            .Field("Intensity", &ReflectionProbeComponent::Intensity).Range(0.0f, 10.0f).Speed(0.01f)
            .Field("Box Projection", &ReflectionProbeComponent::BoxProjection).Tooltip("Reflections line up with the box's walls")
            .Field("Blend Distance", &ReflectionProbeComponent::BlendDistance).Range(0.0f, 100.0f).Speed(0.05f)
                .Tooltip("The probe fades out over this distance inside its box, blending into overlapping probes or the sky")
            .Field("Realtime", &ReflectionProbeComponent::Realtime).Tooltip("Re-capture every frame (costly: six extra renders)");

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
            .Field("Multi Line", &UIInputFieldComponent::MultiLine).Tooltip("Enter starts a new line; Ctrl+Enter submits")
            .Field("Word Wrap", &UIInputFieldComponent::WordWrap).Tooltip("Multi-line: long lines wrap at the field's width instead of scrolling sideways")
            .Field("Font Size", &UIInputFieldComponent::FontSize).Range(1.0f, 500.0f).Speed(0.5f)
            .Field("Interactable", &UIInputFieldComponent::Interactable)
            .Field("Background Color", &UIInputFieldComponent::BackgroundColor).As(FieldType::Color4)
            .Field("Text Color", &UIInputFieldComponent::TextColor).As(FieldType::Color4)
            .Field("Placeholder Color", &UIInputFieldComponent::PlaceholderColor).As(FieldType::Color4);
        Register<UILayoutGroupComponent>("UI Layout Group", "UI")
            .Field("Direction", &UILayoutGroupComponent::Direction).Enum({ "Vertical", "Horizontal", "Grid" })
            .Field("Spacing", &UILayoutGroupComponent::Spacing).Speed(0.5f)
            .Field("Padding", &UILayoutGroupComponent::Padding).Speed(0.5f).Tooltip("left, top, right, bottom")
            .Field("Stretch Children", &UILayoutGroupComponent::StretchChildren).Tooltip("Rows and columns: children fill the cross axis")
            .Field("Cell Size", &UILayoutGroupComponent::CellSize).Speed(0.5f).Tooltip("Grid: size of every cell")
            .Field("Columns", &UILayoutGroupComponent::Columns).Range(0, 1000).Tooltip("Grid: fixed column count (0 = as many as fit)");
        Register<UIDropdownComponent>("UI Dropdown", "UI")
            .Field("Options", &UIDropdownComponent::Options)
            .Field("Value", &UIDropdownComponent::Value).Range(0, 10000).Tooltip("Index of the selected option")
            .Field("Font Size", &UIDropdownComponent::FontSize).Range(1.0f, 500.0f).Speed(0.5f)
            .Field("Max Visible Items", &UIDropdownComponent::MaxVisibleItems).Range(1, 100).Tooltip("The open list scrolls beyond this")
            .Field("Interactable", &UIDropdownComponent::Interactable)
            .Field("Background Color", &UIDropdownComponent::BackgroundColor).As(FieldType::Color4)
            .Field("Text Color", &UIDropdownComponent::TextColor).As(FieldType::Color4)
            .Field("List Color", &UIDropdownComponent::ListColor).As(FieldType::Color4)
            .Field("Highlight Color", &UIDropdownComponent::HighlightColor).As(FieldType::Color4);

        Register<UIScrollViewComponent>("UI Scroll View", "UI")
            .Field("Vertical", &UIScrollViewComponent::Vertical)
            .Field("Horizontal", &UIScrollViewComponent::Horizontal).Tooltip("Shift + wheel scrolls sideways (the wheel alone when Vertical is off)")
            .Field("Scroll Speed", &UIScrollViewComponent::ScrollSpeed).Range(1.0f, 10000.0f).Speed(1.0f)
            .Field("Scrollbar", &UIScrollViewComponent::Scrollbar).Tooltip("Shown while the content is larger than the view")
            .Field("Scrollbar Width", &UIScrollViewComponent::ScrollbarWidth).Range(1.0f, 200.0f).Speed(0.25f)
            .Field("Scrollbar Track Color", &UIScrollViewComponent::ScrollbarTrackColor).As(FieldType::Color4)
            .Field("Scrollbar Handle Color", &UIScrollViewComponent::ScrollbarHandleColor).As(FieldType::Color4);
        Register<UIButtonComponent>("UI Button", "UI")
            .Field("Normal Color", &UIButtonComponent::NormalColor).As(FieldType::Color4)
            .Field("Hover Color", &UIButtonComponent::HoverColor).As(FieldType::Color4)
            .Field("Pressed Color", &UIButtonComponent::PressedColor).As(FieldType::Color4)
            .Field("Interactable", &UIButtonComponent::Interactable);

        Register<UINavigationComponent>("UI Navigation", "UI")
            .Field("Mode", &UINavigationComponent::Mode).Enum({ "Automatic", "Explicit", "None" })
                .Tooltip("Automatic: the nearest widget in the pressed direction, unless a link below is set. Explicit: only the links. "
                         "None: arrows and the D-pad never select this widget or leave it")
            .Field("Select On Up", &UINavigationComponent::SelectOnUp)
            .Field("Select On Down", &UINavigationComponent::SelectOnDown)
            .Field("Select On Left", &UINavigationComponent::SelectOnLeft)
            .Field("Select On Right", &UINavigationComponent::SelectOnRight);

        Register<DirectionalLightComponent>("Directional Light", "Rendering")
            .Field("Color", &DirectionalLightComponent::Color).As(FieldType::Color3)
            .Field("Intensity", &DirectionalLightComponent::Intensity).Range(0.0f, 100.0f).Speed(0.01f)
            .Field("Cast Shadows", &DirectionalLightComponent::CastShadows)
            .Field("Shadow Distance", &DirectionalLightComponent::ShadowDistance).Range(1.0f, 1000.0f).Speed(0.5f)
            .Field("Shadow Bias", &DirectionalLightComponent::ShadowBias).Range(0.0f, 0.05f).Speed(0.0001f)
            .Field("Shadow Cascades", &DirectionalLightComponent::ShadowCascades).Range(1, 4)
            .Field("Debug Cascades", &DirectionalLightComponent::DebugCascades)
            .Field("Soft Shadows", &DirectionalLightComponent::SoftShadows)
            .Field("Light Angle", &DirectionalLightComponent::LightAngle).Range(0.01f, 20.0f).Speed(0.05f);

        Register<PointLightComponent>("Point Light", "Rendering")
            .Field("Color", &PointLightComponent::Color).As(FieldType::Color3)
            .Field("Intensity", &PointLightComponent::Intensity).Range(0.0f, 1000.0f).Speed(0.05f)
            .Field("Range", &PointLightComponent::Range).Range(0.01f, 1000.0f).Speed(0.1f)
            .Field("Cast Shadows", &PointLightComponent::CastShadows)
            .Field("Shadow Bias", &PointLightComponent::ShadowBias).Range(0.0f, 1.0f).Speed(0.001f)
            .Field("Cookie", &PointLightComponent::Cookie).As(FieldType::Texture)
                .Tooltip("Equirectangular (2:1) texture wrapped around the light, like a pierced lantern: its color multiplies the light. "
                         "The center of the image faces the light's forward axis; it turns with the light.");

        Register<SpotLightComponent>("Spot Light", "Rendering")
            .Field("Color", &SpotLightComponent::Color).As(FieldType::Color3)
            .Field("Intensity", &SpotLightComponent::Intensity).Range(0.0f, 1000.0f).Speed(0.05f)
            .Field("Range", &SpotLightComponent::Range).Range(0.01f, 1000.0f).Speed(0.1f)
            .Field("Spot Angle", &SpotLightComponent::SpotAngle).Range(1.0f, 179.0f).Speed(0.5f)
            .Field("Inner Spot Angle", &SpotLightComponent::InnerSpotAngle).Range(0.0f, 179.0f).Speed(0.5f)
            .Field("Cast Shadows", &SpotLightComponent::CastShadows)
            .Field("Shadow Bias", &SpotLightComponent::ShadowBias).Range(0.0f, 1.0f).Speed(0.001f)
            .Field("Cookie", &SpotLightComponent::Cookie).As(FieldType::Texture)
                .Tooltip("Texture projected through the cone, like a gobo: its color multiplies the light. Rolls with the light.");

        Register<ClothComponent>("Cloth", "Physics")
            .Field("Size", &ClothComponent::Size).Speed(0.01f).Tooltip("Width and height in the object's XY plane (top edge at +Y)")
            .Field("Resolution", &ClothComponent::Resolution).Range(1, 128).Tooltip("Quads per side")
            .Field("Pin", &ClothComponent::Pin).Enum({ "Top Edge", "Top Corners", "None" }).Tooltip("Vertices fixed to the object (they follow it when it moves)")
            .Field("Mass", &ClothComponent::Mass).Range(0.001f, 1000.0f).Speed(0.01f)
            .Field("Stretch Compliance", &ClothComponent::StretchCompliance).Range(0.0f, 1.0f).Speed(0.00001f).Tooltip("0 = does not stretch; larger = stretchier")
            .Field("Bend Compliance", &ClothComponent::BendCompliance).Range(0.0f, 100.0f).Speed(0.001f).Tooltip("Larger = folds more easily")
            .Field("Damping", &ClothComponent::Damping).Range(0.0f, 10.0f).Speed(0.01f)
            .Field("Friction", &ClothComponent::Friction).Range(0.0f, 1.0f).Speed(0.01f)
            .Field("Iterations", &ClothComponent::Iterations).Range(1, 64)
            .Field("Thickness", &ClothComponent::Thickness).Range(0.0f, 1.0f).Speed(0.001f).Tooltip("How far it stays from what it touches")
            .Field("Wind", &ClothComponent::Wind).Speed(0.05f).Tooltip("World-space acceleration (m/s^2) on the free vertices");
        Register<SoftBodyComponent>("Soft Body", "Physics")
            .Field("Mass", &SoftBodyComponent::Mass).Range(0.001f, 10000.0f).Speed(0.01f)
            .Field("Pressure", &SoftBodyComponent::Pressure).Range(0.0f, 100000.0f).Speed(0.5f).Tooltip("Inflates a closed mesh like a balloon (0 = none)")
            .Field("Stretch Compliance", &SoftBodyComponent::StretchCompliance).Range(0.0f, 1.0f).Speed(0.00001f).Tooltip("0 = edges do not stretch; larger = stretchier")
            .Field("Bend Compliance", &SoftBodyComponent::BendCompliance).Range(0.0f, 100.0f).Speed(0.001f).Tooltip("Larger = loses its shape more easily (jelly)")
            .Field("Damping", &SoftBodyComponent::Damping).Range(0.0f, 10.0f).Speed(0.01f)
            .Field("Friction", &SoftBodyComponent::Friction).Range(0.0f, 1.0f).Speed(0.01f)
            .Field("Bounciness", &SoftBodyComponent::Restitution).Range(0.0f, 1.0f).Speed(0.01f)
            .Field("Iterations", &SoftBodyComponent::Iterations).Range(1, 64)
            .Field("Thickness", &SoftBodyComponent::Thickness).Range(0.0f, 1.0f).Speed(0.001f);
        Register<AudioOccluderComponent>("Audio Occluder", "Audio")
            .Field("Occlusion", &AudioOccluderComponent::Occlusion).Range(0.0f, 1.0f).Speed(0.01f)
                .Tooltip("How much this collider blocks occluding Audio Sources (0 = not at all, 1 = fully; colliders without this component block fully). Several along the line add up.");
        Register<VehicleComponent>("Vehicle", "Physics")
            .Field("Type", &VehicleComponent::Type).Enum({ "Car", "Motorcycle", "Tank" })
                .Tooltip("Car: four wheels. Motorcycle: two wheels that lean into turns (visuals in the Front/Rear Left slots). Tank: tracks steered by side speed, turning on the spot.")
            .Field("Track Wheels", &VehicleComponent::TrackWheels).Range(2, 16).Tooltip("Tanks: road wheels per track, along the wheel base")
            .Field("Center Of Mass Offset", &VehicleComponent::CenterOfMassOffset).Speed(0.01f).Tooltip("Moves the body's center of mass (lower keeps the vehicle on its wheels)")
            .Field("Wheel Base", &VehicleComponent::WheelBase).Range(0.2f, 20.0f).Speed(0.01f).Tooltip("Front to rear axle (m). The car faces -Z.")
            .Field("Track Width", &VehicleComponent::TrackWidth).Range(0.2f, 10.0f).Speed(0.01f).Tooltip("Left to right wheel (m)")
            .Field("Wheel Height", &VehicleComponent::WheelHeight).Speed(0.01f).Tooltip("Height of the suspension mounts in the body's space")
            .Field("Wheel Radius", &VehicleComponent::WheelRadius).Range(0.05f, 5.0f).Speed(0.01f)
            .Field("Wheel Width", &VehicleComponent::WheelWidth).Range(0.02f, 2.0f).Speed(0.01f)
            .Field("Suspension Travel", &VehicleComponent::SuspensionTravel).Range(0.01f, 2.0f).Speed(0.01f)
            .Field("Suspension Frequency", &VehicleComponent::SuspensionFrequency).Range(0.1f, 20.0f).Speed(0.01f).Tooltip("Spring stiffness (Hz)")
            .Field("Suspension Damping", &VehicleComponent::SuspensionDamping).Range(0.0f, 2.0f).Speed(0.01f)
            .Field("Max Steer Angle", &VehicleComponent::MaxSteerAngle).Range(0.0f, 80.0f).Speed(0.1f)
            .Field("Drive", &VehicleComponent::Drive).Enum({ "Rear Wheel", "Front Wheel", "All Wheel" }).Tooltip("Cars only")
            .Field("Engine Torque", &VehicleComponent::EngineTorque).Range(0.0f, 100000.0f).Speed(1.0f).Tooltip("Newton metres")
            .Field("Max RPM", &VehicleComponent::MaxRPM).Range(500.0f, 30000.0f).Speed(10.0f)
            .Field("Brake Torque", &VehicleComponent::BrakeTorque).Range(0.0f, 100000.0f).Speed(1.0f)
            .Field("Hand Brake Torque", &VehicleComponent::HandBrakeTorque).Range(0.0f, 100000.0f).Speed(1.0f)
            .Field("Wheel Front Left", &VehicleComponent::WheelFrontLeft).Tooltip("Optional visual (e.g. a Cylinder) moved and spun with the wheel")
            .Field("Wheel Front Right", &VehicleComponent::WheelFrontRight)
            .Field("Wheel Rear Left", &VehicleComponent::WheelRearLeft)
            .Field("Wheel Rear Right", &VehicleComponent::WheelRearRight)
            .Field("Throttle", &VehicleComponent::Throttle).Range(-1.0f, 1.0f).Speed(0.01f).Tooltip("Set by scripts: -1..1, negative reverses")
            .Field("Steering", &VehicleComponent::Steering).Range(-1.0f, 1.0f).Speed(0.01f).Tooltip("Set by scripts: -1..1, positive steers right")
            .Field("Brake", &VehicleComponent::Brake).Range(0.0f, 1.0f).Speed(0.01f)
            .Field("Hand Brake", &VehicleComponent::HandBrake).Range(0.0f, 1.0f).Speed(0.01f);

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
            .Field("Mute", &AudioSourceComponent::Mute)
            .Field("Occlusion", &AudioSourceComponent::Occlusion)
                .Tooltip("Spatial sources: 3D colliders between the listener and the source muffle and quiet the sound (triggers and the source's own colliders don't; nor does the Ignore Raycast layer)")
            .Field("Occlusion Volume", &AudioSourceComponent::OcclusionVolume).Range(-60.0f, 0.0f).Speed(0.1f).Tooltip("dB when fully occluded")
            .Field("Occlusion Cutoff", &AudioSourceComponent::OcclusionCutoff).Range(100.0f, 20000.0f).Speed(10.0f).Tooltip("Low-pass cutoff (Hz) when fully occluded");

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
            .Field("Type", &JointComponent::Type).Enum({ "Fixed", "Hinge", "Point", "Distance", "Slider" })
            .Field("Connected Body", &JointComponent::ConnectedBody).Tooltip("Empty = attached to the world")
            .Field("Anchor", &JointComponent::Anchor).Speed(0.01f)
            .Field("Axis", &JointComponent::Axis).Speed(0.01f).Tooltip("Hinge or slider axis (local space)")
            .Field("Connected Anchor", &JointComponent::ConnectedAnchor).Speed(0.01f).Tooltip("Distance joint: local point on the connected body, or a world point")
            .Field("Use Limits", &JointComponent::UseLimits)
            .Field("Min Angle", &JointComponent::MinAngle).Range(-180.0f, 0.0f).Speed(0.5f)
            .Field("Max Angle", &JointComponent::MaxAngle).Range(0.0f, 180.0f).Speed(0.5f)
            .Field("Min Distance", &JointComponent::MinDistance).Range(0.0f, 10000.0f).Speed(0.01f)
            .Field("Max Distance", &JointComponent::MaxDistance).Range(0.0f, 10000.0f).Speed(0.01f)
            .Field("Slider Min", &JointComponent::SliderMin).Range(-10000.0f, 0.0f).Speed(0.01f).Tooltip("Slider: lowest travel along Axis (units)")
            .Field("Slider Max", &JointComponent::SliderMax).Range(0.0f, 10000.0f).Speed(0.01f).Tooltip("Slider: highest travel along Axis (units)")
            .Field("Spring Frequency", &JointComponent::SpringFrequency).Range(0.0f, 100.0f).Speed(0.05f).Tooltip("Distance: > 0 makes the joint a spring toward its distance (Hz); 0 = rigid")
            .Field("Spring Damping", &JointComponent::SpringDamping).Range(0.0f, 10.0f).Speed(0.01f).Tooltip("Distance spring damping ratio (1 = no overshoot)")
            .Field("Enable Collision", &JointComponent::EnableCollision)
            .Field("Use Motor", &JointComponent::UseMotor).Tooltip("Hinge or slider: drive toward Motor Speed. Distance: a winch that changes the joint's length")
            .Field("Motor Speed", &JointComponent::MotorSpeed).Speed(1.0f)
                .Tooltip("Hinge: degrees per second. Slider: units per second along Axis. Distance: units per second of lengthening (negative reels in)")
            .Field("Motor Max Torque", &JointComponent::MotorMaxTorque).Range(0.0f, 1e9f).Speed(10.0f)
                .Tooltip("Hinge: N*m. Slider: maximum force (N). Distance: reeling in stalls while the joint pulls harder than this (N)")
            .Field("Break Force", &JointComponent::BreakForce).Range(0.0f, 1e9f).Speed(10.0f).Tooltip("Newtons; 0 = unbreakable")
            .Field("Break Torque", &JointComponent::BreakTorque).Range(0.0f, 1e9f).Speed(10.0f).Tooltip("Newton-meters; 0 = unbreakable");
        Register<MeshColliderComponent>("Mesh Collider", "Physics")
            .Field("Mesh", &MeshColliderComponent::Mesh).As(FieldType::Mesh).Tooltip("None = the Mesh Renderer's mesh")
            .Field("Convex", &MeshColliderComponent::Convex).Tooltip("Use the convex hull (always on for dynamic bodies)")
            .Field("Convex Decomposition", &MeshColliderComponent::ConvexDecomposition)
                .Tooltip("Convex and dynamic: several convex parts that follow the mesh's shape (holes, hollows) instead of one hull. Computed once per mesh when play starts; the parts show while selected.")
            .Field("Max Parts", &MeshColliderComponent::MaxParts).Range(1, 256)
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
            .Field("Push Strength", &CharacterControllerComponent::PushStrength).Range(0.0f, 100000.0f).Speed(1.0f)
            .Field("Ride Moving Platforms", &CharacterControllerComponent::RideMovingPlatforms)
                .Tooltip("Standing on a moving (e.g. kinematic) body carries the character with it");
        Register<Joint2DComponent>("Joint 2D", "Physics 2D")
            .Field("Type", &Joint2DComponent::Type).Enum({ "Fixed", "Hinge", "Distance", "Slider" })
            .Field("Connected Body", &Joint2DComponent::ConnectedBody).Tooltip("Empty = attached to the world")
            .Field("Anchor", &Joint2DComponent::Anchor).Speed(0.01f)
            .Field("Connected Anchor", &Joint2DComponent::ConnectedAnchor).Speed(0.01f)
            .Field("Use Limits", &Joint2DComponent::UseLimits)
            .Field("Lower Angle", &Joint2DComponent::LowerAngle).Range(-180.0f, 180.0f).Speed(0.5f)
            .Field("Upper Angle", &Joint2DComponent::UpperAngle).Range(-180.0f, 180.0f).Speed(0.5f)
            .Field("Distance", &Joint2DComponent::Distance).Range(0.0f, 10000.0f).Speed(0.01f)
            .Field("Axis", &Joint2DComponent::Axis).Speed(0.01f).Tooltip("Slider axis (local space)")
            .Field("Lower Translation", &Joint2DComponent::LowerTranslation).Range(-10000.0f, 0.0f).Speed(0.01f).Tooltip("Slider: lowest travel along Axis (units)")
            .Field("Upper Translation", &Joint2DComponent::UpperTranslation).Range(0.0f, 10000.0f).Speed(0.01f).Tooltip("Slider: highest travel along Axis (units)")
            .Field("Spring Frequency", &Joint2DComponent::SpringFrequency).Range(0.0f, 100.0f).Speed(0.05f).Tooltip("Distance: > 0 makes the joint a spring toward its distance (Hz); 0 = rigid")
            .Field("Spring Damping", &Joint2DComponent::SpringDamping).Range(0.0f, 10.0f).Speed(0.01f).Tooltip("Distance spring damping ratio (1 = no overshoot)")
            .Field("Collide Connected", &Joint2DComponent::CollideConnected)
            .Field("Use Motor", &Joint2DComponent::UseMotor).Tooltip("Hinge, slider or distance spring: drive toward Motor Speed")
            .Field("Motor Speed", &Joint2DComponent::MotorSpeed).Speed(1.0f).Tooltip("Hinge: degrees per second. Slider: units per second along Axis. Distance spring: units per second of lengthening")
            .Field("Motor Max Torque", &Joint2DComponent::MotorMaxTorque).Range(0.0f, 1e9f).Speed(1.0f).Tooltip("Hinge: N*m. Slider: maximum force (N)")
            .Field("Break Force", &Joint2DComponent::BreakForce).Range(0.0f, 1e9f).Speed(1.0f).Tooltip("Newtons; 0 = unbreakable")
            .Field("Break Torque", &Joint2DComponent::BreakTorque).Range(0.0f, 1e9f).Speed(1.0f).Tooltip("Newton-meters; 0 = unbreakable");
        BuiltinCount() = Infos().size();
    }
}
