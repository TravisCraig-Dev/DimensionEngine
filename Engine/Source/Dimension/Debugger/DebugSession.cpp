#include "Dimension/Debugger/DebugSession.h"
#include "Dimension/Core/Log.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <fstream>

using nlohmann::json;

namespace Dimension
{
    namespace
    {
        std::string Utf8(const fs::path& p)
        {
            std::u8string s = p.u8string();
            return std::string(s.begin(), s.end());
        }
        fs::path FromUtf8(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }

        double Now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

        // A line of the player's log without its colour codes.
        std::string Plain(const std::string& line)
        {
            std::string out;
            for (size_t i = 0; i < line.size(); ++i)
            {
                if (line[i] == '\x1b' && i + 1 < line.size() && line[i + 1] == '[')
                {
                    i += 2;
                    while (i < line.size() && !std::isalpha((unsigned char)line[i])) ++i;
                    continue;
                }
                if (line[i] != '\r') out += line[i];
            }
            return out;
        }
    }

    DebugSession::~DebugSession()
    {
        Stop();
        if (m_DebuggerReader.joinable()) m_DebuggerReader.join();
        if (m_PlayerReader.joinable()) m_PlayerReader.join();
    }

    std::string DebugSession::Key(const fs::path& file)
    {
        std::error_code ec;
        fs::path abs = fs::absolute(file, ec);
        std::string key = Utf8((ec ? file : abs).lexically_normal().generic_u8string());
        std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        return key;
    }

    // (the debugger matches sources by the paths in the PDBs: Windows' with backslashes)
    std::string DebugSession::Native(const fs::path& file)
    {
        std::error_code ec;
        fs::path abs = fs::absolute(file, ec);
        return Utf8((ec ? file : abs).lexically_normal().make_preferred());
    }

