# Copyright (c) 2026 Tom Chapman. All rights reserved.
#
# Build the game module and run the C++ automation tests headless.
#
# This is the gate Scripts/ci_release_check.py cannot be: GitHub's runners have no Unreal
# install, so the workflow can only check text. Compiling (with bWarningsAsErrors) and running
# the CombatForge.* automation tests is what actually proves the C++ correct, so run this before
# packaging a release.
#
#   .\Scripts\run-tests.ps1                     # build + run every CombatForge.* test
#   .\Scripts\run-tests.ps1 -Filter Grid        # just CombatForge.Grid.*
#   .\Scripts\run-tests.ps1 -SkipBuild          # re-run against the current binaries
#
# The editor must be CLOSED - it holds a lock on UnrealEditor-CombatForge.dll.
param(
	[string]$ProjectRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path,
	[string]$Engine      = "C:\Program Files\Epic Games\UE_5.6",
	[string]$Filter      = "CombatForge",
	[int]$TimeoutSec     = 900,
	[switch]$SkipBuild
)

$ErrorActionPreference = "Stop"

$UProject = Join-Path $ProjectRoot "CombatForge.uproject"
$BuildBat = Join-Path $Engine "Engine\Build\BatchFiles\Build.bat"
$EditorCmd = Join-Path $Engine "Engine\Binaries\Win64\UnrealEditor-Cmd.exe"
if (-not (Test-Path $UProject))  { throw "Project not found: $UProject" }
if (-not (Test-Path $EditorCmd)) { throw "UnrealEditor-Cmd not found: $EditorCmd" }

if (-not $SkipBuild) {
	if (-not (Test-Path $BuildBat)) { throw "Build.bat not found: $BuildBat" }
	Write-Host "==> Building CombatForgeEditor (Win64 Development)" -ForegroundColor Cyan
	& $BuildBat CombatForgeEditor Win64 Development -project="$UProject" -waitmutex
	if ($LASTEXITCODE -ne 0) { throw "Build failed ($LASTEXITCODE) - fix the compile before running tests." }
}

# Fresh report dir each run: a stale index.json from a crashed run would otherwise be reported as
# this run's result.
$ReportDir = Join-Path $ProjectRoot "Saved\Automation"
if (Test-Path $ReportDir) { Remove-Item $ReportDir -Recurse -Force }
New-Item -ItemType Directory -Force -Path $ReportDir | Out-Null

# -nullrhi keeps it headless. Do NOT add -NoShaderCompile: it leaves the shader manager null and
# the editor dies on a TNotNull assert during FEngineLoop::Init, before any test runs.
$EditorArgs = @(
	"`"$UProject`"",
	"-ExecCmds=`"Automation RunTests $Filter; Quit`"",
	"-TestExit=`"Automation Test Queue Empty`"",
	"-ReportExportPath=`"$ReportDir`"",
	"-unattended", "-nopause", "-nosplash", "-nullrhi", "-NoLiveCoding", "-stdout"
) -join " "

Write-Host "==> Running automation tests matching '$Filter'" -ForegroundColor Cyan
$Proc = Start-Process -FilePath $EditorCmd -ArgumentList $EditorArgs -PassThru -NoNewWindow
if (-not $Proc.WaitForExit($TimeoutSec * 1000)) {
	$Proc | Stop-Process -Force -ErrorAction SilentlyContinue
	throw "Automation run did not finish within $TimeoutSec s."
}

# The editor's exit code is not a verdict - it exits 0 with tests still red. The report is.
$IndexPath = Join-Path $ReportDir "index.json"
if (-not (Test-Path $IndexPath)) {
	throw "No automation report at $IndexPath (editor exit $($Proc.ExitCode)). See Saved\Logs\CombatForge.log."
}
$Report = Get-Content -LiteralPath $IndexPath -Raw | ConvertFrom-Json

$Failed = 0
foreach ($Test in $Report.tests) {
	$State = "$($Test.state)"
	if ($State -eq "Success") {
		Write-Host ("  PASS  {0}" -f $Test.fullTestPath) -ForegroundColor Green
	} else {
		$Failed++
		Write-Host ("  {0}  {1}" -f $State.ToUpper(), $Test.fullTestPath) -ForegroundColor Red
		foreach ($Entry in $Test.entries) {
			if ("$($Entry.event.type)" -ne "Info") {
				Write-Host ("          {0}" -f $Entry.event.message) -ForegroundColor Red
			}
		}
	}
}

$Total = @($Report.tests).Count
Write-Host ""
if ($Total -eq 0) {
	throw "No tests matched '$Filter' - the filter is wrong, or the tests were compiled out."
}
if ($Failed -gt 0 -or [int]$Report.failed -gt 0) {
	Write-Host "FAIL: $Failed of $Total automation test(s) failed." -ForegroundColor Red
	Write-Host "      Full report: $IndexPath"
	exit 1
}
Write-Host "PASS: all $Total automation test(s) green." -ForegroundColor Green
Write-Host "      Full report: $IndexPath"
