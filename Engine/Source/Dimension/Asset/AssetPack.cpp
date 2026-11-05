#include "Dimension/Asset/AssetPack.h"
#include "Dimension/Core/Log.h"
#include "Dimension/Core/PlayerPrefs.h"
#include <cstdlib>
#include <cstring>
#include <fstream>

namespace Dimension
{
    namespace AssetPack
    {
        namespace
        {
            constexpr char kMagic[8] = { 'D', 'I', 'M', 'P', 'A', 'K', '1', '\0' };

            void Fnv(uint64_t& h, const void* data, size_t n)
            {
                const auto* p = static_cast<const uint8_t*>(data);
                for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 1099511628211ull;
            }
            template <class T> void Put(std::ofstream& out, const T& v) { out.write(reinterpret_cast<const char*>(&v), sizeof(T)); }
            template <class T> bool Get(std::ifstream& in, T& v) { return (bool)in.read(reinterpret_cast<char*>(&v), sizeof(T)); }
        }

        std::vector<fs::path> ListFiles(const fs::path& root)
        {
            std::vector<fs::path> out;
            std::error_code ec;
            for (auto& entry : fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec))
                if (entry.is_regular_file()) out.push_back(entry.path().lexically_relative(root));
            std::sort(out.begin(), out.end());
            return out;
        }

        bool Write(const fs::path& pak, const fs::path& root, const std::vector<fs::path>& files)
        {
            // Read everything first: the table needs the sizes, the header the hash.
            std::vector<Entry> entries;
            std::vector<std::vector<char>> data;
            uint64_t hash = 1469598103934665603ull;
            for (const fs::path& rel : files)
            {
                std::ifstream in(root / rel, std::ios::binary);
                if (!in)
                {
                    Log::Error("Packing: cannot read {}", (root / rel).string());
                    return false;
                }
                std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                Entry e;
                e.Path = rel.generic_string();
                e.Size = bytes.size();
                Fnv(hash, e.Path.data(), e.Path.size());
                Fnv(hash, bytes.data(), bytes.size());
                entries.push_back(std::move(e));
                data.push_back(std::move(bytes));
            }
            uint64_t tableSize = 0;
            for (auto& e : entries) tableSize += 4 + e.Path.size() + 16;
            uint64_t offset = sizeof(kMagic) + 4 + 8 + tableSize;
            for (auto& e : entries)
            {
                e.Offset = offset;
                offset += e.Size;
            }
            std::ofstream out(pak, std::ios::binary | std::ios::trunc);
            if (!out) return false;
            out.write(kMagic, sizeof(kMagic));
            Put(out, (uint32_t)entries.size());
            Put(out, hash);
            for (auto& e : entries)
            {
                Put(out, (uint32_t)e.Path.size());
                out.write(e.Path.data(), (std::streamsize)e.Path.size());
                Put(out, e.Offset);
                Put(out, e.Size);
            }
            for (auto& bytes : data) out.write(bytes.data(), (std::streamsize)bytes.size());
            return (bool)out;
        }

        bool Read(const fs::path& pak, std::vector<Entry>& entries, uint64_t& hash)
        {
            std::ifstream in(pak, std::ios::binary);
            char magic[8];
            uint32_t count = 0;
            if (!in.read(magic, sizeof(magic)) || std::memcmp(magic, kMagic, sizeof(kMagic)) != 0 || !Get(in, count) || !Get(in, hash)) return false;
            entries.clear();
            entries.reserve(count);
            for (uint32_t i = 0; i < count; ++i)
            {
                uint32_t length = 0;
                if (!Get(in, length) || length > 4096) return false;
                Entry e;
                e.Path.resize(length);
                if (!in.read(e.Path.data(), length) || !Get(in, e.Offset) || !Get(in, e.Size)) return false;
                // Paths stay inside the folder they unpack into.
                if (e.Path.empty() || e.Path.find("..") != std::string::npos || e.Path[0] == '/' || e.Path.find(':') != std::string::npos) return false;
                entries.push_back(std::move(e));
            }
            return true;
        }

        bool Extract(const fs::path& pak, const fs::path& folder)
        {
            std::vector<Entry> entries;
            uint64_t hash = 0;
            if (!Read(pak, entries, hash)) return false;
            std::ifstream in(pak, std::ios::binary);
            std::vector<char> buffer;
            for (const Entry& e : entries)
            {
                fs::path target = folder / fs::path(e.Path);
                std::error_code ec;
                fs::create_directories(target.parent_path(), ec);
                buffer.resize((size_t)e.Size);
                in.seekg((std::streamoff)e.Offset);
                if (!in.read(buffer.data(), (std::streamsize)e.Size)) return false;
                std::ofstream out(target, std::ios::binary | std::ios::trunc);
                if (!out.write(buffer.data(), (std::streamsize)e.Size)) return false;
            }
            return true;
        }

        fs::path Mount(const fs::path& pak, const std::string& product)
        {
            std::vector<Entry> entries;
            uint64_t hash = 0;
            if (!Read(pak, entries, hash)) return {};
            const char* local = std::getenv("LOCALAPPDATA");
            fs::path base = local ? fs::path(local) : fs::temp_directory_path();
            char name[17];
            std::snprintf(name, sizeof(name), "%016llx", (unsigned long long)hash);
            fs::path folder = base / PlayerData::FolderName(product) / "AssetCache" / name;
            fs::path marker = folder / ".complete";
            if (fs::exists(marker)) return folder;
            std::error_code ec;
            fs::remove_all(folder, ec);
            if (!Extract(pak, folder))
            {
                Log::Error("Unpacking {} failed", pak.string());
                return {};
            }
            std::ofstream(marker) << entries.size();
            Log::Info("Unpacked {} asset files to {}", entries.size(), folder.string());
            return folder;
        }
    }
}
