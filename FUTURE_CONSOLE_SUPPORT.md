# Future Xbox and PlayStation support

**This document is for future planning, not work to implement right now.**
It records possible expansion beyond Nintendo Switch after Hikari has a
working, tested end-to-end implementation for its initial console. It is
not a claim of current compatibility, a committed roadmap, or a hardware
shopping list. No console-support implementation is included with this note.

## Scope and feasibility

Potential targets are Xbox One, Xbox Series X/S, PS4, and PS5. Xbox 360 would
require a separate investigation and implementation.

Hikari's HDMI capture, audio capture, encoding, and network transport are
largely independent of the source console. Controller input is the main
console-specific obstacle: the console must recognize the controller protocol
and accept its authentication. Changing USB identifiers or button names alone
is insufficient.

Basic buttons, both sticks, and analog triggers should be the first milestone.
Touch, motion, rumble, advanced haptics, controller audio, microphone input,
and remote wake need separate implementation and validation.

## Software changes

- Make input mappings console-neutral. Replace Switch-specific target inputs
  with common button positions and capabilities, while displaying each
  console's labels and special buttons.
- Expand `firmware/esp32-s3/main/console_profile.h` into a controller backend
  capable of handling initialization, authentication, console output,
  reconnects, and protocols beyond the current HID assumptions.
- Remove the hardcoded HORI profile from USB initialization and bridge
  identification. Select a backend explicitly and report its capabilities
  and authentication state to the host.
- Preserve analog trigger values through the selected backend. The shared
  pad state already carries these values, but the shipped HORI profile emits
  digital trigger buttons. Keep digital-trigger conversion specific to
  consoles that need it.
- Add PlayStation touchpad click, touch coordinates, and gestures where
  required. Motion fields already exist in the shared protocol, but fields
  alone do not provide working controller sampling or console reporting.
- Add a console-to-client feedback path for rumble and supported controller
  effects. The current HORI profile ignores output reports. DualSense haptics
  and adaptive triggers would require dedicated handling and a capable
  controller at the remote client.
- Version and negotiate protocol extensions so unsupported features are
  visible and older clients or bridges fail clearly.
- Implement console-specific wake, controller/account assignment, and setup
  behavior. Do not assume the current USB wake and Home-button sequence works
  on another console.
- Treat party-chat microphone input and controller speaker/headset audio as
  separate audio features; HDMI game-audio capture does not provide them.

## Xbox One and Series X/S

These targets need an Xbox-compatible controller protocol and authentication.
PC XInput compatibility does not establish Xbox console compatibility.

GP2040-CE provides an existing reference for Xbox One/Series support using
a USB host port and a compatible authentication dongle. Its documented
implementation does not accept ordinary Xbox Wireless, Elite, or other
licensed Xbox One controllers as authentication devices. Do not assume an
arbitrary genuine controller can be plugged into a passthrough port.

Possible hardware routes are a custom bridge with authentication support,
a validated external controller converter, or electrical input injection
into a genuine Xbox controller. Basic support is a moderate-to-large extension,
depending on how much existing firmware can be reused.

## PS4

PS4 controller authentication must be handled, including recurring requests.
An incomplete implementation can work initially and then stop after about
eight minutes. GP2040-CE documents compatible USB authentication passthrough
as one approach.

A PS4 backend needs analog triggers, PS/Options/Share mappings, touchpad
support, initialization, and reconnect handling. A touchpad-click binding is
enough for some games; other games require actual touch positions or gestures.

Hardware options are a custom bridge with an independent USB host interface
and compatible authentication device, a tested converter, or a modified
DualShock 4. Authentication devices must be selected for the particular
implementation rather than assumed interchangeable.

## PS5: separate compatibility milestones

| Milestone | Important limitation |
| --- | --- |
| Supported PS4 games running on PS5 | PS4-compatible controllers can cover this use case; it does not prove native PS5 game support. |
| Native PS5 games supporting categorized controllers | A compatible fightstick or other specialty-controller route can work only where the game supports that category. |
| General native PS5 gamepad support | Requires a separately validated solution accepted for those games. |

GP2040-CE's documented PS5 passthrough route uses a categorized controller,
such as a licensed PS4 fightstick. It is not general PS5 gamepad support.

Broader commercial converter routes deserve investigation. For example,
Brook advertises Wingman FGC 2 support for all PS5 games. That is a vendor
claim and a prototype candidate, not proof that Hikari's emulated controller
will work with it. Validate the exact input profile, converter firmware,
console firmware, game coverage, both sticks, analog triggers, special
buttons, and any lost features before selecting hardware.

Electrical input injection into a genuine DualSense is another possible
route for basic controls, but does not automatically provide remote touch,
motion, haptics, or controller audio. Full DualSense functionality is a
separate project beyond basic gamepad support.

## Bridge hardware

The current topology is:

