#pragma once
#include "Dimension/Core/Base.h"
#include "Dimension/Core/Guid.h"
#include <array>
#include "Dimension/Audio/AudioMixer.h"
#include <vector>

namespace Dimension
{
    // A game project: a folder containing Project.dproj and an Assets/ directory.
    struct ProjectConfig
    {
        std::string Name = "New Project";
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

        std::vector<AudioMixerGroup> MixerGroups = AudioMixer::DefaultGroups();
        std::vector<AudioMixerSnapshot> MixerSnapshots;

        // Physics layers (like Unity's Layers + Layer Collision Matrix). Empty names are unused layers.
        static constexpr int kMaxLayers = 32;
        std::array<std::string, kMaxLayers> PhysicsLayers = MakeDefaultLayers();
        std::array<uint32_t, kMaxLayers> LayerCollisionMatrix = MakeDefaultMatrix(); // bit j of row i: i collides with j

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
        static fs::path GetAssetsDirectory();
    };
}
