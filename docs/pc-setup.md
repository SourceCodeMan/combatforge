# PaintForge — Windows PC setup & first compile

**Audience:** you, on the Windows PC, doing the first-ever compile and first editor run of this
repo. Follow it top to bottom, in order. Budget **half a day** for the whole thing (per
architecture risk R8) — most of that is downloads and the first build.

Everything here assumes the repo lives at `D:\projects\paintforge`. If you put it elsewhere,
substitute your path (avoid deep nesting and spaces in the path).

---

## 1. Install Visual Studio 2022

Install **Visual Studio 2022 Community** (17.10 or newer) from
<https://visualstudio.microsoft.com/>. In the Visual Studio Installer, select **both** workloads:

1. **Game development with C++**
   - In the right-hand "Installation details" pane for this workload:
     - ✅ **Windows 11 SDK** (any recent 10.0.2xxxx version)
     - ✅ **MSVC v143 – VS 2022 C++ x64/x86 build tools**
     - ❌ **Unreal Engine installer** — UNCHECK this (we install the engine via the Epic launcher,
       not VS)
     - Optional but recommended: ✅ **C++ AddressSanitizer**
2. **.NET desktop development** — required; UnrealBuildTool and UnrealHeaderTool are .NET apps and
   will not run without it.

If VS is already installed, open the Visual Studio Installer → **Modify** and confirm both
workloads and the components above.

## 2. Install Unreal Engine 5.6

1. Install the **Epic Games Launcher** from <https://www.unrealengine.com/download>.
2. Launcher → **Unreal Engine** tab → **Library** → **＋** next to "Engine Versions" → pick
   **5.6.x** (latest hotfix of 5.6).
3. Click **Options** on the install tile before installing:
   - ❌ **Engine Source** — OFF (not needed for this project)
   - ❌ **Editor symbols for debugging** — OFF (saves ~40 GB; add later only if you need engine
     callstacks)
   - Target platforms: none needed beyond the default Windows support.
4. Install (default location is fine) and wait — this is the long download.

## 3. Clone the repo

```bat
D:
mkdir D:\projects 2>nul
cd D:\projects
git clone <your-remote-url> paintforge
cd paintforge
```

