#pragma once
// Minimal authentication primitives for pairing and per-packet tags.
//
// Confidentiality of the media stream is delegated to the VPN overlay
// (Tailscale / WireGuard). What Hikari adds on top is *authentication*:
// nobody without the pairing code can drive the console, even on the same
// tailnet, and input packets cannot be replayed into a later session.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace hikari::crypto {

using Digest = std::array<uint8_t, 32>;

class Sha256 {
public:
    Sha256() { reset(); }
    void reset();
    void update(const void* data, size_t len);
    Digest finish();

private:
    void block(const uint8_t* p);
    uint32_t h_[8];
    uint8_t buf_[64];
    size_t buf_len_ = 0;
    uint64_t total_ = 0;
};

Digest sha256(const void* data, size_t len);

// HMAC-SHA256 with a precomputed key schedule so per-packet tagging only
// hashes the message (two compression rounds for a small input packet).
class Hmac {
public:
    Hmac() = default;
    explicit Hmac(const uint8_t* key, size_t len) { set_key(key, len); }
    void set_key(const uint8_t* key, size_t len);
    Digest mac(const void* data, size_t len) const;
    Digest mac2(const void* a, size_t alen, const void* b, size_t blen) const;
    // First kTagSize bytes of the MAC.
    void tag(const void* data, size_t len, uint8_t out[8]) const;
    bool verify_tag(const void* data, size_t len, const uint8_t tag[8]) const;

private:
    Sha256 inner_, outer_;
};

bool constant_time_equal(const uint8_t* a, const uint8_t* b, size_t n);

void random_bytes(void* out, size_t n);
uint32_t random_u32();

// Pairing codes look like "K7QM-2XPA-9TRB" (60 bits, Crockford base32).
std::string generate_pairing_code();
// Uppercases, strips separators, maps ambiguous characters (O->0, I/L->1).
std::string normalize_pairing_code(const std::string& code);
Digest psk_from_code(const std::string& code);

// Handshake derivations (see docs/protocol.md).
Digest auth_mac(const Digest& psk, const uint8_t* client_nonce, const uint8_t* host_nonce);
Digest welcome_mac(const Digest& psk, const uint8_t* host_nonce, const uint8_t* client_nonce);
Digest session_key(const Digest& psk, const uint8_t* client_nonce, const uint8_t* host_nonce);

}  // namespace hikari::crypto
