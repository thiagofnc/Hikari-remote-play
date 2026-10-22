# Troubleshooting

Each entry follows the same pattern: **symptom → cause → fix**. Start with the logs, because they usually name the problem directly:

```
%LOCALAPPDATA%\Hikari\logs\host.log        (and host.prev.log from the previous run)
%LOCALAPPDATA%\Hikari\logs\client.log
```

Press <kbd>Win</kbd>+<kbd>R</kbd>, paste `%LOCALAPPDATA%\Hikari\logs` and press Enter. When you open an issue, attach both logs plus the output of `hikari-pad status` and `hikari-capture-probe`.

---

## Picture

### No picture / "Capture card has no HDMI signal"

| Cause | Fix |
| --- | --- |
| Console asleep or off | Press **Play** (it wakes the console) or wake it manually. The host decides "picture present" by checking that the frame isn't one uniform colour, so a black screen counts as no signal. |
| Dock not outputting | Make sure the console is seated in the dock and the dock has power. Try another HDMI cable. |
| Card in use by another app | Close OBS, Camera, Discord or anything else previewing the card. Windows gives the card to one app at a time. |
| Splitter / EDID problems | Remove any HDMI splitter or switch and test direct. |
| Card enumerates late | Some cards only lock onto a signal that was present when they were plugged in. Replug the card with the console awake. |

### 30 fps instead of 60 (or stutter at 60)

| Cause | Fix |
| --- | --- |
| Card on a **USB 2.0** port or hub | Move it to a blue or "SS" USB 3.0 port directly on the PC. `host.log` shows the chosen mode (`Capture: <card>, 1920x1080@60 NV12 …`). |
| Card running **MJPEG** | The card offers 1080p60 only as MJPEG on this port or in this mode. Fix the USB 3 connection first. Run `hikari-capture-probe` to compare formats. Hikari prefers NV12, then YUY2, then MJPEG. |
| Host CPU/GPU throttling | Plug the laptop in, disable battery saver, and use a *Balanced* or *Best performance* power mode. |
| ABR stepped down to 720p30 | The network can't carry 60 fps at the minimum bitrate. See the network section below. The overlay (<kbd>F1</kbd>) shows the current mode. |

### Blocky picture / smearing after packet loss

That's the decoder recovering after a lost frame. The client sends a keyframe request and the picture heals on the next IDR. If it happens often, you're losing packets. Check `tailscale ping`, use Ethernet, or pick the **Balanced** preset so FEC and the ABR have more headroom.

### Tearing, or input feels laggy with v-sync

Cycle the display mode with <kbd>F2</kbd>:

| Mode | Behaviour | Trade-off |
| --- | --- | --- |
| **Minimum Latency** | Present immediately, tearing allowed (needs fullscreen and a tearing-capable flip-model path) | Lowest latency, visible tear lines |
| **Balanced** | V-synced, one frame queued at most | No tearing, up to one refresh of extra latency |
| **Smooth** | Small pacing buffer | Smoothest motion on jittery networks, the most latency |

