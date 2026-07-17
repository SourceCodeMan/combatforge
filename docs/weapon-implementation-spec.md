# Weapon Overhaul — Implementation Spec (for Grok)

*Companion to `docs/weapon-balance-plan.md` (the WHY + full research). This is the HOW: ordered
stages, exact seams, paste-ready numbers. Self-contained — you don't need the research context.
Every file:line was verified against commit `479692e`; expect small drift.*

**Scope: game-side C++ only.** Backend work (D1 unlock rows, API changes) is Claude's — the
contract you code against is in §7. Build + verify after every stage; stages are independently
shippable in order. Standing rule: build BOTH targets (`CombatForge` + `CombatForgeEditor`).

---

## ⚠️ Four engine contracts you must not break

1. **B3 — exactly ONE `VRandCone` pull per shot per stream** (comment at
   `PFWeaponComponent.cpp:326-327` and `:548-549`). Client `FireOneShot`, server `ServerFire`,
   and remote `MulticastShotFX` must all derive identical directions. Shotgun pellets therefore
   use deterministic SUB-seeds (`HashCombine(ShotSeed, PelletIdx)`), never extra pulls from the
   main stream — copy the frag-burst pattern (`PFGrenadeProjectile.cpp:264-322`, `Seed + i + 1`,
   replicated seed → cosmetic mirror).
2. **Bloom determinism** — bloom is chained on the shooter's `ClientTime` stamp so client + server
   compute bit-identical cones (`PFWeaponComponent.h:187-190`). Per-weapon bloom params are safe
   ONLY because both sides read them from the compiled catalog via the replicated kit. Never
   derive bloom from anything non-replicated.
3. **Movement prediction** — `GetMaxSpeed` (`PFCharacterMovementComponent.cpp:115-156`) runs in
   the prediction/replay path. A per-weapon `MoveSpeedMult` MUST derive only from replicated state
   (`KitRep` + `bSecondaryActive`), identically on server and autonomous proxy, or weapon swaps
   rubber-band.
4. **Server authority** — all new combat math (HitValue, pellets, spin-up) resolves SERVER-side.
   Never trust a client-sent damage/pellet field. `FPFShotPacket` does not change in this spec.

---

## Stage 1 — Catalog fields + per-weapon feel (no new systems)

**Goal:** every weapon gets identity stats; bloom/recoil/handling become per-weapon.

1. **Extend `FPFWeaponDef`** (`PFWeaponCatalog.h:16-40`) with:
   ```cpp
   const TCHAR* WeaponId = nullptr;   // stable slug, e.g. TEXT("ar_ak_black") — NEVER nullptr in the table
   uint8  HitValue        = 1;        // region-counter credit per BB (Stage 2 consumes)
   uint8  Pellets         = 1;        // >1 = volley (Stage 3 consumes)
   float  PelletSpreadDeg = 0.f;      // fixed pellet cone half-angle (independent of bloom)
   float  BloomPerShotDeg = 0.15f;    // hipfire-only bloom (existing global values as defaults)
   float  BloomCapDeg     = 2.0f;
   uint8  BloomFreeShots  = 5;
   float  BloomDecayDegPerSec = 6.f;  // NEW: continuous decay (see 1.4)
   float  ClimbPitchPerShotDeg = 0.30f;   // real recoil climb (existing global values)
   float  ClimbYawPerShotDeg   = 0.12f;
   float  ClimbRecoverDegPerSec = 14.f;
   float  ADSTimeSec      = 0.25f;    // per-class ADS-in (replaces the 0.14 global; out stays 0.14)
   float  SprintOutTime   = 0.18f;
   float  MoveSpeedMult   = 0.95f;    // CoD weapon weight (⚠️ contract 3)
   float  ReloadTime      = 1.0f;
   float  SpinupSec       = 0.f;      // Minigun only (Stage 4)
   float  ReburstDelaySec = 0.f;      // Rifle 03 burst-DMR cadence (0 = default burst behavior)
   ```
2. **Thread through the ONE copy block** — `ACombatForgeCharacter::ApplyWeaponLoadout`
   (`CombatForgeCharacter.cpp:2117-2128`) copies def → `UPFWeaponComponent`. Add the new fields
   there. Convert the component's matching global `UPROPERTY`s (`PFWeaponComponent.h:130-158`)
   into per-weapon values set by this block (keep the UPROPERTYs as the receiving members).
