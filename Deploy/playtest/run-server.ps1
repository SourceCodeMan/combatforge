# Launch a headless-style PaintForge server for LAN/VPN playtest.
#
# Uses the GAME binary with -server -nullrhi (Launcher UE cannot build TargetType.Server).
# Prefers PaintForgeServer.exe only if present (source-engine builds).
param(
	[string]$ProjectRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path,
	[int]$Port = 7777,
	[string]$Map = "/Game/Maps/L_Graybox",
	[switch]$NoFirewall,
	[string]$ExtraArgs = ""
)

$ErrorActionPreference = "Stop"

function Find-Exe([string]$Root, [string]$Name) {
	$Hits = @()
	foreach ($Rel in @(
		"Binaries\Win64\$Name",
		"Packaged\Playtest\Windows\$Name",
		"Packaged\Playtest\Windows\PaintForge\Binaries\Win64\$Name",
		"Packaged\Playtest\WindowsServer\$Name",
		"Saved\StagedBuilds\Windows\$Name",
		"Saved\StagedBuilds\Windows\PaintForge\Binaries\Win64\$Name"
	)) {
		$P = Join-Path $Root $Rel
		if (Test-Path $P) { return (Resolve-Path $P).Path }
	}
	foreach ($Scan in @(
		(Join-Path $Root "Packaged\Playtest"),
		(Join-Path $Root "Saved\StagedBuilds"),
		(Join-Path $Root "Binaries")
	)) {
		if (Test-Path $Scan) {
			$F = Get-ChildItem $Scan -Recurse -Filter $Name -ErrorAction SilentlyContinue | Select-Object -First 1
			if ($F) { return $F.FullName }
		}
	}
	return $null
}

$ServerExe = Find-Exe $ProjectRoot "PaintForgeServer.exe"
$GameExe = Find-Exe $ProjectRoot "PaintForge.exe"

if (-not $NoFirewall) {
	$RuleName = "PaintForge Playtest $Port"
	if (-not (Get-NetFirewallRule -DisplayName "$RuleName UDP" -ErrorAction SilentlyContinue)) {
		Write-Host "Adding firewall rules for TCP/UDP $Port (UAC prompt possible)..."
		try {
			Start-Process powershell -Verb RunAs -Wait -ArgumentList @(
				"-NoProfile", "-Command",
				"New-NetFirewallRule -DisplayName '$RuleName UDP' -Direction Inbound -Protocol UDP -LocalPort $Port -Action Allow -ErrorAction SilentlyContinue; " +
				"New-NetFirewallRule -DisplayName '$RuleName TCP' -Direction Inbound -Protocol TCP -LocalPort $Port -Action Allow -ErrorAction SilentlyContinue"
			)
		} catch {
			Write-Host "Firewall rule skipped — open port $Port manually if friends cannot join."
		}
	}
}

if ($ServerExe) {
	Write-Host "==> Dedicated server binary: $ServerExe"
	$Exe = $ServerExe
	$Args = @($Map, "-log", "-port=$Port", "-NOHOMEDIR")
} elseif ($GameExe) {
	Write-Host "==> Game binary as headless server: $GameExe"
	Write-Host "    (Launcher engine has no Server target — using -server -nullrhi)"
	$Exe = $GameExe
	# -server: dedicated-style authority, no local player
	# -nullrhi: no GPU window (still loads some modules; fine for LAN playtest)
	$Args = @($Map, "-server", "-nullrhi", "-nosound", "-log", "-port=$Port", "-NOHOMEDIR")
} else {
	Write-Host "No PaintForge.exe / PaintForgeServer.exe found."
	Write-Host "Package a build first, e.g. from editor or:"
	Write-Host "  .\Deploy\playtest\package-playtest.ps1"
	Write-Host "Or use an existing staged build under Saved\StagedBuilds\Windows\"
	throw "Server/game binary missing"
}

if ($ExtraArgs) { $Args += $ExtraArgs }

Write-Host "    Map=$Map  Port=$Port"
Write-Host "    Friends: open <your-ip>:$Port   (.\print-host-ips.ps1)"
Write-Host ""

$WorkDir = Split-Path $Exe -Parent
Set-Location $WorkDir
& $Exe @Args
