#pragma once
#include "Dimension/Core/Base.h"
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Dimension
{
    // A string table asset (.csv, like a sheet of Unity's Localization tables): a header row "Key,English (en),
    // French (fr),..." (a column's language code in parentheses, or the cell is the code), then a row per key with
    // its text in each language. Cells follow RFC 4180 (quotes around commas, line breaks and "" for a quote).
    struct StringTable
    {
        struct Language
        {
            std::string Code; // "en", "fr-CA"
            std::string Name; // "English" (the code when the header gives no name)
        };
        std::vector<Language> Languages; // one per column after the key
        struct Entry
        {
            std::string Key;
            std::vector<std::string> Texts; // per language ("" where not translated)
        };
        std::vector<Entry> Entries;

        static std::optional<StringTable> Parse(std::string_view csv, std::string* error = nullptr);
        static std::optional<StringTable> Load(const fs::path& path, std::string* error = nullptr);
    };

    // The project's text in the player's language: every string table under Assets, merged (a key in two tables: the
    // first by path wins, with a warning). The language starts as the one chosen last (kept in PlayerPrefs), else the
    // system's (matched by code: "fr-CA" also takes "fr"), else the project's default language (Project Settings >
    // Localization), else the first column. A text missing in the language falls back to the default language's.
    namespace Localization
    {
        // Rows of cells from CSV text (a UTF-8 byte order mark is skipped).
        std::vector<std::vector<std::string>> ParseCsv(std::string_view text);

        // The languages of all tables, in column order of first appearance.
        const std::vector<StringTable::Language>& Languages();
        // The key's text in the current language (or in `language`); unknown keys give the key itself (with a warning,
        // once per key).
        std::string Get(const std::string& key);
        std::string Get(const std::string& key, const std::string& language);
        bool Has(const std::string& key);
        std::vector<std::string> Keys(); // sorted

        const std::string& Language(); // the current language's code ("" without string tables)
        std::string LanguageName(const std::string& code);
        // Picks the language (by code, or the best match: "de-AT" picks "de") and keeps the choice for the next run;
        // false (nothing changes) when no table has it.
        bool SetLanguage(const std::string& code);
        // The default language: the project's setting when a table has it, else the first column.
        std::string DefaultLanguage();
        // The system's language as a locale name ("fr-FR"); tests may set another ("" goes back to the system's).
        std::string SystemLanguage();
        void SetSystemLanguage(const std::string& locale);
        // The closest code to `wanted` among the tables' languages: the same code, else the same base language
        // ("pt-BR" and "pt", or "pt" and "pt-PT"), case-insensitive; "" when none is close.
        std::string Match(const std::string& wanted);

        // A string table was imported, changed or removed: the tables are read again on the next call.
        void Invalidate();
        // Another project: tables read again and the language chosen again (from its PlayerPrefs).
        void Reset();
    }
}
