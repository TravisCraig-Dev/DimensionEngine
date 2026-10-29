#include "Dimension/Asset/ProjectFavorites.h"
#include "Dimension/Project/Project.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <fstream>

namespace Dimension::ProjectFavorites
{
    namespace
    {
        fs::path File() { return Project::GetDirectory() / ".dimension" / "Favorites.json"; }

        std::vector<Favorite> Read()
        {
            std::vector<Favorite> out;
            std::ifstream in(File());
            if (!in) return out;
            nlohmann::json j = nlohmann::json::parse(in, nullptr, false);
            if (!j.is_array()) return out;
            for (const auto& f : j)
                if (f.is_object())
                    out.push_back({ f.value("name", std::string()), f.value("folder", std::string()), f.value("search", std::string()) });
            return out;
        }

        void Write(const std::vector<Favorite>& favorites)
        {
            nlohmann::json j = nlohmann::json::array();
            for (const Favorite& f : favorites)
            {
                nlohmann::json e{ { "name", f.Name } };
                if (f.IsSearch()) e["search"] = f.Search;
                else e["folder"] = f.Folder;
                j.push_back(e);
            }
            std::error_code ec;
            fs::create_directories(File().parent_path(), ec);
            std::ofstream(File()) << j.dump(2);
        }

        // The same favorite: the same folder, or the same search.
        bool Same(const Favorite& a, const Favorite& b) { return a.IsSearch() == b.IsSearch() && a.Folder == b.Folder && a.Search == b.Search; }
    }

    std::vector<Favorite> List()
    {
        std::vector<Favorite> out = Read();
        fs::path assets = Project::GetAssetsDirectory();
        std::erase_if(out, [&](const Favorite& f) { return !f.IsSearch() && !fs::is_directory(assets / f.Folder); });
        return out;
    }

    bool AddFolder(const fs::path& folder)
    {
        fs::path assets = Project::GetAssetsDirectory();
        fs::path rel = folder.is_absolute() ? folder.lexically_normal().lexically_relative(assets.lexically_normal()) : folder.lexically_normal();
        std::string key = rel.generic_string();
        while (!key.empty() && key.back() == '/') key.pop_back();
        if (key == ".") key.clear();
        if (key.rfind("..", 0) == 0 || !fs::is_directory(assets / key)) return false;
        Favorite f{ key.empty() ? "Assets" : fs::path(key).filename().string(), key, {} };
        auto all = Read();
        if (std::any_of(all.begin(), all.end(), [&](const Favorite& o) { return Same(o, f); })) return false;
        all.push_back(f);
        Write(all);
        return true;
    }

    bool AddSearch(const std::string& query, const std::string& name)
    {
        size_t a = query.find_first_not_of(' '), b = query.find_last_not_of(' ');
        if (a == std::string::npos) return false;
        std::string q = query.substr(a, b - a + 1);
        Favorite f{ name.empty() ? q : name, {}, q };
        auto all = Read();
        if (std::any_of(all.begin(), all.end(), [&](const Favorite& o) { return Same(o, f); })) return false;
        all.push_back(f);
        Write(all);
        return true;
    }

    void Remove(const Favorite& favorite)
    {
        auto all = Read();
        std::erase_if(all, [&](const Favorite& o) { return Same(o, favorite); });
        Write(all);
    }

    void Rename(const Favorite& favorite, const std::string& name)
    {
        if (name.find_first_not_of(' ') == std::string::npos) return;
        auto all = Read();
        for (Favorite& o : all)
            if (Same(o, favorite)) o.Name = name;
        Write(all);
    }
}
