#include "Dimension/Platform/FileDialogs.h"

#ifndef _WIN32
#include "Dimension/Core/Application.h"
#include "Dimension/Core/Log.h"
#include "Dimension/Platform/Platform.h"
#include <GLFW/glfw3.h>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <thread>

// Linux has no system file dialog API of its own: the desktop's dialog programs are run instead, zenity (GNOME, GTK)
// or else kdialog (KDE), as SDL and most engines' Linux editors do. Their answer is what they print.
namespace Dimension
{
    namespace
    {
        std::string Utf8(const std::wstring& w)
        {
            std::string out;
            for (wchar_t wc : w)
            {
                auto c = (uint32_t)wc;
                if (c < 0x80) out += (char)c;
                else if (c < 0x800) out += (char)(0xC0 | (c >> 6)), out += (char)(0x80 | (c & 0x3F));
                else if (c < 0x10000) out += (char)(0xE0 | (c >> 12)), out += (char)(0x80 | ((c >> 6) & 0x3F)), out += (char)(0x80 | (c & 0x3F));
                else
                    out += (char)(0xF0 | (c >> 18)), out += (char)(0x80 | ((c >> 12) & 0x3F)), out += (char)(0x80 | ((c >> 6) & 0x3F)),
                        out += (char)(0x80 | (c & 0x3F));
            }
            return out;
        }

        bool Has(const char* program)
        {
            std::string out;
            return Platform::RunCommand(std::string("command -v ") + program, &out) == 0 && !out.empty();
        }

        enum class Tool { None, Zenity, KDialog };
        Tool Dialogs()
        {
            static Tool tool = [] {
                if (const char* forced = std::getenv("DIM_DIALOG_TOOL"))
                {
                    std::string f = forced;
                    if (f == "zenity") return Tool::Zenity;
                    if (f == "kdialog") return Tool::KDialog;
                    if (f == "none") return Tool::None;
                }
                // KDE's own dialog on KDE desktops; zenity everywhere else.
                const char* desktop = std::getenv("XDG_CURRENT_DESKTOP");
                bool kde = desktop && std::string(desktop).find("KDE") != std::string::npos;
                if (kde && Has("kdialog")) return Tool::KDialog;
                if (Has("zenity")) return Tool::Zenity;
                if (Has("kdialog")) return Tool::KDialog;
                Log::Warn("No dialog program found: install zenity (or kdialog) for file dialogs");
                return Tool::None;
            }();
            return tool;
        }

        struct Result
        {
            int Code = -1;
            std::string Output;
        };

        // Runs the dialog program on a worker thread while this (the main) thread keeps the window's events flowing,
        // so the desktop does not mark the editor as not responding while the dialog is open.
        Result Run(const std::string& command)
        {
            Result r;
            std::atomic<bool> done = false;
            std::thread worker([&] {
                FILE* pipe = Platform::OpenPipe(command + " 2>/dev/null");
                if (pipe)
                {
                    char buffer[1024];
                    while (std::fgets(buffer, sizeof(buffer), pipe)) r.Output += buffer;
                    r.Code = Platform::ClosePipe(pipe);
                }
                done = true;
            });
            bool pump = Application::TryGet() != nullptr;
            while (!done)
            {
                if (pump) glfwPollEvents();
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            worker.join();
            while (!r.Output.empty() && (r.Output.back() == '\n' || r.Output.back() == '\r')) r.Output.pop_back();
            return r;
        }

        std::string Q(const std::string& s) { return Platform::ShellQuote(s); }

        // zenity: --file-filter='Scene | *.scene *.prefab'; kdialog: '*.scene *.prefab|Scene' entries joined by newlines.
        std::string Patterns(const std::wstring& pattern)
        {
            std::string p = Utf8(pattern);
            for (char& c : p)
                if (c == ';') c = ' ';
            return p;
        }

        std::string StartIn(const fs::path& dir, const std::string& name = {})
        {
            fs::path base = dir.empty() ? fs::current_path() : dir;
            std::string s = base.string();
            if (s.empty() || s.back() != '/') s += '/';
            return s + name;
        }

        std::optional<fs::path> PickPath(const FileDialogs::Filter& filter, const fs::path& initialDir, bool save, bool folders)
        {
            std::string command;
            switch (Dialogs())
            {
            case Tool::Zenity:
                command = "zenity --file-selection";
                if (save) command += " --save --confirm-overwrite";
                if (folders) command += " --directory";
                command += " --filename=" + Q(StartIn(initialDir));
                for (auto& [name, pattern] : filter) command += " --file-filter=" + Q(Utf8(name) + " | " + Patterns(pattern));
                if (!filter.empty()) command += " --file-filter=" + Q("All files | *");
                break;
            case Tool::KDialog:
            {
                command = folders ? "kdialog --getexistingdirectory " : save ? "kdialog --getsavefilename " : "kdialog --getopenfilename ";
                command += Q(StartIn(initialDir));
                std::string filters;
                for (auto& [name, pattern] : filter) filters += (filters.empty() ? "" : "\n") + Patterns(pattern) + "|" + Utf8(name);
                if (!folders && !filters.empty()) command += " " + Q(filters);
                break;
            }
            case Tool::None: return std::nullopt;
            }
            Result r = Run(command);
            if (r.Code != 0 || r.Output.empty()) return std::nullopt;
            return fs::path(r.Output);
        }

        std::string Escaped(const std::string& text) { return Q(text); }
    }