    std::string DebugSession::FrameMessage(const std::string& body) { return "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body; }

    bool DebugSession::UnframeMessage(std::string& buffer, std::string& message)
    {
        size_t end = buffer.find("\r\n\r\n");
        if (end == std::string::npos) return false;
        size_t at = buffer.find("Content-Length:");
        if (at == std::string::npos || at > end)
        {
            buffer.erase(0, end + 4); // (a header without a length: skipped)
            return false;
        }
        size_t length = (size_t)std::strtoull(buffer.c_str() + at + 15, nullptr, 10);
        if (buffer.size() < end + 4 + length) return false;
        message = buffer.substr(end + 4, length);
        buffer.erase(0, end + 4 + length);
        return true;
    }

    // ---- Starting and ending

    bool DebugSession::Start(const Options& options)
    {
        if (IsActive()) return false;
        if (m_DebuggerReader.joinable()) m_DebuggerReader.join();
        if (m_PlayerReader.joinable()) m_PlayerReader.join();
        m_Options = options;
        m_Error.clear();
        m_Pending.clear();
        m_Frames.clear();
        m_Scopes.clear();
        m_Configured = false;
        m_DebuggerGone = false;
        m_Incoming.clear();
        m_PlayerText.clear();
        for (auto& [key, b] : m_Breakpoints)
        {
            b.Verified.assign(b.Lines.size(), false);
            b.Ids.assign(b.Lines.size(), -1);
        }
        std::error_code ec;
        if (!fs::exists(options.Debugger, ec))
        {
            Fail("The C# debugger (netcoredbg) is missing: " + Utf8(options.Debugger));
            return false;
        }
        if (!options.GateFile.empty()) std::ofstream(options.GateFile) << "the player waits while this file exists\n";

        // The game, waiting for the debugger.
        m_Player = std::make_unique<ChildProcess>();
        if (!m_Player->Start(options.Player, options.PlayerArgs, options.WorkingDir, true))
        {
            Fail("The player could not be started: " + Utf8(options.Player));
            return false;
        }
        m_PlayerReader = std::thread([this, process = m_Player.get()] {
            char buffer[4096];
            for (int n; (n = process->Read(buffer, sizeof(buffer))) > 0;)
            {
                std::lock_guard lock(m_Mutex);
                m_PlayerText.append(buffer, (size_t)n);
            }
        });

        // The debugger, attaching to it.
        m_Debugger = std::make_unique<ChildProcess>();
        if (!m_Debugger->Start(options.Debugger, { "--interpreter=vscode" }, options.Debugger.parent_path(), true))
        {
            Fail("The C# debugger could not be started");
            m_Player->Kill();
            return false;
        }
        m_DebuggerReader = std::thread([this, process = m_Debugger.get()] {
            std::string buffer, message;
            char chunk[8192];
            for (int n; (n = process->Read(chunk, sizeof(chunk))) > 0;)
            {
                buffer.append(chunk, (size_t)n);
                while (UnframeMessage(buffer, message))
                {
                    std::lock_guard lock(m_Mutex);
                    m_Incoming.push_back(std::move(message));
                }
            }
            std::lock_guard lock(m_Mutex);
            m_DebuggerGone = true;
        });
        m_State = State::Starting;
        m_StartTime = Now();
        AddOutput("Debugging: the game starts (process " + std::to_string(m_Player->Pid()) + ")");
        Send("initialize", { { "clientID", "dimension" }, { "clientName", "Dimension Editor" }, { "adapterID", "coreclr" }, { "linesStartAt1", true },
                             { "columnsStartAt1", true }, { "pathFormat", "path" }, { "supportsVariableType", true } },
             [this](const json& r) {
                 if (!r.value("success", false)) return Fail("The debugger did not start: " + r.value("message", std::string()));
                 Send("attach", { { "processId", (int)m_Player->Pid() } }, [this](const json& a) {
                     if (!a.value("success", false)) Fail("The debugger could not attach to the game: " + a.value("message", std::string()));
                 });
             });
        return true;
    }

    // After "initialized": breakpoints in, then the game goes on.
    void DebugSession::FinishStart()
    {
        for (auto& [key, b] : m_Breakpoints) SendBreakpoints(key);
        Send("setExceptionBreakpoints", { { "filters", m_BreakOnExceptions ? json::array({ "all" }) : json::array() } });
        Send("configurationDone", json::object(), [this](const json&) {
            m_Configured = true;
            if (m_State == State::Starting) m_State = State::Running;
            std::error_code ec;
            if (!m_Options.GateFile.empty()) fs::remove(m_Options.GateFile, ec); // (the player goes on)
        });
    }

    void DebugSession::Stop()
    {
        if (m_Debugger && IsActive()) Send("disconnect", { { "terminateDebuggee", true } });
        if (m_Player) m_Player->Kill();
        if (m_Debugger)
        {
            // (a moment for the disconnect, then it ends either way)
            for (int i = 0; i < 20 && m_Debugger->IsRunning(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
            m_Debugger->Kill();
            m_Debugger->CloseInput();
        }
        std::error_code ec;
        if (!m_Options.GateFile.empty()) fs::remove(m_Options.GateFile, ec);
        if (IsActive()) AddOutput("Debugging stopped");
        if (IsActive()) m_State = State::Ended;
        m_Frames.clear();
        m_Scopes.clear();
        m_Pending.clear();
    }

    void DebugSession::Fail(const std::string& error)
    {
        m_Error = error;
        AddOutput(error);
        Log::Error("C# debugger: {}", error);
        if (m_Player) m_Player->Kill();
        if (m_Debugger) m_Debugger->Kill();
        m_State = State::Ended;
    }

    void DebugSession::AddOutput(const std::string& text)
    {
        size_t start = 0;
        while (start <= text.size())
        {
            size_t nl = text.find('\n', start);
            std::string line = Plain(text.substr(start, nl == std::string::npos ? std::string::npos : nl - start));
            if (!line.empty() || nl != std::string::npos)
            {
                m_Output.push_back(line);
                ++m_OutputCount;
            }
            if (nl == std::string::npos) break;
            start = nl + 1;
        }
        if (m_Output.size() > 5000) m_Output.erase(m_Output.begin(), m_Output.begin() + (long long)(m_Output.size() - 4000));
    }

    // ---- Messages

    void DebugSession::Send(const std::string& command, const json& arguments, Callback done)
    {
        if (!m_Debugger) return;
        int seq = ++m_Seq;
        json message = { { "seq", seq }, { "type", "request" }, { "command", command }, { "arguments", arguments } };
        if (done) m_Pending[seq] = std::move(done);
        m_Debugger->Write(FrameMessage(message.dump(-1, ' ', false, json::error_handler_t::replace)));
    }

    void DebugSession::Update()
    {
        std::vector<std::string> incoming;
        std::string playerText;
        bool gone = false;
        {
            std::lock_guard lock(m_Mutex);
            incoming.swap(m_Incoming);
            // (whole lines of the player's output)
            size_t nl = m_PlayerText.rfind('\n');
            if (nl != std::string::npos)
            {
                playerText = m_PlayerText.substr(0, nl);
                m_PlayerText.erase(0, nl + 1);
            }
            gone = m_DebuggerGone;
        }
        if (!playerText.empty()) AddOutput(playerText);
        for (const std::string& text : incoming)
        {
            json message = json::parse(text, nullptr, false);
            if (message.is_object()) OnMessage(message);
        }
        if (!IsActive()) return;
        // The game or the debugger ended by itself.
        if (m_Player && !m_Player->IsRunning())
        {
            AddOutput("The game ended (exit code " + std::to_string(m_Player->ExitCode()) + ")");
            Stop();
            m_State = State::Ended;
        }
        else if (gone)
        {
            Fail("The debugger ended unexpectedly");
        }
        else if (m_State == State::Starting && Now() - m_StartTime > 30.0)
        {
            Fail("The debugger did not attach to the game in time");
        }
    }

    void DebugSession::OnMessage(const json& message)
    {
        std::string type = message.value("type", std::string());
        if (type == "response")
        {
            int seq = message.value("request_seq", 0);
            auto it = m_Pending.find(seq);
            if (it == m_Pending.end()) return;
            Callback done = std::move(it->second);
            m_Pending.erase(it);
            done(message);
            return;
        }
        if (type != "event") return;
        std::string event = message.value("event", std::string());
        const json body = message.contains("body") ? message["body"] : json::object();
        if (event == "initialized") FinishStart();
        else if (event == "stopped")
        {
            m_StopReason = body.value("reason", std::string());
            m_StopText = body.value("text", std::string());
            OnStopped(body.value("threadId", m_Thread));
        }
        else if (event == "continued")
        {
            if (m_State == State::Stopped) m_State = State::Running;
            m_Frames.clear();
            m_Scopes.clear();
        }
        else if (event == "exited" || event == "terminated")
        {
            if (IsActive())
            {
                AddOutput("The game ended");
                Stop();
                m_State = State::Ended;
            }
        }
        else if (event == "output")
        {
            std::string category = body.value("category", std::string());
            if (category != "telemetry") AddOutput(body.value("output", std::string()));
        }
        else if (event == "breakpoint")
        {
            const json bp = body.value("breakpoint", json::object());
            int id = bp.value("id", -1);
            for (auto& [key, b] : m_Breakpoints)
                for (size_t i = 0; i < b.Ids.size(); ++i)
                    if (b.Ids[i] == id) b.Verified[i] = bp.value("verified", false);
        }
    }

    void DebugSession::OnStopped(int thread)
    {
        m_State = State::Stopped;
        m_Thread = thread;
        m_Frames.clear();
        m_Scopes.clear();
        m_SelectedFrame = 0;
        uint64_t stop = ++m_StopCount;
        Send("stackTrace", { { "threadId", thread }, { "startFrame", 0 }, { "levels", 64 } }, [this, stop](const json& r) {
            if (stop != m_StopCount || !r.value("success", false)) return;
            m_Frames.clear();
            for (const auto& f : r["body"].value("stackFrames", json::array()))
            {
                Frame frame;
                frame.Id = f.value("id", 0);
                frame.Name = f.value("name", std::string());
                if (f.contains("source") && f["source"].is_object()) frame.File = FromUtf8(f["source"].value("path", std::string()));
                frame.Line = std::max(0, f.value("line", 1) - 1);
                frame.Col = std::max(0, f.value("column", 1) - 1);
                m_Frames.push_back(std::move(frame));
            }
            // The first frame with source (the script, not the engine code around it).
            m_SelectedFrame = 0;
            for (size_t i = 0; i < m_Frames.size(); ++i)
                if (!m_Frames[i].File.empty())
                {
                    m_SelectedFrame = (int)i;
                    break;
                }
            ++m_StopCount; // (the frames are in: the UI goes to the place now)
            LoadScopes();
        });
    }

    void DebugSession::SelectFrame(int index)
    {
        if (index < 0 || index >= (int)m_Frames.size()) return;
        m_SelectedFrame = index;
        LoadScopes();
    }

    void DebugSession::LoadScopes()
    {
        m_Scopes.clear();
        if (m_Frames.empty()) return;
        uint64_t asked = ++m_ScopesFor;
        Send("scopes", { { "frameId", m_Frames[(size_t)m_SelectedFrame].Id } }, [this, asked](const json& r) {
            if (asked != m_ScopesFor || !r.value("success", false)) return;
            m_Scopes.clear();
            for (const auto& s : r["body"].value("scopes", json::array()))
            {
                Variable v;
                v.Name = s.value("name", std::string());
                v.Ref = s.value("variablesReference", 0);
                v.Expanded = true;
                m_Scopes.push_back(std::move(v));
            }
            for (auto& s : m_Scopes) Expand(s);
        });
    }

    void DebugSession::Expand(Variable& v)
    {
        v.Expanded = true;
        if (v.Ref <= 0 || v.Loaded || v.Loading) return;
        v.Loading = true;
        int ref = v.Ref;
        uint64_t asked = m_ScopesFor;
        Send("variables", { { "variablesReference", ref } }, [this, ref, asked](const json& r) {
            if (asked != m_ScopesFor) return;
            // (found again by its reference: the tree may have changed since)
            std::function<Variable*(std::vector<Variable>&)> find = [&](std::vector<Variable>& list) -> Variable* {
                for (auto& x : list)
                {
                    if (x.Ref == ref && x.Loading) return &x;
                    if (Variable* c = find(x.Children)) return c;
                }
                return nullptr;
            };
            Variable* target = find(m_Scopes);
            if (!target) return;
            target->Loading = false;
            target->Loaded = true;
            target->Children.clear();
            if (!r.value("success", false)) return;
            for (const auto& x : r["body"].value("variables", json::array()))
            {
                Variable c;
                c.Name = x.value("name", std::string());
                c.Value = x.value("value", std::string());
                c.Type = x.value("type", std::string());
                c.EvaluateName = x.value("evaluateName", std::string());
                c.Ref = x.value("variablesReference", 0);
                target->Children.push_back(std::move(c));
            }
        });
    }

    void DebugSession::Evaluate(const std::string& expression, const std::string& context,
                                std::function<void(bool, const std::string&, const std::string&, int)> done)
    {
        if (m_State != State::Stopped || m_Frames.empty())
        {
            if (done) done(false, "(not stopped)", "", 0);
            return;
        }
        Send("evaluate", { { "expression", expression }, { "frameId", m_Frames[(size_t)m_SelectedFrame].Id }, { "context", context } },
             [done](const json& r) {
                 if (!done) return;
                 if (!r.value("success", false))
                 {
                     done(false, r.value("message", std::string("error")), "", 0);
                     return;
                 }
                 const json body = r.value("body", json::object());
                 done(true, body.value("result", std::string()), body.value("type", std::string()), body.value("variablesReference", 0));
             });
    }

    // ---- Breakpoints

    void DebugSession::SetBreakpoints(const fs::path& file, const std::vector<int>& lines)
    {
        std::string key = Key(file);
        FileBreakpoints& b = m_Breakpoints[key];
        std::vector<int> sorted = lines;
        std::sort(sorted.begin(), sorted.end());
        sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
        if (b.Lines == sorted && !b.File.empty()) return;
        b.File = file;
        b.Lines = sorted;
        b.Verified.assign(sorted.size(), false);
        b.Ids.assign(sorted.size(), -1);
        if (m_Configured && IsActive()) SendBreakpoints(key);
    }

    void DebugSession::SendBreakpoints(const std::string& key)
    {
        auto it = m_Breakpoints.find(key);
        if (it == m_Breakpoints.end()) return;
        json list = json::array();
        for (int line : it->second.Lines) list.push_back({ { "line", line + 1 } });
        std::vector<int> lines = it->second.Lines;
        Send("setBreakpoints", { { "source", { { "path", Native(it->second.File) } } }, { "breakpoints", list } }, [this, key, lines](const json& r) {
            auto f = m_Breakpoints.find(key);
            if (f == m_Breakpoints.end() || f->second.Lines != lines || !r.value("success", false)) return;
            const json answers = r["body"].value("breakpoints", json::array());
            for (size_t i = 0; i < answers.size() && i < f->second.Lines.size(); ++i)
            {
                f->second.Ids[i] = answers[i].value("id", -1);
                f->second.Verified[i] = answers[i].value("verified", false);
            }
        });
    }

    bool DebugSession::IsVerified(const fs::path& file, int line) const
    {
        auto it = m_Breakpoints.find(Key(file));
        if (it == m_Breakpoints.end()) return false;
        for (size_t i = 0; i < it->second.Lines.size(); ++i)
            if (it->second.Lines[i] == line) return it->second.Verified[i];
        return false;
    }

    void DebugSession::SetBreakOnExceptions(bool on)
    {
        m_BreakOnExceptions = on;
        if (m_Configured && IsActive()) Send("setExceptionBreakpoints", { { "filters", on ? json::array({ "all" }) : json::array() } });
    }

    // ---- Running

    void DebugSession::Continue()
    {
        if (m_State != State::Stopped) return;
        Send("continue", { { "threadId", m_Thread } });
        m_State = State::Running;
        m_Frames.clear();
        m_Scopes.clear();
    }

    void DebugSession::Pause()
    {
        if (m_State != State::Running) return;
        // (any thread: the debugger stops them all)
        Send("threads", json::object(), [this](const json& r) {
            const json threads = r.value("body", json::object()).value("threads", json::array());
            int thread = threads.empty() ? m_Thread : threads[0].value("id", m_Thread);
            Send("pause", { { "threadId", thread } });
        });
    }

    void DebugSession::StepOver()
    {
        if (m_State != State::Stopped) return;
        Send("next", { { "threadId", m_Thread } });
        m_State = State::Running;
    }

    void DebugSession::StepIn()
    {
        if (m_State != State::Stopped) return;
        Send("stepIn", { { "threadId", m_Thread } });
        m_State = State::Running;
    }

    void DebugSession::StepOut()
    {
        if (m_State != State::Stopped) return;
        Send("stepOut", { { "threadId", m_Thread } });
        m_State = State::Running;
    }
}
