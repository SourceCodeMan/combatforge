#!/usr/bin/env bash
# CombatForge Mac environment preflight — detects the recurring breakages on this
# machine that otherwise surface as cryptic build errors or editor-startup asserts.
# READ-ONLY: prints PASS/FAIL per check plus the exact fix command; it never
# modifies, deletes, or writes anything. Safe to run at any time.
#
# Usage:
#   ./Scripts/check-mac-env.command              exit 0 if all PASS, 1 if any FAIL
#   ./Scripts/check-mac-env.command --warn-only  report but always exit 0 (for hooks)
#
# Callers that hook this can honor CF_SKIP_PREFLIGHT=1 to bypass it.
# Background + release workflow: docs/packaging.md. Failure modes covered:
#   1. Epic Launcher Verify/update reverts Apple_SDK.json MaxVersion
#      -> "Platform Mac is not a valid platform to build"
#   2. Incomplete engine install drops MacTargetPlatform dylibs
#      -> editor asserts check(TargetPlatform) at GlobalShader.cpp:384
#   3. Poisoned project module manifest (non-project entries) -> same assert
#   4. Metal Toolchain missing after an Xcode update -> shader compiles fail
#   5. LFS pointers reappear after content force-pushes -> assets missing/corrupt
#   6. Metal SM6 target disappears -> Nanite content cooks as broken fallback geometry
#
# Deliberately NOT `set -e`: every check must still run after an earlier failure.
set -uo pipefail
cd "$(dirname "${BASH_SOURCE[0]:-$0}")/.."   # script lives in Scripts/ -> project root

# Engine location on this Mac (matches Package-Mac.command; either env name is accepted).
CF_ENGINE="${CF_ENGINE:-${UE:-/Users/Shared/Epic Games/UE_5.6}}"
SDK_JSON="$CF_ENGINE/Engine/Config/Apple/Apple_SDK.json"
ENGINE_BIN="$CF_ENGINE/Engine/Binaries/Mac"
MANIFEST="Binaries/Mac/UnrealEditor.modules"
ENGINE_INI="Config/DefaultEngine.ini"

WARN_ONLY=0
[ "${1:-}" = "--warn-only" ] && WARN_ONLY=1

FAILS=0
pass() { printf 'PASS  [%s] %s\n' "$1" "$2"; }
fail() { printf 'FAIL  [%s] %s\n' "$1" "$2"; FAILS=$((FAILS+1)); }
note() { printf '      %s\n' "$1"; }

finish() {
	echo
	if [ "$FAILS" -eq 0 ]; then
		echo "RESULT: all checks PASS — Mac environment is good."
		exit 0
	fi
	echo "RESULT: $FAILS check(s) FAILED — apply the fixes above, then re-run. (This script modified nothing.)"
	[ "$WARN_ONLY" = 1 ] && exit 0
	exit 1
}

echo "== CombatForge Mac preflight =="
echo "   Project: $PWD"
echo "   Engine:  $CF_ENGINE"
echo

# ------------------------------------------------------------------ 0. engine present
if [ ! -d "$CF_ENGINE" ]; then
	fail "0" "UE 5.6 not found at $CF_ENGINE"
	note "Install UE 5.6 from the Epic Games Launcher, or point CF_ENGINE at it."
	note "Remaining checks skipped."
	finish
fi

