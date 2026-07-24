# Shared helpers for playtest host scripts. ASCII-only (PowerShell encoding safe).

function Get-ProjectRoot {
	param([string]$ScriptRoot)
	return (Resolve-Path (Join-Path $ScriptRoot "..\..")).Path
}

function Find-CombatForgeExe {
	param([string]$Root, [string]$Name = "CombatForge.exe")
	foreach ($Rel in @(
		"Binaries\Win64\$Name",
		"Packaged\Playtest\Windows\$Name",
		"Packaged\Playtest\Windows\CombatForge\Binaries\Win64\$Name",
		"Packaged\Playtest\WindowsServer\$Name",
		"Saved\StagedBuilds\Windows\$Name",
		"Saved\StagedBuilds\Windows\CombatForge\Binaries\Win64\$Name"
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
