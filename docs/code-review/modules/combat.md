# Combat module review — Pass 2

| Field | Value |
|-------|-------|
| **Date** | 2026-07-23 |
| **Pass** | 2 |
| **Status** | done |
| **Scope** | `/home/josh/projects/combatforge/Source/CombatForge/Combat/` (current code only) |
| **Files** | 26 (13 `.h` + 13 `.cpp`) |
| **Contract skim** | `docs/design/05-code-contract.md` weapons/hit rules (B2–B3, B9–B12, T1, T10, T20–T21, §3.4, numbers table) |
| **Prior reports** | not read |

## Summary

Combat is a mature fake-and-verify pipeline: client cosmetic + `ServerFire` token/origin/dir/spin-up/re-burst gates, deterministic `MakeShotStream` + single main-stream `VRandCone`, owner-only ammo rep, and locational region counters (3/5/8/10) with corpse collision rules. Splats, smoke-for-bots, warm-up dummies, ammo barrels, and layered audio/VFX are coherent on the default listen-host / LAN path.

Product has deliberately drifted past the graybox contract (locational HP, finite reserve, multi-weapon catalog, grenades, breach bombs, decal scuffs). That drift is mostly intentional; the remaining **real** issues cluster around **burst/grenade/bomb projectiles reusing the live-fire hitmarker + HitValue path**, **bomb detonation cost (1000 authoritative actors)**, and **reload state surviving weapon swap**.

Default Lobby → Combat with a normal AR is sound. Grenade / bomb / dual-wield edge paths are where majors live.

## Findings (IDs P2-CB*)

### P2-CB1. Bomb BB spray inherits planter weapon `HitValue` (sniper = multi-lethal pellets)
- **Severity:** major
- **Status:** open
- **File:** `Source/CombatForge/Combat/PFBombActor.cpp:539-540` (`InitProjectile` with `SrcWeapon`); `Source/CombatForge/Combat/PFPaintballProjectile.cpp:282-285` (`PaintHit.Damage = Weapon->HitValue`); catalog e.g. `PFWeaponCatalog.cpp:440` (`snp_01` HV5), `:470` (`snp_04` HV8); thresholds `PFHealthComponent.h:39-42` (`ChestOut=5`, `HeadOut=3`)
- **Symptom:** A planter with a high-`HitValue` primary (sniper 5–8) arms a charge; on boom each authoritative BB credits that full HitValue. One chest pellet from HV5 already meets `ChestOut`; HV8 one-shots almost any region. Combined with proximity paint + hundreds of BBs, the charge is an arena wipe rather than a breach tool.
- **Why:** Bomb reuses the live-fire paintball resolve path and the planter’s **currently equipped** `UPFWeaponComponent` as `SourceWeapon`. Live-fire HitValue is a per-gun rank lever; breach BBs were not given a fixed damage of 1.
- **Fix:** Force `Damage = 1` (or a dedicated `BombHitValue`) for bomb/grenade-spawned balls — either pass a null weapon and set damage in a bomb-only resolve branch, or add `InitProjectile(..., uint8 OverrideDamage)` / a flag that ignores `Weapon->HitValue`. Keep `SourceWeapon` only for team/splat/multicast routing if needed.
- **Confidence:** high
- **Source:** this-pass

### P2-CB2. Frag grenade BBs also inherit thrower `HitValue`
- **Severity:** major
- **Status:** open
- **File:** `Source/CombatForge/Combat/PFGrenadeProjectile.cpp:272-294` (`SpawnFragBurst` → `InitProjectile(..., SrcWeapon, ...)`); same damage resolve as P2-CB1
- **Symptom:** Frag nade + sniper/HV primary turns ~90 BBs into multi-point hits each. Teammates stay immune (B12), but any enemy in LOS of the cloud is deleted instantly even with partial cover gaps.
- **Why:** Same design hole as the bomb: frag is “a spray of real paintballs,” and those paintballs always copy the marker’s HitValue.
- **Fix:** Same as P2-CB1 — fixed damage 1 for utility bursts; HitValue only on the player’s direct `ServerFire` projectiles.
- **Confidence:** high
- **Source:** this-pass

