# Generates arena SHELL materials for CombatForge (airsoft / CQB warehouse look).
#   /Game/Materials/M_PF_ArenaFloor  — triplanar scuffed concrete (large tiles)
#   /Game/Materials/M_PF_ArenaWall   — triplanar concrete (finer tiles, cooler)
#   /Game/Materials/M_PF_ArenaMetal  — dark scuffed metal (midline posts)
#   /Game/Materials/M_PF_ArenaMark   — floor paint / spawn stripe; vector "Color" contract
#
# Zero-texture fallback if Concrete034 is missing. Uses WorldAlignedTexture so
# non-uniform BasicShape scales (floor 64x40x0.3, walls 0.2 thick) don't smear.
#
# Run headless (editor closed preferred for clean save):
#   UnrealEditor-Cmd.exe "<uproject>" -run=pythonscript -script="<abs>" -unattended -nosplash -nopause -stdout
import unreal

MEL = unreal.MaterialEditingLibrary
MP = unreal.MaterialProperty
tools = unreal.AssetToolsHelpers.get_asset_tools()

WAT_PATH = "/Engine/Functions/Engine_MaterialFunctions01/Texturing/WorldAlignedTexture"
BC_PATH = "/Game/Textures/Concrete/T_Concrete034_Color"
N_PATH = "/Game/Textures/Concrete/T_Concrete034_Normal"
R_PATH = "/Game/Textures/Concrete/T_Concrete034_Rough"

WAT_TEXOBJ_IN = ["TextureObject", "Texture Object", "Tex"]
WAT_SIZE_IN = ["WorldSize", "World Size", "Texture Size", "TextureSize", "Size"]
WAT_OUT = ["XYZ Texture", "XY Texture", ""]


def log(m):
    unreal.log("[PFArena] " + m)


def try_set(o, p, v):
    try:
        o.set_editor_property(p, v)
        return True
    except Exception as e:
        unreal.log_warning("[PFArena] set %s failed: %s" % (p, e))
        return False


def link(a, a_out, b, b_in):
    if not MEL.connect_material_expressions(a, a_out, b, b_in):
        raise RuntimeError("link failed: %s -> %s.%s" % (a.get_name(), b.get_name(), b_in))


def link_in(a, a_out, b, candidates):
    for bi in candidates:
        if MEL.connect_material_expressions(a, a_out, b, bi):
            return bi
    raise RuntimeError("no matching input in %s" % (candidates,))


def prop_out(a, outs, prop):
    for ao in outs:
        if MEL.connect_material_property(a, ao, prop):
            return ao
    raise RuntimeError("property link failed")


def load_tex(path):
    if not path:
        return None
    a = unreal.load_asset(path)
    if a is None:
        unreal.log_warning("[PFArena] texture missing: %s" % path)
    return a


def fresh_material(name):
    path = "/Game/Materials"
    full = path + "/" + name
    if unreal.EditorAssetLibrary.does_asset_exist(full):
        unreal.EditorAssetLibrary.delete_asset(full)
        log("deleted existing " + full)
    mat = tools.create_asset(name, path, unreal.Material, unreal.MaterialFactoryNew())
    assert mat is not None
    return mat, full


def world_aligned(mat, wat_fn, tex_asset, sampler_type, param_name, tile_expr, x, y):
    tobj = MEL.create_material_expression(mat, unreal.MaterialExpressionTextureObjectParameter, x, y)
    tobj.set_editor_property("parameter_name", param_name)
    tobj.set_editor_property("texture", tex_asset)
    tobj.set_editor_property("sampler_type", sampler_type)
    call = MEL.create_material_expression(mat, unreal.MaterialExpressionMaterialFunctionCall, x + 340, y)
    call.set_material_function(wat_fn)
    link_in(tobj, "", call, WAT_TEXOBJ_IN)
    try:
        link_in(tile_expr, "", call, WAT_SIZE_IN)
    except RuntimeError:
        unreal.log_warning("[PFArena] WorldTileSize pin unbound for %s" % param_name)
    return call


