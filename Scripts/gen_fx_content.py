# Copyright (c) 2026 Tom Chapman. All rights reserved.
#
# RETIRED - do not run. `gen_combat_fx.py` is the single owner of /Game/Materials/M_PF_Flash.
#
# This script used to build M_PF_Flash too, with DIFFERENT default emissive colour and strength
# (6.0,3.4,1.1 x1.0 here vs 8.0,4.2,1.2 x1.6 there). Both delete-and-recreate the same asset path,
# so whichever ran last silently won and the muzzle flash changed brightness depending on which one
# someone happened to run. One owner of the flash defaults, and it is the combat-FX generator that
# also builds the impact and tracer materials alongside it. (P2-S2)
#
# To regenerate the flash:
#   UnrealEditor-Cmd.exe "<uproject>" -run=pythonscript -script="<abs>/Scripts/gen_combat_fx.py" \
#       -unattended -nosplash -nopause -stdout
#
# Flash defaults now live in ONE place: the make_unlit("M_PF_Flash", ...) call at the bottom of
# gen_combat_fx.py. Change them there.
import unreal

unreal.log_error(
    "[PFFX] gen_fx_content.py is RETIRED. /Game/Materials/M_PF_Flash is owned by "
    "gen_combat_fx.py, which also builds the impact + tracer materials and carries the "
    "authored emissive defaults. Run that instead. Nothing was changed.")
raise SystemExit(1)
