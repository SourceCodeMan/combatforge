# Breachworks (working title) — EOS Integration Notes (roadmap M4)

> Generated 2026-07-10 by a code-grounded research pass (workflow w9867j3g6). Planning notes for adding **EOS (Epic Online Services)** — sessions/matchmaking + crossplay — to enable the future PC-crossplay and Phase-2 console goals. **Not urgent; deferrable.** Read the *change-now verdict* first.


## Overview

## EOS integration plan — Breachworks (UE 5.6.1)

**Bottom line: EOS is cleanly deferrable. Nothing in the current code needs to change during the art pass. The one thing you owe your future self is not code — it's an identity decision (build on EOS *Connect / Product User ID*, not Epic Account Services) that costs $0 today because there's no session code to point the wrong way yet.**

Breachworks is exactly where you want to be for an additive online layer. A tree-wide grep confirms **zero** session/matchmaking/travel/OnlineSubsystem code exists — joining a match is not game code at all, it's the engine console command `open <host-ip>` run against a listen server. This was a deliberate call, stamped as architecture decision **D12** (`docs/design/02-architecture.md:24`), the `Build.cs:25` comment (`NOT needed: OnlineSubsystem (02 D12)`), and the single-persistent-level design (`CombatForgeGameMode.cpp:53`, `bUseSeamlessTravel=false`).

Because the level never travels and every connection — whether formed by `open <ip>` or by an EOS session — funnels through the same `PostLogin` → PlayerController → possession path, **the phase state machine (Lobby→Build→Combat→Vote→Results), the FastArray build-grid replication, and every gameplay RPC are transport-agnostic and need no rework.** EOS replaces exactly one thing: *how a client discovers the host and travels to it.* D12 predicted this precisely — "adding OSS later touches only the Lobby flow."

**Plugin recommendation:** ship on **OSSv1** (`OnlineSubsystemEOS` + `OnlineSubsystemEOSPlus`), not the still-beta OSSv2 `OnlineServicesEOS`. OSSv1 has mature `IOnlineSession`, a working `NetDriverEOS` P2P transport, EOSPlus for stacking Steam-native under EOS (your Phase-1 Steam launch), and the entire tutorial/marketplace ecosystem. OSSv2 is Epic's stated *future* but its session/lobby surface — the exact part you need — is the least-finished corner in 5.6, with config that shifted between 5.5 and 5.6. Wrap whichever you pick behind a thin `USessionService` facade so a future OSSv2 swap is one file, not a rewrite. If you'd rather buy than build the glue, Redpoint's EOS Online Subsystem or Betide's EOS Integration Kit are what many indies actually ship.


## ⚠️ Change-now verdict (read this first)

## The honest answer: does anything need to change NOW?

**No code needs to change now. EOS is cleanly deferrable until after the art pass.** The architecture is already correctly shaped (D1 + D12), and I found nothing that will paint you into a corner if left as-is. But there are **three zero-to-near-zero-cost commitments** worth locking in *now*, while there's no session code to point the wrong way — deferring the *decision* is what would create a corner, not deferring the *code*.

### 1. Decide the crossplay identity now: PUID/Connect, NOT Epic Account Services (the one real corner)
EOS has two independent identity systems and conflating them is the classic mistake:
- **Connect interface → Product User ID (PUID):** the *game-services* identity. Sessions, lobbies, P2P/NAT, matchmaking all key off the PUID. A Steam player, an Xbox player, and a PS player each log in with their platform token and resolve to comparable PUIDs in one session. **This is what crossplay runs on. Does NOT require the player to own an Epic account.**
- **Epic Account Services (EAS) → Epic Account ID:** a *social* layer (Epic friends, presence, overlay). Optional. Requires the player to own and link an Epic account.

**Commit to PUID/Connect as the network identity and treat EAS as an optional later bolt-on.** If you ever make EAS mandatory you lock out everyone who won't create an Epic account and you couple netcode to the social layer. Cost to commit today: **$0** — it's a design-note line, there's no code yet. This is the single decision that keeps M4 a discovery/join swap.

### 2. Keep your vote `FGuid` exactly as-is — it does NOT conflict with a PUID
`CombatForgeGameInstance`'s install GUID is an anonymized *vote/rating* key with no online meaning. Let PUID be the *network* identity and leave the vote hash alone. Two identities, two jobs. **No change needed** — just don't "unify" them later thinking they overlap. They don't.

