#pragma once
#include "Dimension/Core/Base.h"

namespace Dimension
{
    // Layout of a game project's C# scripts:
    //   <Project>/Scripts/GameScripts.csproj        (generated if missing; open it in Rider/VS)
    //   <Project>/Scripts/Directory.Build.props     (regenerated: points at the engine's managed API)
    //   <Project>/Scripts/**/*.cs                   (your Behaviour classes)
    //   <Project>/.dimension/ScriptAssemblies/GameScripts.dll  (build output loaded by editor/player)
    namespace CSharpProject
    {
        fs::path ScriptsDirectory(const fs::path& projectDir);
        fs::path ProjectFile(const fs::path& projectDir);
        fs::path AssemblyPath(const fs::path& projectDir);

        // Writes the csproj (if missing) and Directory.Build.props (always) for `managedDir`.
        void Scaffold(const fs::path& projectDir, const fs::path& managedDir);
        bool HasSources(const fs::path& projectDir);
        // Newest modification time of any .cs/.csproj under Scripts (min() if none).
        fs::file_time_type LatestSourceTime(const fs::path& projectDir);
        // Command line that builds the scripts assembly (stderr merged into stdout).
        std::string BuildCommand(const fs::path& projectDir);
    }
}
