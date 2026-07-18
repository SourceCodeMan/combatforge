#!/usr/bin/env bash
# macOS headless playtest SERVER (no local player). Sibling of run-server.ps1.
#   Priority: built Development game (?listen -server -nullrhi) -> Unreal Editor -server
# VERIFIED cross-platform (run-server.ps1): on a GAME binary "-server" ALONE boots a
# non-listening standalone — the "?listen" on the map URL is what actually opens the port.
# Env overrides: CF_PORT, CF_MAP, CF_ENGINE.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]:-$0}")"
source ./_common.sh

ROOT="$(cf_project_root)"
UPROJECT="$ROOT/CombatForge.uproject"
[ -f "$UPROJECT" ] || { echo "ERROR: missing $UPROJECT" >&2; exit 1; }

GAME="$(cf_game_binary "$ROOT" || true)"
EDITOR="$(cf_editor || true)"

if [ -n "${GAME:-}" ] && [ "${1:-}" != "--editor" ]; then
	echo "==> CombatForge SERVER (game -server) — Map: $CF_MAP  Port: $CF_PORT"
	cf_print_ips
	echo "Ctrl+C or close the window to stop."
	exec "$GAME" "${CF_MAP}?listen" -server -nullrhi -nosound -log -port="$CF_PORT"
elif [ -n "${EDITOR:-}" ]; then
	echo "==> CombatForge SERVER (editor -server) — Map: $CF_MAP  Port: $CF_PORT"
	cf_print_ips
	echo "Ctrl+C or close the window to stop."
	exec "$EDITOR" "$UPROJECT" "$CF_MAP" -server -log -port="$CF_PORT" -nullrhi -nosound -nosplash -unattended
else
	echo "ERROR: no server host available. Build the game (Development, Mac) or install UE 5.6." >&2
	exit 1
fi
