# Combat Forge — AI Pathfinding & Intelligence: Research + Implementation Plan

*Research doc, 2026‑07‑14. Written overnight per Tom's request: "the goal is intelligent AI… they run into walls and they're pretty dumb… find out how other games (Gray Zone Warfare, CoD, Battlefield) do bots/pathfinding, and whether we can implement it here." Nothing here is built yet — this is the plan to review in the morning.*

---

## 0. TL;DR

**Why the bots are dumb:** they have *no pathfinding at all*. `PFBotController` steers the pawn straight at its target with `AddMovementInput` and a single forward sphere‑sweep "avoidance probe." There is **no navmesh in the project**, and every wall/floor/build‑piece is explicitly `SetCanEverAffectNavigation(false)`. A greedy "turn if something's ahead" rule cannot route around a corner, into a doorway, or out of a concave bunker — so they grind on walls. This is the single biggest fix and it's a well‑trodden one.

**The one hard constraint that shapes everything:** the arena is **built by players every match**. That immediately rules out the classic CoD approach (designers hand‑place waypoint nodes) and points straight at the Battlefield/UE approach: a **navmesh auto‑generated from geometry**, made **dynamic** so it rebuilds as pieces are placed. UE5 does this out of the box.

**Recommended path (phased, each phase is a real improvement):**
1. **Dynamic navmesh + real pathfinding** → bots stop running into walls. *Biggest win, ~1–2 days.*
2. **Perception + a proper behavior layer** → they react to sight/sound, chase last‑known‑position, investigate.
3. **EQS‑style tactical positioning** → cover, flanking, holding angles, spreading out.
4. **Accuracy model** (Gray Zone‑style) → believable, difficulty‑tunable aim built on the per‑class weapon stats we already have.
5. **Squad coordination** → they flank as a group instead of clumping.

