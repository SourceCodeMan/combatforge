# 04 — Combat Feel Spec ("CoD-fast, paintball fiction")

**Owner:** Gunfeel design
**Scope:** Every tunable an engineer types into `APFCharacter`, `UPFCharacterMovementComponent`, `APFMarker`, `APFPaintballProjectile`, and the C++ UMG feedback widgets. All distances in Unreal units (1 uu = 1 cm). All angles in degrees. All times in seconds.
**Cross-refs:** hits-to-eliminate count and round format live in the game design doc (02). Arena/spawn-zone geometry lives in the build system doc (03). This doc specs the *mechanisms* and both spawn-feel variants.

---

## 1. Movement & Camera

### 1.1 Locomotion numbers (UCharacterMovementComponent config)

Subclass as `UPFCharacterMovementComponent` (needed for slide, §1.2). Set in the `APFCharacter` constructor.

| Property | Value | Notes |
|---|---|---|
| `MaxWalkSpeed` | **600** | CoD base-walk equivalent. |
| Sprint speed (custom, drives `MaxWalkSpeed` while sprinting) | **830** | 1.38x walk — CoD AR-class sprint. |
| `MaxWalkSpeedCrouched` | **300** | 0.5x walk. |
| ADS move multiplier (custom scalar on MaxWalkSpeed) | **0.55** → 330 | Applied multiplicatively; ADS+crouch = 300 × 0.55 = 165. |
| `MaxAcceleration` | **4096** | Reaches walk speed in ~0.15 s. Snappy, no ice-skating. |
| `GroundFriction` | **10.0** | High = instant direction changes (CoD strafe feel). |
| `BrakingDecelerationWalking` | **2500** | Stop from sprint in ~0.33 s. |
| `BrakingFrictionFactor` | **1.0** | Default 2.0 makes stops mushy-then-sudden; 1.0 is linear. |
| `JumpZVelocity` | **630** | ~1.95 m apex — clears the 1.5 m half-wall bunker tier (see arena doc), not the 2.5 m tier. |
| `AirControl` | **0.9** | CoD-high. Full strafe authority mid-jump. |
| `AirControlBoostMultiplier` | 2.0 (engine default) | Leave. |
| `GravityScale` | **1.5** | 1470 uu/s² effective. Short, punchy jump arc (~0.86 s airtime), kills floatiness. |
| `MaxStepHeight` | 45 (default) | Leave. |
| `PerchRadiusThreshold` | 15 | Prevents ledge-hang jank on player-built floor edges. |
| Crouch transition | Interp half-height 88→58 over **0.2 s** | Smooth camera, not a snap. |
| Coyote time / jump buffer | **None / 0.1 s buffer** | Buffer jump input 0.1 s before landing; no coyote time (builder game — edges are player-made, forgiveness reads as float). |

**Sprint rules:** hold-to-sprint (default) with a toggle option flag. Sprint is forward-hemisphere only (input dot forward > 0.5). Sprinting blocks firing and ADS; see sprint-out below. Sprint does not drain anything — infinite sprint (CoD MW-style, and matches "run the break" paintball fiction).

**Sprint-out time (sprint → first shot): 0.18 s.** Fire input during sprint immediately cancels sprint and starts the 0.18 s raise timer; the buffered fire input releases when the timer ends. ADS input during sprint: sprint cancels, ADS-in begins immediately (ADS-in time absorbs the sprint-out; total sprint→ADS-fire = 0.25 s, see §1.4).

### 1.2 Slide

**Decision: slide is in v1, and you CAN fire during it (hipfire only).** Slide-shooting is half of why CoD movement feels expressive; paintball fiction supports it perfectly (superman-slides into bunkers are iconic speedball). Rationale for hipfire-only: firing while ADS'd in a slide makes crouch-strafing obsolete.

Implemented as a custom movement mode `CMOVE_Slide` in `UPFCharacterMovementComponent` with full `SavedMove` prediction data (slide state must be client-predicted or it rubber-bands — non-negotiable).

