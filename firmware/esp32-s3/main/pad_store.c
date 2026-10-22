#include "pad_store.h"

#include <string.h>

#include "freertos/FreeRTOS.h"

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static pad_snapshot s_snap;
static volatile uint32_t s_injected;

void pad_store_init(void)
{
    memset(&s_snap, 0, sizeof(s_snap));
    s_snap.state.lx = s_snap.state.ly = s_snap.state.rx = s_snap.state.ry = HK_AXIS_CENTER;
    s_snap.state.flags = HK_PAD_FLAG_NEUTRAL;
}

void pad_store_publish(const hk_pad_state *s, int64_t rx_time_us)
{
    portENTER_CRITICAL(&s_mux);
    s_snap.state = *s;
    s_snap.rx_time_us = rx_time_us;
    s_snap.generation++;
    portEXIT_CRITICAL(&s_mux);
}

void pad_store_read(pad_snapshot *out)
{
    portENTER_CRITICAL(&s_mux);
    *out = s_snap;
    portEXIT_CRITICAL(&s_mux);
}

void pad_store_set_injected_buttons(uint32_t buttons) { s_injected = buttons; }
uint32_t pad_store_injected_buttons(void) { return s_injected; }
