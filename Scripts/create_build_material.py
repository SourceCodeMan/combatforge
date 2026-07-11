# =============================================================================
#  M_PF_BuildPiece  —  master material generator for "Breachworks" (UE 5.6)
#
#  Creates /Game/Materials/M_PF_BuildPiece:
#    * Concrete BASE via world-aligned / triplanar projection (WorldAlignedTexture)
#      so it does NOT stretch on non-uniformly scaled BasicShape primitives.
#    * TEAM ACCENT as an emissive rim/trim driven by a vector param named EXACTLY
#      "Color" (hard C++ contract: SetVectorParameterValue(TEXT("Color"), ...)).
#    * Fully self-contained: works with NO textures (flat concrete-gray fallback);
#      Megascans BaseColor / Normal / ORD just upgrade the base if you supply paths.
#
#  Run from the editor:  Tools > Execute Python Script  (or py "C:/path/to/file.py")
#  Idempotent: if the asset exists it is wiped clean and rebuilt.
# =============================================================================
import unreal

# --------------------------------------------------------------------------- #
#  CONFIG  —  optional Megascans surface. Leave "" to use the concrete fallback.
#  Paste UE asset paths (NOT disk paths), e.g.
#      "/Game/Megascans/Surfaces/Concrete_xyz/T_xyz_BaseColor"
# --------------------------------------------------------------------------- #
BASECOLOR_TEXTURE  = ""      # sRGB color map            -> BaseColor
NORMAL_TEXTURE     = ""      # tangent-space normal map  -> Normal
ORD_TEXTURE        = ""      # packed map, R=AO G=Rough  -> Roughness / AO
                             #   (adjust channel masks below if your packing differs)

WORLD_TILE_SIZE    = 256.0   # world units per texture tile (scalar param "WorldTileSize")
ACCENT_BOOST       = 2.5     # emissive multiplier          (scalar param "AccentBoost")
FRESNEL_EXPONENT   = 4.0     # rim tightness
TOP_EDGE_THICKNESS = 12.0    # world-units-thick top trim band (scalar param "TopEdgeThickness")

CONCRETE_GRAY      = (0.34, 0.34, 0.35)   # neutral fallback albedo
DEFAULT_ROUGHNESS  = 0.85
DEFAULT_AO         = 1.0
TEAM_COLOR_DEFAULT = (0.10, 0.50, 1.00)   # placeholder only; C++ overrides "Color" per team

ASSET_NAME = "M_PF_BuildPiece"
ASSET_PATH = "/Game/Materials"
FULL_PATH  = ASSET_PATH + "/" + ASSET_NAME

# WorldAlignedTexture pin names vary slightly across engine builds; try best-first.
WAT_TEXOBJ_IN = ["TextureObject", "Texture Object", "Tex"]
WAT_SIZE_IN   = ["WorldSize", "World Size", "Texture Size", "TextureSize", "Size"]
WAT_OUT       = ["XYZ Texture", "XY Texture", ""]   # "" == first/default output (last resort)
BOUNDS_MAX    = ["Full Bounds Max", "Local Space Bounds Max", "Bounds Max", "Half Extents"]

MEL = unreal.MaterialEditingLibrary
MP  = unreal.MaterialProperty

# --------------------------------------------------------------------------- #
#  Connection helpers (raise on hard-required links so failures are visible)
# --------------------------------------------------------------------------- #
def _expr(cls, x, y):
    return MEL.create_material_expression(mat, cls, x, y)

def _link(a, a_out, b, b_in):
    if not MEL.connect_material_expressions(a, a_out, b, b_in):
        raise RuntimeError("link failed: '%s' -> '%s'" % (a_out, b_in))

def _link_in(a, a_out, b, b_in_candidates):
    for bi in b_in_candidates:
        if MEL.connect_material_expressions(a, a_out, b, bi):
            return bi
    raise RuntimeError("no matching input in %s" % (b_in_candidates,))

def _link_out(a, a_out_candidates, b, b_in):
    for ao in a_out_candidates:
        if MEL.connect_material_expressions(a, ao, b, b_in):
            return ao
    raise RuntimeError("no matching output in %s" % (a_out_candidates,))

