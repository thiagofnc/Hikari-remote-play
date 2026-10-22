---
name: Hikari
description: Premium native Windows remote-play workspaces: near-black canvas, raised hairline cards and lit amber actions.
colors:
  graphite-bg: "#0E0F11"
  graphite-panel: "#16171A"
  control: "#1E1F23"
  control-hover: "#26282D"
  border: "#2B2D33"
  border-strong: "#3A3D44"
  text: "#F3F4F6"
  text-secondary: "#A8ACB4"
  text-muted: "#80858E"
  amber: "#FBBB04"
  amber-hover: "#FFCB33"
  amber-pressed: "#E2A600"
  on-amber: "#1A1400"
  good: "#3DD68C"
  warning: "#F5A524"
  error: "#FF5C5C"
  joycon-left: "#00B8D9"
  joycon-right: "#FF5F52"
typography:
  headline:
    fontFamily: "Segoe UI Variable Text, Segoe UI"
    fontSize: "22px"
    fontWeight: 600
  title:
    fontFamily: "Segoe UI Variable Display, Segoe UI"
    fontSize: "22px"
    fontWeight: 600
  body:
    fontFamily: "Segoe UI Variable Text, Segoe UI"
    fontSize: "13px"
    fontWeight: 400
  controller-body:
    fontFamily: "Segoe UI Variable Text, Segoe UI"
    fontSize: "13.5px"
    fontWeight: 400
  button:
    fontFamily: "Segoe UI Variable Text, Segoe UI"
    fontSize: "14px"
    fontWeight: 600
  label:
    fontFamily: "Segoe UI Variable Text, Segoe UI"
    fontSize: "12px"
    fontWeight: 400
rounded:
  button: "8px"
  card: "12px"
  binding: "7px"
  controller-control: "8px"
  field: "8px"
  controller-panel: "12px"
  pill: "12px"
spacing:
  tight: "8px"
  control-gap: "16px"
  panel-inset: "18px"
  controller-margin: "24px"
  page-margin: "24px"
  pane-content-inset: "22px"
components:
  button-primary:
    backgroundColor: "{colors.amber}"
    textColor: "{colors.on-amber}"
    rounded: "{rounded.button}"
    typography: "{typography.button}"
  button-primary-hover:
    backgroundColor: "{colors.amber-hover}"
  button-secondary:
    backgroundColor: "{colors.control}"
    textColor: "{colors.text}"
    rounded: "{rounded.button}"
    typography: "{typography.button}"
  button-secondary-hover:
    backgroundColor: "{colors.control-hover}"
  button-ghost:
    backgroundColor: "transparent"
    textColor: "{colors.text-secondary}"
    rounded: "{rounded.button}"
  connection-field:
    backgroundColor: "{colors.control}"
    textColor: "{colors.text}"
    rounded: "{rounded.field}"
    height: "40px"
  binding:
    backgroundColor: "{colors.control}"
    textColor: "{colors.text}"
    rounded: "{rounded.binding}"
    typography: "{typography.controller-body}"
  controller-panel:
    backgroundColor: "{colors.graphite-panel}"
    rounded: "{rounded.controller-panel}"
    padding: "18px"
---

# Design System: Hikari

## Overview

**Creative North Star: "A precise remote-play utility"**

**Premium SaaS pass (2026-10-22, branch `ui/premium-saas`).** The user asked for a modern, very premium SaaS feel and chose the Linear-style refined direction: keep graphite and amber, deepen the canvas to near-black, raise working panes as hairline-bordered cards with a soft offset shadow, light primary actions with a vertical amber gradient and inner top highlight, and use tinted status chips, drawn status icons and keycaps. This supersedes the earlier "no cards" rule for working panes only; decorative glows, glass and breathing actions remain banned. Supplied assets are untouched. Before/after evidence for every offline preview: `artifacts/ui-review/premium-saas/{before,after}/*.png` and `artifacts/ui-review/premium-saas/index.html`.

Hikari is a native C++ Windows application rendered with Win32, Direct2D and DirectWrite. Its matte graphite surfaces, restrained Hikari amber and compact Windows controls put connection state and useful actions first. The user selected the controller settings screen as the reference and requested a refined SaaS product feel for the other menus. The built result carries that same measured title/subtitle hierarchy, compact split workspaces and consistent actions across setup, connection, host and lobby while retaining the native Windows stack.

