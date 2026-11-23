#include "Dimension/Code/CSharpLexer.h"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>

namespace Dimension::Code
{
    namespace
    {
        constexpr std::string_view kControl[] = { "break", "case", "catch", "continue", "default", "do", "else", "finally", "for", "foreach",
                                                  "goto", "if", "return", "switch", "throw", "try", "while", "yield", "await", "when" };
        constexpr std::string_view kTypes[] = { "bool", "byte", "char", "decimal", "double", "dynamic", "float", "int", "long", "nint", "nuint",
                                                "object", "sbyte", "short", "string", "uint", "ulong", "ushort", "var", "void" };
        constexpr std::string_view kKeywords[] = {
            "abstract", "add", "alias", "and", "args", "as", "ascending", "async", "base", "by", "checked", "class", "const", "delegate",
            "descending", "enum", "equals", "event", "explicit", "extern", "false", "field", "file", "fixed", "from", "get", "global", "group",
            "implicit", "in", "init", "interface", "internal", "into", "is", "join", "let", "lock", "managed", "nameof", "namespace", "new",
            "not", "notnull", "null", "on", "operator", "or", "orderby", "out", "override", "params", "partial", "private", "protected",
            "public", "readonly", "record", "ref", "remove", "required", "scoped", "sealed", "select", "set", "sizeof", "stackalloc", "static",
            "struct", "this", "true", "typeof", "unchecked", "unmanaged", "unsafe", "using", "value", "virtual", "volatile", "where", "with" };

        template <size_t N>
        bool In(const std::string_view (&list)[N], std::string_view word)
        {
            return std::find(std::begin(list), std::end(list), word) != std::end(list);
        }

        bool IdentStart(unsigned char c) { return std::isalpha(c) || c == '_' || c >= 0x80; }
        bool IdentChar(unsigned char c) { return std::isalnum(c) || c == '_' || c >= 0x80; }

        // The end of a string body starting at `i` (just past the opening quote): past the closing quote, or the line's
        // end when it does not close here. `verbatim`: "" is a quote and backslashes are plain; `closed` says which.
        size_t StringEnd(std::string_view s, size_t i, bool verbatim, bool& closed)
        {
            closed = false;
            while (i < s.size())
            {
                char c = s[i];
                if (!verbatim && c == '\\')
                {
                    i += 2;
                    continue;
                }
                if (c == '"')
                {
                    if (verbatim && i + 1 < s.size() && s[i + 1] == '"')
                    {
                        i += 2;
                        continue;
                    }
                    closed = true;
                    return i + 1;
                }
                ++i;
            }
            return s.size();
        }

        // A raw string's closing run (`quotes` quotes or more) from `i`: past it, or npos.
        size_t RawEnd(std::string_view s, size_t i, int quotes)
        {
            for (; i < s.size(); ++i)
            {
                if (s[i] != '"') continue;
                size_t run = 0;
                while (i + run < s.size() && s[i + run] == '"') ++run;
                if ((int)run >= quotes) return i + run;
                i += run - 1;
            }
            return std::string_view::npos;
        }
    }

    bool IsCSharpKeyword(std::string_view word) { return In(kKeywords, word) || In(kControl, word) || In(kTypes, word); }

