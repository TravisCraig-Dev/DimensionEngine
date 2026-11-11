#pragma once
#include "Dimension/Core/Base.h"

namespace Dimension
{
    // Layout of a game project's C# scripts:
    //   <Project>/Scripts/GameScripts.csproj        (generated if missing; open it in Rider/VS)
    //   <Project>/Scripts/Directory.Build.props     (regenerated: points at the engine's managed API)
    //   <Project>/Scripts/**/*.cs                   (your Behaviour classes)
    //   <Project>/.dimension/ScriptAssemblies/GameScripts.dll  (build output loaded by editor/player)
    // A folder with a <Name>.asmdef file is an assembly of its own (see ScriptAssembly); its generated project goes in
    // <Project>/.dimension/ScriptProjects/<Name>/, and Scripts/Directory.Build.targets (regenerated) takes its folder
    // out of GameScripts.
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
        // Command line that builds the scripts assembly, then the editor-only split assemblies (stderr merged into stdout).
        std::string BuildCommand(const fs::path& projectDir);

        // A script assembly split off by folder (like Unity's assembly definitions): a <Name>.asmdef file (JSON) makes
        // the .cs files of its folder and its subfolders (down to those with an .asmdef of their own) an assembly of
        // their own. "name"; "references": the assemblies its code uses (other .asmdef names, or "GameScripts");
        // "editorOnly" (or Unity's "includePlatforms": ["Editor"]): only in the editor, left out of builds (editor tools,
        // tests), and may use GameScripts; "autoReferenced" (default true): GameScripts' code may use it.
        struct ScriptAssembly
        {
            std::string Name;
            fs::path Directory, File;
            std::vector<std::string> References;
            bool EditorOnly = false, AutoReferenced = true;
        };
        // The project's .asmdef assemblies (by name); problems (no name, duplicates, unknown references, a cycle, a
        // build assembly using an editor-only one) go to `errors`.
        std::vector<ScriptAssembly> FindAssemblies(const fs::path& projectDir, std::vector<std::string>* errors = nullptr);
        // Writes their projects, Scripts/Directory.Build.targets and ScriptAssemblies.json (the list the script host
        // loads and builds filter by), and deletes the outputs of assemblies no longer there. False with errors.
        bool GenerateAssemblyProjects(const fs::path& projectDir, const fs::path& managedDir, std::vector<std::string>& errors);
        // The editor-only assemblies in a ScriptAssemblies folder's manifest (left out of builds).
        std::vector<std::string> EditorOnlyAssemblies(const fs::path& scriptAssembliesDir);

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