The supplied Hikari logo and original transparent Joy-Con image carry the product identity. The controller diagram is an interactive mapping surface: selecting a real control identifies its binding, while precise amber overlays show capture and live input. Preview screens explicitly identify sample data.

**Key Characteristics:**

- Controller-led workspace hierarchy, compact native desktop controls and left-aligned information.
- Flat graphite surfaces, thin boundaries and restrained amber state feedback.
- Original supplied artwork with preserved transparent pixels.
- Motion attached to progress, capture, input feedback and transient status.

Evidence: `ui/hikari/ui_kit.h`, `ui/ui_kit.cpp`, `client/src/controller_settings.cpp`, `client/src/joycon_art.cpp`, `client/src/client_app.cpp`, `app/main.cpp`; console extension reviewed against `artifacts/ui-review/console/{switch,switch2,compact,failed,waking}.png`; earlier menu review captures in `artifacts/ui-review/after` (01?06 and 08 for refreshed menus, 13 for compact lobby, 09 for the unchanged controller reference). Frontmatter `px` values represent logical Windows DIPs at 96 DPI, not physical pixels or a CSS implementation. DirectWrite keeps its native line metrics; no fixed line-height or letter-spacing scale is established.

## Colors

The primary palette is warm amber against neutral graphite; device and semantic colors remain functional exceptions.

### Primary

- **Hikari amber:** primary actions, selected preset radio marker, keyboard focus, capture progress and diagram overlays.
- **Amber hover / on-amber:** brighter shared-button hover and dark action text.

### Secondary

- **Good, warning and error:** connection dots, device readiness and actionable failures; pair color with text.
- **Joy-Con left / right:** cyan and coral hardware identity in the lobby illustration. The supplied controller atlas keeps its own original colors.

### Neutral

- **Graphite background / panel:** window canvas and functional settings panes.
- **Control / control hover:** buttons, dropdowns and binding targets.
- **Border:** thin structural separators and control outlines.
- **Text / secondary / muted:** main information, supporting information and quiet hints.

**The Amber Has a Job Rule.** Use amber for an action, selection, focus or actual input state. Do not add amber background ornament.

## Typography

**Body Font:** Segoe UI Variable Text, with Segoe UI fallback.
**Controller Title Font:** Segoe UI Variable Display, with Segoe UI fallback.

DirectWrite uses a single Windows text family relationship. Shared UI text is regular, medium or semibold; the controller's diagram button labels use bold. This native utility intentionally uses the Windows system face; it is not a general endorsement of system display typography for other visual worlds.

### Hierarchy

- **Headline:** setup, connection, host and lobby page titles at 22 DIP semibold in the shared Text family.
- **Section title:** pane titles at 15 DIP semibold; setup roles and lobby device title at 18 DIP semibold.
- **Title:** controller settings at 22 DIP semibold; controller dialog title at 17 DIP.
- **Body:** shared page subtitles and main information at 13 DIP; shared button labels at 14 DIP; controller mappings at 13.5 DIP.
- **Label:** shared hints and controller keys at 12 DIP; shared status pill at 12.5 DIP medium.

Non-wrapping text trims with a character ellipsis. Messages that need multiple lines explicitly request wrapping. Controller toolbar and group captions currently use 11 DIP semibold uppercase; they are functional field/group labels, not a transferable eyebrow or display style.

## Layout

Dimensions are logical DIPs, scaled through the shared painter or a DPI-aware controller render target. Retain ordinary Windows frames and resize behavior. There is no web breakpoint or mobile layout.

Setup, connection and host client sizes are 680 by 400, 600 by 540 and 900 by 688 DIP respectively. Their headers pair the supplied 24 DIP Hikari mark with a 22 DIP title and 13 DIP supporting line, followed by a thin divider at 100 DIP. Setup uses two full-width role rows with 17 DIP names, a stacked-server icon for Host, a display icon for Client, and directional arrows. Host uses a continuous split workspace with a thin vertical divider instead of enclosing panels. Connection uses a single-column address and pairing-code form without nearby discovery, keeps Tailscale setup below the fields and anchors Cancel and Save and connect below a footer rule. Host device rows use a 78 DIP pitch and align readiness at the right. Footer actions remain separate from device and pairing information.

