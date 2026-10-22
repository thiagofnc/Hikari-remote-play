#pragma once
// Latest controller state, shared between the host-RX task (writer) and the
// USB report task (reader). Exactly one slot: newer state overwrites older
// state, there is never a backlog of stale input.

#include <stdbool.h>
#include <stdint.h>

#include "hk_link.h"

typedef struct pad_snapshot {
    hk_pad_state state;
    int64_t rx_time_us;   /* when the frame finished arriving on the UART */
    uint32_t generation;  /* increments on every publish */
} pad_snapshot;

void pad_store_init(void);
void pad_store_publish(const hk_pad_state *s, int64_t rx_time_us);
// Copies the newest snapshot. Cheap (~40 byte copy under a spinlock).
void pad_store_read(pad_snapshot *out);

// Buttons OR'ed into every report while active (used by wake: HOME press).
void pad_store_set_injected_buttons(uint32_t buttons);
uint32_t pad_store_injected_buttons(void);
