#include "Dimension/Asset/ImportCache.h"
#include "Dimension/Asset/AssetPrefetch.h"
#include "Dimension/Asset/AssetManager.h"
#include "Dimension/Renderer/Mesh.h"
#include "Dimension/Project/Project.h"
#include "Dimension/Core/Log.h"
#include <atomic>
#include <cstring>
#include <fstream>
#include <thread>
#include <unordered_set>
#include <type_traits>

namespace Dimension::ImportCache
{
    namespace
    {
        // Bump when an importer's output changes, so old entries are not read back.
        constexpr uint32_t kImageVersion = 1, kMeshVersion = 3; // 2: OBJ materials; 3: cubic spline tangents
        std::atomic<bool> s_Enabled{ true };

        uint64_t Hash(const void* data, size_t size, uint64_t h = 1469598103934665603ull)
        {
            for (size_t i = 0; i < size; ++i) h = (h ^ ((const uint8_t*)data)[i]) * 1099511628211ull;
            return h;
        }

        // Files under the project's Assets are keyed by their path in it, so a build (same files, same times) hits the
        // cache it ships with; others by their absolute path.
        std::string KeyPath(const fs::path& source)
        {
            std::error_code ec;
            fs::path full = fs::weakly_canonical(source, ec);
            fs::path assets = Project::GetDirectory().empty() ? fs::path() : fs::weakly_canonical(Project::GetAssetsDirectory(), ec);
            if (!assets.empty())
                if (fs::path rel = full.lexically_relative(assets); !rel.empty() && *rel.begin() != "..") return "assets:" + rel.generic_string();
            return full.generic_string();
        }

        // Cache file for a source: hash of its normalized path, size, write time, the kind and its version.
        fs::path EntryFor(const fs::path& source, const char* kind, uint32_t version, const std::string& variant = {})
        {
            fs::path dir = Directory();
            if (dir.empty() || !s_Enabled) return {};
            std::error_code ec;
            auto size = fs::file_size(source, ec);
            if (ec) return {};
            auto time = fs::last_write_time(source, ec).time_since_epoch().count();
            if (ec) return {};
            std::string path = KeyPath(source);
            uint64_t h = Hash(path.data(), path.size());
            h = Hash(&size, sizeof(size), h);
            h = Hash(&time, sizeof(time), h);
            h = Hash(kind, std::strlen(kind), h);
            h = Hash(&version, sizeof(version), h);
            if (!variant.empty()) h = Hash(variant.data(), variant.size(), h);
            char name[48];
            std::snprintf(name, sizeof(name), "%016llx.%s", (unsigned long long)h, kind);
            return dir / name;
        }

        struct Writer
        {
            std::vector<uint8_t> Bytes;
            void Raw(const void* p, size_t n) { Bytes.insert(Bytes.end(), (const uint8_t*)p, (const uint8_t*)p + n); }
            template <class T> void Pod(const T& v) { static_assert(std::is_trivially_copyable_v<T>); Raw(&v, sizeof(T)); }
            template <class T> void Vec(const std::vector<T>& v) { Pod((uint64_t)v.size()); if (!v.empty()) Raw(v.data(), v.size() * sizeof(T)); }
            void Str(const std::string& s) { Pod((uint64_t)s.size()); Raw(s.data(), s.size()); }
        };

        struct Reader
        {
            const uint8_t* P;
            const uint8_t* End;
            bool Ok = true;
            void Raw(void* out, size_t n)
            {
                if (!Ok || (size_t)(End - P) < n) { Ok = false; return; }
                std::memcpy(out, P, n);
                P += n;
            }
            template <class T> void Pod(T& v) { static_assert(std::is_trivially_copyable_v<T>); Raw(&v, sizeof(T)); }
            uint64_t Count(size_t elementSize)
            {
                uint64_t n = 0;
                Pod(n);
                if (Ok && n > (uint64_t)(End - P) / std::max<size_t>(elementSize, 1)) Ok = false;
                return Ok ? n : 0;
            }
            template <class T> void Vec(std::vector<T>& v) { v.resize(Count(sizeof(T))); if (!v.empty()) Raw(v.data(), v.size() * sizeof(T)); }
            void Str(std::string& s) { s.resize(Count(1)); if (!s.empty()) Raw(s.data(), s.size()); }
        };

