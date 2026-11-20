#include "Dimension/Platform/Platform.h"
#include <cstdlib>
#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#else
#include <climits>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace Dimension::Platform
{
    namespace
    {
        fs::path Env(const char* name)
        {
            const char* v = std::getenv(name);
            return v && *v ? fs::path(v) : fs::path();
        }

        // An XDG base directory: the variable when set (and absolute), else ~/<fallback>.
        [[maybe_unused]] fs::path Xdg(const char* variable, const char* fallback)
        {
            fs::path p = Env(variable);
            if (!p.empty() && p.is_absolute()) return p;
            fs::path home = HomeDirectory();
            return home.empty() ? fs::temp_directory_path() : home / fallback;
        }
    }

    fs::path ExecutablePath()
    {
#ifdef _WIN32
        std::wstring buffer(MAX_PATH, L'\0');
        for (;;)
        {
            DWORD n = GetModuleFileNameW(nullptr, buffer.data(), (DWORD)buffer.size());
            if (n < buffer.size()) return fs::path(buffer.substr(0, n));
            buffer.resize(buffer.size() * 2);
        }
#else
        std::error_code ec;
        fs::path p = fs::read_symlink("/proc/self/exe", ec);
        return ec ? fs::path() : p;
#endif
    }

    fs::path ExecutableDirectory()
    {
        fs::path exe = ExecutablePath();
        return exe.empty() ? fs::current_path() : exe.parent_path();
    }

    fs::path HomeDirectory()
    {
#ifdef _WIN32
        return Env("USERPROFILE");
#else
        return Env("HOME");
#endif
    }

    fs::path DocumentsDirectory()
    {
#ifdef _WIN32
        // (the shell's Documents folder: it may be moved, e.g. into OneDrive)
        PWSTR path = nullptr;
        fs::path out;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &path)) && path) out = fs::path(path);
        CoTaskMemFree(path);
        if (!out.empty()) return out;
        return HomeDirectory() / "Documents";
#else
        // XDG's user-dirs.dirs names it (XDG_DOCUMENTS_DIR="$HOME/Documents"); else ~/Documents when there is one.
        fs::path home = HomeDirectory();
        fs::path config = Env("XDG_CONFIG_HOME");
        if (config.empty()) config = home / ".config";
        if (FILE* f = std::fopen((config / "user-dirs.dirs").string().c_str(), "r"))
        {
            char line[1024];
            fs::path found;
            while (std::fgets(line, sizeof(line), f))
            {
                std::string l = line;
                const std::string key = "XDG_DOCUMENTS_DIR=\"";
                if (l.rfind(key, 0) != 0) continue;
                std::string value = l.substr(key.size());
                value = value.substr(0, value.find('"'));
                if (value.rfind("$HOME", 0) == 0) found = home / value.substr(std::min<size_t>(value.size(), 6));
                else found = value;
            }
            std::fclose(f);
            if (!found.empty() && fs::is_directory(found)) return found;
        }
        std::error_code ec;
        return fs::is_directory(home / "Documents", ec) ? home / "Documents" : home;
#endif
    }

    fs::path UserSettingsDirectory()
    {
#ifdef _WIN32
        fs::path p = Env("APPDATA");
        return p.empty() ? fs::temp_directory_path() : p;
#else
        return Xdg("XDG_CONFIG_HOME", ".config");
#endif
    }

    fs::path UserDataDirectory()
    {
#ifdef _WIN32
        return UserSettingsDirectory();
#else
        return Xdg("XDG_DATA_HOME", ".local/share");
#endif
    }

    fs::path UserCacheDirectory()
    {
#ifdef _WIN32
        fs::path p = Env("LOCALAPPDATA");
        return p.empty() ? fs::temp_directory_path() : p;
#else
        return Xdg("XDG_CACHE_HOME", ".cache");
#endif
    }

    FILE* OpenPipe(const std::string& command)
    {
#ifdef _WIN32
        // cmd /c strips the outer quotes, so commands that start with a quoted program still run.
        return _popen(("\"" + command + "\"").c_str(), "r");
#else
        return popen(command.c_str(), "r");
#endif
    }

    int ClosePipe(FILE* pipe)
    {
#ifdef _WIN32
        return _pclose(pipe);
#else
        int status = pclose(pipe);
        if (status == -1) return -1;
        return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
    }

    int RunCommand(const std::string& command, std::string* output)
    {
        FILE* pipe = OpenPipe(command + " 2>&1");
        if (!pipe) return -1;
        char buffer[512];
        while (std::fgets(buffer, sizeof(buffer), pipe))
            if (output) *output += buffer;
        return ClosePipe(pipe);
    }

    std::string ShellQuote(const std::string& text)
    {
#ifdef _WIN32
        return "\"" + text + "\"";
#else
        std::string out = "'";
        for (char c : text)
            if (c == '\'') out += "'\\''";
            else out += c;
        return out + "'";
#endif
    }

    std::string SharedLibraryName(const std::string& name) { return name + (IsWindows() ? ".dll" : ".so"); }
    std::string ExecutableName(const std::string& name) { return IsWindows() ? name + ".exe" : name; }
}
