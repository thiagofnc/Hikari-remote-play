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

// The functions below ask the local Tailscale client (`tailscale status
// --json`). They block for up to ~3 s, so call them off the UI thread.

// This PC's MagicDNS name (e.g. "living-room.tail1234.ts.net"), or "" when
// Tailscale is not running / MagicDNS is unavailable.
std::string tailscale_dns_name();

struct TailscalePath {
    bool known = false;    // Tailscale answered and knows the peer
    bool direct = false;   // peer-to-peer UDP path established
    std::string relay;     // DERP region used when not direct (e.g. "nyc")
    std::string endpoint;  // direct path "ip:port"
};
// How traffic to the tailnet address `peer_ip` currently flows.
TailscalePath tailscale_path_to(const std::string& peer_ip);

// Parsers behind the two calls above (exposed for tests).
std::string tailscale_dns_name_from_json(const std::string& json);
TailscalePath tailscale_path_from_json(const std::string& json, const std::string& peer_ip);
} // namespace hikari
