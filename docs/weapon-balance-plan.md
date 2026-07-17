# CombatForge Weapon Balance + Rank-Unlock Plan — "Feels like CoD"

*2026-07-17 · research run `wf_3555eb44-5e8` (codebase audit + CoD mechanics research + design translation).
PLAN ONLY — nothing implemented. North star (Tom): gunplay feels like Call of Duty — snappy, fast
eliminations, strong class identities, minimal random spread, rank unlocks. The FICTION stays
non-lethal airsoft ("he's out"); the MECHANICS target BO6/MW2019.*

## 0. The five design pillars (all grounded in CoD's actual model)

1. **ADS = zero random spread + learnable recoil.** CoD's core accuracy rule: aiming collapses
   spread to ~0; ALL aimed difficulty is a deterministic per-weapon recoil pattern you learn to
   pull against (BO6 even ships a Recoil Wall to practice them). Our `SpreadADSDeg` → ~0 across
   the board; the existing recoil-climb system becomes per-weapon.
2. **The "halo effect" = hipfire-only bloom.** CoD never blooms ADS (the anti-Fortnite rule).
   Hipfire is a per-class cone that grows under sustained fire and decays on release — that's the
   halo, and **our engine already implements it** (globally): bloom per-shot/cap/free-shots +
   crosshair-gap projection are live code. This plan just makes the knobs per-weapon.
3. **Power = HitValue per BB** against the untouched head 3 / chest 5 / limbs 8 / mix 10
   thresholds. One new number (1/2/3/5/8) gives CoD-class TTKs with math a kid can verify:
   "this gun tags twice as hard." Snipers/shotguns get CoD's one-shot treatment (below).
4. **Handling is the price of power.** Per-class ADS-in, sprint-out, and move-speed (CoD's ms
   targets): SMG 0.20s/fast, AR 0.26s, pistol 0.17s, shotgun 0.28s, LMG 0.40s, sniper 0.45–0.55s,
   Minigun hip-only + 0.6s spin-up. Fast eliminations are always paid for in weight.
5. **Rank ladder, BO6/MW2019 style.** A genuinely competitive free starter per category (the
   Kilo-141 rule), one unlock nearly every level through 20, back-loaded spacing after,
   iconic aspirationals at 45–50. Near-clone meshes = blueprint-style skins, not ladder slots.

**TTK targets (chest, close):** AR 296–381 ms · SMG 235–364 ms · pistol 400–500 ms · LMG 333–400 ms
· sniper/shotgun instant-in-envelope. Deliberately one notch softer than MW2019's ~200–300 ms —
kids + adults share lobbies. Formula: `TTK = (ceil(ChestOut/HitValue) − 1) / FireRateBps`.

## 1. The power mechanic (HitValue + pellets + one-shots)

- **`HitValue` (uint8, default 1)** added to `FPFWeaponDef`; `UPFHealthComponent::ApplyPaintHit`
  adds it to the region counter + TotalHits instead of `++` (the seam is the hardcoded
  `Hit.Damage = 1` at PFHealthComponent.cpp:123 — `FPFPaintHitInfo.Damage` already exists and is
  simply overwritten today).
- **Tiers:** HV1 standard (head 3/chest 5/limb 8) · HV2 heavy (2/3/4) · HV3 marksman (1/2/3) ·
  HV5 sniper bolt (1/1/2) · HV8 ".50" (one tag anywhere = out; saturates every threshold).
- **Sniper one-shot** = HV5 (instant on head OR chest, 2 on limbs) — mirrors BO6's SVD
  upper-torso-up zone. The *starter* sniper is the least forgiving; zone forgiveness is the sniper
  progression axis (exactly BO6: the L1 LW3A1 has the smallest one-shot zone, the L49 LR 7.62 the
  biggest). Costs shipped WITH it: 0.45–0.55s ADS, flinch, scope glint, slow rechamber, useless
  hipfire (4.5° cone).
