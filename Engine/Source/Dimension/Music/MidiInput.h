#pragma once
#include "Dimension/Core/Base.h"
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace Dimension::Music
{
    // A channel message from a MIDI device (note on/off, controllers, pitch bend...).
    struct MidiMessage
    {
        uint8_t Status = 0, Data1 = 0, Data2 = 0;
        int Kind() const { return Status & 0xF0; }
        int Channel() const { return Status & 0x0F; }
        // (a note on with velocity 0 is a note off, as keyboards send it)
        bool IsNoteOn() const { return Kind() == 0x90 && Data2 > 0; }
        bool IsNoteOff() const { return Kind() == 0x80 || (Kind() == 0x90 && Data2 == 0); }
        bool IsController() const { return Kind() == 0xB0; }
        float Velocity() const { return Data2 / 127.0f; }
    };

    // Turns a raw MIDI byte stream into channel messages: running status kept, real-time bytes (clock) and system
    // exclusive messages skipped.
    class MidiParser
    {
    public:
        void Feed(uint8_t byte, std::vector<MidiMessage>& out);

    private:
        uint8_t m_Status = 0, m_Data[2] = { 0, 0 };
        int m_Count = 0;
        bool m_SysEx = false;
    };

    // A MIDI keyboard (or other controller): one input device open at a time. Messages go to the handler on the
    // device's own thread, as they arrive (so notes reach the sound without waiting for a frame).
    class MidiInput
    {
    public:
        static std::vector<std::string> Devices();
        static bool Open(int device);
        static void Close();
        static bool IsOpen();
        static int OpenDevice(); // -1 when none
        using Handler = std::function<void(const MidiMessage&)>;
        static void SetHandler(Handler handler);
        // Delivers a message as if a device sent it (automation, tests).
        static void Inject(const MidiMessage& message);
    };
}
