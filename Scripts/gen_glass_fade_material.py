# Generate /Game/Materials/M_PF_GlassFade — the project's own unlit translucent "tinted glass" master.
#
# WHY THIS EXISTS: the midline tint screen and the build ghost both used
# /Engine/EngineDebugMaterials/M_SimpleUnlitTranslucent. Engine DEBUG content is not guaranteed to cook
# into packaged builds, and /Game/Materials IS force-cooked (DirectoriesToAlwaysCook) — so the project
# ships its own equivalent: unlit, translucent, two-sided, with the SAME parameter contract the code
# already drives (Vector "Color" RGB+A, Scalar "Opacity"; final opacity = Color.A * Opacity).
#
# Run EDITOR-CLOSED:
#   & "C:\Program Files\Epic Games\UE_5.6\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" `
#     D:\projects\combatforge\CombatForge.uproject -run=pythonscript `
#     -script=D:/projects/combatforge/Scripts/gen_glass_fade_material.py -stdout -unattended -nosplash -nullrhi
#
# Delete-and-recreate on rerun (never wipe-in-place — the rooted-material crash, see MATERIAL_SETUP.md).

import unreal

PATH = "/Game/Materials"
NAME = "M_PF_GlassFade"
FULL = PATH + "/" + NAME

if unreal.EditorAssetLibrary.does_asset_exist(FULL):
    unreal.EditorAssetLibrary.delete_asset(FULL)
    unreal.log_warning("[M_PF_GlassFade] existing asset deleted; recreating fresh.")

tools = unreal.AssetToolsHelpers.get_asset_tools()
mel = unreal.MaterialEditingLibrary
mat = tools.create_asset(NAME, PATH, unreal.Material, unreal.MaterialFactoryNew())
assert mat is not None, "could not create material"

mat.set_editor_property("blend_mode", unreal.BlendMode.BLEND_TRANSLUCENT)
mat.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_UNLIT)
mat.set_editor_property("two_sided", True)

col = mel.create_material_expression(mat, unreal.MaterialExpressionVectorParameter, -450, -50)
col.set_editor_property("parameter_name", "Color")
col.set_editor_property("default_value", unreal.LinearColor(0.012, 0.017, 0.03, 1.0))  # dark tint, opaque

op = mel.create_material_expression(mat, unreal.MaterialExpressionScalarParameter, -450, 250)
op.set_editor_property("parameter_name", "Opacity")
op.set_editor_property("default_value", 1.0)

mul = mel.create_material_expression(mat, unreal.MaterialExpressionMultiply, -180, 220)
mel.connect_material_expressions(col, "A", mul, "A")   # Color.A ...
mel.connect_material_expressions(op, "", mul, "B")     # ... * Opacity

mel.connect_material_property(col, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)
mel.connect_material_property(mul, "", unreal.MaterialProperty.MP_OPACITY)

mel.recompile_material(mat)
ok = unreal.EditorAssetLibrary.save_asset(FULL)
unreal.log_warning("[M_PF_GlassFade] created + saved: %s (saved=%s)" % (FULL, ok))
