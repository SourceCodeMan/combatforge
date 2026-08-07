# =============================================================================
#  Running shorts (procedural) — two Pants options for CombatForge
#
#  Creates:
#    /Game/Bandits/Textures/Pants/RunningShorts/T_RunningShorts_Hearts
#    /Game/Bandits/Textures/Pants/RunningShorts/T_RunningShorts_Leopard
#    /Game/Materials/M_PF_RunningShorts          (BaseColor texture + slight fabric rough)
#    /Game/Bandits/Mesh/Pants/RunningShorts/SKM_RunningShorts_Hearts
#    /Game/Bandits/Mesh/Pants/RunningShorts/SKM_RunningShorts_Leopard
#
#  Mesh source: SKM_Pants_Knees_Bege (knee-length pants read as athletic shorts).
#  Patterns are pure procedural pixel art (no external art). The Pants slot enumerates
#  /Game/Bandits/Mesh/Pants recursively, so these show up as two new options automatically.
#
#  C++ dress code: any pants part whose path contains "RunningShorts" leaves bare legs
#  visible (calves under the hem) — see PFChar::PantsLeaveLegsVisible.
#
#  Run headless (editor closed preferred):
#    UnrealEditor-Cmd.exe "<uproject>" -run=pythonscript -script="<abs this file>" -unattended -nosplash -nopause -stdout
# =============================================================================
import math
import os
import struct
import tempfile
import zlib

import unreal

if os.environ.get("PF_DRY_RUN") == "1":
    unreal.log_warning(
        "[PFShorts] DRY RUN: would DELETE/recreate RunningShorts textures, material, and SKMs. Exiting."
    )
    raise SystemExit(0)

MEL = unreal.MaterialEditingLibrary
MP = unreal.MaterialProperty
tools = unreal.AssetToolsHelpers.get_asset_tools()
lib = unreal.EditorAssetLibrary

TEX_DIR = "/Game/Bandits/Textures/Pants/RunningShorts"
MESH_DIR = "/Game/Bandits/Mesh/Pants/RunningShorts"
MAT_PATH = "/Game/Materials"
MAT_NAME = "M_PF_RunningShorts"
MAT_FULL = MAT_PATH + "/" + MAT_NAME

# Knee pants — short enough to read as athletic shorts once patterned.
SRC_MESH = "/Game/Bandits/Mesh/Pants/Pants_Knees/SKM_Pants_Knees_Bege"

TEX_SIZE = 512


def log(m):
    unreal.log("[PFShorts] " + m)


def try_set(o, p, v):
    try:
        o.set_editor_property(p, v)
        return True
    except Exception as e:
        unreal.log_warning("[PFShorts] set %s failed: %s" % (p, e))
        return False


def ensure_dir(package_path):
    # package_path like /Game/Bandits/Mesh/Pants/RunningShorts — no-op if present
    if not lib.does_directory_exist(package_path):
        lib.make_directory(package_path)


def write_png(path, w, h, rgba):
    """Minimal PNG writer (RGBA8). rgba = bytes length w*h*4."""
    assert len(rgba) == w * h * 4

    def chunk(tag, data):
        crc = zlib.crc32(tag + data) & 0xFFFFFFFF
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", crc)

    raw = b"".join(b"\x00" + rgba[y * w * 4 : (y + 1) * w * 4] for y in range(h))
    ihdr = struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)  # 8-bit RGBA
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


def clamp01(x):
    return 0.0 if x < 0.0 else (1.0 if x > 1.0 else x)


def pack_rgba(r, g, b, a=255):
    return bytes(
        (
            int(clamp01(r) * 255 + 0.5),
            int(clamp01(g) * 255 + 0.5),
            int(clamp01(b) * 255 + 0.5),
            int(a),
        )
    )


def heart_mask(u, v):
    """Classic heart SDF-ish in [0,1]x[0,1] local cell coords. True = inside heart."""
    # Centered coords
    x = (u - 0.5) * 2.4
    y = (0.45 - v) * 2.4
    # (x^2 + y^2 - 1)^3 - x^2 y^3 < 0
    a = x * x + y * y - 1.0
    return (a * a * a - x * x * y * y * y) < 0.0


