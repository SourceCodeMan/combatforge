#!/usr/bin/env bash
set -euo pipefail

MAP="${PF_MAP:-/Game/Maps/L_Graybox}"
PORT="${PF_PORT:-7777}"

# Find server binary in common UAT layouts
BIN=""
for c in \
	"/server/PaintForgeServer" \
	"/server/PaintForge/Binaries/Linux/PaintForgeServer" \
	"/server/LinuxServer/PaintForge/Binaries/Linux/PaintForgeServer" \
	"/server/Binaries/Linux/PaintForgeServer"
do
	if [[ -x "$c" ]]; then BIN="$c"; break; fi
done

if [[ -z "$BIN" ]]; then
	echo "PaintForgeServer not found under /server. Mount or COPY a Linux dedicated server build."
	echo "Contents of /server:"
	find /server -maxdepth 4 -type f 2>/dev/null | head -50
	exit 1
fi

echo "Starting $BIN  map=$MAP port=$PORT"
exec "$BIN" "$MAP" -log -port="$PORT" -NOHOMEDIR "$@"
