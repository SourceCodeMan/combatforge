# Scripts — Pass 2 | Date 2026-07-23 | Status done

## Summary

Editor Python tooling (materials, nav bounds, retarget, dumps) and cross-platform package wrappers are generally careful: delete-and-recreate materials (avoid rooted wipe crash), privacy scrub on package, NetProtocol bump reminder, dry-run on the master build material. Gaps: most destructive generators lack `PF_DRY_RUN`; `gen_fx_content.py` overlaps `gen_combat_fx.py`; weapon geometry dump covers only a fraction of the catalog; packaging scripts remind but cannot enforce NetProtocol bumps.

**Counts:** 0 blocker · 0 major · 5 minor · 4 nit

## Findings

### 1. Destructive material generators lack dry-run guard
- **ID:** P2-S1
- **Severity:** minor
- **Status:** open
- **File:** e.g. `Scripts/gen_arena_materials.py:69-77`, `Scripts/gen_combat_fx.py:30-32`, `Scripts/gen_team_material.py:25-26`, `Scripts/gen_glass_fade_material.py:22-24`, `Scripts/gen_splat_decal_material.py:43-45`, `Scripts/make_roofpanel_nonvt.py:11-12`
- **Symptom:** Accidental re-run of a headless script deletes live content assets with no preview step.
- **Why:** Only `create_build_material.py` implements `PF_DRY_RUN=1` (lines 96-102). Peer generators always `delete_asset` then recreate.
- **Fix:** Copy the dry-run env gate (and optional confirm log) into all delete-and-recreate scripts; document in `MATERIAL_SETUP.md`.
- **Confidence:** high
- **Source:** this-pass

### 2. `gen_fx_content.py` duplicates / fights `gen_combat_fx.py` for `M_PF_Flash`
- **ID:** P2-S2
- **Severity:** minor
- **Status:** open
- **File:** `Scripts/gen_fx_content.py:14-42` vs `Scripts/gen_combat_fx.py:163`
- **Symptom:** Running both scripts thrash-rebuilds the same path with different default emissive strengths/colors.
- **Why:** Older flash-only generator left beside the multi-material combat FX generator; both target `/Game/Materials/M_PF_Flash`.
- **Fix:** Delete or redirect `gen_fx_content.py` to call/document “use gen_combat_fx.py”; keep one owner of flash defaults.
- **Confidence:** high
- **Source:** this-pass

### 3. `dump_weapon_geometry.py` mesh list is stale vs full catalog
- **ID:** P2-S3
- **Severity:** minor
- **Status:** open
- **File:** `Scripts/dump_weapon_geometry.py:12-20`
- **Symptom:** Geometry dump misses MarketplaceBlockout / modern catalog guns used by `PFWeaponCatalog` soft loads — FP pose compute tooling under-covers shipping weapons.
- **Why:** Hardcoded 7-mesh dict from earlier packs only.
- **Fix:** Drive the list from catalog paths (or parse `PFWeaponCatalog.cpp`) so dumps stay complete when guns are added.
- **Confidence:** high
- **Source:** this-pass

### 4. Package scripts only *remind* NetProtocol bump (non-enforcing)
- **ID:** P2-S4
- **Severity:** minor
- **Status:** open
- **File:** `Scripts/Package-Windows.bat:32-38`, `Scripts/Package-Mac.command:33-35`
- **Symptom:** Operator can package and push itch without bumping `PFBuild::NetProtocol`, defeating the version gate (stale clients join silently wrong content).
- **Why:** Scripts `findstr`/`grep` and print a reminder; they do not fail the package or compare against last butler userversion.
- **Fix:** Optional hard fail when `PF_REQUIRE_PROTOCOL_BUMP=1`, or integrate with the itch deploy script that already reads NetProtocol for `--userversion`.
- **Confidence:** high
- **Source:** this-pass

### 5. Arena / team / combat FX materials do not set ISM usage (only build piece does)
- **ID:** P2-S5
- **Severity:** minor
- **Status:** open
- **File:** `Scripts/create_build_material.py:287-290` (correct) vs `Scripts/gen_arena_materials.py` (no ISM flag)
- **Symptom:** If arena shell materials are ever applied to ISMCs without the flag, runtime shader recompiles / flash-vanish (the exact bug the build-piece script documents).
- **Why:** Only `M_PF_BuildPiece` sets `used_with_instanced_static_meshes`. Arena mats are currently for static shell meshes — latent if ISM reuse expands.
- **Fix:** Set the flag on any material known to land on ISMs; document which mats are ISM-safe.
- **Confidence:** med
- **Source:** this-pass

