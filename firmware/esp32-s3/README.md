# Hikari bridge firmware (ESP32-S3)

The bridge makes the Switch 2 see a wired controller and lets the Hikari Host
drive it over a 2 Mbaud UART link.

```
Hikari Host (Windows) --USB cable--> [UART port]  ESP32-S3-DevKitC-1  [USB port] --USB cable--> Switch 2 dock
```

## Flashing (no toolchain needed)

1. Download `hikari-bridge-esp32s3.bin` from the latest release.
2. Plug the board's **UART** port into the PC. Open <https://espressif.github.io/esptool-js/>
   in Chrome/Edge, click **Connect**, choose the board's COM port.
3. Flash address **0x0**, select the `.bin`, click **Program**.
4. Plug the **USB** port (native USB OTG) into the Switch dock. Leave the UART
   port plugged into the host PC.

The Hikari Host finds the bridge automatically. You never pick a COM port.

## Building

ESP-IDF 5.3 or newer:

```sh
cd firmware/esp32-s3
idf.py set-target esp32s3
idf.py build flash
```

`idf.py menuconfig` → **Hikari bridge** exposes the UART pins/baud rate, the
failsafe timeout, the latency-marker GPIO and the optional power-button GPIO.

## How it stays fast

| Stage | Mechanism |
| --- | --- |
| UART RX | IRAM ISR, RX-timeout interrupt after 2 idle symbols, so a frame is handed over ~10 µs after its last byte |
| Hand-off | single-slot latest state under a spinlock; no FIFO, so stale input can't queue up |
| USB | `usb_report` task is notified directly, arms the HID IN endpoint immediately; 1 ms `bInterval` |
| Truth | `tud_hid_report_complete_cb` timestamps the moment the console *reads* the report, so latency is measured to the console, not just to our buffer |

Telemetry (1 Hz to the host): packets, gaps, stale drops, USB reports/s, and
p50/p95/p99/max for *UART RX → report armed*, *UART RX → console read*, and
the console's real polling interval.

## Safety

* No valid host frame for 250 ms (configurable, overridable by the host) → all
  buttons released, sticks centred.
* Frames from an older host run are rejected by session ID; out-of-order
  frames by sequence number.
* The task watchdog reboots a hung bridge into the neutral state.

## Waking the console

See [`docs/remote-wake.md`](../../docs/remote-wake.md). The firmware implements
an escalating chain: USB remote-wakeup → HOME press → (optional) power-button
GPIO. It reports the result of each attempt, so the host learns which method
works for your console.

### Optional power-button fallback

Set `HIKARI_POWER_BUTTON_GPIO` and wire an optocoupler (PC817 or similar):

```
GPIO ──[ 330 Ω ]──► PC817 anode      PC817 collector ──► power-button pad
GND  ─────────────► PC817 cathode    PC817 emitter   ──► button ground pad
```

The host requests a 150 ms pulse, the same as a short press. Pulses are
capped at 5 s, so the bridge can never hold the button long enough to force a
hard power-off.

## Profiles

USB descriptors and report layout live behind `console_profile`
(`main/console_profile.h`). The shipped profile is a HORI-style wired pad
(no handshake, 8-byte report). A Pro Controller profile with gyro can be
added as a new `profile_*.c` without touching the latency path.

## Tests

The link parser is shared with the host (`shared/protocol/hk_link.c`) and is
unit-tested and fuzzed natively. See `tests/fuzz_link.c` for the libFuzzer
harness.
