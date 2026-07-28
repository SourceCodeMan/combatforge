# Project Code Review Tracker — Pass 2

| Field | Value |
|-------|-------|
| **Pass** | **2** (full inventory re-review) |
| **Completed** | 2026-07-23 |
| **GitHub issue** | [#24](https://github.com/SourceCodeMan/combatforge/issues/24) |
| **Criteria** | [RUBRIC.md](./RUBRIC.md) |
| **Contract** | `docs/design/05-code-contract.md` |
| **Prior pass** | Pass 1 (2026-07-17) — superseded; do not use old B/C IDs as open work |

## Progress summary

| Metric | Value |
|--------|------:|
| Modules done | **12 / 12** |
| Open **blocker** | **0** — both fixed 2026-07-24 |
| Open **major** | **0** — all 16 fixed 2026-07-24 |
| Open minor | **1** (P2-U9 only) |
| Open nit | **13** |
| **Total findings** | **107** (93 fixed) |
| Last updated | 2026-07-28 |

> **Fix pass 2026-07-24 (branch `docs/code-review-pass-2`):** both blockers and all 16 majors
> fixed and compile-verified; P2-C9 and P2-I2 folded in. P2-C2/C3 were largely fixed on `main`
> by the same-day playtest batch (`7c02585`, replicated `bHeadlessPhantom` + count filters);
> this branch adds the `ServerSetAliveInRound` root choke. Per-finding notes in the fix-queue
> tables below; module reports retain the original open write-ups.

### Counts by module (open, after the 2026-07-28 minors+nits pass)

The module reports below retain the ORIGINAL write-ups; this table and the per-module GitHub
issues (#27-#38, checkboxes ticked) are the current status.

| Module | Blocker | Major | Minor | Nit | Still open | Report |
|--------|--------:|------:|------:|----:|------------|--------|
| Core | 0 | 0 | 0 | 0 | — | [core.md](./modules/core.md) |
| Combat | 0 | 0 | 0 | 3 | P2-CB11/12/13 (contract errata, belongs in 05) | [combat.md](./modules/combat.md) |
| Building | 0 | 0 | 0 | 0 | — | [building.md](./modules/building.md) |
| Player | 0 | 0 | 0 | 0 | — | [player.md](./modules/player.md) |
| UI | 0 | 0 | 1 | 2 | P2-U9 (elim feed by id, core change), U11 (ring brush), U12 (howto copy: wording call) | [ui.md](./modules/ui.md) |
| Objectives | 0 | 0 | 0 | 0 | — | [objectives.md](./modules/objectives.md) |
| Voting | 0 | 0 | 0 | 0 | — | [voting.md](./modules/voting.md) |
| AI | 0 | 0 | 0 | 2 | P2-AI4/AI5 (both "optional / not required at current roster sizes") | [ai.md](./modules/ai.md) |
| Online | 0 | 0 | 0 | 1 | P2-ON4 (plaintext token — also an open item in docs/legal/privacy-policy.md) | [online.md](./modules/online.md) |
| Input+Audio+root | 0 | 0 | 0 | 3 | P2-I7 (doc), I8/I9 (both "no change required") | [input-audio-root.md](./modules/input-audio-root.md) |
| Scripts | 0 | 0 | 0 | 0 | — | [scripts.md](./modules/scripts.md) |
| Deploy+Config | 0 | 0 | 0 | 2 | P2-D9 (rated acceptable in its own finding), D10 (editor ini) | [deploy-config.md](./modules/deploy-config.md) |

> **Minors + nits pass 2026-07-28 (branch `fix/review-pass-2-minors`, 7 commits):** 93 of 107
> findings closed, editor target compile-verified after every batch. Four items were Tom's product
> calls: P2-C5 mid-round joiners now spectate until the next round; P2-BD5 structural pieces get a
> subtle per-team accent tint; P2-ON5 `pf.SetRank`/DevRankOverride stripped; P2-CB6 fire mode is
> now enforced server-side. **P2-CB6 adds a ServerSetFireMode RPC, so `PFBuild::NetProtocol` went
> 16 -> 17** — NOT packaged, it rides the next itch push (client + server together).
> Nothing here has been playtested; it is code-verified only.

---

## Fix queue (blocker + major only)

Prefer this order.

### Blockers (fix first)

| ID | Title | Module | Status (2026-07-24) |
|----|-------|--------|--------|
| **P2-C1** | `RequestResetToSpawn` fully revives eliminated pawns (heal + fire) | Core | ✅ **fixed** — alive/out/bEliminated gates (never a revive), 20 s per-player cooldown, heal+reload suppressed during live rounds (teleport-only unstuck), showdown HP honored (folds P2-C9) |
| **P2-P1** | `ServerRequestResetToSpawn` free mid-match heal / ammo / teleport | Player | ✅ **fixed** — same server-side gate; the RPC now lands on a hardened handler |

### Majors

| ID | Title | Module | Status (2026-07-24) |
|----|-------|--------|--------|
| **P2-C2** | Headless pilot phantom stays permanently `bAliveInRound` → Elimination wipe skew | Core | ✅ **fixed** — `main` batch filtered RecountAlive/GetTeamCounts + all widgets (replicated `bHeadlessPhantom`); this branch chokes `ServerSetAliveInRound` at the source |
| **P2-C3** | Ready + vote early-advance still require headless phantom | Core | ✅ **fixed** on `main` (7c02585) — AreAllPlayersReady / CheckAllVotesIn / FinalizeVotePhase all skip the phantom |
| **P2-C4** | Armed bombs survive Elimination Intermission / next Freeze | Core | ✅ **fixed** — `DestroyBombs()` at EndRound + StartNextRound; detonate piece-removal now requires RoundState==Live |
| **P2-CB1** | Bomb BB spray inherits planter weapon `HitValue` (sniper multi-lethal) | Combat | ✅ **fixed** — `UtilityDamageOverride = 1` on breach BBs |
| **P2-CB2** | Frag grenade BBs inherit thrower `HitValue` | Combat | ✅ **fixed** — same override on the frag cloud |
| **P2-CB3** | Every projectile hit → reliable `ClientHitConfirm` (frag/bomb/shotgun spam) | Combat | ✅ **fixed** — RPC now Unreliable + `SendHitConfirmCoalesced` (≤1 confirm / 50 ms; elims always pass) |
| **P2-CB4** | Bomb detonation spawns up to 1000 authoritative projectile actors | Combat | ✅ **fixed** — FragBBCount 1000→120, batch 80→40 (proximity paint already covers close range) |
| **P2-CB5** | Weapon swap leaves reload mid-flight (FinishReload fills new gun) | Combat | ✅ **fixed** — `CancelReload()` (now public) on both the predicting owner and authority swap paths |
| **P2-P2** | Mid-fight `ServerSetKit` / class cycle can equip new gun full mag | Player | ✅ **fixed** — weapon ids frozen while alive in a live round (cosmetics still apply); client re-push on respawn delivers the new class |
| **P2-P3** | `ServerSetKit` does not clamp `CharParts` size / indices | Player | ✅ **fixed** — array truncated to SlotCount, each index clamped to the slot's catalog range |
| **P2-P4** | `EnforceSingleFirstPersonWeapon` always-on full scan + Warning spam | Player | ✅ **fixed** — hide always runs silently (one Warning only when it hides something); FPSCAN dump + world iterator behind `pf.FPWeaponScan 1` |
| **P2-BD1** | `ComputeArenaId` grid header ignores per-map CellsY/Levels | Building | ✅ **fixed** — hash header takes the active map's CellsY (+derived Levels). Warehouse ids unchanged (default header); Yard ids fork — old Yard favorites re-key |
| **P2-U1** | Key-rebind capture not cancelled when Options closes | UI | ✅ **fixed** — capture cleared on Open/Close/Destruct + labels refreshed |
| **P2-U2** | Fullscreen checkbox vs Window-mode desync / persist fail | UI | ✅ **fixed** — checkbox drives WorkingWindowMode (0/2); prefs pull re-derives the flag |
| **P2-I1** | Rebind conflict check ignores non-rebindable keys (LMB/WASD) | Input | ✅ **fixed** — full fixed-mapping deny-list; own shipped default always allowed (folds P2-I2) |
| **P2-D1** | Pilot server restart loop uses `&` on GUI exe → spawn storm | Deploy | ✅ **fixed** — OnBox launch pattern (Start-Process + WaitForExit + crash-loop backoff); file normalized to ASCII (its em-dash parsed as a smart quote under ANSI) |

---

## Queue status

| # | Module | Status | Reviewed | Report |
|---|--------|--------|----------|--------|
| 1 | Core | **done** | 2026-07-23 | [core.md](./modules/core.md) |
| 2 | Combat | **done** | 2026-07-23 | [combat.md](./modules/combat.md) |
| 3 | Building | **done** | 2026-07-23 | [building.md](./modules/building.md) |
| 4 | Player | **done** | 2026-07-23 | [player.md](./modules/player.md) |
| 5 | UI | **done** | 2026-07-23 | [ui.md](./modules/ui.md) |
| 6 | Objectives | **done** | 2026-07-23 | [objectives.md](./modules/objectives.md) |
| 7 | Voting | **done** | 2026-07-23 | [voting.md](./modules/voting.md) |
| 8 | AI | **done** | 2026-07-23 | [ai.md](./modules/ai.md) |
| 9 | Online | **done** | 2026-07-23 | [online.md](./modules/online.md) |
| 10 | Input+Audio+root | **done** | 2026-07-23 | [input-audio-root.md](./modules/input-audio-root.md) |
| 11 | Scripts | **done** | 2026-07-23 | [scripts.md](./modules/scripts.md) |
| 12 | Deploy+Config | **done** | 2026-07-23 | [deploy-config.md](./modules/deploy-config.md) |

## Pass notes

- Finding IDs use **`P2-*`** prefix so they do not collide with Pass 1.
- Pass 1 baseline / GitHub issues #10–#21 are **historical** unless you re-open or file new issues.
- Pass 1 majors that were patched (Respawn, Domination HUD, melee, etc.) were **not** re-listed; this pass only reports what is wrong **now**.
- Independent of prior reports (agents told not to read old module docs).

## Recommended next steps

1. **Fix blockers** P2-C1 + P2-P1 together (Options / reset-to-spawn)  
2. **Combat batch** P2-CB1–CB5 (utility HitValue, confirm spam, bomb actor count, reload cancel)  
3. **Core pilot/bomb** P2-C2–C4  
4. **Player kit** P2-P2–P4  
5. UI / Input / Deploy majors  
6. Optional: `gh issue create` one issue per module (or one mega-issue)  

Or ask: **“fix blockers”** / **“make GitHub issues for pass 2”**.
