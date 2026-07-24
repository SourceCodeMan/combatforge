# Deploy + Config — Pass 2 | Date 2026-07-23 | Status done

## Summary

Playtest host helpers, smoke tests, pilot fleet deploy/export, and Default*.ini are production-aware: dual TCP/UDP firewall for playtest, ServerKey backup on redeploy, log secret redaction, write-only tunnel inbox, persistent ArenaDir outside wiped trees, Enhanced Input defaults for packaged clients, force-cook lists for soft-loaded content. Main issues: pilot home server still uses the GUI-exe `&` call pattern that the on-box launcher already fixed (process spawn storm); packaging config still stages unused loose Audio for dead MediaPlayer path; ProjectVersion cosmetic skew vs NetProtocol.

**Counts:** 0 blocker · 1 major · 6 minor · 3 nit

## Findings

### 1. Start-Pilot-Server restart loop does not wait on GUI exe
- **ID:** P2-D1
- **Severity:** major
- **Status:** open
- **File:** `Deploy/pilot/Start-Pilot-Server.ps1:54-60`
- **Symptom:** Restart loop launches a new `CombatForge.exe` every ~3s; multiple servers fight for port 7777 / CPU; pilot is unusable.
- **Why:** `& $Exe @args` on a Win64 **GUI-subsystem** game exe returns immediately (documented and fixed in `Start-Server-OnBox.ps1:101-133` via `Start-Process -PassThru` + `WaitForExit`). Pilot script never adopted that fix.
- **Fix:** Copy OnBox launch pattern (single argument string + `WaitForExit`); add crash-loop backoff already present on OnBox.
- **Confidence:** high
- **Source:** this-pass

### 2. DefaultGame still stages loose Audio NonUFS for dropped MediaPlayer path
- **ID:** P2-D2
- **Severity:** minor
- **Status:** open
- **File:** `Config/DefaultGame.ini:53-54`
- **Symptom:** Packaged builds stage loose `Content/Audio` files (legacy mp3s) that the game no longer plays; package size/noise.
- **Why:** Comment still says “Phase music MP3s (Suno) — … for MediaPlayer” while lines 35-38 correctly force-cook SoundWave assets and state MediaPlayer was dropped.
- **Fix:** Remove `+DirectoriesToAlwaysStageAsNonUFS=(Path="Audio")` unless another consumer needs loose files; keep `/Game/Audio` cook line.
- **Confidence:** high
- **Source:** this-pass

### 3. Deploy-OnBox stops the live server before download completes
- **ID:** P2-D3
- **Severity:** minor
- **Status:** open
- **File:** `Deploy/pilot/Deploy-OnBox.ps1:101-123`
- **Symptom:** Failed mid-download after stop leaves the box offline until manual recovery (probe only checks tunnel HEAD, not full zip integrity).
- **Why:** Order is: backup key → **stop processes** → download ~3.8 GB → extract. Reachability check reduces but does not eliminate failure window.
- **Fix:** Download to a temp name first, verify size/hash, then stop + extract; or stop only after download success.
- **Confidence:** high
- **Source:** this-pass

### 4. Pilot server firewall opens UDP only (no TCP rule)
- **ID:** P2-D4
- **Severity:** minor
- **Status:** open
- **File:** `Deploy/pilot/Start-Pilot-Server.ps1:27-35`, `Deploy/pilot/Start-Server-OnBox.ps1:69-78`
- **Symptom:** Rare join path failures if anything needs TCP on 7777; inconsistent with playtest `_common.ps1` which opens both.
- **Why:** Pilot/OnBox only `New-NetFirewallRule ... -Protocol UDP`.
- **Fix:** Match playtest helper: UDP + TCP inbound on `$Port`.
- **Confidence:** med
- **Source:** this-pass

