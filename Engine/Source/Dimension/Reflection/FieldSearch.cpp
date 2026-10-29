#include "Dimension/Reflection/FieldSearch.h"
#include <cctype>
#include <sstream>

namespace Dimension::FieldSearch
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

        // The text folded, and where its words start in it (after a space, or an upper case letter after a lower case
        // one: "MaxSpeed" is "max" and "speed").
        void FoldWithStarts(const std::string& text, std::string& folded, std::vector<bool>& starts)
        {
            bool newWord = true;
            char prev = ' ';
            for (char c : text)
            {
                if (c == ' ' || c == '_' || c == '-')
                {
                    newWord = true;
                    prev = c;
                    continue;
                }
                bool camel = std::isupper((unsigned char)c) && std::islower((unsigned char)prev);
                bool digitStart = std::isdigit((unsigned char)c) && !std::isdigit((unsigned char)prev);
                starts.push_back(newWord || camel || digitStart);
                folded += (char)std::tolower((unsigned char)c);
                newWord = false;
                prev = c;
            }
        }
    }

    bool Matches(const std::string& query, const std::string& text)
    {
        // Each word of the query starts where a word of the text starts ("int" finds Intensity, not the end of
        // "Point"); it may run on into the next words ("pointlight" finds "Point Light").
        std::string folded;
        std::vector<bool> starts;
        FoldWithStarts(text, folded, starts);
        std::istringstream words(query);
        for (std::string w; words >> w;)
        {
            std::string q = Fold(w);
            bool found = false;
            for (size_t at = folded.find(q); at != std::string::npos && !found; at = folded.find(q, at + 1)) found = starts[at];
            if (!found) return false;
        }
        return true;
    }

    bool FieldShown(const std::string& query, const std::string& component, const FieldInfo& field)
    {
        return !field.Hidden && Matches(query, component + " " + field.Name);
    }

    bool ComponentNameMatches(const std::string& query, const std::string& component) { return Matches(query, component); }

    bool ComponentShown(const std::string& query, const std::string& component, const std::vector<FieldInfo>& fields)
    {
        if (ComponentNameMatches(query, component)) return true;
        for (const FieldInfo& f : fields)
            if (FieldShown(query, component, f)) return true;
        return false;
    }
}