3. **ADS time:** `ADSInTime` lives at `CombatForgeCharacter.h:462` (actual default 0.14 — the
   `0.18` in comments at h:59 / cpp:1620 is STALE). Set it from the def in ApplyWeaponLoadout.
   `UpdateADSAlpha` runs on every role already (server feeds the spread cone) so it's
   replication-safe as long as both sides read the kit-derived value.
4. **Continuous bloom decay:** in `GetBloomDegForStamp`/`AdvanceBloom`
   (`PFWeaponComponent.cpp:862-883`), replace the hard `BloomResetGap` reset with
   `bloom -= BloomDecayDegPerSec * gap` (clamped ≥ 0), keeping the full reset when
   `gap > BloomResetGap` as the floor. Decay must use the STAMP gap (deterministic), not
   wall-clock. Start decay 0.15s after the last shot.
5. **MoveSpeedMult:** multiply inside `GetMaxSpeed` (`PFCharacterMovementComponent.cpp:115-156`),
   sourced from the equipped def resolved via the character's replicated kit +
   `bSecondaryActive` (⚠️ contract 3). Suggested: a `CachedMoveSpeedMult` on the CMC updated in
   `ApplyWeaponLoadout` (which runs from OnRep on clients — same value everywhere).
6. **Fill the whole 37-row catalog** (`PFWeaponCatalog.cpp`) from the table in §6. Give the six
   Modern Rifles their distinct rows (today they're all default-stat clones). Skins
   (`ar_m4_olive`, `ar_ak_wood`, `pis_02`, `smg_aksu_wood`) copy their base row's stats verbatim
   but keep their own `WeaponId`.
7. **`PFWeapon::FindById(FStringView) -> FPFWeaponConfig`** + `IdOf(Category, Index)`; migrate
   `SaveConfig/LoadConfig` (`PFWeaponCatalog.cpp:325-407`) to store slugs with int fallback for
   one version (indices already shifted once when the modern pack merged — never trust them).
8. **Crosshair halo ring:** in `UpdateCrosshair` (`PFCombatHUDWidget.cpp:805-898`), when current
   bloom ≥ 75% of `BloomCapDeg`, draw a faint ring at the projected spread radius (existing
   `tan(spread)` projection at :874-881) — the "ease off the trigger" tell. Crosshair stays
   hidden while ADS (existing behavior).

**Verify:** AK feels heavy (slow ADS, hard climb, 154ms head double-tap once Stage 2 lands);
SMG barely blooms while moving; LMG crosshair huge while walking; sniper hipfire absurd.
`pf.WeaponFP`-style testing loop unchanged.

## Stage 2 — HitValue (per-weapon power)

1. `ResolveAuthoritativeImpact` (`PFPaintballProjectile.cpp:267-279`) already holds
   `SourceWeaponWeak` — set `PaintHit.Damage = Weapon->HitValue` (server-side resolve; zero
   packet change; cheat-proof). Field `FPFPaintHitInfo.Damage` exists (`CombatForgeTypes.h:175`)
   and is currently overwritten by the next step:
2. `UPFHealthComponent::ApplyPaintHit` (`PFHealthComponent.cpp:106-182`): **delete the
   `Hit.Damage = 1` hardcode at :123**; add `Hit.Damage` (clamped 1..10) to the region counter
   AND `TotalHits` at :125-131 instead of `++`.
3. Thresholds stay untouched (`HeadOut 3 / ChestOut 5 / LimbOut 8 / TotalOut 10`, h:39-42).
   HV5 (snipers) → head 1 / chest 1 / limbs 2. HV8 (snp_04) → out on any tag. No special
   region-override flags needed — the tiers fall out of the thresholds.
4. HUD: the region pips (`PFCombatHUDWidget::HandleHitsChanged`) already read the counters —
   verify multi-point hits render sanely (a sniper limb tag should visibly fill most of a bar).
5. Hitmarker: `ClientHitConfirm` is keyed by ShotIndex (`PFWeaponComponent.cpp:667-671`) —
   unchanged. Optional polish: distinct headshot tick.

**Verify (PIE, 2-player or bots):** AK chest = 3 tags, AK head = 2; sniper chest = instant;
sniper limb ×2 = out; mixed-region math still trips `TotalOut 10` with fractional-free integers.

## Stage 3 — Shotgun pellet volleys

Pattern to copy: frag burst (`PFGrenadeProjectile.cpp:264-322`) — N authoritative BBs with
per-BB seed offset, replicated seed drives the remote cosmetic mirror.

1. **One pull = one packet = one ammo = one fire token.** In `FireOneShot`
   (`PFWeaponComponent.cpp:301-421`): after the single B3 cone pull produces `BaseDir`, if
   `Pellets > 1`, spawn `Pellets` cosmetic projectiles, each direction =
   `BaseDir` rotated inside `PelletSpreadDeg` by a stream seeded `HashCombine(ShotSeed, i)`.
   Ammo decrement stays 1 (:368). ONE `FPFShotPacket` (:410-415), unchanged shape.
2. **`ServerFire`** (:441-588): identical pellet loop around the authoritative spawn (:560-571),
   same sub-seed derivation → bit-identical directions. Token bucket: unchanged (1 token per
   packet — refill is already per-weapon `max(1, FireRateBps)` at :481-490; do NOT charge per
   pellet).
3. **`MulticastShotFX`** (:610-634): same loop for remote cosmetics. Cosmetic pool is 64
   (`PFSplatSubsystem.h:54`) — 8 pellets fine.
4. Each pellet is an independent projectile → each landed pellet = 1 hit (HV1) through the
   normal `ApplyPaintHit` path; ≥5 chest pellets in one volley = out (instant inside envelope).
   Hitmarkers coalesce naturally via the shared ShotIndex.
5. Shotgun `ProjLifetimeSec` (~0.30s) is the hard range cliff — table values in §6.

**Verify:** point-blank chest = one-shot; ~15m = 2 volleys; ~25m = pellets vanish mid-air.
All three machines (host, client, remote viewer) show pellets flying the SAME directions.

## Stage 4 — Minigun spin-up + burst DMR cadence

1. **Spin-up:** in `TryFire` (`PFWeaponComponent.cpp:194-261`), when `SpinupSec > 0`: on trigger
   press start a spin timer; block firing until elapsed (server enforces the same via its own
   copy — deterministic from the def). Audio/FX hook optional. Releasing the trigger resets after
   a short grace (~0.4s) so feathering keeps it hot.
2. **Negative bloom:** the Minigun's `BloomPerShotDeg = -0.02` with `BloomCapDeg` used as the
   FLOOR (2.2°) — clamp direction flips when the per-shot value is negative. Cone starts at
   hip 3.0° and tightens while sustained.
3. **Rifle 03 re-burst:** `ReburstDelaySec = 0.38` — after a 3-shot burst completes, block the
   next burst for that delay (extends the existing burst latch `ShotsThisPull` logic at
   :212-218). In-burst rate uses `FireRateBps` (14) as today.

**Verify:** Minigun: 0.6s of nothing, then a hose that visibly tightens; TTK from cold ≈ 0.8s.
Rifle 03: perfect chest burst = instant out; whiffed burst ≈ 0.5s to recover.

## Stage 5 — Rank gating (client UX + server enforcement)

**Do this stage last; it's inert until Claude's backend rows exist. Code against §7's contract.**

1. **Parse unlocks:** `FPFBackendProfile` (`PFBackendSubsystem.h:34-45`) gains
   `TArray<FString> UnlockIds`; parse the `unlocks` string array in `FetchProfile`
   (`PFBackendSubsystem.cpp:379-430` — the API already returns it; the game currently drops it).
   Helper: `bool IsWeaponUnlocked(const FString& WeaponId)` — **returns true when logged out or
   offline** (LAN/bots/offline play is fully ungated, by decision).
2. **Picker badges:** `RefreshWeaponLabels` (`PFLoadingMenuWidget.cpp:694-710`) renders
   `"<Name> (i/N)"` — append `"  🔒 Rank N"` for locked entries (rank from the local
   `UnlockRank` field you add to the table rows for display; truth stays server-side).
   `NotifyWeaponStep` (:712-741): allow BROWSING locked entries but refuse to save/equip
   (revert to previous index with a status line). Same awareness for the death-screen class
   scroll (`PFCombatHUDWidget.cpp:1036-1054`) — skip class slots whose saved weapon is locked.
3. **Secondary picker:** there is NO menu UI for the secondary (console `pf.Weapon2` only —
   `CombatForgeCharacter.cpp:2394-2421`). Add a second stepper row to `BuildWeaponPicker`
   (:622-692) mirroring the primary, with the same lock handling. Default policy: any category
   allowed (current behavior) unless Tom rules pistols-only — leave a one-line clamp ready.
4. **Server enforcement:** `ServerSetKit_Implementation` (`CombatForgeCharacter.cpp:2258-2262`)
   currently validates NOTHING. Add: on fleet servers (`UPFBackendSubsystem::IsFleetActive()`),
   check the claimed weapon ids against the player's unlock set (populated per §7.3); on failure
   force the category's rank-1 starter (`ar_m4`/`smg_aksu_black`/`pis_std`/`sg_01`/`snp_01`/
   `lmg_01`). Listen/LAN servers: skip (advisory client-side gating only).