### 5. smoke-package does not scrub Saved/pdb from archive
- **ID:** P2-D5
- **Severity:** minor
- **Status:** open
- **File:** `Deploy/playtest/smoke-package.ps1` (post-cook section ends ~65; no scrub) vs `package-playtest.ps1:56-62`
- **Symptom:** If an operator distributes the smoke archive path, runtime Saved (session tokens historically) or pdbs could ship.
- **Why:** Privacy scrub lives in package-playtest / Package-Windows but not smoke-package.
- **Fix:** Call the same scrub after cook, or document smoke archive as non-distributable only.
- **Confidence:** med
- **Source:** this-pass

### 6. ProjectVersion cosmetic drift vs alpha/NetProtocol
- **ID:** P2-D6
- **Severity:** minor
- **Status:** open
- **File:** `Config/DefaultGame.ini:7` (`ProjectVersion=0.1.0`)
- **Symptom:** Project Settings / about strings lag `PFBuild::NetProtocol` (15 / alpha.15).
- **Why:** Join safety uses NetProtocol; packaging ProjectVersion is independent and not auto-bumped.
- **Fix:** Optional sync note in packaging docs; do not dual-source the gate into Config.
- **Confidence:** high
- **Source:** this-pass

### 7. Docker server-linux tree is empty placeholder
- **ID:** P2-D7
- **Severity:** minor
- **Status:** open
- **File:** `Deploy/playtest/docker/server-linux/` (empty), `Dockerfile:20`, `entrypoint.sh:18-22`
- **Symptom:** `docker compose up --build` builds an image that exits 1 with “CombatForgeServer not found”.
- **Why:** Docs require a Linux dedicated cook dropped into server-linux; folder is empty in-repo (expected without source-engine Linux server cook). Easy to miss prereq.
- **Fix:** Fail Dockerfile COPY with a clear README sentinel file; or document “image will not start until …” more loudly in compose comments (partially present).
- **Confidence:** high
- **Source:** this-pass

### 8. collect-crash-evidence may capture secrets in logs without redaction
- **ID:** P2-D8
- **Severity:** minor
- **Status:** open
- **File:** `Deploy/playtest/collect-crash-evidence.ps1:38-53`
- **Symptom:** Zipped logs may contain session tokens / command lines if present; Export-ServerLogs redacts, this helper does not.
- **Why:** Blind copy of Saved logs + packaged Saved into a zip for triage.
- **Fix:** Reuse `Redact-Secrets` patterns from `Export-ServerLogs.ps1` on staged text files; never include ServerKey.txt.
- **Confidence:** med
- **Source:** this-pass

### 9. run-listen / run-server use bare `&` (acceptable; no restart loop)
- **ID:** P2-D9
- **Severity:** nit
- **Status:** open
- **File:** `Deploy/playtest/run-listen.ps1:76`, `run-server.ps1:91`
- **Symptom:** Script process may exit while game keeps running; Ctrl+C may not kill the game.
- **Why:** GUI exe + call operator; unlike pilot, there is no spawn storm.
- **Fix:** Optional `Start-Process -Wait` for cleaner host lifecycle.
- **Confidence:** high
- **Source:** this-pass

### 10. DefaultEditor.ini commits huge AdvancedPreviewScene profiles
- **ID:** P2-D10
- **Severity:** nit
- **Status:** open
- **File:** `Config/DefaultEditor.ini:11+`
- **Symptom:** Massive noise in diffs; not gameplay-critical.
- **Why:** Engine shared preview profiles serialized into project DefaultEditor.
- **Fix:** Ignore or strip if not intentionally customized; keep PlayNetMode standalone settings at top.
- **Confidence:** high
- **Source:** this-pass

### 11. package-playtest omits iostore/compressed/prereqs vs Package-Windows
- **ID:** P2-D11
- **Severity:** nit
- **Status:** open
- **File:** `Deploy/playtest/package-playtest.ps1:26-38` vs `Scripts/Package-Windows.bat:40-48`
- **Symptom:** Playtest packages may differ in size/layout from shipping bat packages.
- **Why:** Slimmer UAT flags for LAN friend zips — likely intentional.
- **Fix:** Document the intentional delta; align flags if friends need identical cook layout.
- **Confidence:** med
- **Source:** this-pass

