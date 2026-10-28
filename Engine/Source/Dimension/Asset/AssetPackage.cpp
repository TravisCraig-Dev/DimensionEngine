#include "Dimension/Asset/AssetPackage.h"
#include "Dimension/Asset/AssetDependencies.h"
#include "Dimension/Core/Log.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cstring>
#include <fstream>
#include <sstream>

namespace Dimension::AssetPackage
{
    namespace
    {
        // "DIMPKG" + version, then the manifest's length (8 bytes, little-endian), the manifest (JSON: each file's path,
        // size and .meta) and the files' bytes one after another.
        constexpr char kMagic[8] = { 'D', 'I', 'M', 'P', 'K', 'G', '\0', '\1' };

        std::string ReadFile(const fs::path& path)
        {
            std::ifstream in(path, std::ios::binary);
            std::ostringstream s;
            s << in.rdbuf();
            return s.str();
        }

        uint64_t GuidOf(const std::string& meta)
        {
            if (meta.empty()) return 0;
            try
            {
                return nlohmann::json::parse(meta).value("guid", uint64_t(0));
            }
            catch (const std::exception&)
            {
                return 0;
            }
        }

        bool IsUnder(const fs::path& path, const fs::path& folder)
        {
            auto p = path.begin();
            for (auto f = folder.begin(); f != folder.end(); ++f, ++p)
                if (p == path.end() || *p != *f) return false;
            return true;
        }
    }

    std::vector<AssetHandle> AssetsIn(const fs::path& folder)
    {
        std::vector<AssetHandle> out;
        for (int type = (int)AssetType::Texture; type <= (int)AssetType::Preset; ++type)
            for (AssetHandle h : AssetManager::GetAssetsOfType((AssetType)type))
                if (const AssetMetadata* m = AssetManager::GetMetadata(h); m && IsUnder(m->Path.lexically_normal(), folder.lexically_normal()))
                    out.push_back(h);
        return out;
    }

    std::vector<fs::path> FilesFor(const std::vector<AssetHandle>& assets, bool dependencies)
    {
        std::vector<AssetHandle> roots;
        for (AssetHandle h : assets) roots.push_back(h);
        std::vector<fs::path> files;
        if (dependencies)
        {
            AssetDependencySet set = AssetDependencies::Collect(roots, false);
            for (AssetHandle h : set.Assets)
                if (const AssetMetadata* m = AssetManager::GetMetadata(h)) files.push_back(m->Path);
            for (const fs::path& f : set.ExtraFiles) files.push_back(f);
        }
        else
            for (AssetHandle h : roots)
                if (const AssetMetadata* m = AssetManager::GetMetadata(h)) files.push_back(m->Path);
        // Sub-assets (a model's textures and materials in memory) have no file of their own.
        const fs::path& dir = AssetManager::GetAssetsDirectory();
        std::erase_if(files, [&](const fs::path& f) { return !fs::is_regular_file(dir / f); });
        for (fs::path& f : files) f = f.lexically_normal();
        std::sort(files.begin(), files.end());
        files.erase(std::unique(files.begin(), files.end()), files.end());
        return files;
    }

    bool Export(const std::vector<fs::path>& files, const fs::path& package)
    {
        const fs::path& dir = AssetManager::GetAssetsDirectory();
        nlohmann::json manifest = nlohmann::json::array();
        std::string data;
        for (const fs::path& f : files)
        {
            fs::path absolute = dir / f;
            if (!fs::is_regular_file(absolute)) continue;
            std::string bytes = ReadFile(absolute);
            fs::path metaPath = absolute;
            metaPath += ".meta";
            std::string meta = fs::exists(metaPath) ? ReadFile(metaPath) : std::string();
            manifest.push_back({ { "path", f.generic_string() }, { "size", bytes.size() }, { "meta", meta } });
            data += bytes;
        }
        std::string head = nlohmann::json{ { "version", 1 }, { "files", manifest } }.dump();
        uint64_t length = head.size();
        std::ofstream out(package, std::ios::binary);
        out.write(kMagic, sizeof(kMagic));
        out.write(reinterpret_cast<const char*>(&length), sizeof(length));
        out.write(head.data(), (std::streamsize)head.size());
        out.write(data.data(), (std::streamsize)data.size());
        return (bool)out;
    }

    std::optional<Contents> Read(const fs::path& package)
    {
        std::string all = ReadFile(package);
        if (all.size() < sizeof(kMagic) + 8 || std::memcmp(all.data(), kMagic, sizeof(kMagic)) != 0) return std::nullopt;
        uint64_t length = 0;
        std::memcpy(&length, all.data() + sizeof(kMagic), sizeof(length));
        size_t at = sizeof(kMagic) + 8;
        if (length > all.size() - at) return std::nullopt;
        try
        {
            auto head = nlohmann::json::parse(all.substr(at, length));
            at += length;
            Contents contents;
            for (auto& f : head.at("files"))
            {
                Entry e;
                e.Path = fs::path(f.at("path").get<std::string>()).lexically_normal();
                // Only paths inside the Assets folder.
                if (e.Path.empty() || e.Path.is_absolute() || e.Path.has_root_name() || *e.Path.begin() == "..") return std::nullopt;
                size_t size = f.at("size").get<size_t>();
                if (size > all.size() - at) return std::nullopt;
                e.Data = all.substr(at, size);
                at += size;
                e.Meta = f.value("meta", std::string());
                e.Guid = GuidOf(e.Meta);
                contents.Files.push_back(std::move(e));
            }
            return contents;
        }
        catch (const std::exception& e)
        {
            Log::Error("Invalid package '{}': {}", package.filename().string(), e.what());
            return std::nullopt;
        }
    }

    Status Check(const Entry& entry)
    {
        if (entry.Guid)
            if (const AssetMetadata* m = AssetManager::GetMetadata(AssetHandle(entry.Guid)); m && m->Path.lexically_normal() != entry.Path)
                return Status::GuidTaken;
        fs::path absolute = AssetManager::GetAssetsDirectory() / entry.Path;
        if (!fs::exists(absolute)) return Status::New;
        return ReadFile(absolute) == entry.Data ? Status::Same : Status::Changed;
    }

    int Import(const Contents& contents, const std::vector<bool>& chosen)
    {
        const fs::path& dir = AssetManager::GetAssetsDirectory();
        int written = 0;
        for (size_t i = 0; i < contents.Files.size(); ++i)
        {
            if (!chosen.empty() && (i >= chosen.size() || !chosen[i])) continue;
            const Entry& e = contents.Files[i];
            Status status = Check(e);
            fs::path absolute = dir / e.Path;
            std::error_code ec;
            fs::create_directories(absolute.parent_path(), ec);
            std::ofstream(absolute, std::ios::binary).write(e.Data.data(), (std::streamsize)e.Data.size());
            // A new file brings its .meta (its GUID, so references to it hold) unless another asset holds that GUID: then
            // it gets a new one. A file replacing one of the project's keeps the project's .meta (its references hold).
            fs::path metaPath = absolute;
            metaPath += ".meta";
            if (status == Status::New && !e.Meta.empty()) std::ofstream(metaPath, std::ios::binary) << e.Meta;
            ++written;
        }
        AssetManager::Refresh(); // new files in, replaced ones read again
        return written;
    }
}
