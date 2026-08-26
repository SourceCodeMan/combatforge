#!/usr/bin/env python3
"""Fast, engine-independent gates for a CombatForge release candidate."""

from __future__ import annotations

import json
import re
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
FAILURES: list[str] = []


def read(relative: str) -> str:
    return (ROOT / relative).read_text(encoding="utf-8")


def require(condition: bool, message: str) -> None:
    if not condition:
        FAILURES.append(message)


def ini_value(text: str, key: str) -> str | None:
    match = re.search(rf"^{re.escape(key)}[ \t]*=[ \t]*(.*?)[ \t]*$", text, re.MULTILINE)
    return match.group(1) if match else None


uproject = json.loads(read("CombatForge.uproject"))
require(uproject.get("EngineAssociation") == "5.6", "CombatForge.uproject must target Unreal Engine 5.6")
require(
    any(module.get("Name") == "CombatForge" and module.get("Type") == "Runtime"
        for module in uproject.get("Modules", [])),
    "CombatForge runtime module is missing from CombatForge.uproject",
)

header = read("Source/CombatForge/CombatForge.h")
require(
    re.search(r"constexpr\s+bool\s+OfficialServersEnabled\s*=\s*false\s*;", header) is not None,
    "Public Alpha must keep official servers disabled until the service is intentionally released",
)
protocol_match = re.search(r"constexpr\s+int32\s+NetProtocol\s*=\s*(\d+)\s*;", header)
require(protocol_match is not None, "PFBuild::NetProtocol is missing or no longer machine-readable")
if protocol_match:
    require(int(protocol_match.group(1)) >= 20, "Epic release candidates must use NetProtocol 20 or newer")

game_ini = read("Config/DefaultGame.ini")
for key, expected in {
    "BuildConfiguration": "PPBC_Shipping",
    "FullRebuild": "True",
    "ForDistribution": "True",
    "IncludeDebugFiles": "False",
    "bUseIoStore": "True",
    "UsePakFile": "True",
}.items():
    require(ini_value(game_ini, key) == expected, f"DefaultGame.ini must set {key}={expected}")

maps_to_cook = set(re.findall(r'^\+MapsToCook=\(FilePath="([^"]+)"\)\s*$', game_ini, re.MULTILINE))
for required_map in {"/Game/Maps/L_Graybox", "/Game/Scene_Warehouse/Maps/Industrial_Warehouse"}:
    require(required_map in maps_to_cook, f"Shipping cook list is missing map {required_map}")

directories_to_cook = set(
    re.findall(r'^\+DirectoriesToAlwaysCook=\(Path="([^"]+)"\)\s*$', game_ini, re.MULTILINE)
)
for required_dir in {
    "/Game/Audio",
    "/Game/Bandits",
    "/Game/MarketplaceBlockout",
    "/Game/RifleAnims",
    "/Game/Scene_Warehouse",
}:
    require(required_dir in directories_to_cook, f"Shipping cook list is missing {required_dir}")

engine_ini = read("Config/DefaultEngine.ini")
for key, expected in {
    "bEnablePlugin": "False",
    "bAllowNetworkConnection": "False",
    "SecurityToken": "",
    "bIncludeInShipping": "False",
    "bAllowExternalStartInShipping": "False",
}.items():
    require(ini_value(engine_ini, key) == expected, f"Android File Server must keep {key}={expected}")

package_script = read("Deploy/playtest/package-playtest.ps1")
for flag in ("-clean", "-distribution", "-iostore", "-compressed", "-nodebuginfo", "-prereqs"):
    require(f'"{flag}"' in package_script, f"Shipping package path is missing {flag}")
for artifact in ("Auth.json", "ServerKey.txt", "*.pdb", "CombatForge-build.json"):
    require(artifact in package_script, f"Shipping package safety gate is missing {artifact}")
require(
    re.search(r'if\s*\(\$Config\s*-eq\s*"Shipping"\).*?\$UatArgs\s*\+=', package_script, re.DOTALL)
    is not None,
    "Release-only UAT flags are no longer guarded by the Shipping configuration",
)
for invariant in ("AllowDirtyTree", ".last-packaged-protocol-windows", "lfs ls-files"):
    require(invariant in package_script, f"Windows Shipping source/LFS gate is missing {invariant}")