## Subsystems audited clean

| Area | Notes |
|------|--------|
| **playtest/_common.ps1** | Robust exe discovery; dual firewall rules; join banner with VPN tagging. |
| **run-server.ps1** | Correct `?listen` + `-server -nullrhi` for game exe; ArenaDir under ProgramData; editor fallback. |
| **run-listen.ps1** | Clear listen URL; PreferEditor path. |
| **package-playtest.ps1** | Saved scrub + connect.ps1 copy into client tree. |
| **smoke-improvement / smoke-package** | Seed arenas under LOCALAPPDATA; SMOKE markers; soft-fail exit 3 on package when content unproven. |
| **Start-Server-OnBox.ps1** | Nested real exe (not bootstrapper); VC redist; ArenaDir; WaitForExit; crash-loop backoff; ABSLOG. |
| **Deploy-OnBox.ps1** | Auto zip discovery; ServerKey backup/restore; extract layout verification. |
| **Export-ServerLogs.ps1** | Secret skip + log redaction of PFServerKey/tokens; tail cap; PUT to tunnel. |
| **serve-and-receive.py** | Localhost bind; PUT /inbox/ only; inbox GET 403; size cap; safe names. |
| **DefaultEngine.ini** | Maps/GameMode/GI; net tick; collision channels; nav dynamic + invokers; NearClip 4; VT; stream pool; redirects PaintForge→CombatForge; AFS disabled. |
| **DefaultInput.ini** | Enhanced input default classes — required for packaged input. |
| **DefaultGame.ini cook lists** | Soft-load force-cook for Audio/Bandits/MarketplaceBlockout/etc. well documented. |
| **DefaultScalability.ini** | Only scalability-flagged cvars; method switches deferred to C++ prefs. |
| **DefaultEditor.ini top** | PIE standalone — no accidental LAN host on Play. |

## File checklist

| File | Reviewed |
|------|----------|
| `Deploy/playtest/_common.ps1` | yes |
| `Deploy/playtest/build-server.ps1` | yes |
| `Deploy/playtest/collect-crash-evidence.ps1` | yes |
| `Deploy/playtest/connect.ps1` | yes |
| `Deploy/playtest/package-playtest.ps1` | yes |
| `Deploy/playtest/print-host-ips.ps1` | yes |
| `Deploy/playtest/run-listen.ps1` | yes |
| `Deploy/playtest/run-server.ps1` | yes |
| `Deploy/playtest/smoke-improvement.ps1` | yes |
| `Deploy/playtest/smoke-package.ps1` | yes |
| `Deploy/playtest/start-host.bat` | yes |
| `Deploy/playtest/start-server.bat` | yes |
| `Deploy/playtest/README.md` | skim |
| `Deploy/playtest/docker/Dockerfile` | yes |
| `Deploy/playtest/docker/docker-compose.yml` | yes |
| `Deploy/playtest/docker/entrypoint.sh` | yes |
| `Deploy/playtest/docker/server-linux/` | empty placeholder |
| `Deploy/pilot/Deploy-OnBox.ps1` | yes |
| `Deploy/pilot/Export-ServerLogs.ps1` | yes |
| `Deploy/pilot/Start-Pilot-Client.ps1` | yes |
| `Deploy/pilot/Start-Pilot-Server.ps1` | yes |
| `Deploy/pilot/Start-Server-OnBox.ps1` | yes |
| `Deploy/pilot/serve/serve-and-receive.py` | yes |
| `Config/DefaultEditor.ini` | yes (PIE + note on profiles) |
| `Config/DefaultEngine.ini` | yes |
| `Config/DefaultGame.ini` | yes |
| `Config/DefaultInput.ini` | yes |
| `Config/DefaultScalability.ini` | yes |