Sanity check — you should see `PaintForge.uproject`, `Source\`, `Config\`, `Content\Maps\` (empty
except `.gitkeep`), and `docs\`. There is deliberately **no** `Binaries\` or `Intermediate\` in
git; the first build creates them.

## 4. Generate Visual Studio project files

1. In File Explorer, go to `D:\projects\paintforge`.
2. **Right-click `PaintForge.uproject`** → **Generate Visual Studio project files**.
   - On Windows 11 the item may be under **Show more options** (or press `Shift+F10` on the file).
   - If the menu item is missing entirely, see the triage table (§10, row 1).
3. A console window runs UnrealBuildTool for a few seconds and produces `PaintForge.sln` plus
   `Intermediate\ProjectFiles\`.

Do **not** double-click the `.uproject` yet — the editor can't open the project until the module
is compiled.

## 5. First build (Development Editor | Win64)

1. Open **`PaintForge.sln`** in Visual Studio 2022.
2. In the toolbar set the configuration to **Development Editor** and the platform to **Win64**.
3. In Solution Explorer, expand the **Games** folder, right-click the **PaintForge** project →
   **Set as Startup Project**.
4. **Build → Build Solution** (`Ctrl+Shift+B`). First build compiles the game module against the
   installed engine — expect **5–20 minutes** depending on CPU. Subsequent builds are incremental
   and take seconds.
5. Zero errors expected. Warnings are treated as errors by the build settings — if the build
   fails, go to the triage table in §10 before touching any code.

## 6. First editor open

Press **F5** (run with debugger) or **Ctrl+F5** (without) in Visual Studio. The Unreal Editor
launches with the PaintForge project.

**Expected on the very first open:** `Config/DefaultEngine.ini` points the editor startup map and
game default map at `/Game/Maps/L_Graybox` — a map that **does not exist yet** (the repo ships
`Content/` empty by design). The editor will open an untitled/default level instead and the Output
Log will warn that the startup map could not be found. That is normal; fix it in the next step.

Also check the **Output Log** (Window → Output Log) for the `PaintForgeLog` category: module
startup verifies the engine BasicShapes assets and the `BasicShapeMaterial` `Color` parameter with
`ensureMsgf`. Any ensure failure here means an engine-content mismatch — stop and investigate
before continuing.

## 7. Create and save `L_Graybox` (one-time, contract T17)

The one and only content file. It is an **empty level container** — all world geometry, lighting,
and spawn zones are spawned by C++ at runtime. Never author actors into it.

Exact clicks:

1. **File → New Level…** (`Ctrl+N`).
2. In the "New Level" dialog select the **Empty Level** template → click **Create**.
3. **File → Save Current Level As…** — the "Save Level As" asset dialog opens.
4. In the save dialog's folder tree, select **Content** (shown as `/Game`), then click the
   **Create New Folder** icon and name the folder exactly **`Maps`**.
   (The repo already has `Content/Maps/.gitkeep` on disk, but the editor may not show the folder
   until it contains an asset — creating/selecting it in the dialog is fine either way.)
5. Select the `Maps` folder, set **Name** to exactly **`L_Graybox`** → click **Save**.
6. Verify: the Content Browser now shows `Content/Maps/L_Graybox`, and on disk there is
   `D:\projects\paintforge\Content\Maps\L_Graybox.umap`.
7. Restart the editor once and confirm it now opens straight into `L_Graybox` (the
   `EditorStartupMap` ini setting is already pointed at it).
8. **Commit the map from this PC** (the only `.uasset`/`.umap` that will ever be committed in v1):

   ```bat
   git add Content\Maps\L_Graybox.umap
   git commit -m "Add L_Graybox empty level container (T17)"
   git push
   ```

## 8. Editor preferences & PIE multiplayer settings

PaintForge is server-authoritative multiplayer; **always test with 2+ players**, even for
"single-player" features. The host-works/client-breaks asymmetry is the #1 bug class (risk R5).

1. **Editor Preferences → Level Editor → Play** (this backs the toolbar Play dropdown):
   - **Multiplayer Options → Number of Players: `2`** (raise to 3 when testing spectate/vote
     edge cases).
   - **Net Mode: `Play As Listen Server`** — one PIE window is the host (server+client in one
     process), the other is a pure network client. This matches how real matches run.
   - Leave **Run Under One Process = ON** for everyday iteration (fast). **Periodically turn it
     OFF** — separate client processes surface real replication/initial-bunch bugs that
     one-process PIE hides. Do an under-separate-processes pass before calling any networked
     feature done.
2. The same options are reachable from the level-editor toolbar: the **⋮ (kebab) dropdown next to
   the Play button** → Number of Players / Net Mode.
3. **Live Coding** (`Ctrl+Alt+F11`): fine for `.cpp`-only tweaks. For anything touching headers,
   `UPROPERTY`s, or `UFUNCTION`s, **close the editor and rebuild in VS instead** — Live Coding +
   reinstancing on a native-only project causes ghost state.
4. Useful console commands while testing (PIE window, `~`):
   - `p.NetShowCorrections 1` — visualize movement mispredictions (must stay quiet during
     sprint/slide/ADS).
   - `Net PktLag=100` — emulate 100 ms latency; the movement/shooting feel contract must hold
     under this.

**Run the loop:** press **Play**. Two windows appear, each possessing its own pawn in the warm-up
pen. Walk, jump, sprint, slide, and shoot the target dummies in both windows; ready up (`F`) in
both to advance Lobby → Build and walk the whole loop through to the Results screen. A match JSON
should appear under `Saved\Arenas\` on the host after the vote phase.

To test across two physical machines on the LAN: host runs a listen server (PIE or packaged),
client uses the console command `open <host-ip>`.

## 9. Iteration habits (summary)

- Code lives in VS; build **Development Editor | Win64**; F5 to run.
- Full editor restart after header/UPROPERTY changes; Live Coding for `.cpp` bodies only.
- Test everything with 2 PIE players, listen server; periodically with separate processes.
- Warnings-as-errors stays ON. Fix the warning, don't suppress it.
- Don't add assets to `Content/` — v1 is zero-editor-assets; `L_Graybox.umap` is the only
  exception, and it stays empty.

## 10. First-compile / first-run error triage

| # | Symptom | Likely cause | Fix |
|---|---|---|---|
| 1 | Right-clicking `.uproject` shows no **Generate Visual Studio project files** | Epic's shell handler not registered | Install/launch the Epic Games Launcher once with UE 5.6 installed. Still missing: run `C:\Program Files (x86)\Epic Games\Launcher\Engine\Binaries\Win64\UnrealVersionSelector.exe` once, then retry. |
| 2 | Generate step asks to "Select Unreal Engine version" or errors about `EngineAssociation` | UE 5.6 not installed, or only another major version present | Install 5.6.x via the launcher (§2). The `.uproject` pins `"EngineAssociation": "5.6"`. |
| 3 | Generate/build fails with MSBuild / .NET SDK / `dotnet` not found | **.NET desktop development** workload missing | VS Installer → Modify → add the workload → regenerate project files. |
| 4 | `error C1083: Cannot open include file: '...generated.h'` | Stale or missing UnrealHeaderTool output | Close VS/editor, delete `Intermediate\` and `Binaries\`, regenerate project files (§4), rebuild. |
| 5 | Hundreds of errors on the very first file compiled | Wrong configuration selected | Toolbar must read **Development Editor** + **Win64**. `Development` (no "Editor") or Win32/x86 will not build this project. |
| 6 | `LNK2019: unresolved external symbol` in PaintForge code | Partial/incremental build confusion after switching configs or pulling | **Build → Rebuild Solution**; if it persists, do the full clean in row 4. |
| 7 | Double-clicking `.uproject` says *"PaintForge could not be compiled. Try rebuilding from source manually."* | Editor launched before the module was built | Build in VS first (§5), then launch. Never accept the editor's own compile prompt for the first build — you want VS's error list. |
| 8 | Build is glacially slow / disk thrashing | Windows Defender scanning every compile artifact | Add exclusions for `D:\projects\paintforge` and the `UE_5.6` engine folder (Windows Security → Virus & threat protection → Exclusions). |
| 9 | Editor opens an untitled level; log warns startup map missing | `L_Graybox` not created yet | Expected before §7 — create and save the map. |
| 10 | Editor boot shows `ensureMsgf` failure about BasicShapes / `Color` parameter | Engine content moved/renamed in your engine install | Verify the install is stock 5.6 via launcher (Library → dropdown on the tile → Verify). Do not silence the ensure. |
| 11 | UBT complains the MSVC toolchain version is unsupported/too new | A newer v14.4x toolset than the engine supports | VS Installer → Individual components → install the exact **MSVC v143** version UBT names in its error output, or update to the latest UE 5.6 hotfix. |
| 12 | Input dead in PIE after ~a minute, or on clients only | Enhanced Input objects GC'd / context applied too early (risk R1) | That's a code bug, not setup — check `UPFInputConfig` UPROPERTY rooting and the `OnPossess`/`BeginPlayingState` retry before filing anything else. |
| 13 | Feature works in the host window, broken in the client window | Listen-server asymmetry (risk R5) | Also a code bug: gameplay writes need `HasAuthority()`, cosmetics keyed off OnRep/multicast, owner-only via `IsLocallyControlled()`. Reproduce with Run Under One Process OFF and fix in code. |

## 11. Packaging (later — not part of first setup)

For the first playable milestone (not before, not the last week): Project Settings → Packaging →
*Use Pak File* ✓, *Use Io Store* ✓, List of maps = `L_Graybox`; Platforms → Windows → Package
(**Shipping**). Engine assets referenced via `FObjectFinder` cook automatically. Smoke-test the
Shipping package early — packaging failures are the classic end-of-project landmine.
