#include "console_usb.h"

#include <string.h>

#include "board.h"
#include "console_profile.h"
#include "esp_timer.h"
#include "hk_link.h"
#include "host_link.h"
#include "pad_store.h"
#include "sdkconfig.h"
#include "telemetry.h"
#include "tinyusb.h"

static const console_profile *s_profile = &g_profile_hori;
static TaskHandle_t s_task;

static volatile bool s_mounted;
static volatile bool s_suspended;
static volatile bool s_remote_wakeup_en;

/* In-flight report bookkeeping (report task writes, TinyUSB task reads). */
static volatile int64_t s_inflight_rx_us;
static volatile bool s_inflight_valid;
static volatile int64_t s_last_complete_us;

uint8_t console_usb_state(void)
{
    if (!s_mounted) return HK_USB_DETACHED;
    return s_suspended ? HK_USB_SUSPENDED : HK_USB_ACTIVE;
}

bool console_usb_remote_wakeup_enabled(void) { return s_remote_wakeup_en; }

bool console_usb_remote_wakeup(void)
{
    if (!s_mounted || !s_suspended || !s_remote_wakeup_en) return false;
    return tud_remote_wakeup();
}

static void notify_state(void) { host_link_send_event(HK_EVENT_USB_STATE, console_usb_state()); }

/* ---- TinyUSB callbacks (run in the TinyUSB task) ------------------------ */

void tud_mount_cb(void)
{
    s_mounted = true;
    s_suspended = false;
    board_status_led(true);
    notify_state();
}

void tud_umount_cb(void)
{
    s_mounted = false;
    s_inflight_valid = false;
    board_status_led(false);
    notify_state();
}

void tud_suspend_cb(bool remote_wakeup_en)
{
    s_suspended = true;
    s_remote_wakeup_en = remote_wakeup_en;
    s_inflight_valid = false;
    board_status_led(false);
    notify_state();
}

void tud_resume_cb(void)
{
    s_suspended = false;
    board_status_led(true);
    notify_state();
    if (s_task) xTaskNotifyGive(s_task);
}

uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance)
{
    (void)instance;
    return s_profile->hid_report_descriptor;
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t type, uint8_t *buffer,
                               uint16_t reqlen)
{
    pad_snapshot snap;
    pad_store_read(&snap);
    if (reqlen < s_profile->report_len) return 0;
    s_profile->build_report(&snap.state, pad_store_injected_buttons(), buffer);
    return s_profile->report_len;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t type, uint8_t const *buffer,
                           uint16_t bufsize)
{
    s_profile->on_output(buffer, bufsize);
}

// The console just read our report off the bus: this is the moment the input
// actually reached it, so it is the most honest "input delivered" timestamp.
void tud_hid_report_complete_cb(uint8_t instance, uint8_t const *report, uint16_t len)
{
    const int64_t now = esp_timer_get_time();
    if (s_inflight_valid) telemetry_record(LAT_RX_TO_CONSUMED, now - s_inflight_rx_us);
    if (s_last_complete_us) telemetry_record(LAT_POLL_INTERVAL, now - s_last_complete_us);
    s_last_complete_us = now;
    s_inflight_valid = false;
    if (s_task) xTaskNotifyGive(s_task);  // a newer state may be waiting
}

/* ---- report task ---------------------------------------------------------- */

static void report_task(void *arg)
{
    uint32_t sent_generation = UINT32_MAX;
    uint32_t sent_injected = 0;
    int64_t last_send_us = 0;
    bool failsafe = false;
    uint8_t report[64];
    const TickType_t keepalive = pdMS_TO_TICKS(CONFIG_HIKARI_KEEPALIVE_REPORT_MS);

    for (;;) {
        ulTaskNotifyTake(pdTRUE, keepalive > 0 ? keepalive : 1);
        const int64_t now = esp_timer_get_time();

        // Failsafe: never leave a button held if the host goes quiet.
        const int64_t last_rx = host_link_last_valid_rx_us();
        const bool stale = last_rx == 0 || (now - last_rx) > (int64_t)host_link_failsafe_ms() * 1000;
        if (stale && !failsafe) {
            hk_pad_state neutral;
            memset(&neutral, 0, sizeof(neutral));
            neutral.lx = neutral.ly = neutral.rx = neutral.ry = HK_AXIS_CENTER;
            neutral.flags = HK_PAD_FLAG_NEUTRAL;
            neutral.session_id = host_link_session();
            pad_store_publish(&neutral, now);
            failsafe = true;
            host_link_send_event(HK_EVENT_FAILSAFE, 1);
        } else if (!stale && failsafe) {
            failsafe = false;
            host_link_send_event(HK_EVENT_FAILSAFE, 0);
        }

        if (!s_mounted || s_suspended || !tud_hid_ready()) continue;

        pad_snapshot snap;
        pad_store_read(&snap);
        const uint32_t injected = pad_store_injected_buttons();
        const bool changed = snap.generation != sent_generation || injected != sent_injected;
        const bool due = (now - last_send_us) >= (int64_t)CONFIG_HIKARI_KEEPALIVE_REPORT_MS * 1000;
        if (!changed && !due) continue;

        s_profile->build_report(&snap.state, injected, report);
        // Only a genuinely new host state is timed; keepalive repeats are not.
        s_inflight_rx_us = snap.rx_time_us;
        s_inflight_valid = snap.generation != sent_generation;
        if (tud_hid_report(0, report, s_profile->report_len)) {
            g_counters.usb_reports++;
            if (changed) {
                telemetry_record(LAT_RX_TO_SUBMIT, esp_timer_get_time() - snap.rx_time_us);
                if (snap.state.flags & HK_PAD_FLAG_LATENCY_MARK) {
                    board_latency_marker_pulse();
                    host_link_send_event(HK_EVENT_LATENCY_MARK, snap.state.seq);
                }
            }
            sent_generation = snap.generation;
            sent_injected = injected;
            last_send_us = now;
        } else {
            g_counters.usb_fail++;
            s_inflight_valid = false;
        }
    }
}

TaskHandle_t console_usb_init(void)
{
    // Report task first so callbacks can notify it from the start. Pinned to
    // core 1 next to host RX; TinyUSB's own task lives on core 0.
    xTaskCreatePinnedToCore(report_task, "usb_report", 4096, NULL, configMAX_PRIORITIES - 1, &s_task, 1);

    const tinyusb_config_t cfg = {
        .device_descriptor = s_profile->device_descriptor,
        .string_descriptor = s_profile->strings,
        .string_descriptor_count = s_profile->string_count,
        .external_phy = false,
        .configuration_descriptor = s_profile->configuration_descriptor,
    };
    ESP_ERROR_CHECK(tinyusb_driver_install(&cfg));
    return s_task;
}
