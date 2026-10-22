#pragma once
// Invites: everything a client needs to pair with a host, as one string the
// user copies on the host and pastes on the client.
//
//   hikari://living-room.tail1234.ts.net/K7QM-2XPA-9TRB?ip=100.101.102.103
//   hikari://100.101.102.103:47801/K7QM-2XPA-9TRB
//
// `host` is preferably the host's Tailscale (MagicDNS) name, which survives
// address changes; `ip` is a fallback for clients whose DNS does not resolve
// MagicDNS names.

#include <cstdint>
#include <string>

namespace hikari {

struct Invite {
    std::string host;      // name or IP, optionally with ":port"
    std::string fallback;  // IP (optionally ":port"), may be empty
    std::string code;      // normalized pairing code (12 chars)
};

std::string make_invite(const std::string& host, uint16_t port, const std::string& fallback_ip,
                        const std::string& pairing_code);
// Accepts surrounding whitespace/quotes and a missing "hikari://" prefix.
bool parse_invite(const std::string& text, Invite& out);

}  // namespace hikari