def _prop(a, a_out, prop):
    if not MEL.connect_material_property(a, a_out, prop):
        raise RuntimeError("property link failed")

def _prop_out(a, a_out_candidates, prop):
    for ao in a_out_candidates:
        if MEL.connect_material_property(a, ao, prop):
            return ao
    raise RuntimeError("no matching output for property in %s" % (a_out_candidates,))

def _load_tex(path, label):
    if not path:
        return None
    a = unreal.load_asset(path)
    if a is None:
        unreal.log_warning("[M_PF_BuildPiece] %s texture not found: %s (using fallback)" % (label, path))
    return a

# --------------------------------------------------------------------------- #
#  Create (or reuse + wipe) the material asset
# --------------------------------------------------------------------------- #
# NOTE: do NOT wipe-and-rebuild in place. delete_all_material_expressions on an existing,
# loaded material can call MarkAsGarbage() on a rooted object -> crash (check(!IsRooted())).
# Always DELETE the asset and create it fresh; the fresh-create path is crash-safe.
if unreal.EditorAssetLibrary.does_asset_exist(FULL_PATH):
    unreal.EditorAssetLibrary.delete_asset(FULL_PATH)
    unreal.log_warning("[M_PF_BuildPiece] existing asset deleted; recreating fresh.")
tools = unreal.AssetToolsHelpers.get_asset_tools()
mat = tools.create_asset(ASSET_NAME, ASSET_PATH, unreal.Material, unreal.MaterialFactoryNew())

assert mat is not None, "Could not create/load material"

# Load the engine triplanar function once (shared by base / normal / ORD).
WAT = unreal.load_asset("/Engine/Functions/Engine_MaterialFunctions01/Texturing/WorldAlignedTexture")
assert WAT is not None, "WorldAlignedTexture material function not found"

# --------------------------------------------------------------------------- #
#  Shared params
# --------------------------------------------------------------------------- #
tile = _expr(unreal.MaterialExpressionScalarParameter, -1600, -200)
tile.set_editor_property("parameter_name", "WorldTileSize")
tile.set_editor_property("default_value", WORLD_TILE_SIZE)
tile.set_editor_property("group", "PF Surface")

# --------------------------------------------------------------------------- #
#  Build a world-aligned (triplanar) sample of a texture object.
#  Returns the MaterialFunctionCall expression (use WAT_OUT for its output).
# --------------------------------------------------------------------------- #
def world_aligned(tex_asset, sampler_type, param_name, x, y):
    tobj = _expr(unreal.MaterialExpressionTextureObjectParameter, x, y)
    tobj.set_editor_property("parameter_name", param_name)
    tobj.set_editor_property("texture", tex_asset)
    tobj.set_editor_property("sampler_type", sampler_type)
    tobj.set_editor_property("group", "PF Surface")

    call = _expr(unreal.MaterialExpressionMaterialFunctionCall, x + 340, y)
    call.set_material_function(WAT)

    _link_in(tobj, "", call, WAT_TEXOBJ_IN)          # texture object (required)
    try:                                             # world tile size (soft-fail -> default tiling)
        _link_in(tile, "", call, WAT_SIZE_IN)
    except RuntimeError:
        unreal.log_warning("[M_PF_BuildPiece] could not bind WorldTileSize to WorldAlignedTexture; default tiling used.")
    return call

# --------------------------------------------------------------------------- #
#  BASE COLOR  (triplanar concrete, or flat gray fallback)
# --------------------------------------------------------------------------- #
bc_tex = _load_tex(BASECOLOR_TEXTURE, "BaseColor")
if bc_tex:
    bc_call = world_aligned(bc_tex, unreal.MaterialSamplerType.SAMPLERTYPE_COLOR, "BaseColorTex", -1200, -600)
    _prop_out(bc_call, WAT_OUT, MP.MP_BASE_COLOR)
else:
    gray = _expr(unreal.MaterialExpressionConstant3Vector, -600, -600)
    gray.set_editor_property("constant", unreal.LinearColor(CONCRETE_GRAY[0], CONCRETE_GRAY[1], CONCRETE_GRAY[2], 1.0))
    _prop(gray, "", MP.MP_BASE_COLOR)

