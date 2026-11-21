#include "Dimension/Platform/GamepadHaptics.h"
#include "Dimension/Platform/SonyPadReports.h"

#ifdef _WIN32
#include <windows.h>
#include <hidsdi.h>
#include <setupapi.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Gaming.Input.h>
#include <algorithm>
#include <mutex>
#include <optional>
#include <vector>

#pragma comment(lib, "hid.lib")
#pragma comment(lib, "setupapi.lib")

namespace Dimension::GamepadHaptics
{
    namespace
    {
        namespace wgi = winrt::Windows::Gaming::Input;

        // Windows.Gaming.Input fills its gamepad list asynchronously in desktop apps (Gamepads() is empty until the
        // first GamepadAdded), so the list is kept from the events, which arrive on worker threads.
        struct State
        {
            std::mutex Mutex;
            std::vector<wgi::Gamepad> Pads;
            winrt::event_token Added{}, Removed{};
            bool Started = false, Failed = false;
        };
        State& S()
        {
            static State s;
            return s;
        }

        bool Start()
        {
            State& s = S();
            if (s.Started || s.Failed) return s.Started;
            try
            {
                // The main thread may already be in an apartment (C# scripting makes it MTA): keep whichever it is.
                try { winrt::init_apartment(winrt::apartment_type::multi_threaded); }
                catch (const winrt::hresult_error&) {}
                s.Added = wgi::Gamepad::GamepadAdded([](const auto&, const wgi::Gamepad& pad) {
                    std::lock_guard lock(S().Mutex);
                    for (const auto& p : S().Pads)
                        if (p == pad) return;
                    S().Pads.push_back(pad);
                });
                s.Removed = wgi::Gamepad::GamepadRemoved([](const auto&, const wgi::Gamepad& pad) {
                    std::lock_guard lock(S().Mutex);
                    std::erase(S().Pads, pad);
                });
                std::lock_guard lock(s.Mutex);
                for (const auto& pad : wgi::Gamepad::Gamepads())
                    if (std::find(s.Pads.begin(), s.Pads.end(), pad) == s.Pads.end()) s.Pads.push_back(pad);
                s.Started = true;
            }
            catch (...)
            {
                s.Failed = true; // no Windows.Gaming.Input (e.g. Windows Server without the media features)
            }
            return s.Started;
        }

        // Sony pads: Windows has no rumble driver for them, so the motors are set by writing their HID output
        // report to the pad's HID device. Devices are found by vendor/product id; the n-th pad with a GUID takes
        // the n-th HID device with those ids.
        struct SonyDevice
        {
            HANDLE Handle = INVALID_HANDLE_VALUE;
            uint16_t Vendor = 0, Product = 0;
            Sony::Pad Pad{};
            bool Bluetooth = false;
            size_t OutputLength = 0;
            uint8_t Sequence = 0;
        };
        std::vector<SonyDevice> s_Sony;
        bool s_SonyListed = false;

        void CloseSony()
        {
            for (SonyDevice& d : s_Sony)
                if (d.Handle != INVALID_HANDLE_VALUE) CloseHandle(d.Handle);
            s_Sony.clear();
            s_SonyListed = false;
        }

