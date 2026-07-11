# Generates /Game/Materials/M_PF_Flash: an unlit emissive material for muzzle-flash blobs.
# Params (driven per-instance from C++): EmissiveColor (vector), EmissiveStrength (scalar).
# Run headless:
#   UnrealEditor-Cmd.exe "<uproject>" -run=pythonscript -script="<abs>" -unattended -nosplash -nopause -stdout
import unreal

MEL = unreal.MaterialEditingLibrary
MP = unreal.MaterialProperty
tools = unreal.AssetToolsHelpers.get_asset_tools()

def log(m): unreal.log("[PFFX] " + m)

PATH = "/Game/Materials"
NAME = "M_PF_Flash"
FULL = PATH + "/" + NAME

if unreal.EditorAssetLibrary.does_asset_exist(FULL):
    unreal.EditorAssetLibrary.delete_asset(FULL)

mat = tools.create_asset(NAME, PATH, unreal.Material, unreal.MaterialFactoryNew())
try:
    mat.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_UNLIT)
    log("shading model = Unlit")
except Exception as e:
    unreal.log_warning("[PFFX] could not set unlit: %s" % e)

col = MEL.create_material_expression(mat, unreal.MaterialExpressionVectorParameter, -500, 0)
col.set_editor_property("parameter_name", "EmissiveColor")
col.set_editor_property("default_value", unreal.LinearColor(6.0, 3.4, 1.1, 1.0))

strg = MEL.create_material_expression(mat, unreal.MaterialExpressionScalarParameter, -500, 220)
strg.set_editor_property("parameter_name", "EmissiveStrength")
strg.set_editor_property("default_value", 1.0)

mul = MEL.create_material_expression(mat, unreal.MaterialExpressionMultiply, -180, 0)
MEL.connect_material_expressions(col, "", mul, "A")
MEL.connect_material_expressions(strg, "", mul, "B")
MEL.connect_material_property(mul, "", MP.MP_EMISSIVE_COLOR)

MEL.recompile_material(mat)
unreal.EditorAssetLibrary.save_asset(FULL)
log("created " + FULL)
