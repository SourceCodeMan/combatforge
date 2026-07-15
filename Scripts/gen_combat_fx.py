# Generates combat juice materials (mesh-based, no Niagara):
#   /Game/Materials/M_PF_Flash        — hot unlit muzzle flash core
#   /Game/Materials/M_PF_MuzzleSmoke  — soft gray smoke wisp (grenade-cloud fallback; muzzle smoke itself is gone)
#   /Game/Materials/M_PF_ImpactDust   — neutral grey dust puff
#   /Game/Materials/M_PF_SmokeVolume  — unlit translucent smoke volume (params: SmokeColor, Density)
# Unlit params: EmissiveColor (vector), EmissiveStrength (scalar).
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
    # UNLIT translucent smoke (the lit version read as flat soap bubbles under the locked-exposure rig):
    # emissive is the SmokeColor param straight through; opacity = Density x (1 - Fresnel, thins the sphere
    # silhouette at grazing angles) x DepthFade (soft floor/wall/pawn contact) x procedural world-space noise
    # (MaterialExpressionNoise on Absolute World Position + a small Time pan so the cloud billows — headless-safe,
    # no texture dependency). Param names SmokeColor + Density are a C++ contract
    # (PFGrenadeProjectile::StartSmokeVisual / Tick) — do not rename.
    path = "/Game/Materials"
    full = path + "/" + name
    if unreal.EditorAssetLibrary.does_asset_exist(full):
        unreal.EditorAssetLibrary.delete_asset(full)
        log("deleted " + full)
    mat = tools.create_asset(name, path, unreal.Material, unreal.MaterialFactoryNew())
    trySet(mat, "blend_mode", unreal.BlendMode.BLEND_TRANSLUCENT)
    trySet(mat, "shading_model", unreal.MaterialShadingModel.MSM_UNLIT)
    trySet(mat, "two_sided", True)
    trySet(mat, "dither_opacity_mask", False)

    color = MEL.create_material_expression(mat, unreal.MaterialExpressionVectorParameter, -640, -120)
    color.set_editor_property("parameter_name", "SmokeColor")
    color.set_editor_property("default_value", unreal.LinearColor(0.60, 0.60, 0.62, 1.0))
    MEL.connect_material_property(color, "", MP.MP_EMISSIVE_COLOR)

    density = MEL.create_material_expression(mat, unreal.MaterialExpressionScalarParameter, -1150, 160)
    density.set_editor_property("parameter_name", "Density")
    density.set_editor_property("default_value", 0.85)

    fres = MEL.create_material_expression(mat, unreal.MaterialExpressionFresnel, -1150, 300)
    trySet(fres, "exponent", 2.6)
    trySet(fres, "base_reflect_fraction", 0.05)
    inv = MEL.create_material_expression(mat, unreal.MaterialExpressionOneMinus, -950, 300)
    MEL.connect_material_expressions(fres, "", inv, "")

    dfade = MEL.create_material_expression(mat, unreal.MaterialExpressionDepthFade, -1150, 440)
    trySet(dfade, "fade_distance_default", 70.0)   # was 220 — tighter contact soften, less "bubble on the floor"

    # Procedural billow: world-space noise slowly panned by Time so the cloud crawls even on parked puffs.
    wpos = MEL.create_material_expression(mat, unreal.MaterialExpressionWorldPosition, -1400, 620)
    tim = MEL.create_material_expression(mat, unreal.MaterialExpressionTime, -1400, 780)
    pan = MEL.create_material_expression(mat, unreal.MaterialExpressionConstant3Vector, -1400, 880)
    trySet(pan, "constant", unreal.LinearColor(9.0, 6.0, 14.0, 0.0))   # uu/s drift of the noise field
    panofs = MEL.create_material_expression(mat, unreal.MaterialExpressionMultiply, -1200, 800)
    MEL.connect_material_expressions(tim, "", panofs, "A")
    MEL.connect_material_expressions(pan, "", panofs, "B")
    noisepos = MEL.create_material_expression(mat, unreal.MaterialExpressionAdd, -1050, 660)
    MEL.connect_material_expressions(wpos, "", noisepos, "A")
    MEL.connect_material_expressions(panofs, "", noisepos, "B")
    noise = MEL.create_material_expression(mat, unreal.MaterialExpressionNoise, -880, 640)
    trySet(noise, "scale", 0.006)
    trySet(noise, "levels", 3)
    trySet(noise, "turbulence", True)
    trySet(noise, "output_min", 0.30)
    trySet(noise, "output_max", 1.0)
    MEL.connect_material_expressions(noisepos, "", noise, "Position")

    m1 = MEL.create_material_expression(mat, unreal.MaterialExpressionMultiply, -760, 240)
    MEL.connect_material_expressions(density, "", m1, "A")
    MEL.connect_material_expressions(inv, "", m1, "B")
    m2 = MEL.create_material_expression(mat, unreal.MaterialExpressionMultiply, -560, 300)
    MEL.connect_material_expressions(m1, "", m2, "A")
    MEL.connect_material_expressions(dfade, "", m2, "B")
    m3 = MEL.create_material_expression(mat, unreal.MaterialExpressionMultiply, -360, 380)
    MEL.connect_material_expressions(m2, "", m3, "A")
    MEL.connect_material_expressions(noise, "", m3, "B")
    MEL.connect_material_property(m3, "", MP.MP_OPACITY)

    MEL.recompile_material(mat)
    unreal.EditorAssetLibrary.save_asset(full)
    log("saved " + full)

make_unlit("M_PF_Flash", (8.0, 4.2, 1.2, 1.0), 1.6)
make_unlit("M_PF_MuzzleSmoke", (0.55, 0.58, 0.62, 1.0), 0.55)
make_unlit("M_PF_ImpactDust", (0.42, 0.42, 0.44, 1.0), 0.9)   # neutral concrete grey (runtime overrides tint)
make_smoke_volume("M_PF_SmokeVolume")
log("combat FX materials done")
