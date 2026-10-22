#include "telemetry.h"

#include <string.h>

#include "esp_timer.h"

/* 25 us linear buckets up to 20 ms, plus an overflow bucket. Two windows are
 * double-buffered so the reporter can read one while writers fill the other. */
#define BUCKET_US 25
#define BUCKETS 800

typedef struct {
    uint16_t counts[BUCKETS + 1];
    uint32_t max;
    uint32_t total;
} lat_hist;

fw_counters g_counters;

static lat_hist s_hist[2][LAT_COUNT];
static volatile int s_cur;
static uint32_t s_last_reports, s_last_rx;
static int64_t s_last_snapshot_us;

void telemetry_init(void)
{
    memset(s_hist, 0, sizeof(s_hist));
    s_cur = 0;
    s_last_snapshot_us = esp_timer_get_time();
}

void telemetry_record(lat_metric m, int64_t us)
{
    if (us < 0) us = 0;
    lat_hist *h = &s_hist[s_cur][m];
    uint32_t idx = (uint32_t)(us / BUCKET_US);
    if (idx > BUCKETS) idx = BUCKETS;
    if (h->counts[idx] != 0xFFFF) h->counts[idx]++;
    h->total++;
    if ((uint32_t)us > h->max) h->max = (uint32_t)us;
}

static uint16_t sat16(uint32_t v) { return v > 0xFFFF ? 0xFFFF : (uint16_t)v; }

static void summarize(const lat_hist *h, hk_lat4 *out)
{
    memset(out, 0, sizeof(*out));
    if (h->total == 0) return;
    const uint32_t t50 = h->total / 2 + 1, t95 = (h->total * 95 + 99) / 100, t99 = (h->total * 99 + 99) / 100;
    uint32_t acc = 0;
    bool got50 = false, got95 = false, got99 = false;
    for (int i = 0; i <= BUCKETS; ++i) {
        acc += h->counts[i];
        const uint32_t v = (uint32_t)i * BUCKET_US + BUCKET_US / 2;
        if (!got50 && acc >= t50) { out->p50 = sat16(v); got50 = true; }
        if (!got95 && acc >= t95) { out->p95 = sat16(v); got95 = true; }
        if (!got99 && acc >= t99) { out->p99 = sat16(v); got99 = true; break; }
    }
    out->max = sat16(h->max);
    if (out->p99 > out->max) out->p99 = out->max;
    if (out->p95 > out->max) out->p95 = out->max;
    if (out->p50 > out->max) out->p50 = out->max;
}

void telemetry_snapshot(hk_telemetry *out)
{
    const int prev = s_cur;
    memset(&s_hist[1 - prev], 0, sizeof(s_hist[0]));
    s_cur = 1 - prev; /* writers move to the clean window */

    summarize(&s_hist[prev][LAT_RX_TO_SUBMIT], &out->rx_to_submit);
    summarize(&s_hist[prev][LAT_RX_TO_CONSUMED], &out->rx_to_consumed);
    summarize(&s_hist[prev][LAT_POLL_INTERVAL], &out->poll_interval);

    const int64_t now = esp_timer_get_time();
    const int64_t dt = now - s_last_snapshot_us;
    s_last_snapshot_us = now;
    const uint32_t reports = g_counters.usb_reports, rx = g_counters.rx_packets;
    if (dt > 0) {
        out->usb_reports_per_s = sat16((uint32_t)((int64_t)(reports - s_last_reports) * 1000000 / dt));
        out->rx_packets_per_s = sat16((uint32_t)((int64_t)(rx - s_last_rx) * 1000000 / dt));
    }
    s_last_reports = reports;
    s_last_rx = rx;

    out->uptime_ms = (uint32_t)(now / 1000);
    out->rx_packets = g_counters.rx_packets;
    out->rx_invalid = g_counters.rx_invalid;
    out->rx_seq_gaps = g_counters.rx_seq_gaps;
    out->rx_stale = g_counters.rx_stale;
    out->usb_reports = g_counters.usb_reports;
    out->usb_fail = g_counters.usb_fail;
}
