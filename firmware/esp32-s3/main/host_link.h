#pragma once
// Wired link to the Windows host (UART through the board's USB-UART bridge).

#include <stddef.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

void host_link_init(TaskHandle_t usb_task);
// Thread-safe frame transmit (telemetry, acks, events).
void host_link_send(uint8_t type, const uint8_t *payload, size_t len);
void host_link_send_event(uint8_t code, uint32_t arg);

uint32_t host_link_session(void);
uint16_t host_link_failsafe_ms(void);
int64_t host_link_last_valid_rx_us(void);
