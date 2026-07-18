# CombatForge - collect the server's logs ON THE BOX and upload them to the PC through the tunnel.
#
# Same tunnel flow as a deploy, just pointed the other way:
#   1. On the PC:  python serve-and-receive.py       (in Deploy\pilot\serve, replaces python -m http.server)
#                  cloudflared tunnel --url http://localhost:8000
#   2. On the box: .\Export-ServerLogs.ps1           (paste the cloudflare words)
#   3. The zip lands in Deploy\pilot\serve\inbox\ on the PC.
#
# NEVER uploads ServerKey.txt - that key mints XP and must not leave the box. See the scrub below.
#
# NOTE: this file is deliberately ASCII-only. Windows PowerShell 5.1 reads .ps1 as ANSI unless there is a
# BOM, so smart quotes / em-dashes turn into mojibake and break parsing.

param(
    [string]$Tunnel = "",                                   # full URL or just the random words
    [string]$Root   = "C:\Users\Administrator\Desktop",     # folder that CONTAINS the game folder
    [string]$GameFolderName = "Windows",                    # the extracted build folder
    [int]$TailMB = 25,                                      # per-file cap; bigger logs are tail-trimmed
    [switch]$KeepLocal                                      # keep the zip on the box after upload
)

$ErrorActionPreference = "Stop"
$GameDir = Join-Path $Root $GameFolderName
$DataDir = Join-Path $env:ProgramData "CombatForge"

function Say($msg, $color = "Gray") { Write-Host $msg -ForegroundColor $color }

Say ""
Say "=== CombatForge server log export ===" Cyan
Say ""

# ---- 1. Tunnel address ----
if (-not $Tunnel) {
    Say "Paste the cloudflared address from the PC window." Yellow
    Say "Full URL or just the words both work (e.g. brave-lions-run-fast)." DarkGray
    $Tunnel = Read-Host "Tunnel"
}
$Tunnel = $Tunnel.Trim().Trim('"').Trim("'")
if (-not $Tunnel) { throw "No tunnel address given." }
if ($Tunnel -notmatch '^https?://') {
    if ($Tunnel -notmatch '\.trycloudflare\.com') { $Tunnel = "$Tunnel.trycloudflare.com" }
    $Tunnel = "https://$Tunnel"
}
$Tunnel = $Tunnel.TrimEnd('/')
Say "Using: $Tunnel" Green

# ---- 2. Gather the log files ----
Say ""
Say "Collecting logs..." Gray

$Stamp   = Get-Date -Format "yyyyMMdd-HHmmss"
$StageDir = Join-Path $env:TEMP "cf-logs-$Stamp"
New-Item -ItemType Directory -Force -Path $StageDir | Out-Null

# Candidate sources. Start-Server-OnBox.ps1 pins the live log to <GameDir>\Logs\server.log via -ABSLOG;
# UE also keeps its own Saved\Logs and Crashes under the game folder.
$Sources = @(
    @{ Path = (Join-Path $GameDir "Logs");                      Label = "server" },
    @{ Path = (Join-Path $GameDir "CombatForge\Saved\Logs");    Label = "ue" },
    @{ Path = (Join-Path $GameDir "CombatForge\Saved\Crashes"); Label = "crashes" },
    @{ Path = $DataDir;                                         Label = "data" }
)

$Cap = $TailMB * 1MB
$Collected = 0
$Skipped = 0

foreach ($src in $Sources) {
    if (-not (Test-Path $src.Path)) { continue }
    $outDir = Join-Path $StageDir $src.Label
    New-Item -ItemType Directory -Force -Path $outDir | Out-Null

    Get-ChildItem $src.Path -Recurse -File -ErrorAction SilentlyContinue | ForEach-Object {
        $f = $_

        # SECURITY SCRUB: the server key mints XP. It must never leave the box.
        if ($f.Name -match '(?i)serverkey|\.pem$|\.key$|token') {
            Say ("  SKIP (secret): " + $f.Name) DarkYellow
            $Skipped++
            return
        }
        # Only text-ish logs + crash context; skip anything large and binary-ish we did not ask for.
        if ($f.Extension -notmatch '(?i)^\.(log|txt|json|ini|xml|csv)$') { return }

        $dest = Join-Path $outDir $f.Name
        # De-dup names across subfolders
        $i = 1
        while (Test-Path $dest) {
            $dest = Join-Path $outDir ("{0}_{1}{2}" -f $f.BaseName, $i, $f.Extension)
            $i++
        }

        if ($f.Length -le $Cap) {
            Copy-Item $f.FullName $dest -Force
        } else {
            # Tail-trim: keep the LAST $Cap bytes (the interesting end of a long-running server log).
            $fs = [System.IO.File]::Open($f.FullName, 'Open', 'Read', 'ReadWrite')
            try {
                $null = $fs.Seek(-$Cap, 'End')
                $buf = New-Object byte[] $Cap
                $read = $fs.Read($buf, 0, $Cap)
            } finally { $fs.Close() }
            $header = "=== TRUNCATED: original {0:N0} bytes, keeping last {1:N0} ===`r`n" -f $f.Length, $read
            [System.IO.File]::WriteAllText($dest, $header)
            $out = [System.IO.File]::Open($dest, 'Append', 'Write')
            try { $out.Write($buf, 0, $read) } finally { $out.Close() }
            Say ("  trimmed: " + $f.Name + (" ({0:N1} MB -> {1} MB)" -f ($f.Length / 1MB), $TailMB)) DarkGray
        }
        $Collected++
    }
}

