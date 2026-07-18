#!/usr/bin/env bash
# macOS LISTEN host — this Mac is server AND a player (simplest playtest).
# Sibling of run-listen.ps1. Double-click in Finder, or run from a terminal.
#   Priority: built Development game (?listen) -> Unreal Editor -game ?listen
# Env overrides: CF_PORT, CF_MAP, CF_ENGINE.  First launch: macOS may prompt to
# allow incoming network connections — click "Allow" so friends can join.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]:-$0}")"
source ./_common.sh

ROOT="$(cf_project_root)"
UPROJECT="$ROOT/CombatForge.uproject"
[ -f "$UPROJECT" ] || { echo "ERROR: missing $UPROJECT" >&2; exit 1; }

LISTEN_URL="${CF_MAP}?listen"
GAME="$(cf_game_binary "$ROOT" || true)"
EDITOR="$(cf_editor || true)"

if [ -n "${GAME:-}" ] && [ "${1:-}" != "--editor" ]; then
	echo "==> CombatForge LISTEN host (game) — you play on this Mac"
	echo "    Exe:  $GAME"
	echo "    URL:  $LISTEN_URL   Port: $CF_PORT"
	cf_print_ips
	PROJ_ARG="$(cf_project_arg "$GAME" "$ROOT")"
	exec "$GAME" "$LISTEN_URL" -port="$CF_PORT" -log ${PROJ_ARG:+"$PROJ_ARG"}
elif [ -n "${EDITOR:-}" ]; then
	echo "==> CombatForge LISTEN host (editor -game) — you play on this Mac"
	echo "    URL:  $LISTEN_URL   Port: $CF_PORT"
	cf_print_ips
	exec "$EDITOR" "$UPROJECT" "$LISTEN_URL" -game -log -port="$CF_PORT" -windowed -ResX=1600 -ResY=900
else
	echo "ERROR: no host available. Build the game (Development, Mac) or install UE 5.6." >&2
	exit 1
fi
