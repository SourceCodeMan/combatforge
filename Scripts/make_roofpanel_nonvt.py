import unreal
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
