#pragma once
#include "Dimension/Core/Base.h"
#include <functional>
#include <optional>
#include <vector>

namespace Dimension
{
    // What was selected: objects by GUID (so it holds across play mode, undo and reloading) with the primary one, or
    // assets with theirs.
    struct SelectionSnapshot
    {
        std::vector<uint64_t> Objects;
        uint64_t Primary = 0;
        std::vector<uint64_t> Assets;
        uint64_t PrimaryAsset = 0;
        bool Empty() const { return Objects.empty() && Assets.empty(); }
        bool operator==(const SelectionSnapshot&) const = default;
    };

    // The editor's selection history (like a browser's): Back and Forward step through what was selected before.
    // Recording a new selection after going back drops the ones ahead of it.
    class SelectionHistory
    {
    public:
        static constexpr size_t kMax = 64;
        // Adds a selection unless it is the current one (empty selections are not recorded).
        void Record(const SelectionSnapshot& selection);
        // The selection before / after the current one that `usable` accepts (e.g. some of its objects still exist),
        // which becomes the current one; none at either end.
        std::optional<SelectionSnapshot> Back(const std::function<bool(const SelectionSnapshot&)>& usable = {});
        std::optional<SelectionSnapshot> Forward(const std::function<bool(const SelectionSnapshot&)>& usable = {});
        // Updates the current entry in place (what was restored of it, when some of its objects are gone).
        void ReplaceCurrent(const SelectionSnapshot& selection)
        {
            if (!m_Entries.empty() && !selection.Empty()) m_Entries[m_Current] = selection;
        }
        bool CanGoBack() const { return m_Current > 0; }
        bool CanGoForward() const { return m_Current + 1 < m_Entries.size(); }
        size_t Size() const { return m_Entries.size(); }
        void Clear();

    private:
        std::vector<SelectionSnapshot> m_Entries;
        size_t m_Current = 0;
    };

    // Saved selections (Ctrl+Alt+1-9 stores, Alt+1-9 restores), per scene, kept per user in the project's
    // .dimension/SelectionSets.json by the scene's path (an unsaved scene's under "").
    namespace SelectionSets
    {
        constexpr int kSlots = 9;
        std::optional<SelectionSnapshot> Get(const fs::path& scene, int slot);
        void Set(const fs::path& scene, int slot, const SelectionSnapshot& selection);
        void Clear(const fs::path& scene, int slot);
        std::vector<int> Slots(const fs::path& scene);
    }
}
