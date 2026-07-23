# Character + weapon setup — superseded

This file was a scaffold-status doc from the `feat/art-pass` era and described a pipeline that no
longer exists. Current truth:

- **Third-person weapon grip**: per-weapon `TPLoc/TPRot/TPScale` on the catalog row
  (`Source/CombatForge/Combat/PFWeaponCatalog.h`), with a computed per-mesh default for untuned
  rows (`PFWeapon::ComputeAutoTPGrip` — pivot + barrel-axis compensation). Tune live with
  `pf.WeaponTP`, transfer one gun's tune to the whole catalog with `pf.WeaponTPCalibrate`,
  inspect everything with `pf.WeaponDump`. Kill switch: `pf.WeaponAutoTP 0`.
- **First-person poses**: per-weapon `FPLoc/FPRot/FPScale` + `AdsLoc/AdsRot` catalog rows —
  `pf.WeaponFP` / `pf.WeaponADS` / middle-mouse drag (`pf.WeaponDrag 1`).
- **Modular character**: `Source/CombatForge/Player/PFCharacterCustomization.*` + the in-game
  CLASS tab. Locomotion anims are the IK-retargeted set in the **root** of `/Game/RifleAnims/` —
  NEVER the `Animations/BlendSpaces/...` originals (same asset names; those stretch the torso via
  the compatible-skeleton remap).

See `docs/` for the full playbooks.