5. **Bots:** unaffected by gating. Optional (nice): give bots varied kits by assigning a
   server-side `FPFKitRep` at spawn (today every bot copies the host's saved config —
   `CombatForgeCharacter.cpp:2235-2242`); pick randomly from the full catalog.

## §6 — THE STAT TABLE (paste-ready values)

Columns: `WeaponId | HV | Pellets@Spread° | Mag | Bps | Modes | HipDeg/ADSDeg | Bloom/shot / cap / free / decay | Climb pitch/yaw | Muzzle×Life | ADSTimeSec | MoveMult | Rank`
Modes: S=Single B=Burst A=Auto. Unlisted fields keep Stage-1 defaults. Skins: copy base stats, own WeaponId + Rank.

**AR** (ReloadTime 1.0):
```
ar_m4        1  -      30 12.0 S/B/A 1.1/0.05 0.12/1.8/5/6  0.30/0.12 12000x2.2 0.25 0.95  1
ar_m4_olive  = ar_m4                                                                        4
ar_ak_black  2  -      30  6.5 S/A   1.4/0.06 0.22/2.2/4/6  0.45/0.15 11000x2.4 0.32 0.95  6
ar_ak_wood   = ar_ak_black                                                                 17
ar_r01       1  -      30 13.5 S/A   1.0/0.06 0.15/2.0/5/6  0.28/0.10 10500x1.8 0.20 1.00  9
ar_r02       1  -      30 10.5 S/B/A 1.3/0.03 0.10/1.5/5/6  0.26/0.10 13500x2.6 0.32 0.95 14
ar_r03       2  -      21 14.0 B(3, reburst 0.38) 1.6/0.04 0.08/1.5/3/6 0.35/0.10 13000x2.6 0.25 0.95 35
ar_r04       2  -      24  6.0 S/A   1.5/0.06 0.20/2.0/4/6  0.40/0.14 12000x2.4 0.32 0.90 27
ar_r05       1  -      30 11.0 S/B/A 1.1/0.05 0.08/1.2/6/6  0.18/0.06 12500x2.2 0.25 0.95 20
ar_r06       1  -      40 12.0 S/A   1.3/0.06 0.14/2.0/5/6  0.30/0.12 12000x2.2 0.32 0.90 45
```
**SMG** (ReloadTime 0.9, MoveSpread identity — see plan §2: MoveSpreadMult ~1.15):
```
smg_aksu_black 1 -     25 14.0 B/A   1.5/0.10 0.10/2.2/6/8  0.22/0.08  8500x1.4 0.20 1.00  1
smg_aksu_wood  = smg_aksu_black                                                            11
smg_01         1 -     30 15.0 B/A   1.4/0.09 0.10/2.2/6/8  0.22/0.08  9000x1.5 0.20 1.00  3
smg_02         1 -     25 16.0 A     1.6/0.12 0.12/2.4/6/8  0.24/0.10  8800x1.3 0.18 1.00  8   (MoveSpreadMult 1.05)
smg_03         1 -     30 13.0 S/B/A 1.2/0.07 0.08/1.6/6/8  0.18/0.06 10000x1.8 0.20 1.00 13
smg_04         2 -     20  5.5 S/A   1.7/0.10 0.25/2.4/4/8  0.40/0.14  9500x1.6 0.25 1.00 19
smg_05         1 -     50 15.0 A     1.8/0.12 0.12/2.6/8/8  0.26/0.10  9000x1.4 0.32 0.95 29
smg_06         1 -     22 17.0 B/A   1.5/0.10 0.14/2.4/6/8  0.26/0.10  8800x1.2 0.15 1.00 43
```
**Pistol/Revolver** (ReloadTime 0.8; pistols the fast-swap identity):
```
pis_std  1 -  18  8.0 S/B 1.3/0.15 0.30/2.6/2/10 0.45/0.15  9000x1.2 0.17 1.00  1
pis_01   1 -  15 10.0 S   1.2/0.12 0.28/2.6/2/10 0.45/0.15  9200x1.2 0.17 1.00  2
pis_02   = pis_01                                                              24
pis_03   1 -  17  9.0 S/B 1.4/0.14 0.30/2.6/2/10 0.45/0.15  9200x1.2 0.17 1.00 12
pis_04   1 -  15  9.0 S   0.9/0.08 0.15/2.0/3/10 0.30/0.10  9200x1.3 0.17 1.00 33
rev_01   2 -   6  4.0 S   1.6/0.10 0.30/2.6/1/10 0.80/0.20  9500x1.6 0.17 1.00 16
rev_02   2 -   6  5.0 S   1.9/0.14 0.30/2.6/1/10 0.80/0.20  9000x1.2 0.14 1.00 41
```
**Shotgun** (aim cone = HipDeg; pellets have their own fixed cone; ReloadTime 1.4 (sg_03: 2.2)):
```
sg_01  1 8@2.2  6 1.3 S   0.9/0.15 0.50/2.0/1/8 1.20/0.30 7000x0.30 0.28 0.95  5
sg_02  1 6@1.4  5 1.1 S   0.9/0.15 0.50/2.0/1/8 1.20/0.30 7500x0.35 0.28 0.95 15
sg_03  1 8@2.8  2 3.0 S   0.9/0.15 0.50/2.0/1/8 1.20/0.30 7000x0.30 0.22 1.00 22
sg_04  1 6@2.6 10 2.8 S/B 0.9/0.15 0.50/2.0/1/8 1.00/0.25 7200x0.30 0.28 0.90 37
```
**Sniper** (hipfire deliberately useless; flinch/glint = later polish pass, note in code):
```
snp_01  5 - 5 0.90 S 4.5/0.02 - 3.0/0.5 (one kick) 16000x3.0 0.45 0.90  7
snp_02  5 - 5 1.10 S 4.5/0.02 - 3.0/0.5           16000x2.8 0.30 0.90 18
snp_03  3 - 10 3.00 S 4.0/0.03 - 1.5/0.3          15000x2.8 0.35 0.90 31
snp_04  8 - 4 0.65 S 4.5/0.02 - 4.0/0.6           17000x3.2 0.55 0.85 47
```
**LMG** (ReloadTime 2.5):
```
lmg_01      1 -  75 10.0 A 2.2/0.15 0.05/1.2/10/4 0.35/0.12 11000x2.0 0.40 0.90 10
lmg_02      2 -  60  6.0 A 2.4/0.16 0.05/1.2/10/4 0.50/0.16 12000x2.2 0.45 0.88 25
lmg_03      1 - 100 12.0 A 2.6/0.18 0.06/1.4/12/4 0.35/0.12 11000x2.0 0.40 0.88 39
lmg_minigun 1 - 150 18.0 A 3.0/none -0.02 floor 2.2 -       9000x1.5 none 0.80 50  (SpinupSec 0.6)
```

