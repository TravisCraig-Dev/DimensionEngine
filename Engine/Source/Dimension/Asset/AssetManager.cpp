#include "Dimension/Asset/AssetManager.h"
#include "Dimension/Physics/Cloth.h"
#include "Dimension/Physics/Rope.h"
#include "Dimension/Physics/Water.h"
#include "Dimension/Terrain/Terrain.h"
#include "Dimension/Core/Log.h"
#include "Dimension/Renderer/Mesh.h"
#include "Dimension/Renderer/Texture.h"
#include "Dimension/Renderer/RenderDevice.h"
#include "Dimension/Asset/AssetPrefetch.h"
#include "Dimension/Renderer/Material.h"
#include "Dimension/Renderer/Font.h"
#include "Dimension/Animation/AnimatorController.h"
#include "Dimension/Animation/Timeline.h"
#include "Dimension/Animation/ClipAsset.h"
#include "Dimension/Renderer/SpriteAtlas.h"
#include "Dimension/Asset/ImportDefaults.h"
#include "Dimension/Project/Project.h"
#include "Dimension/Project/Localization.h"
#include "Dimension/Renderer/SpriteSlicing.h"
#include "embedded/kDefaultFontData.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <unordered_map>
#include <unordered_set>
#include <cstring>

namespace Dimension
{
    using json = nlohmann::json;

    namespace
    {
        struct State
        {
            fs::path AssetsDir;
            std::unordered_map<AssetHandle, AssetMetadata> Assets;
            std::unordered_map<std::string, AssetHandle> PathToHandle; // generic relative path
            std::unordered_map<AssetHandle, Ref<Texture2D>> Textures;
            std::unordered_map<AssetHandle, Ref<Mesh>> Meshes;
            std::unordered_map<AssetHandle, std::shared_ptr<const MeshData>> MeshDatas;
            std::unordered_map<AssetHandle, Ref<Material>> Materials;
            std::unordered_map<AssetHandle, const void*> RuntimeMaterials; // made while playing: their owners
            struct RuntimeTexture
            {
                uint32_t Width = 1, Height = 1;
                bool SRGB = true, Point = false, Dirty = true;
                bool RenderTarget = false; // a render texture: cameras draw into the GPU image; Pixels only seed it
                std::vector<uint32_t> Pixels;
                const void* Owner = nullptr;
            };
            std::unordered_map<AssetHandle, RuntimeTexture> RuntimeTextures;
            std::unordered_map<AssetHandle, Ref<Font>> Fonts;
            std::unordered_map<AssetHandle, Ref<AnimatorController>> Controllers;
            std::unordered_map<AssetHandle, Ref<AnimatorOverrideController>> Overrides;
            std::unordered_map<AssetHandle, Ref<TimelineAsset>> Timelines;
            std::unordered_map<AssetHandle, Ref<AnimationClipAsset>> AnimationClips;
            std::unordered_map<AssetHandle, std::vector<AssetHandle>> ClipsByModel; // .anim clips per model (rebuilt when stale)
            bool ClipIndexDirty = true;
            uint64_t ClipVersion = 1;
            std::unordered_map<AssetHandle, Ref<SpriteAtlas>> SpriteAtlases;
            std::unordered_map<AssetHandle, std::vector<SpriteRect>> SpriteRects; // parsed from textures' import settings
            // Sprite texture -> (atlas, region), rebuilt when assets or atlases change.
            std::unordered_map<AssetHandle, std::pair<AssetHandle, glm::vec4>> AtlasIndex;
            bool AtlasIndexDirty = true;
            // Model sub-assets (embedded materials/textures), keyed by the model.
            std::unordered_map<AssetHandle, std::vector<AssetHandle>> MeshMaterials;
            std::unordered_map<AssetHandle, std::vector<std::string>> MeshMaterialNames;
            std::unordered_map<AssetHandle, std::vector<AssetHandle>> SubAssets;
            std::unordered_map<AssetHandle, std::string> SubAssetNames;
            std::unordered_map<AssetHandle, bool> RuntimeMeshes; // handles made by RegisterRuntimeMesh
            Ref<Font> DefaultFont;
            Ref<Texture2D> White, Checker, Particle, Circle;
        };
        State* s = nullptr;

        std::string Key(const fs::path& relative) { return relative.generic_string(); }

        fs::path ToRelative(const fs::path& path)
        {
            if (path.is_relative()) return path.lexically_normal();
            // Fast path: purely lexical when the path is already under the Assets folder.
            fs::path rel = path.lexically_normal().lexically_relative(s->AssetsDir.lexically_normal());
            if (!rel.empty() && *rel.begin() != "..") return rel;
            std::error_code ec;
            fs::path abs = fs::weakly_canonical(path, ec);
            fs::path base = fs::weakly_canonical(s->AssetsDir, ec);
            return abs.lexically_relative(base);
        }

        fs::path MetaPath(const fs::path& absolute) { return fs::path(absolute.native() + L".meta"); }

        json DefaultSettings(AssetType type)
        {
            if (type == AssetType::Texture) return { { "filter", 0 }, { "srgb", true }, { "mips", true } };
            return json::object();
        }

        void WriteMeta(const AssetMetadata& m)
        {
            json j{ { "guid", (uint64_t)m.Handle }, { "type", AssetTypeToString(m.Type) }, { "settings", m.ImportSettings } };
            std::ofstream(MetaPath(s->AssetsDir / m.Path)) << j.dump(2);
        }

        void Register(AssetMetadata m)
        {
            s->AtlasIndexDirty = true;
            if (m.Type == AssetType::AnimationClip)
            {
                s->ClipIndexDirty = true;
                ++s->ClipVersion;
            }
            if (m.Type == AssetType::StringTable) Localization::Invalidate();
            s->PathToHandle[Key(m.Path)] = m.Handle;
            s->Assets[m.Handle] = std::move(m);
        }

        void Unload(AssetHandle h)
        {
            if (auto it = s->Assets.find(h); it != s->Assets.end() && it->second.Type == AssetType::StringTable) Localization::Invalidate();
            // An atlas, or a sprite packed into atlases: packed again when next asked for.
            s->AtlasIndexDirty = true;
            s->SpriteAtlases.erase(h);
            s->SpriteRects.erase(h);
            for (auto it = s->SpriteAtlases.begin(); it != s->SpriteAtlases.end();)
                if (it->second && std::find(it->second->Sprites.begin(), it->second->Sprites.end(), h) != it->second->Sprites.end())
                    it = s->SpriteAtlases.erase(it);
                else
                    ++it;
            if (auto it = s->SubAssets.find(h); it != s->SubAssets.end())
            {
                for (AssetHandle sub : it->second)
                {
                    s->Textures.erase(sub);
                    s->Materials.erase(sub);
                    s->SubAssetNames.erase(sub);
                }
                s->SubAssets.erase(it);
            }
            s->MeshMaterials.erase(h);
            s->MeshDatas.erase(h);
            s->MeshMaterialNames.erase(h);
            s->Textures.erase(h);
            s->Meshes.erase(h);
            s->Materials.erase(h);
            s->Fonts.erase(h);
            s->Controllers.erase(h);
            s->Overrides.erase(h);
            // Controllers made from overrides of this one are made again when next asked for.
            for (auto& [o, ov] : s->Overrides)
                if (ov && ov->Controller == h) s->Controllers.erase(o);
            s->Timelines.erase(h);
            if (s->AnimationClips.erase(h) || (s->Assets.contains(h) && s->Assets[h].Type == AssetType::AnimationClip))
            {
                s->ClipIndexDirty = true;
                ++s->ClipVersion;
            }
        }
    }

