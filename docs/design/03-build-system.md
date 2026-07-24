# CombatForge — Build System Spec (v1)

Doc: `03-build-system.md` · Owner: build-system designer-engineer · Status: **LOCKED for v1 graybox**
Cross-refs: `02-match-loop.md` (phase timing), `04-combat.md` (movement/weapons), `05-voting.md` (vote tags).

Design north star: **a Fortnite player's hands already know it.** Muscle memory lives in the
*controls, snap behavior, and turbo-build cadence* — not in Fortnite's exact dimensions. We keep
their control grammar and slope feel, and re-scale the grid for paintball sightlines.

---

## 1. Grid

| Parameter | Value | Rationale |
|---|---|---|
| Grid cell (X×Y) | **400 × 400 uu** (4 m) | Tighter than Fortnite's 512. Paintball is a ground game with dense cover; 400 uu cells give speedball-like bunker spacing and CoD-range engagements (8–40 m). Also an exact 4× of the 100 uu engine BasicShapes cube → whole-number DMI scales and float-clean snap math (`cell = floor(P / 400)`). |
| Wall height (1 level) | **300 uu** (3 m) | UE pawn is 176 uu tall, standing eye ≈ 160 uu, jump apex ≈ +65 uu. A 300 uu wall is guaranteed full cover: cannot be seen or jumped over. 3× the 100 uu cube. |
| Ramp slope | 300 rise / 400 run = **36.87°** | **Identical angle to Fortnite's 384/512.** Ramp running/sliding feels exactly like Fortnite. Below UE default walkable angle (44.76°) — no character tuning needed. Bonus: 300-400-500 is a 3-4-5 triangle, so the graybox ramp plank is exactly 500 uu long. |
| Vertical levels | **4** (bases at Z = 0, 300, 600, 900) | Height cap 1200 uu (12 m) — see §7. |
| Field size | **16 × 10 cells** (6400 × 4000 uu = 64 × 40 m) | NXL speedball fields are ~46 × 36 m; ours is slightly deeper because each team authors its own half. |
| Grid origin | Field corner at world (0,0,0); +X = Team A → Team B axis | All grid math in field-local space; the graybox map places the field at world origin. |
| Sub-grid (props) | **100 uu** | Props snap to quarter-cell for organic bunker layouts; still integer math. |

Grid coordinates are integers everywhere: structural pieces address **cells** `(cx, cy, level)`;
props address **sub-grid** `(sx, sy, sz)` in 100 uu units. No free-floating floats in placement data.

---

## 2. Piece set (7 total: 4 structural + 3 props)

**Destructibility — DECIDED: everything is indestructible in v1.** Paintballs don't break bunkers;
the arena players voted on must be the arena they fought in (mutating arenas poison the vote
signal); and it deletes an entire replicated-health system from scope. Pieces are deletable during
BuildPhase only (§6).

All meshes are `/Engine/BasicShapes/` primitives (Cube, Cylinder, Cone) with per-piece
`UMaterialInstanceDynamic` team-tinted colors on `BasicShapeMaterial`. Collision = the scaled
primitive's simple collision. All pieces block `Pawn`, `Visibility`, and `ECC_GameTraceChannel1`
(**Paintball** projectile channel).

### Structural (snap to cell grid)

