# Copyright (c) 2026 Tom Chapman. All rights reserved.
#
# Dumps the exact geometry of every catalog weapon mesh (bounds, pivot relation, sockets) so first-person
# poses can be COMPUTED instead of guessed. Output: Saved/weapon_geometry.json.
#
# Run headless (editor closed):
#   "<UE>\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" D:\projects\combatforge\CombatForge.uproject
#     -run=pythonscript -script="D:\projects\combatforge\Scripts\dump_weapon_geometry.py"
import json
import unreal

# Mesh list is DRIVEN BY THE CATALOG, not hand-maintained. The old hardcoded seven covered only a
# fraction of the shipping guns (every MarketplaceBlockout / modern weapon was missing), so the FP
# pose tooling that reads this dump was silently under-covering the catalog it exists to serve.
# (P2-S3)
import os
import re


def _catalog_meshes():
    """Every static-mesh object path quoted in PFWeaponCatalog.cpp, keyed by asset name."""
    try:
        proj = unreal.Paths.project_dir()
    except Exception:
        proj = ""
    cat = os.path.join(proj, "Source", "CombatForge", "Combat", "PFWeaponCatalog.cpp")
    if not os.path.isfile(cat):
        unreal.log_warning("[WeaponGeom] catalog not found at %s" % cat)
        return {}
    with open(cat, "r", encoding="utf-8", errors="replace") as fh:
        text = fh.read()
    found = {}
    # "/Game/<dirs>/SM_Foo.SM_Foo" - the ObjectPath form the catalog stores in MeshPath.
    _MESH_RE = r'"(/Game/[^"]*?/(SM_[A-Za-z0-9_]+)\.\2)"'
    for path in re.findall(_MESH_RE, text):
        full, asset = path
        found.setdefault(asset[3:], full)   # strip the SM_ prefix for a readable key
    if not found:
        unreal.log_error(
            "[WeaponGeom] catalog exists at %s but matched 0 MeshPaths — not falling back"
            % cat
        )
        raise SystemExit(1)
    return found


MESHES = _catalog_meshes()
if not MESHES:
    # Fallback so the tool still produces something on a tree where the catalog moved.
    unreal.log_warning("[WeaponGeom] falling back to the legacy hand-written mesh list")
    MESHES = {
        "Rifle":       "/Game/Weapons/Rifle/Mesh/SM_Rifle.SM_Rifle",
        "Rifle_Olive": "/Game/QuantumCharacter/Mesh/Rifle/SM_Rifle_Olive.SM_Rifle_Olive",
        "AK_Black":    "/Game/Bandits/Mesh/Weapon/Rifle_AK/SM_AK_Black.SM_AK_Black",
        "AK_Wood":     "/Game/Bandits/Mesh/Weapon/Rifle_AK/SM_AK_Wood.SM_AK_Wood",
        "AKSU_Black":  "/Game/Bandits/Mesh/Weapon/Rifle_AK/SM_AKSU_Black.SM_AKSU_Black",
        "AKSU_Wood":   "/Game/Bandits/Mesh/Weapon/Rifle_AK/SM_AKSU_Wood.SM_AKSU_Wood",
        "Pistol":      "/Game/Bandits/Mesh/Weapon/Pistol/SM_Pistol.SM_Pistol",
    }
unreal.log("[WeaponGeom] dumping %d catalog meshes" % len(MESHES))

out = {}
for name, path in MESHES.items():
    mesh = unreal.load_asset(path)
    if mesh is None:
        out[name] = {"error": "load failed", "path": path}
        continue
    b = mesh.get_bounds()
    origin, extent = b.origin, b.box_extent
    entry = {
        "path": path,
        "bounds_origin": [origin.x, origin.y, origin.z],
        "bounds_extent": [extent.x, extent.y, extent.z],
        "box_min": [origin.x - extent.x, origin.y - extent.y, origin.z - extent.z],
        "box_max": [origin.x + extent.x, origin.y + extent.y, origin.z + extent.z],
        "sockets": [],
    }
    socket_list = []
    for prop in ("sockets", "Sockets"):
        try:
            socket_list = list(mesh.get_editor_property(prop)) or []
            break
        except Exception:
            continue
    if not socket_list:
        try:
            socket_list = list(getattr(mesh, "sockets", [])) or []
        except Exception:
            socket_list = []
    for s in socket_list:
        try:
            loc = s.get_editor_property("relative_location")
            rot = s.get_editor_property("relative_rotation")
            entry["sockets"].append({
                "name": str(s.get_editor_property("socket_name")),
                "location": [loc.x, loc.y, loc.z],
                "rotation": [rot.pitch, rot.yaw, rot.roll],
            })
        except Exception as e:
            entry["sockets"].append({"error": str(e)})
    out[name] = entry

dest = unreal.Paths.project_saved_dir() + "weapon_geometry.json"
with open(dest, "w") as f:
    json.dump(out, f, indent=2)
unreal.log("dump_weapon_geometry: wrote %s (%d meshes)" % (dest, len(out)))
