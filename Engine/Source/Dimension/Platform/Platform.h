#pragma once
#include <cstdio>
#include <filesystem>
#include <string>

namespace Dimension::Platform
{
    namespace fs = std::filesystem;

    // The running executable (the editor, the player or a test runner) and its folder.
    fs::path ExecutablePath();
    fs::path ExecutableDirectory();

    // Per-user folders. Windows: %APPDATA% (roaming settings: preferences, recent projects, player saves) and
    // %LOCALAPPDATA% (caches). Linux: $XDG_CONFIG_HOME (~/.config), $XDG_DATA_HOME (~/.local/share) and
    // $XDG_CACHE_HOME (~/.cache). The temporary folder when none is known.
    fs::path UserSettingsDirectory(); // Windows %APPDATA%, Linux ~/.config
    fs::path UserDataDirectory();     // Windows %APPDATA%, Linux ~/.local/share (player saves)
    fs::path UserCacheDirectory();    // Windows %LOCALAPPDATA%, Linux ~/.cache
    fs::path HomeDirectory();         // %USERPROFILE% / $HOME

    // A shell command's output, read as it runs (stderr too when the command redirects it). Commands are run by
    // cmd.exe on Windows and /bin/sh elsewhere: ClosePipe returns the exit code.
    FILE* OpenPipe(const std::string& command);
    int ClosePipe(FILE* pipe);
    // The command run, its output (stdout and stderr) appended to `output`; the exit code (-1: could not run it).
    int RunCommand(const std::string& command, std::string* output = nullptr);
    // A path or argument quoted for the platform's shell.
    std::string ShellQuote(const std::string& text);

    // File names of native modules and programs: "Name.dll" / "Name.so", "Name.exe" / "Name".
    std::string SharedLibraryName(const std::string& name);
    std::string ExecutableName(const std::string& name);
    constexpr bool IsWindows() noexcept
    {
#ifdef _WIN32
        return true;
#else
        return false;
#endif
    }
}
