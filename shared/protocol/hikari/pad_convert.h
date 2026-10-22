#pragma once
// Network pad state (signed sticks, +Y up) -> wired link state (unsigned,
// +Y down) conversion, shared so host and tests agree bit-for-bit.

#include <algorithm>
#include <cstdint>

#include "hikari/net_protocol.h"
#include "hk_link.h"

namespace hikari {

inline uint16_t axis_to_link(int16_t v, bool invert)
{
    int32_t x = invert ? -static_cast<int32_t>(v) : v;  // centre stays exactly at 32768
    x = std::clamp<int32_t>(x, -32768, 32767);
    return static_cast<uint16_t>(x + 32768);
}

inline hk_pad_state to_link_state(const proto::PadState& p, uint32_t session, uint32_t seq, uint32_t host_time_us,
                                  uint8_t flags)
{
    hk_pad_state s{};
    s.flags = flags | (p.has_imu ? HK_PAD_FLAG_HAS_IMU : 0);
    s.session_id = session;
    s.seq = seq;
    s.host_time_us = host_time_us;
    s.buttons = p.buttons;
    // Analog triggers past ~25% count as ZL/ZR on consoles with digital triggers.
    if (p.lt > 64) s.buttons |= HK_BTN_ZL;
    if (p.rt > 64) s.buttons |= HK_BTN_ZR;
    s.lx = axis_to_link(p.lx, false);
    s.ly = axis_to_link(p.ly, true);
    s.rx = axis_to_link(p.rx, false);
    s.ry = axis_to_link(p.ry, true);
    s.lt = p.lt;
    s.rt = p.rt;
    for (int i = 0; i < 3; ++i) {
        s.gyro[i] = p.gyro[i];
        s.accel[i] = p.accel[i];
    }
    return s;
}

inline hk_pad_state neutral_link_state(uint32_t session, uint32_t seq, uint32_t host_time_us)
{
    proto::PadState neutral;
    return to_link_state(neutral, session, seq, host_time_us, HK_PAD_FLAG_NEUTRAL);
}

}  // namespace hikari
