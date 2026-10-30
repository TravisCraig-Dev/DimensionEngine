#include "Dimension/Asset/AssetDependencies.h"
#include "Dimension/Asset/AssetManager.h"
#include "Dimension/Core/Log.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <fstream>
#include <sstream>

namespace Dimension::AssetDependencies
{
    namespace
    {
        using json = nlohmann::json;

        void ScanJson(const json& j, std::vector<AssetHandle>& found)
        {
            if (j.is_number_unsigned() || j.is_number_integer())
            {
                uint64_t v = j.is_number_unsigned() ? j.get<uint64_t>() : (uint64_t)std::max<int64_t>(0, j.get<int64_t>());
                if (v >= 1024 && AssetManager::GetMetadata(Guid(v))) found.push_back(Guid(v));
            }
            else if (j.is_array() || j.is_object())
                for (const auto& item : j) ScanJson(item, found);
        }

        std::string LowerExtension(const fs::path& path)
        {
            std::string ext = path.extension().string();
            for (auto& c : ext) c = (char)std::tolower((unsigned char)c);
            return ext;
        }

        bool IsJsonAsset(const fs::path& path)
        {
            std::string ext = LowerExtension(path);
            return ext == ".scene" || ext == ".prefab" || ext == ".mat" || ext == ".controller" || ext == ".spriteatlas" ||
                   ext == ".timeline" || ext == ".preset";
        }

        // Relative URIs inside a .gltf (buffers and images), resolved against the model's folder (Assets-relative).
        void GltfFiles(const fs::path& absolute, const fs::path& folder, std::vector<fs::path>& out)
        {
            try
            {
                json j = json::parse(std::ifstream(absolute));
                for (const char* key : { "buffers", "images" })
                    for (const auto& item : j.value(key, json::array()))
                    {
                        std::string uri = item.value("uri", std::string());
                        if (!uri.empty() && uri.rfind("data:", 0) != 0) out.push_back((folder / fs::u8path(uri)).lexically_normal());
                    }
            }
            catch (const std::exception& e)
            {
                Log::Warn("Could not read '{}' for build dependencies: {}", absolute.string(), e.what());
            }
        }

        // OBJ: its material libraries and the texture maps they name.
        void ObjFiles(const fs::path& assets, const fs::path& absolute, const fs::path& folder, std::vector<fs::path>& out)
        {
            std::ifstream in(absolute);
            std::string line;
            while (std::getline(in, line))
            {
                if (line.rfind("mtllib ", 0) != 0) continue;
                std::string name = line.substr(7);
                while (!name.empty() && (name.back() == '\r' || name.back() == ' ')) name.pop_back();
                fs::path mtl = (folder / name).lexically_normal();
                out.push_back(mtl);
                std::ifstream mtlIn(assets / mtl);
                std::string m;
                while (std::getline(mtlIn, m))
                    if (m.rfind("map_", 0) == 0 || m.rfind("bump ", 0) == 0 || m.rfind("norm ", 0) == 0)
                    {
                        std::istringstream words(m);
                        std::string word, last;
                        while (words >> word) last = word; // the file name ends the line (after any options)
                        if (!last.empty()) out.push_back((folder / last).lexically_normal());
                    }
            }
        }
    }

    std::vector<AssetHandle> References(const json& value)
    {
        std::vector<AssetHandle> found, out;
        ScanJson(value, found);
        for (AssetHandle h : found)
            if (std::find(out.begin(), out.end(), h) == out.end()) out.push_back(h);
        return out;
    }

    std::vector<AssetHandle> Direct(AssetHandle asset)
    {
        const AssetMetadata* meta = AssetManager::GetMetadata(asset);
        if (!meta) return {};
        json all = json::array({ meta->ImportSettings });
        if (IsJsonAsset(meta->Path))
        {
            std::ifstream in(AssetManager::GetAssetsDirectory() / meta->Path);
            json j = json::parse(in, nullptr, false);
            if (!j.is_discarded()) all.push_back(std::move(j));
        }
        std::vector<AssetHandle> out = References(all);
        std::erase(out, asset);
        return out;
    }