The client lobby opens at 1120 by 700 physical window units scaled by DPI, with the existing 860 by 600 compact preview. It uses a 24 DIP margin, supplied logo, 22 DIP title and compact upper-right actions. Change role is a quiet text action; Controller and Connection retain their existing behavior. Only the console session has an inset graphite surface, with 8 DIP corners and a 220 DIP height. Stream preferences sit directly on the window canvas; a thin vertical divider separates the host column. Four readiness rows have a 48 DIP pitch and display their status in a right-aligned column. Play remains 164 by 38 DIP. The left workspace uses 62% of the available width. Preset rows show descriptions when their pitch is at least 48 DIP and collapse to one line below that. The footer identifies offline sample data.

The session illustration reserves 44% of its inner row width, clamped to 174-300 DIP, at the right of the session pane. Text and status messages end 20 DIP before the artwork; controls remain in the left action area. Progress shows only the current step and spinner when the text area is narrower than 330 DIP, retaining its message and Cancel action. Larger illustrations use the same existing pane rather than changing the workspace hierarchy.

Controller settings default to 1200 by 760 DIP, with a minimum of 1040 by 700 DIP. Its margin is 24 DIP, toolbar controls are 36 DIP tall, and the content begins at 148 DIP. The binding pane width is clamped to 57% of the window between 600 and 800 DIP; a 16 DIP gap separates the artwork pane. The three mapping columns have 22 DIP gaps, adaptive row pitch clamped to 24–40 DIP and targets up to 32 DIP tall. The footer stays anchored with 38 DIP action buttons. These are native minimum-size constraints, not responsive stacking instructions.

**The Shared Rhythm Rule.** Align controls to their toolbar, pane and footer baselines; expand the working area without adding decorative dashboard modules.

## Elevation & Depth

Three tonal levels carry depth: the `#0E0F11` canvas, `#16171A` raised cards and footer bars, and `#1E1F23` controls and fields. Working panes are cards (`Painter::card`, mirrored by `SettingsWindow::card`): 12 DIP corners, a 6.5% white hairline, a 3.5% white top highlight and a soft downward shadow built from six stacked translucent layers (each expanded by 1.6 DIP and offset 2.7–6.2 DIP; strength 0.32). Window footers are flat `#16171A` bands with a hairline top rule; the lobby uses a 36 DIP status bar. The lobby session and stream preference cards keep their original contents and controls; only the surface changed. The console illustration is inset 24 DIP from the card edge. Popups and dialogs use the same shadow at higher strength; dialogs dim the canvas with 55% black.

Windows 11 frames match the canvas: `ui::style_window` sets dark mode, rounded corners, a `#0E0F11` caption and a `#2B2D33` border through DWM.

Streaming telemetry and toasts use translucent dark overlays with a hairline, to keep text readable over moving video; these are operational overlays rather than a glass material system.

## Shapes

Shared buttons and connection fields use 8 DIP corners; cards 12 DIP; status chips are fully rounded. Setup groups its two roles in one card with a hairline divider; connection and host group related content in cards above a footer bar. The lobby console session uses 8 DIP corners. Controller buttons/dropdowns use 8 DIP, bindings 7 DIP and functional panes 12 DIP. Controller popups use 10 DIP and dialogs 14 DIP. Small status badges and switch tracks retain their task-specific rounded silhouettes. Standard control boundaries are 1 DIP. Do not impose one global radius on the two existing native renderers.

## Components

### Buttons

The shared hit list includes only enabled actions. Lobby focus persists across repaint, advances with Tab/Shift+Tab and activates with Enter or Space. When session state replaces Play with Retry, connection setup or Cancel, focus is reconciled to an available action. Native Alt+Enter fullscreen behavior remains available.

Buttons keep the original flat graphite treatment at the user's request: primary amber `#FBBB04` (hover `#FFCB33`) with dark text; secondary `#282A2F` (hover `#303339`) with a `#41444B` outline; ghost gains a 6% white hover fill; danger is a red tint. Shared buttons use 6 DIP corners and 14 DIP semibold labels, inset 1 DIP while pressed and show a 2 DIP amber focus stroke 3 DIP outside. No gradients, highlights or button shadows. Controller settings keep their local 8 DIP variant with the same flat colours.

