#include "Dimension/Platform/ChildProcess.h"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <csignal>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace Dimension
{
#ifdef _WIN32
    namespace
    {
        // One argument as CommandLineToArgvW reads it back.
        std::wstring Quote(const std::wstring& arg)
        {
            if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) return arg;
            std::wstring out = L"\"";
            for (size_t i = 0;; ++i)
            {
                size_t slashes = 0;
                while (i < arg.size() && arg[i] == L'\\') ++i, ++slashes;
                if (i == arg.size())
                {
                    out.append(slashes * 2, L'\\');
                    break;
                }
                if (arg[i] == L'"') out.append(slashes * 2 + 1, L'\\');
                else out.append(slashes, L'\\');
                out += arg[i];
            }
            return out + L"\"";
        }

        std::wstring Wide(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())).native(); }
    }

    ChildProcess::~ChildProcess()
    {
        CloseInput();
        if (m_Out) CloseHandle(m_Out);
        if (m_Process) CloseHandle(m_Process);
    }

    bool ChildProcess::Start(const fs::path& exe, const std::vector<std::string>& args, const fs::path& workingDir, bool pipes)
    {
        std::wstring line = Quote(exe.native());
        for (const std::string& a : args) line += L" " + Quote(Wide(a));
        SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
        HANDLE inRead = nullptr, inWrite = nullptr, outRead = nullptr, outWrite = nullptr;
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        if (pipes)
        {
            if (!CreatePipe(&inRead, &inWrite, &sa, 0) || !CreatePipe(&outRead, &outWrite, &sa, 0)) return false;
            SetHandleInformation(inWrite, HANDLE_FLAG_INHERIT, 0);
            SetHandleInformation(outRead, HANDLE_FLAG_INHERIT, 0);
            si.dwFlags = STARTF_USESTDHANDLES;
            si.hStdInput = inRead;
            si.hStdOutput = outWrite;
            si.hStdError = outWrite;
        }
        PROCESS_INFORMATION pi{};
        std::wstring dir = workingDir.native();
        BOOL ok = CreateProcessW(nullptr, line.data(), nullptr, nullptr, pipes ? TRUE : FALSE, CREATE_NO_WINDOW, nullptr,
                                 dir.empty() ? nullptr : dir.c_str(), &si, &pi);
        if (pipes)
        {
            CloseHandle(inRead);
            CloseHandle(outWrite);
        }
        if (!ok)
        {
            if (inWrite) CloseHandle(inWrite);
            if (outRead) CloseHandle(outRead);
            return false;
        }
        CloseHandle(pi.hThread);
        m_Process = pi.hProcess;
        m_Pid = pi.dwProcessId;
        m_In = inWrite;
        m_Out = outRead;
        return true;
    }

    bool ChildProcess::IsRunning()
    {
        if (!m_Process) return false;
        if (WaitForSingleObject(m_Process, 0) == WAIT_TIMEOUT) return true;
        DWORD code = 0;
        GetExitCodeProcess(m_Process, &code);
        m_ExitCode = (int)code;
        return false;
    }

    int ChildProcess::ExitCode()
    {
        IsRunning();
        return m_ExitCode;
    }

    bool ChildProcess::Write(std::string_view data)
    {
        if (!m_In) return false;
        while (!data.empty())
        {
            DWORD written = 0;
            if (!WriteFile(m_In, data.data(), (DWORD)data.size(), &written, nullptr)) return false;
            data.remove_prefix(written);
        }
        return true;
    }

    void ChildProcess::CloseInput()
    {
        if (m_In) CloseHandle(m_In);
        m_In = nullptr;
    }

    int ChildProcess::Read(char* buffer, int size)
    {
        if (!m_Out) return 0;
        DWORD read = 0;
        if (!ReadFile(m_Out, buffer, (DWORD)size, &read, nullptr)) return 0;
        return (int)read;
    }

    void ChildProcess::Kill()
    {
        if (m_Process && IsRunning()) TerminateProcess(m_Process, 1);
    }
#else
    ChildProcess::~ChildProcess()
    {
        CloseInput();
        if (m_Out >= 0) close(m_Out);
        if (m_Pid) IsRunning(); // (reaps it when it has ended)
    }

    bool ChildProcess::Start(const fs::path& exe, const std::vector<std::string>& args, const fs::path& workingDir, bool pipes)
    {
        int in[2] = { -1, -1 }, out[2] = { -1, -1 };
        if (pipes && (pipe(in) != 0 || pipe(out) != 0)) return false;
        pid_t pid = fork();
        if (pid < 0) return false;
        if (pid == 0)
        {
            if (pipes)
            {
                dup2(in[0], 0);
                dup2(out[1], 1);
                dup2(out[1], 2);
                close(in[0]), close(in[1]), close(out[0]), close(out[1]);
            }
            else
            {
                int null = open("/dev/null", O_RDWR);
                if (null >= 0) dup2(null, 0), dup2(null, 1), dup2(null, 2);
            }
            if (!workingDir.empty() && chdir(workingDir.c_str()) != 0) _exit(127);
            std::vector<char*> argv;
            std::string program = exe.string();
            argv.push_back(program.data());
            std::vector<std::string> copies = args;
            for (std::string& a : copies) argv.push_back(a.data());
            argv.push_back(nullptr);
            execv(program.c_str(), argv.data());
            _exit(127);
        }
        if (pipes)
        {
            close(in[0]);
            close(out[1]);
            m_In = in[1];
            m_Out = out[0];
            signal(SIGPIPE, SIG_IGN); // (writing to one that ended fails instead of ending us)
        }
        m_Pid = (uint32_t)pid;
        return true;
    }

    bool ChildProcess::IsRunning()
    {
        if (!m_Pid || m_ExitCode != -1) return false;
        int status = 0;
        pid_t r = waitpid((pid_t)m_Pid, &status, WNOHANG);
        if (r == 0) return true;
        m_ExitCode = r < 0 ? 0 : WIFEXITED(status) ? WEXITSTATUS(status) : 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
        return false;
    }

    int ChildProcess::ExitCode()
    {
        IsRunning();
        return m_ExitCode;
    }

    bool ChildProcess::Write(std::string_view data)
    {
        while (!data.empty())
        {
            if (m_In < 0) return false;
            ssize_t n = write(m_In, data.data(), data.size());
            if (n <= 0) return false;
            data.remove_prefix((size_t)n);
        }
        return true;
    }

    void ChildProcess::CloseInput()
    {
        if (m_In >= 0) close(m_In);
        m_In = -1;
    }

    int ChildProcess::Read(char* buffer, int size)
    {
        if (m_Out < 0) return 0;
        ssize_t n = read(m_Out, buffer, (size_t)size);
        return n > 0 ? (int)n : 0;
    }

    void ChildProcess::Kill()
    {
        if (m_Pid && IsRunning()) kill((pid_t)m_Pid, SIGKILL);
    }
#endif
}
