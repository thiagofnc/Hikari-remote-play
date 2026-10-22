#include "hk_link.h"

#include <string.h>

/* CRC-16/CCITT-FALSE, nibble table: small enough for the MCU, fast enough
 * for 40-byte frames (a few hundred ns). */
uint16_t hk_crc16(const uint8_t *data, size_t len)
{
    static const uint16_t tbl[16] = {0x0000, 0x1021, 0x2042, 0x3063, 0x4084, 0x50A5, 0x60C6, 0x70E7,
                                     0x8108, 0x9129, 0xA14A, 0xB16B, 0xC18C, 0xD1AD, 0xE1CE, 0xF1EF};
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; ++i) {
        crc = (uint16_t)((crc << 4) ^ tbl[((crc >> 12) ^ (data[i] >> 4)) & 0x0F]);
        crc = (uint16_t)((crc << 4) ^ tbl[((crc >> 12) ^ (data[i] & 0x0F)) & 0x0F]);
    }
    return crc;
}

size_t hk_cobs_encode(const uint8_t *in, size_t len, uint8_t *out)
{
    size_t read = 0, write = 1, code_pos = 0;
    uint8_t code = 1;
    while (read < len) {
        if (in[read] == 0) {
            out[code_pos] = code;
            code = 1;
            code_pos = write++;
            ++read;
        } else {
            out[write++] = in[read++];
            if (++code == 0xFF) {
                out[code_pos] = code;
                code = 1;
                code_pos = write++;
            }
        }
    }
    out[code_pos] = code;
    return write;
}

size_t hk_cobs_decode(const uint8_t *in, size_t len, uint8_t *out, size_t out_cap)
{
    size_t read = 0, write = 0;
    while (read < len) {
        uint8_t code = in[read];
        if (code == 0 || read + code > len) return 0;
        ++read;
        for (uint8_t i = 1; i < code; ++i) {
            if (write >= out_cap || in[read] == 0) return 0;
            out[write++] = in[read++];
        }
        if (code != 0xFF && read != len) {
            if (write >= out_cap) return 0;
            out[write++] = 0;
        }
    }
    return write;
}

size_t hk_link_build(uint8_t type, const uint8_t *payload, size_t len, uint8_t *wire, size_t wire_cap)
{
    uint8_t raw[HK_LINK_MAX_FRAME];
    if (len + 3 > sizeof(raw)) return 0;
    raw[0] = type;
    if (len) memcpy(raw + 1, payload, len);
    uint16_t crc = hk_crc16(raw, len + 1);
    raw[len + 1] = (uint8_t)crc;
    raw[len + 2] = (uint8_t)(crc >> 8);
    size_t need = (len + 3) + (len + 3) / 254 + 2;
    if (wire_cap < need) return 0;
    size_t n = hk_cobs_encode(raw, len + 3, wire);
    wire[n++] = 0;
    return n;
}

/* ---- little-endian helpers -------------------------------------------- */

static inline void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}
static inline void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}
static inline uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* ---- payload codecs ---------------------------------------------------- */

size_t hk_pad_encode(const hk_pad_state *s, uint8_t *o)
{
    o[0] = HK_LINK_VERSION;
    o[1] = s->flags;
    put32(o + 2, s->session_id);
    put32(o + 6, s->seq);
    put32(o + 10, s->host_time_us);
    put32(o + 14, s->buttons);
    put16(o + 18, s->lx);
    put16(o + 20, s->ly);
    put16(o + 22, s->rx);
    put16(o + 24, s->ry);
    o[26] = s->lt;
    o[27] = s->rt;
    if (!(s->flags & HK_PAD_FLAG_HAS_IMU)) return HK_PAD_PAYLOAD_BASE;
    for (int i = 0; i < 3; ++i) put16(o + 28 + 2 * i, (uint16_t)s->gyro[i]);
    for (int i = 0; i < 3; ++i) put16(o + 34 + 2 * i, (uint16_t)s->accel[i]);
    return HK_PAD_PAYLOAD_IMU;
}

