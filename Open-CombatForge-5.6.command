#!/usr/bin/env bash
# macOS sibling of Open-CombatForge-5.6.bat — opens the project directly in the UE 5.6
# editor, bypassing the Epic Launcher's project browser (which can mis-offer to "convert").
# Double-click in Finder, or run from a terminal.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]:-$0}")"

# Preflight: catch the known recurring env breakages (Launcher reverting the SDK
# cap, missing engine dylibs, poisoned manifest, ...) BEFORE a cryptic failure.
if [ "${CF_SKIP_PREFLIGHT:-0}" != 1 ]; then
	./Scripts/check-mac-env.command || exit 1
fi

ENGINE="${CF_ENGINE:-/Users/Shared/Epic Games/UE_5.6}"
EDITOR="$ENGINE/Engine/Binaries/Mac/UnrealEditor.app/Contents/MacOS/UnrealEditor"
UPROJECT="$PWD/CombatForge.uproject"

[ -x "$EDITOR" ] || { echo "ERROR: UE 5.6 editor not found at $EDITOR" >&2; exit 1; }
[ -f "$UPROJECT" ] || { echo "ERROR: $UPROJECT not found" >&2; exit 1; }

echo "Opening CombatForge in UE 5.6 (Mac) ..."
exec "$EDITOR" "$UPROJECT"