        bool ReadFile(const fs::path& file, std::vector<uint8_t>& out)
        {
            std::ifstream in(file, std::ios::binary | std::ios::ate);
            if (!in) return false;
            auto size = (size_t)in.tellg();
            in.seekg(0);
            out.resize(size);
            return size == 0 || (bool)in.read((char*)out.data(), (std::streamsize)size);
        }

        void WriteFile(const fs::path& file, const std::vector<uint8_t>& bytes)
        {
            std::error_code ec;
            fs::create_directories(file.parent_path(), ec);
            // Write then rename, so a reader on another thread never sees half an entry.
            fs::path temp = file;
            temp += "." + std::to_string(std::hash<std::thread::id>()(std::this_thread::get_id())) + ".tmp";
            {
                std::ofstream out(temp, std::ios::binary);
                out.write((const char*)bytes.data(), (std::streamsize)bytes.size());
                if (!out) return;
            }
            fs::rename(temp, file, ec);
            if (ec) fs::remove(temp, ec);
        }
    }

    fs::path Directory()
    {
        const fs::path& project = Project::GetDirectory();
        return project.empty() ? fs::path() : project / ".dimension" / "Cache" / "Import";
    }

    void SetEnabled(bool enabled) { s_Enabled = enabled; }

    bool LoadImage(const fs::path& source, DecodedImage& out)
    {
        fs::path file = EntryFor(source, "image", kImageVersion);
        std::vector<uint8_t> bytes;
        if (file.empty() || !ReadFile(file, bytes)) return false;
        Reader r{ bytes.data(), bytes.data() + bytes.size() };
        r.Pod(out.Width);
        r.Pod(out.Height);
        r.Vec(out.Rgba8);
        r.Vec(out.Rgba32f);
        size_t pixels = (size_t)out.Width * out.Height * 4;
        return r.Ok && (out.Rgba8.size() == pixels || out.Rgba32f.size() == pixels);
    }

    void StoreImage(const fs::path& source, const DecodedImage& image)
    {
        fs::path file = EntryFor(source, "image", kImageVersion);
        if (file.empty()) return;
        Writer w;
        w.Pod(image.Width);
        w.Pod(image.Height);
        w.Vec(image.Rgba8);
        w.Vec(image.Rgba32f);
        WriteFile(file, w.Bytes);
    }

    bool LoadMesh(const fs::path& source, const std::string& variant, MeshData& out)
    {
        fs::path file = EntryFor(source, "mesh", kMeshVersion, variant);
        std::vector<uint8_t> bytes;
        if (file.empty() || !ReadFile(file, bytes)) return false;
        Reader r{ bytes.data(), bytes.data() + bytes.size() };
        MeshData m;
        r.Str(m.Name);
        r.Vec(m.Vertices);
        r.Vec(m.Indices);
        r.Vec(m.Skin);
        r.Vec(m.SubMeshes);
        uint8_t hasSkeleton = 0;
        r.Pod(hasSkeleton);
        if (hasSkeleton)
        {
            m.SkeletonData = MakeRef<Skeleton>();
            r.Pod(m.SkeletonData->RootTransform);
            m.SkeletonData->Joints.resize(r.Count(1));
            for (Joint& j : m.SkeletonData->Joints)
            {
                r.Str(j.Name);
                r.Pod(j.Parent);
                r.Pod(j.Translation);
                r.Pod(j.Rotation);
                r.Pod(j.Scale);
                r.Pod(j.InverseBind);
            }
            m.SkeletonData->ComputeOrder();
        }
        m.Clips.resize(r.Count(1));
        for (AnimationClip& c : m.Clips)
        {
            r.Str(c.Name);
            r.Pod(c.Duration);
            c.Channels.resize(r.Count(1));
            for (AnimationChannel& ch : c.Channels)
            {
                r.Pod(ch.Joint);
                r.Pod(ch.Path);
                r.Pod(ch.Interpolation);
                r.Vec(ch.Times);
                r.Vec(ch.Values);
                r.Vec(ch.InTangents);
                r.Vec(ch.OutTangents);
            }
        }
        m.Materials.resize(r.Count(1));
        for (ImportedMaterial& mat : m.Materials)
        {
            r.Str(mat.Name);
            r.Pod(mat.Values);
            r.Pod(mat.BaseColorImage);
            r.Pod(mat.NormalImage);
            r.Pod(mat.MetallicRoughnessImage);
            r.Pod(mat.EmissionImage);
        }
        m.Images.resize(r.Count(1));
        for (ImportedImage& im : m.Images)
        {
            std::string file;
            r.Str(im.Name);
            r.Str(file);
            if (file.starts_with("assets:")) im.File = Project::GetAssetsDirectory() / fs::u8path(file.substr(7));
            else im.File = fs::u8path(file);
            r.Vec(im.Bytes);
        }
        if (!r.Ok || r.P != r.End) return false;
        out = std::move(m);
        return true;
    }

