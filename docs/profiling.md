# Profiling and optimization

Remote play feels bad because of **stalls**, not averages. Everything in Hikari is recorded as p50/p95/p99/max, and every optimization follows the same loop: baseline, one change, measure, keep or revert.

## Built-in numbers

### Stats overlay (client, <kbd>F1</kbd>)

The overlay merges client-side histograms with `HostStats` (1 Hz) and the bridge's own telemetry. It covers:

| Line | Source | What to look for |
| --- | --- | --- |
| Video mode / bitrate / FEC K / encoder | `StreamConfig`, `HostStats` | Unexpected 720p or 30 fps means the ABR stepped down. Check the network lines. |
| Capture fps / drops | `HostStats.capture_fps_x100`, `capture_drops` | Below 60 means a USB 2.0 port, MJPEG, or a starved CPU. |
| Host: capture → send, encode p50/p95/p99 | `HostStats` | Encode p99 spikes point to GPU contention or power states on the host. |
| Net: RTT, jitter, queueing delay, loss, FEC repairs | `ReceiverReport` | Rising queueing delay before loss is congestion. Loss without delay is radio or random loss, which FEC handles. |
| Decode p95, decode queue | client | A queue above 2 means the client GPU can't keep up. |
| Display mode, present fps | client | |
| Input: sample → send, one-way, host → serial, ESP32 RX → USB / → console | client + `HostStats` | |
| Bridge: reports/s, failsafe, UART RTT | `HostStats.esp_*` | Failsafe flicker means the host lost the client for ≥ 250 ms. |

### Logs

`%LOCALAPPDATA%\Hikari\logs\<role>.log`, with the previous run kept as `<role>.prev.log`. The logs record capture modes, encoder benchmark results, bridge connection, console USB state changes, wake attempts (`method 0x.. result ..`) and HDMI picture detection.

### Tools

| Tool | Use it to |
| --- | --- |
| `hikari-capture-probe` | Compare formats (NV12 / YUY2 / MJPEG) and modes on a capture card: real fps, frame-interval p50/p99, driver delay. |
| `hikari-encoder-probe` | Benchmark every hardware encoder on the host. It uses the same `benchmark_encoder()` that picks the encoder on first run. |
| `hikari-netsim` | Put a controlled bad network between client and host (delay, jitter, loss, bandwidth cap) to test FEC and the ABR reproducibly. |
| `hikari-latency-test` | Drive the ESP32 path at 125 / 250 / 500 / 1000 Hz and collect `rx_to_submit`, `rx_to_consumed` and `poll_interval` from the bridge telemetry. |
| `hikari-pad` | Drive the console directly from the host CLI (`status`, `wake`, button presses). Useful to isolate the bridge from the network. |

Each tool prints its options with `--help`, and most can save results with `--json FILE` / `--csv FILE` for before/after comparisons. Some starting points:

```powershell
hikari-capture-probe --seconds 6                    # every plausible 1080p60/720p60 mode on the card
hikari-encoder-probe --codec all --seconds 5        # all GPUs, H.264 + HEVC
hikari-latency-test --rates 125,250,500,1000 --json bridge.json
hikari-netsim --target <host>:47800 --profile wifi  # then point the client at this PC, port 47801
hikari-netsim --target <host>:47800 --delay 20 --jitter 5 --loss 1 --rate 15000
```

Run `hikari-latency-test` with the console awake on a harmless screen (*System Settings → Controllers → Test Input Devices*), because it moves the sticks and toggles buttons.

## Windows tracing recipes

### WPR / WPA: CPU scheduling, DPCs, thread wake-ups

```powershell
# Elevated prompt. Reproduce the problem for 20-30 s while recording.
wpr -start GeneralProfile -start CPU -start GPU -filemode
# ... play ...
wpr -stop hikari.etl
wpa hikari.etl
```

Hikari names its threads (`SetThreadDescription`), so in WPA they appear by name (`hikari-bridge`, …) under **CPU Usage (Precise)**. Things to check:

- **Ready time** of the input, network and video threads. Long ready times mean something is starving them.
- **DPC/ISR duration** on the host. A misbehaving USB, Wi-Fi or GPU driver can stall the capture or network path for milliseconds.
- Whether threads land on **efficiency cores** on hybrid CPUs. Hikari opts out of EcoQoS, so this should not happen.

