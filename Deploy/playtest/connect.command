#!/usr/bin/env bash
# macOS friend-side join helper. Sibling of connect.ps1.
#   Usage:  ./connect.command <server-ip>[:port]
# Finds a built/packaged CombatForge game binary and joins the given host.
# Env overrides: CF_PORT, CF_ENGINE.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]:-$0}")"
source ./_common.sh

SERVER="${1:-}"
if [ -z "$SERVER" ]; then
	echo "Usage: $(basename "$0") <server-ip>[:port]" >&2
	exit 1
fi
case "$SERVER" in
	*:*) TARGET="$SERVER" ;;
	*)   TARGET="$SERVER:$CF_PORT" ;;
esac

ROOT="$(cf_project_root)"
GAME="$(cf_game_binary "$ROOT" || true)"
if [ -z "${GAME:-}" ]; then
	EDITOR="$(cf_editor || true)"
	UPROJECT="$ROOT/CombatForge.uproject"
	[ -n "${EDITOR:-}" ] || { echo "ERROR: no CombatForge game binary or editor found." >&2; exit 1; }
	echo "Connecting (editor -game) to $TARGET ..."
	exec "$EDITOR" "$UPROJECT" "$TARGET" -game -log -windowed -ResX=1600 -ResY=900
fi

echo "Connecting to $TARGET ..."
exec "$GAME" "$TARGET" -log