def gen_hearts_rgba(n):
    """White fabric with repeating soft pink/red hearts."""
    out = bytearray(n * n * 4)
    # Fabric base: warm white linen
    tiles = 6
    for y in range(n):
        for x in range(n):
            # subtle weave noise
            weave = 0.97 + 0.03 * math.sin(x * 0.35) * math.cos(y * 0.31)
            r = 0.96 * weave
            g = 0.95 * weave
            b = 0.93 * weave
            # tile hearts
            cu = (x / float(n)) * tiles
            cv = (y / float(n)) * tiles
            # offset every other row
            row = int(cv)
            if row % 2:
                cu += 0.5
            fu = cu - math.floor(cu)
            fv = cv - math.floor(cv)
            if heart_mask(fu, fv):
                # pink → red edge
                edge = 0.0
                # soft fill
                r, g, b = 0.92, 0.28, 0.42
                # darker outline by sampling neighbors cheaply
                if not heart_mask(fu + 0.04, fv) or not heart_mask(fu - 0.04, fv) or not heart_mask(fu, fv + 0.04) or not heart_mask(fu, fv - 0.04):
                    r, g, b = 0.72, 0.12, 0.28
            i = (y * n + x) * 4
            out[i : i + 4] = pack_rgba(r, g, b, 255)
    return bytes(out)


def hash2(ix, iy):
    n = (ix * 374761393 + iy * 668265263) & 0x7FFFFFFF
    n = (n ^ (n >> 13)) * 1274126177
    return (n & 0x7FFFFFFF) / float(0x7FFFFFFF)


