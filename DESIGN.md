---
name: Hikari
description: Refined native Windows remote-play workspaces in graphite and amber.
colors:
  graphite-bg: "#191A1D"
  graphite-panel: "#202226"
  control: "#282A2F"
  control-hover: "#303339"
  border: "#41444B"
  text: "#F1F2F3"
  text-secondary: "#B7BAC0"
  text-muted: "#A0A4AC"
  amber: "#FBBB04"
  amber-hover: "#FFCB33"
  on-amber: "#1A1400"
  good: "#3DDC84"
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
  button: "6px"
  binding: "7px"
  controller-control: "8px"
  field: "8px"
  controller-panel: "12px"
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

Hikari is a native C++ Windows application rendered with Win32, Direct2D and DirectWrite. Its matte graphite surfaces, restrained Hikari amber and compact Windows controls put connection state and useful actions first. The user selected the controller settings screen as the reference and requested a refined SaaS product feel for the other menus. The built result carries that same measured title/subtitle hierarchy, functional panes and compact actions across setup, connection, host and lobby while retaining the native Windows stack.

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

Setup, connection and host client sizes are 680 by 454, 760 by 568 and 900 by 724 DIP respectively. They share a 24 DIP outside margin, 22 DIP page title and 13 DIP supporting line. Functional panes use 12 DIP corners, 16 DIP gaps and approximately 22 DIP content insets. Setup presents Host and Client as equal side-by-side role choices. Connection places host details beside nearby hosts and anchors actions at the bottom. Host places connected-device checks beside pairing details. These compositions group actual tasks; they are not decorative dashboard cards.

The lobby applies the same 24 DIP margin and page hierarchy. Real Controller and Connection actions sit at the upper right with 36 DIP height. Session state and stream preference occupy the left column; host connection and readiness checks occupy the right. Content starts at 100 DIP with a 16 DIP pane gap. The session pane is 220 DIP tall; Play is 164 by 38 DIP. The left/right division uses 62% of the usable width for the left workspace. Stream preference is a vertical radio-choice list with row pitch up to 64 DIP; below 48 DIP pitch, labels collapse to one line and descriptions are omitted. The footer identifies offline sample data. This compact behavior is measured inside the native window, not a web breakpoint.

The session illustration reserves 44% of its inner row width, clamped to 174-300 DIP, at the right of the session pane. Text and status messages end 20 DIP before the artwork; controls remain in the left action area. Progress shows only the current step and spinner when the text area is narrower than 330 DIP, retaining its message and Cancel action. Larger illustrations use the same existing pane rather than changing the workspace hierarchy.

Controller settings default to 1200 by 760 DIP, with a minimum of 1040 by 700 DIP. Its margin is 24 DIP, toolbar controls are 36 DIP tall, and the content begins at 148 DIP. The binding pane width is clamped to 57% of the window between 600 and 800 DIP; a 16 DIP gap separates the artwork pane. The three mapping columns have 22 DIP gaps, adaptive row pitch clamped to 24–40 DIP and targets up to 32 DIP tall. The footer stays anchored with 38 DIP action buttons. These are native minimum-size constraints, not responsive stacking instructions.

**The Shared Rhythm Rule.** Align controls to their toolbar, pane and footer baselines; expand the working area without adding decorative dashboard modules.

## Elevation & Depth

The ordinary canvas and working panes use flat tonal separation and thin strokes, without background gradients or decorative glows. Rounded controller panes are functional groupings. Dropdown popups and dialogs are actual overlay layers: their native shadow helper draws four black rounded rectangles, expanded by 3, 6, 9 and 12 DIP and offset down by 6 DIP. Popup strength is 0.35; dialog strength is 0.5, divided evenly among those layers. Dialogs additionally dim the canvas with 55% black. Keyboard keycaps have a small 2 DIP dark lower edge. Do not turn these structural exceptions into floating dashboard cards.

Streaming telemetry and toasts use translucent dark overlays to keep text readable over moving video; these are operational overlays rather than a glass material system.

## Shapes

Shared buttons use softly squared 6 DIP corners; connection field surrounds use 8 DIP corners. The refreshed setup, connection, host and lobby working panes use 12 DIP corners. Controller buttons/dropdowns use 8 DIP, bindings 7 DIP and functional panes 12 DIP. Controller popups use 10 DIP and dialogs 14 DIP. Small status badges and switch tracks retain their task-specific rounded silhouettes. Standard control boundaries are 1 DIP. Do not impose one global radius on the two existing native renderers.

## Components

### Buttons

The shared hit list includes only enabled actions. Lobby focus persists across repaint, advances with Tab/Shift+Tab and activates with Enter or Space. When session state replaces Play with Retry, connection setup or Cancel, focus is reconciled to an available action. Native Alt+Enter fullscreen behavior remains available.

