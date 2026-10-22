#pragma once
// Controller Settings window: edit pad/keyboard -> Switch bindings with a
// live Joy-Con preview, and manage mapping profiles.

#include <functional>
#include <string>
#include <vector>

#include <windows.h>

#include "input_mapping.h"

namespace hikari {

struct ControllerSettingsDeps {
    // Optional initial client size in DIPs (0 keeps the standard layout).
    int initial_width = 0, initial_height = 0;
    std::function<RawPadState()> poll_pad;                   // latest raw state of the active physical controller (thread-safe, cheap)
    std::function<std::vector<std::string>()> list_devices;  // names of currently connected controllers (may be empty)
    std::function<void(const std::string&)> select_device;   // user picked a device in the dropdown (may be null)
};

// Modal window (owns its own message loop while open; parent disabled). Edits
// `mapping`, persists profiles into `cfg` (sections "mapping.<profile>", and
// "client"/"mapping_profile" = active profile name), calls cfg.save() on
// Save. Returns true if the user pressed Save (or saved/deleted a profile);
// `mapping` then holds the saved state, otherwise it is left unchanged.
// Must be called on a thread that pumps messages (the UI thread).
bool show_controller_settings(HWND parent, Config& cfg, InputMapping& mapping, const ControllerSettingsDeps& deps);

}  // namespace hikari
