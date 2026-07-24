# Project code review

Inventory-style review of CombatForge source (not binary `Content/`).

## Current: Pass 2 (2026-07-23)

**Status: complete — 12/12 modules.**

| Doc | Purpose |
|-----|---------|
| [TRACKER.md](./TRACKER.md) | Progress, majors fix queue |
| [RUBRIC.md](./RUBRIC.md) | Criteria & severity |
| [modules/](./modules/) | Per-module Pass 2 reports |

### Headline

| | |
|--|--|
| **Blockers** | **2** (Options reset-to-spawn: Core P2-C1 + Player P2-P1 — treat as one fix) |
| **Majors** | **16** |
| **Minors / nits** | **51 / 38** (107 total) |
| **GitHub** | [#24](https://github.com/SourceCodeMan/combatforge/issues/24) — full findings list |
| Default Elimination LAN path | Mostly solid except reset-to-spawn + pilot-phantom + bomb edge cases |

### Fix first

1. **P2-C1 / P2-P1** — reset-to-spawn revives / free heal mid-match  
2. **P2-CB1–2** — bomb/frag BB inherit weapon HitValue  
3. **P2-CB3–4** — hit-confirm spam + 1000 bomb projectiles  
4. **P2-C2–C4** — pilot phantom + bomb intermission  
5. **P2-CB5, P2-P2** — reload/kit full-mag on swap  

Finding IDs: `P2-*` only (Pass 1 is historical).

### Continue

```text
fix blockers
fix combat majors
make GitHub issues for pass 2
```

---

## Historical: Pass 1 (2026-07-17)

Superseded by Pass 2. Old GitHub issues #10–#21 and `BASELINE.md` / `core-reviewer-independent.md` are archive context only.
