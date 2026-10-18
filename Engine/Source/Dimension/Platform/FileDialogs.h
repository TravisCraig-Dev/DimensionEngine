#pragma once
#include "Dimension/Core/Base.h"
#include <optional>

namespace Dimension
{
    // Native OS dialogs. Filter format: { {"Scene", "*.scene"}, ... }.
    class FileDialogs
    {
    public:
        using Filter = std::vector<std::pair<std::wstring, std::wstring>>;
        static std::optional<fs::path> OpenFile(const Filter& filter, const fs::path& initialDir = {});
        static std::optional<fs::path> SaveFile(const Filter& filter, const std::wstring& defaultExt, const fs::path& initialDir = {});
        static std::optional<fs::path> PickFolder(const fs::path& initialDir = {});
        static void RevealInExplorer(const fs::path& path);
        static void OpenWithDefaultApp(const fs::path& path);
        static bool Confirm(const std::string& title, const std::string& message); // Yes/No
        // Returns 0 = Yes, 1 = No, 2 = Cancel
        static int YesNoCancel(const std::string& title, const std::string& message);
    };
}
