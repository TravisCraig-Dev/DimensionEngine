#include "Dimension/Music/MidiInput.h"
#include "Dimension/Core/Log.h"
#include <atomic>
#include <mutex>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")
#else
#include <fcntl.h>
#include <poll.h>
#include <thread>
#include <unistd.h>
#include <fstream>
#include <algorithm>
#endif

namespace Dimension::Music
{
    void MidiParser::Feed(uint8_t byte, std::vector<MidiMessage>& out)
    {
        if (byte >= 0xF8) return; // real-time (clock, start, stop, active sensing): may come anywhere
        if (byte == 0xF0)
        {
            m_SysEx = true;
            return;
        }
        if (byte == 0xF7)
        {
            m_SysEx = false;
            return;
        }
        if (m_SysEx) return;
        if (byte & 0x80)
        {
            // A status byte: system common messages (0xF1..0xF6) end running status and are not passed on.
            m_Status = byte < 0xF0 ? byte : 0;
            m_Count = 0;
            return;
        }
        if (!m_Status) return; // (data with no status)
        m_Data[m_Count++] = byte;
        int kind = m_Status & 0xF0;
        int needed = kind == 0xC0 || kind == 0xD0 ? 1 : 2; // program change and channel pressure have one data byte
        if (m_Count < needed) return;
        out.push_back({ m_Status, m_Data[0], needed == 2 ? m_Data[1] : (uint8_t)0 });
        m_Count = 0; // (running status: the next data bytes reuse it)
    }

    namespace
    {
        std::mutex s_HandlerMutex;
        MidiInput::Handler s_Handler;
        int s_OpenDevice = -1;

        void Deliver(const MidiMessage& m)
        {
            std::lock_guard lock(s_HandlerMutex);
            if (s_Handler) s_Handler(m);
        }

#ifdef _WIN32
        HMIDIIN s_In = nullptr;

        void CALLBACK OnMidi(HMIDIIN, UINT msg, DWORD_PTR, DWORD_PTR param1, DWORD_PTR)
        {
            if (msg != MIM_DATA) return;
            MidiMessage m{ (uint8_t)(param1 & 0xFF), (uint8_t)((param1 >> 8) & 0x7F), (uint8_t)((param1 >> 16) & 0x7F) };
            if (m.Status >= 0x80 && m.Status < 0xF0) Deliver(m);
        }
#else
        // Linux: the raw MIDI device files (/dev/snd/midiCxDy), read on a thread.
        std::thread s_Thread;
        std::atomic<bool> s_Running{ false };
        int s_Fd = -1;

        std::vector<fs::path> RawDevices()
        {
            std::vector<fs::path> out;
            std::error_code ec;
            for (const auto& e : fs::directory_iterator("/dev/snd", ec))
                if (e.path().filename().string().rfind("midiC", 0) == 0) out.push_back(e.path());
            std::sort(out.begin(), out.end());
            return out;
        }
#endif
    }

    std::vector<std::string> MidiInput::Devices()
    {
        std::vector<std::string> out;
#ifdef _WIN32
        UINT count = midiInGetNumDevs();
        for (UINT i = 0; i < count; ++i)
        {
            MIDIINCAPSW caps{};
            if (midiInGetDevCapsW(i, &caps, sizeof(caps)) != MMSYSERR_NOERROR) continue;
            char name[256] = {};
            WideCharToMultiByte(CP_UTF8, 0, caps.szPname, -1, name, sizeof(name), nullptr, nullptr);
            out.push_back(name);
        }
#else
        for (const fs::path& p : RawDevices())
        {
            // "midiC1D0": card 1's id names it.
            std::string file = p.filename().string(), card = file.substr(5, file.find('D') - 5), id;
            std::ifstream("/proc/asound/card" + card + "/id") >> id;
            out.push_back((id.empty() ? std::string("MIDI") : id) + " (" + file + ")");
        }
#endif
        return out;
    }

    bool MidiInput::Open(int device)
    {
        Close();
#ifdef _WIN32
        if (midiInOpen(&s_In, (UINT)device, (DWORD_PTR)&OnMidi, 0, CALLBACK_FUNCTION) != MMSYSERR_NOERROR)
        {
            s_In = nullptr;
            Log::Warn("MIDI: the input device would not open");
            return false;
        }
        midiInStart(s_In);
#else
        auto devices = RawDevices();
        if (device < 0 || device >= (int)devices.size()) return false;
        s_Fd = ::open(devices[device].c_str(), O_RDONLY | O_NONBLOCK);
        if (s_Fd < 0)
        {
            Log::Warn("MIDI: cannot open {}", devices[device].string());
            return false;
        }
        s_Running = true;
        s_Thread = std::thread([] {
            MidiParser parser;
            std::vector<MidiMessage> messages;
            uint8_t buffer[256];
            while (s_Running)
            {
                pollfd p{ s_Fd, POLLIN, 0 };
                if (::poll(&p, 1, 50) <= 0) continue;
                ssize_t n = ::read(s_Fd, buffer, sizeof(buffer));
                for (ssize_t i = 0; i < n; ++i) parser.Feed(buffer[i], messages);
                for (const auto& m : messages) Deliver(m);
                messages.clear();
            }
        });
#endif
        s_OpenDevice = device;
        Log::Info("MIDI input: {}", device < (int)Devices().size() ? Devices()[device] : std::string("device ") + std::to_string(device));
        return true;
    }

    void MidiInput::Close()
    {
#ifdef _WIN32
        if (s_In)
        {
            midiInStop(s_In);
            midiInReset(s_In);
            midiInClose(s_In);
            s_In = nullptr;
        }
#else
        if (s_Running)
        {
            s_Running = false;
            if (s_Thread.joinable()) s_Thread.join();
        }
        if (s_Fd >= 0) ::close(s_Fd), s_Fd = -1;
#endif
        s_OpenDevice = -1;
    }

    bool MidiInput::IsOpen() { return s_OpenDevice >= 0; }
    int MidiInput::OpenDevice() { return s_OpenDevice; }

    void MidiInput::SetHandler(Handler handler)
    {
        std::lock_guard lock(s_HandlerMutex);
        s_Handler = std::move(handler);
    }

    void MidiInput::Inject(const MidiMessage& message) { Deliver(message); }
}
