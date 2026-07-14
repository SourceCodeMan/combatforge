# Combat Forge — Packaging & Cross‑Platform (Windows + Mac)

*How to produce standalone, playable builds. Written 2026‑07‑14 — stored for when we actually ship a build. Nothing here has been run end‑to‑end yet; the config + scripts are authored and ready.*

The project is already set up for packaging: a **Game target** (`Source/PaintForge.Target.cs`), a **Server target**, pak/IoStore on, and a cook list in `Config/DefaultGame.ini`. This doc adds the Mac path, the package scripts, and the gotchas.

---

## TL;DR

```
# Windows (on the PC, editor closed):
Scripts\Package-Windows.bat            # → Packaged\Windows\  (Development)
Scripts\Package-Windows.bat Shipping   # → clean release build

# Mac (ON A MAC with UE 5.6 + Xcode, content synced):
./Scripts/Package-Mac.command          # → Packaged/Mac/  (Development)
```

Both produce a **self‑contained folder** you can zip and hand to someone. LAN cross‑play (Mac client ↔ PC host) works out of the box; internet play does **not** yet (no online subsystem — see §6).

---

## 1. Prerequisites

**Windows (the PC — already set up, this is what builds today):**
- UE 5.6 installed, Visual Studio 2022 + the C++ / game‑dev workload (MSVC).
- The project with all content present, including `Content/Bandits/` (~12 GB, untracked).

