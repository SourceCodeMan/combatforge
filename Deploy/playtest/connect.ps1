# Friend-side helper: launch PaintForge and join a server.
# Copy this next to PaintForge.exe when you zip the client build.
param(
	[Parameter(Mandatory = $true)]
	[string]$Server,
	[int]$Port = 7777
)

$ErrorActionPreference = "Stop"
$Here = $PSScriptRoot
if (-not $Here) { $Here = Get-Location }

$Exe = Join-Path $Here "PaintForge.exe"
if (-not (Test-Path $Exe)) {
	$Hit = Get-ChildItem $Here -Recurse -Filter "PaintForge.exe" -ErrorAction SilentlyContinue | Select-Object -First 1
	if ($Hit) { $Exe = $Hit.FullName }
}
if (-not (Test-Path $Exe)) { throw "PaintForge.exe not found near $Here" }

$Target = if ($Server -match ":\d+$") { $Server } else { "${Server}:${Port}" }
Write-Host "Connecting to $Target ..."
Set-Location (Split-Path $Exe -Parent)
& $Exe $Target