### 3. (Optional, cheap) When you do write the session layer, put it behind a thin facade
A `USessionService : UGameInstanceSubsystem` exposing only Host/Find/Join/Leave, so GameMode/Lobby depend on your interface, not OSS types. This is what makes a future OSSv1→OSSv2 migration a one-file swap and lets you keep `open <ip>` as a dev/LAN fallback via `DriverClassNameFallback=IpNetDriver`. **This is future work, not a now-change** — noted here only so you don't hand-roll session calls scattered across the Lobby widget when the time comes.

### What you can safely IGNORE until after the art pass
- Enabling `OnlineSubsystemEOS`/`EOSPlus` plugins and editing `Build.cs`/`DefaultEngine.ini`.
- Creating the Epic Dev Portal product (Product/Sandbox/Deployment/Client IDs).
- Writing any `CreateSession`/`FindSessions`/`ClientTravel` code or the join-list UI.
- EOS auth/login plumbing.

None of these gate art, and none of them retroactively force gameplay changes when added — that's the whole payoff of D12 being right.


## Current networking state (per the code)

## Current state — what connect/session is TODAY (verified in code)

| Aspect | Reality today | Evidence |
|---|---|---|
| Session / matchmaking code | **None.** Grep for `CreateSession`/`JoinSession`/`OnlineSubsystem`/`GetResolvedConnectString` = zero hits. | tree-wide Grep |
| Travel code | **None.** No `ClientTravel`/`ServerTravel`/`OpenLevel`. | tree-wide Grep |
| How a client joins | Engine console command `open <host-ip>` against a process launched as a listen server. Not game code. | `docs/pc-setup.md:153`, D12 |
| Net driver | Stock `IpNetDriver` only, 60 Hz. No `NetDriverEOS`. | `02-architecture.md:370` |
| Level / travel model | Single persistent level, `bUseSeamlessTravel=false` — phase machine runs in-place, no map load between phases. | `CombatForgeGameMode.cpp:53` (D1) |
| Build.cs deps | `OnlineSubsystem` explicitly excluded. | `CombatForge.Build.cs:25` |
| Only "identity" present | A per-install `FGuid` at `Saved/CombatForge/Identity.json`, SHA1-hashed, used **only** for anonymized voting. No online meaning. | `CombatForgeGameInstance.h:9-33` |
| `GameInstance` role | Holds identity + reserved "pending host/join parameters" — the natural home for a future session subsystem. | `02-architecture.md:34` |

**Why this is the ideal starting point:** once a `UNetConnection` exists, the source of that connection (typed IP vs. EOS session) is invisible to gameplay. The GameMode phase writer, build-grid FastArray, GameState OnRep arrays (elim feed, vote tallies), and all RPCs ride that connection unchanged. So the entire EOS question genuinely reduces to swapping discovery/join — the "additive, no rework" constraint is already satisfied by the existing architecture, not something you have to engineer toward.


## Phased integration plan

## Phased plan — additive path to crossplay + console

Each phase is strictly additive; earlier phases keep working. The gameplay core (phases, build grid, RPCs) is untouched throughout.

### Phase 0 — NOW (during the art pass): decisions only, no code
- Write the design note: **crossplay identity = EOS Connect/PUID; EAS optional.**
- Leave `open <ip>` + 2-player listen-server PIE as the dev/LAN test loop — it stays the fastest iteration path forever.
- Cost: minutes. Unblocks nothing, blocks nothing.

### Phase 1 — Dev/LAN session layer on PC (first real EOS work, post-art)
1. Epic Dev Portal: create Organization → Product → Sandbox (Dev/Stage/Live) → Deployment → Client + Client Policy (Auth, Sessions/Lobbies, P2P, Connect). Collect ProductId, SandboxId, DeploymentId, ClientId, ClientSecret, EncryptionKey. (Browser account action — not scriptable.)
2. Enable plugins **Online Subsystem EOS + Online Subsystem EOS Plus** (SDK ships inside the plugin in 5.6 — no separate C SDK download).
3. `Build.cs`: add `"OnlineSubsystem"`, `"OnlineSubsystemUtils"`, `"OnlineSubsystemEOS"`; update the `Build.cs:25` "NOT needed" comment to cite the M4 decision.
4. `DefaultEngine.ini`: `DefaultPlatformService=EOS`; add the `NetDriverEOS` GameNetDriver definition **with `DriverClassNameFallback=/Script/OnlineSubsystemUtils.IpNetDriver`** (this is what preserves `open <ip>`). Keep `ClientSecret` out of committed git.
5. New `USessionService : UGameInstanceSubsystem`: EOS Connect login (Device ID for dev) → Host = `CreateSession` (set `bUseLobbiesIfAvailable=true` or EOS returns no results; stamp a `BUILD_ID` session attribute), Find = `FindSessions`, Join = `GetResolvedConnectString` → `ClientTravel`. Add a `bUseLAN` toggle that flips `bIsLANMatch`/`bIsLANQuery`.
6. Lobby UX: the only gameplay-facing change — "everyone types the same IP" becomes a host/join + server-browser screen (extend `PFLobbyWidget` or add a pre-lobby widget).

