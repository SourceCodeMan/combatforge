# Windows handheld support (ROG Ally / Legion Go / MSI Claw class)

Written 2026-07-28 on branch `feat/handheld-windows`. Goal: CombatForge is fully playable
on a Windows handheld — pad-only gameplay, pad-driven menus, and sane performance defaults
on 7-8" low-wattage hardware — without touching replication (**NetProtocol stays 16**) and
without changing anything for keyboard+mouse desktop players.

⚠️ **Nothing here has run on real handheld hardware** (none owned). Everything compiles and
the design leans on engine-supported paths, but stick feel, cursor speed, preset framerates,
and the X-button reload/interact pairing need on-device verification — tracked in GitHub
issues (`handheld` label).

---

## 1. What was added

### Gamepad gameplay bindings (`PFInputConfig.cpp`)

The pad maps onto the **same UInputActions** as the keyboard, so every existing handler
works unchanged. Fixed layout (Xbox names):

| Input | Common | Combat | Build |
|---|---|---|---|
| Left stick | Move | — | — |
| Right stick | Look (`IA_LookStick`) | — | wheel sector select while wheel open |
| A | Jump | — | — |
| B | Crouch / slide | — | — |
| X | — | Reload **and** Use/Refill | — |
| Y | — | Fire mode | Delete tool |
| RT | — | Fire | Place |
| LT | — | ADS | — |
| RB | — | Throw frag | Rotate piece |
| LB | — | Throw smoke | **Hold** = build wheel |
| L3 | Sprint | — | — |
| R3 | — | Punch | — |
| D-pad up | Ready | — | — |
| D-pad down | *(lobby: cursor/play toggle)* | Plant bomb | — |
| D-pad left/right | Class cycle (while dead) | — | Piece cycle |
| View | Scoreboard (hold) | — | — |
| Menu | Menu back / options | — | — |

Design notes:
- **`IA_LookStick` is a separate action from `IA_Look`**: stick deflection is a *rate*
  (scaled by frame time in `ACombatForgeCharacter::OnLookStickInput`), mouse deltas are
  absolute. Sharing one action would make pad turn speed framerate-dependent.
  Base rates 220°/s yaw / 150°/s pitch × the "Gamepad look speed" option (0.2–3.0), square
  response curve, shared Invert-Y pref, and the same `pf.ADSSensScale` focal-length ADS
  slowdown as the mouse (factored into `ComputeADSLookScale()`).
- Radial deadzone 0.25 on both sticks at the mapping level (worn handheld sticks drift).
- D-pad class/piece cycle uses an explicit `UInputTriggerPressed` — without it a held
  digital key re-triggers the Axis1D action every tick.
- X doubles as Reload + Use (barrel refill) inside one context — same idea as the shipped
  F pairing (Ready + Interact). Flagged for on-device verification.
- The rebind deny-list now includes the whole fixed pad layout, so the keyboard rebind
  capture can't silently steal a pad button.

### Menu interaction: gamepad-as-pointer (`Input/PFGamepadCursor.*`)

