#!/bin/bash
# ============================================================================
#  Combat Forge - package a standalone macOS build.
#  Output: Packaged/Mac/  (CombatForge.app)
#
#  *** RUN THIS ON THE MAC. *** Unreal cannot cross-compile to macOS — the Mac
#  build must be produced on a Mac with UE 5.6 + Xcode installed.
#
#  Before first run:
#    1. Install UE 5.6 (Epic Launcher) + Xcode + `xcode-select --install`.
#    2. Sync the whole project to the Mac INCLUDING Content/Bandits/ (~12 GB,
#       untracked in git — copy it over manually).
#    3. In the Mac editor: Project Settings > Platforms > Mac, confirm the RHI
#       targets Metal SM6 (needed for Nanite / Virtual Textures). See packaging.md.
#    4. Edit the three paths below to match this Mac.
#
#  Build config: Development. For a release build:  ./Package-Mac.command Shipping
# ============================================================================
set -e

# Preflight: catch the known recurring env breakages (Launcher reverting the SDK
# cap, missing engine dylibs, ...) BEFORE a long cook dies half-way.
if [ "${CF_SKIP_PREFLIGHT:-0}" != 1 ]; then
	"$(dirname "${BASH_SOURCE[0]:-$0}")/check-mac-env.command" || exit 1
fi

# --- EDIT THESE for your Mac ---
UE="/Users/Shared/Epic Games/UE_5.6"
PROJ="$HOME/projects/combatforge/CombatForge.uproject"
OUT="$HOME/projects/combatforge/Packaged/Mac"
# --------------------------------

CONFIG="${1:-Development}"

echo ""
echo "=== Packaging Combat Forge (Mac / $CONFIG) ==="
echo "    Project: $PROJ"
echo "    Output:  $OUT"
echo ""

"$UE/Engine/Build/BatchFiles/RunUAT.sh" BuildCookRun \
  -project="$PROJ" \
  -noP4 -utf8output -nocompileeditor \
  -platform=Mac \
  -clientconfig="$CONFIG" \
  -build -cook -stage -pak -iostore -compressed \
  -archive -archivedirectory="$OUT"

echo ""
# UAT's Mac archive step is unreliable: it has been seen copying the bare 370MB binary wrapper
# (no pak/iostore files) into the archive dir instead of the staged app. If that happened, swap
# in the real staged build from Saved/StagedBuilds/Mac.
PROJROOT="$(dirname "$PROJ")"
ARCHIVED_APP="$OUT/CombatForge.app"
STAGED_APP="$PROJROOT/Saved/StagedBuilds/Mac/CombatForge.app"
if [ -d "$ARCHIVED_APP" ] && ! find "$ARCHIVED_APP" -name '*.pak' -o -name '*.ucas' | grep -q .; then
	if [ -d "$STAGED_APP" ] && find "$STAGED_APP" -name '*.pak' -o -name '*.ucas' | grep -q .; then
		echo "WARNING: archive step produced a pak-less app — swapping in the real staged build."
		rm -rf "$ARCHIVED_APP"
		mv "$STAGED_APP" "$ARCHIVED_APP"
	else
		echo "ERROR: archived app has no paks and no staged fallback exists — package is BROKEN." >&2
		exit 1
	fi
fi

echo "=== Done. App is in $OUT ==="
echo "NOTE: it's unsigned — first launch needs right-click > Open (Gatekeeper). See packaging.md."
echo "TIP:  Saved/Cooked + Saved/StagedBuilds hold ~16GB of re-cook cache — delete them if disk is tight"
echo "      (next package then does a full recook, ~40+ min instead of minutes)."
