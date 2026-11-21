#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// The HID output reports that set the rumble motors of Sony's pads (Windows has no driver for it: GamepadHapticsWin32
// writes these to the pad). Kept apart and platform independent so their bytes are tested anywhere.
namespace Dimension::GamepadHaptics::Sony
{
    enum class Pad { DualShock4, DualSense };
    // Sony's vendor id with a DualShock 4 (both models, the wireless adapter) or DualSense (and Edge) product id.
    std::optional<Pad> Identify(uint16_t vendor, uint16_t product);
    // The pad an SDL-style joystick GUID (as GLFW makes them: vendor and product little-endian at bytes 4 and 8) names.
    std::optional<Pad> IdentifyGuid(const std::string& guid, uint16_t* vendor = nullptr, uint16_t* product = nullptr);
    // The report for these motor speeds (0..1: low = the big left motor, high = the small right one), its length the
    // pad's output report length: USB (DualShock 4: 32 bytes, report 0x05; DualSense: 48, report 0x02) or Bluetooth
    // (78 bytes, reports 0x11 / 0x31, ending in a CRC-32). Only the motors change: the light bar and the rest stay.
    // `sequence` (0..15) numbers DualSense Bluetooth reports.
    std::vector<uint8_t> RumbleReport(Pad pad, bool bluetooth, float low, float high, uint8_t sequence = 0);
    // Bluetooth reports' CRC-32 (the standard one, over a 0xA2 header byte and the report).
    uint32_t Crc32(const uint8_t* data, size_t size, uint32_t crc = 0);
}