```text
Host PC -- USB-to-UART -- ESP32-S3 -- USB device connection -- Console
```

Authentication passthrough would generally require:

```text
Host PC -- UART -- Bridge -- USB device connection -- Console
                     |
                     +-- USB host connection -- Authentication device
```

The ESP32-S3's USB OTG controller cannot act as USB host and USB device
simultaneously. The development board's PC-side USB-to-UART connection is
not a spare programmable USB host port. Adding a socket, hub, or external
PHY alone does not create another OTG controller.

Possible designs include:

| Route | Hardware and engineering implications |
| --- | --- |
| Custom authentication bridge | Add an independent USB host controller, a second microcontroller, or a different platform with suitable host/device resources; implement the required stacks and authentication handling. |
| External converter | Keep Hikari as a controller emulator and use a converter that explicitly accepts that emulation profile; validate compatibility, added latency, reconnects, and firmware-update behavior. |
| Modified genuine controller | Keep the genuine controller connected to the console and electrically drive its inputs; requires controller-specific button-switching and analog circuits, wiring, calibration, and usually soldering. |

For a custom host port, budget for appropriate USB power delivery, current
limiting, protection, connectors, and safe separation of power sources.
Direct GPIO wiring is not a universal method for driving controller buttons,
sticks, or triggers; the circuitry must match the particular controller PCB.
A genuine controller remains dedicated to the installation in that route.

## Capture hardware and media

- Start with 1080p60 SDR. Verify the capture card's actual capture modes,
  audio endpoint, format conversion, and latency on each console.
- PlayStation gameplay capture requires disabling HDCP. Protected media
  applications are outside the gameplay-capture target.
- Capture capability and HDMI passthrough capability are separate. Preserving
  4K120, HDR, or VRR on a local display requires hardware that supports the
  desired signal through the complete HDMI path, even if the remote stream
  remains 1080p60.
- Hikari's current NV12 processing/encoding path is not a complete HDR
  streaming implementation. Proper HDR would require suitable pixel formats,
  color handling, codec configuration, metadata, and client display support.
- Reuse the Windows host and existing capture card where validated. A newer
  console alone does not imply that a more expensive capture card is needed.

## Alternative: native Remote Play backend

Evaluate native Remote Play before committing to PlayStation bridge hardware.
Sony supports PS4/PS5 Remote Play, and chiaki-ng demonstrates an open-source
client implementation. This route could avoid the physical controller bridge
and, if using the console's stream, the capture card.

This is a different software architecture requiring pairing, account/session
handling, protocol integration, wake behavior, feature validation, and latency
measurement. Relaying and re-encoding a console stream could add overhead.
Review dependencies and licenses before reuse; chiaki-ng is AGPLv3. The
existence of a standalone client does not make its integration automatic.

## Future validation and suggested order

1. Complete and measure one reliable end-to-end path for the initial console.
2. Make controller backends and capability reporting replaceable.
3. Prototype one exact hardware combination for Xbox or PS4 before designing
   the product setup flow around it.
4. Test basic controls, full analog ranges, special buttons, sustained play
   beyond authentication timeouts, disconnect/reconnect, and neutral-state
   failsafes. Measure actual input delivery and capture latency.
5. Test sleep/wake and account assignment separately; authentication or an
   active USB connection does not establish reliable remote wake.
6. Validate PS5 game categories explicitly, then investigate broad native PS5
   support and advanced controller features as later milestones.

Do not advertise a console as supported based only on successful USB
enumeration, a working menu, one game, or a short authentication test.

## References

External compatibility information was reviewed on October 21, 2026. Recheck
documentation, firmware support, and exact hardware combinations when this
future work begins.

- [GP2040-CE console compatibility and authentication requirements](https://gp2040-ce.info/faq/faq-console-compatibility/)
- [Espressif FAQ, including ESP32-S3 USB host/device limitations](https://documentation.espressif.com/api/resource/path/docs%2Fprojects%2Fesp-faq%2Fen%2Flatest%2Fesp-faq-en-master.pdf)
- [Sony: PS4 games and controller compatibility on PS5](https://www.playstation.com/en-ph/support/games/ps5-backward-compatibility-games/)
- [Brook: Wingman FGC 2 vendor specifications](https://www.brookaccessory.com/products/wingmanfgc2/index.html)
- [Sony: PS5 FAQ, including external HDMI capture and HDCP](https://blog.playstation.com/2020/11/09/ps5-the-ultimate-faq/comment-page-45/)
- [Sony: PS4 audio output and HDCP restrictions](https://www.playstation.com/en-us/support/hardware/ps4-change-audio-output/)
- [Sony: PS5 Remote Play on Windows and rest-mode settings](https://remoteplay.dl.playstation.net/remoteplay/lang/gb/ps5_win.html)
- [chiaki-ng source and licensing](https://github.com/streetpea/chiaki-ng)