def make_triplanar_surface(name, tile_size, base_tint, roughness, roughness_add, metallic=0.0):
    """Concrete-like surface with optional metal. base_tint multiplies albedo (muted CQB)."""
    mat, full = fresh_material(name)
    wat = unreal.load_asset(WAT_PATH)
    assert wat is not None, "WorldAlignedTexture missing"

    tile = MEL.create_material_expression(mat, unreal.MaterialExpressionScalarParameter, -1600, -200)
    tile.set_editor_property("parameter_name", "WorldTileSize")
    tile.set_editor_property("default_value", float(tile_size))

    bc_tex = load_tex(BC_PATH)
    n_tex = load_tex(N_PATH)
    r_tex = load_tex(R_PATH)

    # ---- BaseColor ----
    if bc_tex:
        bc_call = world_aligned(mat, wat, bc_tex, unreal.MaterialSamplerType.SAMPLERTYPE_COLOR,
                                "BaseColorTex", tile, -1200, -500)
        tint = MEL.create_material_expression(mat, unreal.MaterialExpressionConstant3Vector, -500, -650)
        try_set(tint, "constant", unreal.LinearColor(base_tint[0], base_tint[1], base_tint[2], 1.0))
        mul = MEL.create_material_expression(mat, unreal.MaterialExpressionMultiply, -250, -500)
        # Connect WAT output * tint
        connected = False
        for ao in WAT_OUT:
            if MEL.connect_material_expressions(bc_call, ao, mul, "A"):
                connected = True
                break
        if not connected:
            MEL.connect_material_expressions(bc_call, "", mul, "A")
        MEL.connect_material_expressions(tint, "", mul, "B")
        MEL.connect_material_property(mul, "", MP.MP_BASE_COLOR)
    else:
        gray = MEL.create_material_expression(mat, unreal.MaterialExpressionConstant3Vector, -400, -500)
        try_set(gray, "constant", unreal.LinearColor(base_tint[0], base_tint[1], base_tint[2], 1.0))
        MEL.connect_material_property(gray, "", MP.MP_BASE_COLOR)

    # ---- Normal ----
    if n_tex:
        n_call = world_aligned(mat, wat, n_tex, unreal.MaterialSamplerType.SAMPLERTYPE_NORMAL,
                               "NormalTex", tile, -1200, -100)
        prop_out(n_call, WAT_OUT, MP.MP_NORMAL)

    # ---- Roughness ----
    if r_tex:
        r_call = world_aligned(mat, wat, r_tex, unreal.MaterialSamplerType.SAMPLERTYPE_LINEAR_COLOR,
                               "RoughTex", tile, -1200, 250)
        # Sample often grayscale; use R channel then bias for scuff
        rmask = MEL.create_material_expression(mat, unreal.MaterialExpressionComponentMask, -500, 280)
        try_set(rmask, "r", True)
        try_set(rmask, "g", False)
        try_set(rmask, "b", False)
        try_set(rmask, "a", False)
        for ao in WAT_OUT:
            if MEL.connect_material_expressions(r_call, ao, rmask, ""):
                break
        else:
            MEL.connect_material_expressions(r_call, "", rmask, "")
        rbias = MEL.create_material_expression(mat, unreal.MaterialExpressionAdd, -280, 280)
        try_set(rbias, "const_b", float(roughness_add))
        MEL.connect_material_expressions(rmask, "", rbias, "A")
        rclamp = MEL.create_material_expression(mat, unreal.MaterialExpressionClamp, -100, 280)
        try_set(rclamp, "min_default", 0.05)
        try_set(rclamp, "max_default", 1.0)
        MEL.connect_material_expressions(rbias, "", rclamp, "Input")
        MEL.connect_material_property(rclamp, "", MP.MP_ROUGHNESS)
    else:
        rc = MEL.create_material_expression(mat, unreal.MaterialExpressionConstant, -200, 280)
        try_set(rc, "r", float(roughness))
        MEL.connect_material_property(rc, "", MP.MP_ROUGHNESS)

    if metallic > 0.001:
        met = MEL.create_material_expression(mat, unreal.MaterialExpressionConstant, -200, 400)
        try_set(met, "r", float(metallic))
        MEL.connect_material_property(met, "", MP.MP_METALLIC)

    # Neutral Color param (unused on structure) so ApplyTint never silently no-ops if misapplied
    col = MEL.create_material_expression(mat, unreal.MaterialExpressionVectorParameter, -1200, 600)
    col.set_editor_property("parameter_name", "Color")
    col.set_editor_property("default_value", unreal.LinearColor(0, 0, 0, 1))
    # Zero emissive by default (Color * 0)
    z = MEL.create_material_expression(mat, unreal.MaterialExpressionConstant, -900, 700)
    try_set(z, "r", 0.0)
    em = MEL.create_material_expression(mat, unreal.MaterialExpressionMultiply, -700, 620)
    MEL.connect_material_expressions(col, "", em, "A")
    MEL.connect_material_expressions(z, "", em, "B")
    MEL.connect_material_property(em, "", MP.MP_EMISSIVE_COLOR)

    MEL.recompile_material(mat)
    unreal.EditorAssetLibrary.save_asset(full)
    log("saved " + full)
    return full


