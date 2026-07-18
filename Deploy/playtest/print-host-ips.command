#!/usr/bin/env bash
# Print the addresses friends use to join this Mac's playtest server. Sibling of print-host-ips.ps1.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]:-$0}")"
source ./_common.sh
echo "=== CombatForge host addresses ==="
echo
cf_print_ips
