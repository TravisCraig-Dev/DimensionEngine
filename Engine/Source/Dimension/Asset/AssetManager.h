#pragma once
#include "Dimension/Core/Base.h"
#include "Dimension/Core/Guid.h"
#include <nlohmann/json.hpp>

namespace Dimension
{
    class Texture2D;
    class Mesh;
    struct Material;
    class Font;
    struct AnimatorController;

    enum class AssetType : int32_t { None = 0, Texture, Mesh, Scene, Prefab, Audio, Material, Font, AnimatorController };

    const char* AssetTypeToString(AssetType type);

    struct AssetMetadata
    {
        AssetHandle Handle = AssetHandle::None();
        AssetType Type = AssetType::None;
        fs::path Path;               // relative to the Assets directory
        nlohmann::json ImportSettings = nlohmann::json::object();
        fs::file_time_type LastWrite{};
    };

    // Project asset database. Every importable file in the Assets folder gets a
    // sidecar ".meta" file with a stable GUID (like Unity), so references survive
    // renames and moves done through the editor.
    class AssetManager
    {
    public:
        // Built-in meshes with reserved handles.
        static constexpr AssetHandle kCube{ uint64_t(1) };
        static constexpr AssetHandle kSphere{ uint64_t(2) };
        static constexpr AssetHandle kPlane{ uint64_t(3) };
        static constexpr AssetHandle kQuad{ uint64_t(4) };
        static constexpr AssetHandle kCylinder{ uint64_t(5) };

        static void Init(const fs::path& assetsDirectory);
        static void Shutdown();
        // Rescans the Assets folder: imports new files, drops deleted ones, reloads changed ones.
        static void Refresh();

        static const fs::path& GetAssetsDirectory();
        static AssetType TypeFromExtension(const fs::path& path);

        static AssetHandle Import(const fs::path& absolutePath);
        static AssetHandle GetHandle(const fs::path& path); // absolute or Assets-relative
        static const AssetMetadata* GetMetadata(AssetHandle handle);
        static fs::path GetAbsolutePath(AssetHandle handle);
        static std::string GetDisplayName(AssetHandle handle);
        static std::vector<AssetHandle> GetAssetsOfType(AssetType type);
        static bool IsBuiltin(AssetHandle handle) { return (uint64_t)handle > 0 && (uint64_t)handle < 1024; }

        static void SetImportSettings(AssetHandle handle, const nlohmann::json& settings);
        // Moves/renames an asset (and its .meta) keeping its GUID.
        static bool MoveAsset(const fs::path& fromAbsolute, const fs::path& toAbsolute);
        static void DeleteAsset(const fs::path& absolutePath);

        static Ref<Texture2D> GetTexture(AssetHandle handle);
        static Ref<Mesh> GetMesh(AssetHandle handle);
        // Materials are editable in place; call SaveMaterial after changing one.
        static Ref<Material> GetMaterial(AssetHandle handle);
        static void SaveMaterial(AssetHandle handle);
        static AssetHandle CreateMaterial(const fs::path& absolutePath);
        static Ref<Font> GetFont(AssetHandle handle); // falls back to the built-in font
        // Animator controllers are editable in place; call SaveAnimatorController after changing one.
        static Ref<AnimatorController> GetAnimatorController(AssetHandle handle);
        static void SaveAnimatorController(AssetHandle handle);
        static AssetHandle CreateAnimatorController(const fs::path& absolutePath);
        static Ref<Font> GetDefaultFont();
        static Ref<Texture2D> GetWhiteTexture();
        static Ref<Texture2D> GetCheckerTexture();
        static Ref<Texture2D> GetParticleTexture(); // soft round default sprite for particles
    };
}
