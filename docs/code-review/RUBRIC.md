# Project Code Review Rubric

Use this for every module pass. Keep findings consistent with
`docs/pre-playtest-review.md` and the law in `docs/design/05-code-contract.md`.

## In scope

| Path | Review? |
|------|---------|
| `Source/CombatForge/**` | Yes — primary |
| `Scripts/**` | Yes — tooling |
| `Deploy/**` | Yes — packaging / playtest |
| `Config/**` | Yes — defaults |
| `docs/design/**` | Reference only (not line-review) |
| `Content/**` | No — binaries / art checklists elsewhere |

**Unit of work:** a module folder. Treat each `.h` + `.cpp` pair as one unit.

## Criteria (ask every time)

1. **Correctness** — Matches game rules and `05-code-contract.md`?
2. **Netcode / authority** — Server vs client roles, replication, host OnRep gaps, prediction?
3. **Lifetime / edge cases** — Timers, join-in-progress, mode switches, disconnects?
4. **Quality** — Ownership, `UPROPERTY` GC roots, dead code, duplicated constants, naming?
5. **Safety** — Nulls, GC, bounds, destroy-during-tick, use-after-free patterns?

## Severity

| Level | Meaning |
|-------|---------|
| **blocker** | Breaks the default play path (Lobby → match on RoundElimination) |
| **major** | Breaks a real mode, corrupts match state, or is a serious latent bug when that path is used |
| **minor** | Latent, cosmetic, or only wrong under edge conditions / non-default config |
| **nit** | Style, clarity, small cleanup with no gameplay impact |

## Finding template

```markdown
### N. Short title
- **Severity:** minor
- **Status:** open | fixed | deferred | wontfix
- **File:** `path/to/File.cpp:LINE`
- **Symptom:** what a player/dev sees
- **Why:** root cause
- **Fix:** concrete suggestion
- **Confidence:** high | med | low
- **Source:** baseline | this-pass
```

## Session workflow

1. Mark module `in_progress` in `TRACKER.md`
2. List files; skim relevant contract sections
3. Review (read-only findings first)
4. Write `modules/<name>.md`
5. Update tracker: status, counts, date
6. Promote only **blocker** / **major** into the open fix queue on the tracker

Do not thrash the tree mid-pass unless the fix is tiny and local.
Prefer: **Pass A findings → Pass B fix B/M → Pass C re-review**.

## Definition of done (module)

- Every source pair in the module has been read
- Findings written (or explicit “clean” note per file / subsystem)
- Tracker row updated
- Baseline findings for that module re-verified (still open / fixed / drifted)
