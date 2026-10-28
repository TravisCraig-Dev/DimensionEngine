#include "Dimension/Scene/ProjectSearch.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cctype>
#include <fstream>
#include <unordered_map>

namespace Dimension::ProjectSearch
{
    namespace
    {
        // Lower case without spaces.
        std::string Fold(const std::string& s)
        {
            std::string out;
            for (char c : s)
                if (c != ' ') out += (char)std::tolower((unsigned char)c);
            return out;
        }

        struct Entry
        {
            uint64_t Guid = 0;
            std::string Name, FoldedName, Tag;
            std::vector<std::string> Components; // folded
        };
        struct FileIndex
        {
            fs::file_time_type Time;
            std::vector<Entry> Entries;
        };
        std::unordered_map<std::string, FileIndex> s_Files;

        const std::vector<Entry>& Index(const fs::path& path)
        {
            std::error_code ec;
            auto time = fs::last_write_time(path, ec);
            FileIndex& index = s_Files[path.generic_string()];
            if (!index.Entries.empty() && index.Time == time) return index.Entries;
            index = FileIndex{ time, {} };
            std::ifstream in(path);
            if (!in) return index.Entries;
            try
            {
                auto scene = nlohmann::json::parse(in);
                for (auto& e : scene.value("entities", nlohmann::json::array()))
                {
                    Entry entry;
                    entry.Guid = e.value("guid", uint64_t(0));
                    const auto& components = e.value("components", nlohmann::json::object());
                    for (auto& [key, value] : components.items())
                    {
                        entry.Components.push_back(Fold(key));
                        if (key == "Tag" && value.is_object())
                        {
                            entry.Name = value.value("Name", std::string());
                            entry.Tag = Fold(value.value("Tag", std::string()));
                        }
                        // Scripts count as types too (by class name, with and without its namespace).
                        if (key == "Script" && value.is_object())
                            for (auto& script : value.value("Scripts", nlohmann::json::array()))
                            {
                                std::string cls = Fold(script.value("Class", std::string()));
                                entry.Components.push_back(cls);
                                if (size_t dot = cls.rfind('.'); dot != std::string::npos) entry.Components.push_back(cls.substr(dot + 1));
                            }
                    }
                    entry.FoldedName = Fold(entry.Name);
                    index.Entries.push_back(std::move(entry));
                }
            }
            catch (const std::exception&)
            {
                index.Entries.clear();
            }
            return index.Entries;
        }

        bool Matches(const Entry& e, const std::vector<std::string>& terms)
        {
            for (const std::string& term : terms)
            {
                if (term.rfind("t:", 0) == 0)
                {
                    std::string type = term.substr(2);
                    if (std::none_of(e.Components.begin(), e.Components.end(), [&](const std::string& c) { return c.rfind(type, 0) == 0; })) return false;
                }
                else if (term.rfind("tag:", 0) == 0)
                {
                    if (e.Tag != term.substr(4)) return false;
                }
                else if (e.FoldedName.find(term) == std::string::npos)
                    return false;
            }
            return true;
        }
    }

    std::vector<SceneObject> FindInScenes(const std::string& query, AssetHandle skip, size_t limit)
    {
        std::vector<std::string> terms;
        for (size_t start = 0; start < query.size();)
        {
            size_t end = query.find(' ', start);
            if (end == std::string::npos) end = query.size();
            if (end > start) terms.push_back(Fold(query.substr(start, end - start)));
            start = end + 1;
        }
        std::vector<SceneObject> out;
        if (terms.empty()) return out;
        for (AssetHandle scene : AssetManager::Search("t:Scene")) // sorted by path
        {
            if (scene == skip) continue;
            for (const Entry& e : Index(AssetManager::GetAbsolutePath(scene)))
                if (Matches(e, terms))
                {
                    out.push_back({ scene, e.Guid, e.Name });
                    if (out.size() >= limit) return out;
                }
        }
        return out;
    }
}