| Parameter | Value |
|---|---|
| Trigger | Crouch pressed while: grounded, sprinting, current speed ≥ **750** (0.9× sprint) |
| Entry boost | Velocity set to **1150** uu/s along current velocity direction (not aim) |
| Capsule | Uses crouched half-height (58) for the full slide |
| Steering | Input steers at **15°/s** max heading change; camera aim is free |
| Friction curve | 0.0–0.35 s: friction **0.6** (glide) → 0.35–end: friction lerps 0.6 → **8.0** over 0.5 s |
| End conditions | speed < **320**, OR jump pressed, OR crouch released, OR 1.1 s hard cap |
| Exit state | Crouch-held: stays crouched. Crouch-released/timeout: stands. Jump: slide-jump keeping full horizontal velocity (this is the movement-skill ceiling — keep it) |
| Cooldown | **0.5 s** grounded time before next slide (kills slide-spam locomotion) |
| Fire during slide | **Yes**, hipfire, +1.0° spread penalty (§2.4) |
| ADS during slide | **No.** ADS input queues; applies on slide end |
| Downhill | Slope dot < -0.15 extends glide phase (friction stays 0.6 while descending) |

### 1.3 Camera

| Parameter | Value |
|---|---|
| Base FOV | **105** (UE horizontal FOV at 16:9). Expose 90–120 slider later; hardcode 105 for v1 |
| ADS FOV | **70** (≈1.5x perceived zoom — iron-sight class, right for a mid-range marker) |
| Sprint FOV kick | **+6**, lerp in/out over 0.15 s. Decision: yes — it is the single cheapest "this game is fast" signal |
| Slide FOV kick | **+9** during glide phase, lerp out with friction ramp |
| Head bob | **None.** Decision: zero bob. CoD's modern titles run near-zero; bob fights flick aim. Speed is sold by FOV kick + first-person arms later |
| Landing dip | Camera shake: 0.12 s, 1.5° pitch amplitude, only on falls > 300 uu. C++ `UCameraShakeBase` subclass (`UPFLandShake`), no assets |
| Camera location | Socket-less: capsule top minus 10 uu, standard `UCameraComponent` on a 0-length boom, `bUsePawnControlRotation` |

All FOV changes go through one arbiter function (`APFCharacter::UpdateTargetFOV()`) that composes base + ADS + kick so effects never fight.

### 1.4 ADS

