#pragma once
#include <cstdint>
#include <string_view>
#include <vector>

namespace Dimension::Code
{
    // What a piece of C# is, for colouring (Code Mode). Without the compiler's knowledge of the code: Roslyn's
    // classification comes later; until then names are told apart by their shape (a call, a type-like name).
    enum class TokenKind : uint8_t
    {
        Text,          // spaces and anything unclassified
        Keyword,       // class, public, new, return...
        ControlKeyword,// if, else, for, while, return, break...
        TypeKeyword,   // int, float, bool, string, void, var...
        Identifier,
        TypeName,      // a capitalised name in a type position (after new, :, <, before an identifier...)
        Method,        // a name followed by '('
        Number,
        String,
        Char,
        Comment,
        DocComment,    // /// lines
        Preprocessor,  // #region, #if...
        Operator,
        Punctuation,
        Count
    };

    struct Token
    {
        int Start = 0, Length = 0; // bytes in the line
        TokenKind Kind = TokenKind::Text;
    };

    // Where a line starts: in plain code, or inside a construct begun on an earlier line.
    struct LexState
    {
        enum Mode : uint8_t
        {
            Code, BlockComment, VerbatimString, RawString
        };
        Mode In = Code;
        uint8_t Quotes = 0; // a raw string's opening quotes (""" or more)
        bool operator==(const LexState&) const = default;
    };

    // Splits one line into tokens (appended to `out`, covering the line without gaps) and returns the state the next
    // line starts in.
    LexState LexLine(std::string_view line, LexState state, std::vector<Token>& out);

    bool IsCSharpKeyword(std::string_view word);
}
