# Shared helpers for playtest host scripts. ASCII-only (PowerShell encoding safe).

function Get-ProjectRoot {
	param([string]$ScriptRoot)
	return (Resolve-Path (Join-Path $ScriptRoot "..\..")).Path
}

function Find-CombatForgeExe {
	param([string]$Root, [string]$Name = "CombatForge.exe")
	# Two traps, both of which made this return the wrong file or nothing at all:
	#
	#  1. A SHIPPING stage names the binary with its config suffix - CombatForge-Win64-Shipping.exe -
	#     while Development keeps the bare CombatForge.exe. Matching only the bare name found no
	#     Shipping binary anywhere, so a release package could not be located or smoke-booted.
	#  2. Every staged archive has a ~166 KB launcher STUB called CombatForge.exe at its root. It
	#     must be rejected at EVERY step, not just on the final winner: testing only at the end made
	#     a stub hit early in the list return $null outright, hiding a real binary found later.
	#
	# "$Base-Win64-*" rather than "$Base*": the latter would let a CombatForge.exe lookup return
	# CombatForgeServer.exe out of an archive that contains both.
	$Base = [IO.Path]::GetFileNameWithoutExtension($Name)
	$Patterns = @("$Base.exe", "$Base-Win64-*.exe")
	$IsClient = ($Base -eq "CombatForge")

	$Pick = {
		param($Items)
		$Items | Where-Object { $_ -and (-not $IsClient -or $_.Length -ge 10MB) } |
			Sort-Object Length -Descending | Select-Object -First 1
	}

	# Real Binaries\Win64 locations first; archive roots (where the stub lives) last.
	$Dirs = @(
		"Binaries\Win64",
		"Packaged\Release\Windows\CombatForge\Binaries\Win64",
		"Packaged\Playtest\Windows\CombatForge\Binaries\Win64",
		"Packaged\Playtest\WindowsServer\CombatForge\Binaries\Win64",
		"Saved\StagedBuilds\Windows\CombatForge\Binaries\Win64",
		"Packaged\Release\Windows",
		"Packaged\Playtest\Windows",
		"Packaged\Playtest\WindowsServer",
		"Saved\StagedBuilds\Windows"
	)
	foreach ($Rel in $Dirs) {
		$D = Join-Path $Root $Rel
		if (-not (Test-Path $D)) { continue }
		$Hits = @()
		foreach ($Pat in $Patterns) {
			$Hits += @(Get-ChildItem $D -Filter $Pat -File -ErrorAction SilentlyContinue)
		}
		$Win = & $Pick $Hits
		if ($Win) { return $Win.FullName }
	}

	# $Root last: callers such as smoke-package.ps1 pass an archive dir directly, so none of the
	# project-relative paths above exist under it.
	foreach ($Scan in @(
		(Join-Path $Root "Packaged"),
		(Join-Path $Root "Saved\StagedBuilds"),
		(Join-Path $Root "Binaries"),
		$Root
	)) {
		if (-not (Test-Path $Scan)) { continue }
		$Hits = @()
		foreach ($Pat in $Patterns) {
			$Hits += @(Get-ChildItem $Scan -Recurse -Filter $Pat -File -ErrorAction SilentlyContinue)
		}
		$Win = & $Pick $Hits
		if ($Win) { return $Win.FullName }
	}
	return $null
}

function Get-UnrealEditor {
	param([string]$EngineRoot = "C:\Program Files\Epic Games\UE_5.6")
	$Editor = Join-Path $EngineRoot "Engine\Binaries\Win64\UnrealEditor.exe"
	if (Test-Path $Editor) { return $Editor }
	return $null
}

function Ensure-PlaytestFirewall {
	param([int]$Port = 7777)
	$RuleUdp = "CombatForge Playtest $Port UDP"
	$RuleTcp = "CombatForge Playtest $Port TCP"
	# Check each rule independently (issue #21 D3): the old early-return on the UDP rule alone meant a
	# box that ever got only-UDP created never received the TCP rule on later runs.
	$NeedUdp = -not (Get-NetFirewallRule -DisplayName $RuleUdp -ErrorAction SilentlyContinue)
	$NeedTcp = -not (Get-NetFirewallRule -DisplayName $RuleTcp -ErrorAction SilentlyContinue)
	if (-not ($NeedUdp -or $NeedTcp)) {
		return
	}
	Write-Host "Opening firewall for TCP/UDP $Port (UAC prompt possible)..."
	$Cmds = @()
	if ($NeedUdp) { $Cmds += "New-NetFirewallRule -DisplayName '$RuleUdp' -Direction Inbound -Protocol UDP -LocalPort $Port -Action Allow -ErrorAction SilentlyContinue" }
	if ($NeedTcp) { $Cmds += "New-NetFirewallRule -DisplayName '$RuleTcp' -Direction Inbound -Protocol TCP -LocalPort $Port -Action Allow -ErrorAction SilentlyContinue" }
	try {
		Start-Process powershell -Verb RunAs -Wait -ArgumentList @(
			"-NoProfile", "-Command", ($Cmds -join "; ")
		)
	} catch {
		Write-Host "Firewall rule skipped - open port $Port manually if friends cannot join."
	}
}

function Write-JoinBanner {
	param([int]$Port = 7777)
	Write-Host ""
	Write-Host "Friends join with console (~):"
	Write-Host ("  open YOUR_LAN_IP:{0}" -f $Port)
	Write-Host "Or:  .\Deploy\playtest\connect.ps1 -Server YOUR_LAN_IP"
	Write-Host "Host IPs:"
	Get-NetIPAddress -AddressFamily IPv4 -ErrorAction SilentlyContinue |
		Where-Object { $_.IPAddress -notlike "127.*" } |
		ForEach-Object {
			$Tag = if ($_.InterfaceAlias -match "Tailscale|ZeroTier|WireGuard|VPN|Hamachi") { "VPN" } else { "LAN" }
			Write-Host ("  [{0}] {1}:{2}  ({3})" -f $Tag, $_.IPAddress, $Port, $_.InterfaceAlias)
		}
	Write-Host ""
}
