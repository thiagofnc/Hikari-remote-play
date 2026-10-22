# Hikari

<!-- impeccable:product-schema 1 -->

## Platform

Native Windows desktop. This existing C++/Win32/Direct2D application is not a web, iOS or Android surface.

## Users

People playing their docked Nintendo Switch from another Windows PC. The owner also needs to inspect and edit screens while the second PC and console hardware are unavailable.

## Product Purpose

Stream console video and audio to a remote PC and return controller input with low, predictable latency.

## Operating Context

The host PC connects to HDMI capture hardware and an ESP32-S3 wired-controller bridge. The client connects over UDP, typically through a VPN overlay. First run selects Host or Client. The client pairs with an address and code, then enters a play lobby. Controller settings support gamepad and keyboard mappings. Offline previews provide explicitly labeled sample states without network or hardware services.

## Capabilities and Constraints

Preserve the native stack, input and streaming behavior, bounded queues, GPU processing, settings persistence and current shortcuts. Visual changes must not add latency to the streaming path. Existing automated and loopback tests do not establish hardware compatibility or measured end-to-end latency.

## Brand Commitments

Keep the Hikari name and supplied logo. The user supplied `assets/joycon-reference.png` and requested its front-facing Joy-Cons and useful shoulder views in the interactive controller diagram. Preserve its actual pixels and transparent background. The user approved matte graphite surfaces, restrained amber accents, compact Windows-style controls, and no decorative glows or dashboard cards. They explicitly rejected a generic AI-generated appearance. After reviewing the first redesign, they approved only the controller screen and requested the other menus feel like a refined SaaS product. Use the controller screen as the visual reference across the rest of the app.

## Evidence on Hand

Existing app implementation, local builds and tests, and the actual UI screenshots in `artifacts/ui-review`. The controller reference is supplied by the user. Hardware latency measurements are not available and must not be invented.

## Product Principles

- Make the connection state and next useful action immediately clear.
- Keep control mappings understandable and interactive.
- Keep visual styling out of the real-time data path.
- Clearly distinguish preview data from live device state.

## Switch 2 Controller Artwork

The user supplied four Switch 2 Joy-Con views on 2026-10-22. Use the original front artwork and an exact mirrored left/right shoulder pair, with cyan left, coral right and readable side-specific labels. Preserve the supplied source files. The shoulder base is a label-free edit of the supplied left view; reflection, palette and labels are applied by the native renderer. The current wired profile has no C input mapping.
