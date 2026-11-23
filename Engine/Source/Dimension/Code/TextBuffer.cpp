#include "Dimension/Code/TextBuffer.h"
#include <algorithm>
#include <cctype>

namespace Dimension::Code
{
    namespace
    {
        bool IsContinuation(unsigned char c) { return (c & 0xC0) == 0x80; }

        // Character classes for word jumps: blanks, word characters (letters, digits, _, anything non-ASCII), the rest.
        int ClassOf(unsigned char c)
        {
            if (c == ' ' || c == '\t') return 0;
            if (std::isalnum(c) || c == '_' || c >= 0x80) return 1;
            return 2;
        }

        bool IsWordChar(unsigned char c) { return ClassOf(c) == 1; }

        char Lower(char c) { return (char)std::tolower((unsigned char)c); }
    }

    int PrevChar(const std::string& s, int i)
    {
        i = std::min(i, (int)s.size());
        if (i <= 0) return 0;
        --i;
        while (i > 0 && IsContinuation((unsigned char)s[(size_t)i])) --i;
        return i;
    }

    int NextChar(const std::string& s, int i)
    {
        if (i >= (int)s.size()) return (int)s.size();
        ++i;
        while (i < (int)s.size() && IsContinuation((unsigned char)s[(size_t)i])) ++i;
        return i;
    }

    // ---- Text

    void TextBuffer::SetText(std::string_view text)
    {
        m_Lines.clear();
        m_Crlf = false;
        std::string line;
        for (size_t i = 0; i < text.size(); ++i)
        {
            char c = text[i];
            if (c == '\r')
            {
                if (i + 1 < text.size() && text[i + 1] == '\n') m_Crlf = true;
                else
                {
                    m_Lines.push_back(std::move(line)); // (a lone CR ends a line too)
                    line.clear();
                }
                continue;
            }
            if (c == '\n')
            {
                m_Lines.push_back(std::move(line));
                line.clear();
                continue;
            }
            line += c;
        }
        m_Lines.push_back(std::move(line));
        // Indented with tabs: Tab inserts tabs.
        int tabs = 0, spaces = 0;
        for (const std::string& l : m_Lines)
        {
            if (l.empty()) continue;
            if (l[0] == '\t') ++tabs;
            else if (l.size() >= 2 && l[0] == ' ' && l[1] == ' ') ++spaces;
        }
        IndentUnit = tabs > spaces ? "\t" : std::string((size_t)TabSize, ' ');
        m_Cursor = m_Anchor = {};
        m_PreferredVisual = -1;
        m_Undo.clear();
        m_Redo.clear();
        m_SavedId = 0;
        m_MergeOpen = false;
        ++m_Version;
    }

    std::string TextBuffer::GetText() const
    {
        std::string out;
        const char* eol = m_Crlf ? "\r\n" : "\n";
        for (size_t i = 0; i < m_Lines.size(); ++i)
        {
            if (i) out += eol;
            out += m_Lines[i];
        }
        return out;
    }

    std::string TextBuffer::TextIn(TextPos a, TextPos b) const
    {
        a = Clamp(a);
        b = Clamp(b);
        if (b < a) std::swap(a, b);
        if (a.Line == b.Line) return m_Lines[(size_t)a.Line].substr((size_t)a.Col, (size_t)(b.Col - a.Col));
        std::string out = m_Lines[(size_t)a.Line].substr((size_t)a.Col);
        for (int l = a.Line + 1; l < b.Line; ++l) out += "\n" + m_Lines[(size_t)l];
        out += "\n" + m_Lines[(size_t)b.Line].substr(0, (size_t)b.Col);
        return out;
    }

    TextPos TextBuffer::Clamp(TextPos p) const
    {
        p.Line = std::clamp(p.Line, 0, LineCount() - 1);
        const std::string& s = m_Lines[(size_t)p.Line];
        p.Col = std::clamp(p.Col, 0, (int)s.size());
        while (p.Col > 0 && p.Col < (int)s.size() && IsContinuation((unsigned char)s[(size_t)p.Col])) --p.Col;
        return p;
    }

    int TextBuffer::VisualColumn(int line, int col) const
    {
        const std::string& s = m_Lines[(size_t)std::clamp(line, 0, LineCount() - 1)];
        int visual = 0;
        for (int i = 0; i < col && i < (int)s.size(); i = NextChar(s, i)) visual = s[(size_t)i] == '\t' ? (visual / TabSize + 1) * TabSize : visual + 1;
        return visual;
    }