## §7 — Backend contract (Claude's side; code against this)

1. **Profile JSON** (`GET /v1/profile/me`, already fetched by `UPFBackendSubsystem`):
   `"unlocks": ["wpn.ar_m4", "wpn.smg_aksu_black", ...]` — unlock_ids, prefix `wpn.` + WeaponId.
2. **Claude seeds 37 `unlock_definitions` rows** in D1 (INSERT-only) with
   `criteria {"type":"level","gte":N}` per the Rank column above; starters get `gte:1`.
3. **Fleet join-time unlocks:** a fleet server resolves a joining player's unlock set during the
   existing token-verified join (Claude adds the endpoint; until it exists, `ServerSetKit`
   validation short-circuits to allow — gate it behind
   `Backend->IsFleetActive() && Backend->HasUnlocksFor(PlayerId)`).

## Open decisions (Tom) — build with these defaults, easy to flip

| Question | Default in this spec |
|---|---|
| snp_04 one-tag-anywhere + Minigun (mild power aspirationals) | **As tabled** (paid via worst handling) |
| Secondary slot | **Any category** (current behavior); pistols-only = one clamp |
| Skins | Own ladder slots as tabled |
| Minigun loadout vs pickup | **Loadout @ L50** |
| Range-banded HitValue falloff | **Deferred** — v1 range identity = MuzzleSpeed×Lifetime only |

## Regression checklist (run after each stage)

- Both targets compile; PIE listen + 1 client: fire/reload/swap/melee/bomb unchanged for `ar_m4`.
- Server rejects >9° dir deviation + ROF cheats (existing validation intact).
- Bloom crosshair identical on host + client for the same shot sequence.
- Bot matches still function (bots use the same defs; no recoil climb — by design).
- Version gate: bump `PFBuild::NetProtocol` (CombatForge.h) with the first stat-changing package.
