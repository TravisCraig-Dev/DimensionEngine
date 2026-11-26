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
    // checking the project's scripts as they are edited, on a thread of its own. Ask with Request (a newer request
    // replaces one still waiting); results come back through Poll.
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
        // Check the assembly `file` is in. `tag` comes back with the result (to tell which edit it was for).
        void Request(const fs::path& file, std::vector<Overlay> overlays, uint64_t tag = 0);
        bool Busy() const;

        struct Result
        {
            uint64_t Tag = 0;
            std::string Assembly;
            std::vector<fs::path> Files;                 // every source of the assembly (its problems are all here)
            std::vector<Code::Diagnostic> Diagnostics;   // (0-based lines, UTF-16 columns, with their ends)
            double Milliseconds = 0.0;
            std::string Error;                           // when the check itself failed
        };
        // The newest finished result, once.
        std::optional<Result> Poll();

        // The service's JSON answer as a Result (exposed for tests).
        static Result ParseResult(const std::string& json);

    private:
        void Worker();

        struct Pending
        {
            fs::path File;
            std::vector<Overlay> Overlays;
            uint64_t Tag = 0;
        };
        using AnalyzeFn = char* (*)(const char*);
        using FreeFn = void (*)(char*);
        AnalyzeFn m_Analyze = nullptr;
        FreeFn m_Free = nullptr;

        std::thread m_Thread;
        mutable std::mutex m_Mutex;
        std::condition_variable m_Wake;
        std::optional<Pending> m_Pending;
        std::optional<Result> m_Done;
        bool m_Working = false, m_Quit = false, m_Running = false;
    };
}