    const char* AssetTypeToString(AssetType type)
    {
        switch (type)
        {
        case AssetType::Texture: return "Texture";
        case AssetType::Mesh: return "Mesh";
        case AssetType::Scene: return "Scene";
        case AssetType::Prefab: return "Prefab";
        case AssetType::Audio: return "Audio";
        case AssetType::Material: return "Material";
        case AssetType::Font: return "Font";
        case AssetType::AnimatorController: return "Animator Controller";
        case AssetType::Timeline: return "Timeline";
        case AssetType::SpriteAtlas: return "Sprite Atlas";
        case AssetType::Preset: return "Preset";
        case AssetType::StringTable: return "String Table";
        case AssetType::DataAsset: return "Data Asset";
        case AssetType::AnimatorOverride: return "Animator Override Controller";
        case AssetType::RenderTexture: return "Render Texture";
        case AssetType::UIStyleSheet: return "UI Style Sheet";
        case AssetType::VideoClip: return "Video Clip";
        case AssetType::AnimationClip: return "Animation Clip";
        case AssetType::NavMeshData: return "NavMesh Data";
        default: return "None";
        }
    }

    namespace
    {
        // Stable handle for a sub-asset of a model (kind: 1 sRGB texture, 2 linear texture, 3 material).
        AssetHandle SubAssetHandle(AssetHandle owner, uint64_t kind, uint64_t index)
        {
            uint64_t x = (uint64_t)owner ^ (kind << 56) ^ (index * 0x9E3779B97F4A7C15ull);
            x ^= x >> 30; x *= 0xBF58476D1CE4E5B9ull;
            x ^= x >> 27; x *= 0x94D049BB133111EBull;
            x ^= x >> 31;
            return AssetHandle(x | (1ull << 63));
        }

        // Registers a loaded model's material slots: extracted .mat assets when the meta lists them,
        // otherwise read-only in-memory materials (and textures) built from the file.
        void RegisterMeshMaterials(AssetHandle mesh, const AssetMetadata& meta, const MeshData& data)
        {
            std::vector<AssetHandle> slots(data.Materials.size());
            std::vector<std::string> names(data.Materials.size());
            json extracted = meta.ImportSettings.value("materials", json::array());
            auto& subs = s->SubAssets[mesh];
            auto image = [&](int32_t index, bool srgb) -> AssetHandle {
                if (index < 0 || index >= (int32_t)data.Images.size()) return AssetHandle::None();
                AssetHandle h = SubAssetHandle(mesh, srgb ? 1 : 2, (uint64_t)index);
                if (!s->Textures.contains(h))
                {
                    const ImportedImage& im = data.Images[index];
                    TextureSettings ts;
                    ts.SRGB = srgb;
                    Ref<Texture2D> tex = !im.Bytes.empty() ? Texture2D::LoadFromMemory(im.Bytes.data(), im.Bytes.size(), ts, im.Name)
                                       : !im.File.empty()  ? Texture2D::LoadFromFile(im.File, ts)
                                                           : nullptr;
                    s->Textures[h] = tex;
                    s->SubAssetNames[h] = im.Name;
                    subs.push_back(h);
                }
                return h;
            };
            for (size_t i = 0; i < data.Materials.size(); ++i)
            {
                const ImportedMaterial& im = data.Materials[i];
                names[i] = im.Name;
                if (i < extracted.size() && extracted[i].is_number_unsigned())
                {
                    AssetHandle h(extracted[i].get<uint64_t>());
                    if (AssetManager::GetMetadata(h))
                    {
                        slots[i] = h;
                        continue;
                    }
                }
                AssetHandle h = SubAssetHandle(mesh, 3, i);
                auto m = MakeRef<Material>(im.Values);
                m->BaseColorMap = image(im.BaseColorImage, true);
                m->NormalMap = image(im.NormalImage, false);
                m->MetallicRoughnessMap = image(im.MetallicRoughnessImage, false);
                m->EmissionMap = image(im.EmissionImage, true);
                s->Materials[h] = m;
                s->SubAssetNames[h] = im.Name;
                subs.push_back(h);
                slots[i] = h;
            }
            s->MeshMaterials[mesh] = std::move(slots);
            s->MeshMaterialNames[mesh] = std::move(names);
        }

        std::string SafeFileName(std::string name)
        {
            for (char& c : name)
                if (std::strchr("<>:\"/\\|?*", c) || (unsigned char)c < 32) c = '_';
            return name.empty() ? "Unnamed" : name;
        }

        fs::path UniqueFile(const fs::path& path)
        {
            if (!fs::exists(path)) return path;
            for (int i = 1;; ++i)
            {
                fs::path p = path.parent_path() / (path.stem().string() + " " + std::to_string(i) + path.extension().string());
                if (!fs::exists(p)) return p;
            }
        }
    }

    void AssetManager::Init(const fs::path& assetsDirectory)
    {
        if (s) Shutdown();
        s = new State();
        s->AssetsDir = assetsDirectory;
        fs::create_directories(assetsDirectory);
        Localization::Reset(); // this project's tables, and its player's language

        Refresh();
        Log::Info("Asset database: {} assets in '{}'", s->Assets.size(), assetsDirectory.string());
    }

    void AssetManager::Shutdown()
    {
        Cloth::ReleaseMeshes(); // GPU grids, before the device goes
        Rope::ReleaseMeshes();
        Water::ReleaseMeshes();
        Font::ReleaseFallbackFonts(); // their atlases, before the device goes
        Terrain::ReleaseRuntimes();
        delete s;
        s = nullptr;
        Localization::Reset();
    }

    bool AssetManager::IsInitialized() { return s != nullptr; }

    const fs::path& AssetManager::GetAssetsDirectory() { return s->AssetsDir; }

    AssetType AssetManager::TypeFromExtension(const fs::path& path)
    {
        std::string ext = path.extension().string();
        for (auto& c : ext) c = (char)std::tolower(c);
        if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga" || ext == ".bmp" || ext == ".psd" || ext == ".hdr")
            return AssetType::Texture;
        if (ext == ".gltf" || ext == ".glb" || ext == ".obj") return AssetType::Mesh;
        if (ext == ".scene") return AssetType::Scene;
        if (ext == ".prefab") return AssetType::Prefab;
        if (ext == ".mat") return AssetType::Material;
        if (ext == ".ttf" || ext == ".otf") return AssetType::Font;
        if (ext == ".controller") return AssetType::AnimatorController;
        if (ext == ".overridecontroller") return AssetType::AnimatorOverride;
        if (ext == ".rendertexture") return AssetType::RenderTexture;
        if (ext == ".timeline") return AssetType::Timeline;
        if (ext == ".anim") return AssetType::AnimationClip;
        if (ext == ".navmesh") return AssetType::NavMeshData;
        if (ext == ".spriteatlas") return AssetType::SpriteAtlas;
        if (ext == ".preset") return AssetType::Preset;
        if (ext == ".csv") return AssetType::StringTable;
        if (ext == ".asset") return AssetType::DataAsset;
        if (ext == ".uistyle") return AssetType::UIStyleSheet;
        if (ext == ".mp4" || ext == ".m4v" || ext == ".mov" || ext == ".wmv" || ext == ".avi" || ext == ".mkv" || ext == ".webm") return AssetType::VideoClip;
        if (ext == ".wav" || ext == ".mp3" || ext == ".flac" || ext == ".ogg" || ext == ".opus") return AssetType::Audio;
        return AssetType::None;
    }

