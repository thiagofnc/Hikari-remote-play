# Hardware setup

Everything here happens **at home**, around the Switch 2 dock and the Host PC. The client side needs nothing beyond a Windows PC and a controller.

## What goes where

```
                         ┌──────────────────────┐
                         │   Nintendo Switch 2  │
                         │       (docked)       │
                         └──────┬────────┬──────┘
                          HDMI  │        │  USB-A (dock)
                                ▼        ▼
                  ┌──────────────┐    ┌───────────────────────────┐
                  │ USB 3.0 HDMI │    │    ESP32-S3-DevKitC-1     │
                  │ capture card │    │  [USB] ◄── to the dock    │
                  └──────┬───────┘    │  [UART] ──► to Host PC    │
                         │ USB 3.0    └──────────────┬────────────┘
                         ▼                           │ USB
                  ┌─────────────────────────────────────────────┐
                  │          Host PC (Windows 10/11)            │
                  │  blue/SS USB 3 port      any USB port       │
                  │              Ethernet to router             │
                  └─────────────────────────────────────────────┘
```

| Cable | From | To | Notes |
| --- | --- | --- | --- |
| HDMI | Dock **HDMI OUT** | Capture card **HDMI IN** | Use a known-good cable that handles 1080p60. |
| USB 3.0 | Capture card | Host PC **USB 3.0** port | Blue or "SS" marked port. **Not** through a USB 2.0 hub. |
| USB | ESP32 port labelled **USB** (native USB OTG, GPIO 19/20) | Any **USB-A port on the dock** | The console sees this as a wired controller. |
| USB | ESP32 port labelled **UART** (USB-to-serial bridge) | Host PC, any port | Host link at 2 Mbaud. It also powers the board. |

> [!TIP]
> The two ports on the DevKitC-1 are labelled on the silkscreen. If you swap them, the host won't find the bridge (no serial port) and the console won't see a controller. Older boards (v1.0) use micro-USB, newer ones (v1.1) use USB-C.

## 1. Choose a capture card

The capture card decides your floor for picture quality and latency.

- **It must do uncompressed 1080p60.** Look for NV12 or YUY2 at 1920×1080 @ 60 over USB 3.0. Many inexpensive USB 3.0 cards built on newer chipsets do this, and well-known brands do too.
- **Avoid USB 2.0 "HDMI to USB" dongles** for 60 fps play. They only reach 1080p60 with MJPEG compression (which adds a decode stage) or fall back to 30 fps.
- Want to watch on your TV at home too? Put an HDMI splitter *before* the capture card. Some splitters change the modes the dock offers. If the capture card stops getting 1080p60, test without the splitter.

Run `hikari-capture-probe` (in the release zip) to see what your card actually delivers per format: fps, frame-interval jitter and driver delay.

## 2. Flash the bridge firmware

You only do this once. You don't need a toolchain.

1. Download **`hikari-bridge-esp32s3.bin`** from the [latest release](https://github.com/thiagofnc/Hikari-remote-play/releases/latest).
2. Plug the board's **UART** port into any PC.
3. Open <https://espressif.github.io/esptool-js/> in **Chrome or Edge** (Web Serial is required) and click **Connect**. Choose the board's COM port (CP210x or CH343 / "USB-Enhanced-SERIAL").
4. Set the flash address to **`0x0`**, select the `.bin`, and click **Program**.
5. Unplug, then wire the board as shown above.

Alternatively, from a clone of this repository (needs Python, because it installs `esptool` with pip if missing):

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\flash-firmware.ps1
# options: -Bin .\hikari-bridge-esp32s3.bin  -Port COM7  -Tag v0.2.0  -Erase
```

The script finds the board's serial port, downloads `hikari-bridge-esp32s3.bin` from the latest release, checks it against `SHA256SUMS.txt` and writes it at `0x0`.

If the board doesn't connect for flashing, hold **BOOT**, tap **RESET**, release **BOOT**, then try again.

To build the firmware yourself, see [building.md](building.md#bridge-firmware).

### Firmware options (`idf.py menuconfig` → *Hikari bridge*)

| Option | Default | What it does |
| --- | --- | --- |
| `HIKARI_UART_PORT` / TX / RX GPIO | UART0, GPIO 43 / 44 | The host link. On the DevKitC-1 these go to the UART port's USB-serial chip. |
| `HIKARI_UART_BAUD` | 2 000 000 | The host must use the same rate. |
| `HIKARI_FAILSAFE_MS` | 250 | Neutral state when host frames stop (the host also sets this in `HELLO`). |
| `HIKARI_KEEPALIVE_REPORT_MS` | 8 | Re-send the current HID report at least this often. |
| `HIKARI_LATENCY_MARKER_GPIO` | 4 | 2 ms pulse for camera or logic-analyser latency tests. See [profiling.md](profiling.md#glass-to-glass-with-a-camera). |
| `HIKARI_POWER_BUTTON_GPIO` | −1 | Optional wake fallback. See [remote-wake.md](remote-wake.md#the-power-button-fallback). |
| `HIKARI_STATUS_LED_GPIO` | −1 | Optional LED: on while the console is actively polling the pad. |

## 3. Prepare the Host PC

The Host PC is a small always-on server. Set it up like one:

- **Use Ethernet.** The host's uplink carries the whole video stream, and Wi-Fi jitter here hurts every frame.
- **USB 3.0 for the capture card**, ideally a port directly on the machine, not a hub shared with other heavy devices.
- **Don't let it sleep.**
  - *Settings → System → Power*: set **Sleep** to *Never* when plugged in.
  - For a laptop with the lid closed: *Control Panel → Power Options → Choose what closing the lid does → When plugged in: Do nothing*.
  - Or from an elevated PowerShell:
    ```powershell
    powercfg /change standby-timeout-ac 0
    powercfg /change hibernate-timeout-ac 0
    ```
- **Plug the laptop in** and use a *Balanced* or *Best performance* power mode. Battery saver throttles the encoder.
- **Start with Windows**: enable *Start Hikari with Windows* in the Host settings. It then lives in the tray.
- **Firewall**: on first run, allow Hikari on *Private* networks. If you're asked nothing and clients can't connect, see [troubleshooting](troubleshooting.md#cant-connect--host-not-found).
- **Tailscale** (for Internet play): install it, sign in, and note the host's `100.x.y.z` address or MagicDNS name. Hikari shows both on the Host screen.

## 4. Check it

Start `Hikari.exe` on the Host PC and choose **Host**. The checklist should turn green:

| Check | If it stays red |
| --- | --- |
| Controller bridge | UART port not connected, firmware not flashed, or the ports are swapped. [Troubleshooting](troubleshooting.md#controller-bridge-not-detected). |
| Capture card | Card not on a working USB 3 port, or in use by another app (OBS, Camera). |
| HDMI signal | Console asleep, or the dock is not outputting. Wake it. |
| Audio | The card's audio endpoint is disabled in *Sound settings → Recording*. |
| Encoder | No hardware encoder, or GPU drivers missing. Update the GPU driver. |

Then wake the console and check that it shows a wired controller connected (*Controllers* menu).
