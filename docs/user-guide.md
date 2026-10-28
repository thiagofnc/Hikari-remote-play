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

Your answer is remembered. Click **Change role** at the top of either the Host or Client screen to choose again. Closing the chooser keeps the current role; choosing the other role stops the current session and opens that role. Saved pairing and controller settings are preserved. You can change roles before configuring a client connection. `Hikari.exe --setup` also opens the chooser, and `--host` / `--client` selects a role directly.

### On the Host (the PC at home)

The Host screen runs an automatic checklist:

| | Check | Detected by |
| --- | --- | --- |
| ✅ | Controller bridge | USB VID/PID of the serial port + a handshake with the firmware |
| ✅ | Capture card | Video devices that look like HDMI capture and offer 1080p60 |
| ✅ | Audio | The capture card's own audio input (same physical USB device) |
| ✅ | Encoder | A one-time benchmark of every hardware encoder. The fastest by p95 latency wins. |

Next to the checklist the Host shows:

- **Invite**, e.g. `hikari://living-room.tail1234.ts.net/K7QM-2XPA-9TRB?ip=100.101.102.103`. It holds everything a client needs: the host's Tailscale name, its Tailscale IP as a fallback, and the pairing code. Click **Copy invite** (also in the tray menu) and send it to yourself.
- **Pairing code**, e.g. `K7QM-2XPA-9TRB`, if you prefer to type it. **New pairing code** takes effect immediately; other PCs then need the new invite.
- **This PC on Tailscale**: its MagicDNS name and `100.x.y.z` address, plus the LAN address for play at home.

**Always reachable** keeps the Switch available while you're away:

| Row | What Hikari does |
| --- | --- |
| Windows Firewall | On first start Hikari asks once (a single administrator prompt) to add an inbound rule for UDP 47800. It also removes the *block* rules Windows creates when its "Allow access?" prompt is dismissed. **Allow** repeats this. |
| Start with Windows | On by default: Hikari starts minimised to the tray when you sign in, so a reboot doesn't take your Switch offline. Windows still needs someone to sign in after a restart; enable automatic sign-in if the host PC restarts unattended. |
| Lid closed | Laptops only. If closing the lid would sleep the PC, **Keep awake** sets the lid action to *Do nothing* (asks for administrator rights only if Windows requires them). |

Close the window to keep the host running in the **tray**. Click the tray icon to reopen the status window, or right-click it to copy the invite or quit.

### On the Client (the PC where you play)

For remote play, click **Set up Tailscale** on the Host and **Install Tailscale** on the Client. The button opens the official download page if Tailscale is missing, or the installed Tailscale app otherwise. Install it on both PCs and sign in with the same personal account. Hikari does not bundle a VPN installer or handle your Tailscale credentials. No Hikari hosting account, domain or router port forwarding is required.

The Client shows its own Tailscale IPv4 address once its adapter is available. This confirms that an address is assigned; it does not verify that both PCs belong to the same Tailscale network or that the host is reachable.

1. Paste the **invite** from the host into **Connect to your host**. The host address and pairing code fill in automatically. (Or type them: the host's Tailscale name or `100.x.y.z` address, and the pairing code. Dashes and letter case don't matter, and `O`/`0`, `I`/`L`/`1` are treated the same.)
2. Click **Save and connect**. Hikari contacts the host and verifies the pairing code *before* saving, without interrupting anyone who is playing. If something is wrong you get a specific answer: wrong code, different Hikari version, address not found, or no answer (Hikari not running, Tailscale not signed in, or the host's firewall). If the host PC is simply off right now, **Save anyway** keeps the details.
3. Done. The client remembers the host and goes straight to the lobby next time. If the Tailscale name ever stops resolving, the client automatically tries the IP from the invite.

The same invite works at home and away. A LAN address also works for local-only play.

Only one PC plays at a time. If another PC connects, the one that was playing returns to the lobby with **Another device took over**, and stays there until you press **Take over**.

While connected, the lobby and stats overlay (F1) show the route Tailscale chose: **direct**, or **relayed** through a Tailscale DERP server. Relayed works, but adds latency; see [troubleshooting](troubleshooting.md#tailscale-relayed-derp-vs-direct).

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
| <kbd>F3</kbd> | Fill the whole screen (stretch) or keep the 16:9 shape |
| <kbd>F8</kbd> | Send **HOME** to the console |
| <kbd>F9</kbd> | Send **Capture** (console screenshot) |
| <kbd>F12</kbd> | Save a screenshot of the stream on this PC |
| <kbd>Ctrl</kbd>+<kbd>Alt</kbd>+<kbd>Q</kbd> | Back to the lobby |
| Click the picture / <kbd>Esc</kbd> | Capture / release the mouse for mouse aim |

## Controller mapping

Open **Settings → Controller**. A Joy-Con drawing shows every Switch input. Click a button on the drawing, then press the physical button or key you want for it.

**Controllers supported**: Xbox and other XInput pads, controllers exposed through Windows.Gaming.Input, DualShock 4 and DualSense (raw HID), and keyboard & mouse (while the client window has focus). The client polls them at 1 kHz.

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
| Keyboard & mouse | on | Bind any key, mouse button or wheel direction to any Switch input, including stick directions. |
| Mouse aim | on | Click the stream to capture the mouse; moving it tilts the right stick. <kbd>Esc</kbd> releases it. |
| Mouse sensitivity | 1.0× | How far the right stick tilts for a given mouse speed. **Invert mouse Y** flips vertical aim. |

Mouse aim drives the right stick, so the console's own stick acceleration and top turn speed still apply. Slow movements always tilt the stick past the console deadzone, so fine aiming still registers. Very fast flicks reach full tilt, which is the stick's top turn speed, so they turn less than the same flick would on a PC.

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
| Start with Windows | Toggle in **Always reachable**. On by default; starts minimised to the tray. |
| Windows Firewall / Lid closed | Shown in **Always reachable** with a one-click fix when needed. |
| Pairing code | Show, copy as an invite, or regenerate. A new code applies immediately and un-pairs all clients. |
| Re-detect hardware | Re-runs capture, audio and encoder detection (e.g. after a driver update or new capture card). |
| Audio codec | Opus when built with Opus support; otherwise PCM. Set `[host] audio_opus=false` in `hikari.ini` to prefer PCM. No codec selector is available yet. |

### Where settings live

`%LOCALAPPDATA%\Hikari\hikari.ini` holds detected hardware, the saved host, pairing code and controller mappings. The client Settings window edits the host and pairing code; stream presets and controller settings are accessible from the lobby. Advanced options currently require editing the INI file while Hikari is closed.
