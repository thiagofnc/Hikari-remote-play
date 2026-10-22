#pragma once
// Making the host PC dependably reachable: Windows Firewall, auto-start and
// "don't sleep when the lid closes".
//
// Changing the firewall (and, on some PCs, the power plan) needs
// administrator rights. Hikari never runs elevated itself: it relaunches
// itself once with `--elevated-setup=<tasks>` behind a single UAC prompt.

#include <cstdint>
#include <string>

namespace hikari {

struct FirewallStatus {
    bool checked = false;     // the firewall could be queried
    bool enabled = false;     // firewall on for at least one active profile
    bool allowed = false;     // our inbound allow rule exists and is enabled
    bool blocked = false;     // an inbound block rule targets Hikari.exe
    bool ok() const { return !checked || !enabled || (allowed && !blocked); }
};

FirewallStatus firewall_status(uint16_t port);
// Adds the inbound UDP rule and removes block rules created when someone
// dismissed Windows' "Allow access?" prompt. Requires elevation.
bool configure_firewall(uint16_t port);

// Lid close action on AC power: -1 = no lid, 0 = do nothing, 1 = sleep,
// 2 = hibernate, 3 = shut down.
int lid_close_action();
// Sets "do nothing" (on AC and battery). Tries without elevation first.
bool keep_awake_on_lid_close(bool allow_elevation_fallback);

bool autostart_enabled();
bool set_autostart(bool on);  // HKCU Run key: Hikari.exe --host --minimized

// Relaunches Hikari.exe elevated with `--elevated-setup=<tasks>` (comma list:
// "firewall", "lid") and waits. Returns false if the user declined UAC.
bool run_elevated_setup(const std::string& tasks, uint16_t port);
// Entry point for the elevated instance. Returns the process exit code.
int elevated_setup_main(const std::string& tasks, uint16_t port);

}  // namespace hikari
