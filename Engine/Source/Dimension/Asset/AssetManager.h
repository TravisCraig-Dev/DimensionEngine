#pragma once
#include "Dimension/Core/Base.h"
#include "Dimension/Core/Guid.h"
#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

namespace Dimension
{
    class Texture2D;
    class Mesh;
    struct MeshData;
    struct MeshImportSettings;
    struct Material;
    class Font;
    struct AnimatorController;
    struct TimelineAsset;
    struct SpriteAtlas;
    struct SpriteRect;

    enum class AssetType : int32_t { None = 0, Texture, Mesh, Scene, Prefab, Audio, Material, Font, AnimatorController, Timeline, SpriteAtlas, Preset, StringTable };

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
        static bool IsInitialized();
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
        // True once the asset's runtime object (texture, mesh) has been created.
        static bool IsLoaded(AssetHandle handle);
        static bool IsBuiltin(AssetHandle handle) { return (uint64_t)handle > 0 && (uint64_t)handle < 1024; }

        static void SetImportSettings(AssetHandle handle, const nlohmann::json& settings);
        // Moves/renames an asset (and its .meta) keeping its GUID.
        static bool MoveAsset(const fs::path& fromAbsolute, const fs::path& toAbsolute);
        static void DeleteAsset(const fs::path& absolutePath);

        static Ref<Texture2D> GetTexture(AssetHandle handle);
        static Ref<Mesh> GetMesh(AssetHandle handle);
        // CPU geometry of a mesh asset or built-in primitive (colliders, tools); cached, no GPU needed.
        static std::shared_ptr<const MeshData> GetMeshData(AssetHandle handle);
        // A mesh made at runtime (a Breakable's pieces): kept in memory under a fresh handle, with `materials` as its
        // model material slots, until released. Never saved.
        static AssetHandle RegisterRuntimeMesh(MeshData data, std::vector<AssetHandle> materials = {});
        static void ReleaseRuntimeMesh(AssetHandle handle);
        // Material per slot of a model (glTF materials). Until extracted these are read-only sub-assets of the
        // model, built from the file; after ExtractMeshMaterials they are editable .mat assets. Loads the mesh.
        static const std::vector<AssetHandle>& GetMeshMaterials(AssetHandle mesh);
        // A model's import settings from its .meta (defaults for other assets), and loading it with them.
        static MeshImportSettings GetMeshImportSettings(AssetHandle mesh);
        static bool LoadMeshData(const fs::path& file, MeshData& out);
        static std::vector<std::string> GetMeshMaterialNames(AssetHandle mesh);
        static bool AreMeshMaterialsExtracted(AssetHandle mesh);
        // Writes the model's materials (and embedded textures) next to it as assets and uses them from now on.
        static bool ExtractMeshMaterials(AssetHandle mesh);
        // True for in-memory assets owned by a model (embedded materials and textures).
        static bool IsSubAsset(AssetHandle handle);
        // Materials are editable in place; call SaveMaterial after changing one.
        static Ref<Material> GetMaterial(AssetHandle handle);
        static void SaveMaterial(AssetHandle handle);
        static AssetHandle CreateMaterial(const fs::path& absolutePath);
        static Ref<Font> GetFont(AssetHandle handle); // falls back to the built-in font
        // Animator controllers are editable in place; call SaveAnimatorController after changing one.
        static Ref<AnimatorController> GetAnimatorController(AssetHandle handle);
        static void SaveAnimatorController(AssetHandle handle);
        static AssetHandle CreateAnimatorController(const fs::path& absolutePath);
        // Timelines (.timeline) are editable in place too; SaveTimeline writes the changes.
        static Ref<TimelineAsset> GetTimeline(AssetHandle handle);
        static void SaveTimeline(AssetHandle handle);
        static AssetHandle CreateTimeline(const fs::path& absolutePath);
        // Sprite atlases (.spriteatlas): packed when first asked for (and again after a sprite or the atlas changes);
        // SaveSpriteAtlas writes changes and packs again. CreateSpriteAtlas packs the given textures.
        static Ref<SpriteAtlas> GetSpriteAtlas(AssetHandle handle);
        static void SaveSpriteAtlas(AssetHandle handle);
        static AssetHandle CreateSpriteAtlas(const fs::path& absolutePath, const std::vector<AssetHandle>& sprites);
        // Asset labels (like Unity's), kept in the .meta file: set without importing the asset again.
        static std::vector<std::string> GetLabels(AssetHandle handle);
        static void SetLabels(AssetHandle handle, const std::vector<std::string>& labels);
        // Every label used in the project, sorted.
        static std::vector<std::string> GetAllLabels();
        // Assets matching a search: words match the file name; "t:Type" its type (Texture, Mesh, Material, Prefab,
        // Scene, Audio, Font, AnimatorController, Timeline, SpriteAtlas; spaces and case ignored, a prefix will do) and
        // "l:Label" one of its labels (case ignored). Every term must match. Sorted by path.
        static std::vector<AssetHandle> Search(const std::string& query);
        static bool Matches(const AssetMetadata& meta, const std::string& query);
        // The sprites sliced from a texture (Sprite Editor; its import settings' "sprites"), and one by name (null when
        // none).
        static const std::vector<SpriteRect>& GetSprites(AssetHandle texture);
        static const SpriteRect* FindSprite(AssetHandle texture, const std::string& name);
        // The atlas page a sprite texture is packed into, and its region there (UVs: xy min, zw max); null when none.
        static Ref<Texture2D> FindInAtlas(AssetHandle texture, glm::vec4& region);
        static Ref<Font> GetDefaultFont();
        static Ref<Texture2D> GetWhiteTexture();
        static Ref<Texture2D> GetCheckerTexture();
        static Ref<Texture2D> GetParticleTexture(); // soft round default sprite for particles
        static Ref<Texture2D> GetCircleTexture();   // antialiased disc (UI handles)
    };
}
