#pragma once
#include "Dimension/Core/Base.h"
#include <set>
#include <string>
#include <vector>

namespace Dimension
{
    // Preparing C# for a built player: scripts compiled ahead of time (ReadyToRun, so the game does not JIT them at
    // start) and the bundled .NET runtime cut down to the framework assemblies the scripts can reach.
    namespace ManagedBuild
    {
        // A managed assembly's references (its AssemblyRef names) from its metadata. False when the file is not a
        // managed assembly (a native DLL, or unreadable).
        bool ReadAssemblyReferences(const fs::path& dll, std::vector<std::string>& references);
        // Whether an assembly carries ReadyToRun code (a crossgen2 image).
        bool IsReadyToRun(const fs::path& dll);

        // The framework assemblies (names, without .dll) reachable from `roots` by their references, resolved in
        // `frameworkDir`, plus those the runtime loads by itself (CoreLib, stack traces with line numbers).
        std::set<std::string> FrameworkClosure(const std::vector<fs::path>& roots, const fs::path& frameworkDir);
        // Removes the framework's managed assemblies outside that closure (natives stay, but msquic only with
        // System.Net.Quic) and rewrites its .deps.json to match. `removedBytes` / `removedFiles` report the saving.
        bool TrimFramework(const fs::path& frameworkDir, const std::vector<fs::path>& roots, uintmax_t* removedBytes = nullptr,
                           int* removedFiles = nullptr);

        // crossgen2 for the runtime version (Microsoft.NETCore.App.Crossgen2 from the NuGet cache; the same major
        // version's newest when that one is missing). `restore`: when there is none, have the .NET SDK download it
        // (a restore of a ReadyToRun project). Empty when not found.
        fs::path FindCrossgen(const std::string& runtimeVersion, bool restore);
        // Compiles `input` to ReadyToRun code at `output` (may be the same file; Windows x64, optimized). References:
        // folders (every assembly in them) or assemblies. `log` gets crossgen2's output.
        bool CompileReadyToRun(const fs::path& crossgen, const fs::path& input, const fs::path& output,
                               const std::vector<fs::path>& references, std::string* log = nullptr);
    }
}