### P2-CB3. Every projectile hit fires a reliable `ClientHitConfirm` (frag/bomb/shotgun spam)
- **Severity:** major
- **Status:** open
- **File:** `Source/CombatForge/Combat/PFPaintballProjectile.cpp:291-294`; consumers `PFWeaponComponent.cpp:790-793`, UI `PFCombatFeedbackWidget.cpp:224-263` (no coalesce); bomb spray `PFBombActor.cpp:56-58` (`FragBBCount=1000`), frag `PFGrenadeProjectile.cpp:92` (`FragBBCount=90`); shotgun pellets `PFWeaponComponent.cpp:1095-1099` (up to 8 pellets × same path)
- **Symptom:** One frag detonation can enqueue dozens of reliable Client RPCs + hitmarker audio/UI resets; bomb is worse when the planter’s weapon is still valid. Shotguns click 6–8 hitmarkers per shell. Reliable channel + voice pool pressure under burst combat.
- **Why:** Authoritative impact always calls `Weapon->ClientHitConfirm` once per BB with no “already confirmed this ShotIndex / this burst” gate. Utility bursts deliberately reuse high ShotIndex spaces but never coalesce feedback.
- **Fix:** Coalesce per `ShotIndex` (or per burst seed) on the weapon: first enemy hit → hitmarker; first elim → elim confirm; subsequent BBs in the same burst silent (or throttle to 1 confirm / 50 ms). For bomb/frag, prefer a single `ClientHitConfirm` from the detonation actor after damage resolve, not per pellet.
- **Confidence:** high
- **Source:** this-pass

### P2-CB4. Bomb detonation spawns up to 1000 authoritative projectile actors
- **Severity:** major
- **Status:** open
- **File:** `Source/CombatForge/Combat/PFBombActor.h:56-60` (`FragBBCount=1000`, `BurstBatchSize=80`); `.cpp:504-549` (`SpawnFragBurstBatch`)
- **Symptom:** Detonation hitches the listen host (~13 frames × 80 `SpawnActor` + projectile movement). Overlaps with proximity paint (`ApplyProximityPaint` at `:552-614`) so the spray is both expensive and overkill for the “breach + nearby paint” fantasy.
- **Why:** Batching spreads cost but does not reduce total actors or physics sweeps. Cosmetic path correctly caps at 64 (`CosmeticTracerCount`); authority path does not.
- **Fix:** Drop authority count to a playable band (e.g. 40–120) plus the existing proximity paint, or replace the dual-sphere spray with a small set of cone/line traces. Keep cosmetic tracers capped.
- **Confidence:** high
- **Source:** this-pass

### P2-CB5. Weapon swap leaves reload mid-flight (FinishReload fills the new gun)
- **Severity:** major
- **Status:** open
- **File:** swap call site `Source/CombatForge/Player/CombatForgeCharacter.cpp:3182-3204` (`ServerSwapWeapon` stashes ammo / `ApplyWeaponLoadout` / restore stash — **never** clears reload); reload state machine `Source/CombatForge/Combat/PFWeaponComponent.cpp:823-921` (`bReloading`, `FinishReload`); `ServerResetLoadout` at `:1157-1175` **does** clear `bReloading` (respawn only)
- **Symptom:** Start reload on primary → scroll to secondary before finish → when the timer elapses, `FinishReload` pulls reserve into the **secondary** mag. Player also stays fire-gated (`PassesCommonFireGates` / server `bReloading`) for the rest of the reload on a gun they already swapped to.
- **Why:** Reload is owned by the single shared `UPFWeaponComponent`; swap changes hopper numbers and catalog stats but not the reload timer/flag. Only full loadout reset clears it.
- **Fix:** On swap (and any mid-life `ApplyWeaponLoadout` that changes weapon id), authority + predicting owner call `CancelReload()` (or a public `AbortReload()`). Do not finish a reload that started under a different WeaponId.
- **Confidence:** high
- **Source:** this-pass