Shared primary buttons use amber with dark text; secondary buttons use control gray, a thin border and bright text. Ghost actions gain a 6% white hover fill. Shared buttons inset by 1 DIP while pressed and show a 2 DIP amber focus stroke 3 DIP outside their bounds. Disabled shared fill and text opacity multiply by 0.4. Text is centered, semibold and 14 DIP.

Controller settings have an established local variant: 8 DIP corners and 13.5 DIP semibold text, primary ink `#1E2127`, hover `#FFC933`, and pressed `#E0A800`. Preserve this difference when extending that renderer; it is not a new global theme. Danger actions use red text and restrained tinted states.

### Inputs / Fields

Connection fields are native Win32 edit controls placed inside 40 DIP Direct2D surrounds with 8 DIP corners. Their outline changes from border gray to amber when focused. Controller dropdowns use 12 DIP left text inset, a drawn chevron and an amber open outline; popup selection is marked with a small amber bar. Keep native editing semantics and shortcut behavior.

### Navigation

Gamepad/Keyboard are functional segmented tabs: selected gray fill, semibold text and a short amber underline. The lobby's stream preferences use amber radio markers and a gray selected-row surface; compact rows keep a single-line name. Neither pattern implies a sidebar or global app navigation shell.

### Status Badges

Shared status badges are 26 DIP tall, their width is measured text plus 36 DIP, with a colored dot and text; controller status badges use a 28 DIP allocation and subtle semantic tint. Text explains the state so color is supplementary.

### Lobby Console Illustration

The native painter provides Switch and Switch 2 front elevations in a shared 100 by 64 coordinate space, uniformly fitted within its bounds. The entire console layer?tablet, display, Joy-Cons and every control?is translated down by 8 units before drawing. The original transform is then restored so the dock remains fixed at `(14,23.5)-(86,63)`. Both models share this dock, local fill `#24272E`, 2-unit corners and a lip at 24.5 units. Lower console controls may naturally sit behind the dock.

Before that shared translation, left and right stick centers are `(6.8,13)` and `(93.2,28.5)`; directional and face-button diamonds are centered at `(6.8,27)` and `(93.2,14.5)`. These coordinates move together with the whole console. The display at local `(15,4)-(85,44)` is always blank black (`#090B0D`) for both models, including a ready session. There is no Hikari mark on the console screen and the painter API has no screen-on parameter. The supplied logo remains in the application's existing identity placements. The dock's status LED is a small flat dot without a halo.

Switch uses cyan and coral grips. Switch 2 uses lighter graphite grips (`#444A50`) with the same fill geometry and button/stick cap material (`#353A3F`) as Switch. Its cyan and coral stick rings and lower C button remain the distinguishing details; it uses the same dock as Switch and no extra rounded outline along the inner grip seam. These are local illustration materials, not new global UI tokens. They are separate from the user-supplied interactive Joy-Con atlas, which remains unchanged.

`--preview=switch2` exposes the Switch 2 illustration in an explicitly offline, screen-off sample state. Live rendering currently falls back to Switch: the generic controller bridge reports USB readiness, not reliable console identity. There is no model selector or automatic console-model detection in this implementation. Future connected-model selection requires a reliable model signal; neither these drawings nor their preview imply that capability exists.

### Binding Targets and Controller Artwork

Bindings connect directly to the supplied image. Hovering a diagram control or mapping shares emphasis; capture uses a 1.3 Hz amber outline pulse, countdown and an explicit input prompt. Capture times out after 5 seconds. A successful change flashes and fades over 0.9 seconds. Keyboard bindings render as keycaps; live mapped input adds a small amber dot.

`assets/joycon-reference.png` is decoded once from embedded resource 102. The renderer draws front source rectangles `(28,193,182,620)` and `(1018,193,1172,620)`, plus top views `(28,18,180,138)` and `(1020,18,1172,138)`. The artwork is scaled uniformly into its available area with linear interpolation. Hit tests use the same transform. Amber paths and circles are separate runtime overlays: do not bake them into the original bitmap or replace the supplied image with generated art. Provenance is recorded alongside the source image in `assets/joycon-reference.png.json`.

## Do's and Don'ts

### Do:

- **Do** preserve the supplied Hikari logo and the original transparent Joy-Con pixels and provenance.
- **Do** show the current connection state, next useful action and visible keyboard focus.
- **Do** keep input feedback aligned to the actual control and mapping target.
- **Do** label preview sample states explicitly and keep styling outside the streaming data path.

### Don't:

- **Don't** add decorative background glows, glass panels, breathing primary actions or dashboard cards.
- **Don't** replace this Win32/Direct2D implementation with web or mobile conventions.
- **Don't** infer live hardware readiness or latency measurements from offline preview screens.
- **Don't** promote local renderer differences or tiny functional captions into new global visual rules.

Not canonized or repaired by this documentation pass: small uppercase controller captions and local button radius/color differences. These remain implementation observations, not instructions to reproduce defects on future surfaces.
