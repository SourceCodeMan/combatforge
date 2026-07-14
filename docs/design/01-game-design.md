# CombatForge — Game Design Document

**Doc:** 01-game-design.md · **Owner:** Lead Game Design · **Status:** v1.0 (locked for graybox milestone)
**Engine context:** UE 5.6, C++-first, server-authoritative multiplayer from day 1. See locked technical decisions in project charter.

All distances in meters (1 m = 100 Unreal units). All times in seconds unless noted.

---

## 0. Design pillars (tie-breakers for every future argument)

1. **Build like Fortnite, shoot like CoD, look like paintball.** Any mechanic that violates one of these three verbs gets cut.
2. **The arena is the content.** Players make the map; the game's job is to make that fast, fair, and rateable.
3. **Never wait long, never run dry.** No phase over 3 minutes without player agency; no ammo/reload anxiety, ever.
4. **One match, one verdict.** Every match ends with a rating that could someday promote the arena into a community pool.

---

## 1. Match structure

### 1.1 Player counts

| Parameter | Value | Rationale |
|---|---|---|
| Design target | **4v4** | Big enough for flanks and crossfire lanes, small enough that each player's builds visibly matter and rounds stay under 90 s. |
| Supported range | **1v1 to 6v6** (12 max) | 1v1 must work for dev testing on a listen server; 6v6 is the replication/perf ceiling for v1. |
| Minimum viable | **1v1** | The full loop (build → fight → vote) must be exercisable by two people. |
| Uneven teams | Allowed in Lobby (host override), never auto-created | Testing convenience only. |

### 1.2 The five phases, minute by minute (4v4 typical match ≈ 12–13 min)

| Clock (typical) | Phase | Duration | What happens |
|---|---|---|---|
| 0:00–1:00 | **Lobby** | Until all ready (host may force-start; 5 s countdown once all ready) | Players join listen server, auto-balanced onto Team A / Team B (host can drag-swap). Free-roam a flat 20×20 m warm-up pen with one target dummy per player — shooting works here so the marker feel is tested before it matters. |
| 1:00–4:00 | **BuildPhase** | **180 s** fixed cap; ends early if **both teams are 100% Ready** (no minimum elapsed time) | Each team builds its own half of the plot (see §2). 30 s and 10 s warnings (HUD flash + tone). At 0 s, unspent budget is discarded and all players are teleported to their spawn strip. |
| 4:00–12:00 | **CombatPhase** | Round-based, **first to 4 round wins, max 7 rounds** (typical 5 rounds ≈ 8 min, hard max ≈ 12 min) | See §1.3 and §3. |
| 12:00–12:20 | **VotePhase** | **20 s hard cap** | Thumbs vote + category chips (see §4). Auto-submits at timeout. |
| 12:20–12:35 | **Results** | **15 s** | Final score, MVP (most elims), arena thumb tally (e.g. "6👍 / 2👎"), top liked/disliked category. Then return to Lobby with same roster for a rematch vote (new build) or disband. |

### 1.3 CombatPhase format — DECISION: round-based elimination (Search-style), not respawn TDM

**Decision: rounds with no in-round respawn, alternating sides every round.** Rationale: paintball's core fiction *is* elimination — one game, walk off the field when you're hit; respawn TDM breaks that instantly. It also protects the build loop: a round-based format makes players *learn and route through* the arena repeatedly from both sides, which produces informed votes. "CoD fast-paced" is delivered inside the round via movement speed, snappy ADS, sub-500 ms TTK, and short 90 s rounds — CoD's own most competitive mode (Search & Destroy) proves elimination rounds and CoD pacing coexist. Dead time is bounded: max spectate wait is 90 s and typically ~40 s.

**Round anatomy:**

| Beat | Value |
|---|---|
| Freeze time (players locked at spawn, can look/ADS) | 5 s |
| Round timer | **90 s** |
| Round win | Eliminate all enemy players, **or** more players alive at 0:00 |
| Timer-expiry tie (equal alive) | Round is a **draw** — no point either side |
| Inter-round intermission (scoreboard strip, side swap teleport) | 7 s |
| Side assignment | **Teams swap halves every round** (odd rounds: home half; even rounds: enemy half) |

Side-swap every round is the fairness mechanism for asymmetric team-built halves (§2): over any 2 rounds both teams have attacked/defended from both halves, so a "stronger" half advantages nobody. It's also a feature — you get to fight *inside the enemy's creation*, which feeds informed category votes.

