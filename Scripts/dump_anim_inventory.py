# Copyright (c) 2026 Tom Chapman. All rights reserved.
import unreal
import json
import os

PATHS = [
    "/Game/AnimStarterPack",
    "/Game/Fab",
    "/Game/Bandits/Demo",
    "/Game/QuantumCharacter",
    "/Game/Survival_Character",
    "/Game/Characters",
]

CLASSES = [
    "AnimSequence",
    "BlendSpace",
    "BlendSpace1D",
    "AimOffsetBlendSpace",
    "AimOffsetBlendSpace1D",
    "AnimBlueprint",
    "Skeleton",
    "AnimMontage",
    "SkeletalMesh",
]

ar = unreal.AssetRegistryHelpers.get_asset_registry()
ar.wait_for_completion()

results = []
skeleton_paths = set()

for root in PATHS:
    assets = ar.get_assets_by_path(root, recursive=True)
    for a in assets:
        cls = str(a.asset_class_path.asset_name)
        if cls not in CLASSES:
            continue
        entry = {
            "asset_path": str(a.package_name) + "." + str(a.asset_name),
            "asset_name": str(a.asset_name),
            "class": cls,
            "pack_root": root,
        }
        # Skeleton tag
        try:
            skel_tag = a.get_tag_value("Skeleton")
            if skel_tag:
                entry["skeleton"] = str(skel_tag)
                skeleton_paths.add(str(skel_tag))
        except Exception:
            pass
        # Additional useful tags
        for tag in ("AdditiveAnimType", "SequenceLength", "Number of Frames", "bEnableRootMotion", "RootMotionRootLock", "TargetSkeleton", "ParentClass"):
            try:
                v = a.get_tag_value(tag)
                if v:
                    entry[tag] = str(v)
            except Exception:
                pass
        if cls == "Skeleton":
            skeleton_paths.add(entry["asset_path"])
        results.append(entry)

# Try to get bone names per skeleton by loading a SkeletalMesh bound to it
skeleton_bones = {}

# Map skeleton -> a skeletal mesh using it
skel_to_mesh = {}
for e in results:
    if e["class"] == "SkeletalMesh" and "skeleton" in e:
        # skeleton tag looks like /Script/Engine.Skeleton'/Game/....Name'
        s = e["skeleton"]
        skel_to_mesh.setdefault(s, e["asset_path"])

def clean_skel_path(s):
    # strip /Script/Engine.Skeleton'...' wrapper if present
    if "'" in s:
        s = s.split("'")[1]
    return s

for skel_ref, mesh_path in skel_to_mesh.items():
    key = clean_skel_path(skel_ref)
    if key in skeleton_bones:
        continue
    try:
        mesh = unreal.load_asset(mesh_path)
        if mesh is None:
            continue
        bones = []
        try:
            num = unreal.SkeletalMeshEditorSubsystemLibrary.get_num_bones(mesh)
        except Exception:
            num = 0
        if num == 0:
            try:
                subsys = unreal.get_editor_subsystem(unreal.SkeletalMeshEditorSubsystem)
                num = subsys.get_num_bones(mesh)
                for i in range(min(num, 30)):
                    bones.append(str(subsys.get_bone_name(mesh, i)))
            except Exception:
                pass
        else:
            for i in range(min(num, 30)):
                try:
                    bones.append(str(unreal.SkeletalMeshEditorSubsystemLibrary.get_bone_name(mesh, i)))
                except Exception:
                    break
        if not bones:
            # fallback: try via skeleton object ref-skeleton is not exposed; report unavailable
            skeleton_bones[key] = {"bones": [], "note": "bone names unavailable via python", "sample_mesh": mesh_path}
        else:
            skeleton_bones[key] = {"bones": bones, "sample_mesh": mesh_path}
    except Exception as ex:
        skeleton_bones[key] = {"bones": [], "note": "error: %s" % ex, "sample_mesh": mesh_path}

out = {
    "assets": results,
    "skeleton_bones": skeleton_bones,
}

out_path = os.path.join(unreal.Paths.project_saved_dir(), "anim_inventory.json")
with open(out_path, "w") as f:
    json.dump(out, f, indent=1)

# PFINV summary
counts = {}
for e in results:
    counts[(e["pack_root"], e["class"])] = counts.get((e["pack_root"], e["class"]), 0) + 1
print("PFINV total assets: %d" % len(results))
for (root, cls), n in sorted(counts.items()):
    print("PFINV %s %s: %d" % (root, cls, n))
for k, v in skeleton_bones.items():
    print("PFINV skeleton %s bones[first %d]: %s" % (k, len(v["bones"]), ",".join(v["bones"][:30]) if v["bones"] else v.get("note", "none")))
print("PFINV wrote %s" % out_path)
