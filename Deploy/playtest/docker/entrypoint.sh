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

mkdir -p /var/lib/combatforge
if [ -n "${PF_SERVER_KEY:-}" ]; then
	umask 077
	printf '%s' "$PF_SERVER_KEY" > /var/lib/combatforge/ServerKey.txt
fi

echo "Starting $BIN  map=$MAP port=$PORT"
delay=3
while true; do
	start_ts=$(date +%s)
	code=0
	"$BIN" "$MAP" -log -port="$PORT" -NOHOMEDIR "$@" || code=$?
	end_ts=$(date +%s)
	runtime=$((end_ts - start_ts))
	if [ "$runtime" -lt 60 ]; then
		delay=$((delay * 2))
		if [ "$delay" -gt 300 ]; then
			delay=300
		fi
	else
		delay=3
	fi
	echo "exited code=$code after ${runtime}s; restarting in ${delay}s"
	sleep "$delay"
done