bool hk_pad_decode(const uint8_t *in, size_t len, hk_pad_state *s)
{
    if (len != HK_PAD_PAYLOAD_BASE && len != HK_PAD_PAYLOAD_IMU) return false;
    if (in[0] != HK_LINK_VERSION) return false;
    s->flags = in[1];
    bool imu = (s->flags & HK_PAD_FLAG_HAS_IMU) != 0;
    if (imu != (len == HK_PAD_PAYLOAD_IMU)) return false;
    s->session_id = get32(in + 2);
    s->seq = get32(in + 6);
    s->host_time_us = get32(in + 10);
    s->buttons = get32(in + 14);
    s->lx = get16(in + 18);
    s->ly = get16(in + 20);
    s->rx = get16(in + 22);
    s->ry = get16(in + 24);
    s->lt = in[26];
    s->rt = in[27];
    if (imu) {
        for (int i = 0; i < 3; ++i) s->gyro[i] = (int16_t)get16(in + 28 + 2 * i);
        for (int i = 0; i < 3; ++i) s->accel[i] = (int16_t)get16(in + 34 + 2 * i);
    } else {
        memset(s->gyro, 0, sizeof(s->gyro));
        memset(s->accel, 0, sizeof(s->accel));
    }
    return true;
}

size_t hk_hello_encode(const hk_hello *h, uint8_t *o)
{
    o[0] = HK_LINK_VERSION;
    put32(o + 1, h->session_id);
    put16(o + 5, h->failsafe_timeout_ms);
    o[7] = h->profile;
    return 8;
}

bool hk_hello_decode(const uint8_t *in, size_t len, hk_hello *h)
{
    if (len != 8 || in[0] != HK_LINK_VERSION) return false;
    h->session_id = get32(in + 1);
    h->failsafe_timeout_ms = get16(in + 5);
    h->profile = in[7];
    return true;
}

size_t hk_hello_ack_encode(const hk_hello_ack *h, uint8_t *o)
{
    o[0] = h->link_version;
    o[1] = h->profile;
    put16(o + 2, h->fw_version);
    put32(o + 4, h->session_id);
    o[8] = h->usb_state;
    o[9] = h->wake_methods;
    o[10] = h->remote_wakeup_enabled;
    return 11;
}

bool hk_hello_ack_decode(const uint8_t *in, size_t len, hk_hello_ack *h)
{
    if (len != 11) return false;
    h->link_version = in[0];
    h->profile = in[1];
    h->fw_version = get16(in + 2);
    h->session_id = get32(in + 4);
    h->usb_state = in[8];
    h->wake_methods = in[9];
    h->remote_wakeup_enabled = in[10];
    return true;
}

size_t hk_wake_encode(const hk_wake *w, uint8_t *o)
{
    o[0] = w->methods;
    put16(o + 1, w->press_ms);
    return 3;
}

bool hk_wake_decode(const uint8_t *in, size_t len, hk_wake *w)
{
    if (len != 3) return false;
    w->methods = in[0];
    w->press_ms = get16(in + 1);
    return true;
}

size_t hk_ping_encode(const hk_ping *p, uint8_t *o)
{
    put32(o, p->seq);
    put32(o + 4, p->host_time_us);
    return 8;
}

bool hk_ping_decode(const uint8_t *in, size_t len, hk_ping *p)
{
    if (len != 8) return false;
    p->seq = get32(in);
    p->host_time_us = get32(in + 4);
    return true;
}

size_t hk_pong_encode(const hk_pong *p, uint8_t *o)
{
    put32(o, p->seq);
    put32(o + 4, p->host_time_us);
    put32(o + 8, p->device_time_us);
    return 12;
}

bool hk_pong_decode(const uint8_t *in, size_t len, hk_pong *p)
{
    if (len != 12) return false;
    p->seq = get32(in);
    p->host_time_us = get32(in + 4);
    p->device_time_us = get32(in + 8);
    return true;
}

static size_t put_lat4(uint8_t *o, const hk_lat4 *l)
{
    put16(o, l->p50);
    put16(o + 2, l->p95);
    put16(o + 4, l->p99);
    put16(o + 6, l->max);
    return 8;
}

static void get_lat4(const uint8_t *in, hk_lat4 *l)
{
    l->p50 = get16(in);
    l->p95 = get16(in + 2);
    l->p99 = get16(in + 4);
    l->max = get16(in + 6);
}

