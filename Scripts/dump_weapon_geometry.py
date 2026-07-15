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

MESHES = {
    "Rifle":       "/Game/Weapons/Rifle/Mesh/SM_Rifle.SM_Rifle",
    "Rifle_Olive": "/Game/QuantumCharacter/Mesh/Rifle/SM_Rifle_Olive.SM_Rifle_Olive",
    "AK_Black":    "/Game/Bandits/Mesh/Weapon/Rifle_AK/SM_AK_Black.SM_AK_Black",
    "AK_Wood":     "/Game/Bandits/Mesh/Weapon/Rifle_AK/SM_AK_Wood.SM_AK_Wood",
    "AKSU_Black":  "/Game/Bandits/Mesh/Weapon/Rifle_AK/SM_AKSU_Black.SM_AKSU_Black",
    "AKSU_Wood":   "/Game/Bandits/Mesh/Weapon/Rifle_AK/SM_AKSU_Wood.SM_AKSU_Wood",
    "Pistol":      "/Game/Bandits/Mesh/Weapon/Pistol/SM_Pistol.SM_Pistol",
}

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
