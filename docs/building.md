# Building from source

Hikari has two independent builds:

| Part | Toolchain | Output |
| --- | --- | --- |
| Desktop app, tools, tests | CMake ≥ 3.21 + **Visual Studio 2022 Build Tools** (C++ workload) *or* **llvm-mingw** | `Hikari.exe`, `hikari-*.exe`, `hikari_tests.exe` |
| Bridge firmware | **ESP-IDF ≥ 5.3** | `hikari-bridge-esp32s3.bin` |

The desktop build targets **Windows 10/11 only** (Media Foundation, D3D11, WASAPI). CMake stops with an error on other platforms. The firmware builds anywhere ESP-IDF runs.

## Desktop

### Prerequisites

- **Visual Studio 2022** or **Build Tools for Visual Studio 2022** with the *Desktop development with C++* workload. This includes CMake, Ninja and the Windows SDK.
  *Or* [llvm-mingw](https://github.com/mstorsjo/llvm-mingw) plus CMake and Ninja on `PATH`.
- Git.

### The easy way

```powershell
git clone https://github.com/thiagofnc/Hikari-remote-play.git
cd Hikari-remote-play
.\scripts\build.ps1 -Opus -Tests
```

`build.ps1` finds Visual Studio (or llvm-mingw) and configures a Release build. `-Opus` enables compressed audio; `-Tests` runs CTest after building. Outputs default to `%LOCALAPPDATA%\Hikari-build\<toolchain>-Release\bin`, outside the checkout to avoid long paths. Use `-BuildDir` to choose another directory. Without `-Opus`, the script builds PCM audio support.

### Plain CMake

From a *Developer PowerShell for VS 2022*:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

Or with Ninja / llvm-mingw:

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

### Build options

| Option | Default | Effect |
| --- | --- | --- |
| `HIKARI_WITH_OPUS` | `ON` | Opus audio support (libopus) |
| `HIKARI_BUILD_TESTS` | `ON` | `hikari_tests` unit tests |
| `HIKARI_BUILD_TOOLS` | `ON` | probe, benchmark and test tools |

Release builds link the C/C++ runtime statically (`/MT` on MSVC, `-static` on MinGW) and use LTO, so the executables run on a clean Windows install without any redistributable.

### Targets

| Target | Directory | What |
| --- | --- | --- |
| `hikari_link` | `shared/protocol/hk_link.c` | Dependency-free C99 wired protocol, shared with the firmware |
| `hikari_shared` | `shared/` | Protocol, transport + FEC, ABR, crypto, UDP, config, logging, histograms |
| `hikari_media` | `media/` | D3D11 + Media Foundation plumbing shared by host and client |
| `hikari_host_core` | `host/src/` | Capture, GPU convert, encoder, HDMI audio, bridge driver |
| `hikari_client_ui` | `client/src/` | Input mapping and Controller Settings |
| `hikari_ui` | `ui/` | Shared Direct2D theme and widgets |
| `hikari_client` | `client/src/` | Network, input, audio, decoding and presentation |
| `Hikari` | `app/` | The single `Hikari.exe` |
| `hikari-*` | `tools/` | Probe and test tools |
| `hikari_tests` | `tests/` | Unit tests |

## Tests

`hikari_tests` is a single dependency-free executable (`tests/test_framework.h`) covering:

| File | Covers |
| --- | --- |
| `tests/test_link.cpp` | CRC-16 vector, COBS round trips with zeros, pad frames with and without IMU, decoder resync after corruption, a randomized "never crashes" decoder test, sequence wrap-around, D-pad → hat, telemetry/control round trips, axis conversion |
| `tests/test_shared.cpp` | SHA-256 vectors, HMAC (RFC 4231 case 2), pairing-code normalization, handshake derivations binding both nonces, protocol round trips and truncated-packet rejection, histogram percentiles and ageing, clock sync min-RTT selection, mailbox and bounded-queue semantics |
| `tests/test_transport.cpp` | Video transport on a clean path, FEC repairing one loss per group, two losses in a group abandoned at the deadline, reordering within the window, a whole frame missing, random-loss statistics |

Run them with `ctest` (above) or directly with `build\bin\hikari_tests.exe`.

CTest also runs the controller mapping self-test, a hidden desktop startup/rendering/resource smoke test, and `hikari_session_tests`. The session test uses a random loopback UDP port to exercise wrong-code rejection, authenticated input, video delivery, clock sync and reconnection without console hardware. Run `hikari-netsim --selftest` separately for timed network impairment checks.

These tests do not validate physical capture, hardware decoding, bridge timing or remote wake. Measure those with the actual hardware using [profiling.md](profiling.md).

### Running and packaging

Run `Hikari.exe` for the remembered role or the first-run chooser. Use `--setup` to choose again, or `--host` / `--client` to select a role directly. Settings and controller mappings live in `%LOCALAPPDATA%\Hikari\hikari.ini`.

```powershell
.\scripts\package.ps1 -BuildDir C:\hikari-build -Version 0.1.0
```

Packaging writes the standalone app, ZIP and checksums to `dist`; it does not publish a GitHub release.

### Offline screen previews

Open screens without another computer, capture card or controller bridge:

```powershell
.\scripts\preview.ps1 -Screen ready
.\scripts\preview.ps1 -Screen controller
```

Screens: `setup`, `connection`, `host`, `controller`, `ready`, `asleep`, `waking`, `failed`, `stream`. Use `-BuildDir` for a different build directory, or run `Hikari.exe --preview=ready` directly. Preview mode uses the real UI with sample state, starts no streaming/network/device services, and keeps any saved preview settings separate in `%LOCALAPPDATA%\Hikari\preview`. In the lobby, Play opens the stream-overlay preview; Ctrl+Alt+Q returns to the lobby. The stream preview has no game video or measured telemetry.

### Fuzzing the link parser

The wired-link parser runs on the bridge, so it must survive any byte stream. A libFuzzer harness lives in `firmware/esp32-s3/tests/fuzz_link.c`. With clang on Linux, macOS or WSL:

```sh
cd firmware/esp32-s3/tests
clang -g -O1 -fsanitize=fuzzer,address -I../../../shared/protocol \
      fuzz_link.c ../../../shared/protocol/hk_link.c -o fuzz_link
./fuzz_link -max_total_time=300
```

## Bridge firmware

### Prerequisites

[ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/get-started/) **5.3 or newer** (the Windows installer, VS Code extension or Linux/macOS setup all work). The TinyUSB component (`esp_tinyusb`) is fetched automatically by the IDF component manager (`main/idf_component.yml`).

### Build and flash

From an *ESP-IDF* shell:

```sh
cd firmware/esp32-s3
idf.py set-target esp32s3
idf.py build
idf.py -p COM5 flash        # the board's UART port; omit -p to auto-detect
```

`idf.py menuconfig` → **Hikari bridge** exposes the UART pins and baud rate, the failsafe timeout, the keepalive interval, the latency-marker GPIO, the optional power-button GPIO and a status LED (see [hardware-setup.md](hardware-setup.md#firmware-options-idfpy-menuconfig--hikari-bridge)).

The build produces separate bootloader, partition-table and app images. To make the single image that the browser flasher writes at `0x0`, merge them:

```sh
cd build
esptool.py --chip esp32s3 merge_bin -o hikari-bridge-esp32s3.bin @flash_args
```

### Notable `sdkconfig.defaults`

| Setting | Why |
| --- | --- |
| 240 MHz, `-O2`, power management off, `FREERTOS_HZ=1000` | Stable, low wake-up latency |
| Console and boot logs **off** (`ESP_CONSOLE_NONE`, log level none) | UART0 is the host link, so log text must never mix into frames |
| `UART_ISR_IN_IRAM` | Flash operations can't delay controller input |
| TinyUSB: 1 HID interface, no CDC/MSC, task on core 0 | Core 1 is reserved for the latency path |
| Task watchdog on (3 s) | A hung bridge reboots into the neutral state |

## Releasing

A release publishes three assets: `Hikari.exe`, `Hikari-<ver>-win64.zip` (exe + tools + docs + `scripts\flash-firmware.ps1`) and `hikari-bridge-esp32s3.bin` (merged image). The version comes from `project(Hikari VERSION …)` in the top-level `CMakeLists.txt`.
