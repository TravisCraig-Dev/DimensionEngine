#include "Dimension/Platform/GamepadHaptics.h"

#ifdef __linux__
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <map>
#include <string>
#include <sys/ioctl.h>
#include <unistd.h>
#include <vector>

// Linux rumble through the kernel's force feedback interface: the gamepad's event device (the one GLFW reads it from)
// takes an FF_RUMBLE effect with a strong (heavy) and weak (light) magnitude. The xpad, hid-playstation (DualShock 4,
// DualSense), hid-nintendo and most other gamepad drivers implement it.
namespace Dimension::GamepadHaptics
{
    namespace
    {
        struct Device
        {
            int Fd = -1;
            int Effect = -1;
            bool Rumble = false;
            bool Playing = false;
        };
        std::map<std::string, Device> s_Devices; // by event node ("/dev/input/event5")

        bool TestBit(const unsigned long* bits, int bit)
        {
            constexpr int kBits = (int)(sizeof(unsigned long) * 8);
            return (bits[bit / kBits] >> (bit % kBits)) & 1ul;
        }

        // The ids in an SDL-style GUID as GLFW makes them on Linux: bus, vendor, product and version, little-endian
        // 16-bit fields at bytes 0, 4, 8 and 12. A zero vendor means the GUID holds the device name instead.
        bool ParseGuid(const std::string& guid, uint16_t& bus, uint16_t& vendor, uint16_t& product, uint16_t& version)
        {
            if (guid.size() < 32) return false;
            auto field = [&](int byte) {
                unsigned lo = 0, hi = 0;
                std::sscanf(guid.substr(byte * 2, 2).c_str(), "%2x", &lo);
                std::sscanf(guid.substr(byte * 2 + 2, 2).c_str(), "%2x", &hi);
                return (uint16_t)(lo | hi << 8);
            };
            bus = field(0), vendor = field(4), product = field(8), version = field(12);
            return vendor != 0;
        }

        std::vector<std::string> EventNodes()
        {
            std::vector<std::string> nodes;
            if (DIR* dir = opendir("/dev/input"))
            {
                while (dirent* e = readdir(dir))
                    if (std::strncmp(e->d_name, "event", 5) == 0) nodes.push_back(std::string("/dev/input/") + e->d_name);
                closedir(dir);
            }
            // event2 before event10.
            std::sort(nodes.begin(), nodes.end(), [](const std::string& a, const std::string& b) {
                return a.size() != b.size() ? a.size() < b.size() : a < b;
            });
            return nodes;
        }

        // The event node of the `ordinal`-th connected device with these ids (the order GLFW lists equal pads in).
        Device* Find(const std::string& guid, int ordinal)
        {
            uint16_t bus, vendor, product, version;
            if (!ParseGuid(guid, bus, vendor, product, version)) return nullptr;
            for (const std::string& node : EventNodes())
            {
                auto it = s_Devices.find(node);
                int fd = it != s_Devices.end() ? it->second.Fd : open(node.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
                if (fd < 0) continue; // no permission (not a gamepad, usually) or gone
                input_id id{};
                bool match = ioctl(fd, EVIOCGID, &id) == 0 && id.bustype == bus && id.vendor == vendor && id.product == product &&
                             id.version == version;
                if (match && ordinal-- == 0)
                {
                    Device& d = s_Devices[node];
                    if (d.Fd < 0)
                    {
                        d.Fd = fd;
                        unsigned long ff[(FF_CNT + sizeof(unsigned long) * 8 - 1) / (sizeof(unsigned long) * 8)] = {};
                        d.Rumble = ioctl(fd, EVIOCGBIT(EV_FF, sizeof(ff)), ff) >= 0 && TestBit(ff, FF_RUMBLE);
                    }
                    return &d;
                }
                if (it == s_Devices.end()) close(fd);
            }
            return nullptr;
        }

        void Forget(Device& d)
        {
            if (d.Fd >= 0) close(d.Fd);
            d = {};
        }
    }

    bool Set(int, float, float, float, float) { return false; } // (Windows.Gaming.Input only)
    int Count() { return 0; }

    bool SetDeviceRumble(const std::string& guid, int ordinal, float low, float high)
    {
        Device* d = Find(guid, ordinal);
        if (!d || !d->Rumble) return false;
        if (low <= 0.0f && high <= 0.0f)
        {
            if (d->Playing)
            {
                input_event stop{};
                stop.type = EV_FF;
                stop.code = (uint16_t)d->Effect;
                stop.value = 0;
                if (write(d->Fd, &stop, sizeof(stop)) < 0) Forget(*d);
                else d->Playing = false;
            }
            return true;
        }
        ff_effect effect{};
        effect.type = FF_RUMBLE;
        effect.id = (int16_t)d->Effect; // -1 uploads a new one; an id updates it in place
        effect.u.rumble.strong_magnitude = (uint16_t)std::lround(std::clamp(low, 0.0f, 1.0f) * 65535.0f);
        effect.u.rumble.weak_magnitude = (uint16_t)std::lround(std::clamp(high, 0.0f, 1.0f) * 65535.0f);
        effect.replay.length = 0; // until stopped (Input times it)
        if (ioctl(d->Fd, EVIOCSFF, &effect) < 0)
        {
            Forget(*d); // unplugged: found again when it returns
            return false;
        }
        d->Effect = effect.id;
        if (!d->Playing)
        {
            input_event play{};
            play.type = EV_FF;
            play.code = (uint16_t)d->Effect;
            play.value = 1;
            if (write(d->Fd, &play, sizeof(play)) < 0)
            {
                Forget(*d);
                return false;
            }
            d->Playing = true;
        }
        return true;
    }

    bool DeviceHasRumble(const std::string& guid, int ordinal)
    {
        Device* d = Find(guid, ordinal);
        return d && d->Rumble;
    }

    void Shutdown()
    {
        for (auto& [node, d] : s_Devices)
        {
            if (d.Fd < 0) continue;
            if (d.Effect >= 0) ioctl(d.Fd, EVIOCRMFF, d.Effect);
            close(d.Fd);
        }
        s_Devices.clear();
    }
}
#endif
