#pragma once
#include "Dimension/Core/Base.h"
#include "Dimension/Platform/ChildProcess.h"
#include <nlohmann/json_fwd.hpp>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace Dimension
{
    // Debugging C# scripts (Code Mode's debugger): the game runs in a player process of its own, and a debug engine
    // (netcoredbg, driven through the Debug Adapter Protocol) attaches to it. The editor stays responsive while the game
    // is stopped at a breakpoint and shows where it stopped, its call stack and variables. Call Update every frame (it
    // takes in what the debugger and the player said; callbacks run there).
    class DebugSession
    {
    public:
        DebugSession() = default;
        ~DebugSession();
        DebugSession(const DebugSession&) = delete;
        DebugSession& operator=(const DebugSession&) = delete;

        enum class State
        {
            Idle,     // nothing running
            Starting, // the player started, the debugger attaching
            Running,
            Stopped,  // at a breakpoint, a step's end or a pause
            Ended     // the game ended (or could not start: see Error)
        };

        struct Options
        {
            fs::path Debugger;               // netcoredbg
            fs::path Player;                 // DimensionPlayer
            std::vector<std::string> PlayerArgs;
            fs::path WorkingDir;
            fs::path GateFile;               // the player waits while it exists (until the breakpoints are in)
        };
        bool Start(const Options& options);
        // Ends the game and the debugger.
        void Stop();
        void Update();
        State GetState() const { return m_State; }
        bool IsActive() const { return m_State == State::Starting || m_State == State::Running || m_State == State::Stopped; }
        const std::string& Error() const { return m_Error; }

        // Breakpoints (0-based lines) of a file: sent while a session runs, and at its start.
        void SetBreakpoints(const fs::path& file, const std::vector<int>& lines);
        // Whether the debugger found code at a breakpoint's line (false until it has said, or when it can't).
        bool IsVerified(const fs::path& file, int line) const;
        // Stop where any exception is thrown (kept for the next sessions too).
        void SetBreakOnExceptions(bool on);
        bool BreakOnExceptions() const { return m_BreakOnExceptions; }

        // ---- Stopped
        void Continue();
        void Pause();
        void StepOver();
        void StepIn();
        void StepOut();
        // Changes with each stop (to go to where it stopped once).
        uint64_t StopCount() const { return m_StopCount; }
        const std::string& StopReason() const { return m_StopReason; }
        const std::string& StopText() const { return m_StopText; } // (an exception's message)

        struct Frame
        {
            int Id = 0;
            std::string Name;
            fs::path File;       // empty: no source (framework or engine code without one)
            int Line = 0, Col = 0; // 0-based (the column in UTF-16 units)
        };
        const std::vector<Frame>& Frames() const { return m_Frames; }
        int SelectedFrame() const { return m_SelectedFrame; }
        void SelectFrame(int index);

        struct Variable
        {
            std::string Name, Value, Type, EvaluateName;
            int Ref = 0;         // > 0: has children
            bool Expanded = false, Loading = false, Loaded = false;
            std::vector<Variable> Children;
        };
        // The selected frame's scopes (Locals...), each a variable whose children are its variables.
        std::vector<Variable>& Scopes() { return m_Scopes; }
        // Children of an expandable variable (asked for once; they arrive in a later Update).
        void Expand(Variable& v);

        // An expression in the selected frame (watches, hover); `done` gets its value or the error (ok false).
        void Evaluate(const std::string& expression, const std::string& context,
                      std::function<void(bool ok, const std::string& value, const std::string& type, int ref)> done);

        // What the game and the debugger printed (newest last; older lines dropped past a limit).
        const std::vector<std::string>& Output() const { return m_Output; }
        uint64_t OutputCount() const { return m_OutputCount; }

        // DAP framing (exposed for tests): one message with its header; and taking complete messages off a buffer.
        static std::string FrameMessage(const std::string& json);
        static bool UnframeMessage(std::string& buffer, std::string& message);

    private:
        using Callback = std::function<void(const nlohmann::json& response)>;
        void Send(const std::string& command, const nlohmann::json& arguments, Callback done = {});
        void OnMessage(const nlohmann::json& message);
        void OnStopped(int thread);
        void SendBreakpoints(const std::string& key);
        void Fail(const std::string& error);
        void AddOutput(const std::string& text);
        void LoadScopes();
        void FinishStart();
        static std::string Key(const fs::path& file);
        static std::string Native(const fs::path& file);

        Options m_Options;
        State m_State = State::Idle;
        std::string m_Error;
        std::unique_ptr<ChildProcess> m_Debugger, m_Player;
        std::thread m_DebuggerReader, m_PlayerReader;
        std::mutex m_Mutex;
        std::vector<std::string> m_Incoming;  // debugger messages (JSON), from its reader thread
        std::string m_PlayerText;             // the player's output, from its reader thread
        bool m_DebuggerGone = false;

        int m_Seq = 0;
        std::map<int, Callback> m_Pending;
        int m_Thread = 0; // the thread stopped (or last stopped)
        uint64_t m_StopCount = 0;
        std::string m_StopReason, m_StopText;
        std::vector<Frame> m_Frames;
        int m_SelectedFrame = 0;
        std::vector<Variable> m_Scopes;
        uint64_t m_ScopesFor = 0; // (the stop and frame they were asked for: late answers dropped)

        struct FileBreakpoints
        {
            fs::path File;
            std::vector<int> Lines;
            std::vector<bool> Verified;
            std::vector<int> Ids;
        };
        std::map<std::string, FileBreakpoints> m_Breakpoints;
        bool m_Configured = false; // (breakpoints go in from configurationDone on)
        bool m_BreakOnExceptions = false;
        std::vector<std::string> m_Output;
        uint64_t m_OutputCount = 0;
        double m_StartTime = 0.0;
    };
}
