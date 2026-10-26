#include "host_link.h"

#include <string.h>

#include "console_usb.h"
#include "driver/uart.h"
#include "esp_timer.h"
#include "freertos/semphr.h"
#include "hk_link.h"
#include "pad_store.h"
#include "sdkconfig.h"
#include "telemetry.h"
#include "wake.h"

#define FW_VERSION 0x0104
#define RX_BUF 1024
#define TX_BUF 512

static const uart_port_t kPort = (uart_port_t)CONFIG_HIKARI_UART_PORT;

static TaskHandle_t s_usb_task;
static SemaphoreHandle_t s_tx_mutex;
static QueueHandle_t s_uart_queue;

static volatile uint32_t s_session;
static volatile uint16_t s_failsafe_ms = CONFIG_HIKARI_FAILSAFE_MS;
static volatile int64_t s_last_valid_rx;
static uint32_t s_last_seq;
static bool s_have_seq;

uint32_t host_link_session(void) { return s_session; }
uint16_t host_link_failsafe_ms(void) { return s_failsafe_ms; }
int64_t host_link_last_valid_rx_us(void) { return s_last_valid_rx; }

void host_link_send(uint8_t type, const uint8_t *payload, size_t len)
{
    uint8_t wire[HK_LINK_MAX_WIRE];
    const size_t n = hk_link_build(type, payload, len, wire, sizeof(wire));
    if (!n) return;
    xSemaphoreTake(s_tx_mutex, portMAX_DELAY);
    uart_write_bytes(kPort, (const char *)wire, n);
    xSemaphoreGive(s_tx_mutex);
}

void host_link_send_event(uint8_t code, uint32_t arg)
{
    hk_event e = {.code = code, .device_time_us = (uint32_t)esp_timer_get_time(), .arg = arg};
    uint8_t buf[16];
    host_link_send(HK_LINK_EVENT, buf, hk_event_encode(&e, buf));
}

static void send_hello_ack(void)
{
    hk_hello_ack a = {
        .link_version = HK_LINK_VERSION,
        .profile = console_usb_profile(),
        .fw_version = FW_VERSION,
        .session_id = s_session,
        .usb_state = console_usb_state(),
        .wake_methods = wake_supported_methods(),
        .remote_wakeup_enabled = console_usb_remote_wakeup_enabled(),
    };
    uint8_t buf[16];
    host_link_send(HK_LINK_HELLO_ACK, buf, hk_hello_ack_encode(&a, buf));
}

static void handle_frame(const hk_link_frame *f, int64_t rx_time)
{
    switch (f->type) {
    case HK_LINK_PAD_STATE: {
        hk_pad_state s;
        if (!hk_pad_decode(f->payload, f->len, &s)) {
            g_counters.rx_invalid++;
            return;
        }
        if (s.session_id != s_session) {  // packet from a previous host run
            g_counters.rx_stale++;
            return;
        }
        if (s_have_seq && !hk_seq_newer(s.seq, s_last_seq)) {
            g_counters.rx_stale++;
            return;
        }
        if (s_have_seq && s.seq != s_last_seq + 1) g_counters.rx_seq_gaps++;
        s_last_seq = s.seq;
        s_have_seq = true;
        g_counters.rx_packets++;
        s_last_valid_rx = rx_time;
        pad_store_publish(&s, rx_time);
        xTaskNotifyGive(s_usb_task);  // wake the report task immediately
        break;
    }
    case HK_LINK_HELLO: {
        hk_hello h;
        if (!hk_hello_decode(f->payload, f->len, &h)) {
            g_counters.rx_invalid++;
            return;
        }
        if (h.session_id != s_session) {
            s_session = h.session_id;
            s_have_seq = false;  // new host run: sequence restarts
        }
        if (h.failsafe_timeout_ms >= 20) s_failsafe_ms = h.failsafe_timeout_ms;
        s_last_valid_rx = rx_time;
        send_hello_ack();
        console_usb_select_profile(h.profile);  // restarts the bridge if the host wants another controller type
        break;
    }
    case HK_LINK_PING: {
        hk_ping p;
        if (!hk_ping_decode(f->payload, f->len, &p)) return;
        hk_pong r = {.seq = p.seq, .host_time_us = p.host_time_us, .device_time_us = (uint32_t)rx_time};
        uint8_t buf[16];
        host_link_send(HK_LINK_PONG, buf, hk_pong_encode(&r, buf));
        s_last_valid_rx = rx_time;
        break;
    }
    case HK_LINK_WAKE: {
        hk_wake w;
        if (hk_wake_decode(f->payload, f->len, &w)) wake_request(&w);
        break;
    }
    default:
        g_counters.rx_invalid++;
        break;
    }
}

static void host_rx_task(void *arg)
{
    hk_link_decoder dec;
    hk_link_decoder_init(&dec);
    uint8_t scratch[HK_LINK_MAX_FRAME];
    uint8_t buf[128];
    uint32_t last_bad = 0;
    for (;;) {
        uart_event_t ev;
        if (!xQueueReceive(s_uart_queue, &ev, portMAX_DELAY)) continue;
        if (ev.type == UART_FIFO_OVF || ev.type == UART_BUFFER_FULL) {
            uart_flush_input(kPort);
            xQueueReset(s_uart_queue);
            hk_link_decoder_init(&dec);
            continue;
        }
        if (ev.type != UART_DATA) continue;
        size_t avail = 0;
        uart_get_buffered_data_len(kPort, &avail);
        while (avail > 0) {
            const int n = uart_read_bytes(kPort, buf, avail < sizeof(buf) ? avail : sizeof(buf), 0);
            if (n <= 0) break;
            avail -= (size_t)n;
            for (int i = 0; i < n; ++i) {
                hk_link_frame f;
                if (hk_link_decoder_push(&dec, buf[i], scratch, sizeof(scratch), &f))
                    handle_frame(&f, esp_timer_get_time());
            }
        }
        if (dec.frames_bad != last_bad) {
            g_counters.rx_invalid += dec.frames_bad - last_bad;
            last_bad = dec.frames_bad;
        }
    }
}

void host_link_init(TaskHandle_t usb_task)
{
    s_usb_task = usb_task;
    s_tx_mutex = xSemaphoreCreateMutex();
    s_last_valid_rx = 0;

    const uart_config_t cfg = {
        .baud_rate = CONFIG_HIKARI_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(kPort, RX_BUF, TX_BUF, 16, &s_uart_queue, ESP_INTR_FLAG_IRAM));
    ESP_ERROR_CHECK(uart_param_config(kPort, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(kPort, CONFIG_HIKARI_UART_TX_GPIO, CONFIG_HIKARI_UART_RX_GPIO, UART_PIN_NO_CHANGE,
                                 UART_PIN_NO_CHANGE));
    // Deliver bytes to the task quickly: interrupt after 16 bytes or after
    // the line has been idle for 2 symbol times (end of a frame).
    uart_set_rx_full_threshold(kPort, 16);
    uart_set_rx_timeout(kPort, 2);

    xTaskCreatePinnedToCore(host_rx_task, "host_rx", 4096, NULL, configMAX_PRIORITIES - 2, NULL, 1);
}
