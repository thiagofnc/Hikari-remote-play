#include "wake.h"

#include "board.h"
#include "console_usb.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "host_link.h"
#include "pad_store.h"

static QueueHandle_t s_queue;

uint8_t wake_supported_methods(void)
{
    uint8_t m = HK_WAKE_USB_REMOTE_WAKEUP | HK_WAKE_HOME_PRESS;
    if (board_has_power_button()) m |= HK_WAKE_POWER_BUTTON;
    return m;
}

static void report(uint8_t method, uint8_t result)
{
    host_link_send_event(HK_EVENT_WAKE, ((uint32_t)method << 8) | result);
}

static bool wait_for_state(uint8_t want, uint32_t timeout_ms)
{
    for (uint32_t t = 0; t < timeout_ms; t += 10) {
        if (console_usb_state() == want) return true;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return console_usb_state() == want;
}

static void press_home(uint16_t ms)
{
    pad_store_set_injected_buttons(HK_BTN_HOME);
    vTaskDelay(pdMS_TO_TICKS(ms ? ms : 120));
    pad_store_set_injected_buttons(0);
}

static void run_wake(const hk_wake *w)
{
    // A request for *only* the power button is a raw button press (used by
    // the host for "put the console to sleep"), regardless of state.
    if (w->methods == HK_WAKE_POWER_BUTTON) {
        if (!board_has_power_button()) {
            report(HK_WAKE_POWER_BUTTON, HK_WAKE_RESULT_UNSUPPORTED);
            return;
        }
        board_power_button_pulse(w->press_ms ? w->press_ms : 150);
        report(HK_WAKE_POWER_BUTTON, HK_WAKE_RESULT_SENT);
        return;
    }

    if (console_usb_state() == HK_USB_ACTIVE) {
        report(0, HK_WAKE_RESULT_ALREADY_AWAKE);
        // Awake bus but the host still asked: the console may be on the
        // sleep/lock screen with HDMI off. HOME brings the picture back.
        if (w->methods & HK_WAKE_HOME_PRESS) {
            press_home(w->press_ms);
            report(HK_WAKE_HOME_PRESS, HK_WAKE_RESULT_SENT);
        }
        return;
    }

    if (w->methods & HK_WAKE_USB_REMOTE_WAKEUP) {
        if (console_usb_state() != HK_USB_SUSPENDED) {
            report(HK_WAKE_USB_REMOTE_WAKEUP, HK_WAKE_RESULT_UNSUPPORTED);
        } else if (!console_usb_remote_wakeup_enabled()) {
            report(HK_WAKE_USB_REMOTE_WAKEUP, HK_WAKE_RESULT_NOT_PERMITTED);
        } else if (console_usb_remote_wakeup()) {
            report(HK_WAKE_USB_REMOTE_WAKEUP, HK_WAKE_RESULT_SENT);
            if (wait_for_state(HK_USB_ACTIVE, 1500) && (w->methods & HK_WAKE_HOME_PRESS)) {
                vTaskDelay(pdMS_TO_TICKS(150));  // let the console finish enumeration
                press_home(w->press_ms);
                report(HK_WAKE_HOME_PRESS, HK_WAKE_RESULT_SENT);
            }
            if (console_usb_state() == HK_USB_ACTIVE) return;
        } else {
            report(HK_WAKE_USB_REMOTE_WAKEUP, HK_WAKE_RESULT_NOT_PERMITTED);
        }
    }

    if (w->methods & HK_WAKE_POWER_BUTTON) {
        if (!board_has_power_button()) {
            report(HK_WAKE_POWER_BUTTON, HK_WAKE_RESULT_UNSUPPORTED);
        } else {
            board_power_button_pulse(w->press_ms ? w->press_ms : 150);
            report(HK_WAKE_POWER_BUTTON, HK_WAKE_RESULT_SENT);
        }
    }
}

static void wake_task(void *arg)
{
    hk_wake w;
    for (;;) {
        if (xQueueReceive(s_queue, &w, portMAX_DELAY)) run_wake(&w);
    }
}

void wake_init(void)
{
    s_queue = xQueueCreate(1, sizeof(hk_wake));  // length 1 for xQueueOverwrite
    xTaskCreatePinnedToCore(wake_task, "wake", 3072, NULL, 3, NULL, 0);
}

void wake_request(const hk_wake *w)
{
    // Coalesce: a pending request is replaced, never queued behind.
    xQueueOverwrite(s_queue, w);
}
