#pragma once
#include "Dimension/Renderer/Quality.h"
#include "Dimension/Core/Base.h"
#include "Dimension/Core/Guid.h"
#include <array>
#include <map>
#include "Dimension/Asset/ImportDefaults.h"
#include "Dimension/Project/InputActions.h"
#include <string>
#include "Dimension/Audio/AudioMixer.h"
#include <vector>

namespace Dimension
{
    // A game project: a folder containing Project.dproj and an Assets/ directory.
    struct ProjectConfig
    {
        std::string Name = "New Project";
        std::string Version = "1.0";   // the game's version (scripts: Application.Version)
        std::string CompanyName;       // (Application.CompanyName)
        AssetHandle StartScene = AssetHandle::None();
        uint32_t WindowWidth = 1280, WindowHeight = 720;
        std::string GameModule = "SandboxGame"; // DLL (without extension) containing scripts and systems

        // Build settings
        std::vector<AssetHandle> BuildScenes; // scenes in the build, in order; index 0 is loaded first (else StartScene)
        bool Fullscreen = false;              // borderless fullscreen on the primary monitor (Alt+Enter toggles)
        bool VSync = true;
        std::string BuildFolder;              // last output folder used by Build
        AssetHandle Icon = AssetHandle::None(); // texture used as the game's executable and window icon (None = engine icon)
        bool StripUnusedAssets = true;        // Build copies only assets the build scenes reach (plus Assets/Resources)
        bool PackAssets = false;              // Build packs the shipped assets into one Assets.pak (see AssetPack)
        bool BundleDotNet = true;             // Build ships the .NET runtime (C# runs without .NET installed)
        bool AheadOfTimeScripts = true;       // Build compiles C# to ReadyToRun code (crossgen2): no JIT at start
        bool TrimDotNet = true;               // a bundled runtime keeps only the framework assemblies scripts can reach

        // Quality (Project Settings > Quality, like Unity's): the levels (empty: Quality::DefaultLevels) and the one a
        // built game starts at (the editor keeps its own choice in its preferences).
        std::vector<QualityLevel> QualityLevels;
        int DefaultQuality = 3;

        std::vector<AudioMixerGroup> MixerGroups = AudioMixer::DefaultGroups();
        std::vector<AudioMixerSnapshot> MixerSnapshots;

        // Physics layers (like Unity's Layers + Layer Collision Matrix). Empty names are unused layers.
        static constexpr int kMaxLayers = 32;
        std::array<std::string, kMaxLayers> PhysicsLayers = MakeDefaultLayers();
        std::array<uint32_t, kMaxLayers> LayerCollisionMatrix = MakeDefaultMatrix(); // bit j of row i: i collides with j

        // Navigation (Window > Navigation): area names and the cost walking each counts by default (a NavMesh Modifier
        // Volume with Cost 0 takes its area's), and how many new paths agents may find a frame.
        static constexpr int kNavAreas = 8;
        std::array<std::string, kNavAreas> NavAreaNames{ "Walkable", "Not Walkable", "Road", "Grass", "Mud", "Water", "Area 6", "Area 7" };
        std::array<float, kNavAreas> NavAreaCosts{ 1.0f, 1.0f, 0.5f, 1.5f, 4.0f, 6.0f, 1.0f, 1.0f };
        int NavPathBudget = 16;
        // Navigation agent types (like Unity's): a surface of a type bakes with its sizes, and an agent of a type walks
        // only surfaces of that type (or of none). Type 0 in components is "Any"; type i is NavAgentTypes[i - 1].
        struct NavAgentType
        {
            std::string Name = "Humanoid";
            float Radius = 0.5f, Height = 2.0f, StepHeight = 0.4f, MaxSlope = 45.0f;
        };
        std::vector<NavAgentType> NavAgentTypes{ NavAgentType{} };

        // Presets: per component type (registry name), the preset applied to it when added in the editor.
        std::map<std::string, AssetHandle> DefaultPresets;
        // Import defaults per folder (ImportDefaults): the settings new assets of a type get there.
        std::vector<ImportRule> ImportRules;
        // Localization: the language for players whose system language no string table has, and for texts missing in
        // a language (a language code; "" = the tables' first column).
        std::string DefaultLanguage;
        // Named input actions (Project Settings > Input); InputActionsSet false: the built-in defaults apply.
        std::vector<InputAction> InputActions;
        bool InputActionsSet = false;

        bool LayersCollide(int a, int b) const
        {
            return a >= 0 && b >= 0 && a < kMaxLayers && b < kMaxLayers && ((LayerCollisionMatrix[a] >> b) & 1u);
        }
        void SetLayersCollide(int a, int b, bool collide) // keeps the matrix symmetric
        {
            if (a < 0 || b < 0 || a >= kMaxLayers || b >= kMaxLayers) return;
            if (collide) { LayerCollisionMatrix[a] |= 1u << b; LayerCollisionMatrix[b] |= 1u << a; }
            else { LayerCollisionMatrix[a] &= ~(1u << b); LayerCollisionMatrix[b] &= ~(1u << a); }
        }

        static std::array<std::string, kMaxLayers> MakeDefaultLayers()
        {
            std::array<std::string, kMaxLayers> names{};
            names[0] = "Default";
            names[1] = "Ignore Raycast";
            return names;
        }
        static std::array<uint32_t, kMaxLayers> MakeDefaultMatrix()
        {
            std::array<uint32_t, kMaxLayers> m{};
            m.fill(0xFFFFFFFFu);
            return m;
        }
    };

    class Project
    {
    public:
        static constexpr const char* kFileName = "Project.dproj";

        // Loads (or creates) the project in `directory` and initializes the asset database.
        static bool Open(const fs::path& directory);
        static bool Save();
        static bool IsOpen();

        static ProjectConfig& GetConfig();
        static const fs::path& GetDirectory();
        // The project's Assets folder; a shipped game with Assets.pak instead uses its unpacked copy.
        static fs::path GetAssetsDirectory();
    };
}
