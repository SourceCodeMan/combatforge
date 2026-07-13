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

def make_smoke_volume(name):
    # Translucent, soft-edged smoke that reads as a volume (not a hard opaque ball): lit base color + opacity
    # driven by an inverted Fresnel (thins the sphere silhouette to nothing at grazing angles) times a DepthFade
    # (soft where it meets the floor/walls/pawns) times a Density knob. Params: SmokeColor, Density.
    path = "/Game/Materials"
    full = path + "/" + name
    if unreal.EditorAssetLibrary.does_asset_exist(full):
        unreal.EditorAssetLibrary.delete_asset(full)
        log("deleted " + full)
    mat = tools.create_asset(name, path, unreal.Material, unreal.MaterialFactoryNew())
    trySet(mat, "blend_mode", unreal.BlendMode.BLEND_TRANSLUCENT)
    trySet(mat, "shading_model", unreal.MaterialShadingModel.MSM_DEFAULT_LIT)
    trySet(mat, "two_sided", True)
    trySet(mat, "translucency_lighting_mode", unreal.TranslucencyLightingMode.TLM_SURFACE)
    trySet(mat, "dither_opacity_mask", False)

    color = MEL.create_material_expression(mat, unreal.MaterialExpressionVectorParameter, -640, -120)
    color.set_editor_property("parameter_name", "SmokeColor")
    color.set_editor_property("default_value", unreal.LinearColor(0.60, 0.60, 0.62, 1.0))
    MEL.connect_material_property(color, "", MP.MP_BASE_COLOR)

    density = MEL.create_material_expression(mat, unreal.MaterialExpressionScalarParameter, -900, 240)
    density.set_editor_property("parameter_name", "Density")
    density.set_editor_property("default_value", 0.85)

    fres = MEL.create_material_expression(mat, unreal.MaterialExpressionFresnel, -900, 380)
    trySet(fres, "exponent", 2.6)
    trySet(fres, "base_reflect_fraction", 0.05)
    inv = MEL.create_material_expression(mat, unreal.MaterialExpressionOneMinus, -700, 380)
    MEL.connect_material_expressions(fres, "", inv, "")

    dfade = MEL.create_material_expression(mat, unreal.MaterialExpressionDepthFade, -900, 520)
    trySet(dfade, "fade_distance_default", 220.0)

    m1 = MEL.create_material_expression(mat, unreal.MaterialExpressionMultiply, -520, 300)
    MEL.connect_material_expressions(density, "", m1, "A")
    MEL.connect_material_expressions(inv, "", m1, "B")
    m2 = MEL.create_material_expression(mat, unreal.MaterialExpressionMultiply, -320, 360)
    MEL.connect_material_expressions(m1, "", m2, "A")
    MEL.connect_material_expressions(dfade, "", m2, "B")
    MEL.connect_material_property(m2, "", MP.MP_OPACITY)

    MEL.recompile_material(mat)
    unreal.EditorAssetLibrary.save_asset(full)
    log("saved " + full)

make_unlit("M_PF_Flash", (8.0, 4.2, 1.2, 1.0), 1.6)
make_unlit("M_PF_MuzzleSmoke", (0.55, 0.58, 0.62, 1.0), 0.55)
make_unlit("M_PF_ImpactDust", (0.58, 0.48, 0.34, 1.0), 0.9)
make_smoke_volume("M_PF_SmokeVolume")
log("combat FX materials done")
