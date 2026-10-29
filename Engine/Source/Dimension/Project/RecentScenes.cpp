#include "Dimension/Project/RecentScenes.h"
#include "Dimension/Project/Project.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <fstream>

namespace Dimension::RecentScenes
{
    namespace
    {
        fs::path File() { return Project::GetDirectory() / ".dimension" / "RecentScenes.json"; }

        // Stored relative to the project.
        std::vector<std::string> Read()
        {
            std::vector<std::string> out;
            std::ifstream in(File());
            if (!in) return out;
            try
            {
                for (auto& s : nlohmann::json::parse(in))
                    if (s.is_string()) out.push_back(s.get<std::string>());
            }
            catch (const std::exception&)
            {
            }
            return out;
        }

        void Write(const std::vector<std::string>& entries)
        {
            std::error_code ec;
            fs::create_directories(File().parent_path(), ec);
            std::ofstream(File()) << nlohmann::json(entries).dump(2);
        }

        std::string Key(const fs::path& scene)
        {
            fs::path p = scene.is_absolute() ? scene.lexically_relative(Project::GetDirectory()) : scene;
            return p.lexically_normal().generic_string();
        }
    }

    std::vector<fs::path> List()
    {
        std::vector<fs::path> out;
        for (const std::string& s : Read())
        {
            fs::path p = (Project::GetDirectory() / s).lexically_normal();
            if (fs::exists(p)) out.push_back(p);
        }
        return out;
    }

    void Add(const fs::path& scene)
    {
        std::vector<std::string> entries = Read();
        std::string key = Key(scene);
        std::erase(entries, key);
        entries.insert(entries.begin(), key);
        if (entries.size() > kMax) entries.resize(kMax);
        Write(entries);
    }

    void Remove(const fs::path& scene)
    {
        std::vector<std::string> entries = Read();
        std::erase(entries, Key(scene));
        Write(entries);
    }

    void Clear() { Write({}); }
}