| Piece | Hotkey | Dims (uu) | Mesh + scale | Snap slot | Rotation |
|---|---|---|---|---|---|
| **Wall** | F1 | 400 W × 20 T × 300 H | Cube (4.0, 0.2, 3.0) | Cell **edge** + level. One wall per edge. | None — auto-orients to edge (Fortnite walls don't rotate either). |
| **Floor** | F2 | 400 × 400 × 20 | Cube (4.0, 4.0, 0.2) | Cell + level (sits at level base Z, top surface at Z+20). One per cell per level. | Rotation-invariant. |
| **Ramp** | F3 | 400 × 400 footprint, rises 0→300 | Cube plank 500 × 400 × 20, pitched −36.87°, anchored low edge | Cell **volume** + level. One *incline* (ramp XOR roof) per cell per level. | 4 × 90° yaw. Defaults facing away from player camera (Fortnite behavior); `R` cycles; resets after each placement. |
| **Roof** | F4 | 400 × 400 footprint, 150 H pyramid proxy | Cone (4.0, 4.0, 1.5) → r 200, h 150 | Same incline slot as Ramp (mutually exclusive per cell-level). | Rotation-invariant in v1 (cone); `r` still stored for future pyramid mesh. |

Wall edges are shared between adjacent cells and **canonicalized** server-side: every edge is stored
as anchor cell + `{0 = North, 1 = East}`; a South/West placement maps to the neighbor cell's N/E.
One wall per canonical edge, period.

### Props — inflatable bunker proxies (snap to 100 uu sub-grid, wheel-only)

Picked 3 (not 5): one tall, one mid wedge, one low — the minimal speedball vocabulary
(stand-up, snap-shoot, slide cover). More shapes = more art debt for zero new tactical roles.

| Prop | Real-world analog | Dims (uu) | Mesh + scale | Cover role |
|---|---|---|---|---|
| **Can** | Stand-up cylinder | r 60, h 220 | Cylinder (1.2, 1.2, 2.2) | Full crouch cover, standing head peek; wrap-around snap shooting. |
| **Dorito** | Wedge/tetra | r 120, h 100 | Cone (2.4, 2.4, 1.0) | Mid cover; WALKABLE by design (39.8° slope < 44.8° limit) so players can jump on and hold the top. Was h 200 until 2026-07-24 — too steep to stand on. |
| **Snake** | Low snake segment | 400 L × 120 W × 120 H | Cube (4.0, 1.2, 1.2) | Slide-behind low cover (crouch eye ≈ 110 uu); chain segments end-to-end for a classic snake run. |

Prop rules: must rest on terrain or a floor top (base Z = support surface); may not overlap any
piece or prop (AABB/primitive overlap test); yaw in 90° steps via `R` (persists until piece switch —
props are deliberate placements, unlike ramp spam); count against the separate **prop budget** (§6).

---

## 3. Controls & the Build Wheel

**DECIDED — hotkeys and wheel coexist by role:** direct hotkeys (F1–F4, exactly Fortnite's default
piece binds) are the *speed path* for structural pieces; the **Build Wheel** is the *discovery and
prop path* (props + delete tool live only there, plus mirrors of the 4 structural pieces). Fortnite
hands get instant familiarity; the wheel delivers Tom's signature UX without slowing anyone down.

During **BuildPhase, build mode is always on** — there are no weapons to switch away from, so no
mode toggle is needed. During **CombatPhase every build input is dead** (§6). `Q` is still bound for
muscle memory: it re-equips your last-used piece (first press of the match = Wall, the Fortnite
pro-standard `Q = wall`).

### Keybind map (BuildPhase)

| Input | Action |
|---|---|
| **F1 / F2 / F3 / F4** | Equip Wall / Floor / Ramp / Roof |
| **Q (tap, <180 ms)** | Equip last-used piece (session default: Wall) |
| **Q (hold, ≥180 ms)** | Open **Build Wheel**; release to select |
| **LMB** | Place equipped piece (**hold = turbo-build**, §4) |
| **R** | Rotate ghost +90° (ramp/props; no-op on wall/floor/roof) |
| **Mouse wheel** | Cycle equipped slot through wheel order (Wall→Floor→Ramp→Roof→Can→Dorito→Snake→Delete→Wall…) |
| **F5 or X** | Equip **Delete tool** (both bound; X for one-hand reach) |
| **G** | Reserved (Edit tool, v1.1 — cut, §9) |
| Movement (WASD, Space, Shift, Ctrl/C) | Unchanged from combat spec; building never slows movement |

Enhanced Input: all `UInputAction`/`UInputMappingContext` objects constructed natively in
`UCombatForgeInputConfig` (per locked decision — no `.uasset`s). Two contexts: `IMC_Build`
(priority 1, added on BuildPhase enter, removed on exit) layered over `IMC_Locomotion`.

### Build Wheel spec (C++ UMG, `UBuildWheelWidget`)

- **8 sectors of 45°**, clockwise from top: Wall, Floor, Ramp, Roof, Can, Dorito, Snake, **Delete**.
- Opens on Q-hold ≥180 ms at screen center; mouse look is captured to a wheel cursor while open
  (pawn keeps moving on WASD; camera frozen while wheel open — matches Fortnite's edit-material feel
  without pausing anything, and it's replication-safe since the wheel is purely client-side).
- Inner **dead zone r = 90 px**: release inside = cancel (keeps current piece). Sector select
  radius 90–280 px; hovered sector scales 1.08× and brightens.
- Center readout: hovered piece name + remaining budget ("Wall — 23 left").
- Sector body: flat color swatch + piece silhouette drawn with `UImage`/`UBorder` in
  `WidgetTree` inside `NativeConstruct` — zero assets.
- Digits **1–8** while wheel is open = instant sector select (accessibility/speed).
- Wheel state is client-only; only the resulting equip is used in the (client-side) ghost, and only
  placement RPCs hit the server.

---

## 4. Placement mechanics

### Ghost preview pipeline (client-side, every tick while a piece is equipped)

1. **Trace:** line trace from camera through crosshair, max **1200 uu** (3 cells — Fortnite-ish
   reach; long enough to floor-over a gap, short enough to keep authors near their work), channel
   `BuildTrace` (hits terrain + placed pieces, ignores pawns).
2. **Anchor point `P`:** trace hit → `P = hit + normal × 8` (bias off the surface). No hit →
   `P = camera + forward × 800`.
3. **Quantize by piece:**
   - **Floor:** `cell = floor(P.xy / 400)`, `level = clamp(round(P.z / 300), 0, 3)`.
   - **Wall:** same cell; pick the nearest of the 4 cell edges by distance from `frac(P.xy / 400)`
     to each edge midline; `level = clamp(round(P.z / 300), 0, 2)` (a level-3-based wall would
     breach the height cap). Canonicalize edge (§2).
   - **Ramp / Roof:** cell + level like Floor; ramp yaw = player camera yaw snapped to 90°
     (low edge nearest the player) plus the `R` offset.
   - **Props:** `subpos = round(P.xy / 100)`; Z = top surface of whatever supports that XY
     (terrain or highest floor below `P.z`); yaw = `R` offset.
4. **Validate** (same predicate the server runs, §4-validation) → tint ghost.
5. **Render ghost:** the piece's mesh at 35% opacity via a shared translucent MID —
   **green** `(0.1, 0.9, 0.2)` valid, **red** `(0.95, 0.1, 0.1)` invalid. Invalid ghost still renders
   at the snapped slot (players learn the grid by seeing where it *would* go).

### Placement rules

- **LMB** with a valid ghost → `Server_PlacePiece(FBuildPieceRec)` RPC. Ghost is pure client
  prediction; the server re-validates everything and is the only authority that spawns state.
- **Turbo-build: YES in v1.** Hold LMB: a placement fires whenever the ghost's snapped slot changes
  *or* every **0.15 s** in the same slot-family, whichever comes first. Client self-caps at 8 RPC/s;
  server hard-caps at **10 placements/s/player** (excess silently dropped). Turbo is half of
  Fortnite's feel and costs ~30 lines; it ships.
- **Rotation reset:** ramp `R`-offset resets after each placement and on piece switch (Fortnite
  behavior). Prop `R`-offset persists until piece switch (§2).
- **Placing while moving/jumping/sliding: fully allowed**, zero movement penalty.
- **Pawn overlap — DECIDED: place-and-eject, not block.** Pieces ignore pawn overlap at validation.
  After a piece spawns, any pawn whose capsule penetrates it is depenetrated **upward** to stand on
  the piece's top surface (single teleport, server-side). This is the Fortnite ramp-under-your-feet
  feel; blocking on pawns would kill turbo-flooring under yourself.
- **Occupancy:** one Floor per (cell, level); one incline (Ramp XOR Roof) per (cell, level); one
  Wall per canonical edge+level; props by physical overlap test only.
- **Anchor rule (anti-sky-spam):** a piece is placeable iff its bounds touch terrain (level 0 always
  qualifies) **or** touch any existing structural piece (any team's — moot in practice since plots
  don't overlap). No structural-integrity re-check on delete: deleting a supporting piece may leave
  floaters. Accepted for v1 — deletes only happen in BuildPhase, and collapse propagation is pure
  scope with indestructible pieces.

### Validation predicate (client for tint, server as authority — shared static function)

```
bValid =  Phase == BuildPhase
       && PieceInsideOwnTeamPlot(bounds)            // §7
       && TopOfPiece <= 1200                        // height cap
       && SlotFree(type, cell/edge, level)          // structural
       && !OverlapsAnyPiece(bounds)                 // props
       && HasAnchor(bounds)
       && Budget[type-class] > 0
       && PlacementsThisSecond < 10                 // server only
```

Server rejection → client RPC `Client_PlaceDenied(reason)` → ghost flashes red 0.2 s + denial tick
sound stub. No client-side rollback needed because nothing was predicted into the world.

**Delete tool:** ghost-highlights the aimed piece (trace 1200 uu) in **orange** `(1.0, 0.55, 0.1)`;
LMB → `Server_DeletePiece(PieceId)`. Valid targets: any piece owned by **your team**, BuildPhase
only. Refund credits the **original builder's** budget (§6). Turbo-delete allowed (same cadence).

---

## 5. Team build zones (field layout)

```
x:   0     1..6           7..8          9..14     15
   [A spawn][A build plot][NEUTRAL strip][B build plot][B spawn]     y: 0..9
```

- **Team plot:** 6 × 10 cells (60 cells, 24 × 40 m) per team. All placement confined to your plot —
  the plot boundary is the single spatial validity check.
- **Neutral strip:** 2 cells wide at midfield, **no building ever**. Guarantees a contested open
  lane and prevents wall-to-wall stalemate turtling at the 50.
- **Spawn columns** (x = 0 and x = 15): no building, and no pieces at *any level* above them (no
  roofing the enemy… or your own spawn). Spawns + out-of-bounds are combat-spec territory; build
  system just enforces the no-build volumes.
- **The Curtain — DECIDED: yes.** During BuildPhase an opaque blocking plane (scaled cube, team-tint
  gray) stands over the neutral strip (full field width at x = 7.5, full height): teams cannot see or
  enter the enemy half while building. It despawns at CombatPhase start. Rationale: reveal moment
  creates the "what did *they* build?!" beat that makes arenas worth voting on; symmetric-info
  building would converge on mirror turtling.

### Anti-grief

| Vector | Mitigation |
|---|---|
| Building on enemy plot / neutral / spawn | Hard validation reject (server) |
| Sky towers | Height cap 1200 uu + anchor rule |
| Teammate deletes your work | Allowed (team-shared arena beats piece-ownership fights), but refund goes to the original builder and every delete is attributed in the kill-feed-style event log ("Alice removed Bob's Wall") — social pressure is the enforcement in v1 |
| Budget drain | Impossible: budgets are strictly per-player (§6) |
| Walling off your own spawn / unwinnable maze | **PUNTED in v1** — no path-guarantee solver. Rationale: (a) spawn columns can't be enclosed (no-build above/at spawn, and the cells adjacent to spawn are the *griefer's own* team's problem), (b) the neutral strip is always open, (c) a walkability solver over ramps/levels is real R&D, and (d) the vote loop is the designed corrective — trash arenas get thumbs-down + "layout/flow" dislike tags and die in the future map pool. v1.1 candidate: ground-level flood-fill warning at BuildPhase end. |

---

## 6. Build economy

**DECIDED: per-player budgets, build-phase-only.** Shared team pools invite drain-griefing and
free-riding; unlimited invites spam walls that flatten arena quality. A per-player cap makes every
piece a choice (better arenas → better votes) and caps total replication load by construction.

| Parameter | Value | Notes |
|---|---|---|
| Structural budget | **30 per player** | 4v4 → up to 120 structural on a 60-cell plot: enough to fully develop 2 lanes + verticality, not enough to fill the plot. |
| Prop budget | **6 per player** | Separate pool so bunker flavor can't be starved by wall spam. 24 props/team ≈ real speedball density (NXL fields run ~25 bunkers total). |
| Delete refund | **100%, instantly, to the original builder** | BuildPhase only. Iteration should be free — the timer is the real currency. |
| Build during CombatPhase | **NO — build-phase-only, hard cut** | The pitch is design-then-play; the vote is about a *fixed* arena; and CoD-style combat pacing dies if fights turn into Fortnite box battles. All build inputs disabled, all `Server_Place/Delete` RPCs rejected by phase check. |
| Edit/delete during CombatPhase | **NO**, same rationale — the arena is frozen at BuildPhase end | |
| Delete enemy pieces | **Never**, any phase | |
| BuildPhase duration | Owned by `02-match-loop.md`; this system is tuned for **90–150 s** (30 + 6 placements at turbo cadence uses <20% of that — the time goes to thinking, which is the point) | |

HUD (C++ UMG, bottom-right): `▦ 23/30   ◆ 4/6` + equipped-piece name. Budget lives on
`ACombatForgePlayerState` (2 replicated uint8s), server-mutated only.

---

## 7. Data model, replication, serialization

### The piece record (one struct everywhere: wire, memory, disk)

```cpp
UENUM(BlueprintType)
enum class EBuildPiece : uint8
{
    Wall = 0, Floor = 1, Ramp = 2, Roof = 3,
    PropCan = 4, PropDorito = 5, PropSnake = 6
};

USTRUCT()
struct FBuildPieceRec : public FFastArraySerializerItem
{
    GENERATED_BODY()
    UPROPERTY() uint16 PieceId   = 0;  // server-assigned, monotonically increasing, stable for the match
    UPROPERTY() EBuildPiece Type = EBuildPiece::Wall;
    UPROPERTY() int16  X = 0;          // sub-grid units (100 uu). Structural: multiples of 4.
    UPROPERTY() int16  Y = 0;          //   Anchor = cell min-corner (structural) / prop center (props).
    UPROPERTY() int16  Z = 0;          // sub-grid units. Structural: level * 3 (i.e. 0/3/6/9).
    UPROPERTY() uint8  Rot = 0;        // 0-3 = 90° yaw steps. Walls: canonical edge (0=N, 1=E).
    UPROPERTY() uint8  Owner = 0;      // player index within match roster (0-7)
    UPROPERTY() uint8  Team  = 0;      // 0 = A, 1 = B
};
```

10 bytes of payload per piece. Everything is integer sub-grid — a full 240-piece arena is ~2.4 KB,
one comfortable replication burst even on join-in-progress.

### Replication — DECIDED: FastArray on one manager actor + ISM rendering, not per-piece actors

- **`ABuildGrid`** (one per match, always relevant, replicated): owns
  `FBuildPieceArray : FFastArraySerializer { TArray<FBuildPieceRec> }`. Server adds/removes records;
  clients get per-item deltas for free.
- **Rendering/collision (client + server):** 7 `UInstancedStaticMeshComponent`s on `ABuildGrid`
  (one per `EBuildPiece`, engine primitive mesh + team-tint MID via per-instance custom data float
  for team). `PostReplicatedAdd/Remove` → add/remove ISM instance; a `TMap<uint16 PieceId,
  FInstanceHandle>` survives ISM swap-removal. ISM instances carry the collision.
- Why not actors: 240 replicated `AActor`s works at this scale but pays actor overhead, spawn
  hitches during turbo-build, and per-actor relevancy churn for zero benefit. FastArray + ISM is the
  same effort in C++ and is the shape that survives bigger arenas and the future map-pool loader
  (an `ArenaLayout` file replays straight into the same array).
- RPC surface: `Server_PlacePiece(rec)` (server assigns `PieceId`, fills `Owner/Team` from the
  requesting `PlayerState` — never trusts the client's), `Server_DeletePiece(uint16 PieceId)`,
  `Client_PlaceDenied(uint8 Reason)`.

### ArenaLayout serialization (spec'd now, v1 uses it for save/debug)

Written at **CombatPhase start** (the frozen arena) to
`Saved/Arenas/arena_<UTC yyyyMMdd_HHmmss>_<matchGuid8>.json` via `FFileHelper` + `FJsonSerializer`
(plain files beat SaveGame blobs: greppable, diffable, trivially uploadable to a future backend).
Vote block appended in place at VotePhase end.

```json
{
  "schema": 1,
  "game": "CombatForge",
  "grid": { "cellUU": 400, "subUU": 100, "wallH": 300, "cellsX": 16, "cellsY": 10, "levels": 4 },
  "match": { "id": "8f3a2c1e", "createdUtc": "2026-07-09T21:14:03Z", "teamSize": 4 },
  "pieces": [
    { "id": 1,  "t": 0, "x": 8,  "y": 12, "z": 0, "r": 1, "own": 2, "team": 0 },
    { "id": 2,  "t": 4, "x": 21, "y": 15, "z": 0, "r": 0, "own": 3, "team": 0 }
  ],
  "votes": [
    { "player": "a1b2c3d4", "up": true, "liked": ["cover", "flow"], "disliked": ["verticality"] }
  ]
}
```

- `t/x/y/z/r/own/team` mirror `FBuildPieceRec` fields exactly — serializer is a dumb field copy.
- `schema` is the migration key; the future backend aggregates by hashing `grid` + sorted `pieces`
  (arena identity) independent of `votes`. Vote tag vocabulary is owned by `05-voting.md`;
  `player` is an anonymized hash, never a raw name.
- Loading a layout (map-pool future, debug now): clear FastArray → append records → replication and
  ISM pipeline rebuild the world with zero extra code. A `pf.LoadArena <file>` cheat console command
  ships in v1 for debugging.

---

## 8. v1 scope cuts (explicit)

| Cut | Why / where it lands |
|---|---|
| **Edit tool (G)** | Fortnite's biggest system after building itself; delete + replace covers v1 authorship. G stays reserved. v1.1. |
| Destructible pieces / piece health | Decided against, §2. Revisit only if playtests demand breach mechanics. |
| Structural collapse on delete | Floaters allowed; build-phase-only deletes make it cosmetic. |
| Path/reachability solver | Punted with mitigations, §5. Flood-fill warning is v1.1. |
| Combat-phase building/editing | Decided against, §6 — design pillar, not a deferral. |
| 45° prop rotation, curved snake pieces | 90° only; needs real meshes anyway. |
| Material tiers / build costs beyond piece count | One "material". Paintball, not Minecraft. |
| Prefab/blueprint stamps ("copy my tower") | Map-pool era feature. |
| Controller/gamepad build bindings | KBM only in v1; `IMC_Build` structure already isolates the add. |
| Backend arena upload | Local JSON only; the file format above is the contract. |

---

## 9. Open questions (need Tom)

1. **Team size** — economy above is tuned for 4v4 (budgets and plot density scale linearly; 5v5 →
   drop structural budget to 24). Confirm the v1 headcount.
2. **The Curtain** — I decided *hidden enemy half during build* for the reveal moment. If you'd
   rather teams scout and counter-build each other live, delete the curtain actor; nothing else
   changes.
3. **Prop flavor set** — Can/Dorito/Snake is the classic speedball trio. Any must-have from your
   paintball days (Temple? Aztec?) swaps 1-for-1 as long as it maps to a scaled engine primitive.