menu = read("Source/CombatForge/UI/PFLoadingMenuWidget.cpp")
for alpha_copy in (
    "NO OFFICIAL SERVERS AVAILABLE",
    "LAN / VPN play is available now",
    "Official servers are an upcoming feature",
    "Alpha v%d · LAN only",
):
    require(alpha_copy in menu, f"LAN-only Alpha menu copy is missing: {alpha_copy}")
require(
    re.search(r"if\s*\(PFBuild::OfficialServersEnabled\).*?QuickPlayBtn", menu, re.DOTALL) is not None,
    "Official Quick Play/server controls must remain behind OfficialServersEnabled",
)
require(
    "if (PFBuild::OfficialServersEnabled && SaveSlot > 0)" in menu,
    "LAN-only Alpha must not leave local class slots gated by the hidden account service",
)
require(
    menu.count("if (!PFBuild::OfficialServersEnabled)") >= 7,
    "Dormant official-server handlers must fail closed even if invoked outside the hidden UI",
)

windows_push = read("Scripts/Push-Itch.ps1")
for invariant in (
    "Packaged\\Release\\Windows",
    "CombatForge-build.json",
    "executableSha256",
    "configuration",
    "AllowDevelopment",
):
    require(invariant in windows_push, f"Windows itch Shipping gate is missing {invariant}")

mac_package = read("Scripts/Package-Mac.command")
for invariant in (
    'CONFIG="${1:-Shipping}"',
    "CombatForge-build.json",
    "executableSha256",
    "shasum -a 256",
    "check-mac-env.command",
    "CFBundleExecutable",
    "pak-less wrapper",
    ".last-packaged-protocol-mac",
):
    require(invariant in mac_package, f"Mac Shipping package gate is missing {invariant}")

mac_push = read("Scripts/Push-Itch.command")
for invariant in (
    "thathorseslayer/combatforge:osx-alpha",
    "CombatForge-build.json",
    "executableSha256",
    "ALLOW_DEVELOPMENT",
    "PUSH=0",
    "gitCommit",
):
    require(invariant in mac_push, f"Mac itch Shipping gate is missing {invariant}")
require("gitCommit" in windows_push, "Windows itch upload must bind the artifact to its Git commit")

require("+TargetedRHIs=SF_METAL_SM6" in engine_ini,
        "Mac Shipping must target Metal SM6 for the M2+ Nanite build")
require(ini_value(engine_ini, "MetalLanguageVersion") == "8",
        "Mac Shipping must target Metal 3 (MetalLanguageVersion=8)")

store_copy = read("docs/legal/store-copy.md")
require(
    "No official servers are currently available" in store_copy,
    "Store copy must disclose that official servers are unavailable",
)
require(
    "Official servers are upcoming" in store_copy,
    "Store copy must describe official servers as upcoming, not currently playable",
)

backend = read("Source/CombatForge/Online/PFBackendSubsystem.cpp")
require(
    backend.count("!PFBuild::OfficialServersEnabled") >= 6,
    "LAN-only Alpha must fail closed across backend initialization, unlocks, directory, and fleet paths",
)
require(
    "official service disabled for LAN-only Alpha" in backend,
    "LAN-only Alpha must make its dormant backend state explicit in logs",
)
require(
    re.search(
        r"#if\s+!UE_BUILD_SHIPPING\s+FParse::Value\([^\n]+PFServerKey=.*?#endif",
        backend,
        re.DOTALL,
    )
    is not None,
    "Shipping builds must not accept PFServerKey from the command line",
)
require("ServerKey.txt" in backend, "Fleet server keys must load from ServerKey.txt")
require("CryptProtectData" in backend and "CryptUnprotectData" in backend,
        "Windows player tokens must remain protected with DPAPI")
require("Crypt32.lib" in read("Source/CombatForge/CombatForge.Build.cs"),
        "CombatForge.Build.cs must link Crypt32.lib for DPAPI")

