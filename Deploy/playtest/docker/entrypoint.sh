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

# This script is PID 1 in the container, and the kernel drops signals that PID 1 has no
# handler for -- so without an explicit trap `docker stop` is swallowed entirely, the server
# never gets a chance to POST /unregister, and it dies to the SIGKILL after the grace period.
# (The old `exec "$BIN" ...` did not need this: the game WAS PID 1. The restart loop replaced
# the exec, so the forwarding has to be done by hand.)
child=""
shutdown=0

forward_shutdown() {
	shutdown=1
	if [ -n "$child" ] && kill -0 "$child" 2>/dev/null; then
		echo "shutdown requested; forwarding SIGTERM to $BIN (pid $child)"
		kill -TERM "$child" 2>/dev/null || true
	fi
}
trap forward_shutdown TERM INT

echo "Starting $BIN  map=$MAP port=$PORT"
delay=3
while true; do
	start_ts=$(date +%s)
	"$BIN" "$MAP" -log -port="$PORT" -NOHOMEDIR "$@" &
	child=$!

	# A trapped signal makes `wait` return early (128+n) with the child STILL RUNNING, so keep
	# waiting until it is actually reaped. A real 128+n exit leaves no live pid, so the guard
	# below ends the loop rather than spinning.
	code=0
	wait "$child" || code=$?
	while [ "$code" -gt 128 ] && kill -0 "$child" 2>/dev/null; do
		code=0
		wait "$child" || code=$?
	done
	child=""

	end_ts=$(date +%s)
	runtime=$((end_ts - start_ts))
	if [ "$shutdown" -eq 1 ]; then
		echo "stopped on request after ${runtime}s (code=$code)"
		exit 0
	fi
	if [ "$runtime" -lt 60 ]; then
		delay=$((delay * 2))
		if [ "$delay" -gt 300 ]; then
			delay=300
		fi
	else
		delay=3
	fi
	echo "exited code=$code after ${runtime}s; restarting in ${delay}s"
	# Background + wait here too: bash defers a trap until the running FOREGROUND child
	# finishes, and only the `wait` builtin is interruptible. A plain `sleep 300` would
	# swallow the stop for the whole backoff and still end in a SIGKILL.
	sleep "$delay" &
	sleep_pid=$!
	wait "$sleep_pid" || true
	kill -TERM "$sleep_pid" 2>/dev/null || true
	if [ "$shutdown" -eq 1 ]; then
		echo "stopped on request during restart backoff"
		exit 0
	fi
done
