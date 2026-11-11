#include "Dimension/Scripting/ManagedBuild.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <initializer_list>

namespace Dimension::ManagedBuild
{
    namespace
    {
        // A PE file read whole: its sections (to turn RVAs into file offsets) and the CLI header's place.
        struct PeImage
        {
            std::vector<uint8_t> Data;
            struct Section { uint32_t Address, Size, Raw, RawSize; };
            std::vector<Section> Sections;
            uint32_t CliRva = 0;

            template <typename T> T Get(size_t at) const
            {
                T v{};
                if (at + sizeof(T) <= Data.size()) std::memcpy(&v, Data.data() + at, sizeof(T));
                return v;
            }
            size_t Offset(uint32_t rva) const
            {
                for (const Section& s : Sections)
                    if (rva >= s.Address && rva < s.Address + std::max(s.Size, s.RawSize)) return (size_t)rva - s.Address + s.Raw;
                return SIZE_MAX;
            }
            bool Load(const fs::path& path)
            {
                std::ifstream in(path, std::ios::binary);
                if (!in) return false;
                Data.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
                if (Data.size() < 0x40 || Data[0] != 'M' || Data[1] != 'Z') return false;
                uint32_t pe = Get<uint32_t>(0x3C);
                if (Get<uint32_t>(pe) != 0x00004550) return false; // "PE\0\0"
                size_t coff = pe + 4, optional = coff + 20;
                uint16_t sections = Get<uint16_t>(coff + 2), optionalSize = Get<uint16_t>(coff + 16);
                bool plus = Get<uint16_t>(optional) == 0x20b;
                uint32_t directories = Get<uint32_t>(optional + (plus ? 108 : 92));
                if (directories > 14) CliRva = Get<uint32_t>(optional + (plus ? 112 : 96) + 14 * 8);
                for (size_t s = 0, at = optional + optionalSize; s < sections; ++s, at += 40)
                    Sections.push_back({ Get<uint32_t>(at + 12), Get<uint32_t>(at + 8), Get<uint32_t>(at + 20), Get<uint32_t>(at + 16) });
                return true;
            }
        };

