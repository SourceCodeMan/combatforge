# Registers the AnimStarterPack skeleton (HeroTPP) as COMPATIBLE with the Bandit skeleton, so the
# rifle-hold locomotion set (Idle_Rifle_Hip / Jog_Fwd_Rifle / Sprint_Fwd_Rifle) can play on the modular
# Bandit characters (engine remaps bones by name at runtime — both packs use Mannequin-family bone names).
#
# Run (editor closed):
#   "C:\Program Files\Epic Games\UE_5.6\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" ^
#       D:\projects\combatforge\CombatForge.uproject -run=pythonscript ^
#       -script=D:/projects/combatforge/Scripts/add_compatible_skeleton.py -stdout -unattended -nosplash
#
# NOTE: this saves Content/Bandits/Mesh/SKM_Bandit_Skeleton.uasset — a LOCAL-ONLY asset (Content/Bandits is
# never committed). The edit only needs to happen once per machine that cooks/plays from this content.

import unreal

BANDIT_PATH = "/Game/Bandits/Mesh/SKM_Bandit_Skeleton"
SOURCES = [
    "/Game/AnimStarterPack/Character/HeroTPP_Skeleton",       # UE4 mannequin: sprint/crouch fills
    "/Game/Characters/Mannequins/Meshes/SK_Mannequin",        # UE5 Manny: the full rifle kit (primary)
    "/Game/RifleAnims/ShowcaseAssets/Meshes/Mannequin/UE4_Mannequin_Skeleton",  # Shooter Rifle Animations pack: relaxed LOW-READY idle (fixes gun-at-face)
]

bandit = unreal.load_asset(BANDIT_PATH)
if bandit is None:
    raise SystemExit("PFCOMPAT FAIL: could not load %s" % BANDIT_PATH)

compat = list(bandit.get_editor_property("compatible_skeletons"))
existing = [c.get_path_name() for c in compat if c is not None]
dirty = False
for path in SOURCES:
    src = unreal.load_asset(path)
    if src is None:
        print("PFCOMPAT WARN: missing %s" % path)
        continue
    if src.get_path_name() in existing:
        print("PFCOMPAT OK: already registered %s" % path)
        continue
    compat.append(src)
    existing.append(src.get_path_name())
    dirty = True
    print("PFCOMPAT OK: added %s" % path)
if dirty:
    bandit.set_editor_property("compatible_skeletons", compat)
    saved = unreal.EditorAssetLibrary.save_asset(BANDIT_PATH)
    print("PFCOMPAT OK: saved=%s" % saved)

# Confirm what's on the asset now (grep for PFCOMPAT in the log).
final = [c.get_path_name() for c in bandit.get_editor_property("compatible_skeletons") if c is not None]
print("PFCOMPAT LIST: " + (", ".join(final) if final else "<empty>"))
