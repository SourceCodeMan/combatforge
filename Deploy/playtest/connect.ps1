# Friend-side helper: launch CombatForge and join a server.
# Copy this next to CombatForge.exe when you zip the client build.
param(
	[Parameter(Mandatory = $true)]
	[string]$Server,
	[int]$Port = 7777
)

$ErrorActionPreference = "Stop"
$Here = $PSScriptRoot
if (-not $Here) { $Here = Get-Location }

$Nested = Join-Path $Here "CombatForge\Binaries\Win64\CombatForge.exe"
$Root   = Join-Path $Here "CombatForge.exe"
if (Test-Path $Nested) { $Exe = $Nested }
elseif (Test-Path $Root) {
	if ((Get-Item $Root).Length -ge 10MB) { $Exe = $Root }
	else { throw "CombatForge.exe next to this script is the 166 KB bootstrap. Use CombatForge\Binaries\Win64\CombatForge.exe (or copy connect.ps1 next to that)." }
} else {
	$Hit = Get-ChildItem $Here -Recurse -Filter CombatForge.exe |
	       Sort-Object Length -Descending | Select-Object -First 1
	if (-not $Hit -or $Hit.Length -lt 10MB) { throw "CombatForge.exe not found near $Here" }
	$Exe = $Hit.FullName
}

$Target = if ($Server -match ":\d+$") { $Server } else { "${Server}:${Port}" }
Write-Host "Connecting to $Target ..."
Set-Location (Split-Path $Exe -Parent)
& $Exe $Target
