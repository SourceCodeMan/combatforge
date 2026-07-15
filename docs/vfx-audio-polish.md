# VFX + Audio Polish (Combat Feel Juice)

## What shipped in code

### Muzzle VFX — `UPFCombatVFX`
- Soft-loads Niagara from `/Game/FX/NS_MuzzleFlash` + `/Game/FX/NS_MuzzleSmoke` (optional).
- Fallback: pooled mesh flash (`M_PF_Flash`) + CO₂ wisps (`M_PF_MuzzleSmoke`) + one-frame point light.
- Neutral white/warm (not team-tinted). FP scale is smaller than TP.
- Wired from `FireOneShot` (local) and `MulticastShotFX` (remotes + listen-host viewers).

### Impact VFX — `UPFSplatSubsystem`
- Soft-loads Niagara from `/Game/FX/NS_Impact` (optional), team-tinted via `User.Color` / `Color`.
- Fallback: 3-wisp expanding dust burst with per-slot fade MIDs (`M_PF_ImpactDust`).
- Existing deferred decal scuffs (`M_PF_ImpactMark`) unchanged.
- Spatial impact thump via `UPFCombatAudio::PlayImpactAt`.

### Layered spatial audio — `UPFCombatAudio`
| Layer | Local owner | Remote / distance |
|---|---|---|
| Close transient | 2D procedural pop | Spatial short-range |
| Body | Spatial gunshot cue | Spatial gunshot cue |
| Mech / gas | Quiet metal/whoosh layer | — |
| Tail | — | Lower-pitched distant layer |

- Attenuation: **NaturalSound** + distance LPF (not linear drop).
- Concurrency: muzzle `StopOldest` (cap 10); impact `StopFarthestThenOldest` (cap 12).
- Fixed broken pack paths (`Explosion_Medium`, `Interface_3-3` for fire-select).

### Listen-host fix
`MulticastShotFX` no longer early-outs on `HasAuthority()`. Dedicated servers still skip; local owner still skips (already played in `FireOneShot`); listen host now hears/sees **bot and remote** muzzle reports.

---

## Optional Niagara drop-in (highest visual jump)

Create folder `Content/FX/` and either:

### A) Fab (recommended free) — download these first

| Priority | Asset | Link | Drop / rename as |
|---|---|---|---|
| **P0** | **50+ Niagara VFX Sample Project** (Epic, free) — muzzle, impacts, smoke, explosions | https://www.fab.com/listings/0e188eca-4e54-4fb2-a9ed-d8b8a565e600 | Copy key systems → `/Game/FX/NS_MuzzleFlash`, `NS_MuzzleSmoke`, `NS_Impact` |
| **P0** | **Muzzle Flash (Niagara)** free single | https://www.fab.com/listings/435b3bcb-d7f5-467d-99aa-2edc97a6c5fd | `/Game/FX/NS_MuzzleFlash` |
| **P1** | **50 Free Game Sounds Pack** (already in project as `Free_Sounds_Pack`) | https://www.fab.com/listings/b8cc7270-5e0c-4ef7-a277-6a1d4b69358d | already wired |
| **P1** | **Free Weapon Sound Effects** | https://www.fab.com/listings/1697af22-7e2a-410e-b8c0-88239216520d | replace mil gunshots with closer "marker" takes if available |
| **P2** | Author's **Gun Sounds** listing (from same 50 Free family) | https://www.fab.com/listings/1f8f6556-1861-453d-b9a4-22b4d650493f | more gunshot variety |

Also search Fab free filter: `impact decal`, `dust Niagara`, `smoke Niagara`, `airsoft`, `paintball`.

### B) Migrate from the local Lyra tree (already on disk)
Copy (Migrate in editor, not raw filesystem copy) from `LyraStarterGame/Content/Effects/Particles/`:

| Source | Drop into CombatForge as |
|---|---|
| `Weapons/NS_WeaponFire_MuzzleFlash_Rifle` | `/Game/FX/Lyra/NS_WeaponFire_MuzzleFlash_Rifle` |
| `Weapons/NS_WeaponFire` (or smoke emitters) | `/Game/FX/NS_MuzzleSmoke` or `/Game/FX/Lyra/...` |
| `Impacts/NS_ImpactConcrete` | `/Game/FX/Lyra/NS_ImpactConcrete` or `/Game/FX/NS_Impact` |

Code already soft-loads both `/Game/FX/NS_*` and `/Game/FX/Lyra/*` fallbacks.

### C) Author names the code expects

```
/Game/FX/NS_MuzzleFlash
/Game/FX/NS_MuzzleSmoke
/Game/FX/NS_Impact
```

For impacts: expose a linear color user param named `User.Color` or `Color` (team tint is applied in C++ before activate).

---

## Fab search queries (free filter)

| Priority | Query | Use for |
|---|---|---|
| P0 | `Niagara muzzle flash free` | Replace mesh flash |
| P0 | `Niagara smoke dust free` | Muzzle CO₂ + impact puffs |
| P0 | `airsoft` / `paintball` / `BB gun sound free` | Replace mil gunshots |
| P1 | `impact decal free Unreal` | Scuff variety |
| P1 | `FPS weapon foley free` | Reload / selector / hopper |
| P2 | `explosion Niagara free` | Frag polish |
| P2 | `spatial audio` / MetaSounds gun | Next-gen layering |

**Fiction note:** CombatForge is airsoft/paintball. Prefer **gas dump + mechanical click + BB impact** over AR muzzle blast and shell casings. If you import CoD-style orange flashes, scale them down (~0.5) and desaturate, or players will read "lethal milsim."

---

## Regenerate mesh materials (if missing)

```bat
UnrealEditor-Cmd.exe "D:\projects\combatforge\CombatForge.uproject" -run=pythonscript -script="D:\projects\combatforge\Scripts\gen_combat_fx.py" -unattended -nosplash -nopause -stdout
```

Produces `M_PF_Flash`, `M_PF_MuzzleSmoke`, `M_PF_ImpactDust`, `M_PF_SmokeVolume`.
