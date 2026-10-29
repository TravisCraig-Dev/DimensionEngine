#include "Dimension/Project/SceneBookmarks.h"
#include "Dimension/Project/Project.h"
#include <nlohmann/json.hpp>
#include <fstream>

namespace Dimension::SceneBookmarks
{
    namespace
    {
        using json = nlohmann::json;

        fs::path File() { return Project::GetDirectory() / ".dimension" / "SceneBookmarks.json"; }

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

        // Relative to Assets when inside it.
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

    std::optional<CameraBookmark> Get(const fs::path& scene, int slot)
    {
        if (!Valid(slot)) return std::nullopt;
        json all = Read();
        std::string key = Key(scene), s = std::to_string(slot);
        if (!all.contains(key) || !all[key].is_object() || !all[key].contains(s)) return std::nullopt;
        const json& b = all[key][s];
        CameraBookmark out;
        auto pivot = b.value("pivot", std::vector<float>{ 0.0f, 0.0f, 0.0f });
        if (pivot.size() == 3) out.Pivot = { pivot[0], pivot[1], pivot[2] };
        out.Distance = b.value("distance", out.Distance);
        out.Yaw = b.value("yaw", out.Yaw);
        out.Pitch = b.value("pitch", out.Pitch);
        out.Is2D = b.value("2d", out.Is2D);
        out.OrthoSize = b.value("orthoSize", out.OrthoSize);
        return out;
    }

    void Set(const fs::path& scene, int slot, const CameraBookmark& b)
    {
        if (!Valid(slot)) return;
        json all = Read();
        all[Key(scene)][std::to_string(slot)] = { { "pivot", { b.Pivot.x, b.Pivot.y, b.Pivot.z } }, { "distance", b.Distance }, { "yaw", b.Yaw },
                                                   { "pitch", b.Pitch }, { "2d", b.Is2D }, { "orthoSize", b.OrthoSize } };
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