**Mac (needed only for the Mac build — UE can't cross‑compile to macOS):**
- A Mac, ideally Apple Silicon (M‑series). Your MacBook qualifies.
- UE 5.6 from the Epic Games Launcher.
- **Xcode** + command‑line tools: `xcode-select --install`.
- The full project synced over, **including `Content/Bandits/`** (git doesn't track it — copy it manually, e.g. external drive or LAN).
- Disk: UE + this project + a cooked Mac build is well over 100 GB. Have room.

---

## 2. What the scripts do

Both call `RunUAT BuildCookRun`, which **compiles → cooks content → stages → paks → archives** into `Packaged/<Platform>/`:

- `-build` compile the game target · `-cook` cook content (uses the cook list in `DefaultGame.ini`) · `-stage -pak -iostore -compressed` bundle into compressed pak/IoStore files (matches the project's `bUseIoStore` / `UsePakFile`) · `-archive` copy the finished build out · `-nocompileeditor` skip the editor (not needed for a client build).

Edit the `UE` / `PROJ` / `OUT` paths at the top of each script if your install differs (the **Mac** paths are placeholders — set them on the Mac).

---

## 3. Build config: Development vs Shipping

Default is **Development** (`DefaultGame.ini` → `BuildConfiguration=PPBC_Development`):
- Keeps the console + all `pf.*` debug cvars (`pf.NavCheck`, `pf.BotSkill`, `pf.WeaponFP`, …). **This is what you want for playtests.**

For a **release** build (smaller, faster, no console/debug):
- Pass `Shipping` to the script (`Package-Windows.bat Shipping`), and for a real distributable also flip `BuildConfiguration=PPBC_Shipping` + `ForDistribution=True` in `DefaultGame.ini`.

---

## 4. Content & the cook list (important)

The cook only includes what it can find. Structural pieces use engine primitives (always present), but the **soft‑loaded** content must be force‑cooked — it's listed under `DirectoriesToAlwaysCook` in `DefaultGame.ini`:

- `/Game/Bandits` — **default character body + AK/AKSU/pistol meshes.** Added specifically because these are loaded by string path in C++ (`FSoftObjectPath::TryLoad`), so the cooker can't discover them via hard references. **Omit this and the package ships bodyless characters with no weapons.** It's the ~12 GB local pack — it must be on the build machine (and synced to the Mac for a Mac build).
- `/Game/Scene_Warehouse`, `/Game/QuantumCharacter`, `/Game/Survival_Character`, `/Game/AnimStarterPack`, `/Game/Free_Sounds_Pack`, `/Game/Materials`, `/Game/Textures`, `/Game/Weapons` — surfaces, alt characters, anims, sounds.
- Maps cooked: `L_Graybox` (the game) + the warehouse showcase map.

If you add new soft‑loaded content later, add its folder here or it won't ship.

---

## 5. Mac‑specific checklist

The Mac uses **Metal**, not Direct3D. The C++ is engine‑abstracted and should port clean, but rendering needs attention:

1. **Metal SM6 for Nanite / Virtual Textures.** The warehouse + Bandit content uses Nanite + VT, which need **Metal SM6** on Apple Silicon. Set it authoritatively in the **Mac editor**: *Project Settings → Platforms → Mac → targeted RHI / Metal shader standard → SM6*. (Do it in the editor rather than hand‑editing ini — it writes the exact keys for this engine build. It'll land in `Config/DefaultEngine.ini` under `[/Script/MacTargetPlatform.MacTargetSettings]`, which we deliberately left for the Mac to generate.)
2. **Performance is the real unknown.** This content already stresses the PC (the texture‑streaming pool is bumped to 3 GB, `r.Streaming.PoolSize=3000`). A MacBook — especially a non‑Pro — may need scalability dialed (lower streaming pool, shadow/AA settings, possibly disabling Nanite fallback quirks). Budget time to tune `DefaultDeviceProfiles.ini` for Mac. **Test before assuming it runs well.**
3. **First C++ build on the Mac is slow** (compiles the whole module under clang). Subsequent builds are incremental.
4. **Gatekeeper.** The `.app` is **unsigned**, so macOS blocks it ("unidentified developer"). For friends: **right‑click the app → Open** the first time (per‑user unblock). For real distribution: an **Apple Developer Program** membership ($99/yr) to codesign + notarize.

---

## 6. Cross‑play & connectivity

- **LAN (works now, cross‑platform):** the game is a **listen server** — one machine hosts (the beefy PC should host; it's authoritative for physics + all the AI), everyone else joins the host's LAN IP. A Mac client joining a PC host is fine; the netcode is OS‑agnostic. Same engine version + matching build required.
- **Internet play (NOT yet):** there's **no online subsystem** integrated (`Build.cs` deliberately omits it). Over the internet you'd need NAT traversal + sessions — add **EOS (Epic Online Services — free, cross‑platform, handles invites/sessions/NAT)** or Steam, or fall back to fragile port‑forwarding. That's a separate, real chunk of work.

---

## 7. Distributing a build

- Zip `Packaged/Windows` (or `Packaged/Mac`) and share it. It's self‑contained.
- **Size:** expect **12 GB+** because of the Bandit + warehouse content. Big for casual sharing (external drive / large file transfer). Trimming would mean replacing the heavy Megascans/Bandit content with lighter assets — a separate art decision.
- **Licensing (only matters if you go public):** Megascans/Quixel content is free to ship inside a UE game. The **Bandit character pack**'s license should be checked before redistributing it publicly — marketplace character packs sometimes restrict redistribution. Fine for LAN/personal use.
- Storefronts if it ever gets that far: **itch.io** (simplest, both platforms, no signing enforced) or **Steam** ($100 Steam Direct; handles updates + matchmaking + cross‑play, biggest lift).

---

## 8. First‑time verification checklist

The cheap way to learn the truth (per the deployment discussion): package **Windows** first, then **Mac**, and check:

- [ ] **Windows package builds** (`Package-Windows.bat`), editor closed.
- [ ] Launch `Packaged\Windows\...\CombatForge.exe` standalone — characters have **bodies + weapons** (confirms the Bandit cook), the arena builds, a match runs.
- [ ] **Mac package builds** on the Mac (`Package-Mac.command`) — confirms the C++ compiles under clang and content cooks for Metal.
- [ ] Launch the `.app` (right‑click → Open) — **renders on Metal** (Nanite/VT not black/broken) and is **playable framerate** on the MacBook.
- [ ] **Cross‑play:** PC hosts a LAN match, Mac joins by IP — both see each other, combat works.

That sequence surfaces ~80% of the real risk (clang portability, Metal rendering, Mac perf, cross‑platform netcode) before investing in internet play or a storefront.

---

## Files
- `Config/DefaultGame.ini` — `[/Script/UnrealEd.ProjectPackagingSettings]` (build config, cook list).
- `Config/DefaultEngine.ini` — Windows RHI (D3D12 SM5+SM6); Mac RHI to be generated on the Mac.
- `Scripts/Package-Windows.bat` · `Scripts/Package-Mac.command` — the package commands.
- `Source/PaintForge.Target.cs` (Game) · `Source/PaintForgeServer.Target.cs` (Server, for a future dedicated server).
