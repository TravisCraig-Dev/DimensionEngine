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

    ComponentInfo* ComponentRegistry::FindMutable(const std::string& name)
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

        Register<TerrainComponent>("Terrain", "Rendering")
            .Field("Size", &TerrainComponent::Size).Speed(0.5f).Tooltip("Meters along X and Z, centered on the object")
            .Field("Height", &TerrainComponent::Height).Range(0.0f, 10000.0f).Speed(0.1f)
            .Field("Resolution", &TerrainComponent::Resolution).Range(4, 1024).Tooltip("Height samples per side")
            .Field("Heightmap", &TerrainComponent::Heightmap).As(FieldType::Texture).Tooltip("Grayscale image (16-bit keeps detail). None = generated")
            .Field("Seed", &TerrainComponent::Seed).Header("Generated")
            .Field("Noise Scale", &TerrainComponent::NoiseScale).Range(0.0001f, 1.0f).Speed(0.0005f).Tooltip("Features per meter of the largest hills")
            .Field("Octaves", &TerrainComponent::Octaves).Range(1, 10)
            .Field("Roughness", &TerrainComponent::Roughness).Range(0.0f, 1.0f).Speed(0.01f)
            .Field("Flat Center", &TerrainComponent::FlatCenter).Range(0.0f, 1.0f).Speed(0.01f).Tooltip("Radius kept flat (fraction of the half size)")
            .Field("Flat Height", &TerrainComponent::FlatHeight).Range(0.0f, 1.0f).Speed(0.01f)
            .List<TerrainLayer>("Layers", &TerrainComponent::Layers, [](FieldListBuilder<TerrainLayer>& layer) {
                layer.Field("Name", &TerrainLayer::Name)
                    .Field("Texture", &TerrainLayer::Texture).As(FieldType::Texture)
                    .Field("Color", &TerrainLayer::Color).As(FieldType::Color4)
                    .Field("Tiling", &TerrainLayer::Tiling).Range(0.001f, 100.0f).Speed(0.01f).Tooltip("Texture repeats per meter")
                    .Field("Normal Map", &TerrainLayer::NormalMap).As(FieldType::Texture).Tooltip("Optional; tiled like the texture")
                    .Field("Normal Strength", &TerrainLayer::NormalStrength).Range(0.0f, 4.0f).Speed(0.01f)
                    .Field("Triplanar", &TerrainLayer::Triplanar).Tooltip("Projected from the sides too, so it is not stretched on cliffs (costs three texture reads)")
                    .Field("Rule", &TerrainLayer::Rule).Tooltip("Lays itself out by height, slope and noise, over the layers before it (the first layer is the ground under all)")
                    .Field("Height Range", &TerrainLayer::HeightRange).Speed(0.005f).Tooltip("Rule: from - to, 0-1 of the terrain's Height")
                    .Field("Slope Range", &TerrainLayer::SlopeRange).Speed(0.5f).Tooltip("Rule: from - to, degrees (0 flat, 90 a wall)")
                    .Field("Height Fade", &TerrainLayer::HeightFade).Range(0.0f, 1.0f).Speed(0.002f).Tooltip("Rule: how far past its height range it fades out (0-1 of Height)")
                    .Field("Slope Fade", &TerrainLayer::SlopeFade).Range(0.0f, 45.0f).Speed(0.1f).Tooltip("Rule: how far past its slope range it fades out (degrees)")
                    .Field("Noise", &TerrainLayer::Noise).Range(0.0f, 1.0f).Speed(0.01f).Tooltip("Rule: breaks it into patches (0 = even)")
                    .Field("Noise Scale", &TerrainLayer::NoiseScale).Range(0.1f, 1000.0f).Speed(0.1f).Tooltip("Rule: the patches' size, meters")
                    .Field("Trees", &TerrainLayer::Trees).Range(0.0f, 1.0f).Speed(0.01f).Tooltip("How thickly the terrain's trees grow on this layer by themselves (0 = only where painted)")
                    .Field("Grass", &TerrainLayer::Grass).Range(0.0f, 1.0f).Speed(0.01f).Tooltip("How thickly the terrain's grass tufts grow on this layer by themselves (0 = only where painted)");
            })
            .ElementTitle("Name").Range(1.0f, 16.0f).Header("Layers")
            .Tooltip("Up to 16; layers with a Rule lay themselves out (later over earlier); with none, the first three do by slope and height")
            .Field("Rock Slope", &TerrainComponent::RockSlope).Range(1.0f, 89.0f).Speed(0.5f).Tooltip("Degrees where the second layer takes over")
            .Field("Snow Height", &TerrainComponent::SnowHeight).Range(0.0f, 1.0f).Speed(0.01f).Tooltip("Fraction of Height where the third layer starts")
            .Field("Height Blend", &TerrainComponent::HeightBlend).Range(0.0f, 1.0f).Speed(0.01f)
                .Tooltip("Where layers meet, the higher (brighter) parts of each texture win; 0 = plain blending")
            .Field("Tree Mesh", &TerrainComponent::TreeMesh).As(FieldType::Mesh).Header("Details")
                .Tooltip("Placed where the Trees brush paints and on layers with Trees (trees, bushes, rocks)")
            .Field("Tree Material", &TerrainComponent::TreeMaterial).As(FieldType::Material).Tooltip("None = the model's own materials")
            .Field("Tree Density", &TerrainComponent::TreeDensity).Range(0.0f, 1000.0f).Speed(0.05f).Tooltip("Per 100 m2 at full strength")
            .Field("Tree Scale", &TerrainComponent::TreeScale).Speed(0.01f).Tooltip("Smallest and largest size")
            .Field("Tree Max Slope", &TerrainComponent::TreeMaxSlope).Range(0.0f, 90.0f).Speed(0.5f).Tooltip("Degrees; steeper ground stays bare")
            .Field("Tree Collider Radius", &TerrainComponent::TreeColliderRadius).Range(0.0f, 10.0f).Speed(0.01f)
                .Tooltip("Trunk capsule radius per unit of scale, as tall as the mesh (0 = trees do not collide)")
            .Field("Grass Mesh", &TerrainComponent::GrassMesh).As(FieldType::Mesh).Tooltip("Placed where the Grass Tufts brush paints and on layers with Grass")
            .Field("Grass Material", &TerrainComponent::GrassMaterial).As(FieldType::Material).Tooltip("None = the model's own materials")
            .Field("Grass Density", &TerrainComponent::GrassDensity).Range(0.0f, 5000.0f).Speed(0.5f).Tooltip("Per 100 m2 at full strength")
            .Field("Grass Scale", &TerrainComponent::GrassScale).Speed(0.01f)
            .Field("Grass Max Slope", &TerrainComponent::GrassMaxSlope).Range(0.0f, 90.0f).Speed(0.5f)
            .Field("Grass Sway", &TerrainComponent::GrassSway).Range(0.0f, 10.0f).Speed(0.01f).Tooltip("How much tufts bend in the scene's Wind (0 = still)")
            .Field("Grass Bending", &TerrainComponent::GrassBending).Range(0.0f, 4.0f).Speed(0.01f)
                .Tooltip("How far rigid bodies and characters push tufts aside (0 = not at all)")
            .Field("Sculpted Heights", &TerrainComponent::SculptedHeights).HideInInspector()
            .Field("Painted Layers", &TerrainComponent::PaintedLayers).HideInInspector()
            .Field("Painted Holes", &TerrainComponent::PaintedHoles).HideInInspector()
            .Field("Painted Details", &TerrainComponent::PaintedDetails).HideInInspector();

        // Terrains saved before the layer list: six fixed layers (Grass, Rock, Snow, Dirt, Sand and Moss Texture and
        // Color, one Texture Tiling) become its first six entries.
        FindMutable("Terrain")->Upgrade = [](void* component, const nlohmann::json& data) {
            if (data.contains("Layers")) return;
            auto& terrain = *static_cast<TerrainComponent*>(component);
            static const char* names[] = { "Grass", "Rock", "Snow", "Dirt", "Sand", "Moss" };
            terrain.Layers = DefaultTerrainLayers();
            float tiling = data.contains("Texture Tiling") && data["Texture Tiling"].is_number() ? data["Texture Tiling"].get<float>() : 0.25f;
            for (int k = 0; k < 6; ++k)
            {
                TerrainLayer& layer = terrain.Layers[k];
                std::string texture = std::string(names[k]) + " Texture", color = std::string(names[k]) + " Color";
                if (data.contains(texture) && data[texture].is_number()) layer.Texture = AssetHandle(data[texture].get<uint64_t>());
                if (data.contains(color) && data[color].is_array() && data[color].size() >= 3)
                    for (size_t c = 0; c < std::min<size_t>(data[color].size(), 4); ++c) layer.Color[(int)c] = data[color][c].get<float>();
                layer.Tiling = tiling;
            }
        };

        Register<WeatherComponent>("Weather", "Rendering")
            .Field("Rain", &WeatherComponent::Rain).Range(0.0f, 1.0f).Speed(0.01f).Tooltip("Falling rain around the camera, 0-1")
            .Field("Snowfall", &WeatherComponent::Snowfall).Range(0.0f, 1.0f).Speed(0.01f).Tooltip("Falling snow around the camera, 0-1")
            .Field("Wetness", &WeatherComponent::Wetness).Range(0.0f, 1.0f).Speed(0.01f).Tooltip("Surfaces darker and glossy (flat ground most)")
            .Field("Snow Cover", &WeatherComponent::SnowCover).Range(0.0f, 1.0f).Speed(0.01f).Tooltip("Snow on surfaces facing up")
            .Field("Area", &WeatherComponent::Area).Range(4.0f, 500.0f).Speed(0.5f).Tooltip("Meters around the camera where it falls")
            .Field("Height", &WeatherComponent::Height).Range(1.0f, 200.0f).Speed(0.1f).Tooltip("Meters above the camera where it starts")
            .Field("Lightning", &WeatherComponent::Lightning).Range(0.0f, 1.0f).Speed(0.01f).Tooltip("Flashes: 0 = none, 1 = every few seconds")
            .Field("Thunder", &WeatherComponent::Thunder).As(FieldType::Audio).Tooltip("Played after each flash through this object's Audio Source");
        Register<FurComponent>("Fur", "Rendering")
            .Field("Length", &FurComponent::Length).Range(0.0f, 2.0f).Speed(0.001f).Tooltip("Meters (with the object's scale)")
            .Field("Shells", &FurComponent::Shells).Range(1, 64).Tooltip("Layers drawn: more look denser and smoother at the sides")
            .Field("Density", &FurComponent::Density).Range(1.0f, 5000.0f).Speed(1.0f).Tooltip("Strands per UV unit")
            .Field("Thickness", &FurComponent::Thickness).Range(0.05f, 1.0f).Speed(0.01f).Tooltip("Root width as a fraction of a strand's cell")
            .Field("Droop", &FurComponent::Droop).Range(0.0f, 2.0f).Speed(0.01f).Tooltip("How far the tips hang down, times Length")
            .Field("Wind Response", &FurComponent::WindResponse).Range(0.0f, 5.0f).Speed(0.01f).Tooltip("Sway in the scene's Wind")
            .Field("Tip Color", &FurComponent::TipColor).As(FieldType::Color4).Tooltip("Multiplies the material's color toward the tips")
            .Field("Root Shade", &FurComponent::RootShade).Range(0.0f, 1.0f).Speed(0.01f).Tooltip("Brightness at the roots");
        Register<TerrainPathComponent>("Terrain Path", "Rendering")
            .Field("Points", &TerrainPathComponent::Points).Tooltip("The curve passes through these (the object's space)")
            .Field("Mode", &TerrainPathComponent::Mode).Enum({ "Road", "River" })
            .Field("Width", &TerrainPathComponent::Width).Range(0.1f, 500.0f).Speed(0.05f)
            .Field("Falloff", &TerrainPathComponent::Falloff).Range(0.0f, 500.0f).Speed(0.05f).Tooltip("Blend into the land on each side, meters")
            .Field("Depth", &TerrainPathComponent::Depth).Range(0.0f, 100.0f).Speed(0.01f).Tooltip("River: the channel's depth below the curve")
            .Field("Paint Layer", &TerrainPathComponent::PaintLayer).Range(-1, 15).Tooltip("The terrain layer painted along it (its number in the Layers list); -1 = none")
            .Field("Terrain", &TerrainPathComponent::Terrain).Tooltip("The terrain it shapes (None = the scene's first)");
        Register<PathPlacerComponent>("Path Placer", "Rendering")
            .Field("Points", &PathPlacerComponent::Points).Tooltip("The curve passes through these (the object's space)")
            .Field("Closed", &PathPlacerComponent::Closed).Tooltip("Loop back to the first point")
            .Field("Prefab", &PathPlacerComponent::Prefab).As(FieldType::Prefab).Tooltip("What to place")
            .Field("Source", &PathPlacerComponent::Source).Tooltip("Or an object in the scene to copy (used instead of Prefab when set)")
            .Field("Spacing", &PathPlacerComponent::Spacing).Range(0.05f, 1000.0f).Speed(0.05f).Tooltip("Meters between copies, along the curve")
            .Field("Start Offset", &PathPlacerComponent::StartOffset).Range(0.0f, 1000.0f).Speed(0.05f).Tooltip("Meters along the curve to the first copy")
            .Field("Side Offset", &PathPlacerComponent::SideOffset).Speed(0.05f).Tooltip("Meters to the right of the curve (negative: left)")
            .Field("Align To Path", &PathPlacerComponent::AlignToPath).Tooltip("Face along the curve (forward is -Z)")
            .Field("Rotation", &PathPlacerComponent::Rotation).Speed(0.5f).Tooltip("Degrees about Y, added to each copy")
            .Field("Random Rotation", &PathPlacerComponent::RandomRotation).Range(0.0f, 180.0f).Speed(0.5f).Tooltip("Up to this many degrees either way")
            .Field("Random Scale", &PathPlacerComponent::RandomScale).Range(0.0f, 0.95f).Speed(0.01f).Tooltip("Up to this fraction bigger or smaller")
            .Field("Seed", &PathPlacerComponent::Seed)
            .Field("Snap To Ground", &PathPlacerComponent::SnapToGround).Tooltip("Drop each copy onto the terrain under it");
        Register<DecalComponent>("Decal", "Rendering")
            .Field("Texture", &DecalComponent::Texture).As(FieldType::Texture).Tooltip("None = a soft round spot")
            .Field("Color", &DecalComponent::Color).As(FieldType::Color4).Tooltip("Tint; alpha is the opacity")
            .Field("Size", &DecalComponent::Size).Speed(0.01f).Tooltip("Width and height of the texture, and how deep it projects (along -Z)")
            .Field("Blend", &DecalComponent::Blend).Enum({ "Alpha", "Multiply", "Additive" })
                .Tooltip("Multiply darkens (dirt, scorch marks), Alpha paints over, Additive glows")
            .Field("Angle Fade", &DecalComponent::AngleFade).Range(0.0f, 1.0f).Speed(0.01f)
                .Tooltip("Surfaces tilted away from the projector fade out (cosine where fading starts)")
            .Field("Sorting Order", &DecalComponent::SortingOrder);

        Register<SpriteRendererComponent>("Sprite Renderer", "Rendering")
            .Field("Sprite", &SpriteRendererComponent::Texture).As(FieldType::Texture)
            .Field("Color", &SpriteRendererComponent::Color).As(FieldType::Color4)
            .Field("Tiling", &SpriteRendererComponent::Tiling)
            .Field("Sorting Order", &SpriteRendererComponent::SortingOrder)
            .Field("Flip X", &SpriteRendererComponent::FlipX)
            .Field("Flip Y", &SpriteRendererComponent::FlipY)
            .Field("Sheet Columns", &SpriteRendererComponent::SheetColumns).Range(1, 256)
            .Field("Sheet Rows", &SpriteRendererComponent::SheetRows).Range(1, 256)
            .Field("Frame", &SpriteRendererComponent::Frame).Range(0, 65535)
            .Field("Sprite Name", &SpriteRendererComponent::SpriteName).HideInInspector(); // picked from the texture's sprites (Inspector)

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
            .Field("Root Motion Joint", &AnimatorComponent::RootMotionJoint).Tooltip("Empty = the first root joint (e.g. Hips)")
            .Field("Animation Source", &AnimatorComponent::ClipSource).As(FieldType::Mesh)
                .Tooltip("Play the clips of another model, retargeted onto this skeleton by joint name (None = this mesh's own)");

        Register<LightProbeVolumeComponent>("Light Probe Volume", "Rendering")
            .Field("Size", &LightProbeVolumeComponent::Size).Speed(0.05f).Tooltip("Box centered on the object")
            .Field("Probes X", &LightProbeVolumeComponent::ProbesX).Range(1, 32)
            .Field("Probes Y", &LightProbeVolumeComponent::ProbesY).Range(1, 32)
            .Field("Probes Z", &LightProbeVolumeComponent::ProbesZ).Range(1, 32)
            .Field("Intensity", &LightProbeVolumeComponent::Intensity).Range(0.0f, 20.0f).Speed(0.01f)
            .Field("Blend Distance", &LightProbeVolumeComponent::BlendDistance).Range(0.0f, 100.0f).Speed(0.01f)
                .Tooltip("Fades to the scene's own ambient light over this distance inside the box")
            .Field("Realtime", &LightProbeVolumeComponent::Realtime)
                .Tooltip("Keep capturing the probes again in turn, so bounce light follows moving lights and objects")
            .Field("Probes Per Frame", &LightProbeVolumeComponent::ProbesPerFrame).Range(1, 4)
                .Tooltip("Realtime: probes captured each frame (each is six small scene renders)")
            .Field("Baked", &LightProbeVolumeComponent::Baked).HideInInspector();

        Register<TimelineComponent>("Timeline", "Animation")
            .Field("Timeline", &TimelineComponent::Timeline).As(FieldType::Timeline).Tooltip("Timeline asset (.timeline); edit it in Window > Timeline")
            .Field("Play On Awake", &TimelineComponent::PlayOnAwake)
            .Field("Loop", &TimelineComponent::Loop)
            .Field("Speed", &TimelineComponent::Speed).Range(-10.0f, 10.0f).Speed(0.01f);

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
            .Field("Max Particles", &PS::MaxParticles).Range(1, 1000000)
            .Field("Simulation Space", &PS::SimulationSpace).Enum({ "Local", "World" })
            .Field("GPU Sorting", &PS::GpuSorting).Tooltip("Alpha-blended GPU particles draw back to front (sorted on the GPU for each view)")
            .Field("Simulate On GPU", &PS::SimulateOnGpu)
                .Tooltip("A compute shader simulates the particles (up to a million). Not on the GPU: Velocity over Lifetime,\n"
                         "World collision, Trails, and triggering a Sub Emitter")
            .Field("Rate Over Time", &PS::RateOverTime).Range(0.0f, 100000.0f).Speed(0.5f)
            .Field("Burst Count", &PS::BurstCount).Range(0, 100000)
            .Field("Shape", &PS::Shape).Enum({ "Cone", "Sphere", "Box", "Point", "Mesh" })
            .Field("Shape Angle", &PS::ShapeAngle).Range(0.0f, 90.0f).Speed(0.5f)
            .Field("Shape Radius", &PS::ShapeRadius).Range(0.0f, 1000.0f).Speed(0.01f)
            .Field("Box Size", &PS::BoxSize).Speed(0.01f)
            .Field("Shape Mesh", &PS::ShapeMesh).As(FieldType::Mesh).Tooltip("Mesh shape: emit from this mesh's surface (None = the Mesh Renderer's)")
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
            .Field("Collision Decals", &PS::CollisionDecals).Tooltip("Each particle's first impact leaves a mark where it hit (CPU and GPU systems)")
            .Field("Decal Size", &PS::DecalSize).Range(0.001f, 100.0f).Speed(0.005f)
            .Field("Decal Lifetime", &PS::DecalLifetime).Range(0.0f, 3600.0f).Speed(0.1f).Tooltip("Seconds a mark lasts (fading over the last part)")
            .Field("Max Decals", &PS::MaxDecals).Range(0, 4096).Tooltip("Marks at once (the oldest go first)")
            .Field("Decal Texture", &PS::DecalTexture).As(FieldType::Texture).Tooltip("None = a soft round spot")
            .Field("Decal Blend", &PS::DecalBlendMode).Range(0, 2).Tooltip("0 alpha, 1 multiply, 2 additive")
            .Field("Collide With Colliders", &PS::CollideWithColliders)
                .Tooltip("GPU systems: bounce off the 32 sphere, box and capsule colliders (and characters) nearest the emitter")
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
            .Field("External Forces", &PS::ExternalForces).Range(0.0f, 100.0f).Speed(0.01f)
                .Tooltip("Wind Zones: how fast particles take up the wind, per second (0 = not at all; CPU systems)")
            .Field("Lights", &PS::Lights).Tooltip("Some particles light the scene (CPU systems)")
            .Field("Light Ratio", &PS::LightRatio).Range(0.0f, 1.0f).Speed(0.005f).Tooltip("The fraction of particles carrying a light")
            .Field("Max Lights", &PS::MaxLights).Range(0, 256).Tooltip("At most this many at once (per system)")
            .Field("Light Color", &PS::LightColor).As(FieldType::Color4)
            .Field("Use Particle Color", &PS::UseParticleColor).Tooltip("Multiply by the particle's color")
            .Field("Light Intensity", &PS::LightIntensity).Range(0.0f, 1000.0f).Speed(0.05f).Tooltip("Times the particle's alpha")
            .Field("Light Range", &PS::LightRange).Range(0.01f, 1000.0f).Speed(0.02f).Tooltip("Meters")
            .Field("Size Affects Range", &PS::SizeAffectsRange).Tooltip("Multiply the range by the particle's size")
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
            .Field("Localized Key", &UITextComponent::LocalizedKey)
            .Tooltip("A key of the project's string tables (.csv): the text shown is its text in the player's language, "
                     "and follows when the language changes. Empty: Text is shown.")
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
            .Field("Tear Stretch", &ClothComponent::TearStretch).Range(0.0f, 10.0f).Speed(0.01f)
                .Tooltip("Edges stretched past (1 + this) times their length tear (0 = never), e.g. 0.5 tears at one and a\n"
                         "half times the rest length. Pinned cloth under a load, or hit hard, rips.")
            .Field("Thickness", &ClothComponent::Thickness).Range(0.0f, 1.0f).Speed(0.001f).Tooltip("How far it stays from what it touches")
            .Field("Wind", &ClothComponent::Wind).Speed(0.05f).Tooltip("World-space acceleration (m/s^2) on the free vertices")
            .Field("Attach To", &ClothComponent::AttachTo)
                .Tooltip("The pinned vertices hold on to this object (None = the cloth's own), and the cloth pulls on its Rigidbody");
        Register<RagdollComponent>("Ragdoll", "Physics")
            .Field("Simulate", &RagdollComponent::Simulate).Tooltip("On: limp under physics. Off: the capsules follow the animation (scripts switch it)")
            .Field("Mass", &RagdollComponent::Mass).Range(0.01f, 10000.0f).Speed(0.1f).Tooltip("Whole body, shared by the capsules' volumes")
            .Field("Thickness", &RagdollComponent::Thickness).Range(0.02f, 1.0f).Speed(0.005f).Tooltip("Capsule radius as a fraction of its bone's length")
            .Field("Min Bone Length", &RagdollComponent::MinBoneLength).Range(0.0f, 0.5f).Speed(0.002f)
                .Tooltip("Bones shorter than this fraction of the skeleton's height get no capsule of their own (they ride along with their parent's)")
            .Field("Swing Limit", &RagdollComponent::SwingLimit).Range(0.0f, 180.0f).Speed(0.5f).Tooltip("How far each joint bends away from its pose, degrees")
            .Field("Twist Limit", &RagdollComponent::TwistLimit).Range(0.0f, 180.0f).Speed(0.5f).Tooltip("How far each bone turns around itself, degrees")
            .Field("Friction", &RagdollComponent::Friction).Range(0.0f, 1.0f).Speed(0.01f)
            .Field("Linear Damping", &RagdollComponent::LinearDamping).Range(0.0f, 10.0f).Speed(0.01f)
            .Field("Angular Damping", &RagdollComponent::AngularDamping).Range(0.0f, 10.0f).Speed(0.01f)
            .Field("Get Up Time", &RagdollComponent::GetUpTime).Range(0.0f, 10.0f).Speed(0.01f).Header("Getting Up")
                .Tooltip("Seconds the animation takes to blend in from the limp pose when Simulate turns off (0 = snap)")
            .Field("Partial Root", &RagdollComponent::PartialRoot).Header("Partial")
                .Tooltip("A bone's name: Simulate makes only it and the bones under it go limp, while the rest animates on\n"
                         "(empty = the whole body)")
            .Field("Align On Get Up", &RagdollComponent::AlignOnGetUp)
                .Tooltip("When Simulate turns off, move the object under the body onto the ground and turn it along the body\n"
                         "(toward the head lying face down, toward the feet face up)");
        Register<DeformableComponent>("Deformable", "Physics")
            .Field("Min Speed", &DeformableComponent::MinSpeed).Range(0.0f, 1000.0f).Speed(0.05f).Tooltip("Gentler hits (m/s) leave no dent")
            .Field("Strength", &DeformableComponent::Strength).Range(0.0f, 100.0f).Speed(0.01f).Tooltip("Dent depth, centimeters per m/s over Min Speed")
            .Field("Radius", &DeformableComponent::Radius).Range(0.01f, 100.0f).Speed(0.01f).Tooltip("How wide a dent spreads, meters")
            .Field("Max Dent", &DeformableComponent::MaxDent).Range(0.0f, 100.0f).Speed(0.005f).Tooltip("Deepest any point can be pushed in, meters");
        Register<TowHitchComponent>("Tow Hitch", "Physics")
            .Field("Towed By", &TowHitchComponent::TowedBy).Tooltip("The vehicle (or any body) pulling this one")
            .Field("Hitch", &TowHitchComponent::Hitch).Speed(0.01f).Tooltip("The tow point on Towed By, in its space (a car's rear is +Z)")
            .Field("Coupling", &TowHitchComponent::Coupling).Speed(0.01f).Tooltip("The point on this object that hangs on the hitch")
            .Field("Attached", &TowHitchComponent::Attached).Tooltip("Clear to unhitch; set to hitch again where it stands")
            .Field("Break Force", &TowHitchComponent::BreakForce).Range(0.0f, 1e8f).Speed(10.0f).Tooltip("Newtons that tear it loose (0 = never)");
        Register<BoatComponent>("Boat", "Physics")
            .Field("Throttle", &BoatComponent::Throttle).Range(-1.0f, 1.0f).Speed(0.01f).Tooltip("-1 (reverse) to 1; scripts set it")
            .Field("Steering", &BoatComponent::Steering).Range(-1.0f, 1.0f).Speed(0.01f).Tooltip("-1 (left) to 1 (right); scripts set it")
            .Field("Engine Force", &BoatComponent::EngineForce).Range(0.0f, 1e7f).Speed(10.0f).Tooltip("Newtons at full throttle")
            .Field("Turn Rate", &BoatComponent::TurnRate).Range(0.0f, 720.0f).Speed(0.5f).Tooltip("Degrees per second at full steering and speed")
            .Field("Turn Speed", &BoatComponent::TurnSpeed).Range(0.1f, 100.0f).Speed(0.05f).Tooltip("Speed (m/s) at which the rudder bites fully")
            .Field("Keel", &BoatComponent::Keel).Range(0.0f, 20.0f).Speed(0.01f).Tooltip("Resistance to sliding sideways")
            .Field("Propeller", &BoatComponent::Propeller).Speed(0.01f).Tooltip("The engine works only while this point (the stern) is under water")
            .Field("Drive With Input", &BoatComponent::DriveWithInput).Tooltip("WASD / arrows / left stick steer it");
        Register<RopeComponent>("Rope", "Physics")
            .Field("Length", &RopeComponent::Length).Range(0.01f, 1000.0f).Speed(0.01f)
            .Field("Segments", &RopeComponent::Segments).Range(1, 256).Tooltip("Simulated pieces (more bend more smoothly)")
            .Field("Radius", &RopeComponent::Radius).Range(0.001f, 2.0f).Speed(0.001f).Tooltip("Thickness drawn and colliding")
            .Field("Mass", &RopeComponent::Mass).Range(0.001f, 10000.0f).Speed(0.01f).Tooltip("Total, kg")
            .Field("Stretch Compliance", &RopeComponent::StretchCompliance).Range(0.0f, 1.0f).Speed(0.00001f).Tooltip("0 = does not stretch")
            .Field("Bend Compliance", &RopeComponent::BendCompliance).Range(0.0f, 100.0f).Speed(0.01f).Tooltip("Larger = floppier (a chain), smaller = stiffer (a cable)")
            .Field("Damping", &RopeComponent::Damping).Range(0.0f, 10.0f).Speed(0.01f)
            .Field("Friction", &RopeComponent::Friction).Range(0.0f, 1.0f).Speed(0.01f)
            .Field("Iterations", &RopeComponent::Iterations).Range(1, 64)
            .Field("Pin Start", &RopeComponent::PinStart).Tooltip("The start holds on to this object (off: the whole rope falls)")
            .Field("Attach To", &RopeComponent::AttachTo).Tooltip("The end holds on to this object (a dynamic body feels the pull)")
            .Field("Attach Offset", &RopeComponent::AttachOffset).Speed(0.01f).Tooltip("Where on Attach To, in its space")
            .Field("Break Force", &RopeComponent::BreakForce).Range(0.0f, 1e8f).Speed(10.0f).Tooltip("Newtons of load that snap it (0 = never)")
            .Field("Break Stretch", &RopeComponent::BreakStretch).Range(0.0f, 10.0f).Speed(0.01f)
                .Tooltip("Snaps when pulled this fraction longer than Length (0 = never)");
        Register<BreakableComponent>("Breakable", "Physics")
            .Field("Pieces", &BreakableComponent::Pieces).Range(2, 64).Tooltip("Voronoi cells cut from the mesh (fewer where a cell misses it)")
            .Field("Seed", &BreakableComponent::Seed).Tooltip("The same seed breaks the same way")
            .Field("Break Speed", &BreakableComponent::BreakSpeed).Range(0.0f, 1000.0f).Speed(0.1f)
                .Tooltip("Impact speed along the contact normal that breaks it, m/s (0 = only scripts)")
            .Field("Inside Material", &BreakableComponent::InsideMaterial).As(FieldType::Material).Tooltip("The cut faces. None = the outside's material")
            .Field("Explosion Speed", &BreakableComponent::ExplosionSpeed).Range(0.0f, 100.0f).Speed(0.05f).Tooltip("Pieces fly apart from the break point at up to this speed")
            .Field("Debris Lifetime", &BreakableComponent::DebrisLifetime).Range(0.0f, 3600.0f).Speed(0.1f)
                .Tooltip("Seconds before the pieces shrink away (0 = they stay)")
            .Field("Impact Focus", &BreakableComponent::ImpactFocus).Range(0.0f, 1.0f).Speed(0.01f)
                .Tooltip("Share of the pieces cut small around where it breaks (0 = spread evenly)")
            .Field("Focus Radius", &BreakableComponent::FocusRadius).Range(0.01f, 1.0f).Speed(0.005f)
                .Tooltip("How far the small pieces reach from the break point, as a fraction of the object's size")
            .Field("Glue Strength", &BreakableComponent::GlueStrength).Range(0.0f, 1e7f).Speed(10.0f)
                .Tooltip("Above 0: pieces stay stuck to their neighbours (a static object's bottom ones to the ground) until\n"
                         "a force above this (N) pulls them apart: walls crack and crumble instead of falling apart at once")
            .Field("Density", &BreakableComponent::Density).Range(1.0f, 20000.0f).Speed(10.0f)
                .Tooltip("kg per m3 of the pieces when the object is not a dynamic body (a dynamic body's mass is shared)");
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
        Register<WaterVolumeComponent>("Water Volume", "Physics")
            .Field("Size", &WaterVolumeComponent::Size).Speed(0.05f).Tooltip("Box centered on the object; its top face is the water surface")
            .Field("Buoyancy", &WaterVolumeComponent::Buoyancy).Range(0.0f, 5.0f).Speed(0.01f).Tooltip("1 = neutral, above floats, below sinks")
            .Field("Linear Drag", &WaterVolumeComponent::LinearDrag).Range(0.0f, 50.0f).Speed(0.01f)
            .Field("Angular Drag", &WaterVolumeComponent::AngularDrag).Range(0.0f, 50.0f).Speed(0.01f)
            .Field("Flow", &WaterVolumeComponent::Flow).Speed(0.05f).Tooltip("Current (m/s) that carries floating bodies")
            .Field("Wave Height", &WaterVolumeComponent::WaveHeight).Range(0.0f, 20.0f).Speed(0.01f).Tooltip("Crest to trough of the main wave (0 = flat water)")
            .Field("Wave Length", &WaterVolumeComponent::WaveLength).Range(0.1f, 500.0f).Speed(0.05f).Tooltip("Crest to crest; longer waves move faster (deep-water speed)")
            .Field("Wave Direction", &WaterVolumeComponent::WaveDirection).Range(-360.0f, 360.0f).Speed(0.5f).Tooltip("Degrees around Y from +X")
            .Field("Steepness", &WaterVolumeComponent::Steepness).Range(0.0f, 1.0f).Speed(0.01f).Tooltip("0 = round waves, 1 = sharp crests")
            .Field("Surface Resolution", &WaterVolumeComponent::SurfaceResolution).Range(2, 256)
            .Field("Ocean", &WaterVolumeComponent::Ocean).Tooltip("Reaches the horizon all round: no sides, the surface drawn out to the camera's far plane")
            .Field("Foam", &WaterVolumeComponent::Foam).Range(0.0f, 2.0f).Speed(0.01f).Tooltip("Froth on the wave crests (steeper waves make more)")
            .Field("Shore Foam", &WaterVolumeComponent::ShoreFoam).Range(0.0f, 10.0f).Speed(0.01f).Tooltip("Froth where the ground is less than this many meters under the surface (while playing)")
                .Tooltip("Quads per side of the drawn surface (with a Mesh Renderer for its material)")
            .Field("Underwater Color", &WaterVolumeComponent::UnderwaterColor).As(FieldType::Color4).Tooltip("The murk seen through the water")
            .Field("Underwater Fog", &WaterVolumeComponent::UnderwaterFog).Range(0.0f, 10.0f).Speed(0.005f).Tooltip("How thick the murk is, per meter (0 = clear)")
            .Field("Caustics", &WaterVolumeComponent::Caustics).Range(0.0f, 4.0f).Speed(0.01f).Tooltip("Rippling sunlight on surfaces under water (0 = none)")
            .Field("Caustics Scale", &WaterVolumeComponent::CausticsScale).Range(0.1f, 50.0f).Speed(0.01f).Tooltip("Meters across the caustic pattern's cells");
        Register<WindZoneComponent>("Wind Zone", "Physics")
            .Field("Mode", &WindZoneComponent::Mode).Enum({ "Directional", "Spherical" })
                .Tooltip("Directional: along the object's forward (-Z) everywhere; Spherical: out from the object, within Radius")
            .Field("Radius", &WindZoneComponent::Radius).Range(0.01f, 10000.0f).Speed(0.1f).Tooltip("Spherical: where it fades to nothing")
            .Field("Main", &WindZoneComponent::Main).Range(-200.0f, 200.0f).Speed(0.05f).Tooltip("Wind speed, m/s")
            .Field("Turbulence", &WindZoneComponent::Turbulence).Range(0.0f, 5.0f).Speed(0.01f).Tooltip("Swirls, as a fraction of Main")
            .Field("Pulse Magnitude", &WindZoneComponent::PulseMagnitude).Range(0.0f, 5.0f).Speed(0.01f).Tooltip("How much the gusts swell it")
            .Field("Pulse Frequency", &WindZoneComponent::PulseFrequency).Range(0.0f, 10.0f).Speed(0.01f).Tooltip("Gusts per second");
        Register<NavMeshSurfaceComponent>("NavMesh Surface", "Navigation")
            .Field("Size", &NavMeshSurfaceComponent::Size).Speed(0.1f).Tooltip("The box baked, around the object (meters; not rotated)")
            .Field("Cell Size", &NavMeshSurfaceComponent::CellSize).Range(0.05f, 4.0f).Speed(0.01f).Tooltip("Meters per sample: smaller follows walls closer but bakes slower")
            .Field("Agent Radius", &NavMeshSurfaceComponent::AgentRadius).Range(0.0f, 10.0f).Speed(0.01f).Tooltip("Agents keep this far from walls and edges")
            .Field("Agent Height", &NavMeshSurfaceComponent::AgentHeight).Range(0.1f, 20.0f).Speed(0.01f).Tooltip("Headroom needed to walk under things")
            .Field("Max Slope", &NavMeshSurfaceComponent::MaxSlope).Range(0.0f, 89.0f).Speed(0.5f).Tooltip("Steepest walkable ground, degrees")
            .Field("Step Height", &NavMeshSurfaceComponent::StepHeight).Range(0.0f, 5.0f).Speed(0.01f).Tooltip("Highest step walked up")
            .Field("Show NavMesh", &NavMeshSurfaceComponent::ShowNavMesh)
            .Field("Generate Links", &NavMeshSurfaceComponent::GenerateLinks).Tooltip("Off-mesh links down ledges and across gaps")
            .Field("Drop Height", &NavMeshSurfaceComponent::DropHeight).Range(0.0f, 50.0f).Speed(0.05f).Tooltip("Highest ledge agents drop down (one way)")
            .Field("Jump Distance", &NavMeshSurfaceComponent::JumpDistance).Range(0.0f, 20.0f).Speed(0.05f).Tooltip("Widest gap agents jump across")
            .Field("Moving", &NavMeshSurfaceComponent::Moving)
                .Tooltip("Rides on its object (a boat, a lift): baked from its own colliders, carrying agents on it along");
        Register<NavMeshModifierVolumeComponent>("NavMesh Modifier Volume", "Navigation")
            .Field("Center", &NavMeshModifierVolumeComponent::Center).Speed(0.01f)
            .Field("Size", &NavMeshModifierVolumeComponent::Size).Speed(0.05f)
            .Field("Area", &NavMeshModifierVolumeComponent::Area).Enum({ "Walkable", "Not Walkable", "Road", "Grass", "Mud", "Water", "Area 6", "Area 7" })
            .Field("Cost", &NavMeshModifierVolumeComponent::Cost).Range(0.0f, 100.0f).Speed(0.05f).Tooltip("Walking here counts this many times (below 1: preferred; 0 = the area's cost, Window > Navigation)");
        Register<NavMeshObstacleComponent>("NavMesh Obstacle", "Navigation")
            .Field("Shape", &NavMeshObstacleComponent::Shape).Enum({ "Box", "Capsule" })
            .Field("Center", &NavMeshObstacleComponent::Center).Speed(0.01f)
            .Field("Size", &NavMeshObstacleComponent::Size).Speed(0.01f).Tooltip("Box")
            .Field("Radius", &NavMeshObstacleComponent::Radius).Range(0.0f, 100.0f).Speed(0.01f).Tooltip("Capsule")
            .Field("Height", &NavMeshObstacleComponent::Height).Range(0.0f, 100.0f).Speed(0.01f).Tooltip("Capsule")
            .Field("Carve", &NavMeshObstacleComponent::Carve).Tooltip("Cut a hole in the navigation mesh (paths go around it)")
            .Field("Move Threshold", &NavMeshObstacleComponent::MoveThreshold).Range(0.0f, 10.0f).Speed(0.01f).Tooltip("Carving: cut again after moving this far")
            .Field("Carve Only Stationary", &NavMeshObstacleComponent::CarveOnlyStationary).Tooltip("Carving: only once it stands still (steered around while moving)")
            .Field("Time To Stationary", &NavMeshObstacleComponent::TimeToStationary).Range(0.0f, 10.0f).Speed(0.01f);
        Register<OffMeshLinkComponent>("Off Mesh Link", "Navigation")
            .Field("Start", &OffMeshLinkComponent::Start).Speed(0.01f).Tooltip("In the object's space")
            .Field("End", &OffMeshLinkComponent::End).Speed(0.01f)
            .Field("Bidirectional", &OffMeshLinkComponent::Bidirectional)
            .Field("Activated", &OffMeshLinkComponent::Activated)
            .Field("Arc Height", &OffMeshLinkComponent::ArcHeight).Range(0.0f, 20.0f).Speed(0.01f).Tooltip("How high agents jump crossing it (0 = straight, a ladder)")
            .Field("Cost Modifier", &OffMeshLinkComponent::CostModifier).Range(0.0f, 100.0f).Speed(0.01f).Tooltip("Its length counts this many times when choosing paths")
            .Field("Agents Wait", &OffMeshLinkComponent::AgentsWait).Tooltip("A door: while not Activated, agents still go this way and wait at it until it is");
        Register<NavMeshAgentComponent>("NavMesh Agent", "Navigation")
            .Field("Speed", &NavMeshAgentComponent::Speed).Range(0.0f, 100.0f).Speed(0.05f).Tooltip("Top speed, m/s")
            .Field("Angular Speed", &NavMeshAgentComponent::AngularSpeed).Range(0.0f, 3600.0f).Speed(1.0f).Tooltip("Turning, degrees per second")
            .Field("Acceleration", &NavMeshAgentComponent::Acceleration).Range(0.0f, 1000.0f).Speed(0.1f)
            .Field("Stopping Distance", &NavMeshAgentComponent::StoppingDistance).Range(0.0f, 100.0f).Speed(0.05f).Tooltip("Stops this far from the destination")
            .Field("Auto Braking", &NavMeshAgentComponent::AutoBraking).Tooltip("Slows down to arrive instead of overshooting")
            .Field("Radius", &NavMeshAgentComponent::Radius).Range(0.01f, 10.0f).Speed(0.01f).Tooltip("Room it takes among other agents")
            .Field("Height", &NavMeshAgentComponent::Height).Range(0.01f, 20.0f).Speed(0.01f)
            .Field("Base Offset", &NavMeshAgentComponent::BaseOffset).Speed(0.01f).Tooltip("How far its object stands above the ground")
            .Field("Obstacle Avoidance", &NavMeshAgentComponent::ObstacleAvoidance).Tooltip("Steers around other agents")
            .Field("Avoidance Priority", &NavMeshAgentComponent::AvoidancePriority).Range(0.0f, 99.0f).Tooltip("Gives way to agents with a lower number")
            .Field("Target", &NavMeshAgentComponent::Target).Tooltip("Keeps walking to this object (optional; scripts can set destinations instead)")
            .Field("Area Mask", &NavMeshAgentComponent::AreaMask).Tooltip("A bit per area it may walk: 1 Walkable, 4 Road, 8 Grass, 16 Mud, 32 Water, ... (-1 = all)")
            .Field("Speed Parameter", &NavMeshAgentComponent::SpeedParameter).Header("Animation")
                .Tooltip("An Animator float parameter set to its speed each frame (empty = none)")
            .Field("Root Motion", &NavMeshAgentComponent::RootMotion)
                .Tooltip("Its Animator's root motion (Apply Root Motion) moves it: the agent turns it and asks for a speed through Speed Parameter");
        Register<AudioReverbZoneComponent>("Audio Reverb Zone", "Audio")
            .Field("Min Distance", &AudioReverbZoneComponent::MinDistance).Range(0.0f, 10000.0f).Speed(0.05f)
                .Tooltip("Full reverb while the listener is this close")
            .Field("Max Distance", &AudioReverbZoneComponent::MaxDistance).Range(0.0f, 10000.0f).Speed(0.05f)
                .Tooltip("The reverb fades out by this distance")
            .Field("Room Size", &AudioReverbZoneComponent::RoomSize).Range(0.0f, 1.0f).Speed(0.01f)
            .Field("Damping", &AudioReverbZoneComponent::Damping).Range(0.0f, 1.0f).Speed(0.01f).Tooltip("High-frequency absorption")
            .Field("Wet", &AudioReverbZoneComponent::Wet).Range(0.0f, 1.0f).Speed(0.01f)
            .Field("Group", &AudioReverbZoneComponent::Group).Tooltip("Mixer group whose reverb the zone drives (Master = everything)");
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
            .Field("Drive", &VehicleComponent::Drive).Enum({ "Rear Wheel", "Front Wheel", "All Wheel", "Unpowered" })
                .Tooltip("Cars only. Unpowered: no engine, the wheels roll freely (trailers)")
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

        Register<NoteComponent>("Note", "Miscellaneous")
            .Field("Text", &NoteComponent::Text).Multiline().Tooltip("Shown in the Inspector, at the object in the Scene view and in the Hierarchy")
            .Field("Color", &NoteComponent::Color).As(FieldType::Color4)
            .Field("Show In Scene", &NoteComponent::ShowInScene);

        Register<LODGroupComponent>("LOD Group", "Rendering")
            .Field("LOD 1 Below", &LODGroupComponent::LOD1Below).Range(0.0f, 1.0f).Speed(0.005f).Tooltip("Screen height (fraction) below which child 1 shows instead of child 0")
            .Field("LOD 2 Below", &LODGroupComponent::LOD2Below).Range(0.0f, 1.0f).Speed(0.005f)
            .Field("LOD 3 Below", &LODGroupComponent::LOD3Below).Range(0.0f, 1.0f).Speed(0.005f)
            .Field("Cull Below", &LODGroupComponent::CullBelow).Range(0.0f, 1.0f).Speed(0.001f).Tooltip("Screen height below which nothing shows")
            .Field("Size", &LODGroupComponent::Size).Range(0.0f, 10000.0f).Speed(0.05f).Tooltip("World size measured against the screen (0 = the children's bounds)")
            .Field("Bias", &LODGroupComponent::Bias).Range(0.01f, 10.0f).Speed(0.01f).Tooltip("Above 1 keeps detail farther away")
            .Field("Fade Width", &LODGroupComponent::FadeWidth).Range(0.0f, 1.0f).Speed(0.005f).Tooltip("Cross-fade between levels over this fraction above each threshold (0 = switch at once)")
            .Field("Generated Levels", &LODGroupComponent::GeneratedLevels).Range(0.0f, 3.0f).Tooltip("Levels made from this object's own mesh (instead of children)")
            .Field("Reduction", &LODGroupComponent::Reduction).Range(0.05f, 0.95f).Speed(0.01f).Tooltip("Generated levels: the share of triangles each keeps of the one before")
            .Field("Impostor", &LODGroupComponent::Impostor).Tooltip("The farthest level a camera-facing picture of LOD 0 (from 8 sides), down to Cull Below");
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
