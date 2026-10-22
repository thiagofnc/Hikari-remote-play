/*
 * Hikari host <-> ESP32-S3 wired link protocol.
 *
 * Shared, dependency-free C99 so the exact same parser runs on the Windows
 * host, inside the ESP32 firmware and inside the native unit/fuzz tests.
 *
 * Wire format (every frame):
 *
 *     COBS( type:u8 | payload[...] | crc16:u16le ) 0x00
 *
 * COBS gives us a 0x00 frame delimiter that can never appear inside a frame,
 * so the receiver re-synchronises after at most one corrupted frame and
 * boot-loader noise on the UART is harmlessly discarded.
 *
 * Gaming input is "newest state wins": there is no retransmission, no FIFO
 * and no acknowledgement in the hot path. Every pad frame carries the full
 * controller state and a monotonically increasing sequence number.
 */
#ifndef HK_LINK_H
#define HK_LINK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HK_LINK_VERSION 1u
#define HK_LINK_DEFAULT_BAUD 2000000u

/* Largest decoded frame (type + payload + crc). */
#define HK_LINK_MAX_FRAME 96u
/* Largest encoded frame including COBS overhead and the 0x00 delimiter. */
#define HK_LINK_MAX_WIRE (HK_LINK_MAX_FRAME + HK_LINK_MAX_FRAME / 254u + 2u)

enum hk_link_type {
    /* host -> device */
    HK_LINK_PAD_STATE = 0x01,
    HK_LINK_HELLO = 0x02,
    HK_LINK_PING = 0x03,
    HK_LINK_CONFIG = 0x04,
    HK_LINK_WAKE = 0x05,
    /* device -> host */
    HK_LINK_TELEMETRY = 0x81,
    HK_LINK_HELLO_ACK = 0x82,
    HK_LINK_PONG = 0x83,
    HK_LINK_EVENT = 0x84,
};

/* Canonical buttons, Nintendo naming. Bits 0..13 intentionally match the
 * HORI-style wired pad report so the firmware can copy them directly. */
#define HK_BTN_Y (1u << 0)
#define HK_BTN_B (1u << 1)
#define HK_BTN_A (1u << 2)
#define HK_BTN_X (1u << 3)
#define HK_BTN_L (1u << 4)
#define HK_BTN_R (1u << 5)
#define HK_BTN_ZL (1u << 6)
#define HK_BTN_ZR (1u << 7)
#define HK_BTN_MINUS (1u << 8)
#define HK_BTN_PLUS (1u << 9)
#define HK_BTN_LSTICK (1u << 10)
#define HK_BTN_RSTICK (1u << 11)
#define HK_BTN_HOME (1u << 12)
#define HK_BTN_CAPTURE (1u << 13)
#define HK_BTN_DPAD_UP (1u << 16)
#define HK_BTN_DPAD_DOWN (1u << 17)
#define HK_BTN_DPAD_LEFT (1u << 18)
#define HK_BTN_DPAD_RIGHT (1u << 19)
#define HK_BTN_FACE_MASK 0x3FFFu
#define HK_BTN_DPAD_MASK 0xF0000u

/* Axis convention on the link: 0..65535, centre 32768, +Y is DOWN (HID). */
#define HK_AXIS_CENTER 32768u

/* Pad flags */
#define HK_PAD_FLAG_HAS_IMU (1u << 0)
/* Ask the firmware to pulse the latency-marker GPIO when this exact state is
 * handed to the USB controller (glass-to-glass measurement hook). */
#define HK_PAD_FLAG_LATENCY_MARK (1u << 1)
/* Host-originated neutral state (e.g. client disconnected). */
#define HK_PAD_FLAG_NEUTRAL (1u << 2)

typedef struct hk_pad_state {
    uint8_t flags;
    uint32_t session_id;
    uint32_t seq;
    uint32_t host_time_us; /* low 32 bits of host monotonic clock */
    uint32_t buttons;
    uint16_t lx, ly, rx, ry;
    uint8_t lt, rt;
    int16_t gyro[3];
    int16_t accel[3];
} hk_pad_state;