        // The AssemblyRef names in an assembly's metadata tables (ECMA-335 II.22, II.24).
        bool AssemblyRefs(const PeImage& pe, std::vector<std::string>& out)
        {
            if (!pe.CliRva) return false;
            size_t cli = pe.Offset(pe.CliRva);
            if (cli == SIZE_MAX) return false;
            size_t root = pe.Offset(pe.Get<uint32_t>(cli + 8));
            if (root == SIZE_MAX || pe.Get<uint32_t>(root) != 0x424A5342) return false; // "BSJB"
            size_t at = root + 16 + pe.Get<uint32_t>(root + 12);
            uint16_t streams = pe.Get<uint16_t>(at + 2);
            at += 4;
            size_t tables = SIZE_MAX, strings = SIZE_MAX;
            for (uint16_t s = 0; s < streams && at < pe.Data.size(); ++s)
            {
                uint32_t offset = pe.Get<uint32_t>(at);
                const char* name = (const char*)pe.Data.data() + at + 8;
                size_t length = strnlen(name, std::min<size_t>(32, pe.Data.size() - at - 8));
                if (!std::strcmp(name, "#~") || !std::strcmp(name, "#-")) tables = root + offset;
                else if (!std::strcmp(name, "#Strings")) strings = root + offset;
                at += 8 + ((length + 1 + 3) & ~size_t(3));
            }
            if (tables == SIZE_MAX || strings == SIZE_MAX) return false;
            uint8_t heaps = pe.Get<uint8_t>(tables + 6);
            uint64_t valid = pe.Get<uint64_t>(tables + 8);
            std::array<uint32_t, 64> rows{};
            size_t p = tables + 24;
            for (int t = 0; t < 64; ++t)
                if (valid & (1ull << t)) rows[t] = pe.Get<uint32_t>(p), p += 4;
            if (heaps & 0x40) p += 4; // (edit-and-continue extra data)
            const size_t str = heaps & 1 ? 4 : 2, guid = heaps & 2 ? 4 : 2, blob = heaps & 4 ? 4 : 2;
            auto index = [&](int t) -> size_t { return rows[t] > 0xFFFF ? 4 : 2; };
            auto coded = [&](std::initializer_list<int> ts, int bits) -> size_t {
                uint32_t most = 0;
                for (int t : ts) most = std::max(most, rows[t]);
                return most < (1u << (16 - bits)) ? 2 : 4;
            };
            const size_t typeDefOrRef = coded({ 2, 1, 0x1B }, 2), hasConstant = coded({ 4, 8, 0x17 }, 2);
            const size_t hasCustomAttribute = coded({ 6, 4, 1, 2, 8, 9, 0x0A, 0, 0x0E, 0x17, 0x14, 0x11, 0x1A, 0x1B, 0x20, 0x23, 0x26, 0x27, 0x28, 0x2A, 0x2C, 0x2B }, 5);
            const size_t hasFieldMarshal = coded({ 4, 8 }, 1), hasDeclSecurity = coded({ 2, 6, 0x20 }, 2), memberRefParent = coded({ 2, 1, 0x1A, 6, 0x1B }, 3);
            const size_t hasSemantics = coded({ 0x14, 0x17 }, 1), methodDefOrRef = coded({ 6, 0x0A }, 1), memberForwarded = coded({ 4, 6 }, 1);
            const size_t customAttributeType = coded({ 6, 0x0A }, 3), resolutionScope = coded({ 0, 0x1A, 0x23, 1 }, 2);
            // Row sizes of the tables before AssemblyRef (0x23).
            const size_t sizes[0x23] = {
                2 + str + guid * 3,                                          // Module
                resolutionScope + str * 2,                                   // TypeRef
                4 + str * 2 + typeDefOrRef + index(4) + index(6),            // TypeDef
                index(4), 2 + str + blob,                                    // FieldPtr, Field
                index(6), 4 + 2 + 2 + str + blob + index(8),                 // MethodPtr, MethodDef
                index(8), 2 + 2 + str,                                       // ParamPtr, Param
                index(2) + typeDefOrRef,                                     // InterfaceImpl
                memberRefParent + str + blob,                                // MemberRef
                2 + hasConstant + blob,                                      // Constant
                hasCustomAttribute + customAttributeType + blob,             // CustomAttribute
                hasFieldMarshal + blob,                                      // FieldMarshal
                2 + hasDeclSecurity + blob,                                  // DeclSecurity
                2 + 4 + index(2), 4 + index(4),                              // ClassLayout, FieldLayout
                blob,                                                        // StandAloneSig
                index(2) + index(0x14), index(0x14), 2 + str + typeDefOrRef, // EventMap, EventPtr, Event
                index(2) + index(0x17), index(0x17), 2 + str + blob,         // PropertyMap, PropertyPtr, Property
                2 + index(6) + hasSemantics,                                 // MethodSemantics
                index(2) + methodDefOrRef * 2,                               // MethodImpl
                str, blob,                                                   // ModuleRef, TypeSpec
                2 + memberForwarded + str + index(0x1A),                     // ImplMap
                4 + index(4),                                                // FieldRVA
                8, 4,                                                        // ENCLog, ENCMap
                4 + 8 + 4 + blob + str * 2,                                  // Assembly
                4, 12,                                                       // AssemblyProcessor, AssemblyOS
            };
            for (int t = 0; t < 0x23; ++t) p += sizes[t] * rows[t];
            const size_t row = 8 + 4 + blob + str * 2 + blob;
            for (uint32_t r = 0; r < rows[0x23]; ++r, p += row)
            {
                size_t name = str == 4 ? pe.Get<uint32_t>(p + 12 + blob) : pe.Get<uint16_t>(p + 12 + blob);
                if (strings + name >= pe.Data.size()) return false;
                const char* text = (const char*)pe.Data.data() + strings + name;
                out.emplace_back(text, strnlen(text, pe.Data.size() - strings - name));
            }
            return true;
        }