    AssetHandle AssetManager::Import(const fs::path& absolutePath)
    {
        AssetType type = TypeFromExtension(absolutePath);
        if (type == AssetType::None || !fs::is_regular_file(absolutePath)) return AssetHandle::None();
        fs::path rel = ToRelative(absolutePath);
        if (auto it = s->PathToHandle.find(Key(rel)); it != s->PathToHandle.end()) return it->second;

        AssetMetadata m;
        m.Type = type;
        m.Path = rel;
        m.ImportSettings = DefaultSettings(type);
        m.LastWrite = fs::last_write_time(absolutePath);

        bool writeMeta = true;
        fs::path meta = MetaPath(absolutePath);
        // A new asset (no .meta yet) starts from its folder's import defaults.
        if (!fs::exists(meta) && Project::IsOpen())
            if (const ImportRule* rule = ImportDefaults::For(rel, type)) m.ImportSettings.update(rule->Settings);
        if (fs::exists(meta))
        {
            try
            {
                json j = json::parse(std::ifstream(meta));
                m.Handle = Guid(j.at("guid").get<uint64_t>());
                if (j.contains("settings")) m.ImportSettings.update(j["settings"]);
                writeMeta = false;
            }
            catch (const std::exception& e)
            {
                Log::Warn("Corrupt meta file '{}': {}", meta.string(), e.what());
            }
        }
        // A copied file may carry a duplicate GUID; give the copy a new identity.
        if (!m.Handle.IsValid() || s->Assets.contains(m.Handle))
        {
            m.Handle = Guid();
            writeMeta = true;
        }
        if (writeMeta) WriteMeta(m);
        AssetHandle handle = m.Handle;
        Register(std::move(m));
        return handle;
    }

    void AssetManager::Refresh()
    {
        std::unordered_set<AssetHandle> seen;
        std::error_code ec;
        for (auto it = fs::recursive_directory_iterator(s->AssetsDir, ec); it != fs::recursive_directory_iterator(); it.increment(ec))
        {
            if (ec) break;
            if (!it->is_regular_file() || it->path().extension() == ".meta") continue;
            AssetHandle h = Import(it->path());
            if (!h.IsValid()) continue;
            seen.insert(h);
            auto& meta = s->Assets[h];
            auto writeTime = fs::last_write_time(it->path(), ec);
            if (writeTime != meta.LastWrite)
            {
                meta.LastWrite = writeTime;
                Unload(h); // hot reload on next access
                Log::Info("Reimported '{}'", meta.Path.generic_string());
            }
        }
        // Forget assets whose files are gone.
        for (auto it = s->Assets.begin(); it != s->Assets.end();)
        {
            if (!seen.contains(it->first))
            {
                s->PathToHandle.erase(Key(it->second.Path));
                Unload(it->first);
                it = s->Assets.erase(it);
            }
            else ++it;
        }
    }

    AssetHandle AssetManager::GetHandle(const fs::path& path)
    {
        auto it = s->PathToHandle.find(Key(ToRelative(path)));
        return it != s->PathToHandle.end() ? it->second : AssetHandle::None();
    }

    const AssetMetadata* AssetManager::GetMetadata(AssetHandle handle)
    {
        auto it = s->Assets.find(handle);
        return it != s->Assets.end() ? &it->second : nullptr;
    }

    fs::path AssetManager::GetAbsolutePath(AssetHandle handle)
    {
        auto* m = GetMetadata(handle);
        return m ? s->AssetsDir / m->Path : fs::path();
    }

    std::string AssetManager::GetDisplayName(AssetHandle handle)
    {
        switch ((uint64_t)handle)
        {
        case 0: return "None";
        case 1: return "Cube (built-in)";
        case 2: return "Sphere (built-in)";
        case 3: return "Plane (built-in)";
        case 4: return "Quad (built-in)";
        case 5: return "Cylinder (built-in)";
        default: break;
        }
        if (auto it = s->SubAssetNames.find(handle); it != s->SubAssetNames.end()) return it->second + " (embedded)";
        auto* m = GetMetadata(handle);
        return m ? m->Path.stem().string() : "Missing";
    }

    std::vector<AssetHandle> AssetManager::GetAssetsOfType(AssetType type)
    {
        std::vector<AssetHandle> result;
        for (auto& [h, m] : s->Assets)
            if (m.Type == type) result.push_back(h);
        std::sort(result.begin(), result.end(), [](AssetHandle a, AssetHandle b) {
            return s->Assets[a].Path < s->Assets[b].Path;
        });
        return result;
    }

    void AssetManager::SetImportSettings(AssetHandle handle, const json& settings)
    {
        auto it = s->Assets.find(handle);
        if (it == s->Assets.end()) return;
        it->second.ImportSettings = settings;
        WriteMeta(it->second);
        Unload(handle);
    }

    bool AssetManager::MoveAsset(const fs::path& from, const fs::path& to)
    {
        std::error_code ec;
        if (fs::exists(to)) return false;
        fs::rename(from, to, ec);
        if (ec)
        {
            Log::Error("Move failed: {}", ec.message());
            return false;
        }
        if (fs::exists(MetaPath(from))) fs::rename(MetaPath(from), MetaPath(to), ec);

        // Update database entries (a directory move affects every asset under it).
        fs::path relFrom = ToRelative(from), relTo = ToRelative(to);
        std::string prefix = Key(relFrom);
        std::vector<std::pair<std::string, AssetHandle>> moved;
        for (auto& [key, h] : s->PathToHandle)
            if (key == prefix || key.starts_with(prefix + "/")) moved.emplace_back(key, h);
        for (auto& [key, h] : moved)
        {
            s->PathToHandle.erase(key);
            fs::path newRel = Key(relTo) + key.substr(prefix.size());
            s->Assets[h].Path = newRel;
            s->PathToHandle[Key(newRel)] = h;
        }
        return true;
    }

    void AssetManager::DeleteAsset(const fs::path& absolutePath)
    {
        std::error_code ec;
        fs::remove_all(absolutePath, ec);
        fs::remove(MetaPath(absolutePath), ec);
        Refresh();
    }

    bool AssetManager::IsLoaded(AssetHandle handle)
    {
        return s && (s->Textures.contains(handle) || s->Meshes.contains(handle));
    }

