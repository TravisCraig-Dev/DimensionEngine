#include "Dimension/Scene/SceneVisibility.h"
#include "Dimension/Scene/Components.h"
#include <algorithm>
#include <fstream>

namespace Dimension
{
    void SceneVisibility::SetHidden(Scene& scene, Entity e, bool hidden)
    {
        if (!scene.IsValid(e)) return;
        uint64_t guid = (uint64_t)scene.GetGuid(e);
        if (hidden) m_Hidden.insert(guid);
        else m_Hidden.erase(guid);
    }

    void SceneVisibility::SetPickable(Scene& scene, Entity e, bool pickable)
    {
        if (!scene.IsValid(e)) return;
        uint64_t guid = (uint64_t)scene.GetGuid(e);
        if (pickable) m_Unpickable.erase(guid);
        else m_Unpickable.insert(guid);
    }

    bool SceneVisibility::IsHiddenSelf(Scene& scene, Entity e) const { return scene.IsValid(e) && m_Hidden.contains((uint64_t)scene.GetGuid(e)); }
    bool SceneVisibility::IsUnpickableSelf(Scene& scene, Entity e) const { return scene.IsValid(e) && m_Unpickable.contains((uint64_t)scene.GetGuid(e)); }
    bool SceneVisibility::IsHidden(Scene& scene, Entity e) const { return Applies(scene, e, m_Hidden); }
    bool SceneVisibility::IsUnpickable(Scene& scene, Entity e) const { return Applies(scene, e, m_Unpickable); }

    bool SceneVisibility::Applies(Scene& scene, Entity e, const std::unordered_set<uint64_t>& set) const
    {
        for (Entity at = e; scene.IsValid(at); at = scene.GetParent(at))
            if (set.contains((uint64_t)scene.GetGuid(at))) return true;
        return false;
    }

    std::vector<Entity> SceneVisibility::Expand(Scene& scene, const std::unordered_set<uint64_t>& set) const
    {
        std::vector<Entity> out;
        if (!Enabled || set.empty()) return out;
        std::vector<Entity> stack;
        for (uint64_t guid : set)
        {
            Entity e = scene.FindEntityByGuid(Guid(guid));
            if (!e.IsNull()) stack.push_back(e);
        }
        while (!stack.empty())
        {
            Entity e = stack.back();
            stack.pop_back();
            if (std::find(out.begin(), out.end(), e) != out.end()) continue;
            out.push_back(e);
            for (Entity c : scene.GetChildren(e)) stack.push_back(c);
        }
        return out;
    }

    std::vector<Entity> SceneVisibility::Hidden(Scene& scene) const { return Expand(scene, m_Hidden); }
    std::vector<Entity> SceneVisibility::Unpickable(Scene& scene) const { return Expand(scene, m_Unpickable); }

    void SceneVisibility::Clear()
    {
        m_Hidden.clear();
        m_Unpickable.clear();
    }

    nlohmann::json SceneVisibility::ToJson() const
    {
        auto sorted = [](const std::unordered_set<uint64_t>& set) {
            std::vector<uint64_t> v(set.begin(), set.end());
            std::sort(v.begin(), v.end());
            return v;
        };
        return { { "hidden", sorted(m_Hidden) }, { "unpickable", sorted(m_Unpickable) } };
    }

    void SceneVisibility::FromJson(const nlohmann::json& j)
    {
        Clear();
        for (auto& g : j.value("hidden", nlohmann::json::array()))
            if (g.is_number_unsigned()) m_Hidden.insert(g.get<uint64_t>());
        for (auto& g : j.value("unpickable", nlohmann::json::array()))
            if (g.is_number_unsigned()) m_Unpickable.insert(g.get<uint64_t>());
    }

    void SceneVisibility::Load(const fs::path& file, const std::string& key)
    {
        Clear();
        std::ifstream in(file);
        if (!in) return;
        try
        {
            auto all = nlohmann::json::parse(in);
            if (all.contains(key)) FromJson(all[key]);
        }
        catch (const std::exception&)
        {
        }
    }

    void SceneVisibility::Save(const fs::path& file, const std::string& key) const
    {
        nlohmann::json all = nlohmann::json::object();
        if (std::ifstream in(file); in)
        {
            try
            {
                all = nlohmann::json::parse(in);
            }
            catch (const std::exception&)
            {
                all = nlohmann::json::object();
            }
        }
        if (Empty()) all.erase(key);
        else all[key] = ToJson();
        std::error_code ec;
        fs::create_directories(file.parent_path(), ec);
        std::ofstream(file) << all.dump(2);
    }
}
