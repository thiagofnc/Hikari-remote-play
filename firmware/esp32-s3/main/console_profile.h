#pragma once
// A console "profile" is everything that makes the bridge look like one
// specific controller: USB descriptors, HID report layout and any handshake.
// Swapping profiles does not touch the host link, the latency path or the
// wake logic.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hk_link.h"
#include "tusb.h"

typedef struct console_profile {
    const char *name;
    uint8_t id; /* hk_profile */
    const tusb_desc_device_t *device_descriptor;
    const uint8_t *configuration_descriptor;
    const uint8_t *hid_report_descriptor;
    uint16_t hid_report_descriptor_len;
    const char **strings;
    int string_count;
    uint8_t report_len;
    /* Formats the console report for a state. */
    void (*build_report)(const hk_pad_state *s, uint32_t extra_buttons, uint8_t *out);
    /* Output reports from the console (LEDs/rumble/handshake). report_id is 0
     * when the ID is the first byte of buf (interrupt OUT endpoint). Runs in
     * the TinyUSB task. */
    void (*on_output)(uint8_t report_id, const uint8_t *buf, uint16_t len);
    /* Optional handshake hooks (NULL for classes without one). */
    void (*reset)(void);                              /* bus (re)attached: start the handshake over */
    bool (*take_reply)(const hk_pad_state *s, uint32_t extra_buttons, uint8_t *out); /* queued handshake reply */
    bool (*streaming)(void);                          /* console wants regular input reports */
} console_profile;

extern const console_profile g_profile_hori;
extern const console_profile g_profile_procon;