**Match win & tiebreak:** First team to **4 round wins** takes the match. If drawn rounds cause a 3–3 (or lower) deadlock after round 7, play **one Sudden Death round: 1 hit eliminates (classic paintball "one splat you're out"), 60 s timer, timer tie = match draw.** Sudden death is a parameter change, not a new system.

**Scaling by team size:** 1v1–2v2 → first to 3 wins, max 5 rounds, 60 s round timer (small plots, see §2.2). 3v3+ → values above.

---

## 2. Build phase rules

### 2.1 Who builds what — DECISION: each team builds its own half

**Decision: the plot is split at midline; each team builds only its own half, and the two halves combine into the single arena both teams fight across.** Rationale, in priority order:

1. **Griefing:** shared collaborative building lets the enemy wall off your spawn, delete-war your structures, or troll-build; half-ownership makes cross-team griefing structurally impossible (you cannot place or delete on enemy ground).
2. **Fairness:** asymmetry between halves is neutralized by every-round side swaps (§1.3), so we get fairness *without* forcing boring mirrored maps.
3. **Fun & authorship:** "our half vs their half" creates team identity and pride — the thing the meta loop (§5) monetizes emotionally. Post-match votes implicitly judge both teams' work.
4. **Voting loop:** the rating record stores which team built which half (§4.3), so future aggregation can score halves independently and recombine great halves into community arenas.

**Within-team rules:** any teammate may delete any team piece (full budget refund); every piece shows placer attribution on look-at. Intra-team griefing is socially policed — acceptable at v1 stakes.

**Enemy visibility — DECISION: yes, fully visible.** No fog wall; you can see (not enter) the enemy half while building. Rationale: real speedball players walk the field before every match; counter-building mind games ("they went towers, we need head-high snake cover") are the best emergent gameplay of the build phase, and it's zero engineering cost. A 1 m-tall holographic midline barrier blocks traversal and paintballs during BuildPhase only.

### 2.2 Plot, grid, and pieces

| Parameter | Value |
|---|---|
| Plot (3v3–6v6) | **60 m × 40 m**, height cap **12 m** (3 wall stacks). Each half: 30 m × 40 m. |
| Plot (1v1–2v2) | **40 m × 30 m** (halves 20 × 30), height cap 9 m. |
| Structural grid | **4 m cells** (Fortnite-scale muscle memory), pieces snap to grid + 90° rotations. |
| Prop sub-grid | 1 m snap, 15° yaw steps. |
| Spawn strip | 6 m-deep no-build strip along each team's back wall; 6 fixed spawn points. |
| Midline no-build strip | **4 m each side of midline (8 m total)** — guarantees an open mid lane; combined with elimination rounds + more-alive-wins, this kills turtle-boxing (hiding in a box risks losing rounds on the timer). |
| Floor/outer walls | Plot floor and 12 m perimeter walls are indestructible engine-cube graybox; nothing exists outside the plot. |

**Build budget:** team-pooled **40 Build Points per player** (4v4 = 160 BP/team). Unspent BP is lost at phase end. Costs:

| Piece | Size | Cost (BP) | Notes |
|---|---|---|---|
| Wall | 4 w × 3 h × 0.25 t | 3 | Stackable to height cap |
| Half-wall | 4 × 1.5 × 0.25 | 2 | Head-high slide cover |
| Floor/roof panel | 4 × 4 × 0.25 | 3 | Needs edge support from wall/ramp/floor |
| Ramp | 4 × 4 footprint, 3 rise | 3 | Fortnite-standard stair angle |
| Can (cylinder bunker) | 1.2 dia × 2 h | 2 | Classic speedball snap-shooting cover |
| Dorito (wedge) | 2 × 2 × 2 | 2 | Directional cover |
| Brick (low block) | 2 × 1 × 1 | 1 | Snake-run segment; crawl cover |

**Build wheel & controls (Fortnite-familiar, exact bindings in the input spec doc):** Q enters/exits build mode; wheel or 1–7 selects piece; ghost preview (green valid / red invalid); LMB places; R rotates; X deletes aimed team piece (refund). Placement is server-validated (grid, budget, ownership, no-build zones, overlap).

**Structural rules:** no physics/structural-integrity simulation in v1 — pieces are static once placed and support checks are simple adjacency (a floor panel needs ≥1 supported edge). No building and no destruction during CombatPhase — **decision:** the arena is fixed at fight time. Building *before* and shooting *after* is CombatForge's identity; letting combat-building in would collapse us into a worse Fortnite and wreck arena ratings (the thing voted on must be the thing fought in).

### 2.3 Finishing early

