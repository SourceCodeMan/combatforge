# CombatForge — macOS (Apple Silicon) port

Status of the cross-platform work on the **`mac-port`** branch. `main` stays Windows-authoritative;
everything here is additive and Mac-guarded.

## TL;DR — what works now

**The CombatForge C++ compiles and links on macOS / Apple Silicon.** The monolithic **game target**
builds a valid `Mach-O arm64` executable (`Binaries/Mac/CombatForge`, ~390 MB) under Apple clang 21 with
UE's `-Werror`, with **zero code errors** — only one non-fatal deprecation warning left (see below).

Two genuinely Mac-only bugs were found and fixed (both invisible to MSVC on Windows):

1. **`PFBuildPieceActor.cpp` — `Cell` name collision.** The file's `constexpr float Cell = 400.f`
   collided with Apple Carbon's `typedef Point Cell` (`HIToolbox/Lists.h`), which the engine pulls in
   transitively on Mac → `error: reference to 'Cell' is ambiguous`. Renamed `Cell` → `CellUU` (matches
   the sibling `SubUU`).
2. **`PFRatingSubsystem.h` — `-Wcomment` error.** Doc comments contained `Saved/Arenas/*.json`; the
   `/*` (slash-star from the glob) reads as a nested block-comment opener → `error: '/*' within block
   comment [-Werror,-Wcomment]`. Reworded to `Saved/Arenas *.json`.

## Prerequisites on the Mac (all satisfied as of 2026-07-17)

- **Xcode 26.6** (Apple clang 21) at `/Applications/Xcode.app`, selected via `xcode-select`.
- **UE 5.6** (Launcher binary) at `/Users/Shared/Epic Games/UE_5.6`.
- **git-lfs** installed and **all LFS content hydrated** (`git lfs pull` — 23 GB, incl. `Content/Bandits`).
  The old "manually sync Bandits to the Mac" note in `packaging.md` is obsolete.
- **Metal Toolchain** (`metallib`) installed: `xcodebuild -downloadComponent MetalToolchain`
  (verify with `xcrun -f metallib`). Needed for shader compile/cook; recent Xcode ships it separately.

## The SDK-version gate (Xcode 26.6 is newer than UE 5.6 allows)

UBT rejects the build: `Found Sdk Version=26.6, MinRequired=15.2.0, MaxRequired=16.9.0` →
`Platform Mac is not a valid platform to build`. The cap lives in the engine's
`Engine/Config/Apple/Apple_SDK.json` (`MaxVersion`). Two ways to clear it:

### A. In-repo (committed) — unblocks the GAME target only

`Config/Mac/Mac_SDK.json` = `{ "MainVersion": "26.6" }`.

UBT reads a project-local `Config/<Platform>/<Platform>_SDK.json` and honors **only** its `MainVersion`.
When `MainVersion` equals the installed SDK version, `UEBuildPlatformSDK.IsVersionValidInternal`
short-circuits (`IntVersion == GetMainVersion()` → valid) and never checks the max. This works for the
**game target** because it's **Monolithic** (`AllowsPerProjectSDKVersion()` is true for
Monolithic / Unique-env targets). It does **not** work for the **editor** target — that's a modular
Shared build environment, which forbids per-project SDK overrides (`RulesError`).

> If you bump Xcode again, update `MainVersion` in this file to the new version string
> (the value UBT prints as `Found Sdk Version=…`).

### B. Engine edit — universal (editor + Xcode schemes + clean `.app` finalize)

One line in `/Users/Shared/Epic Games/UE_5.6/Engine/Config/Apple/Apple_SDK.json`:
`"MaxVersion": "16.9.0"` → `"26.9.0"`. A backup was made next to it (`*.combatforge-backup`).
This is **outside the repo** (shared engine install) and re-applies after any engine hotfix/reinstall,
so it was left for a human to apply. It removes the per-project override friction entirely (all targets
see Mac as valid, no cross-target conflict).

## Build commands

```bash
UE="/Users/Shared/Epic Games/UE_5.6"
# GAME/client target — works today with the in-repo Mac_SDK.json:
"$UE/Engine/Build/BatchFiles/Mac/Build.sh" CombatForge Mac Development -project="$PWD/CombatForge.uproject"
# EDITOR target — needs the engine edit (B) above first:
"$UE/Engine/Build/BatchFiles/Mac/Build.sh" CombatForgeEditor Mac Development -project="$PWD/CombatForge.uproject"
```

### Known caveat — "Modern Xcode" `.app` finalization
With approach A, `Build.sh CombatForge` **compiles and links the binary successfully** but exits non-zero
on the final step ("Modern Xcode" wraps the binary into a `.app` via `xcodebuild` and can't find a
generated scheme — `does not contain a scheme named "CombatForge"`). The raw `Binaries/Mac/CombatForge`
executable is complete and runnable (`-game` / listen server). A clean `.app` finalize + Xcode project
generation want approach **B** (project generation with the override hits the same cross-target conflict).
Packaging via `Scripts/Package-Mac.command` (BuildCookRun) stages its own `.app` and is the distribution path.

## Remaining / verify-in-person

- **Editor on Mac** — apply engine edit (B), then build `CombatForgeEditor`, open, let shaders compile.
- **Metal SM6** — in the Mac editor: Project Settings → Platforms → Mac → Targeted RHIs → **Metal SM6**
  (needed for Nanite on the warehouse/Bandit geometry; VT is fine on SM5).
- **Package** — `Scripts/Package-Mac.command` (unsigned `.app`; Gatekeeper right-click-Open first run).
- **LAN cross-play** — Mac client ↔ PC host, by IP (`Deploy/playtest/*.command`). Highest-value proof.
- **Deprecation warning** (optional) — `PFCharacterPreviewActor.cpp:100` uses the deprecated public
  `USceneCaptureComponent::ShowFlagSettings`; migrate to `SetShowFlagSettings`/`GetShowFlagSettings`
  before a future UE makes it a hard error. Non-fatal today; cross-platform.
- **WmfMedia** (v7-added, Windows-only plugin): Mac-safe — its runtime module is `PlatformAllowList:["Win64"]`,
  so UBT skips it on Mac. No action.

## Mac dev scripts (added on this branch)

`.command` siblings of the Windows playtest scripts (double-click in Finder). **Authored, not yet run
end-to-end on Mac** — verify when hosting the first Mac playtest:
- `Open-CombatForge-5.6.command` — open the project in the Mac editor.
- `Deploy/playtest/run-listen.command` — host + play on this Mac.
- `Deploy/playtest/run-server.command` — headless server (`?listen -server -nullrhi`).
- `Deploy/playtest/connect.command <ip>` — join a host by IP.
- `Deploy/playtest/print-host-ips.command` — print LAN/VPN join addresses.
- `Scripts/Package-Mac.command` — already existed; packages a standalone `.app`.