### P2-CB6. Fire mode is client-local; server does not enforce Single/Burst caps
- **Severity:** minor
- **Status:** open
- **File:** `Source/CombatForge/Combat/PFWeaponComponent.h:42-47`, `:226-227` (`CurrentFireMode` not replicated); server path `.cpp:508-703` (token bucket + spin-up + re-burst only when `!bAutoAllowed && ReburstDelaySec`); client pull cap `.cpp:243-251`
- **Symptom:** A modified client on a Mode_S weapon can stream `ServerFire` at the weapon’s `FireRateBps` / token rate (full-auto sniper/shotgun cadence). Honest clients are latched by `ShotsThisPull`.
- **Why:** Fire selector is intentionally “feel only”; server mirrors only spin-up and DMR re-burst, not per-pull Single/Burst counts.
- **Fix:** Replicate fire mode (or derive allowed cadence from catalog + packet metadata) and enforce Single = one accepted shot per press edge / Burst = `BurstCount` then gap. Minimum: reject runs longer than BurstCount without a ≥X ms gap for non-Auto weapons.
- **Confidence:** high
- **Source:** this-pass

### P2-CB7. Grenade inventory is not client-predicted
- **Severity:** minor
- **Status:** open
- **File:** `Source/CombatForge/Combat/PFWeaponComponent.cpp:1224-1256` (`StartThrow` gates locally but does not decrement); `:1259-1315` (server `--Count` + OnRep)
- **Symptom:** On a remote client, HUD frag/smoke count ticks down only after COND_OwnerOnly rep; double-tap G before the round-trip can send a second RPC that the server correctly rejects (or accepts the last remaining) — feel is laggy vs ammo prediction.
- **Why:** Ammo hopper is predicted; grenade counts only change on authority.
- **Fix:** Predict `--FragCount`/`--SmokeCount` + broadcast `OnGrenadeCountChangedEvent` in `StartThrow`, let OnRep correct; or disable throw input until the next OnRep after a throw.
- **Confidence:** high
- **Source:** this-pass

### P2-CB8. Cosmetic projectile pool (64) smaller than frag burst (90)
- **Severity:** minor
- **Status:** open
- **File:** `Source/CombatForge/Combat/PFSplatSubsystem.h:54` (`CosmeticProjectilePoolSize = 64`); `PFGrenadeProjectile.cpp:92`, `:311-328` (loops `FragBBCount`); pool recycle `PFSplatSubsystem.cpp:219-222`
- **Symptom:** Remote clients recycle oldest in-flight cosmetics mid-burst — some tracers vanish early; host still sees full authority spray. Cosmetic-only.
- **Why:** Pool sized for live-fire (04 §5.3); frag burst later grew to 90 without raising the pool or sampling.
- **Fix:** Sample ≤64 directions for cosmetics (bomb already does `CosmeticTracerCount=64`), or raise pool if memory allows.
- **Confidence:** high
- **Source:** this-pass

### P2-CB9. Ammo barrel authority path does not re-check elimination
- **Severity:** minor
- **Status:** open
- **File:** `Source/CombatForge/Combat/PFAmmoBarrel.cpp:251-288` (`AuthorityInteract`); contrast bomb pickup claim gated in Character (`ServerClaimBombPickup` elim check). Character `ServerRefillAtBarrel` (`CombatForgeCharacter.cpp:1769-1774`) also has no elim guard.
- **Symptom:** A still-possessed eliminated pawn that can still fire interact RPCs could top up mag/grenades before round cleanup (usually irrelevant; edge during death cam / lag).
- **Why:** Barrel validates phase/range/full-mag only.
- **Fix:** `if (Health && Health->bEliminated) return;` in `AuthorityInteract` (and/or `ServerRefillAtBarrel`).
- **Confidence:** med
- **Source:** this-pass

