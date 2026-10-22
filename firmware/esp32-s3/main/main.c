// Hikari controller bridge for ESP32-S3.
//
//   Windows host --UART 2 Mbaud--> [host_rx] --latest state--> [usb_report] --USB HID--> Switch 2
//
// Task layout (priorities high -> low):
//   usb_report  core 1  max-1   arms the next HID report the moment new state lands
//   host_rx     core 1  max-2   parses UART frames, publishes newest state, notifies usb_report
//   tinyusb     core 0  22      TinyUSB device stack (esp_tinyusb)
//   wake        core 0  3       wake strategies (may block for seconds)
//   main        core 0  1       1 Hz telemetry to the host

#include "board.h"
#include "console_usb.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hk_link.h"
#include "host_link.h"
#include "pad_store.h"
#include "telemetry.h"
#include "wake.h"

void app_main(void)
{
    board_init();
    pad_store_init();
    telemetry_init();
    wake_init();

    TaskHandle_t usb_task = console_usb_init();
    host_link_init(usb_task);

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        hk_telemetry t = {0};
        telemetry_snapshot(&t);
        t.session_id = host_link_session();
        t.usb_state = console_usb_state();
        t.remote_wakeup_enabled = console_usb_remote_wakeup_enabled();
        const int64_t last = host_link_last_valid_rx_us();
        t.failsafe_active = last == 0 || (esp_timer_get_time() - last) > (int64_t)host_link_failsafe_ms() * 1000;
        uint8_t buf[HK_TELEMETRY_PAYLOAD];
        host_link_send(HK_LINK_TELEMETRY, buf, hk_telemetry_encode(&t, buf));
    }
}
