# CombatForge — macOS (Apple Silicon) port

Status of the cross-platform work on the **`mac-port`** branch. `main` stays Windows-authoritative;
everything here is additive and Mac-guarded.

## TL;DR — what works now

**Both targets build on macOS / Apple Silicon**, once the two environment prerequisites below are met
(one engine config line + a complete UE install). No workarounds live in the project.

| Target | Command | Result |
|---|---|---|
| Game / client (Monolithic) | `Build.sh CombatForge Mac Development` | `Binaries/Mac/CombatForge` — Mach-O arm64 (~390 MB); `.app` finalize OK |
| Editor (Modular) | `Build.sh CombatForgeEditor Mac Development` | `Binaries/Mac/UnrealEditor-CombatForge.dylib` — arm64. **This is what opening `CombatForge.uproject` needs.** |

Xcode project generation (`GenerateProjectFiles.sh`) also succeeds. Compiled under Apple clang 21 with
UE's `-Werror`: **zero code errors**, one non-fatal deprecation warning (see below).

> `CombatForgeServer` cannot be built on a Launcher/binary engine ("Server targets are not currently
> supported from this engine distribution") — expected and identical on Windows; see
> `Source/CombatForgeServer.Target.cs`.

Two genuinely Mac-only bugs were found and fixed (both invisible to MSVC on Windows):

1. **`PFBuildPieceActor.cpp` — `Cell` name collision.** The file's `constexpr float Cell = 400.f`
   collided with Apple Carbon's `typedef Point Cell` (`HIToolbox/Lists.h`), which the engine pulls in
   transitively on Mac → `error: reference to 'Cell' is ambiguous`. Renamed `Cell` → `CellUU` (matches
   the sibling `SubUU`).
2. **`PFRatingSubsystem.h` — `-Wcomment` error.** Doc comments contained `Saved/Arenas/*.json`; the
   `/*` (slash-star from the glob) reads as a nested block-comment opener → `error: '/*' within block
   comment [-Werror,-Wcomment]`. Reworded to `Saved/Arenas *.json`.

## Prerequisites on the Mac (all satisfied as of 2026-07-18)

**Step 0 — run the preflight. It checks all of this in ~2 s and prints the exact fix for anything broken:**
```bash
./Scripts/check-mac-env.command
```
Run it after **every** Epic Launcher operation (Verify/update reverts the SDK cap silently) and whenever
a Mac build or editor launch fails mysteriously. `Open-CombatForge-5.6.command` and
`Scripts/Package-Mac.command` run it automatically (skip with `CF_SKIP_PREFLIGHT=1`).

What it covers:
- **Xcode 26.6** (Apple clang 21) at `/Applications/Xcode.app`, selected via `xcode-select`.
- **UE 5.6** (Launcher binary) at `/Users/Shared/Epic Games/UE_5.6`, install **complete** (a partial
  install omits the Mac target-platform dylibs → startup assert; see Troubleshooting).
- **Engine SDK cap raised** — `Apple_SDK.json` `MaxVersion` → `26.9.0`. **Re-apply after any Launcher
  Verify/update — they silently revert it** (confirmed happening).
- **Metal Toolchain** (`metallib`) present.
- **LFS content hydrated** (no pointer stubs).
- **Project module manifest clean** (see Troubleshooting: stale-manifest assert).

**Also required before heavy editor work — DISK HEADROOM: ≥30 GB free.** The first editor open writes
an estimated 10–25 GB of shader DDC + one-time Virtual Texture build into `~/Library`. macOS degrades
badly under ~5 GB free; do not start a first-time map open, an SM6 shader rebuild, or a cook without
`df -h` showing ≥30 GB.
- **git-lfs** installed and **all LFS content hydrated** (`git lfs pull` — 23 GB, incl. `Content/Bandits`).
  The old "manually sync Bandits to the Mac" note in `packaging.md` is obsolete.
- **Metal Toolchain** (`metallib`) installed: `xcodebuild -downloadComponent MetalToolchain`
  (verify with `xcrun -f metallib`). Needed for shader compile/cook; recent Xcode ships it separately.

## First editor open — what to expect (so nothing reads as a new bug)

- **Time:** 10–30+ min to an interactive `L_Graybox`. Global Metal shaders compile at the splash, then
  materials as the map loads, plus the one-time Virtual Texture build. **Do not force-quit mid-wave** —
  a killed compile leaves a half-built cache.
- **Expected warnings (all normal):** ~18 `CDO Constructor … Failed to find /Game/RifleAnims/…` errors —
  the RifleAnims Fab pack is deliberately gitignored (`.gitignore`: local paid content) and absent on
  this Mac, so characters **T-pose in PIE** until it's delivered (LFS-track it from Windows like
  Bandits, or re-download from Fab). Also: `SkipPackage /Game/Survival_01/…` (stale path, exists on
  Windows too), ProxyLOD module missing (Windows-only), "No Audio Capture implementations" (mic only),
  MikkTSpace normal warning on SKM_QuantumCharacter.
