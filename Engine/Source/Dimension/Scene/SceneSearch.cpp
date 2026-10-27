#include "Dimension/Scene/SceneSearch.h"
#include "Dimension/Scene/ComponentRegistry.h"
#include "Dimension/Scene/Components.h"
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

        bool MatchesWord(Scene& scene, Entity e, const std::string& word)
        {
            auto& reg = scene.GetRegistry();
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

        void Walk(Scene& scene, Entity e, const std::vector<std::string>& words, std::vector<Entity>& out)
        {
            bool all = true;
            for (const auto& w : words) all = all && MatchesWord(scene, e, w);
            if (all) out.push_back(e);
            for (Entity c : scene.GetChildren(e)) Walk(scene, c, words, out);
        }
    }

    bool Matches(Scene& scene, Entity e, const std::string& query)
    {
        for (const auto& w : Words(query))
            if (!MatchesWord(scene, e, w)) return false;
        return true;
    }

    std::vector<Entity> Find(Scene& scene, const std::string& query)
    {
        std::vector<Entity> out;
        std::vector<std::string> words = Words(query);
        if (words.empty()) return out;
        for (Entity root : scene.GetRootEntities()) Walk(scene, root, words, out);
        return out;
    }
}