### In-game overlay

Nothing is drawn over the game by default, and the mouse is not needed: it may be a console input. The stream menu opens with Ctrl+Alt+M or by holding Minus and Plus for one second, and opening it pauses input to the console (every held button is released through `GamepadInput::set_sending(false)`). It is a centred 420 DIP panel over a 50% dim: Resume (Esc), Show or hide stats (F1), Controller settings, Full screen (F11) and End stream (Ctrl+Alt+Q). Arrows or D-pad move, Enter or A selects, Esc or B backs out; the selected row uses the secondary button surface. Ctrl+Alt+Q, or End stream, asks "End the stream?" with Keep playing and End stream (focused). Ending leaves the console on and the host connected; the lobby then says "Stream ended. Your console is still on." and its primary action reads Resume (A also works). F1 still toggles stats directly. The stats panel shows four headline figures, a 0 to 100 ms glass-to-glass trace and two columns of labelled detail; offline previews mark sample values "Sample data". Toasts sit at the top centre.

### Console model

Controller settings has a Switch / Switch 2 segmented toggle in the artwork card header. It swaps the supplied artwork immediately and saves `client.console_model`; the lobby illustration and console name follow it. This is a user choice, not hardware detection.

### Inputs / Fields

Connection fields are native Win32 edit controls placed inside 40 DIP Direct2D surrounds with 8 DIP corners. Their outline changes from border gray to amber when focused. Controller dropdowns use 12 DIP left text inset, a drawn chevron and an amber open outline; popup selection is marked with a small amber bar. Keep native editing semantics and shortcut behavior.

### Navigation

Gamepad/Keyboard are functional segmented tabs: selected gray fill, semibold text and a short amber underline. The lobby's stream preferences use amber radio markers and a gray selected-row surface; compact rows keep a single-line name. Neither pattern implies a sidebar or global app navigation shell.

### Status Badges, Icons and Keycaps

Status chips are 24 DIP, fully rounded, tinted 11% with a 30% hairline of their semantic color, a 3 DIP dot and 12 DIP medium text. Readiness rows pair a drawn status icon (check in a green tinted circle, exclamation in amber, dotted ring while waiting) with a right-aligned text state, so color is never the only signal. Keycaps (`Painter::keycap`) mark real shortcuts: "Or press A on your controller" and the lobby status bar's F11 Full screen. Pairing codes and addresses render in Cascadia Mono (Consolas fallback) because they are data.

### Lobby Console Illustration

The native painter provides Switch and Switch 2 front elevations in a shared 100 by 64 coordinate space, uniformly fitted within its bounds. The entire console layer?tablet, display, Joy-Cons and every control?is translated down by 8 units before drawing. The original transform is then restored so the dock remains fixed at `(14,23.5)-(86,63)`. Both models share this dock, local fill `#24272E`, 2-unit corners and a lip at 24.5 units. Lower console controls may naturally sit behind the dock.

Before that shared translation, left and right stick centers are `(6.8,13)` and `(93.2,28.5)`; directional and face-button diamonds are centered at `(6.8,27)` and `(93.2,14.5)`. These coordinates move together with the whole console. The display at local `(15,4)-(85,44)` is always blank black (`#090B0D`) for both models, including a ready session. There is no Hikari mark on the console screen and the painter API has no screen-on parameter. The supplied logo remains in the application's existing identity placements. The dock's status LED is a small flat dot without a halo.

Switch uses cyan and coral grips. Switch 2 uses lighter graphite grips (`#444A50`) with the same fill geometry and button/stick cap material (`#353A3F`) as Switch. Its cyan and coral stick rings and lower C button remain the distinguishing details; it uses the same dock as Switch and no extra rounded outline along the inner grip seam. These are local illustration materials, not new global UI tokens. They are separate from the user-supplied interactive Joy-Con atlas, which remains unchanged.

`--preview=switch2` exposes the Switch 2 illustration in an explicitly offline, screen-off sample state. Live rendering currently falls back to Switch: the generic controller bridge reports USB readiness, not reliable console identity. There is no model selector or automatic console-model detection in this implementation. Future connected-model selection requires a reliable model signal; neither these drawings nor their preview imply that capability exists.