- **Rendering:** until Metal SM6 is enabled (below), Mac renders **SM5 → Nanite is OFF** — warehouse
  geometry uses fallback meshes. Enable SM6 in Project Settings → Platforms → Mac → Targeted RHIs
  (writes `[/Script/MacTargetPlatform.MacTargetSettings]` `+TargetedRHIs=SF_METAL_SM6`), restart, and
  expect a **second full shader wave**. Runtime gate (macOS 15+, non-M1) passes on this M4 Pro.
- **Headless smoke recipe:** `UnrealEditor-Cmd CombatForge.uproject -ExecCmds="QUIT_EDITOR" -unattended`
  (`-run=NullCommandlet` "class not found" noise is expected on installed engines).
- **Playtest scripts:** until a packaged build exists, prefer `run-listen.command --editor` /
  `run-server.command --editor`. Raw Development binaries get `-project=` appended automatically
  (they have no cooked content); VPN (utun) IPs don't print in `print-host-ips` — use LAN IPv4.

## Troubleshooting: editor crashes on launch — `check(TargetPlatform)` / `RunningPlatform`

Symptom (either assert, both same cause):
```
Assertion failed: TargetPlatform  [RenderCore/Private/GlobalShader.cpp:384]
Assertion failed: RunningPlatform [Engine/Private/StaticMesh.cpp:6821]
```
Look earlier in `~/Library/Logs/Unreal Engine/CombatForgeEditor/CombatForge.log` for:
```
ModuleManager: Unable to load module 'MacTargetPlatform' ... UnrealEditor-MacTargetPlatform.dylib was not found
LogTargetPlatformManager: Failed to load module 'MacTargetPlatform' for platform Mac (Reason=FileNotFound)
```

**Cause: an incomplete UE install, not the project.** `Engine/Binaries/Mac/UnrealEditor.modules`
declares three Mac modules — `MacTargetPlatform`, `MacTargetPlatformControls`, `MacTargetPlatformSettings`
— but a partial install can ship only `Settings`. With no Mac target platform registered,
`GetRunningTargetPlatform()` returns null and the editor asserts during startup.

Diagnose:
```bash
UE="/Users/Shared/Epic Games/UE_5.6"
grep -o '"MacTargetPlatform[^"]*"[^,]*' "$UE/Engine/Binaries/Mac/UnrealEditor.modules"   # what's required
ls "$UE/Engine/Binaries/Mac/" | grep MacTargetPlatform                                    # what's present
```
(For comparison, complete platforms look like `Engine/Binaries/Mac/IOS/UnrealEditor-IOSTargetPlatform*.dylib`
— umbrella + Controls + Settings.)

**Fix:** Epic Games Launcher → Library → UE 5.6 → the `...` dropdown → **Verify**. That re-downloads the
missing dylibs. Reinstall if Verify doesn't restore them. **Then re-run the preflight — Verify also
reverts the `Apple_SDK.json` cap.**

