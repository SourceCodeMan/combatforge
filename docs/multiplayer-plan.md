# CombatForge True Multiplayer — Architecture & Deploy Plan

*2026-07-17 · research pass: 4-agent deep dive (codebase audit + hosting + backend + compliance), run `wf_8af8416e-db1`. Plan only — nothing here is implemented yet.*

## 0. Executive summary

Move from player-hosted listen servers to a **centrally hosted USA dedicated fleet** with a
Battlefield-style server browser, quick-play, private matches, and **accounts on playcombatforge.com**.
Discord stays the only voice/chat (no in-game comms, by design — this also removes COPPA's hardest surface).

**Recommended architecture (everything in stacks we already run):**

```
 Player PC (itch build)                    Cloudflare (playcombatforge.com)
 ┌──────────────────────┐   HTTPS/JSON    ┌──────────────────────────────────┐
 │ CombatForge client   │───────────────▶ │ combatforge-api  (Worker + D1)   │
 │  · server browser UI │  list/quickplay │  · accounts (Better Auth ≥1.6.11)│
 │  · device-link login │  device-link    │  · device-link  /link            │
 │  · join = open ip:port│                │  · server directory (heartbeat)  │
 └──────────┬───────────┘                 │  · private-match join codes      │
            │ UDP 7777+  (UE netcode,     └───────────────▲──────────────────┘
            │  unchanged — already               heartbeat │ every 10s (bearer secret)
            ▼  server-authoritative)                       │
 ┌─────────────────────────────────────────────────────────┴───┐
 │ US VPS (OVHcloud VPS-2, Vint Hill VA — $8.50/mo)            │
 │  4–6 dedicated CombatForge instances (systemd, -port=7777+) │
 └─────────────────────────────────────────────────────────────┘
```

- **No EOS / no Online Subsystem** — keeps design decision D12. A fixed self-hosted fleet is discoverable
  with a plain REST registry; EOS solves crossplay/console P2P discovery, which is not this problem.
  (docs/eos-integration-notes.md stays valid for a *future* crossplay phase behind a thin facade.)
- **Accounts** = Better Auth on Workers + D1 with the official **Device Authorization plugin**: game shows a
  6-char code, player approves at `playcombatforge.com/link` in a browser. **Never a password field in UE.**
- **USA-only, one location** (Virginia): coast-to-coast worst case 60–90 ms — fine for a casual 12-player
  projectile shooter. Second box (Hillsboro/Dallas) only if the community skews West.

## 1. What we already have (audit findings — the migration is smaller than it looks)

The codebase was built for this without knowing it:

| Already true | Evidence |
|---|---|
| **`CombatForgeServer.Target.cs` exists** (TargetType.Server, correct shape) | `Source/CombatForgeServer.Target.cs` |
| **Headless boot already works** via game exe `-server -nullrhi -nosound -port=7777` | `Deploy/playtest/run-server.ps1` (+ README §"server") |
| Match loop is **~90% self-running**: Results→Lobby on 15s timer, Vote finalizes on timer, Lobby auto-starts when ≥2 humans ready | `CombatForgeGameMode::SetPhase` :608-857, `NotifyReadyChangedInternal` :886 |
| Gameplay fully **server-authoritative** (fire/build/interact/melee/bomb all server-validated) | security audit, 22 gated Server RPCs |
| Every cosmetic system already guards `NM_DedicatedServer` | PFCombatAudio/VFX/Splat/Smoke/Lighting/WarehouseStream/ArenaShell drape/build ghost |
| Net-version gate exists (stale clients rejected cleanly) | `PFBuild::NetProtocol` → FNetworkVersion override |
| Join-by-IP path is exactly how clients reach a dedicated server | `open <ip:port>` (unchanged) |
| Mid-match joins, team balance, bot-slot trimming, net timeouts already handled | GameMode PostLogin :333-405 |

**The entire "host" concept is ONE predicate** — `HasAuthority() && IsLocalPlayerController()` — duplicated
4× (`ACombatForgePlayerController::IsHostController` + `IsLocalHost` in PFLoadingMenuWidget /
PFLobbyWidget / PFResultsWidget), gating 10 match-config Server RPCs and ~25 UI-enable sites. On a
dedicated server nobody passes it. Fix = one replicated **MatchLeader** field.

## 2. Game-side changes (workstream W1)

1. **MatchLeader** — replicated `LeaderRosterIndex` (or PlayerState ptr) on `ACombatForgeGameState`;
   assigned in PostLogin (first human), migrated on Logout. Swap the 10 RPC guards + 4 widget predicates
   to leader checks (server-side validated). Identical behavior on listen servers → can ship in the next
   alpha before any fleet exists.
2. **Lifecycle guards**: leader ForceStart works solo (Connected ≥ 1); `humans==0 && Phase!=Lobby` →
   `HostForceReturnToLobby()` in Logout (bot matches must not play forever on an empty server).
3. **Quick-play server config** — URL options parsed in InitGame (already overridable):
   `?Type=2?Mode=2?Format=4?Bots=1?AutoStart=90` so unattended public servers cycle with zero leader.
4. **Community maps on dedicated** (the biggest headless break):
   - Catalog is host-disk-only (`FPFPaths::ArenaDir`) and the picker **hides any map without a .png**;
     `FScreenshotRequest` no-ops headless → maps made on dedicated would be invisible everywhere.
   - Fix: replicate the server's catalog (`FPFCommunityMapInfo` is already USTRUCT/UPROPERTY-ready) to the
     leader's picker; validate picks against the server disk (existing `ServerHostSetCommunityMap` path);
     replace screenshots with a **server-side schematic PNG** rendered from the piece array (deterministic,
     no viewport) — or drop the PNG filter and draw client-side placeholder tiles.
   - Long-term this becomes the roadmap M7 arena-pool backend (central map service on combatforge-api).
5. **Dedicated hygiene**: guard `UCombatForgeGameInstance::Init` video plumbing (frame cap /
   ApplyQualityMethodCVars / GameUserSettings) with `IsRunningDedicatedServer()`; server tick governed only
   by `NetServerMaxTickRate` (30 Hz is fine for this game; measure).
6. **Server browser UI**: boot menu gets **SERVER BROWSER / QUICK PLAY / PRIVATE MATCH** replacing the
   HOST/JOIN row (keep direct-IP + LAN listen as an "LAN / offline" sub-option — it stays the free offline
   path and our dev loop). Browser = `GET /v1/servers` via FHttpModule + JSON (poll on open + 10s
   refresh; no WebSockets needed). Gray out servers whose `netProtocol` ≠ ours.
7. **Names**: subclass `ULocalPlayer`, override `GetNickname()` → account display name (engine sends it as
   `?Name=` on connect; today with no OSS everyone is ephemeral "Player123", so there is **no migration
   debt**). Dedicated server treats `?Name=` as untrusted: after token verification it `ChangeName()`s to
   the server-resolved account name.
8. **Auth seam**: `ServerSetPlayerGuidHash` (CombatForgePlayerController.cpp:637) is the exact seam —
   augment with the account token; keep the install-GUID vote hash **separate** ("two identities, two
   jobs" — per docs/eos-integration-notes.md).

## 3. Fleet & deploy (workstream W2)

**Phase 0 — pilot THIS WEEK, zero engine work:** the existing 3.5 GB package boots headless
(`run-server.ps1` semantics). Rent one **Windows** VPS, run 1-2 instances, point friends at it via the
browser/quick-play API. Validates fun + load before any source-build investment. (Higher RAM per instance
since the full client pak loads — acceptable for a pilot.)

**Phase 1 — real fleet:**
- **Source-built UE 5.6** (hard prerequisite: Launcher binaries *cannot* compile Server targets — the
  Target.cs header + playtest README already document the error). Budget ~200 GB disk + hours on the 5090.
  Known gotchas: Linux cross-compile toolchain **v25** (`v25_clang-18.1.0-rockylinux8.exe`; v24/clang-19
  is explicitly not recommended for 5.6; Epic's CDN briefly shipped a mislabeled v23 — "type_traits not
  found" = wrong toolchain), and set `bAllowUBALocalExecutor=false` if UBA crashes.
- **Linux Shipping server cook**. The `-server` cook strips textures/audio automatically, so the 12 GB
  Bandits force-cook does *not* mean a 12 GB server pak. **MUST keep in the server cook**:
  `/Game/Scene_Warehouse` (prop ISM collision comes from mesh bounds — missing meshes = engine-shape
  fallbacks = server/client collision divergence) and `/Game/Bandits` SKM_Body (bone-based locational
  hits; no skeleton = everything buckets Chest). **Can drop**: RifleAnims, AnimStarterPack,
  Free_Sounds_Pack, the showcase map. Add a dedicated-boot assert that both loaded.
- **Host**: **OVHcloud US VPS-2, Vint Hill VA — $8.50/mo** (4 vCores/8 GB, unlimited traffic, and the
  tiebreaker: **always-on anti-DDoS**, uniquely valuable for public UDP game ports). Fallback: Hetzner
  Ashburn CPX32 ~$16/mo. (July 2026 prices; both raised US prices ~30-50% in April — don't architect
  around a specific price.)
- **Run 4–6 instances as systemd units** (`Restart=always`, `-port=7777..7782/udp`, separate logs).
  Expect ~0.5–1.5 GB RSS and <0.5 core per 12-player instance at 30 Hz — measure. **Skip Docker**: UE
  containers want `--network=host` anyway, so on a single box systemd is simpler and loses nothing.
  (Deploy/playtest/docker/ stub stays for the Phase-2 orchestrator future.)
- **Fleet redeploy is lockstep with itch pushes** (NetProtocol bump per push already mandated).

**Phase 2 — only if it takes off (>150–200 CCU):** containerize (20-line Dockerfile: ubuntu:22.04 +
libssl3/libicu70, non-root — UE refuses root) and burst onto **Edgegap** pay-as-you-go
($0.00115/min/vCPU + $0.10/GB egress, scales to zero) or GameLift spot, keeping the Worker directory as
the front door. Own-binary + own-registry is exactly what makes this a lift-and-shift.
Cautionary tale: **Hathora shut down May 5, 2026** — don't bet the core loop on an indie-focused platform.

**Rejected: EOS for discovery.** Free and no brand review for Game-Services-only, but: adds SDK weight to
a deliberately no-OSS codebase, doesn't provide the web accounts we want anyway, adds third-party COPPA
consent surface (amended rule requires separate opt-in for third-party sharing), and community consensus
is a custom REST registry beats EOS Sessions for a persistent server browser. Revisit only for
crossplay/console (the docs' thin-facade advice).

## 4. Backend: combatforge-api (workstream W3)

One Cloudflare Worker + D1 (`api.playcombatforge.com`) — the proven combatforge-bugs stack:

- **Accounts**: Better Auth **≥ 1.6.11** (pin it — 1.6.0-1.6.10 had a device-flow hijack CVE,
  GHSA-cq3f-vc6p-68fh) with native D1 support (added in 1.5, Feb 2026). Username+password for 13+,
  optional **Discord OAuth gated behind the age screen** (Discord ToS is 13+ — the button must not render
  for child accounts), child accounts per §5.
- **Device-link login for the game** (official Better Auth Device Authorization plugin, RFC 8628):
  game POSTs `/device/code` → shows 6-char code (unambiguous alphabet) → player approves at
  `playcombatforge.com/link` → game polls `/device/token` every 5s → long-lived session token stored in
  `Saved/CombatForge/`, sent as `Authorization: Bearer`. Match server resolves token → {accountId,
  displayName} with one GET per join (upgrade to JWT/JWKS offline verification only if join volume ever
  matters).
- **Server directory**: `POST /v1/servers/register` (per-instance bearer secret) → id;
  `POST /v1/servers/{id}/heartbeat` every 10s {players, phase, map, mode, netProtocol};
  list = `GET /v1/servers` filtered `lastSeen<30s && !private`; **quick-play** = fullest-non-full
  matching netProtocol (concentrates a small population — right call at our scale);
  **private match** = registered `private:true` + 6-char join code, `GET /v1/join/{code}` → addr.
  Join address = Worker-observed public IP + self-reported port (also return self-reported LAN addr so
  same-LAN players hairpin correctly). D1 is fine for this write rate (~1.2 writes/s at full fleet);
  Durable Object migration only if consistency ever bites. KV is disqualified (eventual consistency).
- **Display names**: 3–16 chars `[A-Za-z0-9_]` (engine truncates at 20 — stay under), case-insensitive
  UNIQUE, `obscenity` npm filtering + reserved-word blocks + reject digit-runs/email-shapes (a name must
  never be online contact info), rename 1/30 days with history, site-side report button.

## 5. Accounts UX + COPPA (workstream W4) — **binding constraint, already in force**

The amended COPPA rule is fully effective (compliance deadline April 22, 2026 — passed). We have *actual
knowledge* kids play, so general-audience hand-waving is not available. The hobby-scale-safe design:

- **Neutral age screen** at signup (birth-year picker; store only an `is_child` boolean, never DOB).
- **Under-13 path**: NO email, no real name, **generator-only display names** (adjective+animal — the
  Roblox/Nintendo pattern; kids type real names into free-text fields, and "email plus" consent can never
  authorize public disclosure). Optional **parent-created accounts** (parent's email is the parent's data).
- **13+ path**: username+password, optional email, optional Discord OAuth.
- Persistent identifiers (account id, install GUID, IP) ride the **internal-operations exception**
  (auth/security/service only — no profiling, no contact) → no verifiable-parental-consent flow needed
  at all if child accounts hold zero contact info.
- Two short written docs the amended rule now requires even at hobby scale: a **data-retention policy**
  (e.g. accounts inactive 12-18 mo deleted; request logs w/ IPs 30 days) and a **security program**
  (one page: HTTPS, hashed secrets, D1 via Worker only). Plus a COPPA section in the site privacy policy.
- No third-party analytics/ad SDKs in the client. Ever. (Also why EOS was skipped.)
- Guest play stays allowed at alpha: accounts required only for **listed/matchmade** servers; direct-IP
  and LAN stay unauthenticated.

## 6. Rollout order

| Step | What ships | Depends on |
|---|---|---|
| 1 | MatchLeader + lifecycle guards + URL-option server config (works on listen too) | nothing — next alpha |
| 2 | combatforge-api v1: directory + heartbeat + quickplay + join codes (~1-day Worker build) | nothing |
| 3 | In-game server browser panel (FHttpModule) + Phase-0 **Windows VPS pilot** w/ current package | 1+2 |
| 4 | Accounts (Better Auth + device link) + display names + age gate + policies | 2 |
| 5 | Community-map catalog replication + schematic previews | 1 |
| 6 | Source-built engine → Linux Shipping cook → OVH fleet (systemd ×4-6) | pilot validates |
| 7 | Retire pilot; itch push + fleet redeploy in lockstep (NetProtocol bump) | 6 |

**Monthly cost at alpha scale: ~$9–25** (OVH $8.50 + Cloudflare free tier; pilot Windows VPS ~$15-25
temporarily). No per-player costs until Phase 2.

## 7. Decisions Tom owns

1. **Accounts at alpha: optional (guest fallback) or required?** Plan assumes optional — required only for
   listed servers. (Kids/COPPA friction favors optional.)
2. **Child accounts: parent-created only, or kid self-serve with generated names?** Parent-created is most
   defensible and matches how the friend group works; self-serve is less friction.
3. **Display names for 13+: free text (filtered) or generator-assisted for everyone?**
4. **East-Coast Virginia box OK, or pay ~$48/mo for central (Vultr Dallas) to flatten West-Coast pings?**
   (Depends where the actual players live.)
5. **Keep LAN/listen as a permanent offline mode?** (Plan says yes — it's free and it's the dev loop.)
6. **Community server binary someday?** (Changes the directory trust model — per-server secrets +
   revocation are designed in, but the policy call is Tom's.)
7. **Budget ceiling** — is ~$10-25/mo the target, or is $30-60/mo Edgegap-class scale-to-zero worth it to
   skip server babysitting?