With a VRR (G-SYNC / FreeSync) display in fullscreen, *Minimum Latency* usually gives no tearing *and* low latency. Run `PresentMon` (see [profiling.md](profiling.md#windows-tracing-recipes)) to confirm you're getting *Independent Flip*.

---

## Controller

### "Controller bridge not detected"

| Cause | Fix |
| --- | --- |
| Ports swapped | ESP32 **UART** port → Host PC, ESP32 **USB** port → dock. |
| Firmware not flashed | Flash `hikari-bridge-esp32s3.bin` at `0x0` ([hardware setup](hardware-setup.md#2-flash-the-bridge-firmware)). |
| Charge-only USB cable | Use a cable that carries data. Windows plays the device-connected sound and a new COM port appears in Device Manager. |
| Missing USB-serial driver | Windows 10/11 normally installs the CP210x and CH343 drivers automatically. If *Device Manager → Ports* shows an unknown device, install the vendor driver. |
| Another app holds the port | Close Arduino IDE, PuTTY, esptool-js tabs or serial monitors. The port can only be opened once. |

The host scans every 2 s and tries the last known-good port first, then every serial port with a known USB-serial VID/PID. You never pick a port manually.

### The console doesn't see a controller

| Cause | Fix |
| --- | --- |
| Bridge USB port not in the dock | Plug the ESP32 **USB** port into a dock USB-A port. `hikari-pad status` should say `Console USB   active (console awake)` when the console is awake. Test input with `hikari-pad press A` on a harmless screen. |
| Console asleep | Wake it. A suspended bus doesn't poll the controller. |
| Wired pro controller communication disabled | On the Switch, check *System Settings → Controllers and Sensors → Pro Controller Wired Communication*. If that option exists on your firmware, it must be **On**. |

### Buttons release on their own / inputs stop for a moment

That's the **failsafe**, working as designed. If the bridge receives no valid frame from the host for **250 ms**, it releases everything and centres the sticks, so a dropped connection never leaves a button held down. The host sends a keepalive every 40 ms while you hold a button, so the failsafe only fires when the host or client is really silent.

Causes: Wi-Fi dropouts on the client, a relayed (DERP) Tailscale path with spikes, or a client PC freezing. Check the overlay's *bridge failsafe* indicator and the network section below.

### Stuck input after a disconnect

This shouldn't happen. The host publishes a neutral state the moment a client leaves, and the bridge has its own 250 ms failsafe. If it does, run `hikari-pad status` and attach the logs to an issue.

### ESP32 resets or disappears when the port is opened

The DevKitC-1 auto-reset circuit wires **DTR/RTS** to the chip's EN and BOOT pins. If a program opens the port with DTR/RTS asserted, the board resets or drops into the ROM bootloader. Hikari opens the port **with DTR and RTS disabled**, so this only happens with other tools. Close them and replug the board.

### High input latency on an FTDI adapter

FTDI USB-serial chips buffer received data for up to their **latency timer**, which defaults to **16 ms**. That only affects data coming *from* the bridge (telemetry, pongs), not the controller path. To lower it anyway: *Device Manager → Ports → USB Serial Port → Properties → Port Settings → Advanced → Latency Timer (msec) → 1*. The DevKitC-1's own CP2102N or CH343 chip doesn't need this.

### My controller isn't recognised on the client

XInput controllers (Xbox, most PC pads) work everywhere. DualShock 4 and DualSense are read as raw HID. Close Steam's controller remapping or DS4Windows if they hide the device or create duplicate virtual pads. In *Settings → Controller* the active device is shown by name.

---

## Network

### Can't connect / host not found

| Cause | Fix |
| --- | --- |
| Firewall | On the **host**, check **Always reachable → Windows Firewall**; click **Allow** if it isn't green (one administrator prompt). Manual equivalent in an elevated PowerShell: `New-NetFirewallRule -DisplayName "Hikari (UDP 47800)" -Direction Inbound -Protocol UDP -LocalPort 47800 -Action Allow` |
| Network marked *Public* | Windows blocks discovery on Public networks. Set the home network to *Private*, or add the rule above. |
| Not on the same LAN | LAN discovery only finds hosts on the same subnet. Over the Internet, type the host's Tailscale name or `100.x` address. |
| Wrong pairing code | The host rejects the handshake (`BadAuth` in `host.log`). On the host click **Copy invite** and paste it on the client; the Connect window verifies it before saving. |
| Host busy | A host serves one client at a time (`Busy`). |

### Tailscale: relayed (DERP) vs. direct

On the **client**, run:

```powershell
tailscale ping <host-name-or-100.x>
```

| Output | Meaning |
| --- | --- |
| `pong from host (100.x.y.z) via 203.0.113.5:41641 in 18ms` | **Direct** peer-to-peer. 👍 |
| `pong from host (100.x.y.z) via DERP(nyc) in 74ms` | **Relayed** through a Tailscale DERP server. It works, but adds latency and has limited bandwidth. |

To get a direct path, run `tailscale netcheck` on both ends. Make sure UDP isn't blocked outbound, and check whether either network is behind a strict (symmetric) NAT or CGNAT. Enabling UPnP/NAT-PMP on the home router, or forwarding UDP 41641 to the host (a Tailscale port, not a Hikari one), usually helps. Corporate and campus networks often force DERP.

### Wi-Fi jitter

Wi-Fi adds variable delay (retries, channel contention, power-saving), and remote play shows it as stutter and input hiccups.

- **Use Ethernet on the host.** It carries the full video stream.
- On the client, prefer **5 GHz or 6 GHz** close to the access point, or Ethernet.
- Disable Wi-Fi power saving on the client: *Device Manager → Network adapters → (Wi-Fi) → Advanced → MIMO/Power Saving Mode: No SMPS*, and *Power Options → Wireless Adapter Settings → Maximum Performance*.
- Hikari tags input packets DSCP EF and media AF41 (when qWAVE is available), so WMM-capable access points can prioritise them.
- Watch the overlay: rising **queueing delay** means congestion (the ABR will lower the bitrate), while **loss without delay** means radio loss (FEC will increase).

---

## Audio

### Crackles, gaps, or audio drifting out of sync

| Cause | Fix |
| --- | --- |
| Network jitter bigger than the jitter buffer | The buffer grows automatically after underruns. Fixing the jitter (above) fixes the audio. |
| Clock drift between the capture card and the client's sound card | Normal: two crystals never run at exactly 48 kHz. The client corrects it continuously with tiny resampling adjustments. If drift keeps growing, update the audio driver and check that the client's output device runs at 48 kHz. |
| Wrong audio device on the host | The host matches the capture card's audio endpoint by PnP container ID. If the log says it fell back to a name match, check *Sound settings → Recording* and make sure the card's input is enabled. |
| No audio at all | The card's HDMI audio endpoint is disabled, or the dock is sending audio elsewhere. Check `host.log` for `Audio capture:`. |

---

## Wake

### "The Switch did not wake up"

Read the wake results in `host.log` (`Wake attempt: method 0x.. result ..`), or run `hikari-pad wake`:

| Result | Meaning | What to do |
| --- | --- | --- |
| remote wakeup → *not permitted* | The console didn't arm USB remote wakeup | Fit the [power-button fallback](remote-wake.md#the-power-button-fallback). |
| remote wakeup → *unsupported* | The bus was *detached*, not suspended (console off, or it cuts USB in sleep) | Power-button fallback. |
| power button → *unsupported* | No GPIO configured | Set `HIKARI_POWER_BUTTON_GPIO` and fit the circuit. |
| power button → *sent*, still nothing | Pulse too short, or wiring problem | Check the wiring with a multimeter. Try a longer press. |

See [remote-wake.md](remote-wake.md) for the full test procedure.

---

## Host PC

### The host stops working after a while

Most likely the laptop went to sleep, or closing the lid suspended it. Hikari keeps the system awake while it runs; if the host window's **Always reachable → Lid closed** row isn't green, click **Keep awake**. After a reboot, Hikari only starts once someone signs in to Windows (with **Start with Windows** on). See [hardware setup → Prepare the Host PC](hardware-setup.md#3-prepare-the-host-pc).

### "No usable hardware video encoder"

Update the GPU driver from Intel, NVIDIA or AMD (the drivers Windows Update installs sometimes leave out the Media Foundation encoders). `hikari-encoder-probe` lists what's available and benchmarks it. To re-run the first-start benchmark after a driver update, use **Settings → Host → Re-detect hardware**.