        void ListSony()
        {
            CloseSony();
            s_SonyListed = true;
            GUID hidGuid;
            HidD_GetHidGuid(&hidGuid);
            HDEVINFO set = SetupDiGetClassDevsW(&hidGuid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
            if (set == INVALID_HANDLE_VALUE) return;
            SP_DEVICE_INTERFACE_DATA iface{ sizeof(iface) };
            for (DWORD i = 0; SetupDiEnumDeviceInterfaces(set, nullptr, &hidGuid, i, &iface); ++i)
            {
                DWORD size = 0;
                SetupDiGetDeviceInterfaceDetailW(set, &iface, nullptr, 0, &size, nullptr);
                if (size == 0) continue;
                std::vector<uint8_t> buffer(size);
                auto* detail = (SP_DEVICE_INTERFACE_DETAIL_DATA_W*)buffer.data();
                detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
                if (!SetupDiGetDeviceInterfaceDetailW(set, &iface, detail, size, nullptr, nullptr)) continue;
                // No access rights first: reading the ids works on devices others hold (keyboards, mice).
                HANDLE probe = CreateFileW(detail->DevicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                           OPEN_EXISTING, 0, nullptr);
                if (probe == INVALID_HANDLE_VALUE) continue;
                HIDD_ATTRIBUTES attributes{ sizeof(attributes) };
                bool ok = HidD_GetAttributes(probe, &attributes);
                CloseHandle(probe);
                if (!ok) continue;
                auto pad = Sony::Identify(attributes.VendorID, attributes.ProductID);
                if (!pad) continue;
                HANDLE handle = CreateFileW(detail->DevicePath, GENERIC_READ | GENERIC_WRITE,
                                            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
                if (handle == INVALID_HANDLE_VALUE) continue; // (held exclusively, e.g. by a remapping tool)
                PHIDP_PREPARSED_DATA preparsed = nullptr;
                HIDP_CAPS caps{};
                bool gamepad = false;
                if (HidD_GetPreparsedData(handle, &preparsed))
                {
                    gamepad = HidP_GetCaps(preparsed, &caps) == HIDP_STATUS_SUCCESS && caps.UsagePage == 0x01 &&
                              caps.Usage == 0x05 && caps.OutputReportByteLength > 0;
                    HidD_FreePreparsedData(preparsed);
                }
                if (!gamepad)
                {
                    CloseHandle(handle);
                    continue;
                }
                SonyDevice d;
                d.Handle = handle;
                d.Vendor = attributes.VendorID;
                d.Product = attributes.ProductID;
                d.Pad = *pad;
                // Over USB both pads send 64-byte input reports; over Bluetooth their descriptors are larger.
                d.Bluetooth = caps.InputReportByteLength != 64;
                d.OutputLength = caps.OutputReportByteLength;
                s_Sony.push_back(d);
            }
            SetupDiDestroyDeviceInfoList(set);
        }

        // The ordinal-th listed device with the GUID's ids. A pad that is not listed lists the devices again, at
        // most once a second (or always with `force`, after a write failed).
        SonyDevice* FindSony(const std::string& guid, int ordinal, bool force = false)
        {
            static ULONGLONG s_ListedAt = 0;
            uint16_t vendor = 0, product = 0;
            if (ordinal < 0 || !Sony::IdentifyGuid(guid, &vendor, &product)) return nullptr;
            for (int pass = 0; pass < 2; ++pass)
            {
                int n = ordinal;
                if (s_SonyListed)
                    for (SonyDevice& d : s_Sony)
                        if (d.Vendor == vendor && d.Product == product && n-- == 0) return &d;
                if (pass == 1 || (s_SonyListed && !force && GetTickCount64() - s_ListedAt < 1000)) break;
                ListSony();
                s_ListedAt = GetTickCount64();
            }
            return nullptr;
        }

        bool WriteSony(SonyDevice& d, float low, float high)
        {
            std::vector<uint8_t> report = Sony::RumbleReport(d.Pad, d.Bluetooth, low, high, d.Sequence);
            d.Sequence = (uint8_t)((d.Sequence + 1) & 0x0F);
            // Interrupt writes take the descriptor's full output report length (the report first, zeros after).
            std::vector<uint8_t> padded = report;
            if (padded.size() < d.OutputLength) padded.resize(d.OutputLength, 0);
            DWORD written = 0;
            if (WriteFile(d.Handle, padded.data(), (DWORD)padded.size(), &written, nullptr)) return true;
            return HidD_SetOutputReport(d.Handle, report.data(), (ULONG)report.size()) != FALSE;
        }

        std::optional<wgi::Gamepad> At(int index)
        {
            if (index < 0 || !Start()) return std::nullopt;
            std::lock_guard lock(S().Mutex);
            if (index >= (int)S().Pads.size()) return std::nullopt;
            return S().Pads[index];
        }
    }

    bool Set(int index, float low, float high, float leftTrigger, float rightTrigger)
    {
        auto pad = At(index);
        if (!pad) return false;
        try
        {
            wgi::GamepadVibration v{};
            v.LeftMotor = low;
            v.RightMotor = high;
            v.LeftTrigger = leftTrigger;
            v.RightTrigger = rightTrigger;
            pad->Vibration(v);
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    int Count()
    {
        if (!Start()) return 0;
        std::lock_guard lock(S().Mutex);
        return (int)S().Pads.size();
    }

    void Shutdown()
    {
        CloseSony();
        State& s = S();
        if (!s.Started) return;
        try
        {
            wgi::Gamepad::GamepadAdded(s.Added);
            wgi::Gamepad::GamepadRemoved(s.Removed);
            std::lock_guard lock(s.Mutex);
            for (auto& pad : s.Pads) pad.Vibration(wgi::GamepadVibration{});
            s.Pads.clear();
        }
        catch (...) {}
        s.Started = false;
    }

    bool SetDeviceRumble(const std::string& guid, int ordinal, float low, float high)
    {
        if (SonyDevice* d = FindSony(guid, ordinal); d && WriteSony(*d, low, high)) return true;
        // Unplugged and plugged back in (a new HID device): list again and retry once.
        SonyDevice* d = FindSony(guid, ordinal, true);
        return d && WriteSony(*d, low, high);
    }

    bool DeviceHasRumble(const std::string& guid, int ordinal) { return FindSony(guid, ordinal) != nullptr; }
}
#elif !defined(__linux__) // (GamepadHapticsLinux.cpp)
namespace Dimension::GamepadHaptics
{
    bool Set(int, float, float, float, float) { return false; }
    int Count() { return 0; }
    bool SetDeviceRumble(const std::string&, int, float, float) { return false; }
    bool DeviceHasRumble(const std::string&, int) { return false; }
    void Shutdown() {}
}
#endif
