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
| Open **blocker** | **2** (same feature: Options reset-to-spawn — Core + Player sides) |
| Open **major** | **16** |
| Open minor | **51** |
| Open nit | **38** |
| **Total findings** | **107** |
| Last updated | 2026-07-23 |

### Counts by module (open)

| Module | Blocker | Major | Minor | Nit | Report |
|--------|--------:|------:|------:|----:|--------|
| Core | 1 | 3 | 5 | 3 | [core.md](./modules/core.md) |
| Combat | 0 | 5 | 5 | 3 | [combat.md](./modules/combat.md) |
| Building | 0 | 1 | 6 | 3 | [building.md](./modules/building.md) |
| Player | 1 | 3 | 4 | 4 | [player.md](./modules/player.md) |
| UI | 0 | 2 | 7 | 4 | [ui.md](./modules/ui.md) |
| Objectives | 0 | 0 | 2 | 2 | [objectives.md](./modules/objectives.md) |
| Voting | 0 | 0 | 2 | 2 | [voting.md](./modules/voting.md) |
| AI | 0 | 0 | 2 | 3 | [ai.md](./modules/ai.md) |
| Online | 0 | 0 | 2 | 3 | [online.md](./modules/online.md) |
| Input+Audio+root | 0 | 1 | 4 | 4 | [input-audio-root.md](./modules/input-audio-root.md) |
| Scripts | 0 | 0 | 5 | 4 | [scripts.md](./modules/scripts.md) |
| Deploy+Config | 0 | 1 | 7 | 3 | [deploy-config.md](./modules/deploy-config.md) |

---

## Fix queue (blocker + major only)

Prefer this order.

### Blockers (fix first)

| ID | Title | Module | Notes |
|----|-------|--------|-------|
| **P2-C1** | `RequestResetToSpawn` fully revives eliminated pawns (heal + fire) | Core | Same feature as P2-P1 |
| **P2-P1** | `ServerRequestResetToSpawn` free mid-match heal / ammo / teleport | Player | Gate or disable in Combat; fix both sides together |

### Majors

| ID | Title | Module |
|----|-------|--------|
| **P2-C2** | Headless pilot phantom stays permanently `bAliveInRound` → Elimination wipe skew | Core |
| **P2-C3** | Ready + vote early-advance still require headless phantom | Core |
| **P2-C4** | Armed bombs survive Elimination Intermission / next Freeze | Core |
| **P2-CB1** | Bomb BB spray inherits planter weapon `HitValue` (sniper multi-lethal) | Combat |
| **P2-CB2** | Frag grenade BBs inherit thrower `HitValue` | Combat |
| **P2-CB3** | Every projectile hit → reliable `ClientHitConfirm` (frag/bomb/shotgun spam) | Combat |
| **P2-CB4** | Bomb detonation spawns up to 1000 authoritative projectile actors | Combat |
| **P2-CB5** | Weapon swap leaves reload mid-flight (FinishReload fills new gun) | Combat |
| **P2-P2** | Mid-fight `ServerSetKit` / class cycle can equip new gun full mag | Player |
| **P2-P3** | `ServerSetKit` does not clamp `CharParts` size / indices | Player |
| **P2-P4** | `EnforceSingleFirstPersonWeapon` always-on full scan + Warning spam | Player |
| **P2-BD1** | `ComputeArenaId` grid header ignores per-map CellsY/Levels | Building |
| **P2-U1** | Key-rebind capture not cancelled when Options closes | UI |
| **P2-U2** | Fullscreen checkbox vs Window-mode desync / persist fail | UI |
| **P2-I1** | Rebind conflict check ignores non-rebindable keys (LMB/WASD) | Input |
| **P2-D1** | Pilot server restart loop uses `&` on GUI exe → spawn storm | Deploy |

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