### GPUView: GPU queueing on host or client

GPUView ships with the Windows Performance Toolkit (`C:\Program Files (x86)\Windows Kits\10\Windows Performance Toolkit\gpuview\`).

```powershell
cd "C:\Program Files (x86)\Windows Kits\10\Windows Performance Toolkit\gpuview"
.\log.cmd          # start
# ... reproduce ...
.\log.cmd          # stop -> Merged.etl
.\GPUView.exe Merged.etl
```

Look at the video engine and 3D queues: the encoder work (host) or the decode and present work (client) should start as soon as it's submitted, not wait behind other packets.

### PresentMon: client presentation

```powershell
PresentMon.exe --process_name Hikari.exe --output_file present.csv
```

(PresentMon 2.x syntax.) Check `PresentMode`. You want `Hardware: Independent Flip` or `Hardware Composed: Independent Flip` in fullscreen. `Composed: Flip` means DWM composition, which adds a frame. Also check `MsUntilDisplayed` and `MsBetweenPresents` for each display mode.

## Glass-to-glass with a camera

Software timestamps can't see the console's frame time or your display's processing, so a camera has the final word.

### Equipment

- A phone or camera that records at **240 fps** or more (≈ 4.2 ms per frame. Average over many samples).
- An **LED + 330 Ω resistor** on the ESP32 **latency-marker GPIO** (default **GPIO 4**, `HIKARI_LATENCY_MARKER_GPIO`). It goes high for 2 ms when a pad state carrying the latency-mark flag is handed to the USB controller, and the bridge reports the same moment as `EVENT LATENCY_MARK` with the pad `seq`.
- Optional: a photodiode + logic analyser for sub-millisecond precision. Put one channel on the client screen and one on GPIO 4.

```
ESP32 GPIO 4 ──[ 330 Ω ]──►|── GND        (LED, anode to the resistor)
```

To generate marker presses without the client, run this on the host:

```powershell
hikari-latency-test --marker --seconds 20 --marker-button A
```

Once per second it presses the button with the latency-mark flag set. The LED lights at the exact moment the bridge hands that report to the console.

### Recipes

**Input path (thumb → console)**: film the controller and the marker LED. Frames from button visibly pressed → LED on = client sample + network + host + UART + ESP32 hand-off.

**Button-to-photon (the number players feel)**: film the controller and the *client* screen in a game scene where a button causes an instant on-screen change (opening a menu, a jump). Count frames from button pressed → change visible on the client display.

**Video path**: film the LED and the client screen. LED on → on-screen reaction = console reaction time + capture + encode + network + decode + display. To isolate the console's own reaction time, film the LED and a TV connected through an HDMI splitter in front of the capture card.

Repeat each measurement 20+ times and report the median and the spread. A single sample proves nothing.

## The optimization method

1. **Baseline.** Record the overlay numbers, the relevant tool output and, if it's an end-to-end change, a camera run. Write down the setup (see the template).
2. **One change.** One setting, one patch. Never two at once.
3. **Measure the same way**, for the same duration, with the same scene.
4. **Keep or revert.** Keep the change only if p95/p99 improve (or stay the same while something else improves) and nothing regresses. "Average went down but p99 went up" is a **revert**.
5. **Record it** with the template below, so the next person doesn't redo the experiment.

## Regression record template

Copy this into the PR description or an issue:

```markdown
### Change
<!-- one sentence: what changed and why it should help -->

### Setup
- Hikari version / commit:
- Host: CPU / GPU / encoder / capture card + format + USB port
- Client: CPU / GPU / display Hz / display mode
- Network: LAN / Tailscale direct / DERP; wired / Wi-Fi; RTT
- Preset / scene / duration

### Results (µs or ms, state which)
| Metric | Before p50 | Before p95 | Before p99 | After p50 | After p95 | After p99 |
| --- | --- | --- | --- | --- | --- | --- |
| capture → encoded | | | | | | |
| receive → decoded | | | | | | |
| decoded → display | | | | | | |
| input one-way | | | | | | |
| ESP32 RX → console read | | | | | | |
| glass-to-glass (camera, n=…) | | | | | | |

### Decision
- [ ] Keep
- [ ] Revert
Reason:
```