    void StoreMesh(const fs::path& source, const std::string& variant, const MeshData& m)
    {
        fs::path file = EntryFor(source, "mesh", kMeshVersion, variant);
        if (file.empty()) return;
        static_assert(std::is_trivially_copyable_v<Material> && std::is_trivially_copyable_v<Vertex> && std::is_trivially_copyable_v<SkinVertex>);
        Writer w;
        w.Str(m.Name);
        w.Vec(m.Vertices);
        w.Vec(m.Indices);
        w.Vec(m.Skin);
        w.Vec(m.SubMeshes);
        w.Pod((uint8_t)(m.SkeletonData ? 1 : 0));
        if (m.SkeletonData)
        {
            w.Pod(m.SkeletonData->RootTransform);
            w.Pod((uint64_t)m.SkeletonData->Joints.size());
            for (const Joint& j : m.SkeletonData->Joints)
            {
                w.Str(j.Name);
                w.Pod(j.Parent);
                w.Pod(j.Translation);
                w.Pod(j.Rotation);
                w.Pod(j.Scale);
                w.Pod(j.InverseBind);
            }
        }
        w.Pod((uint64_t)m.Clips.size());
        for (const AnimationClip& c : m.Clips)
        {
            w.Str(c.Name);
            w.Pod(c.Duration);
            w.Pod((uint64_t)c.Channels.size());
            for (const AnimationChannel& ch : c.Channels)
            {
                w.Pod(ch.Joint);
                w.Pod(ch.Path);
                w.Pod(ch.Interpolation);
                w.Vec(ch.Times);
                w.Vec(ch.Values);
                w.Vec(ch.InTangents);
                w.Vec(ch.OutTangents);
            }
        }
        w.Pod((uint64_t)m.Materials.size());
        for (const ImportedMaterial& mat : m.Materials)
        {
            w.Str(mat.Name);
            w.Pod(mat.Values);
            w.Pod(mat.BaseColorImage);
            w.Pod(mat.NormalImage);
            w.Pod(mat.MetallicRoughnessImage);
            w.Pod(mat.EmissionImage);
        }
        w.Pod((uint64_t)m.Images.size());
        for (const ImportedImage& im : m.Images)
        {
            w.Str(im.Name);
            std::string path = im.File.empty() ? std::string() : KeyPath(im.File); // relative to Assets when inside it
            w.Str(path);
            w.Vec(im.Bytes);
        }
        WriteFile(file, w.Bytes);
    }

    size_t ClearUnused(const fs::path& assetsDir)
    {
        fs::path dir = Directory();
        std::error_code ec;
        if (dir.empty() || !fs::exists(dir, ec)) return 0;
        // The live key of every asset file, then everything else goes.
        std::unordered_set<std::string> live;
        for (auto& entry : fs::recursive_directory_iterator(assetsDir, fs::directory_options::skip_permission_denied, ec))
        {
            if (!entry.is_regular_file()) continue;
            for (auto [kind, version] : { std::pair{ "image", kImageVersion }, std::pair{ "mesh", kMeshVersion } })
            {
                // Models are keyed with their import settings too.
                std::string variant = std::string(kind) == "mesh" ? AssetManager::GetMeshImportSettings(AssetManager::GetHandle(entry.path())).Key() : std::string();
                if (fs::path p = EntryFor(entry.path(), kind, version, variant); !p.empty()) live.insert(p.filename().string());
            }
        }
        size_t removed = 0;
        for (auto& entry : fs::directory_iterator(dir, ec))
            if (entry.is_regular_file() && !live.contains(entry.path().filename().string()) && fs::remove(entry.path(), ec)) ++removed;
        return removed;
    }
}
