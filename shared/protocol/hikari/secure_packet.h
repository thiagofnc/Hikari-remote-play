#pragma once
// Authenticated control packets: header + body + 8-byte truncated HMAC.
// Media (video/audio) packets are not tagged; they are useless to an
// attacker and the VPN overlay already protects them in transit.

#include "hikari/crypto.h"
#include "hikari/net_protocol.h"

namespace hikari {

// Appends the tag. Returns the final datagram length (0 if it did not fit).
inline size_t seal(ByteWriter& w, const crypto::Hmac& key)
{
    if (!w.ok() || w.remaining() < proto::kTagSize) return 0;
    uint8_t tag[proto::kTagSize];
    key.tag(w.data(), w.size(), tag);
    w.bytes(tag, sizeof(tag));
    return w.size();
}

// Verifies the trailing tag. On success returns the length without the tag.
inline size_t open_sealed(const uint8_t* buf, size_t len, const crypto::Hmac& key)
{
    if (len < proto::kHeaderSize + proto::kTagSize) return 0;
    const size_t body = len - proto::kTagSize;
    return key.verify_tag(buf, body, buf + body) ? body : 0;
}

inline bool is_media(proto::PacketType t)
{
    return t == proto::PacketType::Video || t == proto::PacketType::VideoParity || t == proto::PacketType::Audio;
}

}  // namespace hikari