**One architectural decision for you to make (§7):** UE's standard AI tools — **Behavior Trees and EQS — are editor assets (`.uasset`)**, which breaks Combat Forge's "zero editor assets, all C++" rule. The navmesh does *not* (it's level/project config). So we can get pathfinding with zero rule‑breaking, but for the *intelligence* layer we either (a) bend the zero‑asset rule for AI only, or (b) keep everything in C++ and implement EQS‑style queries by hand. I lean (b) with a fallback to (a) — details below.

---

## 1. Where we are today (codebase diagnosis)

`Source/PaintForge/AI/PFBotController.cpp` is a **reactive steering bot** ("boids‑style"), not a planner:

- **Movement** (`Tick`, ~L200‑250): computes a flat direction toward the enemy/objective, adds a strafe offset and a range‑keeper (back off if closer than `MinRangeUU`, push in past `PreferredRangeUU`), then `Bot->AddMovementInput(MoveDir)`. No path, no waypoints.
- **Obstacle handling** (`SteerAvoidingObstacles`, ~L460‑480): one forward `SweepSingleByChannel` of radius `AvoidProbeRadius=46` out to `AvoidProbeUU=420`. If the sweep hits, it nudges the heading. That's it.
- **Unstick hack** (~L226‑244): if the bot hasn't moved ~`StuckMoveThresh` in 0.5 s, it peels sideways for 0.7 s. This is a band‑aid for the fact that the bot has no idea how to get *around* things.
- **Targeting:** `AcquireNearestEnemy` — nearest visible enemy. No memory, no last‑known‑position, no hearing.
- **Fire:** `SetFiring` gates on `bAliveInRound` + combat‑live + LOS; recent work added per‑class fire modes and infinite‑ammo top‑ups.

Two facts make pathfinding impossible as built:
1. **No navigation system is used anywhere.** Grep for `UNavigationSystem`, `MoveToLocation`, `MoveToActor`, `RecastNavMesh`, `NavMeshBoundsVolume`, `ProjectPointToNavigation` → **zero hits** in gameplay code.
2. **Nothing is navigation‑relevant.** `PFBuildGrid.cpp:115` (`ISMC->SetCanEverAffectNavigation(false)`), `PFArenaShell.cpp:557/887`, ammo barrels, grenades — all opt *out* of navigation. Even if a navmesh existed, it would treat the built fort as empty air.

So the reactive probe is doing all the work, and it can only ever be "don't walk directly into the thing 4 m in front of me." Corners, doorways, U‑shaped bunkers, and stacked walls all defeat it.

---

## 2. The constraint that decides the architecture: **the map is built every match**

Combat Forge's whole identity is that the arena is player‑built on a snap grid during the Build phase, then fought in. That means **the collision geometry doesn't exist until runtime and is different every match** (and can even change mid‑match if pieces are added/removed). Any AI navigation solution has to cope with that.

This maps cleanly onto how the reference games solve navigation:

| Game | Navigation approach | Fits Combat Forge? |
|---|---|---|
| **Call of Duty** | Designers hand‑place **waypoint nodes**; a post‑compile step links them into a path graph; "traversal" links carry climb/vault anims. | **No.** There are no designers and no compile step — the map is built live by players. Hand‑authored nodes are impossible. |
| **Battlefield** | Auto‑generated **navmesh** from level geometry, baked into pathfinding files (infantry/vehicle **quadtrees + clusters**). Handles multi‑level buildings, bridges. | **Yes, in spirit.** Navmesh is derived from geometry automatically. We need the *live* version of this. |
| **Gray Zone Warfare (UE5.5)** | UE5 navigation + a heavily reworked perception/accuracy/behavior layer ("react, aim, shoot like real combatants"). | **Yes — closest analog.** Recent UE5 tactical FPS; their model is our north star for the *intelligence* + *accuracy* layers (§5, §6). |

**Conclusion:** we want Battlefield's "navmesh from geometry," made **dynamic** so it regenerates as the fort is built — which is precisely what **UE5's runtime navmesh** provides. No hand‑authoring, no per‑map work; it just tracks the geometry.

Sources: [CoD/BF bot navigation (Classic BF modding wiki)](https://classic-battlefield-modding.fandom.com/wiki/AIBehaviors_and_the_Navmesh), [Gray Zone Warfare AI rework](https://www.pcgamesn.com/gray-zone-warfare/update-enemy-ai-rework).

---

## 3. Solution Part A — Dynamic navigation (stop running into walls)

This is the phase‑1 win and it's mostly engine configuration + flipping flags, not a rewrite.

### 3.1 Stand up a navmesh over the arena
- Add a **`NavMeshBoundsVolume`** sized to the play area in `L_Graybox`, plus the auto‑created **`RecastNavMesh`** actor. (These live in the level/`.umap`, not in a content asset — so this does **not** break the zero‑asset rule.)
- Set agent radius/height to match the pawn capsule so paths don't scrape walls (the current `AvoidProbeRadius=46` is a decent hint for agent radius).

### 3.2 Make it **Dynamic** so it rebuilds as the fort is built
- **Project Settings → Navigation Mesh → Runtime → Runtime Generation = `Dynamic`.** Three modes exist:
  - *Static* (default): baked offline, never changes → useless for a built map.
  - *Dynamic Modifiers Only*: baked base + runtime modifiers (cheaper, ~50% less cost) — good for *moving* obstacles on a fixed floor, but it can't add navmesh over *new* surfaces.
  - **`Dynamic`**: regenerates navmesh over geometry that appears/changes at runtime → **this is the one we need**, because build pieces are new geometry.
- The navmesh is **tiled**: only the tiles touching a changed piece rebuild, asynchronously (UE Task system). So the cost of placing one wall is local, not "rebuild the whole map."

### 3.3 Make build pieces + arena actually block navigation
The flags are currently the enemy of pathfinding. Flip them for structural geometry:
- `PFBuildGrid.cpp` structural ISMs: `SetCanEverAffectNavigation(true)` (walls/floors/ramps). **Caveat:** Instanced Static Meshes *can* export nav collision, but with many instances this can be heavy — evaluate two options: (a) let the ISM export nav collision directly, or (b) drop a lightweight **`NavModifierVolume`/`UNavModifierComponent`** (area class = `Null`/blocking) per placed piece, which is cheaper and more predictable than meshing every instance. Prototype (a); fall back to (b) if tile rebuilds spike.
- `PFArenaShell.cpp` outer walls/floor: `SetCanEverAffectNavigation(true)` so the floor is walkable and the shell blocks.
- Keep barrels/props as **cover** (nav‑relevant so bots path around them) but tune so they don't fragment the mesh into uselessness — small props can be nav‑*irrelevant* and just rely on capsule avoidance.
- Ammo barrels: make them nav‑relevant so bots can path to them (fixes the old "bots can't reach the [F] barrels" note).

### 3.4 Trigger rebuilds at the right moments
- On piece place/remove, the dirtied tiles auto‑rebuild in Dynamic mode. For big batch injects (Improvement mode loads a whole saved fort, Creative pre‑builds a half), call `UNavigationSystemV1::Build()` (or rely on dirty‑area updates) once after the batch so bots aren't pathing across stale tiles.
- **Gotcha (well documented):** runtime‑spawned geometry sometimes doesn't dirty the navmesh unless it's genuinely nav‑relevant and registered — verify with `show Navigation` / `Draw Tile Bounds` that placed walls actually punch holes in the mesh.

### 3.5 Switch the bot from steering to **path following**
- Replace the `AddMovementInput(MoveDir)` core with `AIController::MoveToLocation()` / `MoveToActor()` (or a `UPathFollowingComponent`). UE handles the A* over the navmesh + smooth corner‑cutting + local avoidance (RVO/detour crowd) for free.
- Keep the *tactical* direction choices (strafe, keep‑range, hold‑objective) as **goal selection** — i.e., pick a good destination, then let MoveTo path there. The strafing/range logic stops being "which way do I push" and becomes "which nav point do I go to."
- The unstick hack can be deleted once real pathing lands.
- **Perf:** use **Nav Invokers** (generate/keep navmesh tiles only around bots + players) so we're not meshing the whole arena every frame — matters at 8–12 bots.

**Result of Part A:** bots route around walls, through doorways, up ramps, and out of bunkers, on a map they've never seen because it was built 30 seconds ago. This alone turns "pretty dumb" into "competent."

Sources: [Epic — modifying the navmesh at runtime](https://dev.epicgames.com/documentation/unreal-engine/overview-of-how-to-modify-the-navigation-mesh-in-unreal-engine), [Runtime navmesh for runtime‑spawned geometry](https://bugnet.io/blog/how-to-fix-unreal-navmesh-runtime-generation-not-updating), [Navmesh rebuild at runtime (UE forums)](https://forums.unrealengine.com/t/navmesh-rebuild-at-runtime-in-unreal-engine-5/1879759).

---

## 4. Solution Part B — Intelligence (make them *smart*, not just mobile)

Pathing fixes movement; this fixes *decisions*. The proven FPS pattern (and what a strong UE4/5 reference — [mtrebi/AI_FPS](https://github.com/mtrebi/AI_FPS) — and Gray Zone both use) is **Sense → Think → Act**:

### 4.1 Perception (Sense)
- Add **AI Perception** (sight + hearing). Sight cone + range for spotting enemies; hearing for gunfire, footsteps, grenades.
- Track **last‑known‑position** per target. On losing LOS, don't teleport to "I forget you exist" — path to the last‑known spot and search (Gray Zone: "flank on your last known position, sneak‑walk to ambush").
- React to **sound**: investigate a gunshot/nearby splat instead of standing still.

### 4.2 Decision layer (Think)
Two viable structures (see §7 for the asset tension):
- **Behavior states**: Idle → Patrol/Advance → Engage → Reposition/Cover → Search → Retreat‑to‑reload. The current C++ is already a crude FSM; this is a cleaner, richer version.
- Per‑state goals feed Part A's MoveTo. E.g. *Engage* picks an attack position with LOS; *Reposition* picks cover; *Search* picks the last‑known‑position + a spiral.

### 4.3 Tactical positioning (the "smart" that players *feel*) — EQS or C++ equivalent
The single most impactful "intelligence" upgrade after pathing is **choosing good positions**, which is what **EQS (Environment Query System)** does in UE, and what a C++ point‑sampler can replicate:
- **Cover:** sample points around the bot; keep those that (a) break LOS from the enemy's eyes and (b) are close/quick to reach; discard exposed ones. Move to the best. This is a literal EQS query (Generator = points on navmesh near me; Tests = trace‑visibility to enemy, distance, path cost).
- **Attack position:** sample points that *have* LOS to the enemy but are near cover — "peek" spots.
- **Flanking:** bias attack points away from the enemy's facing / away from teammates' angles.
- **Spread:** subtract teammates' chosen positions so bots don't clump (mtrebi does exactly this via shared team data).

### 4.4 Squad coordination (Act, together)
- A tiny shared blackboard per team: each bot publishes its chosen attack/cover/flank point; others query it and pick *different* angles. Emergent flanking without a "commander."
- Optional: a lightweight team target‑focus so 3 bots don't all chase one player while ignoring another.

**Gray Zone's observable behavior list is a great acceptance checklist for this phase:** hide, hold, charge, seek, investigate sounds/bodies, flank last‑known‑position, sneak to ambush from multiple directions.

Sources: [mtrebi/AI_FPS (UE4 BT+EQS FPS AI, Sense‑Think‑Act, tactical A*, squad spread)](https://github.com/mtrebi/AI_FPS), [Epic — Environment Query System](https://dev.epicgames.com/documentation/en-us/unreal-engine/environment-query-system-in-unreal-engine), [Gray Zone AI behaviors](https://steamcommunity.com/app/2479810/discussions/0/598538089547329090/).

---

## 5. Solution Part C — Accuracy (fair, believable, tunable)

Right now aim is basically "shoot if LOS." Gray Zone's reworked model is the gold standard for a tactical shooter and it maps **directly onto systems we already built**:

**Gray Zone's accuracy inputs:** distance vs the weapon's effective range, weapon type, the individual enemy's skill, current health, suppression, and weather; plus dynamic target‑lock (drop/switch target on reload, on being hit, or on losing LOS); plus human‑like aim patterns that change with difficulty/injury/suppression.

**How this lands in Combat Forge (most of the inputs already exist):**
- **Per‑class weapon stats we just added** — `SpreadHip/SpreadADS` (accuracy) + `MuzzleSpeedUU × ProjLifetime` (effective range) per weapon — are exactly the "weapon type + effective range" inputs. Bots should inherit them (they equip real weapons now).
- **Aim error as a cone that shrinks/grows** with: distance vs effective range, bot **skill tier**, whether the bot just started firing (first‑shot penalty) vs settled, and **suppression** (BBs whizzing past widens their cone — we already spawn BBs; count near‑misses).
- **Reaction time**: a spot‑to‑first‑shot delay scaled by skill/difficulty, so they don't laser you the instant you peek.
- **Target‑lock rules**: drop/switch target on reload, on taking a hit, on LOS loss — cheap to add and hugely improves "feel."
- **Difficulty tiers**: one `Skill` float already exists (`ApplySkill`) — expand it into a small profile (reaction time, aim cone, cover discipline, fire‑mode choice) and vary it per bot so a match feels like a mix of grunts and threats (§4 already varies fire mode per bot by roster hash).

This turns aim from "aimbot or whiffs" into "difficulty you can dial," which is what makes bots fun rather than cheap.

Source: [Gray Zone Warfare accuracy/aim rework](https://www.pcgamesn.com/gray-zone-warfare/update-enemy-ai-rework).

---

## 6. How the reference games do it — and what's transferable

| Game / classic | Technique | Transferable to Combat Forge? |
|---|---|---|
| **Call of Duty** | Hand‑placed waypoint node graph + traversal links | ❌ Navigation (needs designers/compile) · ✅ the *idea* of "traversal links" for vault/jump later |
| **Battlefield** | Auto navmesh → quadtree/cluster pathfinding files; multi‑level | ✅ Navmesh‑from‑geometry is our model; UE gives the live version |
| **Gray Zone Warfare (UE5.5)** | UE5 nav + reworked perception + situational accuracy + tactical behaviors | ✅✅ Closest analog; blueprint for §4 (intelligence) + §5 (accuracy) |
| **F.E.A.R. (2005)** | **GOAP** — goal‑oriented action planning; NPCs *plan* action sequences; famous "smart" combat | ⚠️ Powerful/emergent but heavier to build; consider as a §4 upgrade, not the starting point |
| **Halo** | **Behavior Trees** (Isla) — hierarchical, modular; industry standard | ✅ The mainstream structure; UE's BT is this (asset caveat, §7). A C++ BT/FSM captures 90% of it |

**Takeaways:**
- **Navigation:** copy Battlefield/UE (auto navmesh), *not* CoD (hand nodes). Non‑negotiable given built maps.
- **Decision‑making:** start with **Behavior‑Tree‑style hierarchy** (Halo lineage; what mtrebi + Gray Zone use). **GOAP** (F.E.A.R.) is the "make them scary‑smart" stretch goal once the basics work — it plans instead of following fixed branches, but it's more engineering and harder to debug.
- **Accuracy/feel:** copy Gray Zone's situational model; we already have the weapon‑stat plumbing for it.

Sources: [GOAP in F.E.A.R.](https://www.gamedeveloper.com/design/building-the-ai-of-f-e-a-r-with-goal-oriented-action-planning), [GOAP vs Utility vs Behavior Trees](https://tonogameconsultants.com/game-ai-planning/), [Game AI for beginners: FSM/BT/pathfinding](https://respawn.outlookindia.com/gaming/gaming-guides/game-ai-for-beginners-fsm-behavior-trees-pathfinding).

---

## 7. The one real architecture decision: BT/EQS assets vs pure C++

**Combat Forge is deliberately "zero editor assets — every mesh is an engine primitive + runtime material, all UI is C++/Slate, all input is native Enhanced Input."** That principle collides with the standard UE AI stack:

- **Navmesh, NavMeshBoundsVolume, RecastNavMesh, Nav Invokers, `MoveTo`** → **NOT assets.** They're level placement + project config + engine calls. **Fully compatible with the zero‑asset rule.** → Part A (pathfinding) is clean; do it as designed.
- **Behavior Tree + Blackboard + EQS query assets** → **these ARE `.uasset` content.** Adopting them the "normal" way breaks the rule.

**Options for the intelligence layer:**
- **(A) Keep it pure C++ (recommended default).** Extend `PFBotController` into a clean C++ behavior system (states/utility scoring) + implement EQS‑style spatial queries as C++ helpers (sample nav points → run traces/tests → pick best). This preserves the architecture, stays server‑authoritative and debuggable, and is very doable — the current controller is already C++. More upfront code than dragging BT nodes, but no rule‑break and no per‑behavior asset sprawl.
- **(B) Bend the rule for AI only.** Use UE's BT + EQS assets (fastest to author, best tooling, matches every tutorial/reference). Costs: introduces `.uasset` content the project has avoided, and BT/EQS want the editor for iteration.
- **(C) Hybrid.** C++ behavior selection (A) but adopt **EQS assets** just for the spatial queries (B), since hand‑rolling good spatial queries is the most tedious part.

**My recommendation:** **(A)** for the core (fits the project, keeps everything in C++/server‑authoritative), with **(C)** as the pragmatic escape hatch if hand‑rolled cover queries prove fiddly. This is a genuine call for Tom — it trades architectural purity against author‑speed, and I didn't want to silently break the zero‑asset principle. **Flagging for a morning decision.**

*(Note: none of this blocks Part A. Pathfinding is asset‑free either way — we can and should do it first regardless of how the intelligence question lands.)*

---

## 8. Recommended phased plan (each phase ships a real improvement)

**Phase 0 — Dynamic navmesh + real pathing (the "stop running into walls" phase).** *Highest value.*
- Add `NavMeshBoundsVolume` + `RecastNavMesh` to `L_Graybox`; Runtime Generation = **Dynamic**; agent radius = pawn capsule.
- Flip structural build pieces + arena shell to `SetCanEverAffectNavigation(true)` (prototype ISM nav; fall back to per‑piece `NavModifierComponent` if tile rebuilds spike).
- Nav Invokers on bots/players for perf. Rebuild after batch injects.
- Swap `PFBotController` movement core from `AddMovementInput` to `MoveTo…`; keep the tactical goal selection, delete the unstick hack.
- **Acceptance:** bots navigate a freshly‑built fort — around walls, through doors, up ramps, out of bunkers — with no wall‑grinding. Verify with `show Navigation`.

**Phase 1 — Perception + behavior states.**
- AI Perception (sight/hearing) + last‑known‑position + investigate‑sound.
- Restructure the decision layer (C++ per §7‑A): Idle/Advance/Engage/Reposition/Search/Retreat.
- **Acceptance:** bots chase where you *went*, investigate gunfire, and don't stand still when they lose you.

**Phase 2 — Tactical positioning (cover/flank/spread).**
- C++ (or EQS) spatial queries: pick cover (breaks LOS, quick to reach), attack peeks (has LOS, near cover), flank angles, and de‑clump via shared team positions.
- **Acceptance:** bots use the fort — hold corners, peek, flank, spread — instead of walking into your crosshair.

**Phase 3 — Accuracy model + difficulty tiers.**
- Aim cone from distance/effective‑range + skill + first‑shot + suppression; reaction‑time delay; target‑lock drop/switch on reload/hit/LOS‑loss; expand `Skill` into per‑bot profiles.
- Reuse the per‑class weapon stats (spread/range) bots already inherit.
- **Acceptance:** aim feels fair + difficulty‑tunable; a match reads as a mix of grunts and threats, not aimbots.

**Phase 4 — Squad coordination (stretch).**
- Shared team blackboard for angle assignment; coordinated flanks; optional GOAP experiment for emergent plans if we want F.E.A.R.‑tier smarts.

---

## 9. Risks, gotchas, and open questions for the morning

- **ISM navigation cost.** Structural pieces are ISMs; exporting nav collision for hundreds of instances can spike tile rebuilds. Mitigation: per‑piece `NavModifierComponent`, Nav Invokers, and only marking *structural* pieces nav‑relevant (props stay capsule‑avoidance). Needs a perf check at 8–12 bots on a dense fort.
- **Dynamic‑rebuild hitching.** Placing a wall dirties tiles; a huge Improvement‑mode inject dirties many. Batch the rebuild; keep tiles small; profile.
- **Server authority / netcode.** Navigation + AI run on the listen‑server host (bots already do). No client prediction needed for AI — keep it all host‑side, consistent with the current design.
- **The §7 asset decision** is the one thing I can't decide for you — it's an architecture‑principle trade‑off. Phase 0 doesn't depend on it, so I can start there immediately either way.
- **Scope check:** Phase 0 alone probably gets you 70% of the "these bots aren't dumb anymore" feeling, because most of the current dumbness is *movement*, not decisions. I'd build and validate Phase 0 first, then reassess how much of 1–4 you actually want.

---

### Sources
- [Epic — Overview of how to modify the Navigation Mesh at runtime](https://dev.epicgames.com/documentation/unreal-engine/overview-of-how-to-modify-the-navigation-mesh-in-unreal-engine)
- [Epic — Environment Query System (EQS)](https://dev.epicgames.com/documentation/en-us/unreal-engine/environment-query-system-in-unreal-engine)
- [Bugnet — Fix UE navmesh not updating for runtime‑spawned geometry](https://bugnet.io/blog/how-to-fix-unreal-navmesh-runtime-generation-not-updating)
- [UE Forums — Navmesh rebuild at runtime in UE5](https://forums.unrealengine.com/t/navmesh-rebuild-at-runtime-in-unreal-engine-5/1879759)
- [mtrebi/AI_FPS — UE4 FPS AI (BT + EQS + tactical A* + squad spread)](https://github.com/mtrebi/AI_FPS)
- [Gray Zone Warfare — enemy AI rework (accuracy, behaviors, UE5.5)](https://www.pcgamesn.com/gray-zone-warfare/update-enemy-ai-rework)
- [Gray Zone Warfare — AI behavior discussion](https://steamcommunity.com/app/2479810/discussions/0/598538089547329090/)
- [Classic Battlefield Modding — AI behaviors & the navmesh (BF navmesh/clusters; CoD nodes)](https://classic-battlefield-modding.fandom.com/wiki/AIBehaviors_and_the_Navmesh)
- [Building the AI of F.E.A.R. with GOAP](https://www.gamedeveloper.com/design/building-the-ai-of-f-e-a-r-with-goal-oriented-action-planning)
- [Game AI Planning: GOAP, Utility, and Behavior Trees](https://tonogameconsultants.com/game-ai-planning/)
- [Game AI for Beginners: FSM, Behavior Trees, Pathfinding](https://respawn.outlookindia.com/gaming/gaming-guides/game-ai-for-beginners-fsm-behavior-trees-pathfinding)
</content>