    std::vector<AssetHandle> UsedBy(AssetHandle asset)
    {
        std::vector<AssetHandle> out;
        if (!AssetManager::GetMetadata(asset)) return out;
        for (int type = (int)AssetType::Texture; type <= (int)AssetType::StringTable; ++type)
            for (AssetHandle h : AssetManager::GetAssetsOfType((AssetType)type))
            {
                if (h == asset) continue;
                auto refs = Direct(h);
                if (std::find(refs.begin(), refs.end(), asset) != refs.end()) out.push_back(h);
            }
        std::sort(out.begin(), out.end(), [](AssetHandle a, AssetHandle b) {
            return AssetManager::GetMetadata(a)->Path < AssetManager::GetMetadata(b)->Path;
        });
        return out;
    }

    AssetDependencySet Collect(const std::vector<AssetHandle>& roots, bool resources)
    {
        AssetDependencySet set;
        const fs::path& assets = AssetManager::GetAssetsDirectory();
        std::vector<AssetHandle> pending(roots.begin(), roots.end());

        // Everything under Assets/Resources ships, like Unity's Resources folders.
        for (int type = (int)AssetType::Texture; resources && type <= (int)AssetType::SpriteAtlas; ++type)
            for (AssetHandle h : AssetManager::GetAssetsOfType((AssetType)type))
                if (const AssetMetadata* m = AssetManager::GetMetadata(h))
                {
                    auto first = m->Path.begin();
                    if (first != m->Path.end() && first->string() == "Resources") pending.push_back(h);
                }
        // String tables ship wherever they are: scripts and UI Texts name their keys, not the tables.
        for (AssetHandle h : resources ? AssetManager::GetAssetsOfType(AssetType::StringTable) : std::vector<AssetHandle>{}) pending.push_back(h);

        while (!pending.empty())
        {
            AssetHandle h = pending.back();
            pending.pop_back();
            const AssetMetadata* meta = AssetManager::GetMetadata(h);
            if (!meta || !set.Assets.insert(h).second) continue;
            std::vector<AssetHandle> found;
            ScanJson(meta->ImportSettings, found); // e.g. a model's extracted material slots
            fs::path absolute = assets / meta->Path;
            if (IsJsonAsset(meta->Path))
            {
                try
                {
                    ScanJson(json::parse(std::ifstream(absolute)), found);
                }
                catch (const std::exception& e)
                {
                    Log::Warn("Could not read '{}' for build dependencies: {}", meta->Path.generic_string(), e.what());
                }
            }
            std::string ext = LowerExtension(meta->Path);
            std::vector<fs::path> files;
            if (ext == ".gltf") GltfFiles(absolute, meta->Path.parent_path(), files);
            if (ext == ".obj") ObjFiles(assets, absolute, meta->Path.parent_path(), files);
            for (const fs::path& f : files)
            {
                // A referenced file that is itself an asset (a texture) ships as one; anything else as a plain file.
                if (AssetHandle dep = AssetManager::GetHandle(f); dep.IsValid()) found.push_back(dep);
                else if (std::find(set.ExtraFiles.begin(), set.ExtraFiles.end(), f) == set.ExtraFiles.end()) set.ExtraFiles.push_back(f);
            }
            for (AssetHandle dep : found)
                if (!set.Assets.contains(dep)) pending.push_back(dep);
            // Sprite atlases holding a shipped texture ship too (sprites draw from them).
            if (pending.empty())
                for (AssetHandle atlas : AssetManager::GetAssetsOfType(AssetType::SpriteAtlas))
                {
                    if (set.Assets.contains(atlas)) continue;
                    std::vector<AssetHandle> sprites;
                    try
                    {
                        ScanJson(json::parse(std::ifstream(assets / AssetManager::GetMetadata(atlas)->Path)), sprites);
                    }
                    catch (const std::exception&)
                    {
                    }
                    if (std::any_of(sprites.begin(), sprites.end(), [&](AssetHandle h) { return set.Assets.contains(h); })) pending.push_back(atlas);
                }
        }
        return set;
    }
}