    LexState LexLine(std::string_view s, LexState state, std::vector<Token>& out)
    {
        size_t first = out.size();
        auto push = [&](size_t start, size_t end, TokenKind kind) {
            if (end <= start) return;
            // (touching tokens of the same kind join)
            if (out.size() > first && out.back().Kind == kind && (size_t)(out.back().Start + out.back().Length) == start)
                out.back().Length += (int)(end - start);
            else out.push_back({ (int)start, (int)(end - start), kind });
        };
        size_t i = 0;

        // Carried in from the line before.
        if (state.In == LexState::BlockComment)
        {
            size_t end = s.find("*/");
            if (end == std::string_view::npos)
            {
                push(0, s.size(), TokenKind::Comment);
                return state;
            }
            push(0, end + 2, TokenKind::Comment);
            i = end + 2;
            state = {};
        }
        else if (state.In == LexState::VerbatimString)
        {
            bool closed;
            size_t end = StringEnd(s, 0, true, closed);
            push(0, end, TokenKind::String);
            if (!closed) return state;
            i = end;
            state = {};
        }
        else if (state.In == LexState::RawString)
        {
            size_t end = RawEnd(s, 0, state.Quotes);
            if (end == std::string_view::npos)
            {
                push(0, s.size(), TokenKind::String);
                return state;
            }
            push(0, end, TokenKind::String);
            i = end;
            state = {};
        }

        // A preprocessor line: # first (after blanks), to the end (but a trailing comment).
        {
            size_t lead = s.find_first_not_of(" \t", i);
            if (i == 0 && lead != std::string_view::npos && s[lead] == '#')
            {
                push(0, lead, TokenKind::Text);
                size_t comment = s.find("//", lead);
                push(lead, comment == std::string_view::npos ? s.size() : comment, TokenKind::Preprocessor);
                if (comment != std::string_view::npos) push(comment, s.size(), TokenKind::Comment);
                return state;
            }
        }

        std::string_view previousWord; // the last word or : < , before a name (type-name guesses)
        while (i < s.size())
        {
            unsigned char c = (unsigned char)s[i];
            if (c == ' ' || c == '\t')
            {
                size_t j = i;
                while (j < s.size() && (s[j] == ' ' || s[j] == '\t')) ++j;
                push(i, j, TokenKind::Text);
                i = j;
                continue;
            }
            // Comments.
            if (c == '/' && i + 1 < s.size() && s[i + 1] == '/')
            {
                bool doc = i + 2 < s.size() && s[i + 2] == '/' && !(i + 3 < s.size() && s[i + 3] == '/');
                push(i, s.size(), doc ? TokenKind::DocComment : TokenKind::Comment);
                return state;
            }
            if (c == '/' && i + 1 < s.size() && s[i + 1] == '*')
            {
                size_t end = s.find("*/", i + 2);
                if (end == std::string_view::npos)
                {
                    push(i, s.size(), TokenKind::Comment);
                    return { LexState::BlockComment, 0 };
                }
                push(i, end + 2, TokenKind::Comment);
                i = end + 2;
                continue;
            }
            // Strings: "..", @"..", $"..", $@"..", @$"..", """raw""" (with $ prefixes too), u8 suffixes as text.
            {
                size_t j = i;
                bool verbatim = false;
                while (j < s.size() && (s[j] == '$' || s[j] == '@'))
                {
                    if (s[j] == '@') verbatim = true;
                    ++j;
                }
                if (j < s.size() && s[j] == '"' && (j == i || j - i <= 4))
                {
                    size_t run = 0;
                    while (j + run < s.size() && s[j + run] == '"') ++run;
                    if (run >= 3)
                    {
                        size_t end = RawEnd(s, j + run, (int)run);
                        if (end == std::string_view::npos)
                        {
                            push(i, s.size(), TokenKind::String);
                            return { LexState::RawString, (uint8_t)std::min<size_t>(run, 255) };
                        }
                        push(i, end, TokenKind::String);
                        i = end;
                        continue;
                    }
                    bool closed;
                    size_t end = StringEnd(s, j + 1, verbatim, closed);
                    push(i, end, TokenKind::String);
                    if (!closed && verbatim) return { LexState::VerbatimString, 0 };
                    i = end;
                    continue;
                }
            }
            if (c == '\'')
            {
                size_t j = i + 1;
                while (j < s.size() && s[j] != '\'')
                    j += s[j] == '\\' ? 2 : 1;
                j = std::min(j + 1, s.size());
                push(i, j, TokenKind::Char);
                i = j;
                continue;
            }
            // Numbers: 12, 0x1F, 0b1010, 1_000, 3.5f, 1e-3, 10UL.
            if (std::isdigit(c) || (c == '.' && i + 1 < s.size() && std::isdigit((unsigned char)s[i + 1])))
            {
                size_t j = i;
                if (c == '0' && i + 1 < s.size() && (s[i + 1] == 'x' || s[i + 1] == 'X' || s[i + 1] == 'b' || s[i + 1] == 'B'))
                {
                    j += 2;
                    while (j < s.size() && (std::isxdigit((unsigned char)s[j]) || s[j] == '_')) ++j;
                }
                else
                {
                    while (j < s.size() && (std::isdigit((unsigned char)s[j]) || s[j] == '_' || s[j] == '.'))
                    {
                        if (s[j] == '.' && !(j + 1 < s.size() && std::isdigit((unsigned char)s[j + 1]))) break;
                        ++j;
                    }
                    if (j < s.size() && (s[j] == 'e' || s[j] == 'E'))
                    {
                        size_t k = j + 1;
                        if (k < s.size() && (s[k] == '+' || s[k] == '-')) ++k;
                        if (k < s.size() && std::isdigit((unsigned char)s[k]))
                        {
                            j = k;
                            while (j < s.size() && std::isdigit((unsigned char)s[j])) ++j;
                        }
                    }
                }
                while (j < s.size() && std::strchr("fFdDmMuUlL", s[j]) && s[j] != '\0') ++j;
                push(i, j, TokenKind::Number);
                i = j;
                continue;
            }
            // Words: keywords, @verbatim identifiers, names.
            if (IdentStart(c) || (c == '@' && i + 1 < s.size() && IdentStart((unsigned char)s[i + 1])))
            {
                size_t j = i + 1;
                while (j < s.size() && IdentChar((unsigned char)s[j])) ++j;
                std::string_view word = s.substr(i, j - i);
                TokenKind kind;
                if (word[0] != '@' && In(kControl, word)) kind = TokenKind::ControlKeyword;
                else if (word[0] != '@' && In(kTypes, word)) kind = TokenKind::TypeKeyword;
                else if (word[0] != '@' && In(kKeywords, word)) kind = TokenKind::Keyword;
                else
                {
                    // What follows: a call, a generic's <, or another name (a declaration's type).
                    size_t k = j;
                    while (k < s.size() && (s[k] == ' ' || s[k] == '\t')) ++k;
                    char next = k < s.size() ? s[k] : '\0';
                    bool capital = std::isupper((unsigned char)word[word[0] == '@' ? 1 : 0]);
                    if (next == '(') kind = TokenKind::Method;
                    else if (capital && (previousWord == "new" || previousWord == "class" || previousWord == "struct" || previousWord == "interface" ||
                                         previousWord == "enum" || previousWord == "record" || previousWord == "typeof" || previousWord == "is" ||
                                         previousWord == "as" || previousWord == ":" || previousWord == "<" || previousWord == "," ||
                                         next == '<' || IdentStart((unsigned char)next)))
                        kind = TokenKind::TypeName;
                    else kind = TokenKind::Identifier;
                }
                push(i, j, kind);
                previousWord = word;
                i = j;
                continue;
            }
            // Operators and punctuation (one character each; runs join).
            {
                bool punct = std::strchr("{}()[];,.", (char)c) != nullptr;
                push(i, i + 1, punct ? TokenKind::Punctuation : TokenKind::Operator);
                if (c == ':' || c == '<' || c == ',') previousWord = s.substr(i, 1);
                else if (c != '?' && c != '>' && c != '[' && c != ']') previousWord = {};
                ++i;
            }
        }
        return state;
    }
}
