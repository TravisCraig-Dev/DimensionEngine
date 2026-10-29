#include "Dimension/Scene/SceneSearch.h"
#include "Dimension/Scene/ComponentRegistry.h"
#include "Dimension/Scene/Components.h"
#include "Dimension/Scene/SceneSerializer.h"
#include "Dimension/Asset/AssetDependencies.h"
#include "Dimension/Asset/AssetManager.h"
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <cctype>
#include <sstream>

namespace Dimension::SceneSearch
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

        bool Contains(const std::string& text, const std::string& word) { return Fold(text).find(word) != std::string::npos; }

        // For ref: words: the assets each object uses (by entity GUID), with everything those assets use in turn.
        struct References
        {
            bool Built = false;
            std::unordered_map<uint64_t, std::unordered_set<AssetHandle>> ByEntity;
        };

        void Build(Scene& scene, References& refs)
        {
            refs.Built = true;
            std::unordered_map<AssetHandle, std::unordered_set<AssetHandle>> reach; // an asset and all it uses
            nlohmann::json data = SceneSerializer::Serialize(scene);
            for (const auto& rec : data.value("entities", nlohmann::json::array()))
            {
                auto& used = refs.ByEntity[rec.value("guid", uint64_t(0))];
                for (AssetHandle a : AssetDependencies::References(rec.value("components", nlohmann::json::object())))
                {
                    // A prefab instance's objects each list what they use themselves: its link alone says only "the prefab".
                    const AssetMetadata* meta = AssetManager::GetMetadata(a);
                    if (meta && (meta->Type == AssetType::Prefab || meta->Type == AssetType::Scene))
                    {
                        used.insert(a);
                        continue;
                    }
                    auto it = reach.find(a);
                    if (it == reach.end())
                    {
                        auto set = AssetDependencies::Collect({ a }, false).Assets;
                        it = reach.emplace(a, std::unordered_set<AssetHandle>(set.begin(), set.end())).first;
                    }
                    used.insert(it->second.begin(), it->second.end());
                }
            }
        }

        bool MatchesWord(Scene& scene, Entity e, const std::string& word, References& refs)
        {
            auto& reg = scene.GetRegistry();
            if (word.rfind("ref:", 0) == 0)
            {
                std::string id = word.substr(4);
                id = id.substr(0, id.find(':'));
                uint64_t guid = 0;
                try { guid = std::stoull(id); } catch (...) { return false; }
                if (!refs.Built) Build(scene, refs);
                auto it = refs.ByEntity.find((uint64_t)scene.GetGuid(e));
                return it != refs.ByEntity.end() && it->second.contains(Guid(guid));
            }
            if (word.rfind("t:", 0) == 0)
            {
                std::string type = word.substr(2);
                if (type.empty()) return true;
                for (const auto& info : ComponentRegistry::All())
                    if (!info.Hidden && Contains(info.Name, type) && info.Has(scene, e)) return true;
                if (auto* scripts = reg.TryGet<ScriptComponent>(e))
                    for (const auto& s : scripts->Scripts)
                        if (Contains(s.ClassName, type)) return true;
                return false;
            }
            const auto& tag = reg.Get<TagComponent>(e);
            if (word.rfind("tag:", 0) == 0) return Contains(tag.Tag, word.substr(4));
            return Contains(tag.Name, word);
        }

        std::vector<std::string> Words(const std::string& query)
        {
            std::vector<std::string> words;
            std::istringstream in(query);
            for (std::string w; in >> w;)
            {
                std::string folded = Fold(w);
                if (!folded.empty()) words.push_back(folded);
            }
            return words;
        }

        void Walk(Scene& scene, Entity e, const std::vector<std::string>& words, std::vector<Entity>& out, References& refs)
        {
            bool all = true;
            for (const auto& w : words) all = all && MatchesWord(scene, e, w, refs);
            if (all) out.push_back(e);
            for (Entity c : scene.GetChildren(e)) Walk(scene, c, words, out, refs);
        }
    }

    bool Matches(Scene& scene, Entity e, const std::string& query)
    {
        References refs;
        for (const auto& w : Words(query))
            if (!MatchesWord(scene, e, w, refs)) return false;
        return true;
    }

    std::vector<Entity> Find(Scene& scene, const std::string& query)
    {
        std::vector<Entity> out;
        std::vector<std::string> words = Words(query);
        if (words.empty()) return out;
        References refs;
        for (Entity root : scene.GetRootEntities()) Walk(scene, root, words, out, refs);
        return out;
    }

    std::string ReferenceQuery(AssetHandle asset)
    {
        const AssetMetadata* meta = AssetManager::GetMetadata(asset);
        std::string name = meta ? meta->Path.filename().string() : std::string();
        std::replace(name.begin(), name.end(), ' ', '_'); // one search word
        return "ref:" + std::to_string((uint64_t)asset) + (name.empty() ? std::string() : ":" + name);
    }
}
