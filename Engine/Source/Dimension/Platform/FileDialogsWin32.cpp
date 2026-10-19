#include "Dimension/Platform/FileDialogs.h"

#ifdef _WIN32
#include <windows.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <atomic>
#include <thread>
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")

namespace Dimension
{
    namespace
    {
        // Shell objects (file dialogs, ShellExecute handlers) need a single-threaded COM apartment, but the main thread
        // may already be multi-threaded (the .NET runtime initializes it that way once C# scripts run). There the dialog
        // deadlocks, so shell calls run on their own STA thread. The calling thread keeps dispatching its window
        // messages meanwhile: the dialog is owned by the editor window and the editor must not stop responding.
        template <class Fn> void RunOnStaThread(Fn&& fn, bool pumpMessages)
        {
            std::atomic<bool> done = false;
            std::thread worker([&] {
                HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
                fn();
                if (SUCCEEDED(init)) CoUninitialize();
                done = true;
            });
            while (pumpMessages && !done)
            {
                MsgWaitForMultipleObjects(0, nullptr, FALSE, 20, QS_ALLINPUT);
                MSG msg;
                while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
                {
                    TranslateMessage(&msg);
                    DispatchMessageW(&msg);
                }
            }
            worker.join();
        }

        // Runs an IFileDialog (on an STA thread); returns the chosen path.
        std::optional<fs::path> ShowDialog(REFCLSID clsid, const FileDialogs::Filter& filter, const wchar_t* defaultExt,
                                           const fs::path& initialDir, bool folders, HWND owner)
        {
            std::optional<fs::path> result;
            IFileDialog* dialog = nullptr;
            if (SUCCEEDED(CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
            {
                DWORD options = 0;
                dialog->GetOptions(&options);
                dialog->SetOptions(options | FOS_FORCEFILESYSTEM | (folders ? FOS_PICKFOLDERS : 0));
                std::vector<COMDLG_FILTERSPEC> specs;
                for (auto& [name, pattern] : filter) specs.push_back({ name.c_str(), pattern.c_str() });
                if (!specs.empty()) dialog->SetFileTypes((UINT)specs.size(), specs.data());
                if (defaultExt) dialog->SetDefaultExtension(defaultExt);
                if (!initialDir.empty())
                {
                    IShellItem* folder = nullptr;
                    if (SUCCEEDED(SHCreateItemFromParsingName(initialDir.c_str(), nullptr, IID_PPV_ARGS(&folder))))
                    {
                        dialog->SetFolder(folder);
                        folder->Release();
                    }
                }
                if (SUCCEEDED(dialog->Show(owner)))
                {
                    IShellItem* item = nullptr;
                    if (SUCCEEDED(dialog->GetResult(&item)))
                    {
                        PWSTR path = nullptr;
                        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
                        {
                            result = fs::path(path);
                            CoTaskMemFree(path);
                        }
                        item->Release();
                    }
                }
                dialog->Release();
            }
            return result;
        }

        std::optional<fs::path> RunDialog(REFCLSID clsid, const FileDialogs::Filter& filter, const wchar_t* defaultExt,
                                          const fs::path& initialDir, bool folders)
        {
            std::optional<fs::path> result;
            HWND owner = GetActiveWindow(); // per-thread: query it here, on the caller's thread
            RunOnStaThread([&] { result = ShowDialog(clsid, filter, defaultExt, initialDir, folders, owner); }, true);
            return result;
        }

        std::wstring Widen(const std::string& s)
        {
            int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
            std::wstring w(n, L'\0');
            MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
            return w;
        }
    }

    std::optional<fs::path> FileDialogs::OpenFile(const Filter& filter, const fs::path& initialDir)
    {
        return RunDialog(CLSID_FileOpenDialog, filter, nullptr, initialDir, false);
    }

    std::optional<fs::path> FileDialogs::SaveFile(const Filter& filter, const std::wstring& defaultExt, const fs::path& initialDir)
    {
        return RunDialog(CLSID_FileSaveDialog, filter, defaultExt.c_str(), initialDir, false);
    }

    std::optional<fs::path> FileDialogs::PickFolder(const fs::path& initialDir)
    {
        return RunDialog(CLSID_FileOpenDialog, {}, nullptr, initialDir, true);
    }

    void FileDialogs::RevealInExplorer(const fs::path& path)
    {
        std::wstring args = L"/select,\"" + path.wstring() + L"\"";
        RunOnStaThread([&] { ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL); }, false);
    }

    void FileDialogs::OpenWithDefaultApp(const fs::path& path)
    {
        RunOnStaThread([&] { ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL); }, false);
    }

    bool FileDialogs::Confirm(const std::string& title, const std::string& message)
    {
        return MessageBoxW(GetActiveWindow(), Widen(message).c_str(), Widen(title).c_str(), MB_YESNO | MB_ICONQUESTION) == IDYES;
    }

    int FileDialogs::YesNoCancel(const std::string& title, const std::string& message)
    {
        int r = MessageBoxW(GetActiveWindow(), Widen(message).c_str(), Widen(title).c_str(), MB_YESNOCANCEL | MB_ICONWARNING);
        return r == IDYES ? 0 : r == IDNO ? 1 : 2;
    }
}
#else
namespace Dimension
{
    std::optional<fs::path> FileDialogs::OpenFile(const Filter&, const fs::path&) { return std::nullopt; }
    std::optional<fs::path> FileDialogs::SaveFile(const Filter&, const std::wstring&, const fs::path&) { return std::nullopt; }
    std::optional<fs::path> FileDialogs::PickFolder(const fs::path&) { return std::nullopt; }
    void FileDialogs::RevealInExplorer(const fs::path&) {}
    void FileDialogs::OpenWithDefaultApp(const fs::path&) {}
    bool FileDialogs::Confirm(const std::string&, const std::string&) { return true; }
    int FileDialogs::YesNoCancel(const std::string&, const std::string&) { return 1; }
}
#endif
