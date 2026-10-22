#include "board.h"

#include "driver/gpio.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static esp_timer_handle_t s_marker_timer;

static void marker_off(void *arg)
{
    (void)arg;
#if CONFIG_HIKARI_LATENCY_MARKER_GPIO >= 0
    gpio_set_level(CONFIG_HIKARI_LATENCY_MARKER_GPIO, 0);
#endif
}

static void out_pin(int pin, int level)
{
    if (pin < 0) return;
    gpio_config_t c = {
        .pin_bit_mask = 1ULL << pin,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&c);
    gpio_set_level(pin, level);
}

static int power_idle_level(void)
{
#if CONFIG_HIKARI_POWER_BUTTON_ACTIVE_HIGH
    return 0;
#else
    return 1;
#endif
}

void board_init(void)
{
    out_pin(CONFIG_HIKARI_LATENCY_MARKER_GPIO, 0);
    out_pin(CONFIG_HIKARI_POWER_BUTTON_GPIO, power_idle_level());
    out_pin(CONFIG_HIKARI_STATUS_LED_GPIO, 0);
    const esp_timer_create_args_t args = {
        .callback = marker_off,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "marker",
    };
    esp_timer_create(&args, &s_marker_timer);
}

void board_latency_marker_pulse(void)
{
#if CONFIG_HIKARI_LATENCY_MARKER_GPIO >= 0
    gpio_set_level(CONFIG_HIKARI_LATENCY_MARKER_GPIO, 1);
    esp_timer_stop(s_marker_timer);
    esp_timer_start_once(s_marker_timer, 2000);
#endif
}

bool board_has_power_button(void) { return CONFIG_HIKARI_POWER_BUTTON_GPIO >= 0; }

void board_power_button_pulse(uint16_t ms)
{
#if CONFIG_HIKARI_POWER_BUTTON_GPIO >= 0
    if (ms < 20) ms = 20;
    if (ms > 5000) ms = 5000; /* never hold long enough to force power-off */
    gpio_set_level(CONFIG_HIKARI_POWER_BUTTON_GPIO, !power_idle_level());
    vTaskDelay(pdMS_TO_TICKS(ms));
    gpio_set_level(CONFIG_HIKARI_POWER_BUTTON_GPIO, power_idle_level());
#else
    (void)ms;
#endif
}

void board_status_led(bool on)
{
#if CONFIG_HIKARI_STATUS_LED_GPIO >= 0
    gpio_set_level(CONFIG_HIKARI_STATUS_LED_GPIO, on ? 1 : 0);
#else
    (void)on;
#endif
}
