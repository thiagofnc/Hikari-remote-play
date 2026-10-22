---
name: Hikari
description: Compact native Windows remote-play controls in graphite and amber.
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
    fontSize: "30px"
    fontWeight: 600
  title:
    fontFamily: "Segoe UI Variable Display, Segoe UI"
    fontSize: "22px"
    fontWeight: 600
  body:
    fontFamily: "Segoe UI Variable Text, Segoe UI"
    fontSize: "14px"
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
  controller-panel: "12px"
spacing:
  tight: "8px"
  control-gap: "16px"
  panel-inset: "18px"
  controller-margin: "24px"
  dialog-margin: "30px"
  lobby-margin: "40px"
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
    rounded: "{rounded.button}"
    height: "44px"
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

Hikari is a native C++ Windows application rendered with Win32, Direct2D and DirectWrite. Its matte graphite surfaces, restrained Hikari amber and compact Windows controls put connection state and useful actions first. These are the user-approved materials, recorded from the built implementation rather than a web or mobile redesign.

The supplied Hikari logo and original transparent Joy-Con image carry the product identity. The controller diagram is an interactive mapping surface: selecting a real control identifies its binding, while precise amber overlays show capture and live input. Preview screens explicitly identify sample data.

**Key Characteristics:**

- Compact native desktop controls and left-aligned information.
- Flat graphite surfaces, thin boundaries and restrained amber state feedback.
- Original supplied artwork with preserved transparent pixels.
- Motion attached to progress, capture, input feedback and transient status.

Evidence: `ui/hikari/ui_kit.h`, `ui/ui_kit.cpp`, `client/src/controller_settings.cpp`, `client/src/joycon_art.cpp`, `client/src/client_app.cpp`, `app/main.cpp`; rendered review captures in `artifacts/ui-review/after`. Frontmatter `px` values represent logical Windows DIPs at 96 DPI, not physical pixels or a CSS implementation. DirectWrite keeps its native line metrics; no fixed line-height or letter-spacing scale is established.

## Colors

The primary palette is warm amber against neutral graphite; device and semantic colors remain functional exceptions.

### Primary

- **Hikari amber:** primary actions, selected preset underline, keyboard focus, capture progress and diagram overlays.
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

- **Headline:** lobby device title at 30 DIP semibold; setup title at 28 DIP; connection and host titles at 24 DIP.
- **Title:** controller settings at 22 DIP semibold; controller dialog title at 17 DIP.
- **Body:** shared UI at 14 DIP; controller mappings at 13.5 DIP.
- **Label:** shared hints and controller keys at 12 DIP; shared status pill at 12.5 DIP medium.

Non-wrapping text trims with a character ellipsis. Messages that need multiple lines explicitly request wrapping. Controller toolbar and group captions currently use 11 DIP semibold uppercase; they are functional field/group labels, not a transferable eyebrow or display style.

## Layout

Dimensions are logical DIPs, scaled through the shared painter or a DPI-aware controller render target. Retain ordinary Windows frames and resize behavior. There is no web breakpoint or mobile layout.

Setup, connection and host windows use compact, left-aligned action groups. Their client sizes are 520 by 440, 620 by 560 and 620 by 720 respectively. The lobby uses a 40 DIP margin, a brand/status row and separator at 80 DIP, actions on the left, a device illustration on the right and a bottom toolbar. The main action is 180 by 44 DIP; preset and settings controls are 34 DIP tall.

Controller settings default to 1200 by 760 DIP, with a minimum of 1040 by 700 DIP. Its margin is 24 DIP, toolbar controls are 36 DIP tall, and the content begins at 148 DIP. The binding pane width is clamped to 57% of the window between 600 and 800 DIP; a 16 DIP gap separates the artwork pane. The three mapping columns have 22 DIP gaps, adaptive row pitch clamped to 24–40 DIP and targets up to 32 DIP tall. The footer stays anchored with 38 DIP action buttons. These are native minimum-size constraints, not responsive stacking instructions.

**The Shared Rhythm Rule.** Align controls to their toolbar, pane and footer baselines; expand the working area without adding decorative dashboard modules.

## Elevation & Depth

The ordinary canvas and working panes use flat tonal separation and thin strokes, without background gradients or decorative glows. Rounded controller panes are functional groupings. Dropdown popups and dialogs are actual overlay layers: their native shadow helper draws four black rounded rectangles, expanded by 3, 6, 9 and 12 DIP and offset down by 6 DIP. Popup strength is 0.35; dialog strength is 0.5, divided evenly among those layers. Dialogs additionally dim the canvas with 55% black. Keyboard keycaps have a small 2 DIP dark lower edge. Do not turn these structural exceptions into floating dashboard cards.

Streaming telemetry and toasts use translucent dark overlays to keep text readable over moving video; these are operational overlays rather than a glass material system.

## Shapes

Shared buttons and connection field surrounds use softly squared 6 DIP corners. Controller buttons/dropdowns use 8 DIP, bindings 7 DIP and functional panes 12 DIP. Controller popups use 10 DIP and dialogs 14 DIP. Small status badges and switch tracks retain their task-specific rounded silhouettes. Standard control boundaries are 1 DIP. Do not impose one global radius on the two existing native renderers.

## Components

### Buttons

Shared primary buttons use amber with dark text; secondary buttons use control gray, a thin border and bright text. Ghost actions gain a 6% white hover fill. Shared buttons inset by 1 DIP while pressed and show a 2 DIP amber focus stroke 3 DIP outside their bounds. Disabled shared fill and text opacity multiply by 0.4. Text is centered, semibold and 14 DIP.

Controller settings have an established local variant: 8 DIP corners and 13.5 DIP semibold text, primary ink `#1E2127`, hover `#FFC933`, and pressed `#E0A800`. Preserve this difference when extending that renderer; it is not a new global theme. Danger actions use red text and restrained tinted states.

### Inputs / Fields

Connection fields are native Win32 edit controls placed inside 44 DIP Direct2D surrounds. Their outline changes from border gray to amber when focused. Controller dropdowns use 12 DIP left text inset, a drawn chevron and an amber open outline; popup selection is marked with a small amber bar. Keep native editing semantics and shortcut behavior.

### Navigation

Gamepad/Keyboard are functional segmented tabs: selected gray fill, semibold text and a short amber underline. The lobby's stream presets use an amber underline for selection. Neither pattern implies a sidebar or global app navigation shell.

### Status Badges

Shared status badges are 26 DIP tall with a colored dot and text; controller status badges use a 28 DIP allocation and subtle semantic tint. Text explains the state so color is supplementary.

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
