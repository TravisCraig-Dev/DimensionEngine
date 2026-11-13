#include "Dimension/Project/Localization.h"
#include "Dimension/Asset/AssetManager.h"
#include "Dimension/Core/Log.h"
#include "Dimension/Core/PlayerPrefs.h"
#include "Dimension/Project/Project.h"
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace Dimension
{
    namespace
    {
        constexpr const char* kLanguagePref = "Language"; // the PlayerPrefs key keeping the player's choice

        bool s_Loaded = false;
        std::vector<StringTable::Language> s_Languages;
        std::unordered_map<std::string, std::vector<std::string>> s_Strings; // key -> text per s_Languages index
        std::string s_Current;
        bool s_Chosen = false; // s_Current picked (after a load, or by SetLanguage)
        std::string s_SystemOverride;
        std::unordered_set<std::string> s_Warned;

        std::string Lower(std::string s)
        {
            for (char& c : s) c = (char)std::tolower((unsigned char)c);
            std::replace(s.begin(), s.end(), '_', '-');
            return s;
        }

        std::string Trim(std::string_view s)
        {
            size_t a = 0, b = s.size();
            while (a < b && std::isspace((unsigned char)s[a])) ++a;
            while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
            return std::string(s.substr(a, b - a));
        }

        int IndexOf(const std::string& code)
        {
            std::string c = Lower(code);
            for (size_t i = 0; i < s_Languages.size(); ++i)
                if (Lower(s_Languages[i].Code) == c) return (int)i;
            return -1;
        }

        int DefaultIndex()
        {
            int i = IndexOf(Project::GetConfig().DefaultLanguage);
            return i >= 0 ? i : (s_Languages.empty() ? -1 : 0);
        }

        // The language to start in: the player's last choice, the system's, the default.
        void Choose()
        {
            s_Chosen = true;
            std::string pick = Localization::Match(PlayerPrefs::GetString(kLanguagePref));
            if (pick.empty()) pick = Localization::Match(Localization::SystemLanguage());
            if (pick.empty() && DefaultIndex() >= 0) pick = s_Languages[DefaultIndex()].Code;
            s_Current = pick;
        }

        void Load()
        {
            if (s_Loaded) return;
            s_Loaded = true;
            s_Languages.clear();
            s_Strings.clear();
            s_Warned.clear();
            if (!AssetManager::IsInitialized()) return;
            std::vector<AssetHandle> tables = AssetManager::GetAssetsOfType(AssetType::StringTable);
            std::sort(tables.begin(), tables.end(), [](AssetHandle a, AssetHandle b) {
                return AssetManager::GetMetadata(a)->Path < AssetManager::GetMetadata(b)->Path;
            });
            std::unordered_map<std::string, std::string> keyTable; // where each key came from (for the warning)
            for (AssetHandle h : tables)
            {
                std::string path = AssetManager::GetMetadata(h)->Path.generic_string(), error;
                auto table = StringTable::Load(AssetManager::GetAbsolutePath(h), &error);
                if (!table)
                {
                    Log::Warn("String table '{}': {}", path, error);
                    continue;
                }
                std::vector<int> column; // this table's columns -> s_Languages
                for (const auto& lang : table->Languages)
                {
                    int i = IndexOf(lang.Code);
                    if (i < 0)
                    {
                        i = (int)s_Languages.size();
                        s_Languages.push_back(lang);
                    }
                    column.push_back(i);
                }
                for (const auto& entry : table->Entries)
                {
                    if (auto [it, added] = keyTable.emplace(entry.Key, path); !added)
                    {
                        Log::Warn("String table '{}': key '{}' is already in '{}' (that one is used)", path, entry.Key, it->second);
                        continue;
                    }
                    auto& texts = s_Strings[entry.Key];
                    for (size_t c = 0; c < entry.Texts.size() && c < column.size(); ++c)
                    {
                        if ((size_t)column[c] >= texts.size()) texts.resize(column[c] + 1);
                        texts[column[c]] = entry.Texts[c];
                    }
                }
            }
            // The language picked before still applies when the tables still have it.
            if (!s_Chosen || IndexOf(s_Current) < 0) Choose();
        }

        // The key's text in a language (by index), else in the default language; nullptr for unknown keys.
        const std::string* Find(const std::string& key, int language)
        {
            auto it = s_Strings.find(key);
            if (it == s_Strings.end())
            {
                if (s_Warned.insert(key).second) Log::Warn("Localization: no string table has the key '{}'", key);
                return nullptr;
            }
            const auto& texts = it->second;
            if (language >= 0 && language < (int)texts.size() && !texts[language].empty()) return &texts[language];
            int fallback = DefaultIndex();
            if (fallback >= 0 && fallback < (int)texts.size() && !texts[fallback].empty()) return &texts[fallback];
            for (const std::string& t : texts) // any translation beats none
                if (!t.empty()) return &t;
            static const std::string empty;
            return &empty;
        }
    }

    std::optional<StringTable> StringTable::Parse(std::string_view csv, std::string* error)
    {
        auto rows = Localization::ParseCsv(csv);
        auto fail = [&](const char* message) -> std::optional<StringTable> {
            if (error) *error = message;
            return std::nullopt;
        };
        if (rows.empty()) return fail("empty (the first row names the languages: Key,English (en),...)");
        if (rows[0].size() < 2) return fail("the first row needs a language after the key column (Key,English (en),...)");
        StringTable table;
        for (size_t c = 1; c < rows[0].size(); ++c)
        {
            std::string cell = Trim(rows[0][c]);
            if (cell.empty()) continue; // a trailing comma
            Language lang;
            size_t open = cell.rfind('(');
            if (open != std::string::npos && cell.back() == ')')
            {
                lang.Code = Trim(std::string_view(cell).substr(open + 1, cell.size() - open - 2));
                lang.Name = Trim(std::string_view(cell).substr(0, open));
            }
            else lang.Code = cell;
            if (lang.Name.empty()) lang.Name = lang.Code;
            table.Languages.push_back(std::move(lang));
        }
        if (table.Languages.empty()) return fail("the first row names no language");
        for (size_t r = 1; r < rows.size(); ++r)
        {
            std::string key = Trim(rows[r][0]);
            if (key.empty() || key[0] == '#') continue; // blank lines and comments
            Entry e{ std::move(key), {} };
            for (size_t c = 0; c < table.Languages.size(); ++c) e.Texts.push_back(c + 1 < rows[r].size() ? rows[r][c + 1] : std::string());
            table.Entries.push_back(std::move(e));
        }
        return table;
    }

    std::optional<StringTable> StringTable::Load(const fs::path& path, std::string* error)
    {
        std::ifstream in(path, std::ios::binary);
        if (!in)
        {
            if (error) *error = "could not be read";
            return std::nullopt;
        }
        std::stringstream ss;
        ss << in.rdbuf();
        return Parse(ss.str(), error);
    }

    namespace Localization
    {
        std::vector<std::vector<std::string>> ParseCsv(std::string_view text)
        {
            if (text.size() >= 3 && (uint8_t)text[0] == 0xEF && (uint8_t)text[1] == 0xBB && (uint8_t)text[2] == 0xBF) text.remove_prefix(3);
            std::vector<std::vector<std::string>> rows;
            std::vector<std::string> row;
            std::string cell;
            bool quoted = false, any = false; // inside quotes; the row has content
            for (size_t i = 0; i < text.size(); ++i)
            {
                char c = text[i];
                if (quoted)
                {
                    if (c == '"' && i + 1 < text.size() && text[i + 1] == '"') cell += '"', ++i;
                    else if (c == '"') quoted = false;
                    else if (c != '\r') cell += c; // line breaks in a quoted cell are kept (as \n)
                    continue;
                }
                if (c == '"') quoted = any = true;
                else if (c == ',') row.push_back(std::move(cell)), cell.clear(), any = true;
                else if (c == '\n' || c == '\r')
                {
                    if (c == '\r' && i + 1 < text.size() && text[i + 1] == '\n') ++i;
                    if (any || !cell.empty())
                    {
                        row.push_back(std::move(cell));
                        rows.push_back(std::move(row));
                    }
                    row.clear(), cell.clear(), any = false;
                }
                else cell += c, any = true;
            }
            if (any || !cell.empty())
            {
                row.push_back(std::move(cell));
                rows.push_back(std::move(row));
            }
            return rows;
        }

        const std::vector<StringTable::Language>& Languages()
        {
            Load();
            return s_Languages;
        }

        std::string Get(const std::string& key) { return Get(key, Language()); }

        std::string Get(const std::string& key, const std::string& language)
        {
            Load();
            const std::string* text = Find(key, IndexOf(language));
            return text ? *text : key;
        }

        bool Has(const std::string& key)
        {
            Load();
            return s_Strings.contains(key);
        }

        std::vector<std::string> Keys()
        {
            Load();
            std::vector<std::string> keys;
            for (const auto& [k, _] : s_Strings) keys.push_back(k);
            std::sort(keys.begin(), keys.end());
            return keys;
        }

        const std::string& Language()
        {
            Load();
            return s_Current;
        }

        std::string LanguageName(const std::string& code)
        {
            Load();
            int i = IndexOf(code);
            return i >= 0 ? s_Languages[i].Name : code;
        }

        bool SetLanguage(const std::string& code)
        {
            Load();
            std::string pick = Match(code);
            if (pick.empty()) return false;
            s_Current = pick;
            s_Chosen = true;
            PlayerPrefs::SetString(kLanguagePref, pick);
            return true;
        }

        std::string DefaultLanguage()
        {
            Load();
            int i = DefaultIndex();
            return i >= 0 ? s_Languages[i].Code : std::string();
        }

        std::string SystemLanguage()
        {
            if (!s_SystemOverride.empty()) return s_SystemOverride;
#ifdef _WIN32
            wchar_t name[LOCALE_NAME_MAX_LENGTH] = {};
            if (GetUserDefaultLocaleName(name, LOCALE_NAME_MAX_LENGTH) > 0)
            {
                std::string out;
                for (const wchar_t* p = name; *p; ++p) out += (char)*p; // locale names are ASCII
                return out;
            }
#else
            // POSIX locales ("fr_FR.UTF-8", "de_DE@euro") as BCP 47 tags ("fr-FR"); C / POSIX mean none chosen.
            for (const char* var : { "LC_ALL", "LC_MESSAGES", "LANG" })
                if (const char* v = std::getenv(var); v && *v)
                {
                    std::string s(v);
                    s = s.substr(0, s.find_first_of(".@"));
                    if (s == "C" || s == "POSIX" || s.empty()) break;
                    std::replace(s.begin(), s.end(), '_', '-');
                    return s;
                }
#endif
            return "en-US";
        }

        void SetSystemLanguage(const std::string& locale)
        {
            s_SystemOverride = locale;
        }

        std::string Match(const std::string& wanted)
        {
            Load();
            std::string w = Lower(Trim(wanted));
            if (w.empty()) return {};
            if (int i = IndexOf(w); i >= 0) return s_Languages[i].Code;
            auto base = [](const std::string& code) { return code.substr(0, code.find('-')); };
            for (const auto& lang : s_Languages)
                if (base(Lower(lang.Code)) == base(w)) return lang.Code;
            return {};
        }

        void Invalidate()
        {
            s_Loaded = false;
        }

        void Reset()
        {
            s_Loaded = false;
            s_Chosen = false;
            s_Current.clear();
        }
    }
}
