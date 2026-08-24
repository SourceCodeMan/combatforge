# CombatForge

**Build the arena. Fight in it. Judge it.**

CombatForge is a multiplayer first-person **airsoft** shooter for Windows, built on Unreal Engine 5.6.
Nothing is lethal: you tag people and they're *out*, then they respawn. It's built to be played by
kids and adults in the same lobby.

Every match is three games in one:

1. **Build** — teams get 3 minutes and a piece budget to build their half of the arena on a snap
   grid: walls, floors, ramps, ceilings, doors, one-way doors, windows, trap floors, and cover props
   (barrels, crates, box stacks). The enemy half is visible while you build, so counter-building mind
   games start immediately.
2. **Fight** — CoD-feel gunplay in the arena you just made. Sprint, slide, mantle, ADS, true
   projectiles with drop, a 36-weapon catalog, melee tag, smoke and frag, and a plantable breach
   charge for when someone walls themselves in.
3. **Vote** — everyone judges the arena. Thumbs up/down plus category chips (layout, cover,
   verticality, flow, balance, sightlines, creativity, pacing), persisted against a content-derived
   fingerprint so good arenas can be found, replayed, and remixed.

The arena **is** the content. Players make the map; CombatForge's job is to make that fast, fair,
and rateable.

> **Status: shipping alpha.** Live on itch.io, with accounts, progression, and a dedicated server.
> This is no longer the graybox milestone the first version of this README described.

---

## Where it runs

