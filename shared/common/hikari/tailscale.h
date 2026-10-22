#pragma once
#include <string>

namespace hikari {
struct TailscaleInfo {
    bool installed = false;
    std::string ipv4;
};
// Reads local adapter state only; does not launch a CLI or handle login tokens.
TailscaleInfo tailscale_info();
bool is_tailscale_adapter(const wchar_t* name, const wchar_t* description);
bool is_tailscale_ipv4(const std::string& ip);
// Opens the installed Tailscale UI, or its official download page if absent.
bool open_tailscale_setup();
} // namespace hikari
