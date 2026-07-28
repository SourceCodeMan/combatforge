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

# PF_DRY_RUN=1: report and exit BEFORE anything destructive. These commandlets delete and
# recreate live content assets and there is no undo in a headless run, so an accidental
# re-run has no preview step without this. Same gate create_build_material.py already had.
# (P2-S1)
import os as _os
if _os.environ.get("PF_DRY_RUN") == "1":
    unreal.log_warning("[PFCombatFX] DRY RUN: would DELETE and recreate the combat FX materials under /Game/Materials. Exiting without changes.")
    raise SystemExit(0)

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
    # UNLIT translucent smoke — CoD-style bank, NOT anime soap bubbles.
    # The old soft Fresnel silhouette outlined every engine sphere as a hard circle. Fix:
    #   * aggressive edge kill (high-power Fresnel) so sphere rims dissolve instead of outlining
    #   * dual-scale world noise (large billow + fine grain) so density is irregular, not a filled ball
    #   * depth fade softens floor/wall contact
    # Opacity = Density x EdgeSoft x DepthFade x (LargeNoise x FineNoise)
    # Param names SmokeColor + Density are a C++ contract (PFGrenadeProjectile) — do not rename.
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
    # Translucent sort: treat as volume-ish so overlapping puffs blend instead of hard-sorting disks.
    trySet(mat, "translucency_lighting_mode", unreal.TranslucencyLightingMode.TLM_VOLUMETRIC_NON_DIRECTIONAL)

    color = MEL.create_material_expression(mat, unreal.MaterialExpressionVectorParameter, -640, -120)
    color.set_editor_property("parameter_name", "SmokeColor")
    color.set_editor_property("default_value", unreal.LinearColor(0.55, 0.56, 0.58, 1.0))
    # Slightly dim emissive so the bank reads as smoke, not glowing orbs under locked exposure.
    emul = MEL.create_material_expression(mat, unreal.MaterialExpressionMultiply, -360, -120)
    emul_k = MEL.create_material_expression(mat, unreal.MaterialExpressionConstant, -500, -40)
    trySet(emul_k, "r", 0.55)
    MEL.connect_material_expressions(color, "", emul, "A")
    MEL.connect_material_expressions(emul_k, "", emul, "B")
    MEL.connect_material_property(emul, "", MP.MP_EMISSIVE_COLOR)

    density = MEL.create_material_expression(mat, unreal.MaterialExpressionScalarParameter, -1400, 120)
    density.set_editor_property("parameter_name", "Density")
    density.set_editor_property("default_value", 1.0)

    # Edge softener: high exponent → only the extreme rim fades, killing the hard circular outline
    # without hollowing the cloud into a shell (the classic soap-bubble fresnel look).
    fres = MEL.create_material_expression(mat, unreal.MaterialExpressionFresnel, -1400, 280)
    trySet(fres, "exponent", 5.5)
    trySet(fres, "base_reflect_fraction", 0.02)
    # Power the fresnel so edge falloff is steeper, then 1-x so center stays opaque.
    fpow = MEL.create_material_expression(mat, unreal.MaterialExpressionPower, -1180, 280)
    fexp = MEL.create_material_expression(mat, unreal.MaterialExpressionConstant, -1300, 360)
    trySet(fexp, "r", 1.8)
    MEL.connect_material_expressions(fres, "", fpow, "Base")
    MEL.connect_material_expressions(fexp, "", fpow, "Exp")
    inv = MEL.create_material_expression(mat, unreal.MaterialExpressionOneMinus, -1000, 280)
    MEL.connect_material_expressions(fpow, "", inv, "")

    dfade = MEL.create_material_expression(mat, unreal.MaterialExpressionDepthFade, -1400, 460)
    trySet(dfade, "fade_distance_default", 90.0)

    # Dual world-space noise: large slow billow + finer grain. Multiplied so density has holes and
    # clumps — the silhouette stops reading as "filled sphere".
    wpos = MEL.create_material_expression(mat, unreal.MaterialExpressionWorldPosition, -1600, 640)
    tim = MEL.create_material_expression(mat, unreal.MaterialExpressionTime, -1600, 800)

    pan_big = MEL.create_material_expression(mat, unreal.MaterialExpressionConstant3Vector, -1600, 900)
    trySet(pan_big, "constant", unreal.LinearColor(7.0, 4.5, 11.0, 0.0))
    panofs_big = MEL.create_material_expression(mat, unreal.MaterialExpressionMultiply, -1400, 860)
    MEL.connect_material_expressions(tim, "", panofs_big, "A")
    MEL.connect_material_expressions(pan_big, "", panofs_big, "B")
    npos_big = MEL.create_material_expression(mat, unreal.MaterialExpressionAdd, -1220, 700)
    MEL.connect_material_expressions(wpos, "", npos_big, "A")
    MEL.connect_material_expressions(panofs_big, "", npos_big, "B")
    noise_big = MEL.create_material_expression(mat, unreal.MaterialExpressionNoise, -1000, 640)
    trySet(noise_big, "scale", 0.0045)      # large clumps
    trySet(noise_big, "levels", 4)
    trySet(noise_big, "turbulence", True)
    trySet(noise_big, "output_min", 0.15)   # deeper holes so spheres don't fill solid
    trySet(noise_big, "output_max", 1.0)
    MEL.connect_material_expressions(npos_big, "", noise_big, "Position")

    pan_fine = MEL.create_material_expression(mat, unreal.MaterialExpressionConstant3Vector, -1600, 1040)
    trySet(pan_fine, "constant", unreal.LinearColor(-18.0, 12.0, 9.0, 0.0))
    panofs_fine = MEL.create_material_expression(mat, unreal.MaterialExpressionMultiply, -1400, 1000)
    MEL.connect_material_expressions(tim, "", panofs_fine, "A")
    MEL.connect_material_expressions(pan_fine, "", panofs_fine, "B")
    npos_fine = MEL.create_material_expression(mat, unreal.MaterialExpressionAdd, -1220, 900)
    MEL.connect_material_expressions(wpos, "", npos_fine, "A")
    MEL.connect_material_expressions(panofs_fine, "", npos_fine, "B")
    noise_fine = MEL.create_material_expression(mat, unreal.MaterialExpressionNoise, -1000, 860)
    trySet(noise_fine, "scale", 0.018)      # fine grain breaks remaining disk edges
    trySet(noise_fine, "levels", 2)
    trySet(noise_fine, "turbulence", True)
    trySet(noise_fine, "output_min", 0.35)
    trySet(noise_fine, "output_max", 1.0)
    MEL.connect_material_expressions(npos_fine, "", noise_fine, "Position")

    noise_mul = MEL.create_material_expression(mat, unreal.MaterialExpressionMultiply, -780, 760)
    MEL.connect_material_expressions(noise_big, "", noise_mul, "A")
    MEL.connect_material_expressions(noise_fine, "", noise_mul, "B")

    m1 = MEL.create_material_expression(mat, unreal.MaterialExpressionMultiply, -780, 220)
    MEL.connect_material_expressions(density, "", m1, "A")
    MEL.connect_material_expressions(inv, "", m1, "B")
    m2 = MEL.create_material_expression(mat, unreal.MaterialExpressionMultiply, -560, 300)
    MEL.connect_material_expressions(m1, "", m2, "A")
    MEL.connect_material_expressions(dfade, "", m2, "B")
    m3 = MEL.create_material_expression(mat, unreal.MaterialExpressionMultiply, -360, 400)
    MEL.connect_material_expressions(m2, "", m3, "A")
    MEL.connect_material_expressions(noise_mul, "", m3, "B")
    MEL.connect_material_property(m3, "", MP.MP_OPACITY)

    MEL.recompile_material(mat)
    unreal.EditorAssetLibrary.save_asset(full)
    log("saved " + full)

make_unlit("M_PF_Flash", (8.0, 4.2, 1.2, 1.0), 1.6)
make_unlit("M_PF_MuzzleSmoke", (0.55, 0.58, 0.62, 1.0), 0.55)
make_unlit("M_PF_ImpactDust", (0.42, 0.42, 0.44, 1.0), 0.9)   # neutral concrete grey (runtime overrides tint)
make_smoke_volume("M_PF_SmokeVolume")
log("combat FX materials done")
