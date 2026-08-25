# Combat Forge — Packaging & Cross‑Platform (Windows + Mac)

*How to produce standalone, playable builds. Shipping/LAN-only path updated 2026-08-25.*

The project is already set up for packaging: a **Game target** (`Source/CombatForge.Target.cs`), a **Server target**, pak/IoStore on, and a cook list in `Config/DefaultGame.ini`. This doc adds the Mac path, the package scripts, and the gotchas.

---

## TL;DR

```
# Windows (on the PC, editor closed):
Scripts\Package-Windows.bat            # → Packaged\Playtest\Windows (Development)
Scripts\Package-Windows.bat Shipping   # → Packaged\Release\Windows (clean store build)

# Mac (ON A MAC with UE 5.6 + Xcode, content synced):
./Scripts/check-mac-env.command
./Scripts/Package-Mac.command Shipping # → Packaged/Mac/  (clean Shipping app + manifest)
```

Both produce a self-contained archive. The Windows batch file is only a compatibility wrapper around
`Deploy/playtest/package-playtest.ps1`, which is the single Windows packaging implementation.

---

## 1. Prerequisites

**Windows (the PC — already set up, this is what builds today):**
- UE 5.6 installed, Visual Studio 2022 + the C++ / game‑dev workload (MSVC).
- The project with all paid Fab/LFS content fully hydrated, including `Content/Bandits/`.

