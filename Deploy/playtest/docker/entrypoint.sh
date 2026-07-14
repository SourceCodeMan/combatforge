#!/usr/bin/env bash
set -euo pipefail

MAP="${PF_MAP:-/Game/Maps/L_Graybox}"
PORT="${PF_PORT:-7777}"

# Find server binary in common UAT layouts
BIN=""
for c in \
	"/server/CombatForgeServer" \
	"/server/CombatForge/Binaries/Linux/CombatForgeServer" \
	"/server/LinuxServer/CombatForge/Binaries/Linux/CombatForgeServer" \
	"/server/Binaries/Linux/CombatForgeServer"
do
	if [[ -x "$c" ]]; then BIN="$c"; break; fi
done

if [[ -z "$BIN" ]]; then
	echo "CombatForgeServer not found under /server. Mount or COPY a Linux dedicated server build."
	echo "Contents of /server:"
	find /server -maxdepth 4 -type f 2>/dev/null | head -50
	exit 1
fi

echo "Starting $BIN  map=$MAP port=$PORT"
exec "$BIN" "$MAP" -log -port="$PORT" -NOHOMEDIR "$@"