- **Shotguns = pellet volleys** (new `Pellets` field, 6–8 × HV1, fixed `PelletSpreadDeg` cone,
  independent of bloom): ≥5 chest pellets = instant out — CoD's "3-of-8 pellets kill" translated.
  One-shot envelope ≈ chest-radius/tan(PelletSpread): ~1100uu practical, hard-capped by
  ProjLifetimeSec (~2100uu max reach — the CoD shotgun cliff). Pump = deep envelope + slow; semi =
  shallow envelope + fast follow-up (the Marine SP vs ASG-89 split).
- **Revolvers = HV2** (2 head tags = out at 200–250 ms — never 1: the .357 Snake Shot emergency
  nerf is the cautionary tale).
- **Minigun**: new `SpinupSec = 0.6` before the first BB (BO6 Death Machine template), hip-only,
  cone TIGHTENS while spinning (negative bloom to a 2.2° floor), 222 ms sustained but 822 ms from
  a cold trigger, slowest movement in the game.
- **Range identity** (replaces CoD damage falloff; optional v1.1): per-BB HitValue ×0.66–0.8 past
  a per-class range band (SMG ~1500uu, AR ~3500uu, LMG flat — its identity). V1 can ship on
  MuzzleSpeed×Lifetime reach alone (SMG 10.6–18k uu vs AR 26–35k already enforces range roles).

## 2. The halo/bloom + recoil spec (per-class)

Move the existing global knobs (PFWeaponComponent.h:130-158) into `FPFWeaponDef`; add continuous
decay (crosshair visibly "breathes" like CoD) with the reset-gap as floor. ADS stays ~0 spread;
aimed difficulty = the also-already-existing **real recoil climb** (control-rotation pitch/yaw per
shot + recover — PFWeaponComponent.cpp:385-397) made per-weapon.

| Class | Hip cone° | ADS° | Bloom/shot° | Free shots | Cap° | Decay°/s | Move mult | Climb pitch°/shot |
|---|---|---|---|---|---|---|---|---|
| AR | 1.1 | 0.05 | 0.12 | 5 | 1.8 | 6 | 1.5 | 0.30 |
| SMG | 1.5 | 0.10 | 0.10 | 6 | 2.2 | 8 | **1.15** (moves well) | 0.22 |
| Pistol | 1.3 | 0.15 | 0.30 | 2 | 2.6 | 10 | 1.3 | 0.45 |
| Shotgun | 0.9 (aim cone) | 0.15 | 0.50 | 1 | 2.0 | 8 | 1.2 | 1.2 |
| Sniper | 4.5 (useless) | 0.02 | — bolt | — | — | — | 2.0 | 3.0 one big kick |
| LMG | 2.2 | 0.15 | 0.05 | 10 | 1.2 | 4 | **1.8** (plant your feet) | 0.35 |
| Minigun | 3.0 fixed | none | **−0.02** (tightens) | — | 2.2 floor | — | — | — |

Crosshair: the existing spread→pixel-gap projection (PFCombatHUDWidget.cpp:862-878) is already
correct; add a faint **halo ring at ≥75% of bloom cap** — the "ease off the trigger" tell.
Per CoD practice, keep *visual* camera shake smaller than actual climb (readability).

## 3. The 37-weapon stat table

Catalog reality check: **37 weapons**, not 41 (10 AR + 8 SMG + 7 pistol + 4 shotgun + 4 sniper +
4 LMG). Four near-clones are **skins** sharing a stats block (blueprint analogues). ADS classes:
Fast 0.18s / Med 0.25 / Slow 0.35 / Sniper 0.45+. TTK = chest, close.