### P2-CB10. Target dummy stays an invisible paintball blocker for 0.5 s after “elim”
- **Severity:** minor
- **Status:** open
- **File:** `Source/CombatForge/Combat/PFTargetDummy.cpp:84-98` (hide immediately); corpse rule `PFHealthComponent.cpp:179-185`, `:23` (`CorpseBlockSeconds = 0.5f`); respawn at 1.0 s `:100-108`
- **Symptom:** Warm-up shots can hit empty air for half a second after the dummy vanishes (still collides on `PF_ECC_Paintball`), then pass through until respawn.
- **Why:** Player corpse rule (04 §2.4) is shared; dummies use the same health component without a “no corpse block” flag.
- **Fix:** On dummy elim, immediately `Ignore` paintball (and optionally pawn) collision, or call a `bSkipCorpseBlock` path in `ApplyPaintHit` for non-character owners.
- **Confidence:** high
- **Source:** this-pass

### P2-CB11. Server fire anti-cheat tolerances wider than contract numbers
- **Severity:** nit
- **Status:** open
- **File:** `Source/CombatForge/Combat/PFWeaponComponent.cpp:28-31` (`ServerOriginToleranceUU = 200`, `ServerDirToleranceDeg = 9`); contract § numbers / 04 §5.1 historically 150 uu / ~4°
- **Symptom:** None for honest play (FP/TP muzzle + convergence need headroom). Contract readers will mis-expect tighter gates.
- **Why:** Intentional playtest relaxation (commented); law doc not errata’d.
- **Fix:** Contract errata only, or keep code and document “as-shipped” tolerances in 05.
- **Confidence:** high
- **Source:** this-pass

### P2-CB12. Graybox contract drift (intentional product, document only)
- **Severity:** nit
- **Status:** open (documentation / intentional product)
- **File:** multiple — e.g. `PFHealthComponent.h:14-19` (locational 3/5/8/10 vs B2 3-HP body/mask); `PFWeaponComponent.h:113-117` (30+120 finite reserve vs B9 100/∞); `PFSplatSubsystem.h:23-31` (decals vs B10 spheres); grenades/bombs/catalog absent from §3.4 class list
- **Symptom:** Implementers reading only `05-code-contract.md` will build the wrong HP/ammo/splat model.
- **Why:** Post-graybox airsoft product evolution; headers state supersession in places, contract not fully refreshed.
- **Fix:** Errata block in 05 (or “as-shipped combat” appendix): locational counters, mag+reserve, weapon catalog, grenades, breach bomb, decal impacts.
- **Confidence:** high
- **Source:** this-pass

### P2-CB13. `NearestRemaining` / HUD “HP” is a min-of-thresholds proxy, not classic HP
- **Severity:** nit
- **Status:** open
- **File:** `Source/CombatForge/Combat/PFHealthComponent.cpp:334-348`; feedback still pipes `NewHP` via `ClientPaintHitTaken`
- **Symptom:** Widgets that still think in “3 HP dots” can show jumps or non-intuitive remaining when HV>1 or region counters are uneven.
- **Why:** API kept `OnHPChanged` / `NewHP` for feedback compatibility after the locational rewrite.
- **Fix:** Prefer binding HUD to `OnHitsChangedEvent` (region bars) and treat `NearestRemaining` as “hits until nearest threshold” only.
- **Confidence:** high
- **Source:** this-pass

---

## Subsystems audited clean