    Ref<Texture2D> AssetManager::GetTexture(AssetHandle handle)
    {
        if (!handle.IsValid()) return nullptr;
        if (auto rt = s->RuntimeTextures.find(handle); rt != s->RuntimeTextures.end())
        {
            // Made (again) from its pixels when they changed; the old image is freed once the GPU is done with it.
            auto& r = rt->second;
            if ((r.Dirty && !r.RenderTarget) || !s->Textures.contains(handle))
            {
                TextureSettings ts;
                ts.GenerateMips = false;
                ts.SRGB = r.SRGB;
                ts.Filter = r.Point ? TextureFilter::Point : TextureFilter::Bilinear;
                s->Textures[handle] = MakeRef<Texture2D>(r.Pixels.data(), r.Width, r.Height, ts, "Runtime Texture");
                r.Dirty = false;
            }
            return s->Textures[handle];
        }
        if (auto it = s->Textures.find(handle); it != s->Textures.end()) return it->second;
        auto* meta = GetMetadata(handle);
        if (meta && meta->Type == AssetType::RenderTexture)
        {
            // A black image of its size that cameras draw into (sRGB like color maps; no mips).
            glm::uvec2 size = GetRenderTextureSize(handle);
            TextureSettings rs;
            rs.GenerateMips = false;
            json file;
            try { file = json::parse(std::ifstream(s->AssetsDir / meta->Path)); } catch (...) {}
            rs.Filter = (TextureFilter)(file.is_object() ? file.value("filter", 0) : 0);
            std::vector<uint32_t> black((size_t)size.x * size.y, 0xFF000000u);
            auto rt = MakeRef<Texture2D>(black.data(), size.x, size.y, rs, meta->Path.filename().string().c_str());
            s->Textures[handle] = rt;
            return rt;
        }
        if (!meta || meta->Type != AssetType::Texture) return nullptr;
        TextureSettings ts;
        ts.Filter = (TextureFilter)meta->ImportSettings.value("filter", 0);
        ts.SRGB = meta->ImportSettings.value("srgb", true);
        ts.GenerateMips = meta->ImportSettings.value("mips", true);
        fs::path file = s->AssetsDir / meta->Path;
        Ref<Texture2D> tex;
        if (auto image = AssetPrefetch::TakeImage(file)) tex = Texture2D::FromDecoded(*image, ts, meta->Path.filename().string());
        else tex = Texture2D::LoadFromFile(file, ts);
        s->Textures[handle] = tex; // cache failures too (nullptr) until next reimport
        return tex;
    }

    Ref<Mesh> AssetManager::GetMesh(AssetHandle handle)
    {
        if (!handle.IsValid()) return nullptr;
        if (auto it = s->Meshes.find(handle); it != s->Meshes.end()) return it->second;
        Ref<Mesh> mesh;
        switch ((uint64_t)handle)
        {
        case 1: mesh = Mesh::CreateCube(); break;
        case 2: mesh = Mesh::CreateSphere(); break;
        case 3: mesh = Mesh::CreatePlane(); break;
        case 4: mesh = Mesh::CreateQuad(); break;
        case 5: mesh = Mesh::CreateCylinder(); break;
        default:
            if (s->RuntimeMeshes.contains(handle))
            {
                if (auto it = s->MeshDatas.find(handle); it != s->MeshDatas.end() && it->second) mesh = MakeRef<Mesh>(MeshData(*it->second));
            }
            else if (auto* meta = GetMetadata(handle); meta && meta->Type == AssetType::Mesh)
            {
                MeshData data;
                bool ok;
                if (auto prefetched = AssetPrefetch::FindMesh(s->AssetsDir / meta->Path))
                {
                    data = *prefetched; // decoded by a background scene load
                    ok = true;
                }
                else
                    ok = LoadMeshData(s->AssetsDir / meta->Path, data);
                if (ok)
                {
                    RegisterMeshMaterials(handle, *meta, data);
                    mesh = MakeRef<Mesh>(std::move(data));
                }
            }
        }
        s->Meshes[handle] = mesh;
        return mesh;
    }

    std::shared_ptr<const MeshData> AssetManager::GetMeshData(AssetHandle handle)
    {
        if (!handle.IsValid() || !s) return nullptr;
        if (auto it = s->MeshDatas.find(handle); it != s->MeshDatas.end()) return it->second;
        std::shared_ptr<MeshData> data;
        switch ((uint64_t)handle)
        {
        case 1: data = std::make_shared<MeshData>(MeshData::Cube()); break;
        case 2: data = std::make_shared<MeshData>(MeshData::Sphere()); break;
        case 3: data = std::make_shared<MeshData>(MeshData::Plane()); break;
        case 4: data = std::make_shared<MeshData>(MeshData::Quad()); break;
        case 5: data = std::make_shared<MeshData>(MeshData::Cylinder()); break;
        default:
            if (auto* meta = GetMetadata(handle); meta && meta->Type == AssetType::Mesh)
            {
                data = AssetPrefetch::FindMesh(s->AssetsDir / meta->Path);
                if (!data)
                {
                    data = std::make_shared<MeshData>();
                    if (!LoadMeshData(s->AssetsDir / meta->Path, *data)) data.reset();
                }
            }
        }
        s->MeshDatas[handle] = data;
        return data;
    }

    AssetHandle AssetManager::RegisterRuntimeMesh(MeshData data, std::vector<AssetHandle> materials)
    {
        AssetHandle handle; // a fresh random id
        s->RuntimeMeshes[handle] = true;
        s->MeshDatas[handle] = std::make_shared<const MeshData>(std::move(data));
        s->MeshMaterials[handle] = std::move(materials);
        return handle;
    }

    void AssetManager::ReleaseRuntimeMesh(AssetHandle handle)
    {
        if (!s || !s->RuntimeMeshes.erase(handle)) return;
        s->MeshDatas.erase(handle);
        s->Meshes.erase(handle);
        s->MeshMaterials.erase(handle);
    }

    const std::vector<AssetHandle>& AssetManager::GetMeshMaterials(AssetHandle mesh)
    {
        static const std::vector<AssetHandle> kNone;
        if (!s->Meshes.contains(mesh)) GetMesh(mesh);
        auto it = s->MeshMaterials.find(mesh);
        return it != s->MeshMaterials.end() ? it->second : kNone;
    }

    std::vector<std::string> AssetManager::GetMeshMaterialNames(AssetHandle mesh)
    {
        GetMeshMaterials(mesh);
        auto it = s->MeshMaterialNames.find(mesh);
        return it != s->MeshMaterialNames.end() ? it->second : std::vector<std::string>{};
    }

    bool AssetManager::AreMeshMaterialsExtracted(AssetHandle mesh)
    {
        auto* meta = GetMetadata(mesh);
        return meta && meta->ImportSettings.contains("materials") && !meta->ImportSettings["materials"].empty();
    }

    bool AssetManager::IsSubAsset(AssetHandle handle) { return s->SubAssetNames.contains(handle); }

    MeshImportSettings AssetManager::GetMeshImportSettings(AssetHandle mesh)
    {
        MeshImportSettings settings;
        if (!s) return settings;
        auto* meta = GetMetadata(mesh);
        if (!meta || meta->Type != AssetType::Mesh) return settings;
        settings.Scale = std::clamp(meta->ImportSettings.value("scale", 1.0f), 0.0001f, 10000.0f);
        settings.ImportAnimations = meta->ImportSettings.value("animations", true);
        settings.ImportMaterials = meta->ImportSettings.value("importMaterials", true);
        if (auto it = meta->ImportSettings.find("excludedClips"); it != meta->ImportSettings.end() && it->is_array())
            for (const auto& name : *it)
                if (name.is_string()) settings.ExcludedClips.push_back(name.get<std::string>());
        return settings;
    }

    bool AssetManager::LoadMeshData(const fs::path& file, MeshData& out)
    {
        MeshImportSettings settings = s ? GetMeshImportSettings(GetHandle(file)) : MeshImportSettings{};
        return MeshData::LoadFromFile(file, out, settings);
    }