Players who finish press **Ready** (toggleable). Ready players keep full build/edit rights until phase end and are encouraged to **walk their half** — sprint/slide/jump work in BuildPhase so routes can be play-tested (weapons disabled). When *every* player on *both* teams is Ready, BuildPhase ends immediately with a 5 s countdown. HUD shows "Ready 3/4 — 2/4" per team.

---

## 3. Elimination model, win conditions, scoring

### 3.1 Hits-to-eliminate — DECISION: 3-hit elimination, no regen, no in-round respawn

**Decision: every player has 3 Hit Points per round; body hit = 1, mask (head) hit = 2; no health regen; eliminated players spectate until the round ends.** Rationale: pure 1-splat is brutally swingy and anti-"fast feel" (players stop pushing); a CoD health pool with regen erases paintball fiction. Three splats reads as fiction-friendly ("three hits and you're called out"), gives a **~250–375 ms TTK** at our fire rate — squarely CoD-snappy — and rewards aggression because peeking twice against a damaged player is winning math. Mask = 2 keeps a headshot skill ceiling (2 mask hits eliminate).

| Combat parameter | Value |
|---|---|
| HP per round | 3 (body hit 1, mask hit 2) |
| Splat feedback | Hit: paint splat decal on victim + hitmarker; HUD shows own splat count (visual paint creeping onto mask edges at 1/2 damage) |
| Marker (weapon) | One marker, everyone identical in v1 |
| Fire mode | Full-auto (electronic marker fiction), **8 balls/s** |
| Ammo | **Unlimited, no reload mechanic, ever** (pillar #3; hopper never empties) |
| Projectile | True projectile, **90 m/s**, gravity drop 4.9 m/s² (½g — fiction-plausible, aim-relevant only past ~30 m); server-authoritative with client prediction |
| ADS time | 180 ms; ADS move speed 2.6 m/s |
| Move speeds | Walk 4.5 m/s · Sprint 6.7 m/s · Crouch 2.5 m/s |
| Slide | From sprint: 9.0 m/s initial decaying to 4.5 over 0.8 s, capsule half-height |
| Sprint-to-fire delay | 150 ms |
| Friendly fire | **Off** in v1 |
| Eliminated players | 3 s free-cam at death spot, then cycle live-teammate third-person spectate; dead players may text-chat only with other dead + can pre-vote arena thumb early (§4.2) |

### 3.2 Win conditions and scoring

- **Round:** all enemies eliminated, or more players alive at timer 0:00 (equal → draw round).
- **Match:** first to 4 round wins (max 7), sudden-death tiebreak per §1.3.
- **Player score (Results screen + local stats):** Elimination 100 · Assist (dealt ≥1 damage to eliminated enemy) 25 · Round survival 25 · Round win 50 (all living/dead teammates). MVP = highest score, shown on Results.

---

## 4. Vote phase — UX and data

### 4.1 Category vocabulary (one shared list for liked AND disliked)

Exactly **8 categories**, fixed IDs (stable across versions — never renumber):

| ID | Category | Player-facing hint text |
|---|---|---|
| 1 | `layout` | Overall shape and lanes |
| 2 | `cover` | Amount and placement of cover |
| 3 | `verticality` | Ramps, towers, high ground |
| 4 | `flow` | How movement and routes felt |
| 5 | `balance` | Fair for both sides |
| 6 | `sightlines` | Long shots vs close quarters mix |
| 7 | `creativity` | Original, clever, fun to look at |
| 8 | `pacing` | Rounds felt fast / right length |

### 4.2 Vote flow (≤ 20 s, two taps minimum)

One full-screen widget, gamepad/KBM navigable, 20 s countdown ring:

1. **Step 1 — verdict (required):** "Did you like this arena?" Two large buttons: 👍 / 👎. One click advances instantly.
2. **Step 2 — categories (optional, 0–4 selections):** an 8-chip grid; each chip cycles **neutral → 👍 liked → 👎 disliked → neutral** on click, so liked and disliked are captured on a single screen with the shared vocabulary. A SUBMIT button is live immediately.
3. Timeout auto-submits whatever is selected; a Step-1-only vote is valid; no vote at all records `abstained`.

Dead players in the final round may open Step 1 early while spectating, so most players enter VotePhase with the thumb already cast — median real vote time target: **8 s**.

### 4.3 Arena rating data record

Persisted locally in v1 (JSON lines file `Saved/CombatForge/ratings.jsonl`, one record per voter per match) plus one `ArenaSnapshot` JSON per match. Shape is backend-ready: append-only, GUID-keyed, schema-versioned.

```json
// ArenaRating (one per voter per match)
{
  "schemaVersion": 1,
  "ratingId": "GUID",
  "matchId": "GUID",
  "arenaId": "sha1(canonical piece manifest, both halves)",
  "halfHashA": "sha1(team A half manifest)",
  "halfHashB": "sha1(team B half manifest)",
  "timestampUtc": "2026-07-09T21:14:03Z",
  "voterId": "GUID (per-install identity in v1)",
  "voterTeam": "A|B",
  "voterBuiltHalf": "A|B",
  "voterWonMatch": true,
  "thumb": "up|down|abstained",
  "categoriesLiked": [3, 7],
  "categoriesDisliked": [6],
  "context": {
    "playerCount": 8,
    "teamSize": 4,
    "roundsPlayed": 5,
    "finalScore": "4-1",
    "winnerTeam": "A",
    "matchDurationSec": 742,
    "buildPointsSpentA": 152,
    "buildPointsSpentB": 160
  }
}
```

```json
// ArenaSnapshot (one per match) — enough to reload the arena verbatim
{
  "schemaVersion": 1,
  "arenaId": "sha1(...)",
  "plotSize": [60, 40, 12],
  "pieces": [
    { "type": "wall", "team": "A", "cell": [3, 7, 0], "yaw": 90, "placerId": "GUID" }
  ]
}
```

Design notes: `arenaId` is content-derived (hash), so identical rebuilds aggregate to the same arena. `voterBuiltHalf` + `voterWonMatch` let a future backend de-bias self-votes and sore-loser votes. Per-half hashes let a community pool eventually rate and recombine *halves*, not just whole arenas.

---

## 5. Meta loop — why players come back

The core hook is **authorship with an audience**: every match ends with eight real humans passing judgment on the thing you built ninety seconds earlier, and the categories tell you *why* — "they hated my sightlines but loved the verticality" is an actionable, addictive feedback loop no static-map shooter offers. Rematch-with-rebuild (same lobby, fresh plot) creates an immediate "run it back, I know what to fix" impulse. Future hooks the data shape already supports, listed only to justify today's schema: a community map pool seeded from top-thumb arenas (play *featured* arenas on off-build nights), builder profiles with per-category reputation ("top 5% Flow builder"), half-recombination events pairing highly-rated halves from strangers, and seasonal build-piece palettes. None of this is designed further here.

---

## 6. v1 graybox scope cut

### IN — first playable must ship all of this

- Full 5-phase loop (Lobby → Build → Combat → Vote → Results) as a replicated match state machine; listen server; team assignment + host force-start.
- BuildPhase: 180 s timer, both-teams-Ready early end, half-ownership, visible enemy half, midline barrier, 4 m grid, spawn/midline no-build zones, team BP budget (40/player), place/rotate/delete with refund, server validation. **Piece palette reduced to 5:** wall, half-wall, floor, ramp, can. (Dorito, brick deferred.)
- CombatPhase: rounds first-to-4/max-7 (first-to-3/max-5 at ≤2v2), 5 s freeze, 90 s timer, side swap every round, more-alive timer resolution, draw rounds, sudden-death 1-HP tiebreak round.
- Combat kit: single marker, full-auto 8 balls/s, unlimited ammo, 90 m/s projectiles with drop, 3 HP / mask ×2, no regen, splat decals (simple dynamic-material dots are fine), sprint/slide/ADS at the numbers in §3.1, friendly fire off, teammate spectate on death.
- VotePhase: both steps (thumb + cycling category chips), 20 s cap, auto-submit; Results screen with MVP and thumb tally.
- Persistence: `ratings.jsonl` + `ArenaSnapshot` JSON written exactly per §4.3 schema (the schema is IN even though no backend exists).
- Warm-up pen with target dummies in Lobby.

### DEFERRED — explicitly not in first playable

- Dorito and brick pieces; prop 1 m sub-grid / 15° rotation (all v1 pieces use the 4 m grid + 90°).
- Early thumb pre-voting while dead (v1: everyone votes in VotePhase proper).
- Killcam, kill feed beyond text lines, per-category HUD hints, controller support polish.
- Assist/survival scoring granularity (v1 Results may show elims + round score only).
- Matchmaking, parties, dedicated servers, cross-session identity beyond per-install GUID.
- Loading a saved ArenaSnapshot back into a match (write-only in v1).
- Rematch-with-rebuild lobby flow (v1: return to Lobby is enough; restart match manually).
- Bots, uneven-team auto-balance logic, spectator-only slots.
- All art/audio/Niagara (charter: graybox), paint physics beyond decals, inflatable-bunker visuals.

Anything not listed under IN is out. Changes to IN require editing this document first.
