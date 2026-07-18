# CombatForge Weapon Attachments — Design + Implementation Plan

*2026-07-18 · Tom's ask: "the weapon pack has attachments that can be modified. Add the ability to modify
attachments on my gun when creating a class — and match the CoD research for how it affects shooting/accuracy."*

**Status: PLAN + data-model scaffold only.** The gameplay/stat half is pure C++ and layers cleanly on the
existing per-weapon `FPFWeaponDef`. The *visual* half (attachment meshes + weapon sockets) is content/editor
work only Tom can do in-editor — so this is documented rather than half-built. Build order below is safe and
incremental (ship the stat system first; the meshes follow).

## 0. Why this is mostly a delta system, not a new system
The weapon overhaul (`docs/weapon-balance-plan.md`, already implemented in `FPFWeaponDef`) made every stat that
matters per-weapon: `SpreadHipDeg/SpreadADSDeg/MoveSpreadMult` (accuracy), `Bloom*`/`Climb*` (recoil),
`ADSTimeSec` (handling), `MuzzleSpeedUU×ProjLifetimeSec` (range), `MagSize`, `FireRateBps`. An attachment is
just a **named set of deltas** applied on top of the equipped weapon's Def at `ApplyWeaponLoadout` time. So the
whole gameplay effect is: pick attachments → look up their deltas → add them to the working stat block before it
is copied into `UPFWeaponComponent`. No new fire code, no replication redesign.

## 1. Data model (pure C++, no assets)
Add to `Combat/PFWeaponCatalog.h`:
```cpp
enum class EPFAttachSlot : uint8 { Muzzle=0, Optic=1, Underbarrel=2, Magazine=3, Stock=4, Count };

struct FPFAttachmentDef
{
    const TCHAR* Id          = nullptr;      // stable slug, e.g. "muz_suppressor"
    const TCHAR* DisplayName = TEXT("None");
    EPFAttachSlot Slot       = EPFAttachSlot::Muzzle;
    const TCHAR* MeshPath    = nullptr;      // static mesh to socket onto the gun (nullptr = stat-only for now)
    const TCHAR* SocketName  = nullptr;      // socket on the weapon mesh (e.g. "muzzle","optic","grip","mag")
    // --- stat DELTAS (added/multiplied onto the weapon Def; 0 / 1.0 = no change) ---
    float SpreadHipMul   = 1.f;   // <1 tightens hip
    float SpreadADSMul   = 1.f;
    float ADSTimeAdd     = 0.f;   // + slower to aim (optics/underbarrel), - faster (lightweight)
    float MoveSpeedMul   = 1.f;   // heavy attachments slow you
    float ClimbMul       = 1.f;   // <1 = less recoil climb (compensator/grip/stock)
    float BloomMul       = 1.f;   // <1 = tighter sustained fire
    float RangeMul       = 1.f;   // MuzzleSpeed×Lifetime multiplier (barrels/suppressor)
    int8  MagSizeAdd     = 0;     // extended mags
    float ScopedADSFOVSet= 0.f;   // >0 overrides scope zoom (optics that magnify)
    uint8 UnlockRank     = 1;
};
```
A player's per-weapon choice is one index per slot:
```cpp
struct FPFAttachmentLoadout { int8 Slots[(int)EPFAttachSlot::Count] = { -1,-1,-1,-1,-1 }; }; // -1 = none
```

## 2. CoD-grounded slot menu (the deltas are the CoD tradeoff frontier — never pure upgrades)
Every attachment must **cost** something (the anti-Battlefront-II rule from the progression plan). Suggested v1
set (2–4 per slot; tune in playtest):