Every CombatForge menu is a mouse-click UMG tree built in C++. Rather than retrofitting
focus navigation into every widget, a Slate **input preprocessor** (subclass of the
engine's `FAnalogCursor`) turns the pad into a pointer whenever the local player
controller shows the mouse cursor (boot menu, lobby, options, vote/results, Tab-hold):

- Left stick moves the cursor (engine-accelerated, sticky over buttons)
- **A** = click (synthesized left-mouse at the cursor — `FAnalogCursor`'s own click path,
  fed by translating A into `Virtual_Accept`, which Windows never sends natively)
- **B** = Escape (menu back — routes through the existing widget/Enhanced-Input paths)
- Right stick = mouse-wheel scroll (server browser, settings pages)
- **Lobby only:** D-pad down toggles "play mode" — hands the pad back to gameplay to walk
  and shoot the warm-up pen; D-pad down returns to the cursor. (The lobby is the only place
  live gameplay and a clickable panel coexist.)

While the cursor is active **all other pad input is consumed**, so RT can never fire the
weapon through an open options overlay. When the cursor is hidden, the preprocessor is
completely inert. Registered by `UCombatForgeGameInstance` (never on dedicated servers or
during cooks).

The preprocessor also feeds `FPFInputDevice::IsGamepadPrimary()` (last-used device), which
swaps hint text like the build wheel's "hold Q" → "hold LB".

### Handheld detection + first-run preset (`Core/PFHandheldPlatform.*`)

Detection is **conservative on purpose** — explicit signals only, because a
battery+small-screen heuristic would misfire on ordinary laptops:

| Signal | Result |
|---|---|
| `-pfnothandheld` | force off (wins) |
| `-pfsteamdeck` / `-pfhandheld` | force Deck / Windows handheld |
| env `SteamDeck=1` | Steam Deck (SteamOS sets it, survives Proton) |
| CPU brand contains `Custom APU 0405` / `0932` | Steam Deck (LCD / OLED — covers Windows-on-Deck) |
| CPU brand contains `Ryzen Z1` / `Ryzen Z2` | Windows handheld (Ally, Legion Go, Ally X…) |

MSI Claw (Intel Core Ultra) is **not** auto-detected — its CPU strings are shared with
ordinary laptops. Claw owners use `-pfhandheld` once or the in-game preset command.

On first detection (guarded by a saved pref + preset version, so later player changes are
never stomped), the preset applies through the exact same path as the Options screen:

| | Windows handheld | Steam Deck |
|---|---|---|
| Quality | Medium (1) | Low (0) |
| Resolution index | 1920×1080 | 1280×720 |
| Render scale | 65% | 75% |
| FPS cap | 60 | 60 |
| UI scale | 1.15 | 1.10 |
| Window mode | Borderless fullscreen | Borderless fullscreen |

Console: `pf.HandheldInfo` (log the signals — ask a player to run this and send the log),
`pf.HandheldPreset [1|2]` (apply now / force a tier). Detection signals are logged every
boot, so any shipped log answers "did it detect".

### Options additions (`PFOptionsWidget`)

- Controls: **Gamepad look speed** slider (live) + a fixed-layout crib text
- Video: **UI scale** slider 85–130% (live, `UUserInterfaceSettings::ApplicationScale`)
- New prefs (`PFUserPrefs`): `GamepadLookScale`, `UIScale`, `HandheldPresetApplied`

---

## 2. What deliberately did NOT change

- **No replication / RPC / protocol changes** — NetProtocol stays 16; this branch
  cross-plays with live alpha-16.
- No Common UI plugin, no per-widget focus navigation, no glyph textures (text hints only).
- No aim assist (needs on-device tuning), no gamepad rebinding UI, no rumble — issues filed.
- Desktop KB/M behavior untouched: the preprocessor passes non-gamepad input through and
  gamepad input too while the cursor is hidden.

## 3. How to verify on a real device (checklist for whoever gets hardware first)

1. Boot → log shows `HandheldDetect: … -> WindowsHandheld` and `Handheld preset applied`.
2. Boot menu: left stick moves a cursor, A clicks ENTER, B backs out of panels,
   right stick scrolls the server browser.
3. Lobby: cursor clicks MODE/TYPE/GO; D-pad down walks the pen (stick move + RT fire);
   D-pad up readies; D-pad down again returns the cursor.
4. Build: LB hold opens the wheel, right stick picks a sector, release commits; RT places;
   RB rotates; D-pad left/right cycles pieces; Y = delete tool.
5. Combat: RT/LT fire/ADS, X reloads **and** refills at a barrel (verify both fire), RB/LB
   grenades, R3 punch, D-pad down plants the bomb.
6. Options overlay opened mid-combat: RT must NOT fire while it is open.
7. Sliders: gamepad look speed + UI scale live-apply.
8. `stat fps` at the preset: ≥55 sustained on a Z1 Extreme @ 1080p/65%/Medium, else lower
   the preset defaults.

## 4. Files touched

- `Source/CombatForge/Input/PFGamepadCursor.{h,cpp}` — new
- `Source/CombatForge/Core/PFHandheldPlatform.{h,cpp}` — new
- `Source/CombatForge/Input/PFInputConfig.{h,cpp}` — pad mappings, `IA_LookStick`, deny-list
- `Source/CombatForge/Player/CombatForgeCharacter.{h,cpp}` — stick look, ADS-scale refactor
- `Source/CombatForge/Core/CombatForgeGameInstance.{h,cpp}` — boot hook + preprocessor
- `Source/CombatForge/Core/CombatForgePlayerController.{h,cpp}` — `IsBootMenuActive()`
- `Source/CombatForge/Core/PFUserPrefs.{h,cpp}` — 3 new prefs
- `Source/CombatForge/UI/PFBuildWheelWidget.{h,cpp}` — stick sector select, pad hint
- `Source/CombatForge/UI/PFOptionsWidget.{h,cpp}` — 2 sliders + layout note
