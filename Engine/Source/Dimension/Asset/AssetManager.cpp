#include "Dimension/Asset/AssetManager.h"
#include "Dimension/Core/Log.h"
#include "Dimension/Renderer/Mesh.h"
#include "Dimension/Renderer/Texture.h"
#include "Dimension/Renderer/Material.h"
#include "Dimension/Renderer/Font.h"
#include "Dimension/Animation/AnimatorController.h"
#include "embedded/kDefaultFontData.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <unordered_map>
#include <unordered_set>

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
            std::unordered_map<AssetHandle, Ref<Material>> Materials;
            std::unordered_map<AssetHandle, Ref<Font>> Fonts;
            std::unordered_map<AssetHandle, Ref<AnimatorController>> Controllers;
            Ref<Font> DefaultFont;
            Ref<Texture2D> White, Checker, Particle;
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
        if (ext == ".wav" || ext == ".mp3" || ext == ".flac" || ext == ".ogg") return AssetType::Audio;
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
        auto tex = Texture2D::LoadFromFile(s->AssetsDir / meta->Path, ts);
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
                mesh = Mesh::LoadFromFile(s->AssetsDir / meta->Path);
        }
        s->Meshes[handle] = mesh;
        return mesh;
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
        controller.States.push_back({ "Idle", "", 1.0f, true, { 0.0f, 0.0f } });
        if (!controller.Save(absolutePath)) return AssetHandle::None();
        return Import(absolutePath);
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
