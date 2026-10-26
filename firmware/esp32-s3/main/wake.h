#pragma once
// Console wake strategies, tried in escalating order:
//
//   1. USB remote wakeup  - resume signalling on the suspended bus. Only works
//                           if the console armed DEVICE_REMOTE_WAKEUP before
//                           sleeping (reported to the host as a capability).
//   2. HOME press         - once the bus resumes, press HOME so the console
//                           leaves the sleep screen and turns on HDMI.
//   3. BLE beacon         - replay the Bluetooth LE wake beacon of a paired
//                           Joy-Con 2, learned by the bridge (see ble_wake.h).
//                           The only method a sleeping Switch 2 responds to
//                           without extra hardware.
//   4. Power button       - optional hardware fallback: GPIO pulse into an
//                           optocoupler/relay across the console's power button.
//
// Every attempt is reported to the host as an HK_EVENT_WAKE event so the host
// can measure which method actually works on a given console and firmware.

#include <stdint.h>

#include "hk_link.h"

void wake_init(void);
uint8_t wake_supported_methods(void);
// Non-blocking: queues the request for the wake task.
void wake_request(const hk_wake *w);
