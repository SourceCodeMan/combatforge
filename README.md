# PaintForge

**Build the arena. Fight in it. Judge it.**

PaintForge is a multiplayer first-person paintball shooter for Windows, built on Unreal Engine 5.6.
Every match is three games in one:

1. **Build** (Fortnite-style): each team gets 3 minutes and a per-player piece budget to build its
   own half of the arena on a snap grid — walls, floors, ramps, and classic speedball bunkers
   (cans, doritos, snakes). The enemy half is fully visible while you build, so counter-building
   mind games start immediately.
2. **Fight** (CoD-feel paintball): round-based elimination — first team to 4 round wins, no
   in-round respawn, teams swap halves every round so asymmetric arenas stay fair. Sprint, slide,
   snappy ADS, true-projectile paintballs with drop, 3 hits and you're out (mask hits count
   double).
3. **Vote**: the match ends with every player passing judgment on the arena — thumbs up/down plus
   liked/disliked category chips (layout, cover, verticality, flow, balance, sightlines,
   creativity, pacing). Every verdict is persisted as a backend-ready JSON record, keyed by a
   content-derived arena fingerprint, so great arenas (and great *halves*) can someday be
   aggregated, rated, and recombined into a community pool.

The arena **is** the content. Players make the map; PaintForge's job is to make that fast, fair,
and rateable.

## v1 graybox — what this repo is

This is the v1 **graybox** milestone: the complete match loop with zero editor-authored assets.

- **C++-first, zero assets:** every mesh is an engine BasicShape with a runtime material instance;
  all UI is C++ UMG; all input is natively-constructed Enhanced Input. The `Content/` folder ships
  empty except for one editor-created empty level container (see below).
- **Server-authoritative multiplayer from day 1:** listen server, connect by IP. Building,
  shooting, phases, votes — everything is validated on the server; clients predict for feel.
- **The five phases:** `Lobby → Build → Combat → Vote → Results`, one persistent level, no map
  travel. Lobby includes a warm-up pen with target dummies so the marker is testable before it
  matters.
- **Fake-and-verify shooting:** instant client-side cosmetic projectiles plus server-authoritative
  projectiles sharing a deterministic spread seed; hitmarkers only on server confirm.
- **Persistence:** one JSON file per match in `Saved/Arenas/` — the frozen arena layout, SHA1
  fingerprints (whole arena + per-half), the match result, and every player's anonymized vote.

## LAN / VPN playtest (friends on the network)

Connect-by-IP (no EOS yet). Use **current `main`**.

- Operator checklist: [`docs/playtest-checklist.md`](docs/playtest-checklist.md)
- Full host guide: [`Deploy/playtest/README.md`](Deploy/playtest/README.md)

```powershell
git pull origin main
.\Deploy\playtest\run-listen.ps1            # host + play on this PC (port 7777)
.\Deploy\playtest\print-host-ips.ps1        # share LAN/VPN IP with friends
# friends:  open <ip>:7777
.\Deploy\playtest\smoke-improvement.ps1     # automated inject smoke (optional)
```

**First kids match:** Lobby TYPE=Skirmish, MODE=Play-Only, FORMAT=4v4, Enter to start.

Launcher UE cannot build a true `PaintForgeServer` target; playtest uses the game binary
as listen or `-server -nullrhi`.

## Getting started (Windows)

The first compile and first run happen on a Windows PC with Visual Studio 2022 and UE 5.6.

**Follow [`docs/pc-setup.md`](docs/pc-setup.md) step by step.** It covers engine/VS installation,
the first build, creating the one required level file (`L_Graybox`), editor settings, and
2-player listen-server testing in PIE — plus a triage table for common first-compile errors.

Quick version:

1. Install UE 5.6 (Epic Games Launcher) and VS 2022 with the *Game development with C++* and
   *.NET desktop development* workloads.
2. Clone this repo to `D:\projects\paintforge`.
3. Right-click `PaintForge.uproject` → **Generate Visual Studio project files**.
4. Open `PaintForge.sln`, select **Development Editor | Win64**, build, launch.
5. First run only: create and save the empty level `/Game/Maps/L_Graybox` (exact clicks in
   [`docs/pc-setup.md`](docs/pc-setup.md)), then commit it.

## Repository layout

```
PaintForge.uproject             UE 5.6 project (module: PaintForge, Enhanced Input)
Config/                         DefaultEngine / DefaultGame / DefaultInput ini
Content/
  Maps/                         Ships empty; L_Graybox.umap is created once on the PC (T17)
Source/
  PaintForge.Target.cs          Game target
  PaintForgeEditor.Target.cs    Editor target
  PaintForge/
    PaintForge.h/.cpp           Module impl, log category, startup engine-asset checks
    Core/                       Types header, GameInstance, GameMode, GameState,
                                PlayerState, PlayerController (phase/round state machine)
    Player/                     Character (one pawn, both phases), custom movement
                                component (sprint/slide/ADS prediction), camera shakes
    Input/                      UPFInputConfig — all input actions/contexts built in C++
    Combat/                     Weapon component, paintball projectile, health (3-HP paint
                                model), splat subsystem, audio stubs, target dummy
    Building/                   Build component, replicated build grid (FastArray + ISMs),
                                grid math, arena shell, layout serialization/fingerprints
    Voting/                     UPFRatingSubsystem — match record + vote persistence
    UI/                         All widgets, pure C++ UMG (HUD, wheel, vote, results, ...)
docs/
  pc-setup.md                   Windows first-compile + first-run guide (start here)
  design/                       Design docs 01–05; 05-code-contract.md is LAW
```

## The design docs

Everything in `Source/` implements [`docs/design/05-code-contract.md`](docs/design/05-code-contract.md)
— the binding contract for class APIs, gameplay numbers, and UE 5.6 correctness rules. The
supporting docs: [`01-game-design.md`](docs/design/01-game-design.md) (rules),
[`02-architecture.md`](docs/design/02-architecture.md) (structure/netcode),
[`03-build-system.md`](docs/design/03-build-system.md) (grid/pieces),
[`04-combat-feel.md`](docs/design/04-combat-feel.md) (movement/gunplay). Where they conflict, the
contract wins. Code follows the docs, never the reverse.

## Status

| Area | State |
|---|---|
| Design docs 01–05 | Final (locked for the graybox milestone) |
| Source (six parallel work packages) | Implemented against the contract; first compile pending on the Windows PC |
| `L_Graybox.umap` | Not yet created — done once during PC setup, then committed |
| Packaging / installer | Post-first-playable |

Match spec at a glance: 4v4 design target (1v1–6v6 supported) · Build 180 s · rounds 90 s,
first to 4 of max 7, sudden-death tiebreak · 3 HP, mask ×2 · 12 balls/s, 100-ball hopper,
infinite reserve · vote 20 s · full match ≈ 12–13 minutes.
