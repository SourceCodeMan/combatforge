# Collect crash / disconnect evidence from this PC into a zip for triage.
# Run on HOST after a bad session, and on any KID PC that "just closed".
#
# Usage:
#   .\Deploy\playtest\collect-crash-evidence.ps1
#   .\Deploy\playtest\collect-crash-evidence.ps1 -ProjectRoot D:\projects\combatforge
param(
	[string]$ProjectRoot = "",
	[string]$OutDir = ""
)

$ErrorActionPreference = "Stop"
if (-not $ProjectRoot) {
	$ProjectRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
}
if (-not $OutDir) {
	$OutDir = Join-Path $ProjectRoot "Saved\CrashEvidence"
}
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$Stamp = Get-Date -Format "yyyyMMdd_HHmmss"
$Machine = $env:COMPUTERNAME
$Dest = Join-Path $OutDir ("evidence_{0}_{1}" -f $Machine, $Stamp)
New-Item -ItemType Directory -Force -Path $Dest | Out-Null

function Copy-IfExists($Path, $Sub) {
	if (Test-Path $Path) {
		$Target = Join-Path $Dest $Sub
		New-Item -ItemType Directory -Force -Path $Target | Out-Null
		Copy-Item $Path -Destination $Target -Recurse -Force -ErrorAction SilentlyContinue
		Write-Host "  + $Path"
	}
}

Write-Host "==> Collecting crash evidence on $Machine"
Write-Host "    Project: $ProjectRoot"
Write-Host "    Out:     $Dest"

# Project Saved logs
Copy-IfExists (Join-Path $ProjectRoot "Saved\Logs") "ProjectLogs"
Copy-IfExists (Join-Path $ProjectRoot "Saved\ClientLogs") "ClientLogsShipped"
Copy-IfExists (Join-Path $ProjectRoot "Saved\Crashes") "ProjectCrashes"
Copy-IfExists (Join-Path $ProjectRoot "Saved\Config\CrashReportClient") "ProjectCRC"

# Packaged game Saved (kids install folder may be elsewhere — also search common spots)
$PackagedCandidates = @(
	(Join-Path $ProjectRoot "Packaged\Playtest\Windows\CombatForge\Saved"),
	(Join-Path $ProjectRoot "Packaged\Playtest\Windows\Saved"),
	"C:\Games\CombatForge\CombatForge\Saved",
	"C:\Games\CombatForge\Saved"
)
foreach ($P in $PackagedCandidates) {
	Copy-IfExists $P ("PackagedSaved_" + ($P -replace '[\\/:\s]', '_'))
}

# Engine / OS crash report client
Copy-IfExists "$env:LOCALAPPDATA\CrashReportClient\Saved\Logs" "LocalCRC_Logs"
Copy-IfExists "$env:LOCALAPPDATA\CrashReportClient\Saved\Reports" "LocalCRC_Reports"
Copy-IfExists "$env:LOCALAPPDATA\UnrealEngine\*\Saved\Crashes" "UE_Crashes"

# Machine notes
@(
	"Machine=$Machine",
	"User=$env:USERNAME",
	"Time=$Stamp",
	"ProjectRoot=$ProjectRoot",
	"OS=$([System.Environment]::OSVersion.VersionString)"
) | Set-Content (Join-Path $Dest "machine.txt")

$Zip = Join-Path $OutDir ("crash_evidence_{0}_{1}.zip" -f $Machine, $Stamp)
if (Test-Path $Zip) { Remove-Item $Zip -Force }
Compress-Archive -Path (Join-Path $Dest '*') -DestinationPath $Zip -Force
Write-Host "==> ZIP: $Zip"
Write-Host "Send that zip (host + each crashed kid PC) for triage."
