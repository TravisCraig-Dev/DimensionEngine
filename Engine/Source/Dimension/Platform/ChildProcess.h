#pragma once
#include "Dimension/Core/Base.h"
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace Dimension
{
    // A program started by the engine, optionally with its standard input and output as pipes (stderr goes with
    // stdout): the C# debugger (netcoredbg) the editor talks to, the player it debugs. No console window opens.
    class ChildProcess
    {
    public:
        ChildProcess() = default;
        ~ChildProcess(); // (closes the pipes; the program keeps running unless Kill was called)
        ChildProcess(const ChildProcess&) = delete;
        ChildProcess& operator=(const ChildProcess&) = delete;

        bool Start(const fs::path& exe, const std::vector<std::string>& args, const fs::path& workingDir = {}, bool pipes = true);
        bool IsRunning();
        int ExitCode(); // (-1 while running)
        uint32_t Pid() const { return m_Pid; }

        // Its standard input (false once it is gone).
        bool Write(std::string_view data);
        void CloseInput();
        // Its standard output: blocks until something comes; 0 at the end (it exited or closed it).
        int Read(char* buffer, int size);
        void Kill();

    private:
        uint32_t m_Pid = 0;
        int m_ExitCode = -1;
#ifdef _WIN32
        void* m_Process = nullptr;
        void* m_In = nullptr;
        void* m_Out = nullptr;
#else
        int m_In = -1, m_Out = -1;
#endif
    };
}
