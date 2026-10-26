#include "Dimension/Asset/AssetManager.h"
#include "Dimension/Physics/Cloth.h"
#include "Dimension/Terrain/Terrain.h"
#include "Dimension/Core/Log.h"
#include "Dimension/Renderer/Mesh.h"
#include "Dimension/Renderer/Texture.h"
#include "Dimension/Asset/AssetPrefetch.h"
#include "Dimension/Renderer/Material.h"
#include "Dimension/Renderer/Font.h"
#include "Dimension/Animation/AnimatorController.h"
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
            std::unordered_map<AssetHandle, Ref<Font>> Fonts;
            std::unordered_map<AssetHandle, Ref<AnimatorController>> Controllers;
            // Model sub-assets (embedded materials/textures), keyed by the model.
            std::unordered_map<AssetHandle, std::vector<AssetHandle>> MeshMaterials;
            std::unordered_map<AssetHandle, std::vector<std::string>> MeshMaterialNames;
            std::unordered_map<AssetHandle, std::vector<AssetHandle>> SubAssets;
            std::unordered_map<AssetHandle, std::string> SubAssetNames;
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
            s->PathToHandle[Key(m.Path)] = m.Handle;
            s->Assets[m.Handle] = std::move(m);
        }

        void Unload(AssetHandle h)
        {
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

        Refresh();
        Log::Info("Asset database: {} assets in '{}'", s->Assets.size(), assetsDirectory.string());
    }

    void AssetManager::Shutdown()
    {
        Cloth::ReleaseMeshes(); // GPU grids, before the device goes
        Terrain::ReleaseRuntimes();
        delete s;
        s = nullptr;
    }

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
        if (auto it = s->Textures.find(handle); it != s->Textures.end()) return it->second;
        auto* meta = GetMetadata(handle);
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
            if (auto* meta = GetMetadata(handle); meta && meta->Type == AssetType::Mesh)
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
        if (!meta || meta->Type != AssetType::AnimatorController) return nullptr;
        auto controller = AnimatorController::Load(s->AssetsDir / meta->Path);
        s->Controllers[handle] = controller;
        return controller;
    }

    void AssetManager::SaveAnimatorController(AssetHandle handle)
    {
        auto it = s->Controllers.find(handle);
        auto* meta = GetMetadata(handle);
        if (it == s->Controllers.end() || !it->second || !meta) return;
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