    bool AssetManager::ExtractMeshMaterials(AssetHandle mesh)
    {
        auto* meta = GetMetadata(mesh);
        if (!meta || meta->Type != AssetType::Mesh) return false;
        fs::path modelPath = s->AssetsDir / meta->Path;
        MeshData data;
        if (!LoadMeshData(modelPath, data) || data.Materials.empty()) return false;

        fs::path dir = modelPath.parent_path() / (modelPath.stem().string() + " Materials");
        std::error_code ec;
        fs::create_directories(dir, ec);

        // Textures: external files inside Assets are referenced; embedded images are written out.
        std::vector<AssetHandle> images(data.Images.size());
        std::vector<bool> linear(data.Images.size(), false);
        for (auto& m : data.Materials)
            for (int32_t i : { m.NormalImage, m.MetallicRoughnessImage })
                if (i >= 0 && i < (int32_t)linear.size()) linear[i] = true;
        for (size_t i = 0; i < data.Images.size(); ++i)
        {
            const ImportedImage& im = data.Images[i];
            fs::path file;
            if (!im.Bytes.empty())
            {
                bool png = im.Bytes.size() > 4 && im.Bytes[0] == 0x89 && im.Bytes[1] == 'P';
                file = UniqueFile(dir / (SafeFileName(im.Name) + (png ? ".png" : ".jpg")));
                std::ofstream(file, std::ios::binary).write((const char*)im.Bytes.data(), (std::streamsize)im.Bytes.size());
            }
            else if (!im.File.empty() && fs::exists(im.File))
            {
                fs::path rel = im.File.lexically_relative(s->AssetsDir);
                if (rel.empty() || *rel.begin() == "..") // outside the project: copy it in
                {
                    file = UniqueFile(dir / im.File.filename());
                    fs::copy_file(im.File, file, ec);
                }
                else
                    file = im.File;
            }
            if (file.empty()) continue;
            images[i] = Import(file);
            if (images[i].IsValid() && linear[i])
            {
                json settings = GetMetadata(images[i])->ImportSettings;
                settings["srgb"] = false;
                SetImportSettings(images[i], settings);
            }
        }

        json guids = json::array();
        for (auto& im : data.Materials)
        {
            Material m = im.Values;
            auto img = [&](int32_t i) { return i >= 0 && i < (int32_t)images.size() ? images[i] : AssetHandle::None(); };
            m.BaseColorMap = img(im.BaseColorImage);
            m.NormalMap = img(im.NormalImage);
            m.MetallicRoughnessMap = img(im.MetallicRoughnessImage);
            m.EmissionMap = img(im.EmissionImage);
            fs::path file = UniqueFile(dir / (SafeFileName(im.Name) + ".mat"));
            if (!m.Save(file)) return false;
            guids.push_back((uint64_t)Import(file));
        }
        json settings = GetMetadata(mesh)->ImportSettings;
        settings["materials"] = guids;
        SetImportSettings(mesh, settings); // unloads the model; it re-registers with the extracted materials
        Log::Info("Extracted {} material(s) from '{}' to '{}'", guids.size(), meta->Path.filename().string(), dir.filename().string());
        return true;
    }

    Ref<Material> AssetManager::GetMaterial(AssetHandle handle)
    {
        if (!handle.IsValid()) return nullptr;
        if (auto it = s->Materials.find(handle); it != s->Materials.end()) return it->second;
        auto* meta = GetMetadata(handle);
        if (!meta || meta->Type != AssetType::Material) return nullptr;
        auto material = Material::Load(s->AssetsDir / meta->Path);
        s->Materials[handle] = material;
        return material;
    }

    void AssetManager::SaveMaterial(AssetHandle handle)
    {
        auto it = s->Materials.find(handle);
        auto* meta = GetMetadata(handle);
        if (it == s->Materials.end() || !it->second || !meta) return;
        fs::path path = s->AssetsDir / meta->Path;
        it->second->Save(path);
        std::error_code ec;
        s->Assets[handle].LastWrite = fs::last_write_time(path, ec); // our own write is not an external change
    }

    AssetHandle AssetManager::CreateRuntimeMaterial(const Material& source, const void* owner)
    {
        AssetHandle handle; // a new random one
        s->Materials[handle] = MakeRef<Material>(source);
        s->RuntimeMaterials[handle] = owner;
        return handle;
    }

    bool AssetManager::IsRuntimeMaterial(AssetHandle handle) { return s && s->RuntimeMaterials.contains(handle); }

    void AssetManager::DestroyRuntimeMaterial(AssetHandle handle)
    {
        if (!s || !s->RuntimeMaterials.erase(handle)) return; // only made ones (assets stay)
        s->Materials.erase(handle);
    }

    void AssetManager::ReleaseRuntimeMaterials(const void* owner)
    {
        if (!s) return;
        for (auto it = s->RuntimeMaterials.begin(); it != s->RuntimeMaterials.end();)
        {
            if (it->second != owner) { ++it; continue; }
            s->Materials.erase(it->first);
            it = s->RuntimeMaterials.erase(it);
        }
    }

    AssetHandle AssetManager::CreateRuntimeTexture(uint32_t width, uint32_t height, bool srgb, bool pointFilter, const void* owner)
    {
        if (!s) return AssetHandle::None();
        AssetHandle handle; // a new random one
        auto& r = s->RuntimeTextures[handle];
        r.Width = std::clamp(width, 1u, 8192u);
        r.Height = std::clamp(height, 1u, 8192u);
        r.SRGB = srgb;
        r.Point = pointFilter;
        r.Owner = owner;
        r.Pixels.assign((size_t)r.Width * r.Height, 0xFFFFFFFFu); // white, like Unity's new Texture2D's gray-ish default
        return handle;
    }

    bool AssetManager::IsRuntimeTexture(AssetHandle handle) { return s && s->RuntimeTextures.contains(handle); }

    AssetHandle AssetManager::CreateRuntimeRenderTexture(uint32_t width, uint32_t height, const void* owner)
    {
        AssetHandle h = CreateRuntimeTexture(width, height, true, false, owner);
        if (h.IsValid())
        {
            auto& r = s->RuntimeTextures[h];
            r.RenderTarget = true;
            std::fill(r.Pixels.begin(), r.Pixels.end(), 0xFF000000u);
        }
        return h;
    }

    bool AssetManager::ReadTexturePixels(AssetHandle handle, std::vector<uint32_t>& out, glm::uvec2& size)
    {
        Ref<Texture2D> tex = GetTexture(handle);
        if (!tex) return false;
        VkFormat format = tex->GetImage().GetFormat();
        if (format != VK_FORMAT_R8G8B8A8_SRGB && format != VK_FORMAT_R8G8B8A8_UNORM) return false;
        size = { tex->GetWidth(), tex->GetHeight() };
        Buffer staging((VkDeviceSize)size.x * size.y * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, MemoryUsage::GpuToCpu);
        VkImage image = tex->GetImage().GetHandle();
        RenderDevice::Get().ImmediateSubmit([&](VkCommandBuffer cmd) {
            Vk::TransitionImage(cmd, image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            VkBufferImageCopy copy{};
            copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            copy.imageExtent = { size.x, size.y, 1 };
            vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging.GetHandle(), 1, &copy);
            Vk::TransitionImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        });
        out.resize((size_t)size.x * size.y);
        std::memcpy(out.data(), staging.GetMapped(), out.size() * 4);
        return true;
    }