    int TextBuffer::ColumnAtVisual(int line, int visual) const
    {
        const std::string& s = m_Lines[(size_t)std::clamp(line, 0, LineCount() - 1)];
        int at = 0;
        for (int i = 0; i < (int)s.size(); i = NextChar(s, i))
        {
            int next = s[(size_t)i] == '\t' ? (at / TabSize + 1) * TabSize : at + 1;
            if (visual < next) return visual - at <= next - visual ? i : NextChar(s, i); // (the nearer side)
            at = next;
        }
        return (int)s.size();
    }

    std::string TextBuffer::IndentOf(int line) const
    {
        const std::string& s = m_Lines[(size_t)line];
        size_t n = 0;
        while (n < s.size() && (s[n] == ' ' || s[n] == '\t')) ++n;
        return s.substr(0, n);
    }

    // ---- Cursor

    void TextBuffer::SetCursor(TextPos p, bool select)
    {
        m_Cursor = Clamp(p);
        if (!select) m_Anchor = m_Cursor;
        m_PreferredVisual = -1;
        m_MergeOpen = false;
    }

    void TextBuffer::SetSelection(TextPos anchor, TextPos cursor)
    {
        m_Anchor = Clamp(anchor);
        m_Cursor = Clamp(cursor);
        m_PreferredVisual = -1;
        m_MergeOpen = false;
    }

    void TextBuffer::SelectAll() { SetSelection({ 0, 0 }, { LineCount() - 1, (int)m_Lines.back().size() }); }

    void TextBuffer::SelectWordAt(TextPos p)
    {
        p = Clamp(p);
        const std::string& s = m_Lines[(size_t)p.Line];
        if (s.empty()) return SetCursor(p);
        // The run of the class under the cursor (or just before it, at a line's end).
        int at = p.Col < (int)s.size() ? p.Col : PrevChar(s, p.Col);
        int cls = ClassOf((unsigned char)s[(size_t)at]);
        int a = at, b = NextChar(s, at);
        while (a > 0 && ClassOf((unsigned char)s[(size_t)PrevChar(s, a)]) == cls) a = PrevChar(s, a);
        while (b < (int)s.size() && ClassOf((unsigned char)s[(size_t)b]) == cls) b = NextChar(s, b);
        SetSelection({ p.Line, a }, { p.Line, b });
    }

    void TextBuffer::SelectLines(int first, int last)
    {
        if (first > last) std::swap(first, last);
        TextPos end = last + 1 < LineCount() ? TextPos{ last + 1, 0 } : TextPos{ last, (int)m_Lines[(size_t)last].size() };
        SetSelection({ first, 0 }, end);
    }

    TextPos TextBuffer::Left(TextPos p, bool word) const
    {
        p = Clamp(p);
        if (p.Col == 0) return p.Line > 0 ? TextPos{ p.Line - 1, (int)m_Lines[(size_t)p.Line - 1].size() } : p;
        const std::string& s = m_Lines[(size_t)p.Line];
        if (!word) return { p.Line, PrevChar(s, p.Col) };
        int i = p.Col;
        while (i > 0 && ClassOf((unsigned char)s[(size_t)PrevChar(s, i)]) == 0) i = PrevChar(s, i);
        if (i == 0) return { p.Line, 0 };
        int cls = ClassOf((unsigned char)s[(size_t)PrevChar(s, i)]);
        while (i > 0 && ClassOf((unsigned char)s[(size_t)PrevChar(s, i)]) == cls) i = PrevChar(s, i);
        return { p.Line, i };
    }

    TextPos TextBuffer::Right(TextPos p, bool word) const
    {
        p = Clamp(p);
        const std::string& s = m_Lines[(size_t)p.Line];
        if (p.Col >= (int)s.size()) return p.Line + 1 < LineCount() ? TextPos{ p.Line + 1, 0 } : p;
        if (!word) return { p.Line, NextChar(s, p.Col) };
        int i = p.Col;
        int cls = ClassOf((unsigned char)s[(size_t)i]);
        if (cls != 0)
            while (i < (int)s.size() && ClassOf((unsigned char)s[(size_t)i]) == cls) i = NextChar(s, i);
        while (i < (int)s.size() && ClassOf((unsigned char)s[(size_t)i]) == 0) i = NextChar(s, i);
        return { p.Line, i };
    }