### Phase 2 — EOS sessions as primary on PC / Steam launch (crossplay-ready fabric)
- Switch to EOSPlus layering: `DefaultPlatformService=EOSPlus`, `NativePlatformService=Steam`, enable Steam OSS. One build presents Steam identity natively while EOS provides the session/crossplay fabric.
- Enable **"Use Crossplay Sessions"** so cross-platform players share a session pool.
- Smoke-test the **Live** deployment early (not just Dev sandbox) — console cert will test against Live.

### Phase 3 — Console crossplay (Xbox ID@Xbox + PlayStation Partners)
- Requires approved dev programs + NDA'd platform SDKs you can't touch until accepted. **The PC/Steam EOS build is 100% of what you can validate solo.**
- Per platform, the *only* thing that changes is the Connect login token (Xbox Live XSTS / PSN token) + that platform's online subsystem. Session/matchmaking code above Connect is identical — the payoff of committing to PUID in Phase 0.
- UI must handle "crossplay unavailable for this account" (parental controls / platform policy) gracefully.

### Phase 4 — Optional social bolt-on
- Add Epic Account Services for Epic friends/invites/overlay if desired. Never a dependency of the join path.


## Risks

## Risks & honest gotchas

- **OSSv1 is the "legacy" path Epic keeps signaling for deprecation** — but "soon-to-be-deprecated" has meant "still the pragmatic choice" across several 5.x releases. Mitigation: the thin `USessionService` facade makes an OSSv2 migration a one-file swap. Don't adopt still-beta OSSv2 now just to be "current" — its session/lobby surface is the least-baked part and the config shifted 5.5→5.6.
- **EOS auth is net-new work, separate from the vote GUID.** `CreateSession`/`FindSessions` require a logged-in EOS local user (Connect/Auth). This is a genuinely new pre-match step — the install GUID does NOT satisfy it. Don't underestimate it as "just config."
- **The EOS connect string is a relay/P2P handle, not an IP.** That's exactly what enables Xbox/PS crossplay without NAT punching — but it also means the LAN `open <ip>` path only exists on the IpNetDriver. Keep both (the fallback line above).
- **`bUseLobbiesIfAvailable=true` is mandatory** on EOS sessions or `FindSessions` silently returns nothing. Classic first-run "why can't I find my own game."
- **Clock drift silently kills auth.** If the machine clock drifts from EOS server time, tokens fail and players get kicked as invalid-auth with no obvious cause. Log EOS auth failures explicitly from day one.
- **Sandbox vs Live is a first-class build switch.** Hard-coding a dev sandbox/Deployment ID is a common late-stage foot-gun; console cert runs against Live. Make it a config toggle, smoke-test Live early.
- **EOS voice is NOT cross-platform** — don't promise crossplay voice in design copy; point players at Discord.
- **Don't put version in the session name** — put build/version in a session *attribute* so mismatched clients are filtered, not silently joined.
- **Listen host is also a player:** host must login AND `CreateSession` before the phase machine starts; host needs no `GetResolvedConnectString` (connection is local).


---

# Detail


## EOS plumbing in UE 5.6 — the plugin decision and project setup

### Where the code stands (the good news)
Breachworks has **no session/matchmaking layer at all**, and that's by design — architecture decision **D12** (`docs/design/02-architecture.md:24`), the `Build.cs:25` comment ("NOT needed: OnlineSubsystem"), and `pc-setup.md:153` (clients run console `open <host-ip>` against a listen server). The level is single/persistent (`CombatForgeGameMode.cpp:53`, `bUseSeamlessTravel=false`), so the phase state machine, FastArray build-grid, and gameplay RPCs ride over `UNetConnection` regardless of *how* the connection was formed. **EOS is therefore purely additive: it replaces "type an IP" with "find/create a session and join," and nothing in the phase/replication code changes.** D12 called this exactly right.

### The real question: OSSv1 vs OSSv2 in 5.6
There are two Epic-provided ways to talk to EOS, and they are genuinely different code paths:

| | **OnlineSubsystemEOS + OnlineSubsystemEOSPlus** (OSSv1) | **Online Services EOS** (OSSv2) |
|---|---|---|
| API | Classic `IOnlineSession`/`IOnlineIdentity` interfaces | New `UE::Online::` interfaces (async op / `TOnlineResult`) |
| Status in 5.6 | Mature, stable, **still fully supported** | Epic's *recommended* direction, but shipped **beta** |
| Sessions/lobbies | Complete and battle-tested | Newer, with known rough edges around sessions/lobbies |
| Steam layering | **EOSPlus** cleanly stacks Steam-native under EOS | Weaker/less-proven multi-platform layering story |
| Ecosystem | Almost every EOS tutorial, forum answer, and marketplace plugin targets this | Thin tutorial coverage; more "read the source" |
| Migration cost later | You may migrate to OSSv2 eventually | You're already on it |

**Epic's official guidance** (Online Services docs, and the X157 dev notes) is: for a *new* project intending to ride UE 5.x forward, prefer OSSv2 because OSSv1 is "soon-to-be-deprecated." That's the forward-looking answer. But "soon-to-be-deprecated" has meant "still here and still the pragmatic choice" across several 5.x releases, and OSSv2 is **still labeled beta in 5.6** with the session/lobby surface being the least-finished part — which is precisely the part Breachworks needs. Note also a real 5.5→5.6 config churn: the old `bUseOnlineServicesV2=true` switch was deprecated in favor of setting `[/Script/Engine.OnlineEngineInterface] ClassName=/Script/OnlineSubsystemUtils.OnlineServicesEngineInterfaceImpl`, which is the kind of moving-target friction you feel on the OSSv2 path.

### Recommendation: ship on OSSv1 (OnlineSubsystemEOS + EOSPlus), behind a thin interface
For a **shipping indie** whose immediate need is "sessions instead of IP" and whose Phase-1 store is **Steam** feeding a Phase-2 crossplay console goal:
1. **Use OnlineSubsystemEOS + OnlineSubsystemEOSPlus (OSSv1).** It's the most-documented, most-shipped, and most-complete path in 5.6; sessions and P2P transport work today; EOSPlus is the piece that lets one build present Steam identity natively while EOS provides the crossplay/session fabric.
2. **Wrap it in a small `USessionService` (UGameInstanceSubsystem)** — Host/Find/Join/Leave, nothing else — so the GameMode/Lobby flow depends on your interface, not on OSS types directly. If OSSv2 matures (or Epic hard-deprecates OSSv1) you re-implement one file, not the game.
3. Keep raw-IP `open` working as a dev/LAN fallback behind a build flag — it costs nothing and it's the fastest 2-player PIE/LAN test loop.