| Parameter | Value |
|---|---|
| ADS-in time | **0.18 s** (paintball markers are ~1 kg — faster than a CoD AR's 0.25 s reads correctly) |
| ADS-out time | **0.14 s** |
| Curve | Ease-out cubic on FOV and (later) weapon socket lerp |
| Hold vs toggle | Hold-to-ADS default, toggle option flag |
| Fire while ADS-in | Allowed at any point; spread lerps with the transition alpha (§2.4) |

---

## 2. The Marker (one weapon, spec'd fully)

Class: `APFMarker` (actor, attached to character). Projectile: `APFPaintballProjectile`.

### 2.1 Rate of fire

**Decision: full-auto at 12 balls/sec (720 RPM), hold-to-fire.** Real speedball ramping caps at 10.5 bps; we run "tournament ramping" fiction at 12 bps because 720 RPM sits exactly in CoD AR territory (AK-74u/M4 class) and holding the trigger IS the ramping fantasy. No semi mode in v1 — mode switching is scope, and semi-only would read as slow.

- Fire interval: **0.0833 s**, accumulated in a timer that carries remainder across frames (no frame-quantized ROF).
- First shot fires on press instantly (no spin-up).
- Server-side ROF validation: token bucket, capacity 3, refill 12/s — tolerates jitter bursts, rejects macros (§5.2).

### 2.2 Projectile — visible paintballs, not hitscan

**Decision: true projectiles at 10,000 uu/s (100 m/s).** Real paintball is ~90 m/s; we round up for snap. At typical arena engagements (10–40 m) that is 0.10–0.40 s travel — visible, dodgeable at range, effectively instant up close. This is the paintball identity; do not hitscan it.

| Parameter | Value |
|---|---|
| Muzzle speed | **10,000** uu/s (`UProjectileMovementComponent::InitialSpeed/MaxSpeed`) |
| Gravity | `ProjectileGravityScale` **0.35** → drop ≈ 15 cm at 30 m, ≈ 65 cm at 60 m. Flat inside a firefight, lob at cross-map |
| Lifetime | **2.0 s** (`InitialLifeSpan`) ≈ 190 m ceiling |
| Inherit shooter velocity | **No** (adds aim noise for zero feel gain at these speeds) |
| Collision | Sphere radius **7 uu** (real ball is 0.85 cm radius — inflated ~8x for hit-reg forgiveness; this is our "bullet magnetism") |
| Visual (graybox) | `/Engine/BasicShapes/Sphere` scaled 0.14 (14 cm diameter), `UMaterialInstanceDynamic` on engine emissive-capable base material, team color, emissive boost ×3 so it reads as a tracer (§4.5) |
| Bounces | **None in v1.** Every impact breaks (spawns splat, destroys). Real-paintball bounces are a later fiction flourish, and they'd create self-splat edge cases now |
| Penetration | None. First blocking hit wins |
| Muzzle origin | Server & sim proxies: weapon socket approximation (capsule center + 30 forward, +20 right, −10 down). Owning client cosmetic: camera position + 20 forward so it never visibly clips own geometry |

### 2.3 Spread & bloom

Spread = half-angle cone applied as a random rotation (uniform in cone) to the fire direction, using a **deterministic seed** shared client/server per shot index (§5.2).

| State | Base half-angle |
|---|---|
| ADS, still/walking | **0.25°** (laser — ADS is the accuracy contract) |
| Hipfire, still | **1.5°** |
| Hipfire, moving (>50% walk) | **2.0°** |
| Airborne (jump/fall) | +**1.5°** additive |
| Sliding | +**1.0°** additive |
| Crouched | ×**0.8** multiplicative |

**Bloom:** +**0.12°** per shot fired, capped at +**1.8°** additive. Decays at **6°/s** starting 0.15 s after the last shot. ADS halves accumulated bloom's effect (×0.5 when applying). First-shot accuracy is therefore near-perfect ADS'd; a full 2-second hose walks the cone from laser to ~2°, which at 12 bps and visible projectiles is the balance lever against beam-crossmapping.

During ADS transition, base spread lerps hipfire→ADS with the transition alpha.

### 2.4 Hit registration & damage mechanism (number deferred to game design doc)

- Server-authoritative projectile performs the only real hit test (§5). On blocking hit against a pawn, server calls `IPaintHitReceiver::OnPaintHit(FPaintHitInfo)` on the victim.
- `FPaintHitInfo { AActor* Shooter; uint8 ShooterTeam; FVector_NetQuantize ImpactPoint; FVector_NetQuantizeNormal ImpactNormal; uint8 BodyRegion; float ServerTime; }` — this struct is also the persisted shape the vote/analytics backend can aggregate later.
- **One ball = one hit event.** No damage floats, no falloff — a paintball that arrives, breaks. The elimination system (game design doc) counts hit events to its hits-to-eliminate threshold and owns health/elim state. Combat feel's contract ends at delivering clean hit events + feedback.
- `BodyRegion`: v1 = single capsule, always `Body`. Enum reserves `Head/Body/Legs` so hit zones can land later without a data migration.
- **Friendly fire: OFF for elimination, ON cosmetically.** Decision: a teammate hit spawns the splat on their model/screen (fiction: paint is paint) but generates no hit event, no hitmarker, no elim progress. Griefing-free, fiction intact.
- **Self-splat: impossible in v1** (no bounces, projectile spawns outside own capsule, shooter is in the projectile's ignore list). Documented so nobody adds self-damage "for realism."
- Eliminated players: capsule stops generating hit events but still blocks projectiles for **0.5 s** (splats on a falling body feel right), then collision off.

---

## 3. "Feels like unlimited ammo" — hopper model

**Decision: 100-ball hopper + infinite reserve + 1.0 s reload, auto-reload on empty.**

Rejected alternatives:
- *Truly infinite*: kills the last texture of weapon rhythm; also breaks paintball fiction (everyone knows you run dry).
- *Heat/cooldown*: reads sci-fi, not paintball; punishes the hold-trigger ramping fantasy we just sold.

Chosen model in numbers:

| Parameter | Value |
|---|---|
| Hopper capacity | **100** balls (8.3 s of continuous full-auto — nobody hits empty in a normal gunfight) |
| Reserve | **Infinite** (HUD shows `100 / ∞`) |
| Reload time | **1.0 s**, uninterruptible by fire, cancelled by sprint/slide (restores prior count — no partial fill) |
| Reload availability | Manual any time; **auto-triggers on empty** with the buffered fire input resuming after |
| Reload fiction | "Pod flip" — later a first-person pod-into-hopper animation; v1 is a HUD bar + 1.0 s lockout |
| Reload while ADS | Allowed, kicks you out of ADS for the 1.0 s, auto re-ADS if still held |

Why it's CoD-right: CoD feel is *never being punished for shooting*, not literally infinite bullets — the mag/reload beat is core CoD grammar. 100 balls means the reload is a between-fights ritual (like CoD reload-after-kill habit), never a mid-fight death sentence, and the 1.0 s cost is low enough to be texture, not friction.

---

## 4. Feedback systems

Every item ships in v1 as C++-only graybox; "later" column is the polish target. All widgets are built in `UPFCombatFeedbackWidget` (`NativeConstruct` + `WidgetTree`, per project rules). Audio in v1: **procedural-only hooks** — a `UPFCombatAudio` component with named stub functions (`PlayHitmarker()`, `PlayElim()`, `PlayMuzzle()`, `PlaySplatIncoming()`) that v1 implements with pitch-shifted engine default sound or nothing; the call sites are the deliverable.

| System | v1 graybox implementation | Later polish |
|---|---|---|
| **Hitmarker** | On owning-client hit confirm (§5.3): 4 white 45°-rotated `UImage` ticks (plain white brush, drawn via WidgetTree) around crosshair center, scale 1.2→1.0 and opacity 1→0 over **0.15 s**. Elim-hit variant tints team-paint color and scales 1.4. `PlayHitmarker()` stub | Crisp CoD-style tick SFX (the dopamine layer), animated splat-shaped marker |
| **Elim confirm** | Center-screen `UTextBlock` "SPLATTED [name]" 0.9 s + hitmarker elim variant + `PlayElim()` stub. Small score toast row top-right (text only) | Kill-confirm sting, splat burst frame, medal icons |
| **Damage direction indicator** | On receiving a hit event: a 24-uu-thick arc segment (a rotated `UImage` on a circle of radius 140 px around crosshair) pointing at attacker's yaw relative to camera, opacity 0.8→0 over **0.75 s**. Angle from `FPaintHitInfo.ImpactPoint`→shooter position passed in the client RPC | Textured chevron, stacking multi-hit arcs |
| **Mask splat (hit-received fiction)** | THE paintball signature. On each hit taken: 2–3 irregular blobs = overlapping `UImage`s with circular white brushes tinted shooter's team color at 0.85 opacity, random positions biased to the screen edge nearest `ImpactNormal` direction, random scale 60–140 px. Fades to 0.35 opacity over 1 s, **wiped fully on elimination/respawn or over 6 s** ("lens wipe" fiction). Cap 6 concurrent blobs (perf + readability floor — never blind a player who can still fight) | Dripping paint material with runny animation, mask-edge vignette, wipe-hand animation |
| **Tracer visibility** | The projectile IS the tracer: emissive team-color 14 cm sphere (§2.2). No extra system. Verify readability against gray arena at 105 FOV — if weak, add a 30 cm ribbon via a second stretched engine cylinder | Niagara ribbon trail, air-shimmer, ball rotation |
| **Muzzle report** | `UPFFireShake` camera shake: 0.06 s, 0.3° pitch up, 0.15° random yaw — per shot, additive-safe at 12 bps (this doubles as our recoil: **there is no aim-displacing recoil pattern in v1**, bloom is the spray cost; the shake is feel-only). `PlayMuzzle()` stub. Muzzle flash: none in v1 (paintball has none — fiction wins, saves work) | Layered marker "pop-pop" audio with distance variants; CO₂ wisp |
| **Victim-side splat on world** | Server multicast spawns `APFSplatDecal`: a flattened engine cylinder (scale 0.4, 0.4, 0.01) MID-tinted team color, oriented to `ImpactNormal`, on walls/floor/pawns. Pool of **200**, oldest recycled. Splats persist for the match — the arena visibly wears the fight, which players will reference in the vote phase | Deferred decals with drip masks, splat-on-mask POV for victims |

Crosshair (owned here, not UI doc): static 4-line + center dot, plain white images; gap in px = `40 × tan(currentSpreadHalfAngle) / tan(FOV/2) × (ViewportWidth/2)` — i.e., the crosshair truthfully displays the live spread cone including bloom. Hidden while ADS (ADS shows a 2 px center dot only, standing in for future sight geometry).

---

## 5. Netcode feel (v1 pattern)

**Decision: client-cosmetic-immediate + server-authoritative projectile ("fake-and-verify"). Not full server-only.** 60 ms+ of dead trigger is the one thing that would falsify "feels like CoD"; full client authority is a cheating and reconciliation swamp. Fake-and-verify is the industry-standard middle and is very buildable in UE.

### 5.1 Fire pipeline

1. **Client, frame of input:** consume ROF token → spawn **cosmetic projectile** (no gameplay collision; traces for *visual* impact only), muzzle shake, predicted hopper decrement, crosshair bloom. Zero perceived latency.
2. **Same frame:** `ServerFire(FShotPacket)` — reliable RPC. `FShotPacket { FVector_NetQuantize100 Origin; FVector_NetQuantizeNormal Dir; uint32 ShotIndex; float ClientTime; }`.
3. **Server:** validate (token bucket §2.1; Origin within 150 uu of server-side muzzle; Dir within 4° of server view — reject silently and log if out). Spawn the **authoritative projectile** (server-only actor, `SetReplicates(false)`). Apply spread using seed = `HashCombine(PlayerId, ShotIndex)` — client used the same seed, so trajectories match to float precision.
4. **Remote clients:** see the shot via a lightweight **unreliable NetMulticast** `MulticastShotFX(Origin, Dir, ShotIndex)` from the weapon (skip owning client) — they spawn their own cosmetic projectile. No projectile actor replication ever (12 bps × 16 players would melt the channel).

### 5.2 Reconciliation

- **World splat:** owning client's cosmetic projectile leaves a *pending* (60% opacity) splat at its predicted impact. Server impact multicasts `SpawnConfirmedSplat(Location, Normal, Team)`; the owning client deletes its pending splat if one exists within **75 uu** and spawns the confirmed one. Divergence >75 uu (moved player, late join of geometry): pending fades out over 0.2 s — reads as a ball that "broke weird," fiction absorbs the lie.
- **Pawn hit / hitmarker:** ONLY on server confirm. Server calls owning-client RPC `ClientHitConfirm(ShotIndex, bElimHit)` → hitmarker fires. Cost: hitmarker arrives one RTT late (~30–60 ms LAN/regional) — this is exactly CoD's model and imperceptible; the instant part (your ball visibly flying and splatting) is already client-side.
- **Elimination:** fully server-authoritative via the hit-event count (game design doc). Replicated via existing state (health/elim repnotify) — no prediction of eliminations, ever.
- **No lag-compensation rewind in v1.** The 7 uu ball radius + projectile travel (targets must be *led* anyway, so ping shifts feel less discretely wrong than hitscan) makes rewind deferrable. Listed as the #1 post-v1 netcode item; the `ClientTime` field in `FShotPacket` is already there for it.
- **Movement prediction:** slide + sprint state ride the `SavedMove`/`FCharacterNetworkMoveData` pipeline (custom compressed flags: Sprint, SlideActive). Standard, but stated: any movement state NOT in saved moves will rubber-band and is a bug, not a tuning problem.

### 5.3 Tick/replication budget

- Listen server target 60 Hz fixed. `NetUpdateFrequency` 60 on pawns, 10 on match-state actors. `MinNetUpdateFrequency` 30 on pawns.
- Cosmetic projectiles tick at frame rate but are pooled (pool 64/client) — 12 bps burns actors fast.

---

## 6. Spawn/respawn feel (both variants, thin — game design doc picks)

Shared: players spawn at team spawn zones (geometry reserved by the build system — build doc owns placement rules; combat requires: zone ≥ 600×600 uu, no player-built geometry inside, mutual team-zone distance ≥ 6000 uu). Spawn = camera fade-in from team color 0.3 s, weapon raised, full hopper, all mask splats wiped.

### Variant A — Round-based elimination (speedball fiction)
- No respawn. On elimination: 0.5 s locked death cam at own body (sell the splat), then instant first-person **spectate of nearest living teammate**; Fire = next teammate, ADS = previous. No free-cam (spectators calling positions is real-paintball cheating fiction *and* an info exploit).
- Round start: 3-2-1 lock — players stand in spawn zone, movement locked, camera free, then a horn stub (`PlayBreakout()`). The "breakout" sprint from the box is the round's signature moment — spawn zones should face the arena.
- Between rounds: 5 s intermission, score toast, positions reset, world splats **persist across rounds** (arena wear).

### Variant B — Respawn (deathmatch-style)
- Fixed **5.0 s** respawn timer (no waves in v1 — waves need pacing data we don't have). Death cam 0.5 s, then countdown over teammate spectate.
- Spawn point selection: among own team-zone points, score = distance to nearest enemy (weight 1.0) − distance to nearest teammate (weight 0.3); pick max. Never spawn in enemy line-of-sight if any non-LOS point exists (single sphere trace check per candidate).
- Spawn protection: **1.5 s** — cannot receive hit events; broken early by firing or ADS'ing. Visual: own weapon MID pulses team color.

Both variants use the identical elimination → camera → spawn pipeline (`APFPlayerController::HandleElimination()`); the variant is a `EPFRespawnMode` on the game mode. Build both switches now — it's ~30 lines of difference.

---

## 7. v1 scope cut list (explicitly OUT)

1. **Recoil patterns / aim displacement** — bloom + fire shake only. Patterns need tuning time v1 doesn't have.
2. **Second weapon, weapon switching, pistol/melee** — one marker. The gunfeel bar is easier to hit with one gun tuned hard.
3. **Lag-compensation rewind** — deferred (§5.2); `ClientTime` plumbing ships now.
4. **Paintball bounces / no-break rolls** — all impacts break (§2.2).
5. **Hit zones (head/legs)** — single capsule; enum reserved.
6. **Tactical sprint / mantle / ledge grab** — slide-jump is the v1 movement ceiling. Mantle is the #1 post-v1 movement item (builder games generate ledges), noted for the arena doc's max-wall-height rules.
7. **Audio assets** — stub component with named hooks only (§4).
8. **Controller/gamepad tuning + aim assist** — KBM only in v1. Aim assist with projectiles is a project, not a task.
9. **FOV/sensitivity settings UI** — hardcoded 105 FOV; sensitivity as a config-file scalar (`PFSensitivity`, default 1.0 = 0.07°/mouse-unit via Enhanced Input scalar).
10. **Killcam / replay** — death cam is a static 0.5 s look-at.
11. **Ball trail ribbons, Niagara anything** — emissive sphere is the tracer until the PC/asset phase.

---

## Appendix — one-page number card (for the engineer's constructor)

```
// Movement
Walk 600 | Sprint 830 | Crouch 300 | ADS x0.55 | Accel 4096 | Friction 10
BrakeWalk 2500 | BrakeFactor 1.0 | JumpZ 630 | GravScale 1.5 | AirControl 0.9
SprintOut 0.18s | ADS-in 0.18s | ADS-out 0.14s
Slide: trigger ≥750, boost 1150, steer 15°/s, glide 0.35s@fr0.6 → ramp to 8.0 over 0.5s,
       end <320 or 1.1s, cooldown 0.5s, fire=hip only (+1.0°), slide-jump keeps velocity
FOV 105 | ADS 70 | SprintKick +6 | SlideKick +9 | Bob none | JumpBuffer 0.1s

// Marker
ROF 12 bps full-auto | Projectile 10000 uu/s | GravScale 0.35 | Life 2.0s
Ball collision r=7 | visual d=14cm emissive team color | no bounce, no pen
Spread: ADS 0.25 | hip 1.5 | hip-move 2.0 | air +1.5 | slide +1.0 | crouch x0.8
Bloom +0.12/shot cap +1.8, decay 6/s after 0.15s, ADS applies bloom x0.5
Hopper 100 | reserve ∞ | reload 1.0s auto-on-empty | FF: cosmetic only | self-splat: none

// Net
Fake-and-verify: client cosmetic instant + reliable ServerFire + server-only real projectile
Shared spread seed Hash(PlayerId, ShotIndex) | multicast remote FX | splat reconcile @75uu
Hitmarker on ClientHitConfirm only | no rewind v1 | 60Hz listen | pawn NetUpdate 60

// Spawn
A: no respawn, teammate spectate, 3-2-1 breakout, 5s intermission, splats persist
B: 5.0s fixed, farthest-from-enemy scoring, 1.5s protection broken by fire/ADS
```
