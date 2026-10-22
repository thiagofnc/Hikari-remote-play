#pragma once
// USB device side: presents the active console profile and streams reports.

#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// Creates the report task (returned so the host link can notify it).
TaskHandle_t console_usb_init(void);

uint8_t console_usb_state(void);              // hk_usb_state
bool console_usb_remote_wakeup_enabled(void); // console armed remote wakeup
// Requests USB resume signalling. Returns false if not suspended/permitted.
bool console_usb_remote_wakeup(void);