def make_metal(name):
    """Dark corrugated / painted steel feel without textures — procedural noise roughness."""
    mat, full = fresh_material(name)

    base = MEL.create_material_expression(mat, unreal.MaterialExpressionConstant3Vector, -600, 0)
    # Muted charcoal-green industrial metal
    try_set(base, "constant", unreal.LinearColor(0.12, 0.13, 0.135, 1.0))
    MEL.connect_material_property(base, "", MP.MP_BASE_COLOR)

    met = MEL.create_material_expression(mat, unreal.MaterialExpressionConstant, -600, 160)
    try_set(met, "r", 0.72)
    MEL.connect_material_property(met, "", MP.MP_METALLIC)

    # World-position noise -> roughness variation (scuffs)
    wpos = MEL.create_material_expression(mat, unreal.MaterialExpressionWorldPosition, -900, 320)
    noise = MEL.create_material_expression(mat, unreal.MaterialExpressionNoise, -650, 320)
    try_set(noise, "scale", 0.015)
    try_set(noise, "levels", 2)
    try_set(noise, "output_min", 0.35)
    try_set(noise, "output_max", 0.85)
    MEL.connect_material_expressions(wpos, "", noise, "Position")
    MEL.connect_material_property(noise, "", MP.MP_ROUGHNESS)

    spec = MEL.create_material_expression(mat, unreal.MaterialExpressionConstant, -600, 480)
    try_set(spec, "r", 0.45)
    MEL.connect_material_property(spec, "", MP.MP_SPECULAR)

    # Color param for optional gray/team post tint via ApplyTint (dim)
    col = MEL.create_material_expression(mat, unreal.MaterialExpressionVectorParameter, -900, 600)
    col.set_editor_property("parameter_name", "Color")
    col.set_editor_property("default_value", unreal.LinearColor(0.2, 0.2, 0.22, 1.0))
    # Weak emissive so metal posts can take ApplyTint gray without glowing like paint
    emul = MEL.create_material_expression(mat, unreal.MaterialExpressionMultiply, -550, 600)
    try_set(emul, "const_b", 0.04)
    MEL.connect_material_expressions(col, "", emul, "A")
    MEL.connect_material_property(emul, "", MP.MP_EMISSIVE_COLOR)

    MEL.recompile_material(mat)
    unreal.EditorAssetLibrary.save_asset(full)
    log("saved " + full)
    return full


def make_mark(name):
    """Floor hazard / spawn strip paint. Driven by vector param Color (C++ ApplyTint)."""
    mat, full = fresh_material(name)

    col = MEL.create_material_expression(mat, unreal.MaterialExpressionVectorParameter, -700, 0)
    col.set_editor_property("parameter_name", "Color")
    col.set_editor_property("default_value", unreal.LinearColor(0.9, 0.75, 0.1, 1.0))

    # Albedo slightly darker than pure Color so it reads as paint on concrete, not neon
    dark = MEL.create_material_expression(mat, unreal.MaterialExpressionMultiply, -350, 0)
    try_set(dark, "const_b", 0.55)
    MEL.connect_material_expressions(col, "", dark, "A")
    MEL.connect_material_property(dark, "", MP.MP_BASE_COLOR)

    # Soft emissive so team strips pop under warehouse lighting
    em = MEL.create_material_expression(mat, unreal.MaterialExpressionMultiply, -350, 180)
    try_set(em, "const_b", 0.35)
    MEL.connect_material_expressions(col, "", em, "A")
    MEL.connect_material_property(em, "", MP.MP_EMISSIVE_COLOR)

    rough = MEL.create_material_expression(mat, unreal.MaterialExpressionConstant, -350, 320)
    try_set(rough, "r", 0.7)
    MEL.connect_material_property(rough, "", MP.MP_ROUGHNESS)

    MEL.recompile_material(mat)
    unreal.EditorAssetLibrary.save_asset(full)
    log("saved " + full)
    return full


# ---------------------------------------------------------------------------
# Build set
# ---------------------------------------------------------------------------
make_triplanar_surface(
    "M_PF_ArenaFloor",
    tile_size=512.0,
    base_tint=(0.72, 0.72, 0.70),   # slightly warm scuffed floor
    roughness=0.9,
    roughness_add=0.08,
    metallic=0.0,
)
make_triplanar_surface(
    "M_PF_ArenaWall",
    tile_size=280.0,
    base_tint=(0.62, 0.64, 0.66),   # cooler concrete wall
    roughness=0.88,
    roughness_add=0.05,
    metallic=0.0,
)
make_metal("M_PF_ArenaMetal")
make_mark("M_PF_ArenaMark")
log("arena material set complete")
