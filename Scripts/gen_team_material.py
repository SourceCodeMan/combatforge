# Generates /Game/Materials/M_PF_TeamBody: a lit body material for the mannequin,
# driven per-team from C++ via a vector param named EXACTLY "Color" (hard contract).
#   BaseColor  = Color                     (C++ passes a MUTED team color)
#   Emissive   = Fresnel * Color * 0.6     (subtle team-colored rim -> reads at distance)
#   Roughness  = 0.55 constant
# Run headless:
#   UnrealEditor-Cmd.exe "<uproject>" -run=pythonscript -script="<abs>" -unattended -nosplash -nopause -stdout
import unreal

# PF_DRY_RUN=1: report and exit BEFORE anything destructive. These commandlets delete and
# recreate live content assets and there is no undo in a headless run, so an accidental
# re-run has no preview step without this. Same gate create_build_material.py already had.
# (P2-S1)
import os as _os
if _os.environ.get("PF_DRY_RUN") == "1":
    unreal.log_warning("[PFTeam] DRY RUN: would DELETE and recreate /Game/Materials/M_PF_Team. Exiting without changes.")
    raise SystemExit(0)

MEL = unreal.MaterialEditingLibrary
MP = unreal.MaterialProperty
tools = unreal.AssetToolsHelpers.get_asset_tools()

def log(m): unreal.log("[PFTeam] " + m)
def trySet(o, p, v):
    try:
        o.set_editor_property(p, v); return True
    except Exception as e:
        unreal.log_warning("[PFTeam] set %s failed: %s" % (p, e)); return False

NAME = "M_PF_TeamBody"
PATH = "/Game/Materials"
FULL = PATH + "/" + NAME

if unreal.EditorAssetLibrary.does_asset_exist(FULL):
    unreal.EditorAssetLibrary.delete_asset(FULL)

mat = tools.create_asset(NAME, PATH, unreal.Material, unreal.MaterialFactoryNew())

# Color vector param -> BaseColor
col = MEL.create_material_expression(mat, unreal.MaterialExpressionVectorParameter, -650, 0)
col.set_editor_property("parameter_name", "Color")
col.set_editor_property("default_value", unreal.LinearColor(0.18, 0.24, 0.5, 1.0))
MEL.connect_material_property(col, "", MP.MP_BASE_COLOR)

# Fresnel * Color * 0.6 -> Emissive (subtle team rim)
fres = MEL.create_material_expression(mat, unreal.MaterialExpressionFresnel, -650, 260)
trySet(fres, "exponent", 4.0)
emul = MEL.create_material_expression(mat, unreal.MaterialExpressionMultiply, -350, 220)
MEL.connect_material_expressions(fres, "", emul, "A")
MEL.connect_material_expressions(col, "", emul, "B")
emul2 = MEL.create_material_expression(mat, unreal.MaterialExpressionMultiply, -150, 220)
trySet(emul2, "const_b", 0.6)
MEL.connect_material_expressions(emul, "", emul2, "A")
MEL.connect_material_property(emul2, "", MP.MP_EMISSIVE_COLOR)

# Roughness constant
rough = MEL.create_material_expression(mat, unreal.MaterialExpressionConstant, -350, 380)
trySet(rough, "r", 0.55)
MEL.connect_material_property(rough, "", MP.MP_ROUGHNESS)

MEL.recompile_material(mat)
unreal.EditorAssetLibrary.save_asset(FULL)
log("created " + FULL)
