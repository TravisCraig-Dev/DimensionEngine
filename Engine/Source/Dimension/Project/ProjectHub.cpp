#include "Dimension/Project/ProjectHub.h"
#include "Dimension/Project/Project.h"
#include "Dimension/Asset/AssetManager.h"
#include "Dimension/Scene/SceneSerializer.h"
#include "Dimension/Scene/SceneTemplates.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>

namespace Dimension::ProjectHub
{
    namespace
    {
        fs::path s_File;

        fs::path File()
        {
            if (!s_File.empty()) return s_File;
            const char* appData = std::getenv("APPDATA");
            fs::path base = appData ? fs::path(appData) : fs::temp_directory_path();
            return base / "Dimension" / "RecentProjects.json";
        }

        nlohmann::json Read()
        {
            std::ifstream in(File());
            if (!in) return nlohmann::json::array();
            try
            {
                auto j = nlohmann::json::parse(in);
                return j.is_array() ? j : nlohmann::json::array();
            }
            catch (const std::exception&)
            {
                return nlohmann::json::array();
            }
        }

        void Write(const nlohmann::json& entries)
        {
            std::error_code ec;
            fs::create_directories(File().parent_path(), ec);
            std::ofstream(File()) << entries.dump(2);
        }

        std::string Key(const fs::path& project)
        {
            std::error_code ec;
            fs::path p = fs::weakly_canonical(project, ec);
            return (ec ? project : p).lexically_normal().generic_string();
        }

        std::string NameOf(const fs::path& project)
        {
            std::ifstream in(project / Project::kFileName);
            if (in)
                try
                {
                    auto j = nlohmann::json::parse(in);
                    if (j.contains("name") && j["name"].is_string()) return j["name"].get<std::string>();
                }
                catch (const std::exception&)
                {
                }
            return project.filename().string();
        }
    }

    void SetRecentFile(const fs::path& file) { s_File = file; }

    bool IsProject(const fs::path& folder) { return fs::exists(folder / Project::kFileName); }

    std::vector<RecentProject> Recent()
    {
        std::vector<RecentProject> out;
        for (auto& e : Read())
        {
            if (!e.is_object() || !e.contains("path")) continue;
            RecentProject p;
            p.Path = fs::path(e["path"].get<std::string>());
            p.LastOpened = e.value("lastOpened", int64_t(0));
            p.Exists = IsProject(p.Path);
            p.Name = p.Exists ? NameOf(p.Path) : e.value("name", p.Path.filename().string());
            out.push_back(std::move(p));
        }
        return out;
    }

    void Remember(const fs::path& project)
    {
        nlohmann::json entries = Read();
        std::string key = Key(project);
        nlohmann::json kept = nlohmann::json::array();
        for (auto& e : entries)
            if (e.is_object() && e.value("path", std::string()) != key) kept.push_back(e);
        int64_t now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        kept.insert(kept.begin(), nlohmann::json{ { "path", key }, { "name", NameOf(project) }, { "lastOpened", now } });
        Write(kept);
    }

    void Forget(const fs::path& project)
    {
        nlohmann::json kept = nlohmann::json::array();
        std::string key = Key(project);
        for (auto& e : Read())
            if (e.is_object() && e.value("path", std::string()) != key) kept.push_back(e);
        Write(kept);
    }

    bool Create(const fs::path& folder, const std::string& name, const std::string& sceneTemplate)
    {
        if (IsProject(folder)) return false;
        std::error_code ec;
        fs::create_directories(folder, ec);
        if (!Project::Open(folder)) return false;
        Project::GetConfig().Name = name;
        // The first scene, from the template (Basic 3D when the name is unknown).
        SceneTemplates::Template chosen = SceneTemplates::All().front();
        for (const auto& t : SceneTemplates::All())
            if (t.Name == sceneTemplate) chosen = t;
        auto scene = SceneTemplates::Create(chosen);
        fs::create_directories(AssetManager::GetAssetsDirectory() / "Scenes", ec);
        fs::path main = AssetManager::GetAssetsDirectory() / "Scenes" / "Main.scene";
        if (scene)
        {
            scene->SetName("Main");
            SceneSerializer::SaveToFile(*scene, main);
            Project::GetConfig().StartScene = AssetManager::Import(main);
        }
        return Project::Save();
    }
}
