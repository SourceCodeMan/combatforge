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

$Exe = Join-Path $Here "CombatForge.exe"
if (-not (Test-Path $Exe)) {
	$Hit = Get-ChildItem $Here -Recurse -Filter "CombatForge.exe" -ErrorAction SilentlyContinue | Select-Object -First 1
	if ($Hit) { $Exe = $Hit.FullName }
}
if (-not (Test-Path $Exe)) { throw "CombatForge.exe not found near $Here" }

$Target = if ($Server -match ":\d+$") { $Server } else { "${Server}:${Port}" }
Write-Host "Connecting to $Target ..."
Set-Location (Split-Path $Exe -Parent)
& $Exe $Target
