# Online module review — Pass 2

| Date | 2026-07-23 |
| Status | done |

## Summary

`Source/CombatForge/Online/` is one subsystem wearing two hats: player device-link auth + directory/profile/unlocks/casual XP, and fleet server registration/heartbeat/HMAC match reports. Lifecycle is careful (generation-gated device flow, weak-this HTTP callbacks, auth file under user settings not package `Saved/`, fleet key from persistent server data dir, 409 heartbeat re-register, pending-report crash queue, join-code path sanitization, proper JSON for heartbeats/casual reports). Weapon unlock policy (rank authoritative + unlock list additive; offline ungated) matches the documented stage. No blockers or majors in current source.

## Findings

### 1. Fleet match-report eligibility is `IsFleetActive()` (registered), not “has server key”
- **Severity:** minor
- **Status:** open
- **File:** `/home/josh/projects/combatforge/Source/CombatForge/Online/PFBackendSubsystem.h:130` (API); recovery window `/home/josh/projects/combatforge/Source/CombatForge/Online/PFBackendSubsystem.cpp:799`
- **Symptom:** During a heartbeat-409 recovery (or if register never succeeded), `bFleetRegistered` is false. GameMode gates `SendMatchReport` / pending-file write on `IsFleetActive()`, so a live key-bearing box can skip the fleet XP path for a match that ends in that window (dedicated usually also has no player login → no casual fallback either).
- **Why:** `IsFleetActive()` mirrors registration, not key presence. `SendMatchReport` itself only requires `ServerKey`, and re-register correctly re-sends *existing* pending files — but nothing is queued if GameMode never wrote one.
- **Fix:** Expose `HasFleetKey()` (or document that EmitMatchReport should use key presence + queue always when key set). On 409, keep accepting match reports while re-registering.
- **Confidence:** med
- **Source:** this-pass

### 2. Concurrent device-token polls can overlap
- **Severity:** minor
- **Status:** open
- **File:** `/home/josh/projects/combatforge/Source/CombatForge/Online/PFBackendSubsystem.cpp:298`
- **Symptom:** Slow network + short interval → multiple in-flight `/device/token` calls (API spam; rare double success handling).
- **Why:** Ticker always calls `PollDeviceToken` → `Request` without an in-flight lock; generation cancels after stop, but mid-poll concurrency is allowed.
- **Fix:** `bTokenPollInFlight` gate, or only re-arm the ticker from the poll completion callback (RFC slow_down already re-arms).
- **Confidence:** med
- **Source:** this-pass

### 3. Cancel/stop leaves `VerificationUri` populated
- **Severity:** nit
- **Status:** open
- **File:** `/home/josh/projects/combatforge/Source/CombatForge/Online/PFBackendSubsystem.cpp:355`
- **Symptom:** UI reading `GetVerificationUri()` after cancel may show the previous approval URL until the next successful code response.
- **Why:** `StopDevicePolling` clears codes/ticker/generation but not `VerificationUri`.
- **Fix:** `VerificationUri.Reset()` in `StopDevicePolling`.
- **Confidence:** high
- **Source:** this-pass

### 4. Session token stored as plaintext JSON
- **Severity:** nit
- **Status:** open
- **File:** `/home/josh/projects/combatforge/Source/CombatForge/Online/PFBackendSubsystem.cpp:166`
- **Symptom:** Anyone with filesystem access to the user profile can reuse the session until expiry/revoke.
- **Why:** Expected for a local game client session file; path correctly avoids packaging the token into redistributable builds (`UserSettingsDir`).
- **Fix:** Optional DPAPI/keychain later; ensure logout always deletes (already does).
- **Confidence:** high
- **Source:** this-pass

### 5. Alpha `pf.SetRank` still shipped in Online module
- **Severity:** nit
- **Status:** open
- **File:** `/home/josh/projects/combatforge/Source/CombatForge/Online/PFBackendSubsystem.cpp:906`
- **Symptom:** Local menu unlock override for catalog testing (`ECVF_Cheat`).
- **Why:** Explicitly marked DELETE BEFORE BETA; fleet re-clamps kit server-side. Not a production exploit on official servers.
- **Fix:** Strip `DevRankOverride` + command before beta (search “DevRankOverride” as documented).
- **Confidence:** high
- **Source:** this-pass

## Subsystems audited clean

| Subsystem | Notes |
|-----------|--------|
| **Config / init** | ApiBase from ini + `-PFApi=`; key from `-PFServerKey=` then persistent `ServerDataDir/ServerKey.txt`; auth load + profile refresh. |
| **Device login** | Double-start blocked; generation invalidates stale HTTP; `verification_uri` from API; slow_down re-arms ticker; cancel safe. |
| **Profile / unlocks** | 401 demotes cleanly; rank-first unlock; empty unlocks / offline fully ungated by design; DevRankOverride client-only. |
| **Directory** | Parse server rows; quickplay net protocol query; join-code alnum strip (path injection defense). |
| **LAN join** | `JoinAddress` prefers same-/24 LAN addr to avoid hairpin. |
| **Fleet register** | Key is trust boundary (dedicated + listen pilot); private match join code file; pending report resend after register. |
| **Heartbeat** | Re-resolves GameState; human count excludes bots/phantom; JSON body (safe for free-text map labels); unregisters when no longer hosting; 409 → re-register. |
| **Match report** | HMAC-SHA256 over `ts.body`; 2xx delete pending; 4xx quarantine; 5xx leave for retry. |
| **Casual report** | Player Bearer only; proper JSON; no-op when logged out. |
| **HTTP plumbing** | 15s timeout; AuthMode 0/1/2; weak-this on long-lived flows. |

## File checklist

| File | Reviewed | Notes |
|------|----------|--------|
| `Source/CombatForge/Online/PFBackendSubsystem.h` | yes | Dual-hat API; profile; fleet; alpha rank note |
| `Source/CombatForge/Online/PFBackendSubsystem.cpp` | yes | Full auth/fleet/directory; P2-ON1–ON5 |

**Counts:** 0 blocker · 0 major · 2 minor · 3 nit
