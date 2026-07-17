# Retarget the "Shooter Rifle Animations" low-ready set from its UE4-mannequin skeleton onto the
# Bandit skeleton (SKM_Body), baking "_Bandit" AnimSequences that play WITHOUT the compatible-
# skeleton name-remap stretch. The pack low-ready pose is already correct (gun ~25uu below the head);
# proper IK retargeting removes the ONLY defect (proportion stretch).
#
# Run EDITOR-CLOSED:
#   UnrealEditor-Cmd.exe D:/projects/combatforge/CombatForge.uproject -run=pythonscript ^
#     -script=D:/projects/combatforge/Scripts/retarget_rifle_to_bandit.py -stdout -unattended -nosplash -nullrhi -NoLogTimes
# Results are written to the OUT file (headless print is unreliable).

import unreal

OUT = "C:/Users/tomch/AppData/Local/Temp/claude/C--Users-tomch/47a812dc-c31d-41e1-bc01-d5e949eabc20/scratchpad/retarget_result.txt"
L = []
def log(m):
    L.append(str(m)); unreal.log("RETARGET: " + str(m))
def flush():
    open(OUT, "w").write("\n".join(L))

try:
    SRC_MESH = "/Game/RifleAnims/ShowcaseAssets/Meshes/Mannequin/SK_Mannequin"
    TGT_MESH = "/Game/Bandits/Mesh/Body/SKM_Body"
    ANIM_DIR = "/Game/RifleAnims/Animations/BlendSpaces/Standing_IdleWalkJogRun"
    OUT_DIR  = "/Game/RifleAnims/Animations/Bandit_Retargeted"
    SUFFIX   = "_Bandit"
    ANIMS = ["AS_Rifle_Idle","AS_Rifle_RunFwd","AS_Rifle_WalkFwd","AS_Rifle_WalkRight","AS_Rifle_WalkBwdRight",
             "AS_Rifle_WalkBwd","AS_Rifle_WalkBwdLeft","AS_Rifle_WalkLeft","AS_Rifle_JogFwd","AS_Rifle_JogRight",
             "AS_Rifle_JogBwdRight","AS_Rifle_JogBwd","AS_Rifle_JogBwdLeft","AS_Rifle_JogLeft"]

    adv = unreal.AssetToolsHelpers.get_asset_tools()
    eal = unreal.EditorAssetLibrary
    src_mesh = unreal.load_asset(SRC_MESH); log("src_mesh %s -> %s" % (SRC_MESH, bool(src_mesh)))
    tgt_mesh = unreal.load_asset(TGT_MESH); log("tgt_mesh %s -> %s" % (TGT_MESH, bool(tgt_mesh)))

    def make_rig(name, mesh):
        path = OUT_DIR + "/" + name
        if eal.does_asset_exist(path):
            eal.delete_asset(path)
        rig = adv.create_asset(name, OUT_DIR, unreal.IKRigDefinition, unreal.IKRigDefinitionFactory())
        ctrl = unreal.IKRigController.get_controller(rig)
        ctrl.set_skeletal_mesh(mesh)
        ok = ctrl.apply_auto_generated_retarget_definition()
        log("%s: apply_auto_generated_retarget_definition -> %s ; root=%s ; chains=%d"
            % (name, ok, ctrl.get_retarget_root(), len(ctrl.get_retarget_chains())))
        eal.save_asset(path)
        return rig

    src_rig = make_rig("IK_RifleSrc", src_mesh)
    tgt_rig = make_rig("IK_Bandit", tgt_mesh)

    rpath = OUT_DIR + "/RTG_Rifle_To_Bandit"
    if eal.does_asset_exist(rpath):
        eal.delete_asset(rpath)
    rtg = adv.create_asset("RTG_Rifle_To_Bandit", OUT_DIR, unreal.IKRetargeter, unreal.IKRetargetFactory())
    rc = unreal.IKRetargeterController.get_controller(rtg)
    rc.set_ik_rig(unreal.RetargetSourceOrTarget.SOURCE, src_rig)
    rc.set_ik_rig(unreal.RetargetSourceOrTarget.TARGET, tgt_rig)
    try:
        rc.add_default_ops(); log("add_default_ops OK")
    except Exception as e:
        log("add_default_ops raised %s" % e)
    rc.auto_map_chains(unreal.AutoMapChainType.EXACT, True)
    log("auto_map_chains(EXACT) OK")
    try:
        rc.auto_align_all_bones(); log("auto_align_all_bones OK")
    except Exception as e:
        log("auto_align_all_bones raised %s (non-fatal)" % e)
    eal.save_asset(rpath)

    # Build the AssetData list the batch op requires.
    asset_datas = []
    for a in ANIMS:
        p = ANIM_DIR + "/" + a
        if eal.does_asset_exist(p):
            asset_datas.append(eal.find_asset_data(p))
        else:
            log("MISSING anim %s" % p)
    log("retargeting %d assets..." % len(asset_datas))

    op = unreal.IKRetargetBatchOperation()
    made = op.duplicate_and_retarget(asset_datas, src_mesh, tgt_mesh, rtg,
                                     search="", replace="", prefix="", suffix=SUFFIX,
                                     include_referenced_assets=False)
    log("duplicate_and_retarget returned %d AssetData" % (len(made) if made else 0))

    # Move the baked anims (they land next to the sources) into OUT_DIR + report.
    baked = []
    for a in ANIMS:
        newp = ANIM_DIR + "/" + a + SUFFIX
        dst  = OUT_DIR + "/" + a + SUFFIX
        if eal.does_asset_exist(newp):
            eal.rename_asset(newp, dst); baked.append(dst)
        elif eal.does_asset_exist(dst):
            baked.append(dst)
    eal.save_directory(OUT_DIR, False, True)
    log("BAKED %d anims:" % len(baked))
    for b in baked: log("   " + b)
    log("RESULT: %s" % ("SUCCESS" if len(baked) == len(ANIMS) else "PARTIAL/CHECK"))
except Exception as e:
    import traceback
    log("FATAL: %s" % e); log(traceback.format_exc())

flush()
unreal.log("RETARGET DONE")
