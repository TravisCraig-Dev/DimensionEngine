#pragma once
#include "Dimension/Scene/Tilemap.h"
#include "Dimension/Scene/LineRenderer.h"
#include "Dimension/Core/Guid.h"
#include "Dimension/ECS/Entity.h"
#include "Dimension/Renderer/Font.h"
#include "Dimension/Particles/ParticleSystem.h"
#include "Dimension/Animation/AnimatorController.h"
#include "Dimension/Animation/Animation.h"
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
        std::vector<std::string> ComponentOrder; // Inspector display order (component names); empty = registry order
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
        // Part of a prefab nested inside Prefab: the inner prefab, this entity's id in it, and whether it is the
        // nested instance's root (saving the outer prefab stores the nested instance as a reference to the inner one).
        AssetHandle NestedPrefab = AssetHandle::None();
        uint64_t NestedSource = 0;
        bool NestedRoot = false;

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

    // A road or river shaped into a terrain (see Terrain::ApplyPath): a smooth curve through Points (the object's space),
    // Width wide and blending into the land over Falloff on each side. Road levels the ground across it to the curve's
    // height; River carves a channel Depth below the curve (deepest in the middle) and lowers its banks to it. Either can
    // paint a terrain layer along it. Inspector > Apply to Terrain bakes it into Terrain (None = the scene's first).
    enum class TerrainPathMode : int32_t { Road = 0, River = 1 };
    struct TerrainPathComponent
    {
        std::vector<glm::vec3> Points{ { -10.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 5.0f }, { 10.0f, 0.0f, 0.0f } };
        TerrainPathMode Mode = TerrainPathMode::Road;
        float Width = 4.0f;
        float Falloff = 3.0f;
        float Depth = 1.5f;        // River
        int32_t PaintLayer = 3;    // 0 grass, 1 rock, 2 snow, 3 dirt, 4 sand, 5 moss; -1 = none
        Entity Terrain = Entity::Null();
    };

    // Places copies of a prefab (or of a Source object in the scene) along a smooth curve through Points (the object's
    // space), like Unity's Spline Instantiate: fence posts, rows of trees, lamp posts along a road. One every Spacing
    // meters along the curve (the first Start Offset in), Side Offset meters to its right, facing along it (Align To
    // Path; forward is -Z) and turned by Rotation degrees, then scattered by up to Random Rotation degrees and Random
    // Scale (a fraction) from Seed; Snap To Ground drops each onto the terrain under it. Inspector > Place Along Path
    // (PathPlacer::Place) makes the copies the object's children, replacing those placed before.
    struct PathPlacerComponent
    {
        std::vector<glm::vec3> Points{ { -10.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 5.0f }, { 10.0f, 0.0f, 0.0f } };
        bool Closed = false;
        AssetHandle Prefab = AssetHandle::None();
        Entity Source = Entity::Null(); // used instead of Prefab when set
        float Spacing = 2.0f;
        float StartOffset = 0.0f;
        float SideOffset = 0.0f;
        bool AlignToPath = true;
        float Rotation = 0.0f;       // degrees about Y
        float RandomRotation = 0.0f; // degrees, either way
        float RandomScale = 0.0f;    // fraction, either way
        int32_t Seed = 1;
        bool SnapToGround = true;
    };

    // Projects a texture onto whatever surfaces lie inside its box (Size, centered on the object), along the object's
    // -Z axis, like Unity's Decal Projector: dirt, scorch marks, puddles, signs. Multiply darkens what is under it,
    // Alpha paints over it and Additive makes it glow. Surfaces turned away from the projector fade out.
    // A landscape (see Dimension/Terrain/Terrain.h): needs a Mesh Renderer, whose own mesh it replaces.
    struct TerrainComponent
    {
        glm::vec2 Size{ 100.0f, 100.0f }; // meters along X and Z, centered on the object
        float Height = 12.0f;             // meters at full heightmap value
        int32_t Resolution = 128;         // samples per side
        AssetHandle Heightmap = AssetHandle::None(); // grayscale image (16-bit keeps detail); None = generated noise
        // Generated terrain: fractal noise.
        int32_t Seed = 1;
        float NoiseScale = 0.02f; // features per meter of the largest hills
        int32_t Octaves = 5;
        float Roughness = 0.5f;   // how much each finer octave adds
        float FlatCenter = 0.0f;  // radius (fraction of the half size) kept at Flat Height, e.g. for a play area
        float FlatHeight = 0.2f;  // 0-1 of Height
        // Surface layers, blended by slope and height: grass on gentle ground, rock on steep slopes, snow up high.
        AssetHandle GrassTexture = AssetHandle::None(), RockTexture = AssetHandle::None(), SnowTexture = AssetHandle::None();
        glm::vec4 GrassColor{ 0.36f, 0.55f, 0.24f, 1.0f };
        glm::vec4 RockColor{ 0.46f, 0.43f, 0.40f, 1.0f };
        glm::vec4 SnowColor{ 0.95f, 0.96f, 1.0f, 1.0f };
        float TextureTiling = 0.25f; // layer texture repeats per meter
        float RockSlope = 35.0f;     // degrees where rock takes over
        float SnowHeight = 0.72f;    // 0-1 of Height where snow starts
        // Paint-only layers (the automatic blend uses the first three): dirt paths, sand, moss.
        AssetHandle DirtTexture = AssetHandle::None(), SandTexture = AssetHandle::None(), MossTexture = AssetHandle::None();
        glm::vec4 DirtColor{ 0.40f, 0.30f, 0.21f, 1.0f };
        glm::vec4 SandColor{ 0.78f, 0.71f, 0.52f, 1.0f };
        glm::vec4 MossColor{ 0.22f, 0.31f, 0.14f, 1.0f };
        // Sculpted and painted terrain (the editor's brushes): the heights (0-1 of Height, 16-bit) and layer weights
        // (3 or 6 bytes) per sample, base64, replacing the heightmap/noise and the automatic layers. Empty = not edited.
        std::string SculptedHeights, PaintedLayers;
        // Holes (caves, tunnels, a well): one bit per sample, base64; triangles touching a hole sample neither render
        // nor collide. Empty = none.
        std::string PaintedHoles;
        // Details: trees (bushes, rocks) and grass tufts scattered where the detail brushes painted them, as instances
        // of a mesh: Density per 100 m2 where painted at full strength, each turned at random and sized between the
        // Min and Max Scale, only on ground flatter than Max Slope. Trees can collide as upright capsules.
        AssetHandle TreeMesh = AssetHandle::None(), TreeMaterial = AssetHandle::None();
        float TreeDensity = 3.0f;
        glm::vec2 TreeScale{ 0.8f, 1.25f };
        float TreeMaxSlope = 30.0f;
        float TreeColliderRadius = 0.0f; // per unit of scale; 0 = trees do not collide
        AssetHandle GrassMesh = AssetHandle::None(), GrassMaterial = AssetHandle::None();
        float GrassDensity = 80.0f;
        glm::vec2 GrassScale{ 0.7f, 1.3f };
        float GrassMaxSlope = 35.0f;
        // Grass tufts move: Sway is how much they bend in the scene's Wind, Bending how far rigid bodies and characters
        // walking through push them aside (0 = rigid).
        float GrassSway = 1.0f;
        float GrassBending = 1.0f;
        std::string PaintedDetails; // tree and grass density per sample (2 bytes each), base64. Empty = none
        // Runtime: decoded edits while brushing (encoded back when a stroke ends), and a counter that changes with them.
        std::vector<float> EditHeights;
        std::vector<uint32_t> EditLayers; // two RGBA8 splat maps one after the other (grass/rock/snow, dirt/sand/moss)
        std::vector<uint8_t> EditDetails;
        std::vector<uint8_t> EditHoles;   // 1 per hole sample
        uint64_t EditRevision = 0;
    };

    // Fur (and short grass, moss, carpet) on the object's Mesh Renderer, drawn as shells: the mesh again Shells times,
    // each pushed out along its normals by up to Length, where only the strands' cross-sections stay (Density strands
    // per UV unit, Thickness of their cells at the root, tapering to the tip, of random heights). The strands droop
    // under gravity, sway in the scene's Wind, darken toward the roots and take Tip Color toward the tips. Skinned
    // meshes work too.
    struct FurComponent
    {
        float Length = 0.04f;    // meters
        int32_t Shells = 16;
        float Density = 300.0f;  // strands per UV unit
        float Thickness = 0.7f;  // root width as a fraction of a strand's cell
        float Droop = 0.4f;      // how far the tips hang down, times Length
        float WindResponse = 0.5f;
        glm::vec4 TipColor{ 1.15f, 1.1f, 1.05f, 1.0f }; // multiplies the material's color at the tips
        float RootShade = 0.45f; // brightness at the roots (fake self-shadowing)
    };

    // Weather: rain and snow falling around the primary camera (GPU particles over Area, starting Height above it,
    // stopped by what the camera sees), and its marks on every surface: Wetness darkens them and makes them glossy
    // (flat ground most), Snow Cover settles snow on what faces up. Scripts can fade any of them.
    struct WeatherComponent
    {
        float Rain = 0.0f;      // falling rain, 0-1
        float Snowfall = 0.0f;  // falling snow, 0-1
        float Wetness = 0.0f;   // wet surfaces, 0-1
        float SnowCover = 0.0f; // snow on the ground, 0-1
        float Area = 40.0f;     // meters around the camera
        float Height = 18.0f;   // meters above the camera where it starts
        // Lightning: random flashes lighting the sky and the scene (0 = none, 1 = every few seconds), each followed by
        // Thunder (played through the object's Audio Source, later the farther the strike).
        float Lightning = 0.0f;
        AssetHandle Thunder = AssetHandle::None();
        // Runtime: the precipitation's GPU particle systems and where they emit from (set every update; +Y down), and
        // the lightning: the flash's brightness now, seconds since it struck, until the next one and until its thunder.
        ParticleSystemComponent RainParticles, SnowParticles;
        glm::mat4 EmitterWorld{ 1.0f }, RainWorld{ 1.0f }, SnowWorld{ 1.0f };
        float Flash = 0.0f, FlashAge = 1e9f, NextFlash = -1.0f, ThunderIn = -1.0f;
        uint32_t LightningSeed = 0x6d2b79f5u;
    };

    enum class DecalBlend : int32_t { Alpha = 0, Multiply = 1, Additive = 2 };
    struct DecalComponent
    {
        AssetHandle Texture = AssetHandle::None(); // None = a soft round spot
        glm::vec4 Color{ 1.0f };   // sRGB tint, alpha = opacity
        glm::vec3 Size{ 1.0f, 1.0f, 1.0f }; // width, height (texture) and projection depth
        DecalBlend Blend = DecalBlend::Multiply;
        float AngleFade = 0.3f;    // cosine of the surface angle where it starts to fade
        int32_t SortingOrder = 0;
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
        std::vector<AssetHandle> Materials;         // per material slot (sub-mesh) overrides; empty entries fall through
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
        bool SoftShadows = false;     // PCSS: penumbrae widen with the distance from the occluder
        float LightAngle = 1.0f;      // apparent diameter of the light in degrees (the sun is about 0.5)
    };

    struct PointLightComponent
    {
        glm::vec3 Color{ 1.0f };
        float Intensity = 1.0f;
        float Range = 10.0f;
        bool CastShadows = false;  // up to 4 point lights cast shadows (cube shadow maps)
        float ShadowBias = 0.05f;  // world units
        AssetHandle Cookie = AssetHandle::None(); // equirectangular texture around the light (tints it; turns with it)
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
        AssetHandle Cookie = AssetHandle::None(); // texture projected through the cone (tints the light; turns with the light)
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
        int StateIndex = -1, PreviousStateIndex = -1; // controller states (synced layers follow them)
    };

    struct AnimatorComponent
    {
        AssetHandle Controller = AssetHandle::None(); // optional state machine; when set it drives Clip/Loop
        std::string Clip;          // clip name inside the mesh asset
        float Speed = 1.0f;
        bool Loop = true;
        bool Playing = true;
        bool ApplyRootMotion = false;  // the root joint's horizontal movement moves the object (or its Character Controller)
        std::string RootMotionJoint;   // empty = the skeleton's first root joint
        // Animation Source: another model whose clips this one plays, retargeted onto its own skeleton by joint name
        // (None = the clips of its own mesh).
        AssetHandle ClipSource = AssetHandle::None();

        // Root motion (runtime): movement applied this frame, world space.
        glm::vec3 RootMotionDelta{ 0.0f };
        RootMotionState RootMotion;

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
        // Timeline animation tracks (runtime): while TimelineDriven, the Animator plays TimelineClip at TimelineTime
        // (crossfaded from TimelinePrevClip by 1 - TimelineWeight) instead of its own clip or controller.
        bool TimelineDriven = false;
        std::string TimelineClip, TimelinePrevClip;
        float TimelineTime = 0.0f, TimelinePrevTime = 0.0f, TimelineWeight = 1.0f;
        bool TimelineLoop = true, TimelinePrevLoop = true;
        void ResetTrigger(const std::string& name) { Runtime.Triggers.erase(name); }

        // Runtime state (not serialized).
        float Time = 0.0f;
        int TreeState = -1;        // controller state index when playing a blend tree (time is the state machine's)
        std::string PreviousClip;  // crossfade source
        int PreviousTreeState = -1;
        float PreviousTime = 0.0f; // seconds for clips, normalized time for blend trees
        float FadeElapsed = 0.0f, FadeDuration = 0.0f;
        int StateIndex = -1, PreviousStateIndex = -1; // controller states (synced layers follow them)
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

    // Inverse kinematics target (like Animation Rigging's Two Bone IK and Chain IK constraints). Put it on a target
    // object: after the Animator samples its pose, the Tip joint of the Animator's skeleton is pulled to this object's
    // position by turning the Chain Length joints above it. Move the target (in the editor or from scripts) to aim a
    // hand, plant a foot or point a tentacle.
    // Reach pulls the Tip joint to the target; Look At turns the Tip joint (shared over its chain) to aim at it.
    enum class IKMode : int32_t { Reach = 0, LookAt = 1 };

    struct IKTargetComponent
    {
        Entity Animator = Entity::Null(); // object with the Animator; null = the nearest ancestor that has one
        IKMode Mode = IKMode::Reach;
        std::string Tip;                  // joint that reaches (or aims at) the target
        int ChainLength = 2;              // Reach: joints above the tip that turn (2 = analytic two-bone, others iterate);
                                          // Look At: joints sharing the turn, the tip included (1 = the tip alone)
        Entity Hint = Entity::Null();     // optional pole: a two-bone chain bends toward it (an elbow or knee target)
        glm::vec3 AimAxis{ 0.0f, 0.0f, 1.0f }; // Look At: the tip joint's own axis that points at the target
        float MaxAngle = 90.0f;           // Look At: largest turn from the animated pose (degrees)
        float Weight = 1.0f;
        int Layer = -1;                   // applied after this Animator layer (0 = base); -1 = after all layers
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
        bool RichText = true;                   // tags as in UI Text
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
        bool MultiLine = false;     // Enter starts a new line (Ctrl+Enter submits); Up/Down move between lines
        bool WordWrap = true;       // multi-line: long lines wrap at the field's width instead of scrolling sideways
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
        float ScrollY = 0.0f;         // multi-line: pixels scrolled up
    };

    enum class UILayoutDirection : int32_t { Vertical = 0, Horizontal = 1, Grid = 2 };

    // Arranges child UI Rects in a row or column (like Vertical/Horizontal Layout Group), or in a grid of equal cells
    // filled left to right, then top to bottom (like Grid Layout Group).
    struct UILayoutGroupComponent
    {
        UILayoutDirection Direction = UILayoutDirection::Vertical;
        float Spacing = 8.0f;
        glm::vec4 Padding{ 8.0f };  // left, top, right, bottom
        bool StretchChildren = true; // children fill the cross axis (rows and columns only)
        glm::vec2 CellSize{ 100.0f, 100.0f }; // Grid: size of every cell
        int32_t Columns = 0;                  // Grid: fixed column count; 0 = as many as fit the width
    };

    // Clips its children to its rect and scrolls its first child (the content) vertically and/or horizontally.
    // Scrollbars on the right and bottom edges appear while the content is larger than the view.
    struct UIScrollViewComponent
    {
        bool Vertical = true;
        bool Horizontal = false;    // the wheel scrolls sideways with Shift held, or always when Vertical is off
        float ScrollSpeed = 60.0f;  // canvas units per wheel notch
        bool Scrollbar = true;
        float ScrollbarWidth = 10.0f; // canvas units
        glm::vec4 ScrollbarTrackColor{ 1.0f, 1.0f, 1.0f, 0.12f };
        glm::vec4 ScrollbarHandleColor{ 1.0f, 1.0f, 1.0f, 0.55f };

        // Runtime (not serialized)
        float ScrollPosition = 0.0f;  // canvas units scrolled down
        float MaxScroll = 0.0f;       // set by the layout pass
        float ScrollPositionX = 0.0f; // canvas units scrolled right
        float MaxScrollX = 0.0f;
        bool DraggingScrollbar = false;
        bool DraggingHorizontal = false; // which scrollbar is being dragged
        float DragOffset = 0.0f;      // pointer offset inside the handle (pixels)
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
        bool RichText = true; // <b>, <i>, <u>, <s>, <color=...>, <size=...> and <br> tags (see Font::ParseRichText)
    };

    enum class UINavigationMode : int32_t { Automatic = 0, Explicit = 1, None = 2 };

    // Keyboard/gamepad navigation settings of a widget (Unity's Selectable navigation). Automatic moves to the nearest
    // widget in the pressed direction unless a link is set for that direction; Explicit only follows the links (an
    // empty link goes nowhere); None takes the widget out of arrow navigation (the mouse still works).
    struct UINavigationComponent
    {
        UINavigationMode Mode = UINavigationMode::Automatic;
        Entity SelectOnUp = Entity::Null();
        Entity SelectOnDown = Entity::Null();
        Entity SelectOnLeft = Entity::Null();
        Entity SelectOnRight = Entity::Null();
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
        float BlendDistance = 1.0f;           // fades out over this distance inside the box walls (blends with neighbors)
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
        // Occlusion (spatial sources): when 3D colliders block the line from the listener, the sound gets quieter by
        // Occlusion Volume (dB) and muffled by a low-pass at Occlusion Cutoff (Hz), easing in and out.
        bool Occlusion = false;
        float OcclusionVolume = -9.0f;
        float OcclusionCutoff = 1200.0f;
        float OcclusionAmount = 0.0f;  // runtime: 0 = clear line, 1 = fully occluded
        std::shared_ptr<void> Runtime; // ma_sound (and its occlusion filter), runtime only
    };

    // How much a collider blocks sound for occluding Audio Sources (thin glass 0.3, a curtain 0.15, a wall 1).
    // Several along the line add up; colliders without this component count 1.
    struct AudioOccluderComponent
    {
        float Occlusion = 0.5f;
    };

    // A region where a mixer group gets reverb, like Unity's Audio Reverb Zone: full strength while the listener (the
    // primary camera) is within Min Distance of the object, fading out by Max Distance. Overlapping zones blend by
    // strength. The reverb is the group's own (Audio Mixer), so the group's effect settings are what zones fade from.
    // A body of water: dynamic rigidbodies inside its box float (or sink) by the part of their shape under its top
    // face, are slowed by the water and carried by its Flow. The box is axis aligned (the object's rotation is
    // ignored); the visuals are up to the scene (e.g. a translucent plane at the surface).
    struct WaterVolumeComponent;

    // Baked indirect light (like Unity's Light Probe Group, on a grid): a box of probes, each a capture of the scene
    // around it reduced to spherical-harmonic irradiance. Surfaces inside take their ambient light from the probes
    // around them, so a room lit by a window is bright near it and dim in the corners, and color bleeds from lit
    // walls. Bake after changing the scene (Inspector); unbaked volumes bake themselves when first shown.
    struct LightProbeVolumeComponent
    {
        glm::vec3 Size{ 10.0f, 4.0f, 10.0f }; // centered on the object
        int32_t ProbesX = 4, ProbesY = 2, ProbesZ = 4;
        float Intensity = 1.0f;
        float BlendDistance = 1.0f; // fades to the scene's own ambient over this distance inside the box
        // Realtime: the probes keep being captured again, Probes Per Frame at a time in turn, so bounce light follows
        // moving lights and objects (a full refresh every ProbeCount / Probes Per Frame frames). Each capture sees the
        // volume's own light from the last pass, so light bounces more than once.
        bool Realtime = false;
        int32_t ProbesPerFrame = 2;
        std::vector<glm::vec3> Baked; // 9 irradiance SH coefficients per probe (x fastest, then y, then z)
        // Runtime: next probe to capture (-1 = not baking) and captures still on their way back from the GPU.
        int32_t BakeNext = -1;
        int32_t BakePending = 0;
        int ProbeCount() const { return std::max(ProbesX, 1) * std::max(ProbesY, 1) * std::max(ProbesZ, 1); }
        glm::vec3 ProbePosition(const glm::vec3& center, int index) const
        {
            int nx = std::max(ProbesX, 1), ny = std::max(ProbesY, 1), nz = std::max(ProbesZ, 1);
            glm::ivec3 i(index % nx, (index / nx) % ny, index / (nx * ny));
            glm::vec3 n(nx, ny, nz), f = glm::vec3(i) / glm::max(n - 1.0f, glm::vec3(1.0f));
            for (int a = 0; a < 3; ++a)
                if (n[a] <= 1.0f) f[a] = 0.5f;
            return center + (f - 0.5f) * glm::abs(Size);
        }
        bool IsBaked() const { return (int)Baked.size() == ProbeCount() * 9; }
        void RequestBake() { BakeNext = 0; }
    };

    // Plays a Timeline asset (see Dimension/Animation/Timeline.h) on this object and the objects under it.
    struct TimelineComponent
    {
        AssetHandle Timeline = AssetHandle::None();
        bool PlayOnAwake = true;
        bool Loop = false;
        float Speed = 1.0f;
        // Runtime.
        float Time = 0.0f;
        bool Playing = false;
        bool Started = false;
        void Play() { Playing = true; Started = true; Time = 0.0f; }
        void Stop() { Playing = false; Started = true; }
    };

    struct WaterVolumeComponent
    {
        glm::vec3 Size{ 10.0f, 4.0f, 10.0f }; // centered on the object; the top face is the surface
        float Buoyancy = 1.4f;    // 1 = neutral, above floats, below sinks
        float LinearDrag = 0.5f;
        float AngularDrag = 0.05f;
        glm::vec3 Flow{ 0.0f };   // current, m/s
        // Waves: three Gerstner waves (the main one of Wave Height and Wave Length along Wave Direction, two smaller
        // ones turned to either side) that move at deep-water speed. Floating bodies ride them and drift with their
        // orbital motion; with a Mesh Renderer the surface draws as a grid of Surface Resolution following them.
        float WaveHeight = 0.0f;     // crest to trough of the main wave, m (0 = flat)
        float WaveLength = 8.0f;     // m
        float WaveDirection = 0.0f;  // degrees around Y from +X
        float Steepness = 0.5f;      // 0 = round sine waves, 1 = sharp crests
        int32_t SurfaceResolution = 64;
        // Under water: surfaces below the surface catch rippling caustics (the sunlight focused by the waves, Caustics
        // strength, Caustics Scale meters across), and with the camera below the surface (or looking down into the
        // water) everything is seen through Underwater Color murk, thicker by Underwater Fog per meter.
        glm::vec4 UnderwaterColor{ 0.05f, 0.32f, 0.38f, 1.0f };
        float UnderwaterFog = 0.12f;
        float Caustics = 1.0f;
        float CausticsScale = 1.5f;
        // Runtime: the surface grid's displaced vertices and normals (object space) this frame.
        std::vector<glm::vec3> SimulatedPositions, SimulatedNormals;
    };

    // A Wind Zone (like Unity's): Directional blows along the object's forward (-Z) everywhere; Spherical blows out
    // from the object, strongest at its center and fading to nothing at Radius. Main is the wind speed (m/s), stirred by
    // Turbulence (drifting swirls, a fraction of Main) and swelling into gusts by Pulse Magnitude, Pulse Frequency times
    // a second. Zones add up. The air drags rigidbodies by their size (a beach ball blows away, a crate barely moves),
    // cloth, soft bodies and ropes, and particles of CPU systems with External Forces above 0.
    enum class WindZoneMode : int32_t { Directional = 0, Spherical = 1 };
    struct WindZoneComponent
    {
        WindZoneMode Mode = WindZoneMode::Directional;
        float Radius = 20.0f;
        float Main = 5.0f;
        float Turbulence = 0.3f;
        float PulseMagnitude = 0.5f;
        float PulseFrequency = 0.2f;
    };

    struct AudioReverbZoneComponent
    {
        float MinDistance = 4.0f;
        float MaxDistance = 10.0f;
        float RoomSize = 0.8f; // 0-1
        float Damping = 0.4f;  // 0-1
        float Wet = 0.6f;      // 0-1
        std::string Group = "Master"; // mixer group whose reverb the zone drives
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

    enum class ClothPin : int32_t { TopEdge = 0, TopCorners = 1, None = 2 };

    // A sheet of cloth (flags, curtains, banners) simulated as a Jolt soft body while playing. Needs a Mesh Renderer for
    // its material (the mesh itself is the cloth's grid: Size in the object's XY plane, top edge at +Y). Pinned vertices
    // follow the object, so moving it drags the cloth; the rest hang, collide with 3D colliders and blow in the Wind.
    struct ClothComponent
    {
        glm::vec2 Size{ 2.0f, 2.0f };
        int32_t Resolution = 24;      // quads per side
        ClothPin Pin = ClothPin::TopEdge;
        float Mass = 1.0f;            // total, kg
        float StretchCompliance = 0.0001f; // edge springs: 0 = no stretch, larger = stretchier
        float BendCompliance = 0.05f;      // bending: larger = floppier
        float Damping = 0.2f;
        float Friction = 0.5f;
        int32_t Iterations = 8;       // solver iterations per step (more = stiffer)
        float Thickness = 0.02f;      // collision radius of the vertices
        glm::vec3 Wind{ 0.0f };       // world-space acceleration on the free vertices (m/s^2)
        // Tearing: an edge stretched past (1 + Tear Stretch) times its length breaks and the faces on it come off.
        // 0 = never tears.
        float TearStretch = 0.0f;
        // Attach To: the pinned vertices hold on to this object instead of the cloth's own (a cape on a character, a
        // sail on a boat), and the cloth's pull (its weight, swing and wind load) acts back on its Rigidbody.
        Entity AttachTo = Entity::Null();

        // Runtime: the simulated grid in the object's space (empty = the flat rest shape), and once it has torn, the
        // triangles still attached (empty = all of them) and how many edges broke.
        std::vector<glm::vec3> SimulatedPositions, SimulatedNormals;
        std::vector<uint32_t> TornIndices;
        int32_t TornEdges = 0;
    };

    // Dents (car bodies, crates, cans): a hit faster than Min Speed pushes the Mesh Renderer's surface in around the
    // contact point, Strength centimeters per m/s over Min Speed, spread over Radius and never deeper than Max Dent in
    // all. The shading follows the dents, and so does a Mesh Collider (other colliders keep their shape). Scripts can
    // Repair it (Physics3DWorld::Dent dents it by hand).
    struct DeformableComponent
    {
        float MinSpeed = 3.0f;  // m/s
        float Strength = 1.0f;  // centimeters per m/s
        float Radius = 0.5f;    // meters (world)
        float MaxDent = 0.25f;  // meters (world)
        // Runtime: the dented vertices in the object's space (empty = undented) and how far each has moved.
        std::vector<glm::vec3> DentedPositions, DentedNormals;
        std::vector<float> DentDepth;
        void Repair() { DentedPositions.clear(); DentedNormals.clear(); DentDepth.clear(); }
    };

    // A boat (on a dynamic Rigidbody floating in a Water Volume): its engine pushes it forward (-Z, as vehicles) while the
    // Propeller point is under water, the rudder turns it (more the faster it goes through the water, a little at rest), and
    // its keel resists sliding sideways so it carves turns. Throttle and Steering (-1..1) come from scripts, or from WASD
    // / arrows / the left stick with Drive With Input.
    struct BoatComponent
    {
        float Throttle = 0.0f;
        float Steering = 0.0f;
        float EngineForce = 4000.0f;      // newtons at full throttle (reverse: half)
        float TurnRate = 45.0f;           // degrees per second at full steering and speed
        float TurnSpeed = 4.0f;           // m/s at which the rudder reaches full effect
        float Keel = 1.5f;                // sideways drag, per second (times the mass)
        glm::vec3 Propeller{ 0.0f, -0.4f, 2.0f }; // the engine works while this point (the stern, object's space) is under water
        bool DriveWithInput = false;
        // Runtime.
        bool PropellerInWater = false;
    };

    // A rope (see Dimension/Physics/Rope.h): Segments + 1 points simulated as a soft body while playing, drawn as a
    // tube of Radius with the object's Mesh Renderer material. Its start hangs from the object (with Pin Start) and
    // it falls along the object's -Y; with Attach To its end holds on to that object at Attach Offset (its space), and
    // a dynamic body there feels the rope's pull. It collides with 3D colliders. It snaps (in two, at its most stretched
    // segment) when it carries more than Break Force newtons (a weight hanging on it, or its pull on a held body) or is
    // pulled over Break Stretch (a fraction) longer than Length; 0 = never.
    struct RopeComponent
    {
        float Length = 3.0f;
        int32_t Segments = 24;
        float Radius = 0.03f;
        float Mass = 1.0f;                  // total, kg
        float StretchCompliance = 0.00001f; // 0 = does not stretch
        float BendCompliance = 0.5f;        // larger = floppier (a chain), smaller = stiffer (a cable)
        float Damping = 0.1f;
        float Friction = 0.5f;
        int32_t Iterations = 12;
        bool PinStart = true;
        Entity AttachTo = Entity::Null();
        glm::vec3 AttachOffset{ 0.0f };
        float BreakForce = 0.0f;
        float BreakStretch = 0.0f;
        // Runtime: the simulated points in the object's space (empty = the starting shape), the load it carries
        // (newtons) and the segment it snapped at (-1 = whole).
        std::vector<glm::vec3> SimulatedPoints;
        float Tension = 0.0f;
        int32_t BrokenAt = -1;
    };

    // A ragdoll for a skinned character (Animator + skinned Mesh Renderer), like Unity's ragdoll: every bone at least
    // Min Bone Length long (a fraction of the skeleton's height) becomes a capsule, and each joint a swing-twist
    // constraint limited to Swing Limit and Twist Limit; shorter bones (fingers, toes) ride along with their parent's
    // capsule. While Simulate is off the capsules follow the animation (kinematic: they push things and are hit by
    // raycasts); with Simulate on the character goes limp under physics and the object follows the root capsule.
    struct RagdollComponent
    {
        bool Simulate = false;
        float Mass = 60.0f;           // whole body, shared by the capsules' volumes
        float Thickness = 0.25f;      // capsule radius as a fraction of the bone's length
        float MinBoneLength = 0.08f;  // fraction of the skeleton's height
        float SwingLimit = 45.0f;     // degrees
        float TwistLimit = 20.0f;     // degrees
        float Friction = 0.6f;
        float LinearDamping = 0.05f;
        float AngularDamping = 0.3f;
        // Getting up: when Simulate turns off, the animation blends in from the limp pose over Get Up Time (0 = snap),
        // and with Align On Get Up the object first moves under the body onto the ground and turns along it (toward the
        // head when lying face down, toward the feet face up), so a get-up animation starts where the body lies.
        float GetUpTime = 0.6f;
        bool AlignOnGetUp = true;
        // Partial ragdoll: a bone's name (an arm hit, a tail, a hanging limb). Simulate then makes only that bone and the
        // ones under it go limp, hanging from capsules that keep following the animation, while the rest animates on
        // and the object stays where its animation puts it. Empty = the whole body.
        std::string PartialRoot;
        // Runtime: the joints' simulated model-space transforms (they drive the skin while Driving) and the capsules.
        std::vector<glm::mat4> Pose;
        bool Driving = false;
        bool FaceUp = false;              // while simulating: lying on its back (the model's +Z faces up)
        std::vector<uint8_t> Limp;        // partial ragdolls while simulating: 1 per limp joint (empty = all of them)
        std::vector<glm::mat4> BlendFrom; // getting up: the limp pose (model space) the animation blends in from
        float BlendTime = 0.0f, BlendDuration = 0.0f;
        float GetUpProgress() const { return BlendDuration > 0.0f ? std::clamp(BlendTime / BlendDuration, 0.0f, 1.0f) : 1.0f; }
        struct Capsule { glm::mat4 World; float Radius, HalfHeight; };
        std::vector<Capsule> Capsules; // for gizmos and tests
    };

    // Breaks into pieces (see Dimension/Physics/Fracture.h) when something hits it at Break Speed or faster (along the
    // contact normal), or when a script breaks it: the object goes and its pieces fly apart from the hit as convex
    // rigid bodies sharing its mass, with its Mesh Renderer's materials outside and Inside Material on the cut faces.
    struct BreakableComponent
    {
        int32_t Pieces = 8;
        int32_t Seed = 1;                // the same seed breaks the same way
        float BreakSpeed = 6.0f;         // m/s; 0 = only scripts break it
        AssetHandle InsideMaterial = AssetHandle::None(); // the cut faces; None = the outside's material
        float ExplosionSpeed = 1.5f;     // pieces fly apart from the break point at up to this speed
        float DebrisLifetime = 8.0f;     // seconds before pieces shrink away (over the last second); 0 = they stay
        // Impact Focus: the share of pieces cut small around the break point (0 = spread evenly, and the pieces are cut
        // once and shared by every copy), within Focus Radius (a fraction of the object's size).
        float ImpactFocus = 0.0f;
        float FocusRadius = 0.25f;
        // Destructible structures: with Glue Strength above 0 the pieces stay stuck to their neighbours (and, when the
        // object was not a dynamic body, its bottom pieces to the world) until a force above Glue Strength (N) pulls
        // a joint apart, so a wall cracks where it is hit and crumbles as it is loaded, instead of falling apart at once.
        float GlueStrength = 0.0f;
        float Density = 1500.0f; // kg per m3 of the pieces when the object was not a dynamic body (else its mass is shared)
    };

    // A soft, deformable version of the object's mesh (Mesh Renderer), simulated as a Jolt soft body while playing:
    // balloons, jelly, cushions. Pressure inflates closed meshes; without it the shape is held by its edges and bends.
    // Vertices at the same position (UV seams) are welded so the surface stays closed. The body is its own collider
    // (other colliders on the object are ignored), and the object's position follows it.
    struct SoftBodyComponent
    {
        float Mass = 1.0f;                 // total, kg
        float Pressure = 20.0f;            // inflation of a closed mesh (0 = none)
        float StretchCompliance = 0.0001f; // edges: larger = stretchier
        float BendCompliance = 0.01f;      // larger = loses its shape more easily
        float Damping = 0.1f;
        float Friction = 0.5f;
        float Restitution = 0.0f;
        int32_t Iterations = 8;
        float Thickness = 0.02f;

        // Runtime: the simulated mesh vertices in the object's space (empty = the mesh as it is).
        std::vector<glm::vec3> SimulatedPositions, SimulatedNormals;
    };

    enum class VehicleDrive : int32_t { RearWheel = 0, FrontWheel = 1, AllWheel = 2, Unpowered = 3 }; // Unpowered: trailers
    // Car: four wheels. Motorcycle: two on the center line that lean into turns. Tank: tracks (Track Wheels per side)
    // steered by driving the sides at different speeds, turning on the spot when standing.
    enum class VehicleType : int32_t { Car = 0, Motorcycle = 1, Tank = 2 };

    // A four-wheeled car on Jolt's wheeled vehicle (engine, automatic gearbox, differentials, raycast-free cylinder
    // wheel contacts), like Unity's WheelColliders on a car body. Needs a dynamic Rigidbody and a collider for the body
    // on the same object; the car faces -Z (forward). Scripts drive it through Throttle, Steering, Brake and Hand Brake.
    struct VehicleComponent
    {
        VehicleType Type = VehicleType::Car;
        int32_t TrackWheels = 5;     // tanks: road wheels per track
        glm::vec3 CenterOfMassOffset{ 0.0f, -0.3f, 0.0f }; // from the collider's center (lower = harder to tip over)
        float WheelBase = 2.6f;      // distance between the front and rear axles
        float TrackWidth = 1.6f;     // distance between the left and right wheels
        float WheelHeight = -0.1f;   // height of the suspension mounts (body space)
        float WheelRadius = 0.36f;
        float WheelWidth = 0.25f;
        float SuspensionTravel = 0.3f; // metres
        float SuspensionFrequency = 1.5f; // Hz (stiffness)
        float SuspensionDamping = 0.5f;
        float MaxSteerAngle = 30.0f;   // degrees, front wheels
        VehicleDrive Drive = VehicleDrive::RearWheel;
        float EngineTorque = 500.0f;   // newton metres
        float MaxRPM = 6000.0f;
        float BrakeTorque = 1500.0f;
        float HandBrakeTorque = 4000.0f; // rear wheels
        // Optional wheel visuals (front left, front right, rear left, rear right): placed and spun to follow the wheels.
        // A cylinder's axis (Y) becomes the axle.
        Entity WheelFrontLeft = Entity::Null(), WheelFrontRight = Entity::Null(), WheelRearLeft = Entity::Null(), WheelRearRight = Entity::Null();

        // Driver input (scripts): Throttle -1..1 (negative reverses; against the motion it brakes), Steering -1..1
        // (positive = right), Brake and Hand Brake 0..1.
        float Throttle = 0.0f, Steering = 0.0f, Brake = 0.0f, HandBrake = 0.0f;
        // Runtime state.
        float SpeedKmh = 0.0f; // along the car's forward (negative when reversing)
        float EngineRPM = 0.0f;
        int32_t Gear = 0;      // 0 = neutral, negative = reverse
        bool WheelGrounded[4] = { false, false, false, false };
    };

    // Towing: this object (a trailer, a caravan, a broken-down car) hangs on Towed By by a ball joint between the Hitch
    // (a point on the tower, its space; a car's rear is +Z) and the Coupling (a point on this object, its space), so it
    // follows and swings behind. Clearing Attached unhitches it (setting it again hitches it where it stands, snapping
    // the two points together); a pull or push over Break Force (newtons, 0 = never) unhitches it on its own. A trailer
    // is typically a Vehicle with Drive set to Unpowered, its wheels rolling freely.
    struct TowHitchComponent
    {
        Entity TowedBy = Entity::Null();
        glm::vec3 Hitch{ 0.0f, -0.2f, 2.3f };
        glm::vec3 Coupling{ 0.0f, -0.2f, -2.0f };
        bool Attached = true;
        float BreakForce = 0.0f;
        float CurrentForce = 0.0f; // runtime: the joint's load, newtons
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
        // Convex shapes (and dynamic bodies): split a concave mesh into up to Max Parts convex pieces (V-HACD) instead of
        // one hull, so a ring can slide down a pole and a bowl can hold things.
        bool ConvexDecomposition = false;
        int32_t MaxParts = 16;
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

    enum class JointType : int32_t { Fixed = 0, Hinge = 1, Point = 2, Distance = 3, Slider = 4 };

    // Connects this entity's Rigidbody to another Rigidbody, or to the world when Connected Body is empty.
    struct JointComponent
    {
        JointType Type = JointType::Hinge;
        Entity ConnectedBody = Entity::Null();
        glm::vec3 Anchor{ 0.0f };          // pivot in this entity's local space
        glm::vec3 Axis{ 0.0f, 0.0f, 1.0f }; // hinge or slider axis in local space
        glm::vec3 ConnectedAnchor{ 0.0f }; // distance joint: point on the connected body (local), or world point
        bool UseLimits = false;
        float MinAngle = -45.0f;           // hinge limits, degrees
        float MaxAngle = 45.0f;
        float MinDistance = 0.0f;          // distance joint; 0 = the initial distance
        float MaxDistance = 0.0f;
        float SliderMin = -1.0f;           // slider limits: travel along Axis from the start position (units)
        float SliderMax = 1.0f;
        float SpringFrequency = 0.0f;      // distance joint: > 0 makes it a spring (oscillations per second); 0 = rigid
        float SpringDamping = 0.5f;        // damping ratio (0 = bouncy forever, 1 = no overshoot)
        bool EnableCollision = false;      // let the two connected bodies collide with each other
        bool UseMotor = false;             // hinge or slider: drive toward Motor Speed; distance: winch
        float MotorSpeed = 90.0f;          // hinge: degrees per second; slider / distance: units per second (changeable while playing)
        float MotorMaxTorque = 1000.0f;    // hinge: N*m; slider: maximum force, N; distance: force above which reeling in stalls
        float BreakForce = 0.0f;           // N; the joint breaks above this (0 = unbreakable)
        float BreakTorque = 0.0f;          // N*m; 0 = unbreakable

        // Runtime (not serialized)
        bool Broken = false;
        float CurrentForce = 0.0f, CurrentTorque = 0.0f; // reaction during the last physics step
        float CurrentLength = 0.0f;        // distance joint: its (maximum) length now, as a winch changes it
    };

    enum class Joint2DType : int32_t { Fixed = 0, Hinge = 1, Distance = 2, Slider = 3 };

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
        glm::vec2 Axis{ 1.0f, 0.0f };      // slider axis in local space
        float LowerTranslation = -1.0f;    // slider limits: travel along Axis from the start position (units)
        float UpperTranslation = 1.0f;
        float SpringFrequency = 0.0f;      // distance joint: > 0 makes it a spring (oscillations per second); 0 = rigid
        float SpringDamping = 0.5f;        // damping ratio (0 = bouncy forever, 1 = no overshoot)
        bool CollideConnected = false;
        bool UseMotor = false;             // hinge or slider: drive toward Motor Speed
        float MotorSpeed = 90.0f;          // hinge: degrees per second; slider: units per second (changeable while playing)
        float MotorMaxTorque = 100.0f;     // hinge: N*m; slider: maximum force, N
        float BreakForce = 0.0f;           // N; 0 = unbreakable
        float BreakTorque = 0.0f;          // N*m; 0 = unbreakable

        // Runtime (not serialized)
        bool Broken = false;
        float CurrentForce = 0.0f, CurrentTorque = 0.0f;
    };
}
