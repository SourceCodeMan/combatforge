# Generates /Game/Materials/M_PF_PaintSplatDecal: a Deferred-Decal-domain translucent
# paint-splatter material for the client-only splat pool (B10 / contract §3.4).
#
# Driven from C++ via a vector param named EXACTLY "Color"
#   confirmed = ForTeam(team), pending = ForTeam(team)*0.6
#   BaseColor = Color
#   Emissive  = Color * 0.5   (modest fresh-paint pop)
#   Opacity   = irregular noise-warped radial mask * DecalLifetimeOpacity
#               (silhouette is not a clean circle; SetFadeOut drives the 0.2s pending fade)
#
# Run headless (editor must be closed / not locking content):
#   UnrealEditor-Cmd.exe "<uproject>" -run=pythonscript -script="<abs>" -unattended -nosplash -nopause -stdout
import unreal

MEL = unreal.MaterialEditingLibrary
MP = unreal.MaterialProperty
tools = unreal.AssetToolsHelpers.get_asset_tools()

def log(m): unreal.log("[PFSplatDecal] " + m)
def trySet(o, p, v):
    try:
        o.set_editor_property(p, v); return True
    except Exception as e:
        unreal.log_warning("[PFSplatDecal] set %s failed: %s" % (p, e)); return False

NAME = "M_PF_PaintSplatDecal"
PATH = "/Game/Materials"
FULL = PATH + "/" + NAME

if unreal.EditorAssetLibrary.does_asset_exist(FULL):
    unreal.EditorAssetLibrary.delete_asset(FULL)

mat = tools.create_asset(NAME, PATH, unreal.Material, unreal.MaterialFactoryNew())

# Deferred Decal + Translucent is required for UDecalComponent projection + alpha silhouette.
trySet(mat, "material_domain", unreal.MaterialDomain.MD_DEFERRED_DECAL)
trySet(mat, "blend_mode", unreal.BlendMode.BLEND_TRANSLUCENT)
log("domain=DeferredDecal blend=Translucent")

# ---- Color vector param (HARD contract — must be named exactly "Color") ----
col = MEL.create_material_expression(mat, unreal.MaterialExpressionVectorParameter, -700, -40)
col.set_editor_property("parameter_name", "Color")
col.set_editor_property("default_value", unreal.LinearColor(0.05, 0.35, 1.0, 1.0))
MEL.connect_material_property(col, "", MP.MP_BASE_COLOR)

# Emissive = Color * 0.5 (modest pop against concrete; not full-bright)
emul = MEL.create_material_expression(mat, unreal.MaterialExpressionMultiply, -350, 160)
trySet(emul, "const_b", 0.5)
MEL.connect_material_expressions(col, "", emul, "A")
MEL.connect_material_property(emul, "", MP.MP_EMISSIVE_COLOR)

# ---- Irregular splat opacity (noise-warped radial falloff, not a clean circle) ----
# UV0 is decal-projected (Y/Z of decal box after engine swizzle).
tc = MEL.create_material_expression(mat, unreal.MaterialExpressionTextureCoordinate, -1100, 380)

# Center UV at (0.5, 0.5)
half = MEL.create_material_expression(mat, unreal.MaterialExpressionConstant2Vector, -1100, 520)
trySet(half, "r", 0.5)
trySet(half, "g", 0.5)

# Offset from center
sub = MEL.create_material_expression(mat, unreal.MaterialExpressionSubtract, -880, 400)
MEL.connect_material_expressions(tc, "", sub, "A")
MEL.connect_material_expressions(half, "", sub, "B")

# Radial distance from center
dist = MEL.create_material_expression(mat, unreal.MaterialExpressionDistance, -680, 400)
zero2 = MEL.create_material_expression(mat, unreal.MaterialExpressionConstant2Vector, -880, 560)
trySet(zero2, "r", 0.0)
trySet(zero2, "g", 0.0)
MEL.connect_material_expressions(sub, "", dist, "A")
MEL.connect_material_expressions(zero2, "", dist, "B")

# Procedural noise to break the silhouette (Position = UV as float3)
append = MEL.create_material_expression(mat, unreal.MaterialExpressionAppendVector, -1100, 700)
MEL.connect_material_expressions(tc, "", append, "A")
zconst = MEL.create_material_expression(mat, unreal.MaterialExpressionConstant, -1100, 820)
trySet(zconst, "r", 0.0)
MEL.connect_material_expressions(zconst, "", append, "B")

noise = MEL.create_material_expression(mat, unreal.MaterialExpressionNoise, -880, 700)
trySet(noise, "scale", 12.0)
trySet(noise, "levels", 3)
trySet(noise, "output_min", -1.0)
trySet(noise, "output_max", 1.0)
MEL.connect_material_expressions(append, "", noise, "Position")

# radius' = 0.48 + noise * 0.18  → irregular edge
nscale = MEL.create_material_expression(mat, unreal.MaterialExpressionMultiply, -680, 700)
trySet(nscale, "const_b", 0.18)
MEL.connect_material_expressions(noise, "", nscale, "A")

rad = MEL.create_material_expression(mat, unreal.MaterialExpressionAdd, -500, 620)
trySet(rad, "const_b", 0.48)
MEL.connect_material_expressions(nscale, "", rad, "A")

# soft edge: saturate((radius - dist) * sharpness)
edge = MEL.create_material_expression(mat, unreal.MaterialExpressionSubtract, -320, 480)
MEL.connect_material_expressions(rad, "", edge, "A")
MEL.connect_material_expressions(dist, "", edge, "B")

sharp = MEL.create_material_expression(mat, unreal.MaterialExpressionMultiply, -140, 480)
trySet(sharp, "const_b", 6.0)
MEL.connect_material_expressions(edge, "", sharp, "A")

sat = MEL.create_material_expression(mat, unreal.MaterialExpressionClamp, 40, 480)
trySet(sat, "min_default", 0.0)
trySet(sat, "max_default", 1.0)
MEL.connect_material_expressions(sharp, "", sat, "Input")

# Optional secondary blob noise for blotchiness inside the mask
n2 = MEL.create_material_expression(mat, unreal.MaterialExpressionNoise, -500, 860)
trySet(n2, "scale", 28.0)
trySet(n2, "levels", 2)
trySet(n2, "output_min", 0.55)
trySet(n2, "output_max", 1.0)
MEL.connect_material_expressions(append, "", n2, "Position")

blotch = MEL.create_material_expression(mat, unreal.MaterialExpressionMultiply, 40, 620)
MEL.connect_material_expressions(sat, "", blotch, "A")
MEL.connect_material_expressions(n2, "", blotch, "B")

# Multiply by DecalLifetimeOpacity so UDecalComponent::SetFadeOut actually fades alpha.
life = MEL.create_material_expression(mat, unreal.MaterialExpressionDecalLifetimeOpacity, 40, 760)

op = MEL.create_material_expression(mat, unreal.MaterialExpressionMultiply, 260, 560)
MEL.connect_material_expressions(blotch, "", op, "A")
MEL.connect_material_expressions(life, "", op, "B")
MEL.connect_material_property(op, "", MP.MP_OPACITY)

MEL.recompile_material(mat)
unreal.EditorAssetLibrary.save_asset(FULL)
log("created " + FULL)