    bool AssetManager::SetRuntimeTexturePixels(AssetHandle handle, const uint32_t* rgba8, size_t count)
    {
        auto it = s ? s->RuntimeTextures.find(handle) : decltype(s->RuntimeTextures)::iterator{};
        if (!s || it == s->RuntimeTextures.end() || count != it->second.Pixels.size()) return false;
        std::memcpy(it->second.Pixels.data(), rgba8, count * 4);
        it->second.Dirty = true;
        return true;
    }

    bool AssetManager::StreamRuntimeTexturePixels(AssetHandle handle, const uint32_t* rgba8, size_t count)
    {
        auto it = s ? s->RuntimeTextures.find(handle) : decltype(s->RuntimeTextures)::iterator{};
        if (!s || it == s->RuntimeTextures.end() || count != it->second.Pixels.size()) return false;
        auto& r = it->second;
        std::memcpy(r.Pixels.data(), rgba8, count * 4); // kept for reading back
        if (!r.Dirty && !r.RenderTarget && RenderDevice::Exists() && RenderDevice::Get().IsRecordingFrame())
            if (auto tex = s->Textures.find(handle); tex != s->Textures.end() && tex->second &&
                                                     tex->second->Update(RenderDevice::Get().Frame().Cmd, rgba8, r.Width, r.Height))
                return true;
        r.Dirty = true;
        return true;
    }

    const std::vector<uint32_t>* AssetManager::GetRuntimeTexturePixels(AssetHandle handle, glm::uvec2* size)
    {
        auto it = s ? s->RuntimeTextures.find(handle) : decltype(s->RuntimeTextures)::iterator{};
        if (!s || it == s->RuntimeTextures.end()) return nullptr;
        if (size) *size = { it->second.Width, it->second.Height };
        return &it->second.Pixels;
    }

    void AssetManager::DestroyRuntimeTexture(AssetHandle handle)
    {
        if (!s || !s->RuntimeTextures.erase(handle)) return;
        s->Textures.erase(handle);
    }

    void AssetManager::ReleaseRuntimeTextures(const void* owner)
    {
        if (!s) return;
        for (auto it = s->RuntimeTextures.begin(); it != s->RuntimeTextures.end();)
        {
            if (it->second.Owner != owner) { ++it; continue; }
            s->Textures.erase(it->first);
            it = s->RuntimeTextures.erase(it);
        }
    }

    AssetHandle AssetManager::CreateMaterial(const fs::path& absolutePath)
    {
        Material defaults;
        if (!defaults.Save(absolutePath)) return AssetHandle::None();
        return Import(absolutePath);
    }

    Ref<Font> AssetManager::GetFont(AssetHandle handle)
    {
        if (!handle.IsValid()) return GetDefaultFont();
        if (auto it = s->Fonts.find(handle); it != s->Fonts.end()) return it->second ? it->second : GetDefaultFont();
        auto* meta = GetMetadata(handle);
        Ref<Font> font = (meta && meta->Type == AssetType::Font) ? Font::LoadFromFile(s->AssetsDir / meta->Path) : nullptr;
        s->Fonts[handle] = font;
        return font ? font : GetDefaultFont();
    }

    Ref<Texture2D> AssetManager::GetParticleTexture()
    {
        if (!s->Particle)
        {
            constexpr int N = 64;
            std::vector<uint32_t> pixels(N * N);
            for (int y = 0; y < N; ++y)
                for (int x = 0; x < N; ++x)
                {
                    float dx = (x + 0.5f) / N * 2.0f - 1.0f, dy = (y + 0.5f) / N * 2.0f - 1.0f;
                    float d = std::min(1.0f, std::sqrt(dx * dx + dy * dy));
                    float a = (1.0f - d) * (1.0f - d) * (3.0f - 2.0f * (1.0f - d)); // smooth falloff
                    pixels[y * N + x] = 0x00FFFFFFu | (uint32_t(std::clamp(a, 0.0f, 1.0f) * 255.0f) << 24);
                }
            TextureSettings ts;
            ts.SRGB = false;
            s->Particle = MakeRef<Texture2D>(pixels.data(), N, N, ts, "DefaultParticle");
        }
        return s->Particle;
    }

    Ref<AnimatorController> AssetManager::GetAnimatorController(AssetHandle handle)
    {
        if (!handle.IsValid()) return nullptr;
        if (auto it = s->Controllers.find(handle); it != s->Controllers.end()) return it->second;
        auto* meta = GetMetadata(handle);
        if (meta && meta->Type == AssetType::AnimatorOverride)
        {
            // Its controller with the clips swapped (an override of an override is not followed).
            auto o = GetAnimatorOverride(handle);
            auto* baseMeta = o ? GetMetadata(o->Controller) : nullptr;
            if (!baseMeta || baseMeta->Type != AssetType::AnimatorController) return nullptr;
            auto base = GetAnimatorController(o->Controller);
            if (!base) return nullptr;
            auto made = o->Apply(*base);
            s->Controllers[handle] = made;
            return made;
        }
        if (!meta || meta->Type != AssetType::AnimatorController) return nullptr;
        auto controller = AnimatorController::Load(s->AssetsDir / meta->Path);
        s->Controllers[handle] = controller;
        return controller;
    }

    Ref<AnimatorOverrideController> AssetManager::GetAnimatorOverride(AssetHandle handle)
    {
        if (!handle.IsValid() || !s) return nullptr;
        if (auto it = s->Overrides.find(handle); it != s->Overrides.end()) return it->second;
        auto* meta = GetMetadata(handle);
        if (!meta || meta->Type != AssetType::AnimatorOverride) return nullptr;
        auto o = AnimatorOverrideController::Load(s->AssetsDir / meta->Path);
        s->Overrides[handle] = o;
        return o;
    }

    void AssetManager::SaveAnimatorOverride(AssetHandle handle)
    {
        auto it = s->Overrides.find(handle);
        auto* meta = GetMetadata(handle);
        if (it == s->Overrides.end() || !it->second || !meta || meta->Type != AssetType::AnimatorOverride) return;
        fs::path path = s->AssetsDir / meta->Path;
        it->second->Save(path);
        std::error_code ec;
        s->Assets[handle].LastWrite = fs::last_write_time(path, ec);
        s->Controllers.erase(handle); // made again with the new clips
    }

    AssetHandle AssetManager::CreateAnimatorOverride(const fs::path& absolutePath, AssetHandle controller)
    {
        AnimatorOverrideController o;
        o.Controller = controller;
        if (!o.Save(absolutePath)) return AssetHandle::None();
        return Import(absolutePath);
    }

    AssetHandle AssetManager::CreateRenderTexture(const fs::path& absolutePath, uint32_t width, uint32_t height)
    {
        std::ofstream out(absolutePath);
        if (!out) return AssetHandle::None();
        out << json{ { "width", width }, { "height", height }, { "filter", 0 } }.dump(2);
        out.close();
        return Import(absolutePath);
    }