### Assault Rifles (starter: Rifle default)
| Id | Weapon | Personality | HV | Mag | Bps | Modes | Hip/ADS° | Range (uu) | ADS | TTK | Rank |
|---|---|---|---|---|---|---|---|---|---|---|---|
| ar_m4 | Rifle (default) | THE starter — does everything | 1 | 30 | 12 | S/B/A | 1.1/.05 | 26.4k | Med | 333ms | **L1** |
| ar_m4_olive | Rifle (Olive) | *skin of ar_m4* | — | — | — | — | — | — | — | — | L4 |
| ar_ak_black | AK (Black) | the slugger: heavy BBs, kicks hard | 2 | 30 | 6.5 | S/A | 1.4/.06 | 26.4k | Slow | 308ms (head 154!) | L6 |
| ar_ak_wood | AK (Wood) | *skin of ar_ak_black* | — | — | — | — | — | — | — | — | L17 |
| ar_r01 | Rifle 01 | CQB carbine — fastest AR handling, short reach | 1 | 30 | 13.5 | S/A | 1.0/.06 | 18.9k | Fast | 296ms | L9 |
| ar_r02 | Rifle 02 | the marksman AR — tightest ADS, longest reach | 1 | 30 | 10.5 | S/B/A | 1.3/.03 | 35.1k | Slow | 381ms | L14 |
| ar_r03 | Rifle 03 | THE BURST DMR: 3× heavy BBs, all-chest = one-burst (143ms), whiff = 523ms | 2 | 21 | burst3@14 | B | 1.6/.04 | 33.8k | Med | 143–523ms | L35 |
| ar_r04 | Rifle 04 | modern heavy (the AK's polymer cousin) | 2 | 24 | 6 | S/A | 1.5/.06 | 28.8k | Slow | 333ms | L27 |
| ar_r05 | Rifle 05 | the laser — lowest recoil/bloom in the game | 1 | 30 | 11 | S/B/A | 1.1/.05 | 27.5k | Med | 364ms | L20 |
| ar_r06 | Rifle 06 | hi-cap workhorse — 40-round mag | 1 | 40 | 12 | S/A | 1.3/.06 | 26.4k | Slow | 333ms | L45 |

### SMGs (starter: AKSU Black)
| Id | Weapon | Personality | HV | Mag | Bps | Hip/ADS° | Range | ADS | TTK | Rank |
|---|---|---|---|---|---|---|---|---|---|---|
| smg_aksu_black | AKSU | starter SMG — sprays happy | 1 | 25 | 14 | 1.5/.10 | 11.9k | Fast | 286ms | **L1** |
| smg_aksu_wood | AKSU (Wood) | *skin* | — | — | — | — | — | — | — | L11 |
| smg_01 | SMG 01 | the balanced MP5-alike | 1 | 30 | 15 | 1.4/.09 | 13.5k | Fast | 267ms | L3 |
| smg_02 | SMG 02 | run-and-gun: near-zero move penalty, lives airborne | 1 | 25 | 16 | 1.6/.12 | 11.4k | Fast | 250ms | L8 |
| smg_03 | SMG 03 | the precision SMG (MP7 energy) — AR-ish reach | 1 | 30 | 13 | 1.2/.07 | 18k | Fast | 308ms | L13 |
| smg_04 | SMG 04 | the pocket AK — heavy BBs, small gun | 2 | 20 | 5.5 | 1.7/.10 | 15.2k | Med | 364ms (head 182) | L19 |
| smg_05 | SMG 05 | HI-CAP SPRAY — 50-rd drum, hose that wanders | 1 | 50 | 15 | 1.8/.12 | 12.6k | Slow | 267ms | L29 |
| smg_06 | SMG 06 | the speed demon — best close TTK of any primary | 1 | 22 | 17 | 1.5/.10 | 10.6k | Fast (0.15s) | 235ms | L43 |

### Pistols + Revolvers (starter: Pistol)
| Id | Weapon | Personality | HV | Mag | Bps | TTK | Rank |
|---|---|---|---|---|---|---|---|
| pis_std | Pistol | starter secondary — honest, steady | 1 | 18 | 8 | 500ms | **L1** |
| pis_01 | Pistol 01 | the fast striker — snappiest double-tap | 1 | 15 | 10 | 400ms | L2 |
| pis_02 | Pistol 02 | *skin of pis_01* | — | — | — | — | L24 |
| pis_03 | Pistol 03 | hi-cap + 3-burst mode | 1 | 17 | 9 | 444ms | L12 |
| pis_04 | Pistol 04 | the match pistol — tightest handgun (0.9° hip) | 1 | 15 | 9 | 444ms | L33 |
| rev_01 | Revolver 01 | the hand cannon — 2 head tags = out | 2 | 6 | 4 | 500ms (head 250) | L16 |
| rev_02 | Revolver 02 | quick-draw snub — fastest swap+ADS, wilder hip | 2 | 6 | 5 | 400ms (head 200) | L41 |

### Shotguns (pellet volleys; starter: Shotgun 01)
| Id | Weapon | Personality | Pellets@° | Mag | Bps | One-shot env. | Rank |
|---|---|---|---|---|---|---|---|
| sg_01 | Shotgun 01 | starter pump — the classic | 8@2.2° | 6 | 1.3 | ≤1100uu | **L5** |
| sg_02 | Shotgun 02 | the tight choke — reaches, demands center-mass | 6@1.4° | 5 | 1.1 | ≤1700uu | L15 |
| sg_03 | Shotgun 03 | the double-barrel — 2 shells fast, 2.2s reload; glorious chaos | 8@2.8° | 2 | 3.0 | close | L22 |
| sg_04 | Shotgun 04 | THE SEMI-AUTO — 10 rds, usually a 2-tap mid | 6@2.6° | 10 | 2.8 | ~250uu | L37 |

### Snipers (starter: Sniper 01)
| Id | Weapon | Personality | HV | Mag | Bps | Range | ADS | Rank |
|---|---|---|---|---|---|---|---|---|
| snp_01 | Sniper 01 | starter bolt — head/chest instant, limbs 2 | 5 | 5 | 0.9 | 48k | 0.45s | **L7** |
| snp_02 | Sniper 02 | the quickscope — 0.30s ADS, faster bolt, more sway | 5 | 5 | 1.1 | 44.8k | 0.30s | L18 |
| snp_03 | Sniper 03 | the semi marksman — HV3 @3bps, chest 2-tap 333ms, 10-rd | 3 | 10 | 3 | 42k | Slow | L31 |
| snp_04 | Sniper 04 | THE .50 — one tag ANYWHERE = out; 1.55s bolt, slowest gun alive | 8 | 4 | 0.65 | 54.4k | 0.55s | L47 |

### LMGs (starter: LMG 01)
| Id | Weapon | Personality | HV | Mag | Bps | Range | ADS | TTK | Rank |
|---|---|---|---|---|---|---|---|---|---|
| lmg_01 | LMG 01 | starter — 75-rd sustained-fire identity | 1 | 75 | 10 | 22k | 0.40s | 400ms | **L10** |
| lmg_02 | LMG 02 | the heavy MG — HV2 belt | 2 | 60 | 6 | 26.4k | 0.45s | 333ms | L25 |
| lmg_03 | LMG 03 | the suppressor — 100-rd wall of BBs | 1 | 100 | 12 | 22k | Slow | 364ms | L39 |
| lmg_minigun | Minigun | the L50 flex — 0.6s spin-up, 18bps, tightens as it spins | 1 | 150 | 18 | 13.5k | hip-only | 222ms spun / 822ms cold | **L50** |

## 4. The rank-unlock ladder (L1–50)

**L1 open:** AR + SMG + Pistol categories with `ar_m4` / `smg_aksu_black` / `pis_std` — all
top-half competitive (the code's DefaultConfig already points at them, so no default ever
references a locked weapon). Shotgun/Sniper/LMG open **as categories** at L5/L7/L10 with their
starters (BO6's model — category variety IS the early reward).

```
L1  ar_m4 + smg_aksu_black + pis_std      L18 snp_02          L35 ar_r03 (burst DMR)
L2  pis_01                                L19 smg_04          L37 sg_04
L3  smg_01                                L20 ar_r05          L39 lmg_03
L4  ar_m4_olive (skin)                    L22 sg_03           L41 rev_02
L5  sg_01  — SHOTGUNS OPEN                L24 pis_02 (skin)   L43 smg_06
L6  ar_ak_black                           L25 lmg_02          L45 ar_r06
L7  snp_01 — SNIPERS OPEN                 L27 ar_r04          L47 snp_04 (.50)
L8  smg_02                                L29 smg_05          L50 lmg_minigun
L9  ar_r01                                L31 snp_03
L10 lmg_01 — LMGs OPEN                    L33 pis_04
L11 smg_aksu_wood (skin)
L12 pis_03    L13 smg_03    L14 ar_r02    L15 sg_02    L16 rev_01    L17 ar_ak_wood (skin)
```

One unlock **every level 2–20** (matches the XP curve's level-per-session pacing — one new gun per
session, the CoD dopamine cadence), every-other after, capstones at 47/50. Skins fill cheap
intermediate slots. Offline / bots / LAN listen play: **fully ungated** (also the offline-API
fallback — no login, everything available, per the accounts-optional-offline decision).

## 5. Backend + enforcement architecture

- **Stable string weaponIds, never category/index** — indices already shifted once when the
  modern pack merged into `GAllRifles`; positional unlock rows would silently remap. Add
  `WeaponId` to `FPFWeaponDef` + `PFWeapon::FindById()`. Saved loadouts migrate to slugs (int
  fallback one version).
- **unlock_definitions rows (INSERT-only, existing table):**
  `{ unlock_id:"wpn.ar_ak_black", kind:"weapon", criteria:{"type":"level","gte":6}, payload:{weaponId:"ar_ak_black",category:0,index:2,displayName:"AK (Black)"}, active:1, sort:6 }` —
  weaponId is truth, category/index derived convenience. Explicit `gte:1` rows for starters so
  the client rule is one line: *selectable iff unlock_id ∈ profile.unlocks[]*.
- **Client:** `FPFBackendProfile` gains `UnlockIds` (FetchProfile currently DROPS the unlocks[]
  the API already returns); picker filters/badges.
- **Server enforcement seam:** `ServerSetKit_Implementation` (CombatForgeCharacter.cpp:2258)
  currently performs **ZERO validation** — a hacked client can equip anything today. Fleet servers
  validate the claimed kit against the player's unlocks (needs one new API flow: server-key GET of
  a joining player's unlocks); on failure force the category starter. LAN/listen: advisory only.
- **UI:** the weapon stepper (`BuildWeaponPicker`/`RefreshWeaponLabels`, PFLoadingMenuWidget.cpp:622-741)
  gets a 🔒 + "Unlocks at Rank N" suffix; browsing allowed, equip blocked. Death-screen class
  scroll + class cards need the same awareness. NOTE: the secondary weapon has **no menu picker**
  yet (console-only `pf.Weapon2`) — build it with lock support in the same pass.
- **Kid-legible 4-bar display** (Power/Speed/Range/Control) computed honestly from the same sim
  numbers (formulas in the research record) — the starter M4 shows the flattest bar chart in the
  game: "your first gun is good at everything" is both the message and the math.

## 6. Implementation seams (for the build task — all verified file:line)

1. **FPFWeaponDef** (PFWeaponCatalog.h:16-40) gains: `WeaponId`, `HitValue`, `Pellets`,
   `PelletSpreadDeg`, bloom set (PerShot/Cap/FreeShots/DecayPerSec), climb set (Pitch/Yaw/Recover),
   `ADSTimeSec`, `SprintOutTime`, `MoveSpeedMult`, `ReloadTime`, `SpinupSec`, `ReburstDelaySec`.
   Thread through the ONE copy block `ApplyWeaponLoadout` (CombatForgeCharacter.cpp:2117-2128) —
   replication-safe by construction (both sides derive from the shared static catalog keyed by
   the replicated FPFKitRep).
2. **Power:** fill `FPFPaintHitInfo.Damage = HitValue` in `ResolveAuthoritativeImpact`
   (PFPaintballProjectile.cpp:267-279, reads SourceWeaponWeak — server-side resolve, zero packet
   change, cheat-proof); honor in `ApplyPaintHit` (PFHealthComponent.cpp:123-133). Sniper
   instant-out reuses the melee `bForceEliminate` precedent conditioned on region.
3. **Pellets:** clone the frag-burst deterministic sub-seed pattern
   (PFGrenadeProjectile.cpp:264-322, `Seed + i + 1`) into the three shot paths (FireOneShot /
   ServerFire / MulticastShotFX). 1 packet, 1 ammo, 1 fire token per pull. B3 one-VRandCone-pull
   contract respected via per-pellet sub-streams. Cosmetic pool (64) is ample.
4. **Handling:** ADS time (CombatForgeCharacter.h:462, actual defaults 0.14/0.14 — header comment
   stale), SprintOutTime already lives on the weapon component, MoveSpeedMult goes into
   `GetMaxSpeed` (PFCharacterMovementComponent.cpp:115-156) — **must derive from replicated kit
   state on server + autonomous proxy identically or swaps rubber-band** (prediction path!).
5. **Server fire validation:** token refill is ALREADY per-weapon (`max(1, FireRateBps)`,
   PFWeaponComponent.cpp:481-490) — no change needed for high-bps guns; pellet volley = 1 token
   is the only adjustment. Fire MODE remains client-trusted (only ROF is capped) — acceptable.
6. **Bots:** share the full stat path already; per-bot `FPFKitRep` assignment at spawn gives bots
   varied loadouts (plumbing exists, assignment missing). Bots skip recoil climb by design.
7. **Backend:** parse `unlocks[]` in FetchProfile; seed 37 unlock_definitions rows (script);
   new fleet endpoint: GET a joining player's unlocks by identity (server-key auth).

**Suggested build order:** (1) WeaponId + stat fields + per-weapon bloom/climb/handling with the
table's numbers (feel change, no new systems) → (2) HitValue + sniper/revolver tiers →
(3) shotgun pellets → (4) Minigun spin-up → (5) unlock parsing + UI badges + ServerSetKit
validation + D1 seed → (6) secondary-weapon picker UI → (7) hitmarker audio polish (the cheapest
single "feels like CoD" win per the research).

## 7. Risks (from the adversarial pass)

- **Rifle 03 one-burst (143 ms best case)** is the most volatile number for mixed kid/adult
  lobbies — requires all 3 heavy BBs on chest (2/3 = 4 pts < 5, no elim). Playtest early; fallback:
  reburst 0.45s or mag 18.
- **HV2 head TTKs (154–250 ms)** are CoD-fast — verify the youngest players can parse "out in two"
  via the region-pip HUD.
- **Projectile snipers may play harsher than hitscan CoD** (travel time but no falloff) — the
  flinch/glint/slow-ADS costs must ship WITH the one-shot, not later.
- **Modern Rifles currently share identical stats** — shipping the ladder before the stat table
  makes late unlocks feel like reskins; stats first, gates second.
- **Skins-as-unlocks**: group them visually in the picker or kids will hunt a stat difference
  that doesn't exist.
- CoD numbers sourced from season-era secondary sources (TrueGameData/sym.gg resist scraping) —
  treat as design targets; playtest tuning will move them.

## 8. Decisions Tom owns

1. **The two paper-power aspirationals:** snp_04 (one-tag-anywhere .50 @ L47) and the Minigun
   (222 ms sustained @ L50) deliberately soften the "zero paper advantage" guardrail — both paid
   for with the game's worst handling. Bless or flatten?
2. **Secondary slot:** pistols-only (CoD-style) or any-category (current code allows Shotgun 03
   as a secondary — strong)?
3. **Skins:** own ladder slots (as tabled) or auto-bundle with their base weapon?
4. **Minigun as L50 loadout primary** vs CoD-precedent care-package-style pickup?
5. **Range-banded HitValue falloff** in v1 or defer (v1 ships on reach limits alone)?
6. **Burst archetype** — Rifle 03 as tabled, or also a burst SMG/pistol family?