    void TextBuffer::MoveLeft(bool select, bool word)
    {
        if (HasSelection() && !select) return SetCursor(SelStart());
        SetCursor(Left(m_Cursor, word), select);
    }

    void TextBuffer::MoveRight(bool select, bool word)
    {
        if (HasSelection() && !select) return SetCursor(SelEnd());
        SetCursor(Right(m_Cursor, word), select);
    }

    void TextBuffer::MoveVertical(int lines, bool select)
    {
        int visual = m_PreferredVisual >= 0 ? m_PreferredVisual : VisualColumn(m_Cursor.Line, m_Cursor.Col);
        int line = m_Cursor.Line + lines;
        TextPos to;
        if (line < 0) to = { 0, 0 };
        else if (line >= LineCount()) to = { LineCount() - 1, (int)m_Lines.back().size() };
        else to = { line, ColumnAtVisual(line, visual) };
        SetCursor(to, select);
        m_PreferredVisual = visual; // (SetCursor forgot it)
    }

    void TextBuffer::MoveHome(bool select)
    {
        int first = (int)IndentOf(m_Cursor.Line).size();
        SetCursor({ m_Cursor.Line, m_Cursor.Col == first ? 0 : first }, select);
    }

    void TextBuffer::MoveEnd(bool select) { SetCursor({ m_Cursor.Line, (int)m_Lines[(size_t)m_Cursor.Line].size() }, select); }

    // ---- Recording

    TextPos TextBuffer::EndOf(TextPos start, std::string_view text)
    {
        TextPos end = start;
        for (char c : text)
        {
            if (c == '\n')
            {
                ++end.Line;
                end.Col = 0;
            }
            else ++end.Col;
        }
        return end;
    }

    TextPos TextBuffer::ReplaceRaw(TextPos a, TextPos b, std::string_view text)
    {
        // Out with [a, b).
        std::string& first = m_Lines[(size_t)a.Line];
        if (a.Line == b.Line) first.erase((size_t)a.Col, (size_t)(b.Col - a.Col));
        else
        {
            first = first.substr(0, (size_t)a.Col) + m_Lines[(size_t)b.Line].substr((size_t)b.Col);
            m_Lines.erase(m_Lines.begin() + a.Line + 1, m_Lines.begin() + b.Line + 1);
        }
        // In with the text, at a.
        std::vector<std::string> parts(1);
        for (char c : text)
        {
            if (c == '\r') continue;
            if (c == '\n') parts.emplace_back();
            else parts.back() += c;
        }
        std::string& line = m_Lines[(size_t)a.Line];
        TextPos end;
        if (parts.size() == 1)
        {
            line.insert((size_t)a.Col, parts[0]);
            end = { a.Line, a.Col + (int)parts[0].size() };
        }
        else
        {
            std::string tail = line.substr((size_t)a.Col);
            line = line.substr(0, (size_t)a.Col) + parts[0];
            for (size_t i = 1; i < parts.size(); ++i) m_Lines.insert(m_Lines.begin() + a.Line + (long long)i, parts[i]);
            end = { a.Line + (int)parts.size() - 1, (int)parts.back().size() };
            m_Lines[(size_t)end.Line] += tail;
        }
        ++m_Version;
        return end;
    }

    TextPos TextBuffer::ReplaceRecorded(TextPos a, TextPos b, std::string_view text)
    {
        a = Clamp(a);
        b = Clamp(b);
        if (b < a) std::swap(a, b);
        std::string clean;
        clean.reserve(text.size());
        for (char c : text)
            if (c != '\r') clean += c;
        Edit e{ a, TextIn(a, b), clean };
        if (e.Removed.empty() && e.Inserted.empty()) return a;
        TextPos end = ReplaceRaw(a, b, clean);
        m_Undo.back().Edits.push_back(std::move(e));
        return end;
    }

    void TextBuffer::Begin(Merge kind)
    {
        if (m_Depth++ > 0) return;
        m_Redo.clear();
        // Typing goes on in the step before it while nothing else happened since (and that step isn't the saved one).
        if (kind != Merge::None && m_MergeOpen && !m_Undo.empty() && m_Undo.back().Kind == kind && m_Undo.back().Id != m_SavedId) return;
        Group g;
        g.Kind = kind;
        g.Id = m_NextId++;
        g.CursorBefore = m_Cursor;
        g.AnchorBefore = m_Anchor;
        m_Undo.push_back(std::move(g));
    }

