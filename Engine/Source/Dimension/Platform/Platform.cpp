#include "Dimension/Platform/Platform.h"
#include <cstdlib>
#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#include <fcntl.h>
#include <io.h>
#include <mutex>
#include <unordered_map>
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

#ifdef _WIN32
    namespace
    {
        // The processes behind open pipes (OpenPipe -> ClosePipe).
        std::mutex s_PipeMutex;
        std::unordered_map<FILE*, HANDLE> s_PipeProcesses;

        // A handle the child may inherit: a duplicate of `h`, or the null device when `h` isn't usable (a GUI program
        // started without a console has no standard handles).
        HANDLE InheritableOr(HANDLE h, DWORD access)
        {
            HANDLE out = nullptr;
            if (h && h != INVALID_HANDLE_VALUE && GetFileType(h) != FILE_TYPE_UNKNOWN &&
                DuplicateHandle(GetCurrentProcess(), h, GetCurrentProcess(), &out, 0, TRUE, DUPLICATE_SAME_ACCESS))
                return out;
            SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
            return CreateFileW(L"NUL", access, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
        }
    }
#endif

    FILE* OpenPipe(const std::string& command)
    {
#ifdef _WIN32
        // As _popen(command, "r") (cmd /c, its output read through the pipe, stderr where ours goes), but without a
        // console window: the editor is a GUI program, and _popen would flash one up for every command it runs.
        SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
        HANDLE readEnd = nullptr, writeEnd = nullptr;
        if (!CreatePipe(&readEnd, &writeEnd, &sa, 0)) return nullptr;
        SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = InheritableOr(nullptr, GENERIC_READ);
        si.hStdOutput = writeEnd;
        si.hStdError = InheritableOr(GetStdHandle(STD_ERROR_HANDLE), GENERIC_WRITE);
        // cmd /c strips the outer quotes, so commands that start with a quoted program still run. (The command is in
        // the ANSI code page, as _popen takes it.)
        std::string line = "cmd.exe /c \"" + command + "\"";
        int n = MultiByteToWideChar(CP_ACP, 0, line.c_str(), -1, nullptr, 0);
        std::wstring wline(n > 0 ? (size_t)n : 1, L'\0');
        MultiByteToWideChar(CP_ACP, 0, line.c_str(), -1, wline.data(), n);
        PROCESS_INFORMATION pi{};
        BOOL ok = CreateProcessW(nullptr, wline.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
        CloseHandle(writeEnd);
        if (si.hStdInput && si.hStdInput != INVALID_HANDLE_VALUE) CloseHandle(si.hStdInput);
        if (si.hStdError && si.hStdError != INVALID_HANDLE_VALUE) CloseHandle(si.hStdError);
        if (!ok)
        {
            CloseHandle(readEnd);
            return nullptr;
        }
        CloseHandle(pi.hThread);
        int fd = _open_osfhandle((intptr_t)readEnd, _O_RDONLY | _O_TEXT);
        FILE* pipe = fd >= 0 ? _fdopen(fd, "r") : nullptr;
        if (!pipe)
        {
            if (fd >= 0) _close(fd);
            else CloseHandle(readEnd);
            TerminateProcess(pi.hProcess, 1);
            CloseHandle(pi.hProcess);
            return nullptr;
        }
        std::lock_guard lock(s_PipeMutex);
        s_PipeProcesses[pipe] = pi.hProcess;
        return pipe;
#else
        return popen(command.c_str(), "r");
#endif
    }

    int ClosePipe(FILE* pipe)
    {
#ifdef _WIN32
        HANDLE process = nullptr;
        {
            std::lock_guard lock(s_PipeMutex);
            auto it = s_PipeProcesses.find(pipe);
            if (it != s_PipeProcesses.end())
            {
                process = it->second;
                s_PipeProcesses.erase(it);
            }
        }
        std::fclose(pipe);
        if (!process) return -1;
        WaitForSingleObject(process, INFINITE);
        DWORD code = (DWORD)-1;
        GetExitCodeProcess(process, &code);
        CloseHandle(process);
        return (int)code;
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