def gen_leopard_rgba(n):
    """Cream/white base with leopard rosettes (brown rings + darker cores)."""
    out = bytearray(n * n * 4)
    # Pre-place rosette centers on a jittered grid
    cells = 10
    centers = []
    for cy in range(cells):
        for cx in range(cells):
            jx = hash2(cx, cy)
            jy = hash2(cx + 17, cy + 91)
            px = (cx + 0.25 + 0.5 * jx) / float(cells) * n
            py = (cy + 0.25 + 0.5 * jy) / float(cells) * n
            rad = (0.035 + 0.025 * hash2(cx + 3, cy + 7)) * n
            centers.append((px, py, rad, hash2(cx + 5, cy + 2)))

    for y in range(n):
        for x in range(n):
            # cream white base with slight grain
            grain = 0.96 + 0.04 * hash2(x // 3, y // 3)
            r = 0.97 * grain
            g = 0.93 * grain
            b = 0.82 * grain
            for px, py, rad, h in centers:
                dx = x - px
                dy = y - py
                d = math.sqrt(dx * dx + dy * dy)
                # slightly elliptical via hash
                if d < rad * 1.35:
                    # ring thickness
                    inner = rad * 0.35
                    outer = rad
                    if d < inner * 0.55:
                        # core spot (some rosettes solid-ish)
                        if h > 0.55:
                            r, g, b = 0.22, 0.12, 0.06
                        # else leave cream hole
                    elif d < outer and d > outer * 0.55:
                        # brown ring
                        t = (d - outer * 0.55) / (outer * 0.45)
                        # soft edges
                        edge = math.sin(min(1.0, max(0.0, t)) * math.pi)
                        br, bg, bb = 0.38, 0.22, 0.10
                        r = r * (1.0 - edge) + br * edge
                        g = g * (1.0 - edge) + bg * edge
                        b = b * (1.0 - edge) + bb * edge
            # a few freckles
            if hash2(x, y) > 0.992:
                r, g, b = 0.30, 0.16, 0.08
            i = (y * n + x) * 4
            out[i : i + 4] = pack_rgba(r, g, b, 255)
    return bytes(out)


def import_png_as_texture(png_path, dest_package_path, asset_name):
    """Import a PNG into the content browser as Texture2D. Returns full asset path."""
    full = dest_package_path + "/" + asset_name
    if lib.does_asset_exist(full):
        lib.delete_asset(full)
        log("deleted existing " + full)

    task = unreal.AssetImportTask()
    task.set_editor_property("filename", png_path)
    task.set_editor_property("destination_path", dest_package_path)
    task.set_editor_property("destination_name", asset_name)
    task.set_editor_property("replace_existing", True)
    task.set_editor_property("automated", True)
    task.set_editor_property("save", True)

    factory = unreal.TextureFactory()
    task.set_editor_property("factory", factory)

    tools.import_asset_tasks([task])
    if not lib.does_asset_exist(full):
        raise RuntimeError("texture import failed: " + full)

    tex = unreal.load_asset(full)
    # Cloth albedo: sRGB on, no VT
    try_set(tex, "srgb", True)
    try_set(tex, "virtual_texture_streaming", False)
    try_set(tex, "compression_settings", unreal.TextureCompressionSettings.TC_DEFAULT)
    lib.save_asset(full)
    log("texture " + full)
    return full, tex


def create_shorts_material(hearts_tex, leopard_tex):
    """Simple lit cloth: TextureSample(BaseColor param) → BaseColor, constant roughness."""
    if lib.does_asset_exist(MAT_FULL):
        lib.delete_asset(MAT_FULL)
        log("deleted existing " + MAT_FULL)

    mat = tools.create_asset(MAT_NAME, MAT_PATH, unreal.Material, unreal.MaterialFactoryNew())
    assert mat is not None

    # Texture param — default hearts so MIDs/overrides can swap
    tex_param = MEL.create_material_expression(mat, unreal.MaterialExpressionTextureSampleParameter2D, -480, 0)
    tex_param.set_editor_property("parameter_name", "BaseColor")
    tex_param.set_editor_property("texture", hearts_tex)
    try_set(tex_param, "sampler_type", unreal.MaterialSamplerType.SAMPLERTYPE_COLOR)
    MEL.connect_material_property(tex_param, "", MP.MP_BASE_COLOR)

    rough = MEL.create_material_expression(mat, unreal.MaterialExpressionConstant, -480, 220)
    try_set(rough, "r", 0.72)
    MEL.connect_material_property(rough, "", MP.MP_ROUGHNESS)

    # Slight fabric sheen off — keep matte athletic cloth
    spec = MEL.create_material_expression(mat, unreal.MaterialExpressionConstant, -480, 300)
    try_set(spec, "r", 0.15)
    MEL.connect_material_property(spec, "", MP.MP_SPECULAR)

    MEL.recompile_material(mat)
    lib.save_asset(MAT_FULL)
    log("material " + MAT_FULL)
    return mat


def make_material_instance(parent_mat, name, package_path, texture):
    full = package_path + "/" + name
    if lib.does_asset_exist(full):
        lib.delete_asset(full)
    mi = tools.create_asset(name, package_path, unreal.MaterialInstanceConstant, unreal.MaterialInstanceConstantFactoryNew())
    unreal.MaterialEditingLibrary.set_material_instance_parent(mi, parent_mat)
    unreal.MaterialEditingLibrary.set_material_instance_texture_parameter_value(mi, "BaseColor", texture)
    lib.save_asset(full)
    log("MIC " + full)
    return mi


def set_skeletal_mesh_materials(mesh, material):
    """Assign material to every material slot on a skeletal mesh asset."""
    mats = mesh.get_editor_property("materials")
    if mats is None or len(mats) == 0:
        # Some meshes expose slot names differently — try one-slot write
        try_set(mesh, "materials", [unreal.SkeletalMaterial(material_interface=material)])
        return
    new_mats = []
    for sm in mats:
        # FSkeletalMaterial: keep slot name, swap interface
        slot_name = sm.get_editor_property("material_slot_name")
        new_sm = unreal.SkeletalMaterial()
        new_sm.set_editor_property("material_interface", material)
        if slot_name is not None:
            new_sm.set_editor_property("material_slot_name", slot_name)
        # Preserve UV channel data if present
        try:
            new_sm.set_editor_property("uv_channel_data", sm.get_editor_property("uv_channel_data"))
        except Exception:
            pass
        new_mats.append(new_sm)
    mesh.set_editor_property("materials", new_mats)


def duplicate_mesh_with_material(src_path, dest_path, material):
    if lib.does_asset_exist(dest_path):
        lib.delete_asset(dest_path)
        log("deleted existing " + dest_path)
    if not lib.does_asset_exist(src_path):
        raise RuntimeError("source mesh missing: " + src_path)
    if not lib.duplicate_asset(src_path, dest_path):
        raise RuntimeError("duplicate_asset failed: %s -> %s" % (src_path, dest_path))
    mesh = unreal.load_asset(dest_path)
    if mesh is None:
        raise RuntimeError("load failed after duplicate: " + dest_path)
    set_skeletal_mesh_materials(mesh, material)
    lib.save_asset(dest_path)
    log("mesh " + dest_path)
    return mesh


def force_save(path):
    if lib.does_asset_exist(path):
        lib.save_asset(path, only_if_is_dirty=False)
        log("saved " + path)


def verify_mesh(path):
    if not lib.does_asset_exist(path):
        raise RuntimeError("VERIFY FAIL missing: " + path)
    mesh = unreal.load_asset(path)
    if mesh is None:
        raise RuntimeError("VERIFY FAIL load None: " + path)
    # Confirm it is a skeletal mesh the pants enumerator will accept
    if not isinstance(mesh, unreal.SkeletalMesh):
        raise RuntimeError("VERIFY FAIL not SkeletalMesh: %s is %s" % (path, type(mesh)))
    mats = mesh.get_editor_property("materials")
    n = len(mats) if mats is not None else 0
    log("VERIFY ok %s  materials=%d  class=%s" % (path, n, mesh.get_class().get_name()))
    return mesh


def main():
    if not lib.does_asset_exist(SRC_MESH):
        raise RuntimeError("base knee-pants mesh missing: " + SRC_MESH)

    ensure_dir(TEX_DIR)
    ensure_dir(MESH_DIR)
    ensure_dir(MAT_PATH)
    mi_dir = MESH_DIR  # keep MICs next to the SKMs they dress

    hearts_mesh_path = MESH_DIR + "/SKM_RunningShorts_Hearts"
    leopard_mesh_path = MESH_DIR + "/SKM_RunningShorts_Leopard"

    tmp = tempfile.mkdtemp(prefix="pf_shorts_")
    hearts_png = os.path.join(tmp, "hearts.png")
    leopard_png = os.path.join(tmp, "leopard.png")

    log("painting hearts %dx%d…" % (TEX_SIZE, TEX_SIZE))
    write_png(hearts_png, TEX_SIZE, TEX_SIZE, gen_hearts_rgba(TEX_SIZE))
    log("painting leopard %dx%d…" % (TEX_SIZE, TEX_SIZE))
    write_png(leopard_png, TEX_SIZE, TEX_SIZE, gen_leopard_rgba(TEX_SIZE))

    _, hearts_tex = import_png_as_texture(hearts_png, TEX_DIR, "T_RunningShorts_Hearts")
    _, leopard_tex = import_png_as_texture(leopard_png, TEX_DIR, "T_RunningShorts_Leopard")

    parent = create_shorts_material(hearts_tex, leopard_tex)
    mi_hearts = make_material_instance(parent, "MI_RunningShorts_Hearts", mi_dir, hearts_tex)
    mi_leopard = make_material_instance(parent, "MI_RunningShorts_Leopard", mi_dir, leopard_tex)

    duplicate_mesh_with_material(SRC_MESH, hearts_mesh_path, mi_hearts)
    duplicate_mesh_with_material(SRC_MESH, leopard_mesh_path, mi_leopard)

    # Force-save every product so a subsequent PIE / commandlet sees them without a dirty-package lose.
    for p in (
        TEX_DIR + "/T_RunningShorts_Hearts",
        TEX_DIR + "/T_RunningShorts_Leopard",
        MAT_FULL,
        mi_dir + "/MI_RunningShorts_Hearts",
        mi_dir + "/MI_RunningShorts_Leopard",
        hearts_mesh_path,
        leopard_mesh_path,
    ):
        force_save(p)

    # Nudge the asset registry so UObjectLibrary pants enumeration can see the new packages.
    try:
        ar = unreal.AssetRegistryHelpers.get_asset_registry()
        ar.scan_paths_synchronous(["/Game/Bandits/Mesh/Pants/RunningShorts", TEX_DIR, MAT_PATH], force_rescan=True)
        log("asset registry rescanned RunningShorts paths")
    except Exception as e:
        unreal.log_warning("[PFShorts] asset registry rescan failed: %s" % e)

    verify_mesh(hearts_mesh_path)
    verify_mesh(leopard_mesh_path)

    # Cleanup temp pngs
    try:
        os.remove(hearts_png)
        os.remove(leopard_png)
        os.rmdir(tmp)
    except Exception:
        pass

    log("DONE — Pants options ready:")
    log("  1) Running Shorts (Hearts)   -> " + hearts_mesh_path)
    log("  2) Running Shorts (Leopard)  -> " + leopard_mesh_path)
    log("Quit and re-open the editor (or restart PIE) so PFChar re-enumerates the pants slot.")


main()