# ------------------------------------------------------------------ 1. Apple_SDK.json Xcode cap
# Launcher Verify / updates / hotfixes silently revert MaxVersion to the stock
# 16.9.0; with Xcode 26.x installed every UBT invocation then fails with
# "Platform Mac is not a valid platform to build".
XCODE_VER="$(xcodebuild -version 2>/dev/null | awk 'NR==1{print $2}')"
MAX_VER="$(grep -m1 '"MaxVersion":' "$SDK_JSON" 2>/dev/null | sed -E 's/.*"MaxVersion": *"([^"]+)".*/\1/')"
version_ge() {
	python3 - "$1" "$2" <<'PY'
import re
import sys

def parts(value):
    result = [int(item) for item in re.findall(r"\d+", value)[:4]]
    return tuple((result + [0, 0, 0, 0])[:4])

raise SystemExit(0 if parts(sys.argv[1]) >= parts(sys.argv[2]) else 1)
PY
}

if [ -z "$XCODE_VER" ]; then
	fail "1/6" "xcodebuild not answering — Xcode not selected? Cannot compare Xcode vs SDK cap."
	note "Fix: sudo xcode-select -s /Applications/Xcode.app   (then re-run)"
elif [ -z "$MAX_VER" ]; then
	fail "1/6" "could not read MaxVersion from $SDK_JSON"
	note "File missing or reshaped by an engine update — inspect it by hand."
elif version_ge "$MAX_VER" "$XCODE_VER"; then
	pass "1/6" "Apple_SDK.json MaxVersion $MAX_VER >= Xcode $XCODE_VER"
else
	fail "1/6" "Apple_SDK.json MaxVersion $MAX_VER < installed Xcode $XCODE_VER — Launcher Verify/update reverted it"
	note "Symptom: 'Platform Mac is not a valid platform to build'."
	note "Fix (NO sudo needed — the file is user-writable):"
	note "  sed -i '' -E 's/(\"MaxVersion\": *\")[0-9.]+/\\126.9.0/' \"$SDK_JSON\""
	note "This is a known recurring chore: EVERY Launcher Verify/update reverts it. Re-run this script after any Launcher operation."
fi

# ------------------------------------------------------------------ 2. engine MacTargetPlatform dylibs
# An incomplete Launcher install silently omits these; the editor then asserts
# check(TargetPlatform) at GlobalShader.cpp:384 / StaticMesh.cpp:6821 on startup.
MISSING=""
for d in UnrealEditor-MacTargetPlatform.dylib \
         UnrealEditor-MacTargetPlatformControls.dylib \
         UnrealEditor-MacTargetPlatformSettings.dylib; do
	[ -f "$ENGINE_BIN/$d" ] || MISSING="$MISSING $d"
done
if [ -z "$MISSING" ]; then
	pass "2/6" "engine MacTargetPlatform dylibs present in Engine/Binaries/Mac"
else
	fail "2/6" "incomplete engine install — missing:$MISSING"
	note "Symptom: editor asserts check(TargetPlatform) at GlobalShader.cpp:384 on startup."
	note "Fix: Epic Games Launcher -> Library -> UE 5.6 dropdown -> Verify."
	note "WARNING: Verify also reverts Apple_SDK.json (check 1) — re-run this script when Verify finishes."
fi

# ------------------------------------------------------------------ 3. project module manifest purity
# A stale/poisoned Binaries/Mac/UnrealEditor.modules listing engine modules
# (e.g. MacTargetPlatform) makes the module manager resolve them to nonexistent
# PROJECT dylibs and never fall back to the engine -> same assert as check 2.
# A healthy manifest lists ONLY project modules ("CombatForge").
if [ ! -f "$MANIFEST" ]; then
	pass "3/6" "no $MANIFEST yet (editor not built — it will offer to compile; not a poisoning issue)"
else
	EXTRA="$(python3 -c 'import json,sys; m=json.load(open(sys.argv[1])); print(" ".join(sorted(set(m.get("Modules",{}))-{"CombatForge"})))' "$MANIFEST" 2>/dev/null)"
	PYRC=$?
	if [ "$PYRC" -ne 0 ]; then
		fail "3/6" "$MANIFEST is unparseable JSON (or python3 unavailable)"
		note "Inspect it: cat $MANIFEST — expect a Modules map containing only 'CombatForge'."
	elif [ -z "$EXTRA" ]; then
		pass "3/6" "$MANIFEST lists only project modules (CombatForge)"
	else
		fail "3/6" "module manifest poisoned — non-project entries: $EXTRA"
		note "Symptom: editor asserts check(TargetPlatform) at GlobalShader.cpp:384 on startup."
		note "Fix: full clean + rebuild of the project (NOT the engine):"
		note "  rm -rf Binaries/Mac Intermediate/Build/Mac Intermediate/ProjectFiles"
		note "  \"$CF_ENGINE/Engine/Build/BatchFiles/Mac/Build.sh\" CombatForgeEditor Mac Development -project=\"$PWD/CombatForge.uproject\""
	fi
fi

# ------------------------------------------------------------------ 4. Metal Toolchain
# Xcode updates can drop the Metal Toolchain component; shader compilation then
# fails at editor/cook time.
if TOOL="$(xcrun -f metallib 2>/dev/null)"; then
	pass "4/6" "Metal Toolchain OK (metallib -> $TOOL)"
else
	fail "4/6" "xcrun cannot resolve metallib — Metal Toolchain missing (common after Xcode updates)"
	note "Symptom: shader compilation errors / editor wedged on 'Compiling Shaders'."
	note "Fix: xcodebuild -downloadComponent MetalToolchain   (then re-run this script)"
fi

# ------------------------------------------------------------------ 5. LFS hydration
# Content force-pushes can leave tracked binaries as LFS pointer stubs; assets
# then load as missing/corrupt. In `git lfs ls-files`, '-' = pointer, '*' = real.
if ! git rev-parse --git-dir >/dev/null 2>&1; then
	fail "5/6" "$PWD is not a git repository — cannot check LFS state"
elif ! git lfs version >/dev/null 2>&1; then
	fail "5/6" "git-lfs is not installed"
	note "Fix: brew install git-lfs && git lfs install && git lfs pull"
else
	POINTERS="$(git lfs ls-files 2>/dev/null | awk '$2=="-"' | wc -l | tr -d ' ')"
	if [ "$POINTERS" = "0" ]; then
		TOTAL="$(git lfs ls-files 2>/dev/null | wc -l | tr -d ' ')"
		pass "5/6" "LFS content hydrated ($TOTAL tracked files, 0 pointer stubs)"
	else
		fail "5/6" "$POINTERS LFS-tracked files are un-hydrated pointer stubs (content force-push?)"
		note "First few:"
		git lfs ls-files 2>/dev/null | awk '$2=="-"' | sed 's/^[^ ]* . /        /' | head -5
		note "Fix: git lfs pull"
	fi
fi

# ------------------------------------------------------------------ 6. project Metal target
if grep -Fqx '+TargetedRHIs=SF_METAL_SM6' "$ENGINE_INI" 2>/dev/null \
	&& grep -Fqx 'MetalLanguageVersion=8' "$ENGINE_INI" 2>/dev/null; then
	pass "6/6" "project targets Metal SM6 / Metal 3 for the M2+ Nanite build"
else
	fail "6/6" "project is not pinned to Metal SM6 / Metal 3"
	note "Pull the release branch again; Config/DefaultEngine.ini should carry the MacTargetSettings block."
fi

finish
