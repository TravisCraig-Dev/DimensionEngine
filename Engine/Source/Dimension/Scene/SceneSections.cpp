#include "Dimension/Scene/SceneSections.h"
#include "Dimension/Scene/SceneSerializer.h"
#include "Dimension/Core/Log.h"
#include <algorithm>
#include <fstream>
#include <sstream>

namespace Dimension
{
    namespace
    {
        Entity RootOf(Scene& scene, Entity e)
        {
            for (Entity parent = scene.GetParent(e); !parent.IsNull(); parent = scene.GetParent(e)) e = parent;
            return e;
        }
    }

    bool SceneSections::Open(Scene& scene, const fs::path& path)
    {
        fs::path normal = path.lexically_normal();
        if (std::any_of(m_Added.begin(), m_Added.end(), [&](const Section& s) { return s.Path == normal; })) return false;
        std::ifstream in(path);
        if (!in) return false;
        std::stringstream text;
        text << in.rdbuf();
        nlohmann::json data;
        try
        {
            data = nlohmann::json::parse(text.str());
        }
        catch (const std::exception& e)
        {
            Log::Error("Cannot open scene '{}': {}", path.filename().string(), e.what());
            return false;
        }
        Section section;
        section.Name = data.value("name", path.stem().string());
        section.Path = normal;
        section.Settings = data.value("settings", nlohmann::json::object());
        for (Entity root : SceneSerializer::DeserializeInto(scene, data)) section.Roots.insert((uint64_t)scene.GetGuid(root));
        scene.UpdateTransforms();
        m_Added.push_back(std::move(section));
        // What its file holds, as this scene writes it (so IsDirty compares like with like).
        m_Added.back().Saved = Serialize(scene, m_Added.size()).dump(2);
        return true;
    }

    void SceneSections::Close(Scene& scene, size_t section)
    {
        if (section == 0 || section > m_Added.size()) return;
        for (Entity root : Roots(scene, section)) scene.DestroyEntity(root);
        m_Added.erase(m_Added.begin() + (section - 1));
    }

    size_t SceneSections::SectionOf(Scene& scene, Entity e) const
    {
        if (!scene.IsValid(e)) return 0;
        uint64_t root = (uint64_t)scene.GetGuid(RootOf(scene, e));
        for (size_t i = 0; i < m_Added.size(); ++i)
            if (m_Added[i].Roots.contains(root)) return i + 1;
        return 0;
    }

    std::vector<Entity> SceneSections::Roots(Scene& scene, size_t section) const
    {
        std::vector<Entity> out;
        for (Entity root : scene.GetRootEntities())
            if (SectionOf(scene, root) == section) out.push_back(root);
        return out;
    }

    void SceneSections::MoveRoot(Scene& scene, Entity root, size_t section)
    {
        if (!scene.IsValid(root) || !scene.GetParent(root).IsNull()) return;
        uint64_t guid = (uint64_t)scene.GetGuid(root);
        for (Section& s : m_Added) s.Roots.erase(guid);
        if (section > 0 && section <= m_Added.size()) m_Added[section - 1].Roots.insert(guid);
    }

    nlohmann::json SceneSections::Serialize(Scene& scene, size_t section) const
    {
        nlohmann::json j = SceneSerializer::Serialize(scene, Roots(scene, section));
        if (section > 0 && section <= m_Added.size())
        {
            j["name"] = m_Added[section - 1].Name;
            j["settings"] = m_Added[section - 1].Settings;
        }
        return j;
    }

    bool SceneSections::Save(Scene& scene, size_t section)
    {
        if (section == 0 || section > m_Added.size()) return false;
        Section& s = m_Added[section - 1];
        std::string text = Serialize(scene, section).dump(2);
        std::ofstream out(s.Path);
        out << text;
        if (!out) return false;
        s.Saved = text;
        return true;
    }

    bool SceneSections::IsDirty(Scene& scene, size_t section) const
    {
        if (section == 0 || section > m_Added.size()) return false;
        return Serialize(scene, section).dump(2) != m_Added[section - 1].Saved;
    }

    void SceneSections::RegisterLoaded(Scene& playing) const
    {
        for (size_t i = 0; i < m_Added.size(); ++i)
        {
            std::vector<Entity> roots;
            for (Entity root : playing.GetRootEntities())
                if (m_Added[i].Roots.contains((uint64_t)playing.GetGuid(root))) roots.push_back(root);
            playing.RegisterLoadedScene(m_Added[i].Name, std::move(roots));
        }
    }
}
