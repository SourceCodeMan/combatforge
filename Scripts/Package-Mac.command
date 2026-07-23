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

# --- Paths: env vars win so a different Mac layout needs no file edit (issue #20 S5) ---
UE="${UE:-/Users/Shared/Epic Games/UE_5.6}"
PROJ="${PROJ:-$HOME/projects/combatforge/CombatForge.uproject}"
OUT="${OUT:-$(dirname "$PROJ")/Packaged/Mac}"
# ----------------------------------------------------------------------------------------

CONFIG="${1:-Development}"

# Shipping builds are the ones that go to players - mark them for distribution.
DISTFLAG=""
if [ "$CONFIG" = "Shipping" ]; then DISTFLAG="-distribution"; fi

# NetProtocol reminder: every public push MUST bump PFBuild::NetProtocol (join-handshake gate).
grep -n "NetProtocol" "$(dirname "$PROJ")/Source/CombatForge/CombatForge.h" || true
echo "*** REMINDER: bump PFBuild::NetProtocol (above) if this package ships to players. ***"

echo ""
echo "=== Packaging Combat Forge (Mac / $CONFIG) ==="
echo "    Project: $PROJ"
echo "    Output:  $OUT"
echo ""

"$UE/Engine/Build/BatchFiles/RunUAT.sh" BuildCookRun \
  -project="$PROJ" \
  -noP4 -utf8output -nocompileeditor \
  -platform=Mac \
  -clientconfig="$CONFIG" $DISTFLAG \
  -build -cook -stage -pak -iostore -compressed \
  -archive -archivedirectory="$OUT"

# --- PRIVACY SCRUB (mirrors Package-Windows.bat) -----------------------------
# A packaged Saved/ has leaked real data before: hostname, LAN IP, hardware info
# in logs — and once, the login session token (the alpha-4/5 incident). Debug
# symbols (.dSYM here, .pdb on Windows) leak build paths. NEVER ship either.
# This must run as part of this script, not appended after RunUAT elsewhere —
# and it must be checked before every push.
echo ""
echo "=== Scrubbing Saved/ + debug symbols from $OUT ==="
find "$OUT" -type d -name "Saved" -path "*/CombatForge/*" -prune -exec rm -rf {} + 2>/dev/null || true
find "$OUT" -type d -name "Saved" -path "*/Engine/*" -prune -exec rm -rf {} + 2>/dev/null || true
find "$OUT" -name "*.pdb" -delete 2>/dev/null || true
find "$OUT" -type d -name "*.dSYM" -prune -exec rm -rf {} + 2>/dev/null || true

DIRTY=""
if find "$OUT" -type d -name "Saved" \( -path "*/CombatForge/*" -o -path "*/Engine/*" \) | grep -q .; then DIRTY=1; fi
if find "$OUT" \( -name "*.pdb" -o \( -type d -name "*.dSYM" \) \) | grep -q .; then DIRTY=1; fi

echo ""
if [ -n "$DIRTY" ]; then
  echo "*** WARNING: Saved/ or debug symbols still present in $OUT - do NOT push until clean. ***"
else
  echo "=== Done + scrubbed. App is in $OUT - no Saved/, no symbols - safe to push. ==="
fi
echo "NOTE: it's unsigned — first launch needs right-click > Open (Gatekeeper). See packaging.md."