        // A command's output (stderr too); its exit code.
        int Run(const std::string& command, std::string* output)
        {
            // Outer quotes are stripped by cmd /c (used by _popen).
            FILE* pipe = _popen(("\"" + command + " 2>&1\"").c_str(), "r");
            if (!pipe) return -1;
            char buffer[512];
            while (std::fgets(buffer, sizeof(buffer), pipe))
                if (output) *output += buffer;
            return _pclose(pipe);
        }

        std::array<int, 3> Version(const std::string& v)
        {
            std::array<int, 3> n{};
            std::sscanf(v.c_str(), "%d.%d.%d", &n[0], &n[1], &n[2]);
            return n;
        }

        fs::path NuGetPackages()
        {
            if (const char* custom = std::getenv("NUGET_PACKAGES"); custom && *custom) return custom;
            const char* home = std::getenv("USERPROFILE");
            return fs::path(home ? home : "") / ".nuget" / "packages";
        }
    }

    bool ReadAssemblyReferences(const fs::path& dll, std::vector<std::string>& references)
    {
        PeImage pe;
        references.clear();
        return pe.Load(dll) && AssemblyRefs(pe, references);
    }

    bool IsReadyToRun(const fs::path& dll)
    {
        PeImage pe;
        if (!pe.Load(dll) || !pe.CliRva) return false;
        size_t cli = pe.Offset(pe.CliRva);
        if (cli == SIZE_MAX) return false;
        uint32_t header = pe.Get<uint32_t>(cli + 64); // ManagedNativeHeader
        size_t at = header ? pe.Offset(header) : SIZE_MAX;
        return at != SIZE_MAX && pe.Get<uint32_t>(at) == 0x00525452; // "RTR"
    }

    std::set<std::string> FrameworkClosure(const std::vector<fs::path>& roots, const fs::path& frameworkDir)
    {
        // What the runtime loads without a reference: CoreLib, and the stack trace helpers (line numbers in
        // exception traces come from portable PDBs read through System.Reflection.Metadata).
        std::set<std::string> found;
        std::deque<fs::path> queue(roots.begin(), roots.end());
        for (const char* name : { "System.Private.CoreLib", "System.Runtime", "System.Diagnostics.StackTrace", "System.Reflection.Metadata",
                                  "System.Collections.Immutable", "System.Runtime.InteropServices", "System.Runtime.Loader" })
            if (fs::exists(frameworkDir / (std::string(name) + ".dll")) && found.insert(name).second) queue.push_back(frameworkDir / (std::string(name) + ".dll"));
        std::vector<std::string> references;
        while (!queue.empty())
        {
            fs::path dll = queue.front();
            queue.pop_front();
            if (!ReadAssemblyReferences(dll, references)) continue;
            for (const std::string& name : references)
            {
                fs::path candidate = frameworkDir / (name + ".dll");
                if (!found.count(name) && fs::exists(candidate))
                {
                    found.insert(name);
                    queue.push_back(candidate);
                }
            }
        }
        return found;
    }

