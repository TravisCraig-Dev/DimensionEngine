#include "Dimension/Platform/SonyPadReports.h"
#include <algorithm>
#include <cmath>

namespace Dimension::GamepadHaptics::Sony
{
    std::optional<Pad> Identify(uint16_t vendor, uint16_t product)
    {
        if (vendor != 0x054C) return std::nullopt;
        switch (product)
        {
        case 0x05C4: // DualShock 4 (first model)
        case 0x09CC: // DualShock 4 (second model)
        case 0x0BA0: // DualShock 4 USB wireless adapter
            return Pad::DualShock4;
        case 0x0CE6: // DualSense
        case 0x0DF2: // DualSense Edge
            return Pad::DualSense;
        default: return std::nullopt;
        }
    }

    std::optional<Pad> IdentifyGuid(const std::string& guid, uint16_t* vendor, uint16_t* product)
    {
        if (guid.size() < 32) return std::nullopt;
        auto byte = [&](size_t i) -> int {
            int v = 0;
            for (size_t k = i * 2; k < i * 2 + 2; ++k)
            {
                char ch = guid[k];
                int d = ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10 : -1;
                if (d < 0) return -1;
                v = v * 16 + d;
            }
            return v;
        };
        int v0 = byte(4), v1 = byte(5), p0 = byte(8), p1 = byte(9);
        if (v0 < 0 || v1 < 0 || p0 < 0 || p1 < 0) return std::nullopt;
        uint16_t v = (uint16_t)(v0 | v1 << 8), p = (uint16_t)(p0 | p1 << 8);
        if (vendor) *vendor = v;
        if (product) *product = p;
        return Identify(v, p);
    }

    uint32_t Crc32(const uint8_t* data, size_t size, uint32_t crc)
    {
        crc = ~crc;
        for (size_t i = 0; i < size; ++i)
        {
            crc ^= data[i];
            for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
        return ~crc;
    }

    std::vector<uint8_t> RumbleReport(Pad pad, bool bluetooth, float low, float high, uint8_t sequence)
    {
        uint8_t big = (uint8_t)std::lround(std::clamp(low, 0.0f, 1.0f) * 255.0f);
        uint8_t small = (uint8_t)std::lround(std::clamp(high, 0.0f, 1.0f) * 255.0f);
        std::vector<uint8_t> r;
        if (pad == Pad::DualShock4)
        {
            // Report 0x05 (USB) / 0x11 (Bluetooth, two bytes more at the front): flags (0x01 = the motors), then
            // the small (right) and big (left) motor.
            r.assign(bluetooth ? 78 : 32, 0);
            size_t at = 1;
            if (bluetooth)
            {
                r[0] = 0x11;
                r[1] = 0xC4; // (a HID report with a CRC; 4 ms between input reports)
                at = 3;
            }
            else
                r[0] = 0x05;
            r[at] = 0x01;
            r[at + 3] = small;
            r[at + 4] = big;
        }
        else
        {
            // Report 0x02 (USB) / 0x31 (Bluetooth: a sequence number in the high nibble, then the tag 0x10): enable
            // bits (0x01 rumble in its compatible mode, 0x02 its haptics in place of audio haptics), then the right
            // (small) and left (big) motor.
            r.assign(bluetooth ? 78 : 48, 0);
            size_t at = 1;
            if (bluetooth)
            {
                r[0] = 0x31;
                r[1] = (uint8_t)((sequence & 0x0F) << 4);
                r[2] = 0x10;
                at = 3;
            }
            else
                r[0] = 0x02;
            r[at] = 0x01 | 0x02;
            r[at + 2] = small;
            r[at + 3] = big;
        }
        if (bluetooth)
        {
            // CRC-32 over 0xA2 (the output report's header byte) and all but its own last four bytes, little-endian.
            uint8_t header = 0xA2;
            uint32_t crc = Crc32(&header, 1);
            crc = Crc32(r.data(), r.size() - 4, crc);
            for (int i = 0; i < 4; ++i) r[r.size() - 4 + i] = (uint8_t)(crc >> (8 * i));
        }
        return r;
    }
}
