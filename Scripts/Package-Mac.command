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
#    2. Install Git LFS, clone this release branch, and run `git lfs pull` so all paid/content
#       packs are hydrated. The script refuses unresolved LFS pointer files.
#    3. In the Mac editor: Project Settings > Platforms > Mac, confirm the RHI
#       targets Metal SM6 (needed for Nanite / Virtual Textures). See packaging.md.
#    4. If UE is not in the default location, run with UE="/path/to/UE_5.6".
#
#  Build config defaults to Shipping. For a local diagnostic build: ./Package-Mac.command Development
# ============================================================================
set -euo pipefail

# --- Paths: env vars win so a different Mac layout needs no file edit (issue #20 S5) ---
UE="${UE:-/Users/Shared/Epic Games/UE_5.6}"
# Default to the uproject beside this script's parent (Scripts/..), the same %~dp0.. rule
# Package-Windows.bat uses, instead of assuming one hard-coded Mac layout. (P2-S9)
_SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" && pwd -P)"
PROJ_INPUT="${PROJ:-$_SCRIPT_DIR/../CombatForge.uproject}"
PROJECT_ROOT="$(cd "$(dirname "$PROJ_INPUT")" && pwd -P)"
PROJ="$PROJECT_ROOT/$(basename "$PROJ_INPUT")"
PACKAGED_ROOT="$PROJECT_ROOT/Packaged"
OUT="${OUT:-$PACKAGED_ROOT/Mac}"
case "$OUT" in
  /*) ;;
  *) OUT="$PROJECT_ROOT/$OUT" ;;
esac
mkdir -p "$PACKAGED_ROOT" "$OUT"
OUT="$(cd "$OUT" && pwd -P)"
case "$OUT/" in
  "$PACKAGED_ROOT"/*) ;;
  *) echo "Refusing output outside $PACKAGED_ROOT (got $OUT)"; exit 2 ;;
esac
# ----------------------------------------------------------------------------------------

CONFIG="${1:-Shipping}"
if [ "$CONFIG" != "Development" ] && [ "$CONFIG" != "Shipping" ]; then
  echo "Usage: ./Scripts/Package-Mac.command [Development|Shipping]"
  exit 2
fi

# Shipping builds are the ones that go to players - mark them for distribution.
UAT_RELEASE_FLAGS=()
if [ "$CONFIG" = "Shipping" ]; then
	# Do not pass -distribution for this unsigned itch build. On UE 5.6/macOS it enters the
	# ModernXcode .xcarchive path and fails while searching ~/Library/Developer/Xcode/Archives.
	UAT_RELEASE_FLAGS=(-clean -nodebuginfo)
fi

# NetProtocol reminder: every public push MUST use a fresh value so LAN peers and the itch
# userversion stay unambiguous. No official fleet is part of this Alpha release.
grep -n "NetProtocol" "$(dirname "$PROJ")/Source/CombatForge/CombatForge.h" || true
echo "*** REMINDER: bump PFBuild::NetProtocol (above) if this package ships to players. ***"

# Shipping HARD GATE: refuse a protocol already packaged successfully on this machine. Set
# PF_ALLOW_SAME_PROTOCOL=1 only for an intentional local rebuild that will not create a second public
# artifact with the same userversion. Development can opt into the same check with PF_REQUIRE_PROTOCOL_BUMP=1.
PROTO=$(sed -n 's/.*NetProtocol *= *\([0-9][0-9]*\).*/\1/p' \
  "$(dirname "$PROJ")/Source/CombatForge/CombatForge.h" | head -1)
if [ -z "$PROTO" ]; then
  echo "*** REFUSING TO PACKAGE: could not read PFBuild::NetProtocol. ***"
  exit 2
