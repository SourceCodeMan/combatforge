# Baseline findings (pre-playtest review)

Imported from `docs/pre-playtest-review.md` (2026-07-10). Re-verified during full project review **2026-07-17**.

| ID | Title | Severity | Module | Status | Notes |
|----|-------|----------|--------|--------|-------|
| B1 | Respawn mode: every timed round is a draw → match ends 0-0 | **major** | Core | **open** | Still broken. [core.md](./modules/core.md) |
| B2 | Vote countdown ring wrong if vote duration retuned | minor | UI | **fixed** | Uses `GS->PhaseDuration` now. [ui.md](./modules/ui.md) |
| B3 | Missing `OnRep_RoundState()` in `ServerResetMatchState` | minor | Core | **fixed** | [core.md](./modules/core.md) |
| B4 | Breakout horn on join into already-Live round | minor | Core | **fixed** | `LastSeenRoundState` seed. [core.md](./modules/core.md) |
| B5 | Round-pip count from `PlayerArray.Num()` | minor | UI | **fixed** | Uses `RoundWinsToTake`. Residual U1 timing quirk remains. [ui.md](./modules/ui.md) |
| B6 | Corpse collision host vs client skew | minor | Combat | **open** | Accepted v1 latency. [combat.md](./modules/combat.md) |
| B7 | Slide timers not in `FSavedMove_PF` | minor | Player | **open** | [player.md](./modules/player.md) |
| B8 | Slide from non-forward strafe | minor | Player | **open** | [player.md](./modules/player.md) |
| B9 | Sub-frame place-click drops piece | minor | Building | **open** | [building.md](./modules/building.md) |
| B10 | Fast fire-tap out of sprint eaten | minor | Player | **open** | [player.md](./modules/player.md) |

## Scoreboard

| | Count |
|--|------:|
| Fixed since baseline | 4 (B2–B5) |
| Still open | 6 (B1, B6–B10) |
| Promoted severity | B1 → major |

## Subsystems audited clean (original 2026-07-10)

- Enhanced Input lifetime / IMC application
- Join-in-progress / initial replication order (PC ↔ GameState)
- Round/phase state machine default RoundElimination path

(Reconfirmed clean in 2026-07-17 Core pass.)