    bool TrimFramework(const fs::path& frameworkDir, const std::vector<fs::path>& roots, uintmax_t* removedBytes, int* removedFiles)
    {
        std::error_code ec;
        if (!fs::exists(frameworkDir / "System.Private.CoreLib.dll")) return false;
        std::set<std::string> keep = FrameworkClosure(roots, frameworkDir);
        std::set<std::string> removed; // file names
        uintmax_t bytes = 0;
        std::vector<std::string> references;
        for (const auto& entry : fs::directory_iterator(frameworkDir, ec))
        {
            if (!entry.is_regular_file()) continue;
            fs::path file = entry.path();
            std::string stem = file.stem().string(), ext = file.extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return (char)std::tolower(c); });
            if (ext != ".dll") continue;
            bool managed = ReadAssemblyReferences(file, references);
            bool drop = managed ? !keep.count(stem) : stem == "msquic" && !keep.count("System.Net.Quic");
            if (!drop) continue;
            uintmax_t size = entry.file_size(ec);
            if (fs::remove(file, ec))
            {
                bytes += size;
                removed.insert(file.filename().string());
            }
        }
        // The framework's manifest lists only what is left (the host checks the files it names).
        for (const auto& entry : fs::directory_iterator(frameworkDir, ec))
        {
            if (entry.path().extension() != ".json" || entry.path().stem().extension() != ".deps") continue;
            nlohmann::json deps;
            try
            {
                std::ifstream in(entry.path());
                deps = nlohmann::json::parse(in);
                for (auto& [target, libraries] : deps["targets"].items())
                    for (auto& [library, assets] : libraries.items())
                        for (const char* kind : { "runtime", "native" })
                            if (assets.contains(kind))
                                for (auto it = assets[kind].begin(); it != assets[kind].end();)
                                    it = removed.count(fs::path(it.key()).filename().string()) ? assets[kind].erase(it) : std::next(it);
                std::ofstream out(entry.path());
                out << deps.dump(2);
            }
            catch (const std::exception&)
            {
                return false;
            }
        }
        if (removedBytes) *removedBytes = bytes;
        if (removedFiles) *removedFiles = (int)removed.size();
        return true;
    }

    fs::path FindCrossgen(const std::string& runtimeVersion, bool restore)
    {
        auto search = [&]() -> fs::path {
            fs::path packages = NuGetPackages() / "microsoft.netcore.app.crossgen2.win-x64";
            fs::path exact = packages / runtimeVersion / "tools" / "crossgen2.exe";
            if (fs::exists(exact)) return exact;
            fs::path best;
            std::error_code ec;
            for (const auto& entry : fs::directory_iterator(packages, ec))
            {
                std::string v = entry.path().filename().string();
                if (Version(v)[0] != Version(runtimeVersion)[0] || !fs::exists(entry.path() / "tools" / "crossgen2.exe")) continue;
                if (best.empty() || Version(v) > Version(best.parent_path().parent_path().filename().string())) best = entry.path() / "tools" / "crossgen2.exe";
            }
            return best;
        };
        fs::path found = search();
        if (!found.empty() || !restore) return found;
        // The SDK downloads the crossgen2 pack when restoring a project published ReadyToRun.
        std::error_code ec;
        fs::path dir = fs::temp_directory_path(ec) / "dimension_crossgen_restore";
        fs::create_directories(dir, ec);
        {
            std::ofstream project(dir / "Restore.csproj");
            project << "<Project Sdk=\"Microsoft.NET.Sdk\"><PropertyGroup><TargetFramework>net" << Version(runtimeVersion)[0]
                    << ".0</TargetFramework><RuntimeIdentifier>win-x64</RuntimeIdentifier><PublishReadyToRun>true</PublishReadyToRun>"
                    << "</PropertyGroup></Project>";
        }
        Run("dotnet restore \"" + (dir / "Restore.csproj").string() + "\" --nologo -v q", nullptr);
        fs::remove_all(dir, ec);
        return search();
    }

    bool CompileReadyToRun(const fs::path& crossgen, const fs::path& input, const fs::path& output, const std::vector<fs::path>& references,
                           std::string* log)
    {
        std::error_code ec;
        fs::path temp = output;
        temp += ".r2r";
        std::string command = "\"" + crossgen.string() + "\" \"" + input.string() + "\" -o \"" + temp.string() + "\" --targetos windows --targetarch x64 -O";
        for (const fs::path& reference : references) // folders (every assembly in them) or assemblies
            command += " -r \"" + (fs::is_directory(reference) ? reference / "*.dll" : reference).string() + "\"";
        int code = Run(command, log);
        if (code != 0 || !fs::exists(temp))
        {
            fs::remove(temp, ec);
            return false;
        }
        fs::rename(temp, output, ec);
        if (ec)
        {
            fs::remove(output, ec);
            fs::rename(temp, output, ec);
        }
        return !ec && IsReadyToRun(output);
    }
}