| Subsystem | Verdict |
|-----------|---------|
| **Fake-and-verify fire (AR default)** | Clean. Client cosmetic + `ServerFire` monotonic index, ClientTime gap clamp, token bucket, phase gate (T21), origin/dir gates, shared bloom stamp, single main-stream `VRandCone` (B3). Listen host correctly skips double cosmetic/bloom. |
| **Crosshair convergence** | Clean. `PFConvergedShotDir` + min 120 uu clamp; server validates against same expectation. |
| **Friendly fire (B12)** | Clean on live-fire and utility BBs (`VictimTeam == TeamId && !bHitSelf` → splat only). Self-risk for bomb/grenade is intentional. |
| **Splat subsystem** | Clean. 256 decals, pending 0.6 s / 0.2 s fade, 75 uu reconcile, BuildPhase `ResetPool` (T20), dedi no-op, cosmetic pool ownership. |
| **Smoke (AI only)** | Clean. `UPFSmokeSubsystem` sphere registry; meshes NoCollision; paintballs never query smoke. |
| **Combat VFX** | Clean. Soft Niagara + mesh fallback, neutral flash, `EndPlay` destroys ownerless holder (prior leak addressed). |
| **Combat audio** | Clean enough. Procedural fallback, concurrency, `StopCompAfter` for proc-wave voice leaks; layered muzzle. |
| **Target dummy (T29)** | Clean aside from P2-CB10. Self-reset, no GameMode score route, `DefaultRoundHP=1` one-hit mode. |
| **Ammo barrel interact routing** | Clean for join clients (pawn-owned Server RPC → `AuthorityInteract`). Permanent station, unwalkable top. |
| **Bomb plant/defuse authority** | Clean at GameMode/bomb layer: combat-live only, charge consume on success, fuse pause while defusing, `bArmed` replicated, dual-side cosmetic seed. Issues are detonation payload (P2-CB1/3/4), not plant/defuse. |
| **Weapon catalog data** | Clean as data. Apply path (Character) correctly authority-gates hopper count writes; bot roll caps HitValue. |

---

## File checklist

| File | Reviewed | Notes |
|------|----------|-------|
| `PFWeaponComponent.h` | yes | Full API: fire modes, grenades, bloom/climb, HitValue/Pellets/spin-up |
| `PFWeaponComponent.cpp` | yes | Fire, reload, bloom, pellets, throw, validation — P2-CB5/6/7/11 |
| `PFPaintballProjectile.h` | yes | Dual mode, `bIgnoreShooter` |
| `PFPaintballProjectile.cpp` | yes | Auth resolve, FF, pending splat — P2-CB1–3 damage/confirm path |
| `PFHealthComponent.h` | yes | Locational model supersedes B2 |
| `PFHealthComponent.cpp` | yes | Region resolve, corpse, fall death, one-hit mode |
| `PFSplatSubsystem.h` | yes | Decals + cosmetic pool |
| `PFSplatSubsystem.cpp` | yes | Reconcile, puff budget, T20 bind |
| `PFGrenadeProjectile.h` | yes | Frag/smoke, replicated detonate |
| `PFGrenadeProjectile.cpp` | yes | P2-CB2/3/8 |
| `PFBombActor.h` | yes | 15 s fuse / 8 s defuse / 1000 BB |
| `PFBombActor.cpp` | yes | P2-CB1/3/4; plant/defuse logic sound |
| `PFBombPickup.h` | yes | Mid-field charge |
| `PFBombPickup.cpp` | yes | Phase + range + carrying gate |
| `PFAmmoBarrel.h` | yes | Permanent resupply |
| `PFAmmoBarrel.cpp` | yes | P2-CB9 |
| `PFSmokeSubsystem.h` | yes | AI LOS only |
| `PFSmokeSubsystem.cpp` | yes | Clean |
| `PFTargetDummy.h` | yes | T29 |
| `PFTargetDummy.cpp` | yes | P2-CB10 |
| `PFCombatAudio.h` | yes | Hook surface |
| `PFCombatAudio.cpp` | yes | Layered + procedural fallback (partial deep-read of play paths) |
| `PFCombatVFX.h` | yes | Muzzle only |
| `PFCombatVFX.cpp` | yes | Clean |
| `PFWeaponCatalog.h` | yes | Def / auto-pose API |
| `PFWeaponCatalog.cpp` | yes | Data + helpers; HV snipers feed P2-CB1/2 |

---

## Counts

| Severity | Open |
|----------|------|
| blocker | 0 |
| major | 5 (P2-CB1 … P2-CB5) |
| minor | 5 (P2-CB6 … P2-CB10) |
| nit | 3 (P2-CB11 … P2-CB13) |

**Suggested fix order:** P2-CB1/2 (fixed utility damage) → P2-CB3 (confirm coalesce) → P2-CB4 (bomb BB count) → P2-CB5 (cancel reload on swap).