size_t hk_telemetry_encode(const hk_telemetry *t, uint8_t *o)
{
    put32(o + 0, t->uptime_ms);
    put32(o + 4, t->session_id);
    put32(o + 8, t->rx_packets);
    put32(o + 12, t->rx_invalid);
    put32(o + 16, t->rx_seq_gaps);
    put32(o + 20, t->rx_stale);
    put32(o + 24, t->usb_reports);
    put32(o + 28, t->usb_fail);
    put16(o + 32, t->usb_reports_per_s);
    put16(o + 34, t->rx_packets_per_s);
    put_lat4(o + 36, &t->rx_to_submit);
    put_lat4(o + 44, &t->rx_to_consumed);
    put_lat4(o + 52, &t->poll_interval);
    o[60] = t->usb_state;
    o[61] = t->failsafe_active;
    o[62] = t->remote_wakeup_enabled;
    o[63] = 0;
    return HK_TELEMETRY_PAYLOAD;
}

bool hk_telemetry_decode(const uint8_t *in, size_t len, hk_telemetry *t)
{
    if (len != HK_TELEMETRY_PAYLOAD) return false;
    t->uptime_ms = get32(in + 0);
    t->session_id = get32(in + 4);
    t->rx_packets = get32(in + 8);
    t->rx_invalid = get32(in + 12);
    t->rx_seq_gaps = get32(in + 16);
    t->rx_stale = get32(in + 20);
    t->usb_reports = get32(in + 24);
    t->usb_fail = get32(in + 28);
    t->usb_reports_per_s = get16(in + 32);
    t->rx_packets_per_s = get16(in + 34);
    get_lat4(in + 36, &t->rx_to_submit);
    get_lat4(in + 44, &t->rx_to_consumed);
    get_lat4(in + 52, &t->poll_interval);
    t->usb_state = in[60];
    t->failsafe_active = in[61];
    t->remote_wakeup_enabled = in[62];
    return true;
}

size_t hk_event_encode(const hk_event *e, uint8_t *o)
{
    o[0] = e->code;
    put32(o + 1, e->device_time_us);
    put32(o + 5, e->arg);
    return 9;
}

bool hk_event_decode(const uint8_t *in, size_t len, hk_event *e)
{
    if (len != 9) return false;
    e->code = in[0];
    e->device_time_us = get32(in + 1);
    e->arg = get32(in + 5);
    return true;
}

uint8_t hk_dpad_to_hat(uint32_t b)
{
    bool up = (b & HK_BTN_DPAD_UP) != 0, down = (b & HK_BTN_DPAD_DOWN) != 0;
    bool left = (b & HK_BTN_DPAD_LEFT) != 0, right = (b & HK_BTN_DPAD_RIGHT) != 0;
    if (up && down) up = down = false; /* SOCD: neutral */
    if (left && right) left = right = false;
    if (up) return right ? 1 : left ? 7 : 0;
    if (down) return right ? 3 : left ? 5 : 4;
    if (right) return 2;
    if (left) return 6;
    return 8;
}

/* ---- streaming decoder -------------------------------------------------- */

void hk_link_decoder_init(hk_link_decoder *d) { memset(d, 0, sizeof(*d)); }

bool hk_link_decoder_push(hk_link_decoder *d, uint8_t byte, uint8_t *scratch, size_t scratch_cap,
                          hk_link_frame *frame)
{
    if (byte != 0) {
        if (d->raw_len < sizeof(d->raw))
            d->raw[d->raw_len++] = byte;
        else
            d->overflow = true;
        return false;
    }
    /* Delimiter: try to decode what we have. */
    size_t raw_len = d->raw_len;
    bool overflow = d->overflow;
    d->raw_len = 0;
    d->overflow = false;
    if (raw_len == 0) return false; /* idle delimiters are allowed */
    if (overflow) {
        ++d->frames_bad;
        return false;
    }
    size_t n = hk_cobs_decode(d->raw, raw_len, scratch, scratch_cap);
    if (n < 3) {
        ++d->frames_bad;
        return false;
    }
    uint16_t want = (uint16_t)(scratch[n - 2] | (scratch[n - 1] << 8));
    if (hk_crc16(scratch, n - 2) != want) {
        ++d->frames_bad;
        return false;
    }
    ++d->frames_ok;
    frame->type = scratch[0];
    frame->payload = scratch + 1;
    frame->len = n - 3;
    return true;
}
