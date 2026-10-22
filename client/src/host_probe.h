#pragma once
// One-shot pairing check used by the Connect dialog: runs the real handshake
// with the probe flag, so the host proves it knows the pairing code without
// opening a session (and without kicking anyone who is playing).

#include <atomic>
#include <string>
#include <vector>

namespace hikari {

enum class ProbeResult { Ok, WrongCode, VersionMismatch, NoAnswer, CannotResolve, Cancelled };

struct ProbeOutcome {
    ProbeResult result = ProbeResult::NoAnswer;
    std::string host_name;  // as the host calls itself
    std::string address;    // candidate that answered
};

// Tries every candidate address ("name", "ip", "ip:port") in parallel.
// Blocks for up to `timeout_ms`; `cancel` aborts early.
ProbeOutcome probe_host(const std::vector<std::string>& candidates, const std::string& pairing_code,
                        const std::atomic<bool>* cancel = nullptr, int timeout_ms = 6000);

}  // namespace hikari
