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
#include <unordered_map>
#include <string>
#include <vector>

namespace Dimension
{
    class ScriptableEntity;
    struct NavMeshData;

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
    // What a camera's image starts from: a color, the sky, or nothing (Depth Only: a camera drawn over another adds its
    // objects to the image below, a weapon or HUD model over the world).
    enum class ClearMode : int32_t { SolidColor = 0, Skybox = 1, DepthOnly = 2 };

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
        // Where on the screen it draws: x, y (from the top-left), width, height, as fractions of the screen. A camera
        // other than the primary one draws over it when its rectangle is smaller than the screen (CameraStack).
        glm::vec4 Viewport{ 0.0f, 0.0f, 1.0f, 1.0f };
        float Depth = 0.0f;       // of the cameras drawn over the primary, higher ones go on top
        int32_t CullingMask = -1; // layers it draws (bit n = layer n; -1 everything)
        // A Render Texture asset it draws into instead of the screen (at the texture's size; materials, sprites and UI
        // Images showing that texture show its view). None = the screen.
        AssetHandle TargetTexture = AssetHandle::None();
        bool ManualRender = false;    // with a Target Texture: draws only when Camera.Render() asks (snapshots)
        uint32_t RenderRequests = 0;  // runtime: bumped by Camera.Render()

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
        int32_t PaintLayer = 3;    // the terrain layer painted along it (its place in the Layers list); -1 = none
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
    // A terrain surface layer: Texture (None = plain) repeating Tiling times per meter, times Color, with an optional
    // Normal Map (tiled the same) bent by Normal Strength. Triplanar projects it from the sides as well as from above,
    // blended by which way the ground faces, so it is not stretched down cliffs (three times the texture reads).
    // With Rule it is laid out by itself where the ground's height (0-1 of the terrain's Height) and slope (degrees)
    // fall in their ranges, fading out over Height Fade and Slope Fade past them and broken into patches by Noise
    // (0-1; about Noise Scale meters across). Later layers cover earlier ones, the first is the ground under them all.
    // Trees and Grass (0-1): how thickly the terrain's tree and grass details grow on it by themselves (painting adds).
    struct TerrainLayer
    {
        std::string Name = "Layer";
        AssetHandle Texture = AssetHandle::None();
        glm::vec4 Color{ 1.0f };
        float Tiling = 0.25f;
        AssetHandle NormalMap = AssetHandle::None();
        float NormalStrength = 1.0f;
        bool Triplanar = false;
        bool Rule = false;
        glm::vec2 HeightRange{ 0.0f, 1.0f };
        glm::vec2 SlopeRange{ 0.0f, 90.0f };
        float HeightFade = 0.03f;
        float SlopeFade = 4.0f;
        float Noise = 0.0f;
        float NoiseScale = 8.0f;
        float Trees = 0.0f;
        float Grass = 0.0f;
    };
    inline std::vector<TerrainLayer> DefaultTerrainLayers()
    {
        return { { "Grass", AssetHandle::None(), { 0.36f, 0.55f, 0.24f, 1.0f }, 0.25f }, { "Rock", AssetHandle::None(), { 0.46f, 0.43f, 0.40f, 1.0f }, 0.25f },
                 { "Snow", AssetHandle::None(), { 0.95f, 0.96f, 1.0f, 1.0f }, 0.25f },   { "Dirt", AssetHandle::None(), { 0.40f, 0.30f, 0.21f, 1.0f }, 0.25f },
                 { "Sand", AssetHandle::None(), { 0.78f, 0.71f, 0.52f, 1.0f }, 0.25f },  { "Moss", AssetHandle::None(), { 0.22f, 0.31f, 0.14f, 1.0f }, 0.25f } };
    }

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
        // Surface layers, 1 to 16 of them, each its Texture (repeating Tiling times per meter) times Color. Layers with
        // a Rule lay themselves out; with none, the first three do: the first on gentle ground, the second where it is
        // steeper than Rock Slope (degrees), the third above Snow Height (0-1 of Height). Any can be painted.
        std::vector<TerrainLayer> Layers = DefaultTerrainLayers();
        float RockSlope = 35.0f;
        float SnowHeight = 0.72f;
        // Height Blend (0-1): where layers meet, the higher parts of each texture (its brightness) win, so stones
        // show through sand in clumps instead of fading evenly; 0 = plain blending by weight.
        float HeightBlend = 0.0f;
        // Sculpted and painted terrain (the editor's brushes): the heights (0-1 of Height, 16-bit) and layer weights
        // (a byte per layer per sample, at least the first three) base64, replacing the heightmap/noise and the
        // automatic layers. Empty = not edited.
        std::string SculptedHeights, PaintedLayers;
        // Holes (caves, tunnels, a well): one bit per sample, base64; triangles touching a hole sample neither render
        // nor collide. Empty = none.
        std::string PaintedHoles;
        // Details: trees (bushes, rocks) and grass tufts scattered where the detail brushes painted them, and where the
        // layers they grow on are (each layer's Trees and Grass), as instances of a mesh: Density per 100 m2 at full
        // strength, each turned at random and sized between the Min and Max Scale, only on ground flatter than Max
        // Slope. Trees can collide as upright capsules.
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
        std::vector<uint32_t> EditLayers; // RGBA8 splat maps one after the other, 4 layers each
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
        // A sprite sliced from the texture (the Sprite Editor), by name: only that part is drawn (sized to it, the
        // sheet ignored). Empty = the whole texture.
        std::string SpriteName;
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

