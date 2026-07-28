import unreal

# PF_DRY_RUN=1: report and exit BEFORE anything destructive. These commandlets delete and
# recreate live content assets and there is no undo in a headless run, so an accidental
# re-run has no preview step without this. Same gate create_build_material.py already had.
# (P2-S1)
import os as _os
if _os.environ.get("PF_DRY_RUN") == "1":
    unreal.log_warning("[PFRoofPanel] DRY RUN: would DELETE and recreate the non-VT /Game/Textures/RoofPanel/T_RoofPanel_* copies. Exiting without changes.")
    raise SystemExit(0)
# Non-VT copies of the painted-roof textures for the TRIPLANAR path (the cone). The originals stay VT
# for the Megascans MIs. /Game/Textures is force-cooked.
PAIRS = [
 ("/Game/Scene_Warehouse/Assets/MS/Surfaces/Ind_War_Roof_Painted_01/T_Ind_War_Roof_Painted_01_D",   "/Game/Textures/RoofPanel/T_RoofPanel_D"),
 ("/Game/Scene_Warehouse/Assets/MS/Surfaces/Ind_War_Roof_Painted_01/T_Ind_War_Roof_Painted_01_N",   "/Game/Textures/RoofPanel/T_RoofPanel_N"),
 ("/Game/Scene_Warehouse/Assets/MS/Surfaces/Ind_War_Roof_Painted_01/T_Ind_War_Roof_Painted_01_ORDp","/Game/Textures/RoofPanel/T_RoofPanel_ORDp"),
]
lib = unreal.EditorAssetLibrary
for src, dst in PAIRS:
    if lib.does_asset_exist(dst):
        lib.delete_asset(dst)
    if not lib.duplicate_asset(src, dst):
        unreal.log_warning("ROOFPANEL: DUPLICATE FAILED %s" % dst); continue
    t = unreal.load_asset(dst)
    t.set_editor_property("virtual_texture_streaming", False)
    lib.save_asset(dst)
    unreal.log_warning("ROOFPANEL: ok %s vt=%s" % (dst, t.get_editor_property("virtual_texture_streaming")))
