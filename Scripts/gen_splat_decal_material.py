# Generates /Game/Materials/M_PF_PaintSplatDecal: Deferred-Decal translucent impact mark
# for the client-only splat pool (B10 / contract §3.4).
#
# Fiction: airsoft BB / pellet strike — small dark scuff / pockmark on concrete, NOT paint.
# Team identity is a faint tint only (Color contract still exact):
#   C++ confirmed = ForTeam(team), pending = ForTeam(team)*0.6
#   BaseColor  = charcoal scuff + Color * 0.20
#   Emissive   = Color * 0.08 * mask
#   Opacity    = Custom HLSL crater (lobed pockmark + dust flecks) * DecalLifetimeOpacity
#
# Opacity uses MaterialExpressionCustom (not Distance/RadialGradient) so the silhouette is
# reliable — the prior graph produced solid rectangles when pin wiring failed silently.
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

def connect(a, aout, b, bin_):
    ok = MEL.connect_material_expressions(a, aout, b, bin_)
    if not ok:
        unreal.log_warning("[PFSplatDecal] connect failed: %s -> %s.%s" % (a.get_name(), b.get_name(), bin_))
    return ok

# M_PF_ImpactMark = airsoft scuff (preferred). Fall back to legacy M_PF_PaintSplatDecal name
# only if ImpactMark cannot be created (keeps one load path in C++).
NAME = "M_PF_ImpactMark"
PATH = "/Game/Materials"
FULL = PATH + "/" + NAME

if unreal.EditorAssetLibrary.does_asset_exist(FULL):
    mat = unreal.EditorAssetLibrary.load_asset(FULL)
    MEL.delete_all_material_expressions(mat)
    log("rebuilding existing " + FULL)
else:
    mat = tools.create_asset(NAME, PATH, unreal.Material, unreal.MaterialFactoryNew())
    log("creating " + FULL)

trySet(mat, "material_domain", unreal.MaterialDomain.MD_DEFERRED_DECAL)
trySet(mat, "blend_mode", unreal.BlendMode.BLEND_TRANSLUCENT)

# ---- Color vector param (HARD contract — name must stay exactly "Color") ----
col = MEL.create_material_expression(mat, unreal.MaterialExpressionVectorParameter, -900, -40)
col.set_editor_property("parameter_name", "Color")
col.set_editor_property("default_value", unreal.LinearColor(0.05, 0.35, 1.0, 1.0))

# ---- UV ----
tc = MEL.create_material_expression(mat, unreal.MaterialExpressionTextureCoordinate, -1100, 320)

# Per-instance variety via a scalar param default (MID can leave it); random-ish from UV hash inside custom.
# Seed input is constant 0 — custom hashes UV for flecks; world jitter is from DecalSize aspect in C++.

# ---- Custom airsoft crater mask (Float1) ----
# Inputs: UV (float2)
mask = MEL.create_material_expression(mat, unreal.MaterialExpressionCustom, -700, 320)
trySet(mask, "description", "AirsoftImpactMask")
trySet(mask, "output_type", unreal.CustomMaterialOutputType.CMOT_FLOAT1)

# Configure named inputs before connecting
inp_uv = unreal.CustomInput()
inp_uv.set_editor_property("input_name", "UV")
trySet(mask, "inputs", [inp_uv])

trySet(mask, "code", r"""
// UV-space airsoft BB pockmark: lobed crater + fine dust flecks. Hard zero outside radius
// so the decal box never reads as a rectangle.
float2 p = UV - 0.5;
float d = length(p);
float a = atan2(p.y, p.x);

// Irregular lobed edge (scrape / partial transfer), not a paint blob
float lobes = 0.07 * sin(a * 5.0) + 0.04 * sin(a * 13.0 + 1.7);
float radius = 0.36 + lobes;

// Soft outer dust + hard-ish core
float outer = saturate((radius - d) * 16.0);
outer = outer * outer;
float core = saturate((0.10 - d) * 28.0);

// Dust flecks around the strike (hash)
float2 q = UV * 22.0;
float fleck = frac(sin(dot(q, float2(12.9898, 78.233))) * 43758.5453);
float dustRing = saturate(1.0 - abs(d - 0.22) * 8.0);
float dust = smoothstep(0.78, 0.98, fleck) * dustRing * 0.55;

// Micro-scuff streaks (cheap anisotropic noise along one axis)
float streak = saturate(1.0 - abs(p.y) * 6.0) * saturate(1.0 - abs(p.x) * 1.8);
streak *= 0.25 * smoothstep(0.7, 1.0, frac(sin(p.x * 40.0) * 91.7));

float m = max(max(outer, core), dust + streak * outer);
// Kill anything near the box edge (safety margin against rectangular bleed)
m *= saturate((0.48 - d) * 40.0);
return saturate(m);
""")

connect(tc, "", mask, "UV")

# Lifetime fade for SetFadeOut
life = MEL.create_material_expression(mat, unreal.MaterialExpressionDecalLifetimeOpacity, -400, 520)
op = MEL.create_material_expression(mat, unreal.MaterialExpressionMultiply, -200, 400)
connect(mask, "", op, "A")
connect(life, "", op, "B")
MEL.connect_material_property(op, "", MP.MP_OPACITY)

# ---- BaseColor: charcoal scuff + faint team dust ----
scuff = MEL.create_material_expression(mat, unreal.MaterialExpressionConstant3Vector, -900, 80)
trySet(scuff, "constant", unreal.LinearColor(0.04, 0.038, 0.035, 1.0))

team_dust = MEL.create_material_expression(mat, unreal.MaterialExpressionMultiply, -650, 0)
trySet(team_dust, "const_b", 0.20)
connect(col, "", team_dust, "A")

albedo = MEL.create_material_expression(mat, unreal.MaterialExpressionAdd, -420, 40)
connect(scuff, "", albedo, "A")
connect(team_dust, "", albedo, "B")

# Slightly lift the core with the mask so the strike point reads
lit = MEL.create_material_expression(mat, unreal.MaterialExpressionMultiply, -200, 80)
connect(albedo, "", lit, "A")
# core brightness: 0.55 + 0.45 * mask
mscale = MEL.create_material_expression(mat, unreal.MaterialExpressionMultiply, -420, 200)
trySet(mscale, "const_b", 0.45)
connect(mask, "", mscale, "A")
mscale2 = MEL.create_material_expression(mat, unreal.MaterialExpressionAdd, -240, 200)
trySet(mscale2, "const_b", 0.55)
connect(mscale, "", mscale2, "A")
connect(mscale2, "", lit, "B")
MEL.connect_material_property(lit, "", MP.MP_BASE_COLOR)

# ---- Emissive: low team glow for readability at range ----
emul = MEL.create_material_expression(mat, unreal.MaterialExpressionMultiply, -650, -200)
trySet(emul, "const_b", 0.08)
connect(col, "", emul, "A")
emask = MEL.create_material_expression(mat, unreal.MaterialExpressionMultiply, -400, -200)
connect(emul, "", emask, "A")
connect(mask, "", emask, "B")
MEL.connect_material_property(emask, "", MP.MP_EMISSIVE_COLOR)

# Matte scuff on concrete
rough = MEL.create_material_expression(mat, unreal.MaterialExpressionConstant, -200, -320)
trySet(rough, "r", 0.93)
MEL.connect_material_property(rough, "", MP.MP_ROUGHNESS)

MEL.recompile_material(mat)
unreal.EditorAssetLibrary.save_asset(FULL)
log("saved " + FULL)