    // What a renderer adds to the velocity buffer (motion blur, TAA), as Unity's MotionVectorGenerationMode: only the
    // camera's motion, its own movement and animation too, or none at all (it stays sharp, e.g. a first-person weapon).
    enum class MotionVectorMode : int { Camera = 0, Object, ForceNoMotion };

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
        // Multiplies the color (and alpha) of whatever it draws, its material asset's too: a per-object recolor or a
        // script's flash without changing the shared material (like a Unity MaterialPropertyBlock's color).
        glm::vec4 Tint{ 1.0f };
        MotionVectorMode MotionVectors = MotionVectorMode::Object;
        // Runtime: the copy of its material made for it alone (a script's MeshRenderer.Material), while it uses it.
        AssetHandle InstanceMaterial = AssetHandle::None();
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
    // A state's playback as scripts see it (Unity's AnimatorStateInfo): how far it has played (loops done, not wrapped),
    // seconds per loop, whether it loops, and its speed.
    struct AnimatorStateInfo
    {
        bool Valid = false;
        float NormalizedTime = 0.0f, Length = 0.0f, Speed = 1.0f;
        bool Loop = true;
        std::string Tag;
    };

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
        float NormalizedTime = 0.0f; // the current state's: loops done (1 = once through; not wrapped)
        AnimatorStateInfo Info, PreviousInfo; // for scripts, as of the last update
        float StartNormalized = -1.0f;        // without a controller: where the next clip starts (Animator.Play)
    };

    // When an Animator advances (Unity's AnimatorUpdateMode): with the game's time, with real time (it goes on while
    // the game is paused with a time scale of 0), with the physics steps taken this frame, or only by script
    // (Animator.Update).
    enum class AnimatorUpdateMode : int { Normal = 0, UnscaledTime, AnimatePhysics, Manual };
    // Whether an Animator runs while its object is off screen (Unity's AnimatorCullingMode): always, or not at all
    // (time, state machine and pose wait until the object's bounds are in the main camera's view again).
    enum class AnimatorCullingMode : int { AlwaysAnimate = 0, CullCompletely };

    // Animator.MatchTarget: between two normalized times of the base layer's current state, the object is moved so
    // that a joint reaches Position at the end (each axis by Weight: 0 leaves that axis to the animation).
    struct AnimatorMatchTarget
    {
        bool Active = false;
        glm::vec3 Position{ 0.0f }, Weight{ 1.0f };
        std::string Joint;
        float Start = 0.0f, End = 1.0f;
        int State = -1;         // the base layer state it belongs to (a change of state ends it)
        float LastTime = -1.0f; // normalized time last frame (< 0: not started)
    };

    struct AnimatorComponent
    {
        AssetHandle Controller = AssetHandle::None(); // optional state machine; when set it drives Clip/Loop
        std::string Clip;          // clip name inside the mesh asset
        float Speed = 1.0f;
        bool Loop = true;
        bool Playing = true;
        bool ApplyRootMotion = false;  // the root joint's horizontal movement moves the object (or its Character Controller)
        bool ApplyRootRotation = false; // with root motion: the root's turning (about the vertical axis) turns the object
        AnimatorUpdateMode UpdateMode = AnimatorUpdateMode::Normal;
        AnimatorCullingMode CullingMode = AnimatorCullingMode::AlwaysAnimate;
        AnimatorMatchTarget Match; // runtime
        bool Culled = false;       // runtime: off screen and not animated this frame
        std::string RootMotionJoint;   // empty = the skeleton's first root joint
        // Animation Source: another model whose clips this one plays, retargeted onto its own skeleton by joint name
        // (None = the clips of its own mesh).
        AssetHandle ClipSource = AssetHandle::None();

        // Root motion (runtime): movement applied this frame, world space.
        glm::vec3 RootMotionDelta{ 0.0f };
        glm::quat RootMotionRotation{ 1.0f, 0.0f, 0.0f, 0.0f }; // this frame's turn (world space)
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
        float NormalizedTime = 0.0f; // the current state's (or clip's): loops done (1 = once through)
        AnimatorStateInfo Info, PreviousInfo; // the state playing and the one fading out (Valid only while fading)
        float StartNormalized = -1.0f;        // without a controller: where the next clip starts (Animator.Play)
        bool ScriptRootMotion = false;        // a script's OnAnimatorMove takes the root motion (RootMotionDelta)
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

    // Screen Space: over the camera's image, laid out on the screen. World Space: a panel on its object in the 3D world
    // (Pixel Size pixels, Pixels Per Unit of them a meter, centered on the object facing its +Z), drawn with depth and
    // clicked through the main camera's pointer ray.
    enum class CanvasRenderMode : int32_t { ScreenSpace = 0, WorldSpace = 1 };

    struct CanvasComponent
    {
        CanvasScaleMode ScaleMode = CanvasScaleMode::ScaleWithScreenSize;
        glm::vec2 ReferenceResolution{ 1920.0f, 1080.0f };
        int32_t SortingOrder = 0;
        CanvasRenderMode RenderMode = CanvasRenderMode::ScreenSpace;
        glm::vec2 PixelSize{ 800.0f, 600.0f }; // World Space: the panel's layout size in pixels
        float PixelsPerUnit = 400.0f;          // World Space: pixels per meter (800 x 600 is 2 x 1.5 m)
        glm::vec2 WorldSize() const { return PixelSize / std::max(PixelsPerUnit, 1e-3f); }
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
        int PointerId = 0; // runtime: the pointer holding it (0 the mouse, 1 + n the script ray n)
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
        int PointerId = 0; // runtime: the pointer holding it (0 the mouse, 1 + n the script ray n)
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
    enum class UIJustify : int32_t { Start = 0, Center = 1, End = 2, SpaceBetween = 3 };
    enum class UIAlign : int32_t { Start = 0, Center = 1, End = 2 };

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
        // Rows and columns, like flexbox: Wrap flows children onto new lines when one is full; Justify places each
        // line's children along it (with the leftover length); Align places them across their line.
        bool Wrap = false;
        UIJustify Justify = UIJustify::Start;
        UIAlign Align = UIAlign::Start;
    };

    // How a UI Rect sits in its parent's UI Layout Group (like Unity's Layout Element): Min / Preferred Size override
    // its size there (-1: unset, its Size or content is used), Flexible Size shares out the group's leftover length
    // along its axis by weight, and Ignore Layout leaves it where its own anchors put it.
    struct UILayoutElementComponent
    {
        bool IgnoreLayout = false;
        glm::vec2 MinSize{ -1.0f };
        glm::vec2 PreferredSize{ -1.0f };
        glm::vec2 FlexibleSize{ 0.0f };
    };

    enum class UIFitMode : int32_t { Unconstrained = 0, PreferredSize = 1 };

    // Sizes its UI Rect to its content (like Unity's Content Size Fitter): a UI Text's lines (wrapped at the rect's
    // width when only the height fits), a layout group's children, or a Layout Element's preferred size.
    struct UIContentSizeFitterComponent
    {
        UIFitMode Horizontal = UIFitMode::Unconstrained;
        UIFitMode Vertical = UIFitMode::Unconstrained;
    };

    // Takes component values from a UI Style Sheet (.uistyle, like UI Toolkit's USS) by class: each class lists
    // components and field values ({"classes": {"title": {"UI Text": {"Font Size": 40}}}}), applied in the order
    // named here (later classes win) when the classes, the sheet or the element's state change. "types" rules apply
    // to styled elements having that component; "name:hover" / ":pressed" / ":selected" / ":disabled" rules apply
    // in that state; a rule's "transition" (seconds) eases the numeric fields it sets, both ways. Sheet: this one's,
    // else the nearest ancestor UI Style's.
    struct UIStyleComponent
    {
        AssetHandle Sheet = AssetHandle::None();
        std::string Classes; // space-separated
        // Runtime: what was applied last.
        std::string AppliedClasses;
        AssetHandle AppliedSheet = AssetHandle::None();
        int64_t AppliedVersion = -1;
        uint32_t AppliedStates = 0;
        nlohmann::json Base = nlohmann::json::object();        // values rules changed, from before any rule
        nlohmann::json Transitions = nlohmann::json::object(); // seconds per field set by a rule with a transition
        nlohmann::json Anims = nlohmann::json::array();        // fields easing: component, field, from, to, start, seconds
    };

    // Binds this element's widget to a script field (like UI Toolkit's runtime bindings): a UI Text shows it (Format:
    // "Score: {0}", "{0:F1}" for one decimal), and a slider, toggle, input field or dropdown shows it and, Two Way,
    // writes the player's changes back. Path: "Script.Field" (or "Field": the first script having it), on Source, or
    // on this object or the nearest parent with that script.
    struct UIBindingComponent
    {
        Entity Source = Entity::Null();
        std::string Path;
        std::string Format;
        bool TwoWay = true;
        // Runtime: the field's value last shown, and the widget's value then.
        nlohmann::json LastValue;
        nlohmann::json LastWidget;
    };

    // A scroll view that shows a list from a script field (an array) as rows (like a UI Toolkit ListView): Row
    // Template (an element, usually inactive) is cloned for the rows in view only, Row Height plus Spacing apart, and
    // each row's UI Bindings read "item" (or "item.Field"), "index" and "number" (index + 1). Source / Path as UI Binding's.
    struct UIListViewComponent
    {
        Entity RowTemplate = Entity::Null();
        float RowHeight = 40.0f;
        float Spacing = 4.0f;
        Entity Source = Entity::Null();
        std::string Path;
        // Runtime: the list, the rows' parent and the row pool.
        nlohmann::json Items = nlohmann::json::array();
        Entity Content = Entity::Null();
        std::vector<Entity> Rows;
    };

    // Runtime: a list view's row and the item it shows.
    struct UIListRowComponent
    {
        Entity List = Entity::Null();
        int32_t Index = -1;
        nlohmann::json Item;
    };

    enum class UIMaskShape : int32_t { Rect = 0, Image = 1 };

    // Clips its children (like Unity's Mask and RectMask2D): to its rect, or to its UI Image's shape (the texture's
    // alpha), with edges faded over Softness pixels (left/right, top/bottom). Show Mask Graphic off hides its own image.
    struct UIMaskComponent
    {
        UIMaskShape Shape = UIMaskShape::Rect;
        glm::vec2 Softness{ 0.0f };
        bool ShowMaskGraphic = true;
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
        int PointerId = 0; // runtime: the pointer holding it (0 the mouse, 1 + n the script ray n)
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
        // A string table key: when set, the text shown is its text in the current language (Localization), not Text.
        std::string LocalizedKey;
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
        float FadeDuration = 0.1f; // seconds the tint takes between states (Unity's Color Tint fade)
        bool Hovered = false, Pressed = false; // runtime state
        int PointerId = 0; // runtime: the pointer holding it (0 the mouse, 1 + n the script ray n)
        // Runtime: the tint shown, fading from TintFrom to TintTarget (x < 0: not started; the state's color is used).
        glm::vec4 Tint{ -1.0f }, TintFrom{ -1.0f }, TintTarget{ -1.0f };
        float TintTime = 0.0f;
    };

    // Captures the surroundings at its position; meshes inside its box reflect that instead of the sky (Unity's
    // Reflection Probe). Baked when first rendered, when moved or resized, on request, or every frame (Realtime).
    // A Post Process Volume (Unity's Volume with its overrides): post-processing for the cameras inside its box (or all,
    // when Global), blended over the scene's settings (see Dimension/Renderer/PostProcess.h).
    struct PostProcessVolumeComponent
    {
        bool Global = false;
        glm::vec3 Size{ 10.0f };    // the box, centered on the object (its space)
        float BlendDistance = 2.0f; // fades in over this distance outside the box
        float Weight = 1.0f;
        float Priority = 0.0f;      // higher ones blend over lower ones
        bool OverrideExposure = false;
        float Exposure = 1.0f;
        bool OverrideBloom = false;
        float BloomIntensity = 1.0f;
        bool OverrideVignette = false;
        float Vignette = 0.3f;
        bool OverrideSaturation = false;
        float Saturation = 1.0f;
        bool OverrideContrast = false;
        float Contrast = 1.0f;
        bool OverrideColorFilter = false;
        glm::vec3 ColorFilter{ 1.0f }; // sRGB as picked; multiplies the image
        bool OverrideWhiteBalance = false;
        float Temperature = 0.0f, Tint = 0.0f; // -1..1
        bool OverrideLiftGammaGain = false;
        glm::vec3 Lift{ 0.0f }, Gamma{ 1.0f }, Gain{ 1.0f };
        bool OverrideLookup = false;
        AssetHandle LookupTexture = AssetHandle::None();
        float LookupContribution = 1.0f;
        bool OverrideChromaticAberration = false;
        float ChromaticAberration = 0.5f;
        bool OverrideFilmGrain = false;
        float FilmGrain = 0.3f;
        bool OverrideLensDistortion = false;
        float LensDistortion = 0.3f;
        bool OverrideMotionBlur = false;
        float MotionBlur = 0.5f; // the shutter (0 turns it off there)
    };

    struct ReflectionProbeComponent
    {
        glm::vec3 Size{ 10.0f, 5.0f, 10.0f }; // influence box, centered on the probe
        int32_t Resolution = 1;               // 0: 64, 1: 128, 2: 256, 3: 512 pixels per cube face
        float Intensity = 1.0f;
        bool BoxProjection = true;            // parallax-correct reflections against the box (rooms, courtyards)
        float BlendDistance = 1.0f;           // fades out over this distance inside the box walls (blends with neighbors)
        bool Realtime = false;                // re-capture every frame (six extra scene renders)
        uint32_t RenderRequests = 0;          // runtime: bumped by ReflectionProbe.RenderProbe() to capture again once
    };

    // ---- Audio -------------------------------------------------------------------

    // How a spatial source fades with distance (Unity's AudioRolloffMode): full volume inside Min Distance, then as
    // 1/distance (Logarithmic, the natural fall-off), or straight down to silence at Max Distance (Linear).
    enum class AudioRolloff : int32_t { Logarithmic = 0, Linear = 1 };

    // How a Video Player shows its pictures (Unity's VideoRenderMode): on its own object (Material Override: its UI Image,
    // Sprite Renderer or Mesh Renderer's material), only for scripts (VideoPlayer.Texture), or copied into a Render
    // Texture (Target Texture, scaled to its size) that anything can show.
    enum class VideoRenderMode : int { MaterialOverride = 0, APIOnly, RenderTexture };

    // Plays a video file (MP4, MOV, WMV...) while the scene plays, with its sound (see Video::Update).
    struct VideoPlayerComponent
    {
        AssetHandle Clip = AssetHandle::None();
        bool PlayOnAwake = true;
        bool Loop = true;
        float PlaybackSpeed = 1.0f;
        VideoRenderMode RenderMode = VideoRenderMode::MaterialOverride;
        AssetHandle TargetTexture = AssetHandle::None(); // Render Texture mode: the render texture it plays into
        float Emission = 0.0f;   // Material Override on a Mesh Renderer: the picture glows this much (a screen in the dark)
        bool PlayAudio = true;   // the clip's sound, through the audio engine
        float Volume = 1.0f;
        std::string Output;      // Audio Mixer group; empty = Master
    };

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
        AudioRolloff Rolloff = AudioRolloff::Logarithmic;
        // Pitch shift from the source's and the listener's motion (0 none, 1 natural; spatial sources).
        float DopplerLevel = 1.0f;
        float OcclusionAmount = 0.0f;  // runtime: 0 = clear line, 1 = fully occluded
        bool Paused = false;           // runtime: stopped by Pause (UnPause goes on from where it was)
        std::string RoutedOutput;      // runtime: the group its sound goes into (Output changed while playing: rerouted)
        glm::vec3 LastPosition{ 0.0f }; // runtime: last update's position (its velocity, for the Doppler shift)
        bool HasLastPosition = false;
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
        bool ResetOnEnd = false; // without Loop: at the end it stops and goes back to 0 (Unity's wrap mode None)
        float Speed = 1.0f;
        // Runtime.
        float Time = 0.0f;
        bool Playing = false;
        bool Started = false;
        bool Paused = false;          // Pause(): held where it is until Resume()
        std::vector<Entity> Bindings; // per track of its timeline: the object it drives instead of its path (Null: by path)
        void Play() { Playing = true; Started = true; Paused = false; Time = 0.0f; }
        void Stop() { Playing = false; Started = true; Paused = false; }
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
        // Ocean: the water reaches out to any distance across (Size.y deep under the top face), and its surface draws
        // as rings around the camera out to the camera's far plane, the waves fading into calm water in the distance.
        bool Ocean = false;
        // Foam: white froth where the waves crest (Foam: how much; steeper waves make more) and, while playing, along
        // shores and anything standing in the water: where the ground is less than Shore Foam meters below the surface.
        float Foam = 0.6f;
        float ShoreFoam = 1.0f;
        // Under water: surfaces below the surface catch rippling caustics (the sunlight focused by the waves, Caustics
        // strength, Caustics Scale meters across), and with the camera below the surface (or looking down into the
        // water) everything is seen through Underwater Color murk, thicker by Underwater Fog per meter.
        glm::vec4 UnderwaterColor{ 0.05f, 0.32f, 0.38f, 1.0f };
        float UnderwaterFog = 0.12f;
        float Caustics = 1.0f;
        float CausticsScale = 1.5f;
        // Runtime: the surface grid's displaced vertices and normals (object space) this frame; the normals' length is
        // 1 + foam. Depths to the ground under the grid's points (and, for an ocean, under 1 m cells as they are seen).
        std::vector<glm::vec3> SimulatedPositions, SimulatedNormals;
        std::vector<float> ShoreDepths;
        std::unordered_map<int64_t, std::vector<float>> ShoreTiles; // 32 x 32 cells of 1 m each (NaN = not measured yet)
        // The last tile looked up (valid only for the component that set it: a copy looks again).
        const void* ShoreLastOwner = nullptr;
        int64_t ShoreLastKey = 0;
        std::vector<float>* ShoreLastTile = nullptr;
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

    // ---- Navigation ------------------------------------------------------------

    // A NavMesh Surface (like Unity's): bakes where agents can walk inside a box Size meters across around the object
    // (not turned with it) from the colliders there. Only static geometry counts (no dynamic rigidbodies, triggers or
    // characters). A spot is walkable when its ground is no steeper than Max Slope, Agent Height of headroom is above
    // it, and it is at least Agent Radius from any edge or wall; neighbouring spots up to Step Height apart join up.
    // Baked when play starts (and with the Inspector's Bake to preview it, or NavMesh.Build from scripts). A Moving
    // surface rides on its object (a boat's deck, a lift): it is baked from that object's own colliders and its
    // children's only (moving bodies too), in the pose it has then, and goes wherever the object goes; agents on it are
    // carried along (turning with it) and walk it as if it stood still. Other surfaces leave Moving surfaces' objects out.
    struct NavMeshSurfaceComponent
    {
        glm::vec3 Size{ 50.0f, 20.0f, 50.0f };
        float CellSize = 0.25f; // meters per sample
        float AgentRadius = 0.5f;
        float AgentHeight = 2.0f;
        float MaxSlope = 45.0f; // degrees
        float StepHeight = 0.4f;
        bool ShowNavMesh = true; // the baked mesh in the Scene view while the object is selected
        // Generate Links: off-mesh links of its own, down ledges up to Drop Height (one way) and across gaps up to Jump
        // Distance wide (both ways).
        bool GenerateLinks = false;
        float DropHeight = 2.0f;
        float JumpDistance = 1.5f;
        bool Moving = false;
        std::shared_ptr<const NavMeshData> Data;  // runtime: the mesh agents walk (the baked one with obstacles' holes)
        std::shared_ptr<const NavMeshData> Baked; // runtime: as baked
        uint64_t CarveKey = 0;                    // runtime: which obstacles cut Data, and where
        // Runtime (Moving): its object's transform when baked, and from the mesh as baked to the world now (and back).
        glm::mat4 BakedAt{ 1.0f }, Frame{ 1.0f }, FrameInverse{ 1.0f };
    };

    // A NavMesh Obstacle (like Unity's): a box or an upright capsule (Center, Size or Radius and Height, in the
    // object's space) agents keep out of. Moving, agents steer around it; with Carve it cuts a hole in the navigation
    // mesh (Agent Radius wider) that paths go around, renewed when it has moved Move Threshold since, or with Carve
    // Only Stationary only once it has stood still Time To Stationary seconds (until then it is steered around).
    enum class NavObstacleShape : int32_t { Box = 0, Capsule = 1 };
    struct NavMeshObstacleComponent
    {
        NavObstacleShape Shape = NavObstacleShape::Box;
        glm::vec3 Center{ 0.0f };
        glm::vec3 Size{ 1.0f };
        float Radius = 0.5f;
        float Height = 2.0f;
        bool Carve = false;
        float MoveThreshold = 0.1f;
        bool CarveOnlyStationary = true;
        float TimeToStationary = 0.5f;
        // Runtime
        glm::vec3 LastPosition{ 0.0f }, Velocity{ 0.0f };
        float StillTime = 0.0f;
        bool Seen = false, Carving = false;
        glm::mat4 CarvedAt{ 0.0f }; // its transform when it last cut the mesh
    };

    // A NavMesh Modifier Volume (like Unity's): the navigation mesh inside its box (Center, Size, in the object's
    // space) is of an Area: Not Walkable takes it out (agents keep Agent Radius from it), the others make walking
    // there count Cost times on paths (a road below 1, mud above; 0 = the area's cost in the project) and can be left
    // out by agents' Area Mask.
    enum class NavArea : int32_t { Walkable = 0, NotWalkable = 1, Road = 2, Grass = 3, Mud = 4, Water = 5, Area6 = 6, Area7 = 7 };
    struct NavMeshModifierVolumeComponent
    {
        glm::vec3 Center{ 0.0f };
        glm::vec3 Size{ 4.0f, 2.0f, 4.0f };
        NavArea Area = NavArea::Mud;
        float Cost = 0.0f; // 0 = the area's cost in the project (Window > Navigation)
    };

    // An Off-Mesh Link (like Unity's): a way between two points of the navigation mesh that is not walked on it (a
    // jump across a gap or down a ledge, a ladder). Start and End are in the object's space; agents route through it
    // and cross it in a jump Arc Height high (0 = straight, as on a ladder). Its length counts Cost Modifier times
    // when paths are compared. One way unless Bidirectional; off while not Activated. Its ends may lie on two
    // surfaces' meshes (a gangway from a pier onto a boat's deck): agents bound for the other surface cross by it.
    struct OffMeshLinkComponent
    {
        glm::vec3 Start{ 0.0f };
        glm::vec3 End{ 0.0f, 0.0f, 3.0f };
        bool Bidirectional = true;
        bool Activated = true;
        float ArcHeight = 0.5f;
        float CostModifier = 1.0f;
        // A door: while not Activated, agents still take paths through it and wait at its start until it is
        // (without Agents Wait a closed link is no way at all).
        bool AgentsWait = false;
        // Played on a crossing agent's Animator (a state of its controller, or a clip of its model): a jump or a climb.
        // With Match Animation the crossing takes as long as the animation; afterwards the Animator fades back to the
        // state it was in.
        std::string Animation;
        bool MatchAnimation = true;
    };

    // A NavMesh Agent (like Unity's): walks the baked navigation mesh to its destination along the shortest path,
    // at up to Speed (m/s) speeding up and slowing by Acceleration, turning by Angular Speed (degrees/s) to face the
    // way it goes, slowing to arrive (Auto Braking) Stopping Distance short. Agents steer around each other (Obstacle
    // Avoidance), giving way to ones of a lower Avoidance Priority number. With a Target it keeps following that
    // object. It moves its object (keep agents as hierarchy roots), Base Offset above the ground. Agents of any Radius
    // share a surface: they go only where it has room for them (bake with the smallest agent's radius).
    // Animation: each frame it sets its Animator's float Speed Parameter (if named) to its speed. With Root Motion (and
    // its Animator's Apply Root Motion) the animation moves it instead: the agent turns it toward the way, sets the
    // parameter to the speed it wants there, and keeps it on the mesh wherever the animation's steps take it.
    enum class NavPathStatus : int32_t { Complete = 0, Partial = 1, Invalid = 2 };
    struct NavMeshAgentComponent
    {
        float Speed = 3.5f;
        float AngularSpeed = 360.0f;
        float Acceleration = 8.0f;
        float StoppingDistance = 0.0f;
        bool AutoBraking = true;
        float Radius = 0.5f;
        float Height = 2.0f;
        float BaseOffset = 0.0f;
        bool ObstacleAvoidance = true;
        int32_t AvoidancePriority = 50; // 0 most important - 99 least
        Entity Target = Entity::Null();
        int32_t AreaMask = -1; // a bit per area it may walk (NavMesh Modifier Volume areas); -1 = all
        std::string SpeedParameter;
        bool RootMotion = false;
        // Runtime
        glm::vec3 Destination{ 0.0f };
        bool HasDestination = false, PathDirty = false, IsStopped = false;
        std::vector<glm::vec3> Path; // corners: from where the path was found to the destination
        std::vector<float> PathLinks; // per corner: -1, or the arc height of the off-mesh link from it to the next
        std::vector<Entity> PathLinkOwners; // per corner: the Off Mesh Link crossed from it (Null = walked, or the bake's)
        bool Waiting = false;               // at a closed door (Agents Wait), until it opens
        bool Queued = false;                // in line before it, at QueueSpot
        glm::vec3 QueueSpot{ 0.0f };
        size_t Corner = 0;           // the corner it is heading for
        bool Traversing = false;     // crossing an off-mesh link
        glm::vec3 TraverseFrom{ 0.0f }, TraverseTo{ 0.0f };
        float TraverseTime = 0.0f, TraverseDuration = 0.0f, TraverseArc = 0.0f;
        std::string TraverseAnimation;  // the link's animation playing while crossing ("" none)
        std::string TraverseReturnState; // the Animator state to fade back to afterwards
        bool TraverseMatch = false;     // the crossing's duration still to be taken from the animation
        NavPathStatus PathStatus = NavPathStatus::Invalid;
        glm::vec3 Velocity{ 0.0f };
        glm::vec3 DesiredVelocity{ 0.0f }; // the velocity it steers for (with Root Motion, what the animation is asked for)
        // Scripts moving it themselves (Unity's updatePosition / updateRotation): off, the agent still walks its path,
        // but only its NextPosition (world, where its object would stand) moves, and its object does not turn.
        bool UpdatePosition = true, UpdateRotation = true;
        glm::vec3 NextPosition{ 0.0f };
        bool NextValid = false; // NextPosition has been set (by a move, or a script)
        float RemainingDistance = 0.0f;
        float RepathTimer = 0.0f;
        int32_t Node = -1; // where it stands on Mesh (-1 = not placed yet)
        glm::vec3 LastGround{ 0.0f }; // where it stood after the last update
        bool Placed = false;          // LastGround is set (Warp clears it)
        // On a Moving surface the agent works in the surface's space as baked (Path, LastGround, Velocity, ...):
        // MeshDestination is Destination in the space of the mesh it lies on (DestinationMesh), taken when its path was
        // first found (so it rides along with a Moving surface too).
        glm::vec3 MeshDestination{ 0.0f };
        bool HasMeshDestination = false;
        std::shared_ptr<const NavMeshData> DestinationMesh;
        // Bound for another surface: the path ends across an off-mesh link onto CrossMesh, at CrossEnd (in the link
        // object's space).
        std::shared_ptr<const NavMeshData> CrossMesh;
        Entity CrossLink = Entity::Null();
        glm::vec3 CrossEnd{ 0.0f };
        std::shared_ptr<const NavMeshData> Mesh;
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
    // Fast bodies: checked only where they are each step (Discrete: thin walls can be skipped through), or swept along
    // their motion (Continuous: they stop at what they would pass).
    enum class CollisionDetection : int32_t { Discrete = 0, Continuous = 1 };
    // How a dynamic body's object moves between physics steps: jumping a step at a time (None), or blended from the
    // last step's pose to this one's (Interpolate: smooth at any frame rate, a step behind).
    enum class RigidbodyInterpolation : int32_t { None = 0, Interpolate = 1 };

    struct Rigidbody2DComponent
    {
        BodyType Type = BodyType::Dynamic;
        float GravityScale = 1.0f;
        float LinearDamping = 0.0f;
        float AngularDamping = 0.0f;
        bool FixedRotation = false;
        CollisionDetection Detection = CollisionDetection::Discrete; // Continuous: a bullet (swept against moving bodies too)
        RigidbodyInterpolation Interpolation = RigidbodyInterpolation::None;
        bool CanSleep = true;
        float SleepThreshold = 0.05f; // m/s it must stay under to fall asleep
        uint64_t RuntimeBody = 0; // b2BodyId packed, runtime only
        glm::vec3 PreviousPose{ 0.0f }, CurrentPose{ 0.0f }; // runtime, Interpolate: the last two steps' (x, y, angle)
        bool HasPose = false;
        // Runtime: the pose (x, y, angle) the body last left its Transform at. A kinematic body whose Transform moved
        // from there (a script) moves to it by velocity in the next step, so it pushes and carries what it meets.
        glm::vec3 SyncedPose{ 0.0f };
        bool Driven = false;
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

    // A LOD Group (like Unity's): its children are levels of detail of one object, child 0 the most detailed. It
    // shows the one that fits how tall the object looks on screen (its Size over its distance, as a fraction of the
    // view's height, times Bias): LOD 0 above LOD 1 Below, LOD 1 above LOD 2 Below, LOD 2 above LOD 3 Below, LOD 3
    // above Cull Below, and nothing below that. Size 0 measures the children's meshes. Within Fade Width (a fraction
    // of a threshold) above it, the two levels cross-fade (dithered). With Generated Levels, the group's own Mesh
    // Renderer is LOD 0 and levels 1 to 3 are made from its mesh, each keeping Reduction of the triangles before it.
    struct LODGroupComponent
    {
        float LOD1Below = 0.5f;
        float LOD2Below = 0.25f;
        float LOD3Below = 0.12f;
        float CullBelow = 0.01f;
        float Size = 0.0f;
        float Bias = 1.0f;
        float FadeWidth = 0.1f;
        int32_t GeneratedLevels = 0;
        float Reduction = 0.5f;
        // Impostor: one more level after the meshes, down to Cull Below: a flat picture of LOD 0 turned to face the
        // camera, from whichever of 8 sides it is seen (rendered once, lit as it stands; again when it changes).
        bool Impostor = false;
    };

    struct RigidbodyComponent
    {
        BodyType Type = BodyType::Dynamic;
        float Mass = 1.0f;
        float LinearDamping = 0.05f;
        float AngularDamping = 0.05f;
        bool UseGravity = true;
        // Axes a dynamic body may not move along or turn about, as Unity's RigidbodyConstraints: 2/4/8 freeze position
        // X/Y/Z, 16/32/64 freeze rotation X/Y/Z (112 all rotation).
        int32_t Constraints = 0;
        // Moves the center of mass from where its colliders put it (in the object's space): lower is steadier.
        glm::vec3 CenterOfMassOffset{ 0.0f };
        CollisionDetection Detection = CollisionDetection::Discrete;
        RigidbodyInterpolation Interpolation = RigidbodyInterpolation::None;
        float MaxAngularVelocity = 47.1239f; // rad/s it may spin at most (Jolt's default, 15 pi)
        bool CanSleep = true;
        uint32_t RuntimeBody = 0xFFFFFFFF; // JPH::BodyID, runtime only
        glm::vec3 PreviousPosition{ 0.0f }, CurrentPosition{ 0.0f }; // runtime, Interpolate: the last two steps' poses
        glm::quat PreviousRotation{ 1, 0, 0, 0 }, CurrentRotation{ 1, 0, 0, 0 };
        bool HasPose = false;
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
        // Interpolate: drawn between the last two physics steps' shapes, as far along as the time is (smooth motion
        // and motion blur at any frame rate, a step behind), as a Rigidbody's Interpolate moves its object.
        RigidbodyInterpolation Interpolation = RigidbodyInterpolation::None;

        // Runtime: the simulated grid in the object's space (empty = the flat rest shape), and once it has torn, the
        // triangles still attached (empty = all of them) and how many edges broke.
        std::vector<glm::vec3> SimulatedPositions, SimulatedNormals;
        // Runtime, Interpolate: the last two steps' shapes (SimulatedPositions is drawn between them).
        std::vector<glm::vec3> PreviousPositions, CurrentPositions, PreviousNormals, CurrentNormals;
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
        // Interpolate: drawn between the last two physics steps' shapes (and the object between their positions).
        RigidbodyInterpolation Interpolation = RigidbodyInterpolation::None;

        // Runtime: the simulated mesh vertices in the object's space (empty = the mesh as it is).
        std::vector<glm::vec3> SimulatedPositions, SimulatedNormals;
        // Runtime, Interpolate: the last two steps' shapes and object positions.
        std::vector<glm::vec3> PreviousPositions, CurrentPositions, PreviousNormals, CurrentNormals;
        glm::vec3 PreviousPosition{ 0.0f }, CurrentPosition{ 0.0f };
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
        int CollisionFlags = 0;       // CharacterCollisionFlags of the last move
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

    // A note on an object (editor comments, like a sticky note): its text in the Inspector, and with Show In Scene a
    // label at the object in the Scene view (its tooltip in the Hierarchy). Nothing in the game uses it.
    struct NoteComponent
    {
        std::string Text;
        glm::vec4 Color{ 1.0f, 0.85f, 0.35f, 1.0f };
        bool ShowInScene = true;
    };
}
