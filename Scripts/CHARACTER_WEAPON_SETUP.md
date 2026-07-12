# Character + Weapon art loadout (M1 slice, steps 3–4)

`APaintForgeCharacter` now has an optional **art loadout** that stays fully dormant until assets
are assigned — **unset = the exact graybox** (validated playtest behavior unchanged). Written on
branch `feat/art-pass`; **compile-verify pending** (needs the editor closed to rebuild the module).

## What the scaffold exposes (on `APaintForgeCharacter`)

| Property | Type | Feeds |
|---|---|---|
| `ThirdPersonBodyMesh` | `USkeletalMesh` | the TP body → `GetMesh()` (hides the graybox cubes) |
| `ThirdPersonAnimClass` | `TSubclassOf<UAnimInstance>` | the body's anim BP |
| `FirstPersonArmsMesh` | `USkeletalMesh` | the `FirstPersonArms` component (owner-only) |
| `WeaponMesh` | `UStaticMesh` | the `WeaponMeshComp`, snapped to `WeaponAttachSocket` on the body |
| `WeaponAttachSocket` | `FName` (default `hand_rSocket`) | which body socket the weapon attaches to |
| `TeamBodyMaterial` | `UMaterialInterface` | per-team body tint via its `"Color"` param |

`ApplyArtLoadout()` runs in `BeginPlay`; `SetTeamColor`/`SetEliminatedAppearance` already branch to
the skeletal body when it's in use.

## What to shop for

1. **A realistic-military operator** — a single **skeletal mesh** on (or retargetable to) the **UE5
   Mannequin** or **MetaHuman** skeleton, so the free Epic anims retarget cleanly. (A MetaHuman works
   but is multi-part; a Fab "operator/soldier" with one skeletal mesh is the simplest drop-in.)
2. **A rifle** — a **static mesh** is enough for the slice (a weapon in-hand already sells it);
   skeletal + reload anims come later.
3. Anims you already own (free, in your Fab library): **Game Animation Sample** (locomotion) +
   **Animation Starter Pack** (rifle fire/ADS/reload) → retarget to the operator's skeleton → wrap in
   an Anim BP → that's `ThirdPersonAnimClass`.

## Wiring it (when you're back with assets)

The project is native C++ (no BP), so the properties get set one of two ways — tell me which:
- **(a) I add `ConstructorHelpers::FObjectFinder` loads** to your imported asset paths (give me the
  paths, e.g. `/Game/Art/Characters/SK_Operator`, `/Game/Art/Weapons/SM_Rifle`). Matches the existing
  pattern (BasicShapes, `M_PF_BuildPiece`). Fastest for a solo native project.
- **(b) I make a `BP_PaintForgeCharacter` subclass**, set the properties there, and point the GameMode
  default pawn at it. More editor-native, easier for you to tweak later.

Then: close the editor → I build → relaunch → your operator + rifle appear, team-tinted, in place of
the cubes. Socket note: if your operator's hand socket isn't named `hand_rSocket`, we set
`WeaponAttachSocket` to match (or add a socket in the skeleton).