attributes = read(".gitattributes")
for content_dir in ("Bandits", "MarketplaceBlockout", "RifleAnims", "Scene_Warehouse"):
    require(
        re.search(rf"^Content/{re.escape(content_dir)}/?\*\*\s+filter=lfs\b", attributes, re.MULTILINE)
        is not None,
        f"Content/{content_dir} must remain Git LFS tracked",
    )

tracked_raw = subprocess.check_output(["git", "ls-files", "-z"], cwd=ROOT)
tracked = [Path(item.decode("utf-8", errors="surrogateescape")) for item in tracked_raw.split(b"\0") if item]

# UE compilation is the authoritative C++ gate, but catch structurally invalid conditional-compilation
# blocks before a Mac/Windows builder spends hours downloading assets and cooking. This would have caught
# the malformed DPAPI platform branch that the original syntax-only CI missed.
directive_pattern = re.compile(r"^\s*#\s*(if|ifdef|ifndef|elif|else|endif)\b")
for relative in tracked:
    if relative.suffix.lower() not in {".h", ".hpp", ".c", ".cc", ".cpp"}:
        continue
    if not relative.parts or relative.parts[0] != "Source":
        continue
    stack: list[dict[str, int | bool]] = []
    for line_number, line in enumerate(read(str(relative)).splitlines(), start=1):
        match = directive_pattern.match(line)
        if not match:
            continue
        directive = match.group(1)
        if directive in {"if", "ifdef", "ifndef"}:
            stack.append({"line": line_number, "else": False})
        elif directive == "elif":
            if not stack:
                FAILURES.append(f"{relative}:{line_number}: #{directive} without matching #if")
            elif stack[-1]["else"]:
                FAILURES.append(f"{relative}:{line_number}: #elif after #else")
        elif directive == "else":
            if not stack:
                FAILURES.append(f"{relative}:{line_number}: #else without matching #if")
            elif stack[-1]["else"]:
                FAILURES.append(f"{relative}:{line_number}: duplicate #else")
            else:
                stack[-1]["else"] = True
        elif stack:
            stack.pop()
        else:
            FAILURES.append(f"{relative}:{line_number}: #endif without matching #if")
    for frame in stack:
        FAILURES.append(f"{relative}:{frame['line']}: conditional block has no matching #endif")

for relative in tracked:
    lowered = [part.lower() for part in relative.parts]
    if relative.name.lower() in {"auth.json", "serverkey.txt", ".env", ".dev.vars"}:
        FAILURES.append(f"secret-bearing runtime file is tracked: {relative}")
    if any(part in {"saved", "packaged"} for part in lowered):
        FAILURES.append(f"generated/runtime directory is tracked: {relative}")

secret_patterns = {
    "private key": re.compile(rb"-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----"),
    "AWS access key": re.compile(rb"\b(?:AKIA|ASIA)[A-Z0-9]{16}\b"),
    "GitHub token": re.compile(rb"\bgh[pousr]_[A-Za-z0-9]{36,255}\b"),
    "OpenAI key": re.compile(rb"\bsk-(?:proj-)?[A-Za-z0-9_-]{20,}\b"),
    "Discord webhook": re.compile(rb"https://(?:discord(?:app)?\.com)/api/webhooks/\d+/[A-Za-z0-9._-]+"),
}
for relative in tracked:
    path = ROOT / relative
    try:
        if not path.is_file() or path.stat().st_size > 2_000_000:
            continue
        data = path.read_bytes()
    except OSError:
        continue
    if b"\0" in data:
        continue
    for label, pattern in secret_patterns.items():
        if pattern.search(data):
            FAILURES.append(f"possible {label} committed in {relative}")

if FAILURES:
    print("Release static checks failed:", file=sys.stderr)
    for failure in FAILURES:
        print(f"  - {failure}", file=sys.stderr)
    raise SystemExit(1)

print(f"PASS release static checks ({len(tracked)} tracked files, NetProtocol {protocol_match.group(1)})")
