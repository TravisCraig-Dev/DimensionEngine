#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace Dimension::Code
{
    enum class Severity : uint8_t
    {
        Error,
        Warning
    };

    // One problem a build reported (a compiler error or warning), as Code Mode lists and underlines it.
    struct Diagnostic
    {
        std::string File;     // as the build wrote it (empty, or a tool's name such as "CSC", when it is about no file)
        int Line = -1;        // 0-based; -1 when it has no place in a file
        int Col = 0;          // 0-based, in UTF-16 units (what the C# compiler counts)
        int EndLine = -1;     // where it ends, when the build said (-1: not given)
        int EndCol = -1;
        Severity Level = Severity::Error;
        std::string Code;     // CS1002, MSB3021...
        std::string Message;
        std::string Project;  // the [project] MSBuild appends, without the brackets

        bool operator==(const Diagnostic&) const = default;
    };

    // A line of MSBuild / dotnet build / csc output as a diagnostic, when it is one:
    //   C:\Game\Scripts\Player.cs(12,5): error CS1002: ; expected [C:\Game\GameScripts.csproj]
    //   /home/me/Game/Scripts/Player.cs(12,5,12,9): warning CS0168: The variable 'x' is declared but never used
    //   CSC : error CS2001: Source file 'X.cs' could not be found.
    bool ParseBuildLine(std::string_view line, Diagnostic& out);

    // Every diagnostic in a build's output (repeats, which MSBuild prints again in its summary, once).
    std::vector<Diagnostic> ParseBuildOutput(std::string_view output);

    // A column in UTF-16 units (the compiler's) as the byte in a UTF-8 line, and back.
    int Utf16ToByte(const std::string& line, int col);
    int ByteToUtf16(const std::string& line, int byte);
}