**Same assert, second cause — stale/poisoned project module manifest.** If the log's module path points
at the **project** (`<Project>/Binaries/Mac/UnrealEditor-MacTargetPlatform.dylib`) instead of the
engine, the project's `Binaries/Mac/UnrealEditor.modules` manifest has non-project entries (residue of
any UBT build-config experiment — this happened here). The module manager trusts the project manifest
and never falls back to the engine copy, and ordinary rebuilds **preserve** stale entries. Fix:
```bash
rm -rf Binaries/Mac Intermediate/Build/Mac Intermediate/ProjectFiles
"$UE/Engine/Build/BatchFiles/Mac/Build.sh" CombatForgeEditor Mac Development -project="$PWD/CombatForge.uproject"
```
then confirm `Binaries/Mac/UnrealEditor.modules` lists only `"CombatForge"` (preflight check 3 does this).

## The SDK-version gate (Xcode 26.6 is newer than UE 5.6 allows)

UBT rejects Mac builds outright: `Found Sdk Version=26.6, MinRequired=15.2.0, MaxRequired=16.9.0` →
`Platform Mac is not a valid platform to build`. The cap lives in the engine's
`Engine/Config/Apple/Apple_SDK.json` (`MaxVersion`).

### The fix (required, engine-side)

One line in `/Users/Shared/Epic Games/UE_5.6/Engine/Config/Apple/Apple_SDK.json`:

```
"MaxVersion": "16.9.0"   →   "MaxVersion": "26.9.0"
```
```bash
sed -i '' 's/"MaxVersion": "16.9.0"/"MaxVersion": "26.9.0"/' \
  "/Users/Shared/Epic Games/UE_5.6/Engine/Config/Apple/Apple_SDK.json"
```

A backup sits next to it (`Apple_SDK.json.combatforge-backup`). **Caveat: this lives outside the repo and
is lost on every engine hotfix/reinstall** — re-apply it after updating UE. Raise the value again when
Xcode moves past 26.9.

### Why not a project-local override? (tried, rejected — don't retry)

UBT *does* read a project-local `Config/Mac/Mac_SDK.json` and honors its `MainVersion`; setting it to the
installed SDK version makes `IsVersionValidInternal` short-circuit valid and bypasses the max check. That
builds the **game** target (Monolithic → `AllowsPerProjectSDKVersion()` true). Adding
`bAllowSDKOverrideModulesWithSharedEnvironment = true` also gets the **editor** to *compile*.

**But it breaks the editor at runtime.** That flag makes SDK-version-sensitive modules resolve to a
*project-side* copy — so the editor looks for
`<Project>/Binaries/Mac/UnrealEditor-MacTargetPlatform.dylib`, which can never exist on an Installed
(Launcher) engine, since engine modules can't be rebuilt into the project. Result: no running target
platform → `check(TargetPlatform)` assert on startup. Both workarounds were removed; the engine-side
`MaxVersion` bump is the only correct fix.

> **Gotcha:** the engine's error text suggests `bAreTargetSDKVersionsRelevantOverride = false`.
> That does **not** work — `TargetRules.IsSDKVersionRelevant()` returns `true` unconditionally when the
> target's platform *is* the SDK's platform, so the flag is never consulted.

## Build commands

```bash
UE="/Users/Shared/Epic Games/UE_5.6"
# Editor — required to open CombatForge.uproject:
"$UE/Engine/Build/BatchFiles/Mac/Build.sh" CombatForgeEditor Mac Development -project="$PWD/CombatForge.uproject"
# Game/client target:
"$UE/Engine/Build/BatchFiles/Mac/Build.sh" CombatForge Mac Development -project="$PWD/CombatForge.uproject"
```

Generate Xcode project files with:
```bash
"$UE/Engine/Build/BatchFiles/Mac/GenerateProjectFiles.sh" -project="$PWD/CombatForge.uproject" -game
```

> Earlier revisions of this doc listed a failing `.app` finalize and a cross-target project-generation
> conflict. Both were symptoms of the project-local SDK override and disappeared once it was removed in
> favour of the engine-side `MaxVersion` fix.

## Remaining / verify-in-person

- **Open the project** — double-click `CombatForge.uproject` (or `Open-CombatForge-5.6.command`).
  First open compiles Metal shaders and will take a while.
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
