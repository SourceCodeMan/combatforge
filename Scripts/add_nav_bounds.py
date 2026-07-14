# Copyright (c) 2026 Tom Chapman. All rights reserved.
#
# Adds ONE NavMeshBoundsVolume to L_Graybox so the runtime navmesh has a guaranteed build region
# (the "deterministic" path — see docs/ai-pathfinding-and-intelligence-research.md). The arena geometry
# is spawned at runtime and every character carries a UNavigationInvokerComponent, so this volume only
# DEFINES where nav may build; invokers still limit which tiles actually build (near pawns). Sized to
# cover the whole field (0,0)-(6400,4000), floor ~-15 to perimeter top ~1800, with margin.
#
# Idempotent (skips if a NavMeshBoundsVolume already exists) and defensive: only saves the level if the
# spawned volume came out with a real (non-zero) box brush — some engine builds' actor factory fails to
# build the brush, leaving an inert volume, in which case we abort the save and leave the map untouched.
#
# Run headless:
#   "<UE>\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" D:\projects\combatforge\CombatForge.uproject
#     -run=pythonscript -script="D:\projects\combatforge\Scripts\add_nav_bounds.py"
import unreal

MAP = "/Game/Maps/L_Graybox"

# Arena center + generous half-extent coverage (FieldX=6400, FieldY=4000; center 3200,2000).
CENTER = unreal.Vector(3200.0, 2000.0, 900.0)
# Default NavMeshBoundsVolume box brush is 200uu (half-extent 100). Scale to span the arena + margin:
#   X: +-4000 -> (-800..7200), Y: +-3000 -> (-1000..5000), Z: +-1500 -> (-600..2400).
SCALE = unreal.Vector(40.0, 30.0, 15.0)


def run():
    unreal.EditorLoadingAndSavingUtils.load_map(MAP)
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)

    existing = [a for a in eas.get_all_level_actors() if isinstance(a, unreal.NavMeshBoundsVolume)]
    if existing:
        unreal.log("add_nav_bounds: NavMeshBoundsVolume already present ({}) - nothing to do".format(len(existing)))
        return

    vol = eas.spawn_actor_from_class(unreal.NavMeshBoundsVolume, CENTER, unreal.Rotator(0.0, 0.0, 0.0))
    if vol is None:
        unreal.log_error("add_nav_bounds: spawn_actor_from_class returned None - NOT saving")
        return
    vol.set_actor_scale3d(SCALE)
    vol.set_actor_label("NavBounds_Arena")

    # Verify the box brush actually built (non-zero bounds). If it's empty the volume is inert; abort the
    # save so we don't commit a useless .umap change and can rely on the nav invokers (Option A) instead.
    origin, extent = vol.get_actor_bounds(False)
    unreal.log("add_nav_bounds: spawned volume bounds origin={} extent={}".format(origin, extent))
    if extent.x < 500.0 or extent.y < 500.0 or extent.z < 200.0:
        unreal.log_error("add_nav_bounds: spawned volume has empty/near-zero brush - engine factory did not "
                         "build it. Destroying + NOT saving (rely on nav invokers).")
        eas.destroy_actor(vol)
        return

    les = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    les.save_current_level()
    unreal.log("add_nav_bounds: NavMeshBoundsVolume added + level saved (extent {})".format(extent))


run()
