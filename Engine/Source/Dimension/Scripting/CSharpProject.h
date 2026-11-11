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

        // NuGet packages of the scripts (the Packages window): those the project references (its PackageReference
        // items), nuget.org search results, and the .NET SDK commands that search, add (or update) and remove them.
        struct PackageReference
        {
            std::string Id, Version;
        };
        std::vector<PackageReference> ListPackages(const fs::path& projectDir);
        struct PackageInfo
        {
            std::string Id, LatestVersion, Owners;
            uint64_t Downloads = 0;
        };
        std::vector<PackageInfo> ParseSearchResults(const std::string& json); // `dotnet package search --format json`
        std::string SearchCommand(const std::string& term, int take = 20);
        std::string AddPackageCommand(const fs::path& projectDir, const std::string& id, const std::string& version = {});
        std::string RemovePackageCommand(const fs::path& projectDir, const std::string& id);
        // Runs a command (stderr merged in); returns its exit code and output.
        int RunCommand(const std::string& command, std::string& output);
    }
}