    std::optional<fs::path> FileDialogs::OpenFile(const Filter& filter, const fs::path& initialDir)
    {
        return PickPath(filter, initialDir, false, false);
    }

    std::optional<fs::path> FileDialogs::SaveFile(const Filter& filter, const std::wstring& defaultExt, const fs::path& initialDir)
    {
        auto path = PickPath(filter, initialDir, true, false);
        if (path && !defaultExt.empty() && !path->has_extension()) path->replace_extension("." + Utf8(defaultExt));
        return path;
    }

    std::optional<fs::path> FileDialogs::PickFolder(const fs::path& initialDir) { return PickPath({}, initialDir, false, true); }

    void FileDialogs::RevealInExplorer(const fs::path& path)
    {
        // The file manager's org.freedesktop.FileManager1 interface selects the item (Nautilus, Dolphin, Nemo...);
        // without one, the folder opens.
        std::string uri = "file://" + fs::absolute(path).string();
        std::string command = "dbus-send --session --print-reply --dest=org.freedesktop.FileManager1 /org/freedesktop/FileManager1 "
                              "org.freedesktop.FileManager1.ShowItems array:string:" + Q(uri) + " string:''";
        if (Platform::RunCommand(command) != 0)
            Platform::RunCommand("xdg-open " + Q((fs::is_directory(path) ? path : path.parent_path()).string()) + " >/dev/null 2>&1 &");
    }

    void FileDialogs::OpenWithDefaultApp(const fs::path& path)
    {
        Platform::RunCommand("xdg-open " + Q(path.string()) + " >/dev/null 2>&1 &");
    }

    bool FileDialogs::Confirm(const std::string& title, const std::string& message)
    {
        switch (Dialogs())
        {
        case Tool::Zenity: return Run("zenity --question --no-markup --title=" + Escaped(title) + " --text=" + Escaped(message)).Code == 0;
        case Tool::KDialog: return Run("kdialog --title " + Escaped(title) + " --yesno " + Escaped(message)).Code == 0;
        case Tool::None: break;
        }
        return true;
    }

    int FileDialogs::YesNoCancel(const std::string& title, const std::string& message)
    {
        switch (Dialogs())
        {
        case Tool::Zenity:
        {
            // OK = Yes (0); No is the extra button (it prints its label). Cancel, Escape and closing the dialog all exit
            // with 1 and print nothing, so they are Cancel: never "No" (which discards) by accident.
            Result r = Run("zenity --question --no-markup --icon-name=dialog-warning --title=" + Escaped(title) + " --text=" + Escaped(message) +
                           " --ok-label=Yes --cancel-label=Cancel --extra-button=No");
            if (r.Code == 0) return 0;
            return r.Output == "No" ? 1 : 2;
        }
        case Tool::KDialog:
        {
            int code = Run("kdialog --title " + Escaped(title) + " --warningyesnocancel " + Escaped(message)).Code;
            return code == 0 ? 0 : code == 1 ? 1 : 2;
        }
        case Tool::None: break;
        }
        return 1;
    }
}
#endif