If you'd rather not hand-roll the OSS glue at all, the mature third-party option is **Redpoint's EOS Online Subsystem** (or **Betide's EOS Integration Kit**) — both wrap OSSv1 EOS, handle the fiddly config, and are what a lot of indies actually ship. Reasonable to buy instead of build here.

### Concrete setup steps (OSSv1 path)

**1. Epic Dev Portal — create the product identifiers.**
At dev.epicgames.com, create an **Organization → Product**, then a **Sandbox** (you get Dev/Stage/Live) and a **Deployment** per sandbox, and a **Client** (with a **Client Policy** granting the features you use — Auth, Sessions/Lobbies, P2P, Connect). You collect five values: **ProductId, SandboxId, DeploymentId, ClientId, ClientSecret** (plus an EncryptionKey). Accept the EOS SDK / dev-portal terms — that's an account action to do in the browser, not something to script.

**2. Enable the plugins.** In the editor (or `.uproject`): enable **Online Subsystem EOS** and **Online Subsystem EOS Plus** (EOSPlus pulls in the base OnlineSubsystem). The EOS **SDK ships inside the OnlineSubsystemEOS plugin** in 5.6 — you do *not* separately download the C SDK for the Epic-provided plugin (you only fetch the standalone SDK if you go raw-C or need a newer SDK than the engine bundles).

**3. Build.cs.** Add to `PublicDependencyModuleNames` (currently `Build.cs:17`): `"OnlineSubsystem"`, `"OnlineSubsystemUtils"`, `"OnlineSubsystemEOS"` (add `"OnlineSubsystemEOSPlus"` only if you reference its types directly; enabling the plugin is usually enough). This flips the `Build.cs:25` "NOT needed: OnlineSubsystem" note — update that comment to point at the M4 decision.

**4. DefaultEngine.ini.** Replace the bare IpNetDriver block (`Config/DefaultEngine.ini`, per `02-architecture.md:370`) with the EOSPlus config. For an EOS-only (no Steam yet) start:
```ini
[OnlineSubsystem]
DefaultPlatformService=EOS

[OnlineSubsystemEOS]
bEnabled=true

[/Script/OnlineSubsystemEOS.NetDriverEOS]
bIsUsingP2PSockets=true

[/Script/Engine.GameEngine]
!NetDriverDefinitions=ClearArray
+NetDriverDefinitions=(DefName="GameNetDriver",DriverClassName="/Script/OnlineSubsystemEOS.NetDriverEOS",DriverClassNameFallback="/Script/OnlineSubsystemUtils.IpNetDriver")

[EpicOnlineServices]
ProductName=Breachworks
ProductVersion=1
ProductId=<from portal>
SandboxId=<from portal>
DeploymentId=<from portal>
ClientId=<from portal>
ClientSecret=<from portal>   ; move to an encrypted/-ini override before shipping — do NOT commit the secret to git
EncryptionKey=<64 hex chars>
bIsUsingP2PSockets=true
```
When you add Steam for the Phase-1 launch, switch to EOSPlus layering:
```ini
[OnlineSubsystem]
DefaultPlatformService=EOSPlus
NativePlatformService=Steam
[OnlineSubsystemEOSPlus]
bEnabled=true
```
The `DriverClassNameFallback=IpNetDriver` is what preserves your LAN/`open <ip>` path when P2P/EOS isn't in play.

**5. Wire the session flow.** In the new `USessionService`: on launch, EOS **Auth/Connect** login (Device ID or Account Portal for PC dev) to get a **ProductUserId**; Host = `CreateSession` (advertise phase/region as session attributes) then listen; Find = `FindSessions` with filters; Join = read the session's connect string and `ClientTravel` to it. The Lobby UI (currently "everyone types the same IP") gains a server-browser/"host or join" screen — this is the *only* gameplay-facing change. Your per-install **FGuid voter identity** (`CombatForgeGameInstance.h`) is unrelated to the EOS ProductUserId and can stay exactly as is.

### Honest maturity verdict
- **OSSv1 EOS in 5.6:** production-grade, well-trodden, what indies ship. Downside: Epic keeps signaling eventual deprecation, so you're adopting the "legacy" path — mitigated entirely by the thin-interface wrapper.
- **OSSv2 EOS in 5.6:** the future and Epic's stated preference, but still **beta**, with the session/lobby corner being the least-baked and the config surface still shifting between point releases — the wrong place to spend a shipping indie's risk budget right now.
- **Net:** OSSv1 + EOSPlus now, wrapped so OSSv2 is a swap-one-file migration later. Fully consistent with D12 ("adding OSS later touches only the Lobby flow").

**Sources:**
- [Online Subsystem EOS Plugin — Unreal Engine docs](https://dev.epicgames.com/documentation/en-us/unreal-engine/online-subsystem-eos-plugin-in-unreal-engine)
- [Online Services (OSSv2) — X157 Dev Notes](https://x157.github.io/UE5/OnlineServices/)
- [OSSv2 (Online Services) for 5.4 forward — Epic forums](https://forums.unrealengine.com/t/ossv2-online-services-for-5-4-forward/1772940)
- [OnlineServices vs OnlineSubsystem — Epic forums](https://forums.unrealengine.com/t/onlineservices-vs-onlinesubsystem/2358007)
- [EOS Online Subsystem changelog — Redpoint Games](https://docs.redpoint.games/eos-online-subsystem/docs/changelog/)
- [EOS Integration Kit — Betide Studio](https://eik.betide.studio/)


## M4 — EOS Sessions: replacing raw IP-connect (discovery/join only)

### Where we are today (verified in code, not assumed)
Breachworks has **no session, matchmaking, travel, or Online Subsystem code at all**. A tree-wide grep for `ClientTravel` / `ServerTravel` / `OpenLevel` / `GetResolvedConnectString` / `CreateSession` / `IOnlineSession` returns **zero hits**. Joining a match is not game code — it's the engine console command **`open <host-ip>`** run against a process launched as a listen server (`pc-setup.md` §8). `UCombatForgeGameInstance` holds only the anonymized install-identity GUID (voting), and `Build.cs` explicitly excludes `OnlineSubsystem` per decision **D12**.

This is the *good* case: D12 already committed to the additive path — *"Steam/EOS sessions are a lobby-UX feature… adding OSS later touches only the Lobby flow."* The plan below honors that.

### What stays untouched (the whole point)
Once a connection is established, EOS vs. `open <ip>` is invisible to the game: both funnel through the same `PostLogin` → PlayerController/PlayerState → possession path. So **none of these change**:
- `ACombatForgeGameMode` phase state machine (`SetPhase`, the single writer) — Lobby→Build→Combat→Vote→Results.
- FastArray build-grid replication (`PFBuildGrid` / `PFBuildComponent`) and all gameplay RPCs (weapon/health/vote).
- `AGameState` OnRep arrays (elimination feed, vote tallies — D11).

Only the **pre-game discovery/join** changes: the out-of-code `open <ip>` becomes `CreateSession` / `FindSessions` / `JoinSession` inside the GameInstance layer.

### The flow, mapped onto the GameInstance
Put this in a new **`UPFSessionSubsystem : UGameInstanceSubsystem`** rather than bloating `UCombatForgeGameInstance` (keeps identity code isolated; satisfies the GI's reserved "pending host/join parameters" role). Grab the interface once: `IOnlineSubsystem::Get()->GetSessionInterface()`.

**HOST (listen server):**
1. `FOnlineSessionSettings`: `NumPublicConnections = 8`, `bShouldAdvertise = true`, `bAllowJoinInProgress = true`, `bUseLobbiesIfAvailable = true` (**EOS requires this or `FindSessions` returns nothing**), `bIsLANMatch = bUseLAN` (dev toggle, below). Stamp a build/version key via `Settings.Set("BUILD_ID", …)` so mismatched clients can't join.
2. Bind `OnCreateSessionCompleteDelegate`; call `CreateSession(LocalPlayer, NAME_GameSession, Settings)`.
3. On success: because Breachworks uses **one persistent level and `bUseSeamlessTravel=false`**, the host can advertise the already-running listen server in place — **no `ServerTravel` needed**. (Standard alt: re-host via `ServerTravel("/Game/Maps/L_Graybox?listen")`.) The phase machine then runs exactly as today.

**CLIENT (find + join) — this replaces `open <ip>`:**
1. `FindSessions(SearchSettings)` with `QuerySettings` set for presence/lobbies (`bIsLANQuery = bUseLAN`).
2. On `OnFindSessionsComplete`: results live in `SearchSettings->SearchResults` (`TArray<FOnlineSessionSearchResult>`) → list them in a join UI (extend `PFLobbyWidget` or a new pre-lobby widget).
3. `JoinSession(LocalPlayer, NAME_GameSession, ChosenResult)`.
4. On `OnJoinSessionComplete`: `GetResolvedConnectString(NAME_GameSession, ConnectStr)` → `PlayerController->ClientTravel(ConnectStr, TRAVEL_Absolute)`. **That `ClientTravel` is the exact replacement for the manual `open <host-ip>`.**

### Config / plugin wiring (`DefaultEngine.ini`, reverses D12)
```
[OnlineSubsystem]
DefaultPlatformService=EOS        ; EOSPlus later for true crossplay
[OnlineSubsystemEOS]
bEnabled=true
[/Script/Engine.GameEngine]
+NetDriverDefinitions=(DefName="GameNetDriver",DriverClassName="OnlineSubsystemEOS.NetDriverEOS",DriverClassNameFallback="OnlineSubsystemUtils.IpNetDriver")
```
Enable plugins **OnlineSubsystemEOS + EOSShared + OnlineSubsystemUtils**; add `"OnlineSubsystem","OnlineSubsystemEOS","OnlineSubsystemUtils"` to `Build.cs` (undo the D12 exclusion). Product/Sandbox/Deployment/Client IDs come from an **Epic Dev Portal** product entry into `[EOSSDK]` — required before any session call works.

### LAN / direct-IP fallback (keep for dev — do NOT delete `open <ip>`)
- The `DriverClassNameFallback=IpNetDriver` above keeps **`open <ip>` working**, and 2-player "Play As Listen Server" PIE keeps working with `OnlineSubsystemNull` (no Epic login) for pure gameplay iteration.
- A single `bUseLAN` CVar/UPROPERTY that flips `bIsLANMatch`/`bIsLANQuery` on both `CreateSession` and `FindSessions`, so same-LAN discovery works without going through EOS relays.

### Honest gotchas (flag these before committing time)
- **EOS auth is net-new work.** `CreateSession`/`FindSessions` require a **logged-in EOS local user** (EOS Connect/Auth — AutoLogin/DeviceID/Account Portal). New pre-match step. The existing install-GUID identity is for **vote anonymity only** and does **not** satisfy EOS auth — don't conflate them.
- **OSSv1 vs OSSv2 (real UE 5.6 fork).** Epic steers new projects toward **Online Services (OSSv2)** — the Lyra/CommonUser path — but as of 5.5/5.6 it's still **beta with API churn**. Classic **OSSv1 `IOnlineSession`** (functions above) has full session parity, a stable API, and most tutorials. **Recommendation:** ship on **OSSv1 `IOnlineSession`** (lower risk for a solo dev + 2–8p game), wrapped behind the thin `UPFSessionSubsystem` facade so a later OSSv2 swap is contained.
- **EOS connect string is a relay/P2P handle, not an IP** — which is exactly what enables Xbox/PS crossplay later without NAT punching, but also why the LAN `open <ip>` path only exists on the IP net driver. Keep both.
- **Listen host is also a player:** host must **login AND `CreateSession`** before the phase machine starts; host needs no `GetResolvedConnectString` (it's local).
- Use conventional **`NAME_GameSession`**; put build/version in a **session setting**, not the session name, to avoid silent version-mismatch joins.

### Scope estimate
~1 new subsystem class (host/find/join + delegates), a join-list UI, `DefaultEngine.ini` + `Build.cs` + plugin toggles, and an EOS login step. **Zero changes** to GameMode phases, build-grid replication, or gameplay RPCs.

**Sources:** [Epic — Online Subsystem EOS Plugin](https://dev.epicgames.com/documentation/en-us/unreal-engine/online-subsystem-eos-plugin-in-unreal-engine) · [Epic — Online Subsystem overview](https://dev.epicgames.com/documentation/en-us/unreal-engine/online-subsystem-in-unreal-engine) · [Redpoint — Creating/Finding a session](https://docs.redpoint.games/eos-online-subsystem/docs/sessions_creating/) · [X157 — Online Services (OSSv2) notes](https://x157.github.io/UE5/OnlineServices/) · [Cedric Neukirchen — Online Subsystem overview](https://cedric-neukirchen.net/docs/session-management/online-subsystems/)


## EOS crossplay + console identity — what to decide now (Angle: identity & gotchas)

### Where Breachworks stands today
There is **no online/session/identity layer in the code at all**, and that's a documented choice, not a gap: `"02 D12: no OSS"` is stamped in `CombatForgePlayerController.cpp:433` and `CombatForge.Build.cs:25`. Today it's a **listen server + raw-IP join** (`README.md:32`; client uses the console `open <host-ip>`), a single persistent level with `bUseSeamlessTravel=false` (`CombatForgeGameMode.cpp:53`), and the only "identity" is a **local per-install `FGuid`** hashed to SHA-1 for anonymized voting (`CombatForgeGameInstance.h`). A tree-wide grep for `Session/CreateSession/JoinSession/OnlineSubsystem/EOS/ClientTravel` returns **zero** gameplay hits. Good news: this means EOS is genuinely additive — it only replaces *discovery/join*, and the phase state machine, FastArray build-grid, and gameplay RPCs ride UE's replication layer unchanged either way.

### The one architectural decision that matters: build on PUID/Connect, not EAS
EOS has **two independent identity systems**, and conflating them is the classic corner:

| | **Connect interface → Product User ID (PUID)** | **Epic Account Services (EAS) → Epic Account ID** |
|---|---|---|
| What it is | The **game-services identity**. Sessions, lobbies, P2P/NAT, matchmaking, stats, achievements all key off the **PUID**. | A **social identity** — Epic friends, rich presence, the in-game overlay. |
| Needed for crossplay *play*? | **YES. This is the identity crossplay runs on.** | **No** — optional, additive social layer. |
| Requires the player to own/link an Epic account? | **No** — logs in via an external token (Steam / Xbox Live / PSN / Device ID). | **Yes** — player must have an Epic account *and* link their platform account to it. |

**Rule of thumb: PUID is who you *are* to the match; EAS is who you are to your *friends list*.** Architect your session/join code against the **Connect interface** so a Steam player, an Xbox player, and a PS player all resolve to comparable PUIDs and land in one session. Treat EAS (friends/overlay/"invite from Epic friends") as a **later, optional bolt-on** — never a dependency of the join path. If you make EAS mandatory you've locked out every player who doesn't want an Epic account, and you've coupled your netcode to the social layer. Don't.

### How crossplay actually flows (PC → Xbox/PS later)
- The host creates an **EOS Session** (or Lobby) instead of announcing an IP. Clients **find** the session and `ClientTravel` to it — that's the *only* connect-path change. On PC you can run the traffic over **EOS relays/P2P** so there's no port-forwarding, or keep dedicated/listen IPs behind the session.
- **Per platform, the only thing that changes is the login token feeding Connect:** PC=Steam or Epic or Device ID; Xbox=Xbox Live (XSTS) token; PS=PSN token. The session/matchmaking code above Connect stays identical. That's the payoff of building on PUID now.
- **"Use Crossplay Sessions"** must be enabled — it makes the EOS Sessions interface the primary one so cross-platform players share a session pool.

### Console gotchas a solo dev should know NOW (so you don't re-architect at cert)
1. **OSSv1 vs OSSv2 — pick deliberately.** UE 5.6 ships both `OnlineSubsystemEOS` (OSSv1, mature, `IOnlineSession` — most tutorials/marketplace plugins target it) and `OnlineServicesEOS` (OSSv2, Epic's stated future, native EOS/consoles). Epic recommends **OSSv2 for new games that will track UE upward**, but OSSv2 is still evolving and non-EOS platforms are wrapped via an adapter. Practical solo call: prototype crossplay on whichever your chosen session plugin/tutorials support best, but **keep session code behind a thin interface** so an OSSv1↔OSSv2 swap isn't a rewrite. This wrapper is the cheap insurance.
2. **Console SDKs are NDA'd and gated.** Xbox (ID@Xbox) and PlayStation (Partners) console builds require approved dev programs + platform SDKs you can't touch until accepted. **The PC/Steam EOS build is 100% of what you can validate solo** — design so the console layer is *only* a different Connect login token + platform online-subsystem, not new session logic.
3. **Sandbox vs Live / Deployment IDs.** EOS separates **sandbox** (dev) from **live** deployments with different Client/Deployment IDs. Ship config must switch cleanly; hard-coding a dev sandbox ID is a common late-stage foot-gun. Console cert will test against **live**, so smoke-test the live deployment early (mirrors your M6 "packaging early" instinct).
4. **Clock sync = silent auth kill.** If the machine clock drifts from EOS server time, tokens fail to sync and players get **kicked as invalid-auth** with no obvious cause. Log EOS auth failures explicitly from day one.
5. **Voice is NOT cross-platform.** EOS voice is platform-scoped; there is **no cross-platform voice** you get for free — teams typically point players at Discord. Don't promise crossplay *voice* in design copy.
6. **Crossplay can be greyed out by platform policy** (parental controls, PSN/Xbox account restrictions). Your UI must handle "crossplay unavailable for this account" gracefully rather than assuming it's always on.
7. **Keep your local vote `FGuid` as the vote key.** It's an anonymized *content/rating* identity with no online meaning, so it doesn't collide with a PUID. Let PUID be the *network* identity and leave the vote hash alone — two identities, two jobs.

### Net: are you architected into a corner? No.
The "additive, no rework" claim holds. Do three cheap things now and M4 stays a discovery/join swap: (1) commit to **PUID/Connect as the crossplay identity**, EAS optional; (2) put session find/create/join behind a **thin interface** so OSSv1/v2 and per-platform login tokens are swappable; (3) keep **sandbox vs live** EOS config as a first-class build switch. Everything below (phases, build grid, RPCs) is untouched.

**Sources:**
- [Online Subsystem EOS Plugin — UE docs](https://dev.epicgames.com/documentation/en-us/unreal-engine/online-subsystem-eos-plugin-in-unreal-engine)
- [Online Subsystems and Services (OSSv2) — UE docs](https://dev.epicgames.com/documentation/unreal-engine/online-subsystems-and-services-in-unreal-engine)
- [OnlineServices vs OnlineSubsystem — Epic forums](https://forums.unrealengine.com/t/onlineservices-vs-onlinesubsystem/2358007)
- [Online Services (OSSv2) — X157 Dev Notes](https://x157.github.io/UE5/OnlineServices/)
- [EOS Crossplay Technical Overview](https://dev.epicgames.com/docs/epic-account-services/crossplay/crossplay-technical-overview)
- [EOS Identity Provider Management (Connect / Device ID)](https://dev.epicgames.com/docs/epic-online-services/eos-fundamentals/identity-provider-management)
- [EOS Integration Kit — Authentication (Connect vs EAS)](https://eik.betide.studio/authentication)