# --------------------------------------------------------------------------- #
#  NORMAL  (triplanar; only wired when a normal map is supplied)
#  NOTE: WorldAlignedTexture is used per spec. If you later want per-axis
#  re-orientation, swap in /Engine/.../Texturing/WorldAlignedNormal.
# --------------------------------------------------------------------------- #
n_tex = _load_tex(NORMAL_TEXTURE, "Normal")
if n_tex:
    n_call = world_aligned(n_tex, unreal.MaterialSamplerType.SAMPLERTYPE_NORMAL, "NormalTex", -1200, -200)
    _prop_out(n_call, WAT_OUT, MP.MP_NORMAL)

# --------------------------------------------------------------------------- #
#  ROUGHNESS / AO  (from packed ORD if supplied, else constants)
# --------------------------------------------------------------------------- #
ord_tex = _load_tex(ORD_TEXTURE, "ORD")
if ord_tex:
    ord_call = world_aligned(ord_tex, unreal.MaterialSamplerType.SAMPLERTYPE_LINEAR_COLOR, "ORDTex", -1200, 200)

    rough_mask = _expr(unreal.MaterialExpressionComponentMask, -700, 260)   # G = Roughness
    rough_mask.set_editor_property("r", False); rough_mask.set_editor_property("g", True)
    rough_mask.set_editor_property("b", False); rough_mask.set_editor_property("a", False)
    _link_out(ord_call, WAT_OUT, rough_mask, "")
    _prop(rough_mask, "", MP.MP_ROUGHNESS)

    ao_mask = _expr(unreal.MaterialExpressionComponentMask, -700, 380)      # R = Ambient Occlusion
    ao_mask.set_editor_property("r", True);  ao_mask.set_editor_property("g", False)
    ao_mask.set_editor_property("b", False); ao_mask.set_editor_property("a", False)
    _link_out(ord_call, WAT_OUT, ao_mask, "")
    _prop(ao_mask, "", MP.MP_AMBIENT_OCCLUSION)
else:
    r_const = _expr(unreal.MaterialExpressionConstant, -600, 260)
    r_const.set_editor_property("r", DEFAULT_ROUGHNESS)
    _prop(r_const, "", MP.MP_ROUGHNESS)

    ao_const = _expr(unreal.MaterialExpressionConstant, -600, 380)
    ao_const.set_editor_property("r", DEFAULT_AO)
    _prop(ao_const, "", MP.MP_AMBIENT_OCCLUSION)

# --------------------------------------------------------------------------- #
#  TEAM ACCENT  ->  EMISSIVE = Color * AccentBoost * mask
#  Vector param MUST be named exactly "Color" (C++ contract). Base albedo stays
#  neutral concrete so team identity reads only from the emissive trim/rim.
# --------------------------------------------------------------------------- #
team_color = _expr(unreal.MaterialExpressionVectorParameter, -1200, 700)
team_color.set_editor_property("parameter_name", "Color")   # <-- do NOT rename
team_color.set_editor_property("default_value",
    unreal.LinearColor(TEAM_COLOR_DEFAULT[0], TEAM_COLOR_DEFAULT[1], TEAM_COLOR_DEFAULT[2], 1.0))
team_color.set_editor_property("group", "Team")

accent_boost = _expr(unreal.MaterialExpressionScalarParameter, -1200, 840)
accent_boost.set_editor_property("parameter_name", "AccentBoost")
accent_boost.set_editor_property("default_value", ACCENT_BOOST)
accent_boost.set_editor_property("group", "Team")

# --- Fresnel rim -----------------------------------------------------------
fresnel = _expr(unreal.MaterialExpressionFresnel, -1200, 1000)
fresnel.set_editor_property("exponent", FRESNEL_EXPONENT)
fresnel.set_editor_property("base_reflect_fraction", 0.0)   # clean rim, no flat base

