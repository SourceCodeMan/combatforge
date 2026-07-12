# Launch packaged (or local) game as a LISTEN host — you play on this machine too.
param(
	[string]$ProjectRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path,
	[int]$Port = 7777,
	[string]$Map = "/Game/Maps/L_Graybox"
)

$ErrorActionPreference = "Stop"

$Candidates = @(
	(Join-Path $ProjectRoot "Packaged\Playtest\Windows\PaintForge.exe"),
	(Join-Path $ProjectRoot "Packaged\Playtest\Windows\PaintForge\Binaries\Win64\PaintForge.exe"),
	(Join-Path $ProjectRoot "Binaries\Win64\PaintForge.exe"),
	(Join-Path $ProjectRoot "Saved\StagedBuilds\Windows\PaintForge.exe"),
	(Join-Path $ProjectRoot "Saved\StagedBuilds\Windows\PaintForge\Binaries\Win64\PaintForge.exe")
)
$Exe = $null
foreach ($C in $Candidates) {
	if (Test-Path $C) { $Exe = (Resolve-Path $C).Path; break }
}
if (-not $Exe) {
	# deep scan staged/packaged
	foreach ($Root in @("Packaged\Playtest","Saved\StagedBuilds")) {
		$Hit = Get-ChildItem (Join-Path $ProjectRoot $Root) -Recurse -Filter "PaintForge.exe" -ErrorAction SilentlyContinue |
			Select-Object -First 1
		if ($Hit) { $Exe = $Hit.FullName; break }
	}
}
if (-not $Exe) { throw "PaintForge.exe not found. Package a client first (package-playtest.ps1)." }

# Listen URL: map?Listen -port=
$Url = "${Map}?Listen"
Write-Host "==> Listen host: $Exe"
Write-Host "    $Url  port=$Port"
Write-Host "    Friends: open <your-ip>:$Port"

$WorkDir = Split-Path $Exe -Parent
Set-Location $WorkDir
& $Exe $Url "-port=$Port" "-log"