| Slot | Attachment | Helps | Costs |
|---|---|---|---|
| **Muzzle** | Compensator | recoil climb −25% (`ClimbMul .75`) | hip spread +10% |
| | Suppressor | (quieter — audio later) + range +10% | ADS +0.03s, bloom +10% |
| | Long Barrel | range +20% (`RangeMul 1.2`) | ADS +0.05s, move −3% |
| **Optic** | Red Dot | cleaner sight picture (cosmetic) | none |
| | 3× Scope | `ScopedADSFOVSet 40` (mild zoom) | ADS +0.06s |
| | Sniper Scope | `ScopedADSFOVSet 25` (strong) | ADS +0.10s (snipers only) |
| **Underbarrel** | Vertical Grip | climb −15%, hip −10% | move −4% |
| | Angled Grip | ADS −0.03s (faster) | climb +5% |
| **Magazine** | Extended Mag | `MagSizeAdd +10` | ADS +0.04s, move −3% |
| | Fast Mag | reload −20% (add `ReloadMul`) | −5 rounds |
| **Stock** | Light Stock | move +6%, ADS −0.03s | climb +10% |
| | Heavy Stock | climb −20% | move −5%, ADS +0.04s |

The 4-bar kid-legible display (Power/Speed/Range/Control) recomputes from the post-delta stat block, so the
tradeoff is visible without reading numbers.

## 3. Application seam (one place, pure C++)
In `ACombatForgeCharacter::ApplyWeaponLoadout` (`CombatForgeCharacter.cpp:2318`), after resolving `const
FPFWeaponDef& Def`, build a *working copy* and fold in the loadout's attachment deltas BEFORE the block that
copies into `WeaponComponent` (2414-2440) and sets `ADSInTime`/`ADSFOV`/`ScopedADSFOV`. Because both server and
client derive from the replicated kit + the shared catalog, this stays replication-safe by construction (same
rule as the weapon stats). The attachment loadout must be part of the replicated `FPFKitRep` (add a small
fixed-size array) so all machines apply identical deltas — do NOT read it from local prefs on non-owners.

## 4. Persistence
Mirror `PFWeapon::SaveConfig`/`LoadConfig` (per class slot, `GGameUserSettings.ini [CombatForge]`): store the
5 attachment indices per weapon per class slot as a CSV, keyed by `WeaponId` slug (never index — indices shift).

## 5. UI (procedural C++, like the weapon picker)
In `PFLoadingMenuWidget` CLASS tab, under the existing CATEGORY/WEAPON steppers, add one stepper row per
`EPFAttachSlot` (label = slot name, value = attachment display name + a 🔒 rank badge, same pattern as
`BuildWeaponPicker`/`RefreshWeaponLabels`). Reuse the just-fixed browse-vs-equip decoupling. Show the 4-bar
stat preview updating live as attachments change.

## 6. The content half (Tom, in-editor — the actual blocker)
- Confirm the Modern Weapons pack's attachment meshes exist as separate static meshes (muzzle/optic/grip/mag).
  List their asset paths → fill `FPFAttachmentDef::MeshPath`.
- Add **sockets** to each weapon's static mesh in-editor: `muzzle`, `optic`, `grip`, `mag` at the right spots.
  (Static meshes support sockets; the game sockets the attachment mesh onto the weapon at those names.)
- Add `+DirectoriesToAlwaysCook` for the attachment mesh dir (they're string-loaded, same gotcha as the guns).
- Until meshes/sockets exist, ship §1–§5 as **stat-only** attachments (no visible mesh) — fully playable, and
  the visuals drop in later with zero gameplay change.

## 7. Backend (unlocks — optional, later)
Attachments can be rank-gated exactly like weapons: `unlock_definitions` rows with `atch.<id>` ids, parsed into
`FPFBackendProfile::UnlockIds`, checked by an `IsAttachmentUnlocked()` helper (returns true offline/LAN). No new
backend endpoint needed — reuses the existing profile unlocks array.

**Recommended first PR:** §1 data model + §3 application + §5 UI as **stat-only** (no meshes). That delivers the
"modify attachments, feel the CoD tradeoff" gameplay immediately; the meshes + sockets are a clean follow-up.