# --- Thin top-edge band (constant WORLD thickness => scale-invariant) -------
#  Built defensively: if any node/pin is unavailable in this build, we fall
#  back to a Fresnel-only mask so the material still compiles cleanly.
mask_source = fresnel
try:
    thickness = _expr(unreal.MaterialExpressionScalarParameter, -1200, 1180)
    thickness.set_editor_property("parameter_name", "TopEdgeThickness")
    thickness.set_editor_property("default_value", TOP_EDGE_THICKNESS)
    thickness.set_editor_property("group", "Team")

    # object's top corner (local bounds max) -> world position, take Z
    top_max = _expr(unreal.MaterialExpressionObjectLocalBounds, -1900, 1320)
    xform   = _expr(unreal.MaterialExpressionTransformPosition, -1560, 1320)
    xform.set_editor_property("transform_source_type", unreal.MaterialPositionTransformSource.TRANSFORMSOURCE_LOCAL)
    xform.set_editor_property("transform_type",        unreal.MaterialPositionTransformSource.TRANSFORMSOURCE_WORLD)
    _link_out(top_max, BOUNDS_MAX, xform, _link_in(top_max, BOUNDS_MAX[0], xform, ["", "Input"])) \
        if False else _link_out(top_max, BOUNDS_MAX, xform, "Input")   # bounds-max -> transform input

    top_z = _expr(unreal.MaterialExpressionComponentMask, -1250, 1320)     # object top world Z
    top_z.set_editor_property("r", False); top_z.set_editor_property("g", False)
    top_z.set_editor_property("b", True);  top_z.set_editor_property("a", False)
    _link(xform, "", top_z, "")

    wpos = _expr(unreal.MaterialExpressionWorldPosition, -1560, 1500)
    px_z = _expr(unreal.MaterialExpressionComponentMask, -1250, 1500)      # pixel world Z
    px_z.set_editor_property("r", False); px_z.set_editor_property("g", False)
    px_z.set_editor_property("b", True);  px_z.set_editor_property("a", False)
    _link(wpos, "", px_z, "")

    delta = _expr(unreal.MaterialExpressionSubtract, -1000, 1400)          # top - pixel  (>=0 below top)
    _link(top_z, "", delta, "A")
    _link(px_z,  "", delta, "B")

    tnorm = _expr(unreal.MaterialExpressionDivide, -780, 1400)             # / thickness  (world units)
    _link(delta,     "", tnorm, "A")
    _link(thickness, "", tnorm, "B")

    clamp = _expr(unreal.MaterialExpressionClamp, -560, 1400)
    clamp.set_editor_property("min_default", 0.0)
    clamp.set_editor_property("max_default", 1.0)
    _link_in(tnorm, "", clamp, ["", "Input"])

    band = _expr(unreal.MaterialExpressionOneMinus, -360, 1400)            # 1 at the top edge
    _link_in(clamp, "", band, ["", "Input"])

    mask_max = _expr(unreal.MaterialExpressionMax, -140, 1120)             # rim OR top-band
    _link(fresnel, "", mask_max, "A")
    _link(band,    "", mask_max, "B")
    mask_source = mask_max
except Exception as e:
    unreal.log_warning("[M_PF_BuildPiece] top-edge band unavailable (%s); using Fresnel-only accent mask." % e)
    mask_source = fresnel

# --- Emissive = Color * AccentBoost * mask ---------------------------------
mul_cb = _expr(unreal.MaterialExpressionMultiply, 120, 800)               # Color * AccentBoost
_link(team_color,   "", mul_cb, "A")
_link(accent_boost, "", mul_cb, "B")

mul_final = _expr(unreal.MaterialExpressionMultiply, 340, 900)            # * mask
_link(mul_cb,      "", mul_final, "A")
_link(mask_source, "", mul_final, "B")

_prop(mul_final, "", MP.MP_EMISSIVE_COLOR)

# --------------------------------------------------------------------------- #
#  Finalize
# --------------------------------------------------------------------------- #
_layout = getattr(MEL, "layout_material_expressions", None)   # not guaranteed to exist in 5.6
if callable(_layout):
    try:
        _layout(mat)
    except Exception:
        pass

# Build pieces render as Instanced Static Meshes (PFBuildGrid ISMCs). A material used on
# ISMs MUST advertise this usage flag, or UE recompiles it on the fly at runtime — which
# looks like meshes flashing then vanishing / shaders that never settle. Bake it in.
mat.set_editor_property("used_with_instanced_static_meshes", True)

MEL.recompile_material(mat)
unreal.EditorAssetLibrary.save_asset(FULL_PATH)
unreal.log("[M_PF_BuildPiece] Done -> %s" % FULL_PATH)