    void TextBuffer::End()
    {
        if (--m_Depth > 0) return;
        Group& g = m_Undo.back();
        g.CursorAfter = m_Cursor;
        g.AnchorAfter = m_Anchor;
        Merge kind = g.Kind;
        if (g.Edits.empty()) m_Undo.pop_back();
        m_MergeOpen = kind != Merge::None;
        m_PreferredVisual = -1;
    }

    bool TextBuffer::Undo()
    {
        if (m_Undo.empty()) return false;
        Group g = std::move(m_Undo.back());
        m_Undo.pop_back();
        for (auto it = g.Edits.rbegin(); it != g.Edits.rend(); ++it) ReplaceRaw(it->Start, EndOf(it->Start, it->Inserted), it->Removed);
        m_Cursor = Clamp(g.CursorBefore);
        m_Anchor = Clamp(g.AnchorBefore);
        m_Redo.push_back(std::move(g));
        m_MergeOpen = false;
        m_PreferredVisual = -1;
        return true;
    }

    bool TextBuffer::Redo()
    {
        if (m_Redo.empty()) return false;
        Group g = std::move(m_Redo.back());
        m_Redo.pop_back();
        for (const Edit& e : g.Edits) ReplaceRaw(e.Start, EndOf(e.Start, e.Removed), e.Inserted);
        m_Cursor = Clamp(g.CursorAfter);
        m_Anchor = Clamp(g.AnchorAfter);
        m_Undo.push_back(std::move(g));
        m_MergeOpen = false;
        m_PreferredVisual = -1;
        return true;
    }

    // ---- Editing

    void TextBuffer::Insert(std::string_view text)
    {
        Begin(Merge::None);
        m_Cursor = m_Anchor = ReplaceRecorded(SelStart(), SelEnd(), text);
        End();
    }

    void TextBuffer::Type(std::string_view text)
    {
        // (a new line, or replacing a selection, starts a step of its own)
        bool own = HasSelection() || text.find('\n') != std::string_view::npos;
        Begin(own ? Merge::None : Merge::Typing);
        m_Cursor = m_Anchor = ReplaceRecorded(SelStart(), SelEnd(), text);
        End();
    }

    void TextBuffer::Replace(TextPos a, TextPos b, std::string_view text)
    {
        Begin(Merge::None);
        m_Cursor = m_Anchor = ReplaceRecorded(a, b, text);
        End();
    }

    void TextBuffer::DeleteSelection()
    {
        if (!HasSelection()) return;
        Begin(Merge::None);
        m_Cursor = m_Anchor = ReplaceRecorded(SelStart(), SelEnd(), "");
        End();
    }

    void TextBuffer::Backspace(bool word)
    {
        if (HasSelection()) return DeleteSelection();
        TextPos from = Left(m_Cursor, word);
        // In the indentation: back to the previous indent stop (four spaces at once).
        const std::string& s = m_Lines[(size_t)m_Cursor.Line];
        if (!word && m_Cursor.Col > 0 && IndentUnit != "\t" && (int)IndentOf(m_Cursor.Line).size() >= m_Cursor.Col && s[(size_t)m_Cursor.Col - 1] == ' ')
        {
            int visual = VisualColumn(m_Cursor.Line, m_Cursor.Col);
            int stop = ((visual - 1) / TabSize) * TabSize;
            int col = m_Cursor.Col;
            while (col > 0 && s[(size_t)col - 1] == ' ' && VisualColumn(m_Cursor.Line, col) > stop) --col;
            from = { m_Cursor.Line, col };
        }
        if (from == m_Cursor) return;
        Begin(Merge::Backspace);
        m_Cursor = m_Anchor = ReplaceRecorded(from, m_Cursor, "");
        End();
    }

    void TextBuffer::Delete(bool word)
    {
        if (HasSelection()) return DeleteSelection();
        TextPos to = Right(m_Cursor, word);
        if (to == m_Cursor) return;
        Begin(Merge::Delete);
        m_Cursor = m_Anchor = ReplaceRecorded(m_Cursor, to, "");
        End();
    }

    void TextBuffer::SelectedLineRange(int& first, int& last) const
    {
        first = SelStart().Line;
        last = SelEnd().Line;
        if (last > first && SelEnd().Col == 0) --last;
    }

