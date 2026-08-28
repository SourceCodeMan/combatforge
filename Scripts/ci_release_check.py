#!/usr/bin/env python3
"""Fast, engine-independent gates for a CombatForge release candidate."""

from __future__ import annotations

import fnmatch
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


def function_body(source: str, signature: str) -> str | None:
    """The function starting at `signature`, up to the first closing brace in column 0.

    Leans on this codebase's UE brace style (a function's final `}` is unindented). That is
    enough to ask "does THIS entry point carry the guard", which an occurrence count cannot:
    a count of >= N passes just as happily when a guard is deleted from one handler and
    duplicated into another, and it never names the one that regressed.

    Returns None when the signature is gone, so a rename fails the gate loudly instead of
    quietly checking nothing.
    """
    start = source.find(signature)
    if start == -1:
        return None
    end = source.find("\n}", start)
    return source[start:end] if end != -1 else source[start:]


def require_guarded(source: str, source_name: str, signatures: tuple[str, ...], guard: str) -> None:
    for signature in signatures:
        body = function_body(source, signature)
        short = signature.split("(")[0]
        if body is None:
            FAILURES.append(
                f"{source_name} no longer defines {short} - rename it in this gate or the guard "
                f"stops being checked"
            )
        elif guard not in body:
            FAILURES.append(f"{source_name}: {short} must fail closed on {guard}")


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
# Every dormant handler is still reachable from a stale widget binding, so each has to refuse on
# its own — checked by name rather than by counting guards file-wide.
require_guarded(
    menu,
    "PFLoadingMenuWidget.cpp",
    (
        "void UPFLoadingMenuWidget::RefreshOnlinePanel()",
        "void UPFLoadingMenuWidget::RebuildServerRows()",
        "void UPFLoadingMenuWidget::OnLoginClicked()",
        "void UPFLoadingMenuWidget::OnQuickPlayClicked()",
        "void UPFLoadingMenuWidget::OnServerListClicked()",
        "void UPFLoadingMenuWidget::OnNewMatchClicked()",
        "void UPFLoadingMenuWidget::OnJoinCodeClicked()",
        "void UPFLoadingMenuWidget::JoinBrowserRow(",
        "void UPFLoadingMenuWidget::JoinBackendServer(",
    ),
    "!PFBuild::OfficialServersEnabled",
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

# ---------------------------------------------------------------------------
# Cross-file agreement.
#
# Asserting a literal in one file only proves the literal is there. It cannot catch the failure
# that actually shipped on this branch: Push-Itch.command looked for "CombatForge.app" while
# Package-Mac.command had started producing "CombatForge-Mac-Shipping.app", and CI was green the
# whole time because it asserted the same wrong constant the script did. These checks compare two
# files to each other, so a value can only drift by breaking the pair.
# ---------------------------------------------------------------------------

# Every .app name Package-Mac.command can emit must be findable by Push-Itch.command's search.
mac_app_names = set()
for template in re.findall(r'^[ \t]*APP_NAME="([^"]+)"', mac_package, re.MULTILINE):
    if "$CONFIG" in template:
        mac_app_names.update(template.replace("$CONFIG", config) for config in ("Development", "Shipping"))
    elif "$" not in template:
        mac_app_names.add(template)
require(bool(mac_app_names), "Package-Mac.command no longer assigns a literal APP_NAME")
push_app_patterns = re.findall(r'-name "([^"]+\.app)"', mac_push)
require(bool(push_app_patterns), "Push-Itch.command no longer searches for a packaged .app")
for app_name in sorted(mac_app_names):
    require(
        any(fnmatch.fnmatchcase(app_name, pattern) for pattern in push_app_patterns),
        f"Push-Itch.command cannot find {app_name}, which Package-Mac.command produces "
        f"(its search patterns are {push_app_patterns})",
    )

# Both push scripts must publish to the same itch project. Which CHANNEL is correct is only
# knowable from the live itch page, so that stays a human check — see docs/itch-deploy.md.
push_projects = {
    label: re.search(r'"([A-Za-z0-9_-]+/[A-Za-z0-9_-]+):[A-Za-z0-9_-]+"', text)
    for label, text in (("Push-Itch.ps1", windows_push), ("Push-Itch.command", mac_push))
}
for label, match in push_projects.items():
    require(match is not None, f"{label} has no owner/game:channel target")
if all(push_projects.values()):
    projects = {label: match.group(1) for label, match in push_projects.items()}
    require(
        len(set(projects.values())) == 1,
        f"the two itch push scripts target different projects: {projects}",
    )

# The Domination target lives in the GameMode and is repeated as player-facing copy in two
# widgets. It has already drifted once (200 -> 150), which is the kind of thing players report
# as a scoring bug rather than a typo.
domination_match = re.search(r"int32 DominationTargetScore\s*=\s*(\d+)\s*;",
                             read("Source/CombatForge/Core/CombatForgeGameMode.h"))
require(domination_match is not None, "DominationTargetScore is no longer machine-readable")
if domination_match:
    target = domination_match.group(1)
    for widget in ("Source/CombatForge/UI/PFLoadingMenuWidget.cpp", "Source/CombatForge/UI/PFLobbyWidget.cpp"):
        require(
            f"first to {target}" in read(widget),
            f"{widget} does not tell the player 'first to {target}' - it disagrees with "
            f"DominationTargetScore",
        )

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
require_guarded(
    backend,
    "PFBackendSubsystem.cpp",
    (
        "void UPFBackendSubsystem::Initialize(",
        "void UPFBackendSubsystem::BeginDeviceLogin()",
        "void UPFBackendSubsystem::FetchProfile()",
        "bool UPFBackendSubsystem::IsWeaponUnlocked(",
        "void UPFBackendSubsystem::FetchServers(",
        "void UPFBackendSubsystem::RequestQuickPlay(",
        "void UPFBackendSubsystem::RequestJoinByCode(",
        "void UPFBackendSubsystem::FleetRegisterIfServer(",
    ),
    "!PFBuild::OfficialServersEnabled",
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

# Unbalanced conditional-compilation blocks, caught before a builder spends hours downloading
# assets and cooking only to fail at the compiler.
#
# What this does NOT catch, despite an earlier version of this comment claiming it: the malformed
# DPAPI branch in LoadAuthFromDisk. That block's #if/#else/#endif nesting is valid and its braces
# do balance — it is only mis-INDENTED — so no directive checker could ever have flagged it, and a
# linear brace counter would false-positive on any function whose braces span an #if/#else.
# Compiling is the only gate that proves C++ correct and GitHub's runners have no engine, so the
# real check is Scripts/run-tests.ps1 (module build + automation tests) run locally before a
# release is tagged. Treat this loop as a cheap pre-filter, not as C++ coverage.
directive_pattern = re.compile(r"^\s*#\s*(if|ifdef|ifndef|elif|else|endif)\b")
for relative in tracked:
    if relative.suffix.lower() not in {".h", ".hpp", ".c", ".cc", ".cpp"}:
        continue
    if not relative.parts or relative.parts[0] != "Source":
        continue
    stack: list[dict[str, int | bool]] = []
    # errors="replace": only '#' directives matter here, and one stray non-UTF-8 byte in an
    # unrelated source file should not take the whole gate down with a decode traceback.
    source_text = (ROOT / relative).read_text(encoding="utf-8", errors="replace")
    for line_number, line in enumerate(source_text.splitlines(), start=1):
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