#define HK_PAD_PAYLOAD_BASE 28u
#define HK_PAD_PAYLOAD_IMU 40u

typedef struct hk_hello {
    uint32_t session_id;
    uint16_t failsafe_timeout_ms; /* neutral state after this much silence */
    uint8_t profile;              /* console report profile, see hk_profile */
} hk_hello;

enum hk_profile {
    HK_PROFILE_HORI_PAD = 0, /* wired HORI-style pad, no handshake */
    HK_PROFILE_PRO_CONTROLLER = 1, /* reserved for future Pro Controller emulation */
};

/* USB link state as seen by the device. A suspended bus is the most reliable
 * signal we have that the console is asleep (the dock keeps VBUS up). */
enum hk_usb_state {
    HK_USB_DETACHED = 0,  /* no host / not enumerated: console off or undocked */
    HK_USB_SUSPENDED = 1, /* enumerated, bus suspended: console asleep */
    HK_USB_ACTIVE = 2,    /* enumerated and polling: console awake */
};

/* Wake methods. The host asks for a set; the firmware runs the ones it
 * supports in escalating order and reports what happened via HK_EVENT_WAKE.
 * New hardware methods slot in without protocol changes. */
#define HK_WAKE_USB_REMOTE_WAKEUP (1u << 0) /* USB resume signalling (needs host-enabled remote wakeup) */
#define HK_WAKE_HOME_PRESS (1u << 1)        /* press HOME once the bus resumes */
#define HK_WAKE_POWER_BUTTON (1u << 2)      /* GPIO pulse into an optocoupler/relay across the power button */
#define HK_WAKE_HDMI_CEC (1u << 3)          /* reserved: CEC <Image View On>/<User Control Pressed: Power> */

/* A request whose mask is exactly HK_WAKE_POWER_BUTTON is a raw power-button
 * press in any console state (the host uses it to put the console to sleep). */
typedef struct hk_wake {
    uint8_t methods;           /* HK_WAKE_* mask */
    uint16_t press_ms;         /* power-button pulse / HOME hold time */
} hk_wake;

typedef struct hk_hello_ack {
    uint8_t link_version;
    uint8_t profile;
    uint16_t fw_version;
    uint32_t session_id;
    uint8_t usb_state;     /* hk_usb_state */
    uint8_t wake_methods;  /* HK_WAKE_* supported by this build/board */
    uint8_t remote_wakeup_enabled; /* console allowed USB remote wakeup (SET_FEATURE) */
} hk_hello_ack;

typedef struct hk_ping {
    uint32_t seq;
    uint32_t host_time_us;
} hk_ping;

typedef struct hk_pong {
    uint32_t seq;
    uint32_t host_time_us; /* echoed */
    uint32_t device_time_us;
} hk_pong;

/* Latency summary in microseconds, saturated to 65535. */
typedef struct hk_lat4 {
    uint16_t p50, p95, p99, max;
} hk_lat4;

typedef struct hk_telemetry {
    uint32_t uptime_ms;
    uint32_t session_id;
    uint32_t rx_packets;
    uint32_t rx_invalid;
    uint32_t rx_seq_gaps;
    uint32_t rx_stale;
    uint32_t usb_reports;
    uint32_t usb_fail;
    uint16_t usb_reports_per_s;
    uint16_t rx_packets_per_s;
    hk_lat4 rx_to_submit;   /* host packet RX complete -> USB report armed */
    hk_lat4 rx_to_consumed; /* host packet RX complete -> console read the report */
    hk_lat4 poll_interval;  /* interval between console reads */
    uint8_t usb_state;      /* hk_usb_state */
    uint8_t failsafe_active;
    uint8_t remote_wakeup_enabled;
} hk_telemetry;

#define HK_TELEMETRY_PAYLOAD 64u

enum hk_event_code {
    HK_EVENT_USB_STATE = 1,   /* arg = hk_usb_state */
    HK_EVENT_FAILSAFE = 3,    /* arg = 1 entered, 0 left */
    HK_EVENT_LATENCY_MARK = 4,/* arg = pad seq that pulsed the marker */
    HK_EVENT_WAKE = 5,        /* arg = (method_tried << 8) | result (hk_wake_result) */
};

