# Generates /Game/Materials/M_PF_PaintSplat: a matte, emissive team-paint material for the splat pool.
# Driven from C++ via a vector param named EXACTLY "Color" (confirmed = ForTeam, pending = ForTeam*0.6).
#   BaseColor = Color
#   Emissive  = Color * 0.8   (fresh-paint pop against concrete)
#   Roughness = 0.9 constant  (matte)
# Reuses the existing squashed-disc pool + the "Color" MID contract untouched — just a better look.
# Run headless:
#   UnrealEditor-Cmd.exe "<uproject>" -run=pythonscript -script="<abs>" -unattended -nosplash -nopause -stdout
import unreal

MEL = unreal.MaterialEditingLibrary
MP = unreal.MaterialProperty
tools = unreal.AssetToolsHelpers.get_asset_tools()

def log(m): unreal.log("[PFSplat] " + m)
def trySet(o, p, v):
    try:
        o.set_editor_property(p, v); return True
    except Exception as e:
        unreal.log_warning("[PFSplat] set %s failed: %s" % (p, e)); return False

NAME = "M_PF_PaintSplat"
PATH = "/Game/Materials"
FULL = PATH + "/" + NAME

if unreal.EditorAssetLibrary.does_asset_exist(FULL):
    unreal.EditorAssetLibrary.delete_asset(FULL)

mat = tools.create_asset(NAME, PATH, unreal.Material, unreal.MaterialFactoryNew())
trySet(mat, "two_sided", True)

col = MEL.create_material_expression(mat, unreal.MaterialExpressionVectorParameter, -650, 0)
col.set_editor_property("parameter_name", "Color")
col.set_editor_property("default_value", unreal.LinearColor(0.05, 0.35, 1.0, 1.0))
MEL.connect_material_property(col, "", MP.MP_BASE_COLOR)

# Emissive = Color * 0.8 (fresh-paint pop)
emul = MEL.create_material_expression(mat, unreal.MaterialExpressionMultiply, -300, 220)
trySet(emul, "const_b", 0.8)
MEL.connect_material_expressions(col, "", emul, "A")
MEL.connect_material_property(emul, "", MP.MP_EMISSIVE_COLOR)

# Roughness constant (matte)
rough = MEL.create_material_expression(mat, unreal.MaterialExpressionConstant, -300, 360)
trySet(rough, "r", 0.9)
MEL.connect_material_property(rough, "", MP.MP_ROUGHNESS)

MEL.recompile_material(mat)
unreal.EditorAssetLibrary.save_asset(FULL)
log("created " + FULL)