fi
PROTOSTAMP="$PACKAGED_ROOT/.last-packaged-protocol-mac"
if { [ "$CONFIG" = "Shipping" ] || [ "${PF_REQUIRE_PROTOCOL_BUMP:-}" = "1" ]; } \
  && [ "${PF_ALLOW_SAME_PROTOCOL:-}" != "1" ] && [ -f "$PROTOSTAMP" ]; then
  if [ "$PROTO" = "$(cat "$PROTOSTAMP")" ]; then
    echo ""
    echo "*** REFUSING TO PACKAGE: NetProtocol is still $PROTO, the value already packaged."
    echo "*** Bump PFBuild::NetProtocol in Source/CombatForge/CombatForge.h, or set"
    echo "*** PF_ALLOW_SAME_PROTOCOL=1 only for an intentional local rebuild."
    exit 2
  fi
fi

if [ ! -f "$PROJ" ]; then
  echo "Project not found: $PROJ"
  exit 2
fi
if [ ! -x "$UE/Engine/Build/BatchFiles/RunUAT.sh" ]; then
  echo "RunUAT.sh not found under UE=$UE"
  exit 2
fi
if [ "${CF_SKIP_PREFLIGHT:-0}" != "1" ]; then
	CF_ENGINE="$UE" "$_SCRIPT_DIR/check-mac-env.command"
fi
if [ "$CONFIG" = "Shipping" ] && [ "${PF_ALLOW_DIRTY_TREE:-}" != "1" ]; then
  DIRTY_TRACKED=$(git -C "$PROJECT_ROOT" status --porcelain --untracked-files=no)
  if [ -n "$DIRTY_TRACKED" ]; then
    echo "*** REFUSING SHIPPING COOK: tracked files differ from the Git commit in the manifest. ***"
    echo "$DIRTY_TRACKED"
    echo "Commit/stash the changes, or set PF_ALLOW_DIRTY_TREE=1 only for a diagnostic build."
    exit 2
  fi
fi
git -C "$PROJECT_ROOT" lfs version >/dev/null
UNRESOLVED_LFS=$(git -C "$PROJECT_ROOT" lfs ls-files | awk '$2=="-"')
if [ -n "$UNRESOLVED_LFS" ]; then
	echo "$UNRESOLVED_LFS"
	echo "Run 'git lfs pull' and retry. Cooking pointer text would produce a broken build."
	exit 2
fi

if [ "$CONFIG" = "Shipping" ]; then
  rm -rf "$OUT"
  mkdir -p "$OUT"
fi

echo ""
echo "=== Packaging Combat Forge (Mac / $CONFIG) ==="
echo "    Project: $PROJ"
echo "    Output:  $OUT"
echo ""

"$UE/Engine/Build/BatchFiles/RunUAT.sh" BuildCookRun \
  -project="$PROJ" \
  -noP4 -utf8output -nocompileeditor \
  -platform=Mac \
  -clientconfig="$CONFIG" "${UAT_RELEASE_FLAGS[@]}" \
  -build -cook -stage -pak -iostore -compressed \
	-archive -archivedirectory="$OUT"

# UE 5.6's Mac archive step can report success while copying only a small executable wrapper and no
# pak/IoStore payload. The staged app is complete in that failure mode, so recover it before scrubbing.
APP_NAME="CombatForge.app"
if [ "$CONFIG" != "Development" ]; then
	APP_NAME="CombatForge-Mac-$CONFIG.app"
fi
APP_PATH=$(find "$OUT" -type d -name "$APP_NAME" -print -quit)
if [ -z "$APP_PATH" ]; then
	echo "*** ERROR: no $APP_NAME found under $OUT — archive step produced nothing. ***"
	exit 1
fi
STAGED_APP="$PROJECT_ROOT/Saved/StagedBuilds/Mac/$APP_NAME"
if ! find "$APP_PATH" -type f \( -name "*.pak" -o -name "*.ucas" \) -print -quit | grep -q .; then
	if [ -d "$STAGED_APP" ] && \
		find "$STAGED_APP" -type f \( -name "*.pak" -o -name "*.ucas" \) -print -quit | grep -q .; then
		echo "WARNING: archive produced a pak-less wrapper — replacing it with the complete staged app."
		rm -rf "$APP_PATH"
		mv "$STAGED_APP" "$APP_PATH"
	else
		echo "*** ERROR: archived app has no pak/IoStore payload and no complete staged fallback. ***"
		exit 1
	fi