    void TextBuffer::IndentLines()
    {
        int first, last;
        SelectedLineRange(first, last);
        TextPos anchor = m_Anchor, cursor = m_Cursor;
        Begin(Merge::None);
        int unit = (int)IndentUnit.size();
        for (int l = first; l <= last; ++l)
        {
            if (m_Lines[(size_t)l].empty() && first != last) continue; // (blank lines stay blank)
            ReplaceRecorded({ l, 0 }, { l, 0 }, IndentUnit);
            if (anchor.Line == l && (anchor.Col > 0 || !HasSelection())) anchor.Col += unit;
            if (cursor.Line == l && (cursor.Col > 0 || !HasSelection())) cursor.Col += unit;
        }
        m_Anchor = Clamp(anchor);
        m_Cursor = Clamp(cursor);
        End();
    }

    void TextBuffer::OutdentLines()
    {
        int first, last;
        SelectedLineRange(first, last);
        TextPos anchor = m_Anchor, cursor = m_Cursor;
        Begin(Merge::None);
        for (int l = first; l <= last; ++l)
        {
            const std::string& s = m_Lines[(size_t)l];
            int n = 0;
            if (!s.empty() && s[0] == '\t') n = 1;
            else
                while (n < TabSize && n < (int)s.size() && s[(size_t)n] == ' ') ++n;
            if (n == 0) continue;
            ReplaceRecorded({ l, 0 }, { l, n }, "");
            if (anchor.Line == l) anchor.Col = std::max(0, anchor.Col - n);
            if (cursor.Line == l) cursor.Col = std::max(0, cursor.Col - n);
        }
        m_Anchor = Clamp(anchor);
        m_Cursor = Clamp(cursor);
        End();
    }

    void TextBuffer::ToggleLineComment()
    {
        int first, last;
        SelectedLineRange(first, last);
        // Commented already when every line with text has "//" first.
        bool all = true;
        size_t indent = std::string::npos;
        for (int l = first; l <= last; ++l)
        {
            const std::string& s = m_Lines[(size_t)l];
            size_t i = s.find_first_not_of(" \t");
            if (i == std::string::npos) continue;
            indent = std::min(indent, i);
            if (s.compare(i, 2, "//") != 0) all = false;
        }
        if (indent == std::string::npos) return; // (only blank lines)
        TextPos anchor = m_Anchor, cursor = m_Cursor;
        Begin(Merge::None);
        for (int l = first; l <= last; ++l)
        {
            const std::string& s = m_Lines[(size_t)l];
            size_t i = s.find_first_not_of(" \t");
            if (i == std::string::npos) continue;
            int at, removed = 0, added = 0;
            if (all)
            {
                at = (int)i;
                removed = s.compare(i, 3, "// ") == 0 ? 3 : 2;
                ReplaceRecorded({ l, at }, { l, at + removed }, "");
            }
            else
            {
                at = (int)indent;
                added = 3;
                ReplaceRecorded({ l, at }, { l, at }, "// ");
            }
            auto shift = [&](TextPos& p) {
                if (p.Line != l || p.Col < at) return;
                p.Col = std::max(at, p.Col + added - removed);
            };
            shift(anchor);
            shift(cursor);
        }
        m_Anchor = Clamp(anchor);
        m_Cursor = Clamp(cursor);
        End();
    }

    void TextBuffer::DuplicateLines()
    {
        int first, last;
        SelectedLineRange(first, last);
        int count = last - first + 1;
        std::string block;
        for (int l = first; l <= last; ++l) block += "\n" + m_Lines[(size_t)l];
        TextPos anchor = m_Anchor, cursor = m_Cursor;
        Begin(Merge::None);
        ReplaceRecorded({ last, (int)m_Lines[(size_t)last].size() }, { last, (int)m_Lines[(size_t)last].size() }, block);
        anchor.Line += count;
        cursor.Line += count;
        m_Anchor = Clamp(anchor);
        m_Cursor = Clamp(cursor);
        End();
    }

    void TextBuffer::DeleteLines()
    {
        int first, last;
        SelectedLineRange(first, last);
        int column = m_Cursor.Col;
        Begin(Merge::None);
        if (last + 1 < LineCount()) ReplaceRecorded({ first, 0 }, { last + 1, 0 }, "");
        else if (first > 0) ReplaceRecorded({ first - 1, (int)m_Lines[(size_t)first - 1].size() }, { last, (int)m_Lines[(size_t)last].size() }, "");
        else ReplaceRecorded({ 0, 0 }, { last, (int)m_Lines[(size_t)last].size() }, "");
        m_Cursor = m_Anchor = Clamp({ std::min(first, LineCount() - 1), column });
        End();
    }