if ($Collected -eq 0) {
    Remove-Item $StageDir -Recurse -Force -ErrorAction SilentlyContinue
    throw "No log files found under $GameDir - is -Root/-GameFolderName right?"
}

# A little context about the machine + what is running, so the evaluation is not log-only.
$ctx = Join-Path $StageDir "context.txt"
"CombatForge server log export" | Out-File $ctx -Encoding utf8
"when      : $(Get-Date -Format o)"        | Out-File $ctx -Append -Encoding utf8
"host      : $env:COMPUTERNAME"            | Out-File $ctx -Append -Encoding utf8
"gameDir   : $GameDir"                     | Out-File $ctx -Append -Encoding utf8
"dataDir   : $DataDir"                     | Out-File $ctx -Append -Encoding utf8
"files     : $Collected collected, $Skipped skipped as secret" | Out-File $ctx -Append -Encoding utf8
"uptimeSec : $([int]((Get-Date) - (Get-CimInstance Win32_OperatingSystem).LastBootUpTime).TotalSeconds)" |
    Out-File $ctx -Append -Encoding utf8
"" | Out-File $ctx -Append -Encoding utf8
"-- running CombatForge processes --" | Out-File $ctx -Append -Encoding utf8
$procs = Get-Process CombatForge* -ErrorAction SilentlyContinue
if ($procs) {
    $procs | Select-Object Id, ProcessName, StartTime,
        @{n='WorkingSetMB';e={[int]($_.WorkingSet64/1MB)}},
        @{n='CPUsec';e={[int]$_.CPU}} |
        Format-Table -AutoSize | Out-String | Out-File $ctx -Append -Encoding utf8
} else {
    "(none running)" | Out-File $ctx -Append -Encoding utf8
}

Say ("  " + $Collected + " file(s) staged.") Green

# ---- 3. Zip ----
Say ""
Say "Zipping..." Gray
$ZipName = "cf-serverlogs-$Stamp.zip"
$ZipPath = Join-Path $env:TEMP $ZipName
if (Test-Path $ZipPath) { Remove-Item $ZipPath -Force }
Add-Type -AssemblyName System.IO.Compression.FileSystem
[System.IO.Compression.ZipFile]::CreateFromDirectory($StageDir, $ZipPath)
$ZipMB = [math]::Round((Get-Item $ZipPath).Length / 1MB, 2)
Say ("  " + $ZipName + "  (" + $ZipMB + " MB)") Green

# ---- 4. Upload ----
Say ""
Say "Uploading to the PC..." Cyan
& curl.exe -sS --fail -T "$ZipPath" "$Tunnel/inbox/$ZipName"
if ($LASTEXITCODE -ne 0) {
    Say ""
    Say "  Upload FAILED (curl exit $LASTEXITCODE)." Red
    Say "  Check the PC is running serve-and-receive.py (plain http.server cannot accept uploads)," Yellow
    Say "  and that both the python and cloudflared windows are still open." Yellow
    Say "  The zip is kept at: $ZipPath" Yellow
    Remove-Item $StageDir -Recurse -Force -ErrorAction SilentlyContinue
    throw "Aborting."
}

Say ""
Say "=== Done ===" Cyan
Say ("Landed on the PC at: Deploy\pilot\serve\inbox\" + $ZipName) Green
Say ($Collected.ToString() + " file(s), " + $ZipMB + " MB, " + $Skipped + " skipped as secret.") Gray

Remove-Item $StageDir -Recurse -Force -ErrorAction SilentlyContinue
if (-not $KeepLocal) {
    Remove-Item $ZipPath -Force -ErrorAction SilentlyContinue
} else {
    Say "Local copy kept at: $ZipPath" DarkGray
}
