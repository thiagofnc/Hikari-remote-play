# User guide

This guide covers everything from first launch to tweaking controller mappings. It assumes the hardware is wired up. If it isn't, start with [hardware-setup.md](hardware-setup.md).

## Downloads

All downloads are on the [Releases page](https://github.com/thiagofnc/Hikari-remote-play/releases/latest):

| File | What it is |
| --- | --- |
| `Hikari.exe` | The app, a single self-contained executable. No installer and no runtime needed. Use the same file on both PCs. |
| `Hikari-<ver>-win64.zip` | `Hikari.exe` + the [tools](../README.md#tools-for-tinkerers) (`tools\hikari-*.exe`) + README, LICENSE and these docs |
| `hikari-bridge-esp32s3.bin` | Bridge firmware. Flash at `0x0` ([how](hardware-setup.md#2-flash-the-bridge-firmware)). |
| `SHA256SUMS.txt` | Checksums for all of the above |

Windows SmartScreen may warn about a new, unsigned executable. Choose *More info → Run anyway* if you downloaded it from the official Releases page.

## First launch

`Hikari.exe` asks one question:

> **Is this PC connected to your Switch, or is it where you'll play?**
> **[ Host ]**  **[ Client ]**

Your answer is remembered. Launch `Hikari.exe --setup` to choose again, or use `--host` / `--client` to select a role directly.

### On the Host (the PC at home)

The Host screen runs an automatic checklist:

| | Check | Detected by |
| --- | --- | --- |
| ✅ | Controller bridge | USB VID/PID of the serial port + a handshake with the firmware |
| ✅ | Capture card | Video devices that look like HDMI capture and offer 1080p60 |
| ✅ | Audio | The capture card's own audio input (same physical USB device) |
| ✅ | Encoder | A one-time benchmark of every hardware encoder. The fastest by p95 latency wins. |

Below the checklist the Host shows:

- **Pairing code**, e.g. `K7QM-2XPA-9TRB`. Clients need it once. You can generate a new one in Settings, which un-pairs every client.
- **This PC's addresses**: LAN (e.g. `192.168.1.20`) and Tailscale (`100.x.y.z`, when its adapter is available). A configured MagicDNS name can also be entered on the client.

Close the window to keep the host running in the **tray**. Click the tray icon to reopen the status window, or right-click it to quit. Automatic startup at login is not yet exposed in the app.

### On the Client (the PC where you play)

For remote play, click **Set up Tailscale** on the Host and **Install Tailscale** on the Client. The button opens the official download page if Tailscale is missing, or the installed Tailscale app otherwise. Install it on both PCs and sign in with the same personal account. Hikari does not bundle a VPN installer or handle your Tailscale credentials. No Hikari hosting account, domain or router port forwarding is required.

The Client shows its own Tailscale IPv4 address once its adapter is available. This confirms that an address is assigned; it does not verify that both PCs belong to the same Tailscale network or that the host is reachable.

1. Pick your host:
   - **On the same network**: it appears in the list automatically (LAN discovery). When the host has Tailscale running, selecting it fills in its Tailscale address, labeled **Tailscale**. That saved destination works at home and away. Entries labeled **LAN only** require the same local network.
   - **Over the Internet**: type its Tailscale name (e.g. `living-room-pc`) or `100.x.y.z` address. Any reachable IP or hostname works.
2. Enter the **pairing code** once. Dashes and letter case don't matter, and `O`/`0`, `I`/`L`/`1` are treated the same.
3. Done. The client remembers the host and goes straight to the lobby next time.

If you previously saved a LAN address, open Connection settings and select the host again after Tailscale is set up, or enter the Tailscale address shown on the host. Leaving home does not automatically convert an existing LAN address into a Tailscale address.

Tailscale supplies encryption and attempts direct connections, using its relay fallback when necessary. Relayed connections may have higher latency. See [Tailscale setup](https://tailscale.com/docs/install/windows) and its [current personal-use plans](https://tailscale.com/pricing). The host PC must remain awake with Hikari and Tailscale running.

## Playing

The lobby shows your Switch and its state:

| State | Meaning |
| --- | --- |
| **Off** | The console isn't on the USB bus and there is no picture: off or undocked |
| **Asleep** | The console has suspended USB and there is no picture |
| **Awake** | The console is awake but shows no picture yet |
| **Ready** | Awake with a live picture |

Click **Play**, or press **A** on your controller. The host then works through these steps and shows progress for each:

1. **Waking your Switch**: only if it's off or asleep (see [remote-wake.md](remote-wake.md))
2. **Waiting for picture**: until the capture card sees a real picture
3. **Connecting controller**: until the console is reading the bridge's reports
4. **Starting stream**: until the first frame is on your screen

If a step fails, the lobby explains what to check (for example *"Capture card has no HDMI signal"*).

While streaming, the client **reconnects automatically** after a network drop. The host releases all buttons immediately when you disconnect, and the bridge releases them by itself after 250 ms if the host goes silent.

To leave the stream, press <kbd>Ctrl</kbd>+<kbd>Alt</kbd>+<kbd>Q</kbd> to go back to the lobby. The console keeps running.

## Hotkeys (client)

| Key | Action |
| --- | --- |
| <kbd>F11</kbd> or <kbd>Alt</kbd>+<kbd>Enter</kbd> | Toggle fullscreen |
| <kbd>F1</kbd> | Stats overlay |
| <kbd>F2</kbd> | Cycle display mode: Minimum Latency → Balanced → Smooth |
| <kbd>F8</kbd> | Send **HOME** to the console |
| <kbd>F9</kbd> | Send **Capture** (console screenshot) |
| <kbd>F12</kbd> | Save a screenshot of the stream on this PC |
| <kbd>Ctrl</kbd>+<kbd>Alt</kbd>+<kbd>Q</kbd> | Back to the lobby |

## Controller mapping

Open **Settings → Controller**. A Joy-Con drawing shows every Switch input. Click a button on the drawing, then press the physical button or key you want for it.

**Controllers supported**: Xbox and other XInput pads, controllers exposed through Windows.Gaming.Input, DualShock 4 and DualSense (raw HID), and the keyboard (while the client window has focus). The client polls them at 1 kHz.

**Two default layouts:**

| Layout | Xbox A / PS ✕ (bottom) | Xbox B / PS ○ (right) | Xbox X / PS □ (left) | Xbox Y / PS △ (top) |
| --- | --- | --- | --- | --- |
| **Positional** *(default)* | Switch **B** | Switch **A** | Switch **Y** | Switch **X** |
| **By label** | Switch **A** | Switch **B** | Switch **X** | Switch **Y** |

*Positional* keeps buttons where Switch muscle memory expects them ("confirm" is the right button). *By label* matches the letters printed on an Xbox pad.

**Other options:**

| Option | Default | Notes |
| --- | --- | --- |
| Analog sticks | on | Sticks drive the Switch sticks directly at full precision. |
| Stick deadzone | 0 | The console applies its own deadzone. Raise it only for a drifting stick. |
| Triggers → ZL/ZR | pressed past ~25 % travel | The HORI wired profile has digital ZL/ZR. The analog value still travels end to end for future profiles. |
| Keyboard | on | Bind any key to any Switch input, including stick directions. |

**Profiles**: save mappings as named profiles (e.g. *Default*, *Fighting*, *Keyboard*) and switch between them from the same screen. They're stored in `%LOCALAPPDATA%\Hikari\hikari.ini` as `[mapping.<name>]` sections.

## Settings

### Stream preset

How the host encodes and adapts to the network:

| Preset | Bitrate (start → max) | Reacts to queueing delay above | Best for |
| --- | --- | --- | --- |
| **Minimum Latency** | 10 → 15 Mbps | 6 ms | Fighting and rhythm games, competitive play |
| **Balanced** *(default)* | 12 → 20 Mbps | 12 ms | Almost everything |
| **Quality** | 18 → 35 Mbps | 25 ms | Slow-paced games on a fast, clean connection |

With every preset, the host lowers the bitrate when the network fills up and steps resolution down (1080p60 → 900p60 → 720p60) **before** it gives up 60 fps. 720p30 is the last resort. You can also set a **maximum bitrate**, which is useful on metered or shared connections.

### Display mode

How the client presents frames. Cycle it any time with <kbd>F2</kbd>:

| Mode | What it does |
| --- | --- |
| **Minimum Latency** | Shows each frame immediately and allows tearing. Lowest latency. Best with VRR displays. |
| **Balanced** | V-synced, at most one frame waiting. No tearing. |
| **Smooth** | Adds a small buffer that evens out network jitter. Smoothest, slightly more latency. |

### Host settings

| Setting | Notes |
| --- | --- |
| Start with Windows | Planned; no automatic-startup toggle is available yet. |
| Pairing code | Show or regenerate. Regenerating un-pairs all clients. |
| Re-detect hardware | Re-runs capture, audio and encoder detection (e.g. after a driver update or new capture card). |
| Audio codec | Opus when built with Opus support; otherwise PCM. Set `[host] audio_opus=false` in `hikari.ini` to prefer PCM. No codec selector is available yet. |

### Where settings live

`%LOCALAPPDATA%\Hikari\hikari.ini` holds detected hardware, the saved host, pairing code and controller mappings. The client Settings window edits the host and pairing code; stream presets and controller settings are accessible from the lobby. Advanced options currently require editing the INI file while Hikari is closed.