**Mac (needed only for the Mac build — UE can't cross‑compile to macOS):**
- A Mac, ideally Apple Silicon (M‑series). Your MacBook qualifies.
- UE 5.6 from the Epic Games Launcher.
- **Xcode** + command‑line tools: `xcode-select --install`.
- The full private project synced with Git LFS, including `Content/Bandits/`; run `git lfs pull`.
- Disk: UE + this project + a cooked Mac build is well over 100 GB. Have room.

---

## 2. What the scripts do

Both call `RunUAT BuildCookRun`, which **compiles → cooks content → stages → paks → archives** into `Packaged/<Platform>/`:

- `-build` compile the game target · `-cook` cook content (uses the cook list in `DefaultGame.ini`) · `-stage -pak -iostore -compressed` bundle into compressed pak/IoStore files (matches the project's `bUseIoStore` / `UsePakFile`) · `-archive` copy the finished build out · `-nocompileeditor` skip the editor (not needed for a client build).

The Mac script finds the project relative to itself. If UE is not at the default Launcher path, use
`UE="/path/to/UE_5.6" ./Scripts/Package-Mac.command Shipping`; `PROJ` and `OUT` are also supported.

---

## 3. Build config: Development vs Shipping

Project Settings defaults to **Shipping** so an editor-driven store package cannot accidentally
retain development features. The Windows compatibility wrapper defaults to **Development** for local
playtests; the Mac package script defaults to **Shipping**:
- Keeps the console + all `pf.*` debug cvars (`pf.NavCheck`, `pf.BotSkill`, `pf.WeaponFP`, …). **This is what you want for playtests.**

For a **release** build (clean cook, distribution, IoStore, compression, prerequisites, no debug
symbols/console), run `Package-Windows.bat Shipping`,
`Deploy\playtest\package-playtest.ps1 -Config Shipping`, or
`./Scripts/Package-Mac.command Shipping`. The unsigned itch Mac path intentionally omits UAT's
`-distribution` flag because UE 5.6 otherwise enters the Xcode archive workflow instead of emitting
the directly distributable `.app`.

---

## 4. Content & the cook list (important)

The cook only includes what it can find. Structural pieces use engine primitives (always present), but the **soft‑loaded** content must be force‑cooked — it's listed under `DirectoriesToAlwaysCook` in `DefaultGame.ini`:

- `/Game/Bandits` — **default character body + AK/AKSU/pistol meshes.** Added specifically because these are loaded by string path in C++ (`FSoftObjectPath::TryLoad`), so the cooker can't discover them via hard references. **Omit this and the package ships bodyless characters with no weapons.** It's the ~12 GB local pack — it must be on the build machine (and synced to the Mac for a Mac build).
- `/Game/Scene_Warehouse`, `/Game/QuantumCharacter`, `/Game/Survival_Character`, `/Game/AnimStarterPack`, `/Game/Free_Sounds_Pack`, `/Game/Materials`, `/Game/Textures`, `/Game/Weapons` — surfaces, alt characters, anims, sounds.
- Maps cooked: `L_Graybox` (the game) + the warehouse showcase map.

If you add new soft‑loaded content later, add its folder here or it won't ship.

### Bandit pack: texture trim + tracking

The Bandit pack shipped with **4K textures (~8.7 GB)** — overkill for a graybox indie game. `Scripts/trim_bandit_textures.py` caps every Bandit texture's *Maximum Texture Size* to **1024** (tunable at the top; bump to 2048 if hero weapon/arms look soft up close). Run it headless after (re)adding the pack on any machine:

```
"<UE>\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" CombatForge.uproject -run=pythonscript -script="...\Scripts\trim_bandit_textures.py"
```

- **Effect:** the **cooked package** uses ≤1K textures (~1/16 the pixels of 4K) → dramatically smaller build. Non‑destructive + reversible (set `max_texture_size` back to `0`).
- **What it does NOT do:** shrink the dev‑side `.uasset` files. UE keeps the full‑res *source* in the asset to re‑cook per platform, so `Content/Bandits` stays ~12 GB on disk. The cap only bites at cook time.

**Tracking/licensing:** paid Fab content is tracked through Git LFS for private collaborator access.
The repository must remain private, and purchase receipts, tier-at-purchase, and per-pack license
records must be retained. Never publish source assets or LFS objects as standalone downloads.

---

## 5. Mac‑specific checklist

The Mac uses **Metal**, not Direct3D. The C++ is engine‑abstracted and should port clean, but rendering needs attention:

1. **Metal SM6 for Nanite / Virtual Textures.** The warehouse + Bandit content uses Nanite + VT, which need **Metal SM6** on Apple Silicon. Set it authoritatively in the **Mac editor**: *Project Settings → Platforms → Mac → targeted RHI / Metal shader standard → SM6*. (Do it in the editor rather than hand‑editing ini — it writes the exact keys for this engine build. It'll land in `Config/DefaultEngine.ini` under `[/Script/MacTargetPlatform.MacTargetSettings]`, which we deliberately left for the Mac to generate.)
2. **Performance is the real unknown.** This content already stresses the PC (the texture‑streaming pool is bumped to 3 GB, `r.Streaming.PoolSize=3000`). A MacBook — especially a non‑Pro — may need scalability dialed (lower streaming pool, shadow/AA settings, possibly disabling Nanite fallback quirks). Budget time to tune `DefaultDeviceProfiles.ini` for Mac. **Test before assuming it runs well.**
3. **First C++ build on the Mac is slow** (compiles the whole module under clang). Subsequent builds are incremental.
4. **Gatekeeper.** The `.app` is **unsigned**, so macOS blocks it ("unidentified developer"). For friends: **right‑click the app → Open** the first time (per‑user unblock). For real distribution: an **Apple Developer Program** membership ($99/yr) to codesign + notarize.
5. **Run the preflight first.** `Scripts/check-mac-env.command` checks Xcode/UE compatibility,
   Mac target modules, Metal tools, LFS hydration, and the tracked Metal SM6 config before the cook.
6. **Archive integrity is gated.** UE 5.6 sometimes copies a pak-less wrapper into the archive;
   `Package-Mac.command` detects that case, recovers the complete staged app, scrubs it, and emits
   `CombatForge-build.json` bound to the exact clean Git commit.

---

## 6. Cross‑play & connectivity

- **LAN (works now, cross‑platform):** the game is a **listen server** — one machine hosts (the beefy PC should host; it's authoritative for physics + all the AI), everyone else joins the host's LAN IP. A Mac client joining a PC host is fine; the netcode is OS‑agnostic. Same engine version + matching build required.
- **Internet play (NOT yet):** there's **no online subsystem** integrated (`Build.cs` deliberately omits it). Over the internet you'd need NAT traversal + sessions — add **EOS (Epic Online Services — free, cross‑platform, handles invites/sessions/NAT)** or Steam, or fall back to fragile port‑forwarding. That's a separate, real chunk of work.

---

## 7. Distributing the LAN-only Alpha

- Windows Shipping output is `Packaged/Release/Windows`; macOS resolves the actual publish directory
  under `Packaged/Mac`. Each contains `CombatForge-build.json` beside the client artifact.
- Use `Scripts/Push-Itch.ps1` on Windows or `Scripts/Push-Itch.command` on macOS. Both are dry-run by
  default and refuse non-Shipping, stale, dirty, or hash-mismatched artifacts.
- **Size:** current itch downloads are about **3.7 GB** after cooking/compression; the hydrated source
  checkout is much larger. Butler uploads directories and transfers deltas after the first release.
- **Licensing (only matters if you go public):** Megascans/Quixel content is free to ship inside a UE game. The **Bandit character pack**'s license should be checked before redistributing it publicly — marketplace character packs sometimes restrict redistribution. Fine for LAN/personal use.
- The current itch release is explicitly **LAN-only**. Official servers, Quick Play, and public
  matchmaking are upcoming features and must not appear as currently playable store claims.

---

## 8. First‑time verification checklist

The cheap way to learn the truth (per the deployment discussion): package **Windows** first, then **Mac**, and check:

- [ ] **Windows Shipping package builds** (`Deploy\playtest\package-playtest.ps1 -Config Shipping`), editor closed.
- [ ] Launch `Packaged\Release\Windows\CombatForge.exe` standalone — menu says **NO OFFICIAL SERVERS AVAILABLE**, characters have **bodies + weapons**, and a full bot match runs.
- [ ] **Mac Shipping package builds** on the Mac (`./Scripts/Package-Mac.command Shipping`) — confirms the C++ compiles under clang and content cooks for Metal.
- [ ] Launch the `.app` (right‑click → Open) — **renders on Metal** (Nanite/VT not black/broken) and is **playable framerate** on the MacBook.
- [ ] **Cross‑play:** PC hosts a LAN match, Mac joins by IP — both see each other, combat works.

That sequence surfaces ~80% of the real risk (clang portability, Metal rendering, Mac perf, cross‑platform netcode) before investing in internet play or a storefront.

---

## Files
- `Config/DefaultGame.ini` — `[/Script/UnrealEd.ProjectPackagingSettings]` (build config, cook list).
- `Config/DefaultEngine.ini` — Windows RHI (D3D12 SM5+SM6); Mac RHI to be generated on the Mac.
- `Scripts/Package-Windows.bat` · `Scripts/Package-Mac.command` — the package commands.
- `Source/CombatForge.Target.cs` (Game) · `Source/CombatForgeServer.Target.cs` (Server, for a future dedicated server).