    glm::uvec2 AssetManager::GetRenderTextureSize(AssetHandle handle)
    {
        if (auto rt = s ? s->RuntimeTextures.find(handle) : decltype(s->RuntimeTextures)::iterator{}; s && rt != s->RuntimeTextures.end())
            return rt->second.RenderTarget ? glm::uvec2(rt->second.Width, rt->second.Height) : glm::uvec2(0);
        auto* meta = GetMetadata(handle);
        if (!meta || meta->Type != AssetType::RenderTexture) return glm::uvec2(0);
        try
        {
            json j = json::parse(std::ifstream(s->AssetsDir / meta->Path));
            return glm::clamp(glm::uvec2(j.value("width", 256u), j.value("height", 256u)), glm::uvec2(1), glm::uvec2(8192));
        }
        catch (...) { return glm::uvec2(256); }
    }

    bool AssetManager::SetRenderTextureSize(AssetHandle handle, uint32_t width, uint32_t height)
    {
        auto* meta = GetMetadata(handle);
        if (!meta || meta->Type != AssetType::RenderTexture) return false;
        fs::path path = s->AssetsDir / meta->Path;
        json j = json::object();
        try { j = json::parse(std::ifstream(path)); } catch (...) {}
        j["width"] = std::clamp(width, 1u, 8192u);
        j["height"] = std::clamp(height, 1u, 8192u);
        std::ofstream(path) << j.dump(2);
        std::error_code ec;
        s->Assets[handle].LastWrite = fs::last_write_time(path, ec);
        RenderDevice::Get().WaitIdle(); // the old image may still be in use
        s->Textures.erase(handle);
        return true;
    }

    AssetHandle AssetManager::GetBaseController(AssetHandle handle)
    {
        auto* meta = GetMetadata(handle);
        if (meta && meta->Type == AssetType::AnimatorOverride)
            if (auto o = GetAnimatorOverride(handle)) return o->Controller;
        return handle;
    }

    void AssetManager::SaveAnimatorController(AssetHandle handle)
    {
        auto it = s->Controllers.find(handle);
        auto* meta = GetMetadata(handle);
        if (it == s->Controllers.end() || !it->second || !meta || meta->Type != AssetType::AnimatorController) return;
        // Controllers made from overrides of this one are made again with the change.
        for (auto& [o, ov] : s->Overrides)
            if (ov && ov->Controller == handle) s->Controllers.erase(o);
        it = s->Controllers.find(handle);
        fs::path path = s->AssetsDir / meta->Path;
        it->second->Save(path);
        std::error_code ec;
        s->Assets[handle].LastWrite = fs::last_write_time(path, ec);
    }

    AssetHandle AssetManager::CreateAnimatorController(const fs::path& absolutePath)
    {
        AnimatorController controller;
        AnimState idle;
        idle.Name = "Idle";
        controller.States.push_back(idle);
        if (!controller.Save(absolutePath)) return AssetHandle::None();
        return Import(absolutePath);
    }

    Ref<TimelineAsset> AssetManager::GetTimeline(AssetHandle handle)
    {
        if (!handle.IsValid() || !s) return nullptr;
        if (auto it = s->Timelines.find(handle); it != s->Timelines.end()) return it->second;
        auto* meta = GetMetadata(handle);
        if (!meta || meta->Type != AssetType::Timeline) return nullptr;
        auto timeline = TimelineAsset::Load(s->AssetsDir / meta->Path);
        s->Timelines[handle] = timeline;
        return timeline;
    }

    void AssetManager::SaveTimeline(AssetHandle handle)
    {
        auto it = s->Timelines.find(handle);
        auto* meta = GetMetadata(handle);
        if (it == s->Timelines.end() || !it->second || !meta) return;
        fs::path path = s->AssetsDir / meta->Path;
        it->second->Save(path);
        std::error_code ec;
        s->Assets[handle].LastWrite = fs::last_write_time(path, ec);
    }

    AssetHandle AssetManager::CreateTimeline(const fs::path& absolutePath)
    {
        TimelineAsset timeline;
        if (!timeline.Save(absolutePath)) return AssetHandle::None();
        return Import(absolutePath);
    }

    Ref<AnimationClipAsset> AssetManager::GetAnimationClip(AssetHandle handle)
    {
        if (!handle.IsValid() || !s) return nullptr;
        if (auto it = s->AnimationClips.find(handle); it != s->AnimationClips.end()) return it->second;
        auto* meta = GetMetadata(handle);
        if (!meta || meta->Type != AssetType::AnimationClip) return nullptr;
        auto clip = AnimationClipAsset::Load(s->AssetsDir / meta->Path);
        s->AnimationClips[handle] = clip;
        return clip;
    }

    void AssetManager::SaveAnimationClip(AssetHandle handle)
    {
        auto it = s->AnimationClips.find(handle);
        auto* meta = GetMetadata(handle);
        if (it == s->AnimationClips.end() || !it->second || !meta) return;
        fs::path path = s->AssetsDir / meta->Path;
        it->second->Save(path);
        std::error_code ec;
        s->Assets[handle].LastWrite = fs::last_write_time(path, ec);
        s->ClipIndexDirty = true; // (its model may have changed)
        ++s->ClipVersion;
    }

    AssetHandle AssetManager::CreateAnimationClip(const fs::path& absolutePath, const AnimationClipAsset& clip)
    {
        if (!clip.Save(absolutePath)) return AssetHandle::None();
        return Import(absolutePath);
    }

    const std::vector<AssetHandle>& AssetManager::ClipsFor(AssetHandle model)
    {
        static const std::vector<AssetHandle> none;
        if (!s) return none;
        if (s->ClipIndexDirty)
        {
            // (clips made for no model, under None: generic clips for Animators on objects without a skeleton)
            s->ClipsByModel.clear();
            for (AssetHandle h : GetAssetsOfType(AssetType::AnimationClip))
                if (auto clip = GetAnimationClip(h)) s->ClipsByModel[clip->Model].push_back(h);
            s->ClipIndexDirty = false;
        }
        auto it = s->ClipsByModel.find(model);
        return it == s->ClipsByModel.end() ? none : it->second;
    }

    uint64_t AssetManager::ClipsVersion() { return s ? s->ClipVersion : 0; }

    void AssetManager::TouchAnimationClips()
    {
        if (!s) return;
        s->ClipIndexDirty = true;
        ++s->ClipVersion;
    }

    Ref<SpriteAtlas> AssetManager::GetSpriteAtlas(AssetHandle handle)
    {
        if (!handle.IsValid() || !s) return nullptr;
        if (auto it = s->SpriteAtlases.find(handle); it != s->SpriteAtlases.end()) return it->second;
        auto* meta = GetMetadata(handle);
        if (!meta || meta->Type != AssetType::SpriteAtlas) return nullptr;
        auto atlas = SpriteAtlas::Load(s->AssetsDir / meta->Path);
        atlas->Pack();
        atlas->CreateTexture();
        s->SpriteAtlases[handle] = atlas;
        s->AtlasIndexDirty = true;
        return atlas;
    }

    void AssetManager::SaveSpriteAtlas(AssetHandle handle)
    {
        auto it = s->SpriteAtlases.find(handle);
        auto* meta = GetMetadata(handle);
        if (it == s->SpriteAtlases.end() || !it->second || !meta) return;
        fs::path path = s->AssetsDir / meta->Path;
        it->second->Save(path);
        std::error_code ec;
        s->Assets[handle].LastWrite = fs::last_write_time(path, ec);
        it->second->Pack();
        it->second->CreateTexture();
        s->AtlasIndexDirty = true;
    }

