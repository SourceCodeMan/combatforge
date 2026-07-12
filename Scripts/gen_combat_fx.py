# Generates combat juice materials (mesh-based, no Niagara):
#   /Game/Materials/M_PF_Flash        — hot unlit muzzle flash core
#   /Game/Materials/M_PF_MuzzleSmoke  — soft gray smoke wisp
#   /Game/Materials/M_PF_ImpactDust   — tan dust puff
# Params: EmissiveColor (vector), EmissiveStrength (scalar).
#
# Run headless:
#   UnrealEditor-Cmd.exe "<uproject>" -run=pythonscript -script="<abs>" -unattended -nosplash -nopause -stdout
import unreal

MEL = unreal.MaterialEditingLibrary
MP = unreal.MaterialProperty
tools = unreal.AssetToolsHelpers.get_asset_tools()

def log(m):
    unreal.log("[PFCombatFX] " + m)

def trySet(o, p, v):
    try:
        o.set_editor_property(p, v)
        return True
    except Exception as e:
        unreal.log_warning("[PFCombatFX] set %s: %s" % (p, e))
        return False

def make_unlit(name, color_rgba, strength):
    path = "/Game/Materials"
    full = path + "/" + name
    if unreal.EditorAssetLibrary.does_asset_exist(full):
        unreal.EditorAssetLibrary.delete_asset(full)
        log("deleted " + full)
    mat = tools.create_asset(name, path, unreal.Material, unreal.MaterialFactoryNew())
    trySet(mat, "shading_model", unreal.MaterialShadingModel.MSM_UNLIT)

    col = MEL.create_material_expression(mat, unreal.MaterialExpressionVectorParameter, -500, 0)
    col.set_editor_property("parameter_name", "EmissiveColor")
    col.set_editor_property("default_value", unreal.LinearColor(
        color_rgba[0], color_rgba[1], color_rgba[2], color_rgba[3]))

    strg = MEL.create_material_expression(mat, unreal.MaterialExpressionScalarParameter, -500, 200)
    strg.set_editor_property("parameter_name", "EmissiveStrength")
    strg.set_editor_property("default_value", float(strength))

    mul = MEL.create_material_expression(mat, unreal.MaterialExpressionMultiply, -160, 0)
    MEL.connect_material_expressions(col, "", mul, "A")
    MEL.connect_material_expressions(strg, "", mul, "B")
    MEL.connect_material_property(mul, "", MP.MP_EMISSIVE_COLOR)

    MEL.recompile_material(mat)
    unreal.EditorAssetLibrary.save_asset(full)
    log("saved " + full)

make_unlit("M_PF_Flash", (8.0, 4.2, 1.2, 1.0), 1.6)
make_unlit("M_PF_MuzzleSmoke", (0.55, 0.58, 0.62, 1.0), 0.55)
make_unlit("M_PF_ImpactDust", (0.58, 0.48, 0.34, 1.0), 0.9)
log("combat FX materials done")
