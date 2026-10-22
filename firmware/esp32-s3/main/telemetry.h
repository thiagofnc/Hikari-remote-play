#pragma once
// Counters and latency histograms reported to the host once per second.
// Recording is a few instructions and lock-free (single writer per metric).

#include <stdint.h>

#include "hk_link.h"

typedef enum {
    LAT_RX_TO_SUBMIT = 0,
    LAT_RX_TO_CONSUMED,
    LAT_POLL_INTERVAL,
    LAT_COUNT
} lat_metric;

typedef struct {
    volatile uint32_t rx_packets;
    volatile uint32_t rx_invalid;
    volatile uint32_t rx_seq_gaps;
    volatile uint32_t rx_stale;
    volatile uint32_t usb_reports;
    volatile uint32_t usb_fail;
} fw_counters;

extern fw_counters g_counters;

void telemetry_init(void);
void telemetry_record(lat_metric m, int64_t us);
// Fills percentiles for the window and resets it (called from the 1 Hz task).
void telemetry_snapshot(hk_telemetry *out);