    AssetHandle AssetManager::CreateSpriteAtlas(const fs::path& absolutePath, const std::vector<AssetHandle>& sprites)
    {
        SpriteAtlas atlas;
        atlas.Sprites = sprites;
        if (!atlas.Save(absolutePath)) return AssetHandle::None();
        return Import(absolutePath);
    }

    std::vector<std::string> AssetManager::GetLabels(AssetHandle handle)
    {
        std::vector<std::string> out;
        const AssetMetadata* meta = GetMetadata(handle);
        if (!meta) return out;
        for (const auto& l : meta->ImportSettings.value("labels", json::array()))
            if (l.is_string() && !l.get<std::string>().empty()) out.push_back(l.get<std::string>());
        return out;
    }

    void AssetManager::SetLabels(AssetHandle handle, const std::vector<std::string>& labels)
    {
        auto it = s->Assets.find(handle);
        if (it == s->Assets.end()) return;
        std::vector<std::string> unique;
        for (const std::string& l : labels)
            if (!l.empty() && std::find(unique.begin(), unique.end(), l) == unique.end()) unique.push_back(l);
        if (unique.empty()) it->second.ImportSettings.erase("labels");
        else it->second.ImportSettings["labels"] = unique;
        WriteMeta(it->second); // the asset itself is unchanged: nothing to import again
    }

    std::vector<std::string> AssetManager::GetAllLabels()
    {
        std::vector<std::string> all;
        for (auto& [h, meta] : s->Assets)
            for (const std::string& l : GetLabels(h))
                if (std::find(all.begin(), all.end(), l) == all.end()) all.push_back(l);
        std::sort(all.begin(), all.end());
        return all;
    }

    bool AssetManager::Matches(const AssetMetadata& meta, const std::string& query)
    {
        auto lower = [](std::string t) {
            std::string out;
            for (char c : t)
                if (c != ' ') out += (char)std::tolower((unsigned char)c);
            return out;
        };
        std::string name = lower(meta.Path.filename().string());
        std::string type = lower(AssetTypeToString(meta.Type));
        std::vector<std::string> labels;
        for (const auto& l : meta.ImportSettings.value("labels", json::array()))
            if (l.is_string()) labels.push_back(lower(l.get<std::string>()));
        size_t start = 0;
        bool any = false;
        while (start < query.size())
        {
            size_t end = query.find(' ', start);
            if (end == std::string::npos) end = query.size();
            std::string term = lower(query.substr(start, end - start));
            start = end + 1;
            if (term.empty()) continue;
            any = true;
            if (term.rfind("t:", 0) == 0)
            {
                if (type.rfind(term.substr(2), 0) != 0) return false;
            }
            else if (term.rfind("l:", 0) == 0)
            {
                if (std::find(labels.begin(), labels.end(), term.substr(2)) == labels.end()) return false;
            }
            else if (name.find(term) == std::string::npos)
                return false;
        }
        return any;
    }

    std::vector<AssetHandle> AssetManager::Search(const std::string& query)
    {
        std::vector<std::pair<std::string, AssetHandle>> found;
        for (auto& [h, meta] : s->Assets)
            if (!s->SubAssetNames.contains(h) && Matches(meta, query)) found.push_back({ meta.Path.generic_string(), h });
        std::sort(found.begin(), found.end());
        std::vector<AssetHandle> out;
        for (auto& f : found) out.push_back(f.second);
        return out;
    }

    const std::vector<SpriteRect>& AssetManager::GetSprites(AssetHandle texture)
    {
        static const std::vector<SpriteRect> none;
        if (!s || !texture.IsValid()) return none;
        if (auto it = s->SpriteRects.find(texture); it != s->SpriteRects.end()) return it->second;
        const AssetMetadata* meta = GetMetadata(texture);
        if (!meta || meta->Type != AssetType::Texture) return none;
        return s->SpriteRects[texture] = SpriteSlicing::FromJson(meta->ImportSettings.value("sprites", json::array()));
    }

    const SpriteRect* AssetManager::FindSprite(AssetHandle texture, const std::string& name)
    {
        for (const SpriteRect& r : GetSprites(texture))
            if (r.Name == name) return &r;
        return nullptr;
    }

    Ref<Texture2D> AssetManager::FindInAtlas(AssetHandle texture, glm::vec4& region)
    {
        if (!s || !texture.IsValid()) return nullptr;
        if (s->AtlasIndexDirty)
        {
            s->AtlasIndexDirty = false;
            s->AtlasIndex.clear();
            for (auto& [h, meta] : s->Assets)
            {
                if (meta.Type != AssetType::SpriteAtlas) continue;
                auto atlas = GetSpriteAtlas(h);
                if (!atlas) continue;
                for (auto& [sprite, rect] : atlas->Regions) s->AtlasIndex.emplace(sprite, std::make_pair(h, rect));
            }
            s->AtlasIndexDirty = false; // (packing above marked it again)
        }
        auto it = s->AtlasIndex.find(texture);
        if (it == s->AtlasIndex.end()) return nullptr;
        auto atlas = s->SpriteAtlases.find(it->second.first);
        if (atlas == s->SpriteAtlases.end() || !atlas->second || !atlas->second->Texture)
        {
            s->AtlasIndexDirty = true; // unloaded since
            return nullptr;
        }
        region = it->second.second;
        return atlas->second->Texture;
    }

    Ref<Texture2D> AssetManager::GetCircleTexture()
    {
        if (!s->Circle)
        {
            constexpr int N = 64;
            std::vector<uint32_t> pixels(N * N);
            for (int y = 0; y < N; ++y)
                for (int x = 0; x < N; ++x)
                {
                    float dx = x + 0.5f - N * 0.5f, dy = y + 0.5f - N * 0.5f;
                    float a = std::clamp(N * 0.5f - 1.0f - std::sqrt(dx * dx + dy * dy), 0.0f, 1.0f);
                    pixels[y * N + x] = 0x00FFFFFFu | (uint32_t(a * 255.0f) << 24);
                }
            TextureSettings ts;
            ts.SRGB = false;
            s->Circle = MakeRef<Texture2D>(pixels.data(), N, N, ts, "UICircle");
        }
        return s->Circle;
    }

    Ref<Font> AssetManager::GetDefaultFont()
    {
        if (!s->DefaultFont) s->DefaultFont = Font::LoadFromMemory(kDefaultFontData, (size_t)kDefaultFontData_size, "Roboto-Medium (built-in)");
        return s->DefaultFont;
    }

    Ref<Texture2D> AssetManager::GetWhiteTexture()
    {
        if (!s->White) s->White = Texture2D::CreateSolid(0xFFFFFFFFu, "White");
        return s->White;
    }

    Ref<Texture2D> AssetManager::GetCheckerTexture()
    {
        if (!s->Checker)
        {
            std::vector<uint32_t> checker(64 * 64);
            for (uint32_t y = 0; y < 64; ++y)
                for (uint32_t x = 0; x < 64; ++x)
                    checker[y * 64 + x] = ((x / 8 + y / 8) % 2) ? 0xFFCCCCCC : 0xFF888888;
            TextureSettings ts;
            ts.Filter = TextureFilter::Point;
            s->Checker = MakeRef<Texture2D>(checker.data(), 64, 64, ts, "Checker");
        }
        return s->Checker;
    }
}
