#pragma once
#include <compare>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace Dimension::Code
{
    // A place in a document: its line and the byte in that line (UTF-8; always on a character's first byte).
    struct TextPos
    {
        int Line = 0, Col = 0;
        auto operator<=>(const TextPos&) const = default;
    };

    // A document being edited (Code Mode's script editor): lines of UTF-8 text, a cursor with the other end of its
    // selection (the anchor), and undo/redo. Every editing call is one undo step (consecutive typing, or backspacing,
    // merges into one until the cursor is moved). The file's line endings are kept: CRLF files are saved as CRLF.
    class TextBuffer
    {
    public:
        TextBuffer() { m_Lines.emplace_back(); }

        // The whole text (forgets the undo history; the document counts as saved).
        void SetText(std::string_view text);
        std::string GetText() const;
        const std::vector<std::string>& Lines() const { return m_Lines; }
        int LineCount() const { return (int)m_Lines.size(); }
        const std::string& Line(int i) const { return m_Lines[(size_t)i]; }
        bool UsesCrlf() const { return m_Crlf; }
        // Changes with every edit (for caches of what is drawn).
        uint64_t Version() const { return m_Version; }

        // Spaces per tab where columns are measured (vertical movement, drawing).
        int TabSize = 4;
        // What one level of indentation is: four spaces, or a tab in a file indented with tabs (SetText guesses).
        std::string IndentUnit = "    ";

        // ---- Cursor and selection
        TextPos Cursor() const { return m_Cursor; }
        TextPos Anchor() const { return m_Anchor; }
        bool HasSelection() const { return m_Cursor != m_Anchor; }
        TextPos SelStart() const { return m_Cursor < m_Anchor ? m_Cursor : m_Anchor; }
        TextPos SelEnd() const { return m_Cursor < m_Anchor ? m_Anchor : m_Cursor; }
        // Moves the cursor (clamped into the text); `select` keeps the anchor where it is (extends the selection).
        void SetCursor(TextPos p, bool select = false);
        void SetSelection(TextPos anchor, TextPos cursor);
        void SelectAll();
        void SelectWordAt(TextPos p);
        void SelectLines(int first, int last);
        std::string SelectedText() const { return TextIn(SelStart(), SelEnd()); }
        std::string TextIn(TextPos a, TextPos b) const;
        TextPos Clamp(TextPos p) const;

        // ---- Movement (`select` extends the selection; `word` jumps a word at a time)
        void MoveLeft(bool select, bool word);
        void MoveRight(bool select, bool word);
        void MoveVertical(int lines, bool select); // keeps the column it started from across short lines
        void MoveHome(bool select);                // to the first non-blank character, then the line's start
        void MoveEnd(bool select);
        void MoveToStart(bool select) { SetCursor({ 0, 0 }, select); }
        void MoveToEnd(bool select) { SetCursor({ LineCount() - 1, (int)m_Lines.back().size() }, select); }
        TextPos Left(TextPos p, bool word) const;
        TextPos Right(TextPos p, bool word) const;

        // ---- Editing (each an undo step)
        void Insert(std::string_view text);  // in place of the selection
        void Type(std::string_view text);    // the same, merging with the typing before it
        void Backspace(bool word);
        void Delete(bool word);
        void DeleteSelection();
        // Replaces a range (an undo step of its own; the cursor goes to the end of the new text).
        void Replace(TextPos a, TextPos b, std::string_view text);
        void IndentLines();   // the selected lines one level in
        void OutdentLines();  // one level out
        void ToggleLineComment(); // "// " before the selected lines, or off them when they all have it
        void DuplicateLines();
        void DeleteLines();
        void MoveLines(int direction); // the selected lines up (-1) or down (+1) past their neighbour

        bool CanUndo() const { return !m_Undo.empty(); }
        bool CanRedo() const { return !m_Redo.empty(); }
        bool Undo();
        bool Redo();
        // The next edit starts its own undo step (typing after a click).
        void BreakMerge() { m_MergeOpen = false; }

        // Whether the text differs from what was last loaded or saved.
        bool IsDirty() const { return CurrentId() != m_SavedId; }
        void MarkSaved() { m_SavedId = CurrentId(); }

        // ---- Searching
        struct FindOptions
        {
            bool CaseSensitive = false, WholeWord = false;
        };
        // The next match after `from` (before it, backwards), wrapping around; false when there is none.
        bool Find(std::string_view query, const FindOptions& options, TextPos from, bool backwards, TextPos& start, TextPos& end) const;
        // Every match replaced (one undo step); how many.
        int ReplaceAll(std::string_view query, std::string_view replacement, const FindOptions& options);

        // ---- Columns: where a byte is drawn (tabs reach the next multiple of TabSize), and back.
        int VisualColumn(int line, int col) const;
        int ColumnAtVisual(int line, int visual) const;
        // The leading blanks of a line.
        std::string IndentOf(int line) const;

    private:
        struct Edit
        {
            TextPos Start;
            std::string Removed, Inserted;
        };
        enum class Merge : uint8_t
        {
            None, Typing, Backspace, Delete
        };
        struct Group
        {
            std::vector<Edit> Edits;
            TextPos CursorBefore, AnchorBefore, CursorAfter, AnchorAfter;
            Merge Kind = Merge::None;
            uint64_t Id = 0;
        };

        // Removes [a, b) and inserts `text` there, recorded in the open group; returns the end of the new text.
        TextPos ReplaceRecorded(TextPos a, TextPos b, std::string_view text);
        TextPos ReplaceRaw(TextPos a, TextPos b, std::string_view text);
        static TextPos EndOf(TextPos start, std::string_view text);
        void Begin(Merge kind);
        void End();
        uint64_t CurrentId() const { return m_Undo.empty() ? 0 : m_Undo.back().Id; }
        // The selected lines (a selection ending at a line's start leaves that line out).
        void SelectedLineRange(int& first, int& last) const;

        std::vector<std::string> m_Lines;
        TextPos m_Cursor, m_Anchor;
        int m_PreferredVisual = -1; // the column vertical moves keep (-1: take the cursor's)
        bool m_Crlf = false;
        uint64_t m_Version = 1;
        std::vector<Group> m_Undo, m_Redo;
        uint64_t m_NextId = 1, m_SavedId = 0;
        bool m_MergeOpen = false;
        int m_Depth = 0;
    };

    // UTF-8: the start of the character before / after byte `i` of `s`.
    int PrevChar(const std::string& s, int i);
    int NextChar(const std::string& s, int i);
}