### Binding Targets and Controller Artwork

Bindings connect directly to the supplied image. Hovering a diagram control or mapping shares emphasis; capture uses a 1.3 Hz amber outline pulse, countdown and an explicit input prompt. Capture times out after 5 seconds. A successful change flashes and fades over 0.9 seconds. Keyboard bindings render as keycaps; live mapped input adds a small amber dot.

`assets/joycon-reference.png` is decoded once from embedded resource 102. The renderer draws front source rectangles `(28,193,182,620)` and `(1018,193,1172,620)`, plus top views `(28,18,180,138)` and `(1020,18,1172,138)`. The artwork is scaled uniformly into its available area with linear interpolation. Hit tests use the same transform. Amber paths and circles are separate runtime overlays: do not bake them into the original bitmap or replace the supplied image with generated art. Provenance is recorded alongside the source image in `assets/joycon-reference.png.json`.

### Switch 2 Controller Artwork

The Switch 2 controller screen extends the existing mapping pane, typography, capture states and footer without changing global tokens. `assets/joycon2-left.png`, `joycon2-right.png`, `joycon2-left-top.png` and `joycon2-right-top.png` are the four user-supplied sources, retained with adjacent provenance JSON. Front images preserve original pixels: resources 103 and 104 use source rectangles `(254,52)-(783,1423)` and `(320,66)-(807,1406)`, drawn into `(0,0)-(164.7,427)` and `(190,0)-(345.2,427)` on the shared diagram canvas.

The user approved an exact mirrored shoulder pair. `assets/joycon2-shoulder-base.png` is an explicitly authorized image-tool derivative of the supplied left top view: only printed ZL/L labels were removed. Its adjacent JSON retains origin and editing prompt. Resource 107 supplies this same transparent base twice; source rectangle `(354,214)-(1274,845)` maps to `(0,-140)-(164,-27.5)`. The right view reflects geometry with `x -> 344-x` and converts cyan connector pixels to coral while preserving alpha and grayscale shading. ZL/L and ZR/R are drawn afterward in their readable orientations, outside the reflection. The original supplied top files remain preserved references. This narrow approved derivative does not authorize replacement of the supplied front artwork or the unchanged Switch 1 atlas.

Switch 2 has its own measured control centers, including sticks at `(70.1,124)` and `(282.1,218.6)`, A at `(312.7,112.8)`, and Home at `(257.9,290.3)`. Drawing and hit tests share the model, scale and transform for the 26 current mapped inputs. The visible C button is artwork only: the current wired profile has no C mapping. Do not imply it is interactive.

Offline previews `controller-switch2` and `controller-switch2-compact` select this model explicitly; the Controller action in the Switch 2 lobby preview passes the same model. This is preview routing, not automatic connected-console identification or a new model selector. Live model identity remains unavailable. Fresh rendered evidence is `artifacts/ui-review/controller-switch2/standard.png`, `compact.png` and `binding-a.png`; the latter shows exact A-to-binding capture feedback. Source provenance and the original Switch 1 implementation remain intact.

## Do's and Don'ts

### Do:

- **Do** preserve the supplied Hikari logo and the original transparent Joy-Con pixels and provenance.
- **Do** show the current connection state, next useful action and visible keyboard focus.
- **Do** keep input feedback aligned to the actual control and mapping target.
- **Do** label preview sample states explicitly and keep styling outside the streaming data path.

### Don't:

- **Don't** add decorative background glows, glass panels or breathing primary actions. Cards hold working content only; never nest cards or use them as icon-heading-text tiles.
- **Don't** replace this Win32/Direct2D implementation with web or mobile conventions.
- **Don't** infer live hardware readiness or latency measurements from offline preview screens.
- **Don't** promote local renderer differences or tiny functional captions into new global visual rules.

Not canonized or repaired by this documentation pass: small uppercase controller captions and local button radius/color differences. These remain implementation observations, not instructions to reproduce defects on future surfaces.

Menu refinement evidence: `artifacts/ui-review/polished-menus/{setup,connection,host,client,compact,progress,error}.png`. These are screenshots of the running native app in labeled offline preview states. The controller menu, controller artwork, shared UI kit and global color tokens were not changed in this pass.
