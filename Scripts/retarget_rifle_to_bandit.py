# Retarget the "Shooter Rifle Animations" low-ready set from its UE4-mannequin skeleton onto the
# Bandit skeleton (SKM_Body / SKM_Bandit_Skeleton), baking new "_Bandit" AnimSequences that play
# WITHOUT the compatible-skeleton name-remap stretch. The pack low-ready pose is already correct
# (weapon sits ~25uu below the head = chest/low-ready); the ONLY defect is the proportion stretch,
# which proper IK retargeting removes.
#
# Run with the EDITOR CLOSED:
#   "C:\Program Files\Epic Games\UE_5.6\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" ^
#       D:\projects\combatforge\CombatForge.uproject -run=pythonscript ^
#       -script=D:/projects/combatforge/Scripts/retarget_rifle_to_bandit.py ^
#       -stdout -unattended -nosplash -nullrhi -NoLogTimes
#
# Results (success/failure of every step) are written to the OUT file so headless print buffering
# can't hide them. If any auto-generation call is missing on this 5.6 build, the log says which one
# and we fall back to the in-editor recipe in docs/retarget-rifle-anims.md.

import unreal

OUT = "C:/Users/tomch/AppData/Local/Temp/claude/C--Users-tomch/47a812dc-c31d-41e1-bc01-d5e949eabc20/scratchpad/retarget_result.txt"
L = []
def log(m):
    L.append(str(m))
    unreal.log("RETARGET: " + str(m))

SRC_MESH = "/Game/RifleAnims/ShowcaseAssets/Meshes/Mannequin/SK_Mannequin"   # pack UE4 mannequin (anim source)
TGT_MESH = "/Game/Bandits/Mesh/Body/SKM_Body"                                # Bandit body (retarget target)
ANIM_DIR = "/Game/RifleAnims/Animations/BlendSpaces/Standing_IdleWalkJogRun"
OUT_DIR  = "/Game/RifleAnims/Animations/Bandit_Retargeted"
SUFFIX   = "_Bandit"

ANIMS = [
    "AS_Rifle_Idle", "AS_Rifle_RunFwd",
    "AS_Rifle_WalkFwd", "AS_Rifle_WalkRight", "AS_Rifle_WalkBwdRight", "AS_Rifle_WalkBwd",
    "AS_Rifle_WalkBwdLeft", "AS_Rifle_WalkLeft",
    "AS_Rifle_JogFwd", "AS_Rifle_JogRight", "AS_Rifle_JogBwdRight", "AS_Rifle_JogBwd",
    "AS_Rifle_JogBwdLeft", "AS_Rifle_JogLeft",
]

adv = unreal.AssetToolsHelpers.get_asset_tools()
eal = unreal.EditorAssetLibrary

def load(p):
    a = unreal.load_asset(p)
    log("load %s -> %s" % (p, "OK" if a else "FAIL"))
    return a

src_mesh = load(SRC_MESH)
tgt_mesh = load(TGT_MESH)

# --- helper: create an IK Rig from a skeletal mesh with auto-generated biped chains ---
def make_rig(name, mesh):
    path = OUT_DIR + "/" + name
    if eal.does_asset_exist(path):
        eal.delete_asset(path)
    factory = unreal.IKRigDefinitionFactory()
    rig = adv.create_asset(name, OUT_DIR, unreal.IKRigDefinition, factory)
    if rig is None:
        log("IKRig create FAIL for %s" % name); return None
    ctrl = unreal.IKRigController.get_controller(rig)
    ctrl.set_skeletal_mesh(mesh)
    # Auto-generate retarget chains + goals (name varies across 5.x — try the known variants).
    for meth in ("auto_generate_retarget_definition", "add_retarget_chains_to_ik_rig",
                 "auto_generate_retarget_chains"):
        if hasattr(ctrl, meth):
            try:
                getattr(ctrl, meth)()
                log("IKRig %s: %s() OK" % (name, meth))
                break
            except Exception as e:
                log("IKRig %s: %s() raised %s" % (name, meth, e))
    else:
        log("IKRig %s: NO auto-chain method found (methods: %s)" %
            (name, [m for m in dir(ctrl) if 'chain' in m.lower() or 'auto' in m.lower()]))
    # Ensure a retarget root (pelvis) is set.
    for meth in ("set_retarget_root", "set_retarget_root_bone"):
        if hasattr(ctrl, meth):
            try: getattr(ctrl, meth)("pelvis"); log("IKRig %s: root=pelvis via %s" % (name, meth)); break
            except Exception as e: log("IKRig %s: %s(pelvis) raised %s" % (name, meth, e))
    eal.save_asset(path)
    return rig

src_rig = make_rig("IK_RifleSrc", src_mesh) if src_mesh else None
tgt_rig = make_rig("IK_Bandit", tgt_mesh) if tgt_mesh else None

rtg = None
if src_rig and tgt_rig:
    rpath = OUT_DIR + "/RTG_Rifle_To_Bandit"
    if eal.does_asset_exist(rpath):
        eal.delete_asset(rpath)
    rfac = unreal.IKRetargeterFactory()
    rtg = adv.create_asset("RTG_Rifle_To_Bandit", OUT_DIR, unreal.IKRetargeter, rfac)
    if rtg:
        rc = unreal.IKRetargeterController.get_controller(rtg)
        rc.set_ik_rig(unreal.RetargetSourceOrTarget.SOURCE, src_rig)
        rc.set_ik_rig(unreal.RetargetSourceOrTarget.TARGET, tgt_rig)
        for meth in ("auto_map_chains", "auto_align_all_bones"):
            if hasattr(rc, meth):
                try: getattr(rc, meth)(unreal.AutoMapChainType.EXACT, True) if meth=="auto_map_chains" else getattr(rc, meth)()
                except Exception as e: log("retargeter %s raised %s" % (meth, e))
        eal.save_asset(rpath)
        log("retargeter created + rigs assigned")

if rtg:
    assets = []
    for a in ANIMS:
        obj = unreal.load_asset(ANIM_DIR + "/" + a)
        if obj: assets.append(obj)
        else: log("anim MISSING %s" % a)
    log("batch retargeting %d anims..." % len(assets))
    try:
        op = unreal.IKRetargetBatchOperation()
        op.duplicate_and_retarget(assets, src_mesh, tgt_mesh, rtg, search=" ", replace=" ", suffix=SUFFIX)
        log("duplicate_and_retarget returned OK")
    except Exception as e:
        log("duplicate_and_retarget raised: %s  (API mismatch -> use editor recipe)" % e)
    eal.save_directory(OUT_DIR, True, True)
    # Report what actually got baked.
    baked = [p for p in eal.list_assets(OUT_DIR, True, False) if p.endswith(SUFFIX) or SUFFIX in p]
    log("BAKED assets in %s: %d" % (OUT_DIR, len(baked)))
    for b in baked: log("   " + b)
else:
    log("Could not build retargeter -> fall back to docs/retarget-rifle-anims.md editor recipe")

with open(OUT, "w") as f:
    f.write("\n".join(L))
unreal.log("RETARGET DONE")