| Surface | What it is |
|---|---|
| **itch.io** | `thathorseslayer/combatforge`, channel `windows-alpha`. Pushed with `butler`. |
| **Dedicated server** | A Vultr Windows box, port 7777, registered in the public server directory. |
| **Backend** | `api.playcombatforge.com` — Cloudflare Worker + D1. Accounts, progression, unlocks, server directory, match reports. Separate repo: `combatforge-api`. |
| **Brand** | [playcombatforge.com](https://playcombatforge.com) · [Discord](https://discord.gg/f7U2xXxAxc) · bug reports at `report.playcombatforge.com` |

### The version gate — read this before shipping anything

`PFBuild::NetProtocol` in [`Source/CombatForge/CombatForge.h`](Source/CombatForge/CombatForge.h) is
folded into the engine's network version. Clients on a different number **cannot join** — by design,
because a stale client silently mis-renders instead of failing loudly.

**Bump it on every itch push, and push the client and the server together.** They are two halves of
one release. Shipping one without the other means nobody can play, and the symptom (a server that
looks fine but rejects everyone) costs an hour to diagnose. It has.

---

## Playing and hosting

**Play:** download from itch, run `CombatForge.exe`. Sign in for progression, or play offline —
everything except XP and unlocks works logged out.

**Host locally** (LAN / VPN, no backend needed):

```powershell
.\Deploy\playtest\run-listen.ps1        # host + play on this PC (port 7777)
.\Deploy\playtest\print-host-ips.ps1    # share the LAN/VPN IP
.\Deploy\playtest\run-server.ps1        # headless -server -nullrhi instead
```

**Quick first match:** TYPE=Skirmish, MODE=Play-Only, FORMAT=4v4.

---

## Match anatomy

**Build modes** — Creative (build from empty) · Remix (load a community arena and build on top) ·
Play-Only (skip building).

**Match types** — Elimination (round-based, first to N) · Skirmish (team tags to a score) ·
Free-for-All · Capture the Flag · Domination (hold 3 points) · Hardpoint (rotating point).

**Maps** — *Warehouse* (6400×4000 indoor CQB) · *The Yard* (6400×8000 open-air, no roof) · plus any
saved community arena.

**Getting tagged** — locational, not a single health pool: **3** head hits, **5** chest, **8** limb,
or **10** anywhere and you're out. Snipers carry a higher per-hit value, so they tag in one.

**Build phase** is 180 s. Combat length depends on the match type.

---

## What's in it

**Weapons** — 36 across six categories (Assault Rifle, SMG, Pistol, Shotgun, Sniper, LMG), each with
its own fire modes, recoil climb, bloom, ADS time, reload, and unlock rank. Per-weapon first-person
and ADS poses are hand-tuned and baked into the catalog.

**Two weapon slots** — a primary and a second weapon carried on the back, swapped with the scroll
wheel. Any category in either slot.

**Classes** — five saved loadout slots, each with its own clothing and both weapons. Built from a
modular character with ~330 parts across 9 slots.

**Movement** — sprint, slide, crouch (hold or toggle), double-tap-space mantle, and a fall model
that can tag you out.

**Bots** — navmesh pathfinding over the arena *you just built*, sight cones, hearing, last-known
position, cover and flanking, squad awareness, objective play, and doors they can open. They roll
their own weapons rather than mirroring yours.

**Progression** — XP, ranks, and rank-gated weapon unlocks, held server-side. The menu shows locked
weapons in amber and names what you're actually carrying, so it can never disagree with what spawns.

---

## Repository layout

```
CombatForge.uproject          UE 5.6, module: CombatForge
Config/                       Engine / Game / Input ini — DirectoriesToAlwaysCook lives here
Content/                      Character, weapon, warehouse and animation packs (LFS)
Source/CombatForge/
  CombatForge.h               PFBuild::NetProtocol — the multiplayer version gate
  Core/                       Types, GameInstance, GameMode, GameState, PlayerController,
                              PFPaths (persistent data locations)
  Player/                     CombatForgeCharacter (one pawn, both phases), movement component,
                              modular character customization, menu preview actor
  Combat/                     Weapon component + 36-weapon catalog, projectiles, health,
                              grenades, bomb, ammo barrels, audio, VFX
  Building/                   Build component, replicated grid (FastArray + ISMs), grid math,
                              piece visuals, special piece actors (doors/windows/traps), arena shell
  AI/                         Bot controller — perception, tactics, squad coordination
  Objectives/                 Flag, control point, objective layout
  Online/                     PFBackendSubsystem — accounts, directory, progression, fleet reporting
  Voting/                     Rating subsystem — match records, votes, community map catalog
  UI/                         Every widget, procedural C++ UMG. No Blueprints anywhere.
Deploy/
  playtest/                   Local host + packaging scripts
  pilot/                      Dedicated-server deploy + log export (see below)
docs/                         Design docs, plans, runbooks, post-mortems
```

**~47,000 lines of hand-written C++ across 119 files. Zero Blueprints** — all UI, input, and content
loading is procedural, which is why everything here is greppable and diffable.

---

## Where your data lives

This has bitten us more than once, so it's written down.

| Data | Location | Why |
|---|---|---|
| Community maps | `%LOCALAPPDATA%\CombatForge\Arenas` | Survives every reinstall and redeploy |
| Your settings, classes, keybinds | `%LOCALAPPDATA%\CombatForge\UserPrefs.ini` | Same reason — see below |
| Server key, pending XP reports | `%ProgramData%\CombatForge` (or `-ArenaDir`) | Must survive a server redeploy |
| Login token | `%LOCALAPPDATA%\CombatForge\Auth.json` | Per-user, DPAPI-protected on Windows, and **never** inside the package |

**Nothing player-owned may live inside the install folder.** The pre-ship scrub deletes
`<package>\CombatForge\Saved` on every bake — it has to, because a session token once shipped to
every alpha download from there. Anything stored in that folder is destroyed on every build, which is
exactly how player settings used to reset to defaults after each bake.

---

## Shipping a build

```powershell
# 1. Bump PFBuild::NetProtocol in Source/CombatForge/CombatForge.h
# 2a. Playtest/itch Development package (editor must be closed)
.\Deploy\playtest\package-playtest.ps1

# 2b. Epic/store Shipping package (clean cook, distribution, IoStore, compressed, prerequisites)
.\Deploy\playtest\package-playtest.ps1 -Config Shipping
# Output: Packaged\Release\Windows; includes CombatForge-build.json with commit/protocol/hash

# 3. Push to itch
butler push "D:\projects\combatforge\Packaged\Playtest\Windows" `
  thathorseslayer/combatforge:windows-alpha --userversion 0.1.0-alpha.N
```

**Then deploy the server in the same sitting.** On the PC, serve the build folder:

```powershell
cd D:\projects\combatforge\Deploy\pilot\serve
python serve-and-receive.py                     # NOT `python -m http.server` — see below
cloudflared tunnel --url http://localhost:8000  # second window
```

On the box:

```powershell
cd C:\Users\Administrator\Desktop
curl.exe -L -o Deploy-OnBox.ps1 "<tunnel-url>/Deploy-OnBox.ps1"   # always re-pull it
.\Deploy-OnBox.ps1
```

It auto-discovers the zip, protects `ServerKey.txt`, stops the old server, downloads, extracts,
verifies, and relaunches. Watch for `Found: CombatForge-alphaN.zip`, then
`Backend: fleet registered (port 7777)`.

**Before shipping, check:** exactly one zip in the serve folder, no `ServerKey.txt` and no
`GameUserSettings.ini` inside it, and the version tag in the client menu reading
`Client vN  Server vN (in sync)` afterwards. Every one of those checks exists because its absence
cost a bad deploy.

### Getting logs off the server

```powershell
# PC (in Deploy\pilot\serve): python serve-and-receive.py  + cloudflared
# box:
.\Export-ServerLogs.ps1
```

Lands in `Deploy\pilot\serve\inbox\`. Redacts the server key from log bodies — UE writes the full
command line into every log header — and the inbox is write-only over the tunnel.

`serve-and-receive.py` is a drop-in replacement for `python -m http.server` that also accepts
uploads. The stock module is download-only, so the box can pull a build but can't send anything back.

---

## Building from source

Windows, Visual Studio 2022, UE 5.6. Full walkthrough:
[`docs/pc-setup.md`](docs/pc-setup.md).

1. Install UE 5.6 and VS 2022 (*Game development with C++*, *.NET desktop development*).
2. Clone to `D:\projects\combatforge` — content packs are Git LFS, so `git lfs install` first.
3. Right-click `CombatForge.uproject` → **Generate Visual Studio project files**.
4. Open `CombatForge.sln`, **Development Editor | Win64**, build, launch.

Handy console commands: `pf.SetRank <n>` (alpha only — unlock the catalog for testing),
`pf.WeaponFP` / `pf.WeaponADS` / `pf.WeaponTP` (live pose tuning, prints paste-ready values),
`pf.ShowMuzzle`, `pf.NavCheck`.

---

## Documentation worth knowing about

- [`docs/design/05-code-contract.md`](docs/design/05-code-contract.md) — class APIs, gameplay
  numbers, UE 5.6 correctness rules. Where docs conflict, the contract wins.
- [`docs/design/01-game-design.md`](docs/design/01-game-design.md) · [`02-architecture.md`](docs/design/02-architecture.md) ·
  [`03-build-system.md`](docs/design/03-build-system.md) · [`04-combat-feel.md`](docs/design/04-combat-feel.md)
- [`docs/multiplayer-plan.md`](docs/multiplayer-plan.md) — backend, accounts, fleet
- [`docs/itch-deploy.md`](docs/itch-deploy.md) — butler runbook
- [`docs/weapon-balance-plan.md`](docs/weapon-balance-plan.md) — the catalog's stat and rank design
- [`docs/confidence-audit-2026-07-19.md`](docs/confidence-audit-2026-07-19.md) — an honest audit of
  decisions made under uncertainty, and the known-risk list that came out of it. Read before
  trusting a constant you find in the code.

## House rules learned the hard way

- **Verify, don't infer.** A bone existing doesn't mean it's animated. A file-size delta doesn't
  prove a bake worked. A log line saying "migrated 31 prefs" doesn't mean a file was written — check
  the disk. Most of the expensive bugs here came from treating an inference as an observation.
- **Derive from data, don't hand-tune constants.** The third-person weapon pose was fixed by reading
  the hand-to-hand vector out of the animation instead of guessing offsets; the back sling by
  measuring the torso from its bones. Hand-authored offsets are wrong for every pose but one.
- **PowerShell scripts must be ASCII-only.** PS 5.1 reads `.ps1` as ANSI without a BOM, so an
  em-dash becomes mojibake and breaks parsing on the server.
- **One source of truth per script.** A stale copy of a deploy script in an old folder once shipped a
  two-version-old build while reporting success at every step.
