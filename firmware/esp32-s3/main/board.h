#pragma once
// Board-level GPIOs: latency marker, optional power-button emulation, LED.

#include <stdbool.h>
#include <stdint.h>

void board_init(void);

// Latency marker: high for 2 ms, ISR/task safe, non-blocking.
void board_latency_marker_pulse(void);

bool board_has_power_button(void);
// Blocking pulse of the power-button output (call from a low-priority task).
void board_power_button_pulse(uint16_t ms);

void board_status_led(bool on);
