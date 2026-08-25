#!/bin/bash
# Validate and push the packaged macOS LAN-only Alpha to itch.io.
# Dry-run by default; pass --push only after the Shipping package and local launch smoke pass.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
CHANNEL="thathorseslayer/combatforge:mac-alpha"
BUILD_DIR=""
BUTLER_BIN="${BUTLER:-}"
PUSH=0
ALLOW_DEVELOPMENT=0

usage() {
  echo "Usage: ./Scripts/Push-Itch.command [--push] [--channel owner/game:channel] [--build-dir path] [--butler path] [--allow-development]"
}

while [ "$#" -gt 0 ]; do
  case "$1" in
    --push) PUSH=1 ;;
    --channel) shift; CHANNEL="${1:-}" ;;
    --build-dir) shift; BUILD_DIR="${1:-}" ;;
    --butler) shift; BUTLER_BIN="${1:-}" ;;
    --allow-development) ALLOW_DEVELOPMENT=1 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Unknown argument: $1"; usage; exit 2 ;;
  esac
  shift
done

if [ -z "$BUILD_DIR" ]; then
  APP_PATH=$(find "$PROJECT_ROOT/Packaged/Mac" -type d -name "CombatForge.app" -print -quit 2>/dev/null || true)
  if [ -n "$APP_PATH" ]; then BUILD_DIR=$(dirname "$APP_PATH"); fi
fi
if [ -z "$BUILD_DIR" ] || [ ! -d "$BUILD_DIR" ]; then
  echo "No packaged Mac build found. Run ./Scripts/Package-Mac.command Shipping first."
  exit 1
fi
BUILD_DIR="$(cd "$BUILD_DIR" && pwd)"

if [ -z "$BUTLER_BIN" ]; then
  BUTLER_BIN=$(command -v butler 2>/dev/null || true)
fi
if [ -z "$BUTLER_BIN" ]; then
  ITCH_BUTLER_ROOT="$HOME/Library/Application Support/itch/broth/butler/versions"
  if [ -d "$ITCH_BUTLER_ROOT" ]; then
    BUTLER_BIN=$(find "$ITCH_BUTLER_ROOT" -type f -name butler -perm -111 -print 2>/dev/null | sort | tail -1)
  fi
fi
if [ -z "$BUTLER_BIN" ] || [ ! -x "$BUTLER_BIN" ]; then
  echo "butler not found. Install/login with itch.io, put butler on PATH, or pass --butler <path>."
  exit 1
fi

HEADER="$PROJECT_ROOT/Source/CombatForge/CombatForge.h"
PROTO=$(sed -n 's/.*NetProtocol *= *\([0-9][0-9]*\).*/\1/p' "$HEADER" | head -1)
if [ -z "$PROTO" ]; then
  echo "Could not read PFBuild::NetProtocol from $HEADER."
  exit 1
fi
USER_VERSION="0.1.0-alpha.$PROTO"

MANIFEST="$BUILD_DIR/CombatForge-build.json"
if [ ! -f "$MANIFEST" ]; then
  echo "No CombatForge-build.json in $BUILD_DIR; this package did not finish the Shipping gate."
  exit 1
fi
CONFIG=$(/usr/bin/plutil -extract configuration raw -o - "$MANIFEST")
MANIFEST_PROTO=$(/usr/bin/plutil -extract netProtocol raw -o - "$MANIFEST")
EXEC_REL=$(/usr/bin/plutil -extract executable raw -o - "$MANIFEST")
EXPECTED_SHA=$(/usr/bin/plutil -extract executableSha256 raw -o - "$MANIFEST")
MANIFEST_GIT=$(/usr/bin/plutil -extract gitCommit raw -o - "$MANIFEST")
if [ "$ALLOW_DEVELOPMENT" -ne 1 ] && [ "$CONFIG" != "Shipping" ]; then
  echo "Refusing to publish a $CONFIG package. Cook Shipping, or pass --allow-development intentionally."
  exit 1
fi
if [ "$MANIFEST_PROTO" != "$PROTO" ]; then
  echo "Manifest protocol $MANIFEST_PROTO does not match source protocol $PROTO. Repackage this checkout."
  exit 1
fi
SOURCE_GIT=$(git -C "$PROJECT_ROOT" rev-parse HEAD)
if [ "$MANIFEST_GIT" != "$SOURCE_GIT" ]; then
  echo "Manifest commit $MANIFEST_GIT does not match checkout $SOURCE_GIT. Repackage or switch back to the packaged commit."
  exit 1
fi
case "$EXEC_REL" in
  /*|../*|*/../*|*/..) echo "Manifest executable escapes the package root: $EXEC_REL"; exit 1 ;;
esac
APP_EXEC="$BUILD_DIR/$EXEC_REL"
if [ ! -f "$APP_EXEC" ]; then
  echo "Manifest executable is missing: $APP_EXEC"
  exit 1
fi
ACTUAL_SHA=$(shasum -a 256 "$APP_EXEC" | awk '{print $1}')
if [ "$ACTUAL_SHA" != "$EXPECTED_SHA" ]; then
  echo "Executable SHA-256 does not match CombatForge-build.json; refusing a partial/stale upload."
  exit 1
fi
if find "$BUILD_DIR" -type d -name Saved | grep -q .; then
  echo "Saved/ runtime data is still present; refusing upload."
  exit 1
fi
if find "$BUILD_DIR" -type f \( -name Auth.json -o -name ServerKey.txt \) | grep -q .; then
  echo "Runtime credentials are still present; refusing upload."
  exit 1
fi
if find "$BUILD_DIR" \( -name '*.pdb' -o -name '*.dSYM' \) | grep -q .; then
  echo "Debug symbols are still present; refusing upload."
  exit 1
fi

echo "Build:   $BUILD_DIR"
echo "Channel: $CHANNEL"
echo "Version: $USER_VERSION (LAN-only Alpha)"
STATUS=$($BUTLER_BIN status "$CHANNEL")
echo "$STATUS"
if printf '%s\n' "$STATUS" | grep -Fq "$USER_VERSION"; then
  echo "$USER_VERSION is already on $CHANNEL. Bump PFBuild::NetProtocol for the next public build."
  exit 1
fi

if [ "$PUSH" -ne 1 ]; then
  echo "DRY RUN - all checks passed. Re-run with --push after the local launch/LAN smoke."
  exit 0
fi

$BUTLER_BIN push "$BUILD_DIR" "$CHANNEL" --userversion "$USER_VERSION"
$BUTLER_BIN status "$CHANNEL"
echo "macOS LAN-only Alpha is live at $USER_VERSION."
