#include "Dimension/Scene/SelectionHistory.h"
#include "Dimension/Project/Project.h"
#include <nlohmann/json.hpp>
#include <fstream>

namespace Dimension
{
    void SelectionHistory::Record(const SelectionSnapshot& selection)
    {
        if (selection.Empty()) return;
        if (!m_Entries.empty() && m_Entries[m_Current] == selection) return;
        if (!m_Entries.empty()) m_Entries.resize(m_Current + 1); // what was ahead is dropped
        m_Entries.push_back(selection);
        if (m_Entries.size() > kMax) m_Entries.erase(m_Entries.begin());
        m_Current = m_Entries.size() - 1;
    }

    std::optional<SelectionSnapshot> SelectionHistory::Back(const std::function<bool(const SelectionSnapshot&)>& usable)
    {
        for (size_t i = m_Current; i-- > 0;)
            if (!usable || usable(m_Entries[i]))
            {
                m_Current = i;
                return m_Entries[i];
            }
        return std::nullopt;
    }

    std::optional<SelectionSnapshot> SelectionHistory::Forward(const std::function<bool(const SelectionSnapshot&)>& usable)
    {
        for (size_t i = m_Current + 1; i < m_Entries.size(); ++i)
            if (!usable || usable(m_Entries[i]))
            {
                m_Current = i;
                return m_Entries[i];
            }
        return std::nullopt;
    }

    void SelectionHistory::Clear()
    {
        m_Entries.clear();
        m_Current = 0;
    }

    namespace SelectionSets
    {
        namespace
        {
            using json = nlohmann::json;

            fs::path File() { return Project::GetDirectory() / ".dimension" / "SelectionSets.json"; }

            json Read()
            {
                std::ifstream in(File());
                if (!in) return json::object();
                json j = json::parse(in, nullptr, false);
                return j.is_object() ? j : json::object();
            }

            void Write(const json& j)
            {
                std::error_code ec;
                fs::create_directories(File().parent_path(), ec);
                std::ofstream(File()) << j.dump(2);
            }

            // Relative to Assets when inside it (as SceneBookmarks keys them).
            std::string Key(const fs::path& scene)
            {
                if (scene.empty()) return {};
                fs::path p = scene;
                if (p.is_absolute())
                {
                    fs::path rel = p.lexically_normal().lexically_relative(Project::GetAssetsDirectory().lexically_normal());
                    if (!rel.empty() && *rel.begin() != "..") p = rel;
                }
                return p.lexically_normal().generic_string();
            }

            bool Valid(int slot) { return slot >= 1 && slot <= kSlots; }
        }

        std::optional<SelectionSnapshot> Get(const fs::path& scene, int slot)
        {
            if (!Valid(slot)) return std::nullopt;
            json all = Read();
            std::string key = Key(scene), s = std::to_string(slot);
            if (!all.contains(key) || !all[key].is_object() || !all[key].contains(s)) return std::nullopt;
            const json& j = all[key][s];
            SelectionSnapshot out;
            out.Objects = j.value("objects", std::vector<uint64_t>{});
            out.Primary = j.value("primary", uint64_t(0));
            out.Assets = j.value("assets", std::vector<uint64_t>{});
            out.PrimaryAsset = j.value("primaryAsset", uint64_t(0));
            return out;
        }

        void Set(const fs::path& scene, int slot, const SelectionSnapshot& selection)
        {
            if (!Valid(slot)) return;
            if (selection.Empty())
            {
                Clear(scene, slot);
                return;
            }
            json all = Read();
            all[Key(scene)][std::to_string(slot)] = { { "objects", selection.Objects }, { "primary", selection.Primary },
                                                      { "assets", selection.Assets }, { "primaryAsset", selection.PrimaryAsset } };
            Write(all);
        }

        void Clear(const fs::path& scene, int slot)
        {
            json all = Read();
            std::string key = Key(scene);
            if (!all.contains(key)) return;
            all[key].erase(std::to_string(slot));
            if (all[key].empty()) all.erase(key);
            Write(all);
        }

        std::vector<int> Slots(const fs::path& scene)
        {
            std::vector<int> out;
            json all = Read();
            std::string key = Key(scene);
            if (!all.contains(key) || !all[key].is_object()) return out;
            for (int slot = 1; slot <= kSlots; ++slot)
                if (all[key].contains(std::to_string(slot))) out.push_back(slot);
            return out;
        }
    }
}
