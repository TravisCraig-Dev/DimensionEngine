#include "Dimension/Code/BuildDiagnostics.h"
#include <algorithm>
#include <cctype>

namespace Dimension::Code
{
    namespace
    {
        std::string_view Trim(std::string_view s)
        {
            while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
            while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) s.remove_suffix(1);
            return s;
        }

        // "12,5" or "12,5,12,9" (or "12" / "12-14" as MSBuild also writes): false when it is not numbers.
        bool ParsePlace(std::string_view s, int (&v)[4], int& count)
        {
            count = 0;
            int value = 0;
            bool digits = false;
            for (size_t i = 0; i <= s.size(); ++i)
            {
                char c = i < s.size() ? s[i] : ',';
                if (std::isdigit((unsigned char)c))
                {
                    value = value * 10 + (c - '0');
                    digits = true;
                    if (value > 100000000) return false;
                }
                else if (c == ',' || c == '-')
                {
                    if (!digits || count == 4) return false;
                    v[count++] = value;
                    value = 0;
                    digits = false;
                }
                else if (c != ' ') return false;
            }
            return count > 0;
        }
    }

    bool ParseBuildLine(std::string_view line, Diagnostic& out)
    {
        line = Trim(line);
        // Visual Studio / msbuild -m prefixes a node number: "1>".
        size_t gt = line.find('>');
        if (gt != std::string_view::npos && gt > 0 && gt < 4 &&
            std::all_of(line.begin(), line.begin() + (long long)gt, [](char c) { return std::isdigit((unsigned char)c); }))
            line = Trim(line.substr(gt + 1));

        // "<origin>: error <code>: <message>" - the first ": error " / ": warning " after the origin (a Windows
        // path's drive colon has no space before it, so it can't be mistaken for this one).
        Severity level = Severity::Error;
        size_t at = std::string_view::npos, keyword = 0;
        for (size_t i = 0; i + 1 < line.size(); ++i)
        {
            if (line[i] != ':') continue;
            std::string_view rest = line.substr(i + 1);
            size_t skip = 0;
            while (skip < rest.size() && rest[skip] == ' ') ++skip;
            rest = rest.substr(skip);
            auto starts = [&](std::string_view word) {
                return rest.size() > word.size() && rest.substr(0, word.size()) == word && rest[word.size()] == ' ';
            };
            if (starts("error")) level = Severity::Error, keyword = 5;
            else if (starts("fatal error")) level = Severity::Error, keyword = 11;
            else if (starts("warning")) level = Severity::Warning, keyword = 7;
            else continue;
            at = i;
            keyword += i + 1 + skip;
            break;
        }
        if (at == std::string_view::npos) return false;

        Diagnostic d;
        d.Level = level;
        std::string_view origin = Trim(line.substr(0, at));
        std::string_view rest = Trim(line.substr(keyword));
        // The code ("CS1002"), up to the next colon; a message without a code has none.
        size_t colon = rest.find(':');
        std::string_view code = colon == std::string_view::npos ? std::string_view() : Trim(rest.substr(0, colon));
        bool isCode = !code.empty() && code.size() <= 16 &&
                      std::all_of(code.begin(), code.end(), [](char c) { return std::isalnum((unsigned char)c) || c == '_'; });
        if (isCode)
        {
            d.Code = std::string(code);
            rest = Trim(rest.substr(colon + 1));
        }
        // " [C:\Game\GameScripts.csproj]" at the end.
        if (!rest.empty() && rest.back() == ']')
        {
            size_t open = rest.rfind(" [");
            if (open != std::string_view::npos)
            {
                d.Project = std::string(rest.substr(open + 2, rest.size() - open - 3));
                rest = Trim(rest.substr(0, open));
            }
        }
        d.Message = std::string(rest);

        // The origin: "file(line,col[,endLine,endCol])", or a file / tool name alone.
        if (!origin.empty() && origin.back() == ')')
        {
            size_t open = origin.rfind('(');
            int v[4] = {}, count = 0;
            if (open != std::string_view::npos && ParsePlace(origin.substr(open + 1, origin.size() - open - 2), v, count))
            {
                d.File = std::string(Trim(origin.substr(0, open)));
                d.Line = std::max(0, v[0] - 1);
                d.Col = count >= 2 ? std::max(0, v[1] - 1) : 0;
                if (count == 4)
                {
                    d.EndLine = std::max(0, v[2] - 1);
                    d.EndCol = std::max(0, v[3] - 1);
                }
            }
            else d.File = std::string(origin);
        }
        else d.File = std::string(origin);
        out = std::move(d);
        return true;
    }

    std::vector<Diagnostic> ParseBuildOutput(std::string_view output)
    {
        std::vector<Diagnostic> all;
        size_t start = 0;
        while (start <= output.size())
        {
            size_t end = output.find('\n', start);
            if (end == std::string_view::npos) end = output.size();
            Diagnostic d;
            if (ParseBuildLine(output.substr(start, end - start), d) && std::find(all.begin(), all.end(), d) == all.end())
                all.push_back(std::move(d));
            start = end + 1;
        }
        return all;
    }

    int Utf16ToByte(const std::string& line, int col)
    {
        int units = 0;
        size_t i = 0;
        while (i < line.size() && units < col)
        {
            unsigned char c = (unsigned char)line[i];
            int len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
            units += len == 4 ? 2 : 1; // (past the BMP: a surrogate pair)
            i = std::min(line.size(), i + (size_t)len);
        }
        return (int)i;
    }

    int ByteToUtf16(const std::string& line, int byte)
    {
        int units = 0;
        size_t i = 0, end = (size_t)std::clamp(byte, 0, (int)line.size());
        while (i < end)
        {
            unsigned char c = (unsigned char)line[i];
            int len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
            units += len == 4 ? 2 : 1;
            i += (size_t)len;
        }
        return units;
    }
}