fi

# --- PRIVACY SCRUB (mirrors Package-Windows.bat) -----------------------------
# A packaged Saved/ has leaked real data before: hostname, LAN IP, hardware info
# in logs — and once, the login session token (the alpha-4/5 incident). Debug
# symbols (.dSYM here, .pdb on Windows) leak build paths. NEVER ship either.
# This must run as part of this script, not appended after RunUAT elsewhere —
# and it must be checked before every push.
echo ""
echo "=== Scrubbing Saved/ + debug symbols from $OUT ==="
find "$OUT" -type d -name "Saved" -prune -exec rm -rf {} + 2>/dev/null || true
find "$OUT" -name "*.pdb" -delete 2>/dev/null || true
find "$OUT" -type d -name "*.dSYM" -prune -exec rm -rf {} + 2>/dev/null || true

DIRTY=""
if find "$OUT" -type d -name "Saved" | grep -q .; then DIRTY=1; fi
if find "$OUT" \( -name "*.pdb" -o \( -type d -name "*.dSYM" \) \) | grep -q .; then DIRTY=1; fi
if find "$OUT" -type f \( -name "Auth.json" -o -name "ServerKey.txt" \) | grep -q .; then DIRTY=1; fi

echo ""
if [ -n "$DIRTY" ]; then
  echo "*** ERROR: Saved/, credentials, or debug symbols still present in $OUT - do NOT push. Failing. ***"
  exit 1
fi

# UAT can report success while archiving to an unexpected nested folder. Resolve the actual app,
# validate its main executable, and emit the same release manifest the Windows Shipping path uses.
PUBLISH_DIR=$(dirname "$APP_PATH")
INFO_PLIST="$APP_PATH/Contents/Info.plist"
if [ ! -f "$INFO_PLIST" ]; then
  echo "*** ERROR: packaged app has no Info.plist at $INFO_PLIST. ***"
  exit 1
fi
EXEC_NAME=$(/usr/bin/plutil -extract CFBundleExecutable raw -o - "$INFO_PLIST")
case "$EXEC_NAME" in
  ""|*/*|"..") echo "*** ERROR: invalid CFBundleExecutable '$EXEC_NAME'. ***"; exit 1 ;;
esac
APP_EXEC="$APP_PATH/Contents/MacOS/$EXEC_NAME"
if [ ! -f "$APP_EXEC" ]; then
  echo "*** ERROR: packaged app has no main executable at $APP_EXEC. ***"
  exit 1
fi
EXEC_REL="${APP_EXEC#"$PUBLISH_DIR"/}"
EXEC_SHA=$(shasum -a 256 "$APP_EXEC" | awk '{print $1}')
GIT_SHA=$(git -C "$PROJECT_ROOT" rev-parse HEAD)
CREATED_UTC=$(date -u '+%Y-%m-%dT%H:%M:%SZ')
MANIFEST="$PUBLISH_DIR/CombatForge-build.json"
printf '{\n  "product": "CombatForge",\n  "configuration": "%s",\n  "netProtocol": %s,\n  "gitCommit": "%s",\n  "createdUtc": "%s",\n  "executable": "%s",\n  "executableSha256": "%s"\n}\n' \
  "$CONFIG" "$PROTO" "$GIT_SHA" "$CREATED_UTC" "$EXEC_REL" "$EXEC_SHA" > "$MANIFEST"

if [ ! -s "$MANIFEST" ]; then
  echo "*** ERROR: failed to emit $MANIFEST. ***"
  exit 1
fi

if [ "$CONFIG" = "Shipping" ] || [ "${PF_REQUIRE_PROTOCOL_BUMP:-}" = "1" ]; then
  mkdir -p "$(dirname "$PROTOSTAMP")"
  printf '%s\n' "$PROTO" > "$PROTOSTAMP"
fi
echo "=== Done + scrubbed. Publish directory: $PUBLISH_DIR ==="
echo "=== Manifest: $MANIFEST ==="
echo "NOTE: it's unsigned — first launch needs right-click > Open (Gatekeeper). See packaging.md."