### 6. `retarget_rifle_to_bandit.py` default result file under Scripts/
- **ID:** P2-S6
- **Severity:** nit
- **Status:** open
- **File:** `Scripts/retarget_rifle_to_bandit.py:16-18`
- **Symptom:** Default `Scripts/retarget_result.txt` can be written next to sources (noise / accidental commit).
- **Why:** Default OUT is script-dir relative; overridable via `PF_RETARGET_OUT` (good).
- **Fix:** Default to `Saved/retarget_result.txt` via `unreal.Paths.project_saved_dir()` when available.
- **Confidence:** high
- **Source:** this-pass

### 7. `gen_rifle_material.py` / several scripts lack copyright header consistency
- **ID:** P2-S7
- **Severity:** nit
- **Status:** open
- **File:** `Scripts/gen_rifle_material.py:1`, `Scripts/gen_fx_content.py:1`, `Scripts/dump_anim_inventory.py:1`
- **Symptom:** Style drift only.
- **Why:** Mix of copyrighted vs bare scripts.
- **Fix:** Optional project header on tooling scripts.
- **Confidence:** high
- **Source:** this-pass

### 8. `trim_bandit_textures.py` silent on empty Bandits folder
- **ID:** P2-S8
- **Severity:** nit
- **Status:** open
- **File:** `Scripts/trim_bandit_textures.py:19-38`
- **Symptom:** On a machine without Content/Bandits, script “succeeds” with zeros — operator may think trim applied.
- **Why:** No hard fail if `list_assets` returns empty / folder missing.
- **Fix:** Exit non-zero if zero textures found under `/Game/Bandits`.
- **Confidence:** med
- **Source:** this-pass

### 9. Package-Mac path defaults assume one Mac layout
- **ID:** P2-S9
- **Severity:** nit
- **Status:** open
- **File:** `Scripts/Package-Mac.command:22-24`
- **Symptom:** Wrong project path until env overrides set.
- **Why:** Defaults `PROJ=$HOME/projects/combatforge/...` (env override exists — good).
- **Fix:** Prefer discovering uproject relative to script dir (Windows bat already uses `%~dp0..`).
- **Confidence:** high
- **Source:** this-pass

## Subsystems audited clean

| Area | Notes |
|------|--------|
| **Package-Windows.bat** | UE env override; Shipping → `-distribution`; privacy scrub of Saved + pdb + dirty warn; UAT flags include pak/iostore/compressed/prereqs. |
| **Package-Mac.command** | Mirrors scrub (Saved/pdb/dSYM); `set -e`; env overrides for UE/PROJ/OUT. |
| **create_build_material.py** | Color contract; ISM flag; dry-run; no wipe-in-place; WAT pin fallbacks. |
| **gen_glass_fade_material.py** | Opacity-from-scalar-only fix documented; translucent unlit two-sided. |
| **gen_splat_decal_material.py** | Custom HLSL crater; Color contract; DecalLifetimeOpacity fade; delete-recreate. |
| **gen_combat_fx.py** | Smoke volume CoD-style edge kill; Density/SmokeColor contracts. |
| **add_nav_bounds.py** | Idempotent; empty-brush abort avoids inert map save. |
| **add_compatible_skeleton.py** | Safe append + save; missing source soft-warn. |
| **retarget_rifle_to_bandit.py** | Solid IK batch path; result flush on failure. |
| **dump_anim_inventory.py** | Registry wait; Saved output path. |
| **MATERIAL_SETUP.md / CHARACTER_WEAPON_SETUP.md** | Docs only — not line-reviewed as code. |

## File checklist

| File | Reviewed |
|------|----------|
| `Scripts/add_compatible_skeleton.py` | yes |
| `Scripts/add_nav_bounds.py` | yes |
| `Scripts/create_build_material.py` | yes |
| `Scripts/dump_anim_inventory.py` | yes |
| `Scripts/dump_weapon_geometry.py` | yes |
| `Scripts/gen_arena_materials.py` | yes |
| `Scripts/gen_combat_fx.py` | yes |
| `Scripts/gen_fx_content.py` | yes |
| `Scripts/gen_glass_fade_material.py` | yes |
| `Scripts/gen_rifle_material.py` | yes |
| `Scripts/gen_splat_decal_material.py` | yes |
| `Scripts/gen_team_material.py` | yes |
| `Scripts/make_roofpanel_nonvt.py` | yes |
| `Scripts/Package-Mac.command` | yes |
| `Scripts/Package-Windows.bat` | yes |
| `Scripts/retarget_rifle_to_bandit.py` | yes |
| `Scripts/trim_bandit_textures.py` | yes |
| `Scripts/CHARACTER_WEAPON_SETUP.md` | skim (docs) |
| `Scripts/MATERIAL_SETUP.md` | skim (docs) |
