# CombatForge Player Progression — Design Plan

*2026-07-17 · research pass run `wf_8af8416e-db1`. PLAN ONLY — deliberately not implemented, per Tom:
"make a plan and tell me the plan so that we can think through the details and make this right the first time."*

## 0. Executive summary

Account-linked progression on the same Cloudflare Worker + D1 backend as accounts/matchmaking
(see docs/multiplayer-plan.md), built on three commitments:

1. **Append-only `xp_events` ledger** — profiles are a rebuildable projection, never the truth.
2. **Only the dedicated fleet grants XP** — servers report **raw stats** (never XP amounts) authenticated
   by per-server keys; the Worker computes XP under a versioned rules table. Listen/player-hosted matches
   **never** grant (a host-player's machine holds no server key — this one rule kills 90% of cheating).
3. **Design guardrails locked now** (§3) so unlocks can never damage the game: joins never gated, weapons
   lateral-only, no TTK/movement abilities, ~85% of unlocks cosmetic.

The codebase is unusually ready: `ACombatForgePlayerState` already replicates server-authoritative
Eliminations / TimesEliminated / MatchScore / TagCount; every match already has a **MatchId**
(`UPFRatingSubsystem::BeginMatchRecord`) — the natural idempotency key; the after-action screen
(`PFResultsWidget`) is the natural XP display; and vote records already carry per-player install-GUID
hashes, so **pre-account history can attach to accounts retroactively** via an identities link table.

## 1. Data architecture

**Why a ledger, not counters:** *audit* ("where did my XP go?" answerable row-by-row per kid), *replay*
(level-formula change or grant bug → fix rule, rebuild profiles by rescanning events), *recovery*
(corrections are compensating events, a cheater's grants are voidable retroactively), *deletion*
(COPPA account delete = clean per-user DELETE). Scale is trivial for D1 (even 1,000 matches/day ≈ 22M
rows/yr vs 25B included reads/mo, 10 GB cap = decades).

**Schema (D1/SQLite, one DB, additive forever):**

```
users            (id PK, created_at, kind CHECK('adult','child_profile'), parent_user_id NULL→users,
                  display_name UNIQUE COLLATE NOCASE, status)
identities       (guid_hash PK, user_id→users, linked_at, last_seen_at)      -- bridges install-GUID → account
profiles         (user_id PK, xp_total, level, playtime_sec, matches_played, wins, eliminations,
                  times_eliminated, tags, per_mode JSON, last_event_id, updated_at)   -- pure projection
xp_events        (id AUTOINC, user_id, match_id, source, amount, rule_version, meta JSON, created_at,
                  UNIQUE(match_id, user_id, source))
matches          (match_id PK, server_id, mode, arena_id, map, started_at, ended_at, duration_sec,
                  winner_team, net_protocol, raw JSON)
match_players    (match_id, user_id NULL, guid_hash, team, elims, times_elim, score, tags,
                  completed BOOL, PK(match_id, guid_hash))
unlock_definitions(unlock_id PK, kind, criteria JSON, payload JSON, active, sort, created_at)
                  -- e.g. criteria {"type":"level","gte":10}; payload {"slot":"Headwear","partIndex":…}
                  -- NEW UNLOCK = INSERT. Never a migration.
user_unlocks     (user_id, unlock_id, granted_at, source_event_id, PK(user_id, unlock_id))
servers          (server_id PK, key_hash, region, active)
```

**Grant pipeline:** at `CommitMatchRecord` time the match server POSTs `/v1/match-report`
(per-server key + HMAC-SHA256 body + timestamp, >10 min skew rejected). Worker: one D1 batch —
`INSERT match ON CONFLICT DO NOTHING` (conflict = already processed → idempotent 200, append nothing);
`UNIQUE(match_id,user_id,source)` is the second belt. Server persists unsent reports to
`Saved/PendingReports/*.json` and retries with backoff — late delivery is safe because dedupe is by
match_id. Defense-in-depth even with valid keys: clamp max XP/match, elims-vs-duration plausibility,
roster ≤ 12, known enums. **Report raw stats, Worker computes XP** (rules table stamped `rule_version`)
→ tuning never needs a fleet redeploy, and history is recomputable.

## 2. XP economy (kids-casual tuning; ~10–15 min match ≈ 400–600 XP)

| Source | XP | Why |
|---|---|---|
| Match **completion** | 250 | Biggest single chunk — showing up and finishing matters most; anti-ragequit (`completed` flag) |
| Elimination/tag | 10 | Present but never dominant |
| Flag capture / return | 50 / 25 | Objective play **out-earns** pure slaying — kids who can't out-aim adults still progress (Splatoon model) |
| Point capture / hardpoint | 30 / 1 per sec (cap 60) | 〃 |
| Bomb plant / defuse | 50 | 〃 |
| Win bonus | 100 | Deliberately modest — losers keep ~75-80% of a winner's take; a small community can't afford losers feeling punished |
| First **match** of the day | +250 | Participation-gated (vs LoL's first-*win*). **No streaks, no FOMO** — rejected knowingly for a kids audience |
| **Builder XP** | +2/piece surviving round end; +25 if your half wins the vote | The build phase is half the game; no other shooter can reward it. Vote records already exist |

**Curve:** soft-exponential, cap 50: `cost(L) ≈ 600 × 1.06^L` (L2≈650, L10≈1,050, L25≈2,600, L49≈10,400;
total ≈185k XP ≈ 350-400 matches ≈ a year of casual weekly play). Session target: three matches + daily
bonus ≈ 1,500-1,800 XP → **a level-up nearly every session through ~L20** (the retention hook lives early).

**Prestige: NO.** Resets read as loss to a kid; even CoD abandoned true resets (BO6 keeps everything).
If endgame status is ever wanted: Deep Rock Galactic-style **additive** veteran badges — keep everything,
gain a star. Cheap alternative: `season_id` on xp_events makes seasonal recaps a query, not a feature.

## 3. Unlock design — the guardrails (lock these NOW)

1. **Joins are never gated.** Maps/modes/servers joinable by everyone at level 1, forever. Unlocks may gate
   **selection/hosting only** ("unlock The Yard as *your host pick* at L5"). With a 12-player cap and a
   friends-and-family community, any join-side partition kills critical mass — the Battlefield-Premium
   pool-split lesson (DICE shipped "Premium Friends" then abandoned paid maps entirely).
2. **Weapons are lateral sidegrades only.** The existing `FPFWeaponDef` catalog is already a tradeoff
   frontier (spread/muzzle/mag/ROF/fire-modes) — every unlockable weapon sits ON that frontier
   (faster-firing but wider spread…), TF2-style. Anti-pattern on record: Battlefront II 2017.
   **Invariant: a level-1 kid concedes zero paper advantage to a level-50 adult** — the game's premise is
   adults and kids in the same lobby.
3. **No abilities that touch TTK, health, respawn time, or movement.** Breaks both the airsoft-real fantasy
   and the invariant. Safe adjacent space: diegetic loadout *choices* (2 smokes OR 1 frag), build-piece
   prop skins.
4. **~85% of unlocks are cosmetic**, drawn from the Bandit pack's ~437 modular parts (`PFChar` already
   enumerates parts per slot from disk — gating = a class-menu filter + a server-side "part unlocked"
   check on the replicated kit; **zero content work**). Plus: weapon paints (`FPFWeaponDef.MaterialPath`
   swap already supported), splat colors, titles/badges, level emblem on the after-action screen.
   Generous default wardrobe at L1 (kids must build a cool character immediately), then ~1 unlock/level
   early, slowing later; theme bundles at milestones.
5. **Grandfathering:** alpha players currently have ALL parts — before Phase C, flag existing installs as
   founders (`identities.linked_at` makes detection trivial) or lock only NEW part packs. Retroactively
   confiscating a kid's outfit reads as theft.

**Prior art scorecard** — badly: Battlefield Premium (join-gated maps), Battlefront II 2017 (power in
progression), classic CoD prestige (reset-as-loss). Well: TF2 (sidegrades), **Splatoon** (objective-weighted
earning, the best kids-shooter reference), Rocket League / Fall Guys (pure-cosmetic, mixed-age retention),
Deep Rock Galactic (small-community additive endgame).

## 4. Phasing (every phase additive — no migrations, no report-format changes)

- **Phase A — silent (ships WITH accounts + fleet):** full schema, `/v1/match-report` + server keys +
  idempotency, XP rules computing silently. Zero game-UI change.
- **Phase B — visible:** `GET /v1/profile/:id`; after-action XP breakdown + level bar in `PFResultsWidget`,
  small level badge in lobby. Read-only + offline-safe (grant already happened server-side; unreachable
  API degrades to today's local stats).
- **Phase C — unlockables:** unlock_definitions + user_unlocks + an evaluation step appended to the grant
  pipeline; menus filter by unlocked set. Needs the grandfathering decision first.

**Build NOW (pre-accounts, near-zero cost, prevents unrecoverable history loss):**
1. Freeze the match-report JSON schema and **emit it locally** from `UPFRatingSubsystem::CommitMatchRecord`
   (`Saved/MatchReports/`) — real playtests exercise the exact future wire format today.
2. Include per-player guid_hash, completion flags, and objective/builder counters in that record —
   anything not captured now is history we can never grant retroactively.
3. Add the per-player objective/builder stat counters to `ACombatForgePlayerState` alongside Eliminations
   (same replication pattern).

## 5. COPPA notes for stats

Screen names + persistent identifiers ARE personal info for under-13s (2025 amendments, in force).
Posture: per-match scoreboards and private/friends views are fine; **no global public leaderboards in v1**
(if ever added: opt-in, generated handles); zero third-party analytics SDKs; account deletion = per-user
DELETE (the ledger schema makes it complete); written retention policy covers match logs. The no-in-game-
chat decision already removed the hardest surface.

## 6. Decisions Tom owns

1. **Bot matches**: full XP, reduced, or XP only with ≥N humans? (Kids mostly play with bots — this is the
   single biggest tuning call, and it must be decided before Phase B makes XP visible; AFK-vs-bots farming
   is the abuse case.)
2. **Private/friends matches on fleet servers**: grant XP or quick-play/public only? (Friends AFK-farming
   is the risk.)
3. **Grandfathering**: founders keep the full 437-part wardrobe forever, or everyone re-earns?
4. **Level cap 50 flat forever, or annual seasons** with recap + additive veteran badges?
5. **Display of progression to others**: level badge visible in lobby/scoreboard, or private-only?
6. **Abilities**: plan recommends *none* that touch combat math — is a cosmetic-plus-loadout-choice
   progression enough for the vision, or do you want to explore bounded ability ideas anyway?
