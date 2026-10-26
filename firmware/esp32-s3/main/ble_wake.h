#pragma once
// Switch 2 wake beacon over Bluetooth LE.
//
// A sleeping Switch 2 ignores USB controllers, but it keeps listening for its
// paired Switch 2 controllers: pressing HOME on a Joy-Con 2 makes it broadcast
// an unencrypted advertisement (Nintendo company ID 0x0553, state byte 0x81,
// the console's Bluetooth address) from the controller's own address, and that
// wakes the console. The bridge listens for that beacon all the time, learns
// it the first time the user wakes the console with a Joy-Con 2, and replays
// it from the same address when the host asks for a wake.

#include <stdbool.h>
#include <stdint.h>

void ble_wake_init(void);
// True once a beacon has been learned (persisted across reboots).
bool ble_wake_ready(void);
// Broadcasts the learned beacon for duration_ms. Blocks; call from the wake
// task. Returns an hk_wake_result.
uint8_t ble_wake_send(uint32_t duration_ms);
