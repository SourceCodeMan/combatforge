# CombatForge — itch.io alpha deployment runbook

*Written 2026-07-15. Target: friends-only alpha playtest over Tom's VPN within 24h.*

## Facts that shaped this plan

- **Package size ≈ 3.1 GB** (after `-nodebuginfo` dropped the 359 MB pdb). The itch.io
  **1 GB limit applies to the WEB uploader only** — `butler` pushes have no such cap and
  diff-upload future builds (only deltas go over the wire). So: butler, always.
- **Development config on purpose** for the alpha: keeps the console + `pf.*` cvars for
  live debugging, and `~` + `open <ip>` as a fallback join path. Shipping config is a
  post-alpha optimization (see size-trim section).
- **Join-by-IP already exists in the menu** (JOIN box + persisted last-IP), so VPN play
  needs zero code: LAN broadcast discovery dies over VPNs, direct IP connect works.

## One-time setup (Tom — ~10 minutes, cannot be automated)

1. **itch.io account** (if none): https://itch.io/register
2. **Create the project page**: Dashboard → *Upload new project*
   - Title `CombatForge`, slug `combatforge`, Kind: **Downloadable**, Classification: Game
   - Release status: **In development** (this is the alpha marker)
   - Pricing: *No payments* (or donations — pairs with the in-game Buy Me a Coffee button)
   - Save as **Draft** for now (note: Draft pages do NOT allow downloads — visibility
     changes to Restricted in step 5 below)
3. **Install butler**:
   - Download: https://broth.itch.zone/butler/windows-amd64/LATEST/archive/default
   - Unzip to `C:\butler`, add `C:\butler` to PATH
   - Verify: `butler -V`
4. **Login** (browser OAuth): `butler login`

## Push a build (repeatable — this is the whole release process)

```powershell
# from anywhere; pushes the DIRECTORY, not a zip (butler diffs + compresses on the wire)
# Do NOT hand-type the version. Save the block below as Scripts\Push-Itch.ps1 and run:
#   .\Scripts\Push-Itch.ps1 -Channel <ITCH_USERNAME>/combatforge:windows-alpha
butler status <ITCH_USERNAME>/combatforge:windows-alpha
```

### `Scripts\Push-Itch.ps1`

The alpha number lives in exactly ONE place — `PFBuild::NetProtocol` in
`Source\CombatForge\CombatForge.h` — because that constant is what the join handshake
actually gates on. Typing `--userversion` separately is how the itch page and the wire
protocol drift apart: the page says alpha.11 while the binary is still speaking 10, and
players see "update needed" on a build they just downloaded. This script derives the
version from the header and refuses to push a version the channel already has (which
means the header bump was forgotten).

```powershell
param(
  [Parameter(Mandatory=$true)][string]$Channel,
  [string]$BuildDir = "$PSScriptRoot\..\Packaged\Windows"
)
$ErrorActionPreference = 'Stop'

# Single source of truth: constexpr int32 NetProtocol = N;
$Header = Join-Path $PSScriptRoot '..\Source\CombatForge\CombatForge.h'
$m = Select-String -Path $Header -Pattern 'constexpr\s+int32\s+NetProtocol\s*=\s*(\d+)\s*;'
if (-not $m) { throw "Could not read NetProtocol from $Header - did the declaration change shape?" }
$Proto = [int]$m.Matches[0].Groups[1].Value
$UserVersion = "0.1.0-alpha.$Proto"
Write-Host "NetProtocol $Proto -> --userversion $UserVersion"

# Refuse to re-push a version already on the channel: that always means the header bump
# was skipped, and pushing anyway ships a binary that cannot talk to the live servers.
$existing = (& butler status $Channel) -join "`n"
if ($existing -match [regex]::Escape($UserVersion)) {
  throw "$UserVersion is ALREADY on $Channel. Bump PFBuild::NetProtocol in CombatForge.h first."
}

if (-not (Test-Path $BuildDir)) { throw "No packaged build at $BuildDir - run Scripts\Package-Windows.bat." }
if (Test-Path (Join-Path $BuildDir 'CombatForge\Saved')) { throw "Saved\ still present - privacy scrub did not run." }

& butler push $BuildDir $Channel --userversion $UserVersion
if ($LASTEXITCODE -ne 0) { throw "butler push failed ($LASTEXITCODE)" }
& butler status $Channel
```

- Channel name contains `windows` → itch auto-tags it as a Windows executable.
- First push creates the channel. Bump `--userversion` each push (`0.1.0-alpha.2`, …).
- **Before every push**: make sure `Packaged\Windows\CombatForge\Saved\` (playtest logs/
  crash dumps) has been deleted — the packaging step in this repo does this, but check.

## Friends-only visibility (step 5)

On the project Edit page: **Visibility → Restricted** + set a **page password**.
Share URL + password (no itch account needed to download). Per-person alternative:
Distribute → **Download keys** (one revocable URL per friend).
Do NOT use Public-unlisted (anyone with the link) and do NOT share the Draft link
(downloads blocked in Draft).

## Paste into the itch page description (friend install runbook)

```
HOW TO INSTALL (Windows)
1. Download + unzip anywhere.
2. Run the CombatForge.exe in the TOP folder (it installs the VC++ prereq on first run).
3. Windows SmartScreen will warn (unsigned alpha): More info -> Run anyway.

HOW TO PLAY TOGETHER (VPN)
1. Join Tom's Tailscale network (he'll invite you).
2. HOST (Tom): click HOST LAN GAME. ALLOW the Windows Firewall prompt (check BOTH boxes).
   Find your Tailscale IP: tailscale ip -4  (it's the 100.x.y.z one).
   NOTE: the in-game "friends join" label shows your LAN IP — send friends the 100.x IP instead.
3. FRIENDS: type that 100.x.y.z IP into the JOIN box on the main menu and click JOIN.
```

## Post-alpha size trim (deferred on purpose — needs recook + full in-game verification)

Potential cuts measured 2026-07-15 (uncompressed cooked sizes):
- `/Game/Survival_Character` force-cook → narrow to `Meshes` + used demo anims (~1.4 GB → ~50 MB)
- `/Game/QuantumCharacter` force-cook → narrow to `Mesh` + `Demo/Animations` + `Materials` (~730 MB → ~80 MB)
- `Industrial_Warehouse` in MapsToCook only feeds the default-OFF `pf.StreamWarehouseMap`
  cvar (~1.8 GB cooked) — keep for now so the option survives; drop when decided.
- SM5 shader archive (dual SM5+SM6 target) ~75 MB if all testers are on SM6-capable GPUs.
Verify after any trim: class menu cosmetics, all weapons, arena materials, both maps.

## Optional next-package code tweak

Make the hosting label VPN-aware: extend `GetLocalLanIp` (PFLoadingMenuWidget.cpp) to also
surface a 100.64.0.0/10 (Tailscale) / 25.x (Radmin) adapter address and render
"LAN <ip> / VPN <ip>". Pure UI, zero protocol change.
