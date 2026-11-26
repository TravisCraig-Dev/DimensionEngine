#pragma once
#include "Dimension/Core/Base.h"
#include "Dimension/Code/BuildDiagnostics.h"
#include <condition_variable>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace Dimension
{
    // Code Mode's language service: Roslyn in the hosted .NET (bin/managed/codeservice/Dimension.CodeService.dll)
    // checking the project's scripts as they are edited, and answering what can be typed at a place (completion), the
    // call a place is in (signature help) and what a name is (hover), on a thread of its own. Ask with Send (a newer
    // question of a kind replaces one of that kind still waiting; the quick ones go first); answers come back through
    // Poll.
    class LanguageService
    {
    public:
        LanguageService() = default;
        ~LanguageService();
        LanguageService(const LanguageService&) = delete;
        LanguageService& operator=(const LanguageService&) = delete;

        // Loads the service for a project's scripts (needs C# scripting running). False when it can't.
        bool Start(const fs::path& managedDir, const fs::path& projectDir);
        bool IsRunning() const { return m_Running; }

        struct Overlay
        {
            fs::path Path;
            std::string Text; // the text being edited (in place of the file on disk)
        };
        enum class Query : uint8_t
        {
            Completion, Signature, Hover, // (in the order they are answered)
            Definition, References, RenameCheck, Rename, Format,
            Diagnostics,
            Count
        };
        struct Question
        {
            Query Kind = Query::Diagnostics;
            fs::path File;
            std::vector<Overlay> Overlays;
            int Line = 0, Col = 0; // the place asked about (0-based; UTF-16 column)
            uint64_t Tag = 0;      // comes back with the result (to tell which edit or place it was for)
            std::string NewName;   // Rename: to what
            int EndLine = -1;      // Format: the last line of the lines to format (-1: the whole file)
            bool Tabs = false, Crlf = false; // Format: the file's indentation and line ends
        };
        void Send(Question question);
        // Check the assembly `file` is in.
        void Request(const fs::path& file, std::vector<Overlay> overlays, uint64_t tag = 0)
        {
            Send({ Query::Diagnostics, file, std::move(overlays), 0, 0, tag });
        }
        bool Busy(Query kind = Query::Diagnostics) const;

        struct CompletionItem
        {
            std::string Label, Kind, Detail, Doc;
            std::string Insert; // what goes in instead of the label ("$0": where the cursor goes; may span lines)
        };
        struct SignatureParam
        {
            int Start = 0, Length = 0; // in the label (bytes)
            std::string Name, Doc;
        };
        struct Signature
        {
            std::string Label, Doc;
            std::vector<SignatureParam> Params;
        };

        // A place in a file (definitions, references) and a change to make there (rename, format).
        struct Location
        {
            fs::path File;
            int Line = 0, Col = 0, EndLine = 0, EndCol = 0; // (UTF-16 columns)
            std::string Text;      // References: the line's text
            bool Definition = false;
        };
        struct Edit
        {
            fs::path File;
            int Line = 0, Col = 0, EndLine = 0, EndCol = 0; // (UTF-16 columns, in the text the question was about)
            std::string Text;
        };

        struct Result
        {
            uint64_t Tag = 0;
            std::string Assembly;
            std::vector<fs::path> Files;                 // every source of the assembly (its problems are all here)
            std::vector<Code::Diagnostic> Diagnostics;   // (0-based lines, UTF-16 columns, with their ends)
            double Milliseconds = 0.0;
            std::string Error;                           // when the check itself failed
            Query Kind = Query::Diagnostics;
            // Completion
            std::vector<CompletionItem> Items;
            // Signature help
            std::vector<Signature> Signatures;
            int ActiveSignature = 0, ActiveParam = 0;
            // Hover
            std::string HoverText, HoverDoc;
            // Definition, references, rename, format
            std::vector<Location> Locations;
            std::vector<Edit> Edits;
            std::string Name, Message, Warning;     // what was asked about; why there is no answer; a caution
            fs::path RenameFrom, RenameTo;          // Rename: the file named after a renamed type, and its new name
        };
        // The newest finished result of a kind, once.
        std::optional<Result> Poll(Query kind = Query::Diagnostics);

        // The service's JSON answer as a Result (exposed for tests).
        static Result ParseResult(const std::string& json);

    private:
        void Worker();

        using AnalyzeFn = char* (*)(const char*);
        using FreeFn = void (*)(char*);
        AnalyzeFn m_Analyze = nullptr;
        FreeFn m_Free = nullptr;

        std::thread m_Thread;
        mutable std::mutex m_Mutex;
        std::condition_variable m_Wake;
        std::optional<Question> m_Pending[(int)Query::Count];
        std::optional<Result> m_Done[(int)Query::Count];
        int m_Working = -1; // the kind being answered
        bool m_Quit = false, m_Running = false;
    };
}