enum hk_wake_result {
    HK_WAKE_RESULT_SENT = 1,          /* signal emitted */
    HK_WAKE_RESULT_UNSUPPORTED = 2,   /* method not available on this board/state */
    HK_WAKE_RESULT_NOT_PERMITTED = 3, /* console did not enable USB remote wakeup */
    HK_WAKE_RESULT_ALREADY_AWAKE = 4,
};

typedef struct hk_event {
    uint8_t code;
    uint32_t device_time_us;
    uint32_t arg;
} hk_event;

/* ---- primitives ------------------------------------------------------ */

uint16_t hk_crc16(const uint8_t *data, size_t len);

/* COBS. Returns encoded/decoded length, or 0 on error. Encode output must
 * hold len + len/254 + 1 bytes. The 0x00 delimiter is NOT written. */
size_t hk_cobs_encode(const uint8_t *in, size_t len, uint8_t *out);
size_t hk_cobs_decode(const uint8_t *in, size_t len, uint8_t *out, size_t out_cap);

/* Wrap-around safe sequence comparison: true if a is newer than b. */
static inline bool hk_seq_newer(uint32_t a, uint32_t b) { return (int32_t)(a - b) > 0; }

/* Builds a complete wire frame (COBS + delimiter). Returns bytes written or 0. */
size_t hk_link_build(uint8_t type, const uint8_t *payload, size_t len, uint8_t *wire, size_t wire_cap);

/* ---- payload codecs (explicit little-endian, no struct packing) ------- */

size_t hk_pad_encode(const hk_pad_state *s, uint8_t *out); /* out >= HK_PAD_PAYLOAD_IMU */
bool hk_pad_decode(const uint8_t *in, size_t len, hk_pad_state *s);

size_t hk_hello_encode(const hk_hello *h, uint8_t *out);
bool hk_hello_decode(const uint8_t *in, size_t len, hk_hello *h);
size_t hk_hello_ack_encode(const hk_hello_ack *h, uint8_t *out);
bool hk_hello_ack_decode(const uint8_t *in, size_t len, hk_hello_ack *h);
size_t hk_wake_encode(const hk_wake *w, uint8_t *out);
bool hk_wake_decode(const uint8_t *in, size_t len, hk_wake *w);
size_t hk_ping_encode(const hk_ping *p, uint8_t *out);
bool hk_ping_decode(const uint8_t *in, size_t len, hk_ping *p);
size_t hk_pong_encode(const hk_pong *p, uint8_t *out);
bool hk_pong_decode(const uint8_t *in, size_t len, hk_pong *p);
size_t hk_telemetry_encode(const hk_telemetry *t, uint8_t *out);
bool hk_telemetry_decode(const uint8_t *in, size_t len, hk_telemetry *t);
size_t hk_event_encode(const hk_event *e, uint8_t *out);
bool hk_event_decode(const uint8_t *in, size_t len, hk_event *e);

/* Hat value (0=up, clockwise, 8=centre) from the DPAD bits. */
uint8_t hk_dpad_to_hat(uint32_t buttons);

/* ---- streaming frame decoder ------------------------------------------ */

typedef struct hk_link_decoder {
    uint8_t raw[HK_LINK_MAX_WIRE];
    size_t raw_len;
    bool overflow;
    uint32_t frames_ok;
    uint32_t frames_bad; /* COBS/CRC/length failures */
} hk_link_decoder;

typedef struct hk_link_frame {
    uint8_t type;
    const uint8_t *payload;
    size_t len;
} hk_link_frame;

void hk_link_decoder_init(hk_link_decoder *d);

/* Feeds one byte. Returns true when a complete, CRC-valid frame is available
 * in *frame. The payload pointer stays valid until the next call. */
bool hk_link_decoder_push(hk_link_decoder *d, uint8_t byte, uint8_t *scratch, size_t scratch_cap,
                          hk_link_frame *frame);

#ifdef __cplusplus
}
#endif

#endif /* HK_LINK_H */