    void TextBuffer::MoveLines(int direction)
    {
        int first, last;
        SelectedLineRange(first, last);
        if (direction < 0 && first == 0) return;
        if (direction > 0 && last + 1 >= LineCount()) return;
        TextPos anchor = m_Anchor, cursor = m_Cursor;
        Begin(Merge::None);
        if (direction < 0)
        {
            // The line above goes below the block.
            std::string above = m_Lines[(size_t)first - 1];
            ReplaceRecorded({ first - 1, 0 }, { first, 0 }, "");
            int end = last - 1;
            ReplaceRecorded({ end, (int)m_Lines[(size_t)end].size() }, { end, (int)m_Lines[(size_t)end].size() }, "\n" + above);
            anchor.Line -= 1;
            cursor.Line -= 1;
        }
        else
        {
            // The line below goes above the block.
            std::string below = m_Lines[(size_t)last + 1];
            ReplaceRecorded({ last, (int)m_Lines[(size_t)last].size() }, { last + 1, (int)below.size() }, "");
            ReplaceRecorded({ first, 0 }, { first, 0 }, below + "\n");
            anchor.Line += 1;
            cursor.Line += 1;
        }
        m_Anchor = Clamp(anchor);
        m_Cursor = Clamp(cursor);
        End();
    }

    // ---- Searching

    bool TextBuffer::Find(std::string_view query, const FindOptions& options, TextPos from, bool backwards, TextPos& start, TextPos& end) const
    {
        if (query.empty() || query.find('\n') != std::string_view::npos) return false;
        from = Clamp(from);
        auto matchAt = [&](const std::string& s, size_t i) {
            if (i + query.size() > s.size()) return false;
            for (size_t k = 0; k < query.size(); ++k)
            {
                char a = s[i + k], b = query[k];
                if (options.CaseSensitive ? a != b : Lower(a) != Lower(b)) return false;
            }
            if (options.WholeWord)
            {
                if (i > 0 && IsWordChar((unsigned char)s[i - 1])) return false;
                if (i + query.size() < s.size() && IsWordChar((unsigned char)s[i + query.size()])) return false;
            }
            return true;
        };
        int n = LineCount();
        // Every line once from the start line around (the start line's other half last).
        for (int step = 0; step <= n; ++step)
        {
            int l = backwards ? ((from.Line - step) % n + n) % n : (from.Line + step) % n;
            const std::string& s = m_Lines[(size_t)l];
            if (!backwards)
            {
                size_t begin = step == 0 ? (size_t)from.Col : 0;
                size_t limit = step == n ? (size_t)from.Col : s.size();
                for (size_t i = begin; i + query.size() <= s.size() && i <= limit; ++i)
                    if (matchAt(s, i))
                    {
                        start = { l, (int)i };
                        end = { l, (int)(i + query.size()) };
                        return true;
                    }
            }
            else
            {
                long long top = step == 0 ? (long long)from.Col - (long long)query.size() : (long long)s.size() - (long long)query.size();
                long long bottom = step == n ? (long long)from.Col : 0;
                for (long long i = top; i >= bottom; --i)
                    if (matchAt(s, (size_t)i))
                    {
                        start = { l, (int)i };
                        end = { l, (int)(i + (long long)query.size()) };
                        return true;
                    }
            }
        }
        return false;
    }

    int TextBuffer::ReplaceAll(std::string_view query, std::string_view replacement, const FindOptions& options)
    {
        if (query.empty()) return 0;
        // The matches first (top to bottom), then replaced from the last up so the earlier ones stay put.
        std::vector<std::pair<TextPos, TextPos>> found;
        TextPos at{ 0, 0 }, a, b;
        while (Find(query, options, at, false, a, b))
        {
            if (!found.empty() && a <= found.back().first) break; // (wrapped around)
            found.push_back({ a, b });
            at = b;
        }
        if (found.empty()) return 0;
        Begin(Merge::None);
        for (auto it = found.rbegin(); it != found.rend(); ++it) ReplaceRecorded(it->first, it->second, replacement);
        m_Cursor = m_Anchor = Clamp(m_Cursor);
        End();
        return (int)found.size();
    }
}
