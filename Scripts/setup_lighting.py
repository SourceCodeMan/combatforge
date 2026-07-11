# Headless environment-lighting pass for L_Graybox.
# Adds (idempotently, keyed by actor label) a modern UE5.6 outdoor lighting kit:
#   sun (DirectionalLight, atmosphere sun) + SkyAtmosphere + real-time-capture SkyLight
#   + ExponentialHeightFog (volumetric) + VolumetricCloud + an unbound graded PostProcessVolume.
# Turns the empty gray stage into a lit game world. Re-runnable: reconfigures its own
# actors instead of duplicating them.  Run headless:
#   UnrealEditor-Cmd.exe "<uproject>" -run=pythonscript -script="<abs path>" -unattended -nosplash -nopause -stdout
import unreal

MAP = "/Game/Maps/L_Graybox"
INCLUDE_CLOUDS = True   # open-top arena => sky is visible; clouds add a lot. Cut if perf suffers.

les = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)

def log(m):   unreal.log("[PFLight] " + m)
def warn(m):  unreal.log_warning("[PFLight] " + m)

if not les.load_level(MAP):
    unreal.log_error("[PFLight] could not load " + MAP)
    raise SystemExit

# ---- inspect what's already in the stage --------------------------------------
existing = eas.get_all_level_actors()
log("existing actors (%d):" % len(existing))
for a in existing:
    log("   - %s  [%s]" % (a.get_actor_label(), a.get_class().get_name()))

def ensure(cls, label, loc=unreal.Vector(0, 0, 1500), rot=unreal.Rotator(0, 0, 0)):
    """Find our labelled actor of this class, else spawn one. Returns the actor."""
    for a in eas.get_all_level_actors():
        if a.get_actor_label() == label and a.get_class() == cls:
            log("reuse " + label)
            return a
    a = eas.spawn_actor_from_class(cls, loc, rot)
    a.set_actor_label(label)
    log("spawned " + label)
    return a

def comp(actor, comp_cls):
    return actor.get_component_by_class(comp_cls)

def trySet(obj, prop, val):
    try:
        obj.set_editor_property(prop, val)
        return True
    except Exception as e:
        warn("  set %s failed: %s" % (prop, e))
        return False

MOVABLE = unreal.ComponentMobility.MOVABLE

# ---- 1. Sun -------------------------------------------------------------------
sun = ensure(unreal.DirectionalLight, "PF_Light_Sun",
             loc=unreal.Vector(0, 0, 2000),
             rot=unreal.Rotator(0.0, -46.0, -35.0))   # (roll, pitch, yaw): angled afternoon sun
dl = comp(sun, unreal.DirectionalLightComponent)
if dl:
    trySet(dl, "mobility", MOVABLE)
    trySet(dl, "intensity", 6.0)
    trySet(dl, "atmosphere_sun_light", True)
    trySet(dl, "dynamic_shadow_distance_movable_light", 20000.0)
    try:
        dl.set_light_color(unreal.LinearColor(1.0, 0.96, 0.88, 1.0))
    except Exception as e:
        warn("sun color failed: %s" % e)

# ---- 2. Sky atmosphere (physical sky colour + horizon) ------------------------
ensure(unreal.SkyAtmosphere, "PF_Light_SkyAtmosphere")

# ---- 3. Sky light (ambient bounce; real-time captures the atmosphere) ----------
sky = ensure(unreal.SkyLight, "PF_Light_SkyLight", loc=unreal.Vector(0, 0, 1200))
sc = comp(sky, unreal.SkyLightComponent)
if sc:
    trySet(sc, "mobility", MOVABLE)
    trySet(sc, "real_time_capture", True)
    trySet(sc, "intensity_scale", 1.0)

# ---- 4. Height fog (depth + light shafts) -------------------------------------
fog = ensure(unreal.ExponentialHeightFog, "PF_Light_HeightFog", loc=unreal.Vector(0, 0, 0))
fc = comp(fog, unreal.ExponentialHeightFogComponent)
if fc:
    trySet(fc, "fog_density", 0.015)
    trySet(fc, "fog_height_falloff", 0.2)
    trySet(fc, "volumetric_fog", True)
    trySet(fc, "volumetric_fog_scattering_distribution", 0.2)

# ---- 5. Volumetric clouds (optional) ------------------------------------------
if INCLUDE_CLOUDS:
    ensure(unreal.VolumetricCloud, "PF_Light_Clouds", loc=unreal.Vector(0, 0, 0))

# ---- 6. Post-process volume (unbound, graded, locked exposure) ----------------
ppv = ensure(unreal.PostProcessVolume, "PF_Light_PostProcess", loc=unreal.Vector(0, 0, 0))
trySet(ppv, "unbound", True)
trySet(ppv, "priority", 1.0)
s = ppv.get_editor_property("settings")

def grade(override_prop, value_prop, value):
    # every FPostProcessSettings knob needs its bOverride_* companion set true
    ok1 = trySet(s, override_prop, True)
    ok2 = trySet(s, value_prop, value)
    if not (ok1 and ok2):
        warn("  grade %s incomplete" % value_prop)

# lock exposure so brightness doesn't "breathe" as you turn
grade("override_auto_exposure_min_brightness", "auto_exposure_min_brightness", 1.0)
grade("override_auto_exposure_max_brightness", "auto_exposure_max_brightness", 1.0)
grade("override_auto_exposure_bias", "auto_exposure_bias", 0.0)
# gentle filmic polish
grade("override_bloom_intensity", "bloom_intensity", 0.6)
grade("override_vignette_intensity", "vignette_intensity", 0.35)
grade("override_ambient_occlusion_intensity", "ambient_occlusion_intensity", 0.55)
grade("override_color_saturation", "color_saturation", unreal.Vector4(1.06, 1.06, 1.06, 1.0))
grade("override_color_contrast", "color_contrast", unreal.Vector4(1.04, 1.04, 1.04, 1.0))
ppv.set_editor_property("settings", s)

# ---- save ---------------------------------------------------------------------
if les.save_current_level():
    log("SAVED " + MAP)
else:
    unreal.log_error("[PFLight] save failed")

log("final actors:")
for a in eas.get_all_level_actors():
    log("   - %s  [%s]" % (a.get_actor_label(), a.get_class().get_name()))
log("done")
