#!/usr/bin/env bash
# Shared helpers for the macOS playtest .command scripts (siblings of the Windows *.ps1).
# Sourced by run-listen.command / run-server.command / connect.command / print-host-ips.command.
# LF line endings enforced by .gitattributes (*.sh, *.command = eol=lf).

# Engine location on this Mac (matches Scripts/Package-Mac.command).
CF_ENGINE="${CF_ENGINE:-/Users/Shared/Epic Games/UE_5.6}"
CF_PORT="${CF_PORT:-7777}"
CF_MAP="${CF_MAP:-/Game/Maps/L_Graybox}"

# Absolute path to the project root (…/Deploy/playtest/ -> project root two levels up).
cf_project_root() {
	local here
	here="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" && pwd)"
	cd "$here/../.." && pwd
}

# The Mac editor binary inside the installed engine, or empty if missing.
cf_editor() {
	local ed="$CF_ENGINE/Engine/Binaries/Mac/UnrealEditor.app/Contents/MacOS/UnrealEditor"
	[ -x "$ed" ] && printf '%s' "$ed"
}

# A built CombatForge game binary (raw Development build or a staged/packaged .app), or empty.
cf_game_binary() {
	local root="$1"
	local candidates=(
		"$root/Binaries/Mac/CombatForge"
		"$root/Binaries/Mac/CombatForge.app/Contents/MacOS/CombatForge"
		"$root/Packaged/Mac/Mac/CombatForge.app/Contents/MacOS/CombatForge"
		"$root/Packaged/Mac/CombatForge.app/Contents/MacOS/CombatForge"
	)
	local c
	for c in "${candidates[@]}"; do
		[ -x "$c" ] && { printf '%s' "$c"; return 0; }
	done
	return 1
}

# Print this machine's join addresses (LAN + VPN), one per active interface.
cf_print_ips() {
	echo "Host IPs (share one with friends):"
	local i ip tag
	for i in $(ifconfig -l 2>/dev/null); do
		ip="$(ipconfig getifaddr "$i" 2>/dev/null)"
		[ -z "$ip" ] && continue
		case "$i" in
			utun*|tailscale*|wg*) tag="VPN" ;;
			*)                    tag="LAN" ;;
		esac
		printf '  [%s] %-6s %s:%s\n' "$tag" "$i" "$ip" "$CF_PORT"
	done
	echo
	echo "Friends join from the client console (~):  open YOUR_IP:$CF_PORT"
}
