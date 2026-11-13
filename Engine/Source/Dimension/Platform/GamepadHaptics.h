#pragma once
#include <string>

namespace Dimension::GamepadHaptics
{
    // Windows.Gaming.Input gamepads (Xbox One / Series controllers and other XInput pads): their four motors,
    // including the impulse triggers that XInput cannot reach. `index` is the controller's rank among the connected
    // XInput controllers (the order GLFW lists them in). Returns false when there is no such gamepad (the caller falls
    // back to XInput). Elsewhere these do nothing.
    bool Set(int index, float low, float high, float leftTrigger, float rightTrigger);
    // How many gamepads Windows.Gaming.Input sees (0 before its first arrival event, or off Windows).
    int Count();
    // Linux: the rumble motors of the `ordinal`-th connected pad with this (GLFW / SDL) GUID, through the kernel's
    // force feedback (FF_RUMBLE). False when it has none or cannot be opened (elsewhere: always).
    bool SetDeviceRumble(const std::string& guid, int ordinal, float low, float high);
    bool DeviceHasRumble(const std::string& guid, int ordinal);
    // Stops every motor and drops the gamepads (before the window closes).
    void Shutdown();
}
