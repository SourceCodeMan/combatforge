# CombatForge Roadmap

**Last updated:** 2026-07-27
**Current release:** `0.1.0-alpha.16` · NetProtocol **16**

Companion docs: [`store-beta-plan.md`](store-beta-plan.md) (storefront mechanics) · [`code-review/TRACKER.md`](code-review/TRACKER.md) (quality backlog) · [`multiplayer-plan.md`](multiplayer-plan.md) (netcode) · [`mac-port.md`](mac-port.md)

---

## Where we are

| | Status |
|---|---|
| **Windows** | Live on itch — channel `windows-alpha`, upload #18350204 |
| **macOS (arm64)** | Live on itch — channel `osx-alpha`, upload #18571629, shipped 2026-07-27 |
| **Cross-play** | Proven Mac ↔ PC. Both channels on `0.1.0-alpha.16` / NetProtocol 16 |
| **Code review Pass 2** | 2 blockers + 16 majors **fixed**. 49 minors + 38 nits open (issues #27–#38) |
| **Backend** | Plain REST `api.playcombatforge.com`. No OnlineSubsystem, no EOS |
| **Servers** | Vultr dedicated fleet + listen-server hosting |

Both desktop platforms ship from the same `mac-port` lineage; the Mac port took ~3 engineer-days start to finish, which is the calibration anchor for every estimate below.

---

## How to read this

Work is sequenced by **leverage**, not by platform. The single most important insight from the 2026-07-27 platform audit is that most remaining platforms share the *same* prerequisite work — so the ordering below front-loads what pays out more than once.

Effort figures are engineer-days for a solo developer, calibrated against the 3-day Mac port.

---

## NOW — this week

Cheap, high-leverage, or time-gated. Several of these are calendar-bound, so starting them early costs nothing.

### 1. Verify the Steam Deck hypothesis on real hardware — *2 hours*
The existing `windows-alpha` build should run on a Deck under Proton with **zero code changes**: no anti-cheat, no DRM, no launcher, no OnlineSubsystem, and joining is a plain `open <ip:port>` console command over UDP.

**This gates every other handheld decision.** Two claims are unverified: whether VKD3D exposes `AtomicInt64OnTypedResource` (the capability UE5 gates Nanite on), and whether 8 RDNA2 CUs at 15W hold 30fps at 800p with Nanite + virtual shadow maps + volumetric fog through a translation layer. If either fails, the Deck plan changes shape entirely.

*Do not spend a day on Deck tuning until this test passes.*

### 2. Start the Steam Direct clock — *$100, ~30 calendar days*
Two-week mandatory "Coming Soon" plus review. The clock runs in parallel with all engineering, so there is no reason to wait. Note `store-beta-plan.md:106` is wrong that Epic is free — Epic also charges $100 recoupable; the real Epic advantage is 0% on the first $1M/yr vs Steam's 30%, which is moot while the game is free. Discovery favours Steam decisively for a game whose core problem is population.

### 3. Confirm the Fab licence tier applies — *10 minutes* ⚠️
Epic's documentation defines the Personal tier by *"gross revenue from **commercial activity**"* — not *digital-content* revenue. If that test is all-business gross revenue, the current tier may not cover this project, and that is a defect on **builds that are already live**, not a future console question.

Open `fab.com/eula` and `unrealengine.com/eula/content` **in a browser** and read §2(a), §4(c), §5(a), §6(b)(iii). Both pages return HTTP 403 to automated fetch, so no automated audit can settle this — it needs human eyes.

### 4. Fix the dedicated-server volume gap — *15 minutes* — issue #26
`docker-compose.yml` declares no volume for `/var/lib/combatforge` ([`PFPaths.cpp:56`](../Source/CombatForge/Core/PFPaths.cpp)), so every container recreate destroys all community arenas.

---

## NEXT — 2 to 4 weeks

### 5. The shared input layer — *5–8 days* ⭐ highest leverage on this page
Every remaining platform — Deck Verified, Steam Machine, Windows handhelds, Xbox, PS5, Switch — requires the **same five items**. Building them once unlocks all of them, *and* delivers couch-controller play on Windows and Mac immediately.

| Item | Effort | Evidence |
|---|---|---|
| Gamepad bindings | 1–2 d | All 40 `MapKey` calls in `PFInputConfig.cpp` are keyboard/mouse; repo-wide grep for `gamepad\|xinput\|thumbstick` returns **zero** |
| Look input: DeltaTime + deadzone + response curve | 1 d | `CombatForgeCharacter.cpp:1221-1222` — turn speed currently scales with framerate on a stick |
| Build wheel off mouse-delta onto a stick vector | 0.5 d | `PFBuildWheelWidget.cpp:275` |
| `DefaultDeviceProfiles.ini` | — | Does not exist; every scalability value is a single global desktop setting |
| UI scale curve | — | No `[/Script/Engine.UserInterfaceSettings]` UIScaleCurve |

**Two warnings.** The multi-week part is *focus navigation*, not bindings: ~55 click-only widgets across a 3,483-line `PFLoadingMenuWidget.cpp` and a 1,590-line `PFOptionsWidget.cpp`, with `CommonUI` not enabled, plus two deliberate anti-focus decisions to reverse (`PFVoteWidget.cpp:359-367`, `PFBuildWheelWidget.h:22-27`) that exist because taking focus caused real regressions. And the DPI fix is not one line — pinning 800px to 1.0 shrinks the logical canvas from 1730×1081 to 1280×800, giving a layout authored for 2560×1440 *less* room than it has ever had, with only 2 of 12 UI files using a `ScrollBox`.

### 6. Close the NetProtocol fragmentation risk — *1–2 days*
`PFBuild::NetProtocol` ([`CombatForge.h:26`](../Source/CombatForge/CombatForge.h)) is bumped every push and hard-fails mismatched joins. Every storefront with async review adds a potential stale client version, and **each stale version is its own empty server pool**. At current population, two pools is fatal.

The fix is mostly built: the directory already carries `netProtocol` per server (`PFBackendSubsystem.cpp:567,649`), so a min-compatible-protocol constant is a query change, not an architecture project. **Do this before a second storefront exists, not after.**

### 7. Add an age gate — *1–2 days*
[`legal/privacy-policy.md:158-162`](legal/privacy-policy.md) says outright there isn't one; `README.md:7` says the game is "built to be played by kids and adults in the same lobby"; the amended COPPA deadline (2026-04-22) has passed and there is actual knowledge that children play. Steam does not use IARC — it runs its own content survey, and since 2024-11-15 a game without a valid age rating **is not shown to customers in Germany**. Cover the in-game Discord button (13+ service) and the external commerce links.

### 8. Notarize the macOS build — *1 day, $99/yr*
The live Mac download currently makes every user do the Gatekeeper right-click dance. This is the cheapest retention fix available — players are lost at the front door, before the game ever runs. Apple Developer Program is needed for signing + notarization only; this is *not* an App Store commitment.

### 9. Shrink the package — *1 day*
Roughly **3.9 GB of the 4.3 GB is recoverable** without touching authored fidelity: point `Scripts/trim_bandit_textures.py` at the other five packs, and drop `Industrial_Warehouse` from `MapsToCook` (`DefaultGame.ini:25` — it only feeds a cvar that defaults to OFF). Shrinks the download for every existing player on every platform, and is a prerequisite for any future mobile conversation.

### 10. Work the quality backlog — issues #27–#38
49 minors + 38 nits, one issue per module, checkboxed. None are ship-blockers. Best done module-by-module since each shares files and context.

---

## LATER — conditional

### Windows handhelds — ROG Xbox Ally, Legion Go, MSI Claw — *0 extra days*
These run Windows natively, so the existing build simply works: no Proton, no Nanite translation question, no sideload dance. ~2M units across the three, versus ~3.7–4M Steam Decks. **The same input and font work from item 5 serves both.** This is the free half of the handheld audience.

### Steam Machine — *conditional on item 5*
Verified on Steam Machine **skips the display/legibility tests entirely** (Valve does not test text size where the display varies). Gamepad + focus navigation alone plausibly earns a Verified badge without the font/layout pass that dominates the Deck estimate.

### GeForce NOW — *1–2 days, $0*
Uniquely valuable here: the Metal SM6-only decision (`DefaultEngine.ini:101-106`) hard-excludes every M1 and Intel Mac from the native build, and GFN lets them back in. Requires Steam first. Caveat: NVIDIA requires Steam Cloud or an online save, and state currently persists locally (`PFPaths.cpp:61,79`).

### Linux headless dedicated server → community hosting — *4–8 days*
The most strategically interesting item on this page. `Deploy/playtest/run-server.ps1:51-55` documents that a plain *Game* exe run as `?listen -server -nullrhi -nosound` works headless — meaning the ~200 GB source-built engine that `multiplayer-plan.md` treats as blocking may not be required. Publishing that binary turns CombatForge into a community-hosted game (the Valheim/Minecraft shape), where communities carry the population instead of the Vultr bill. Budget `SDL_VIDEODRIVER=dummy`/xvfb — UE still initialises SDL under `-nullrhi` on Linux.

### Microsoft Store (Windows, not Xbox console) — *1–2 days*
$0 registration, plain x64 build, no GDKX, no concept approval, no certification. The cheapest possible second storefront.

### Build automation — *before the third platform*
There is **no CI in the repo** — no `.github/`, no workflows. Every release today is: bump NetProtocol → cook on the PC → cook on the MacBook → `butler push` per channel → redeploy the fleet, all in lockstep. Adding Steam + Linux + a Deck config makes that 4 cooks and 3 upload paths *per release, forever*. For a hobby project this recurring cost, not the port cost, should cap how many platforms exist.

---

## Not doing — and why

These were assessed on 2026-07-27 and explicitly rejected. Recorded so the question does not get re-opened without new information.

| Platform | Verdict | Reason |
|---|---|---|
| **iOS / iPadOS** | ~100–190 days (33–63× Mac port) | UE 5.6 ships **no SM6 shader platform for iOS at all** — Nanite and VSMs off regardless of hardware, so an M4 iPad buys nothing. Megascans materials break without virtual textures. The 256-decal splat system uses deferred decals, unsupported by the mobile forward renderer. Zero touch input (`DefaultInput.ini:13`); 39 keyboard actions to collapse. Package is 4.59 GB against Apple's hard 4 GB cap. |
| **Android** | ~55–105 days | Same renderer and touch story, plus UE 5.6's AAB path emits a single base module with no asset-pack support against Play's 500 MB compressed cap and 4.11 GB of cooked data. Sideload escape hatch closing — developer verification gates installs from 2026-09-30. |
| **Nintendo Switch / Switch 2** | Blocked, not costed | Nintendo is **not accepting Switch 2 dev-environment requests** — you cannot apply. Switch 1 is hardware-incapable (Tegra X1 Maxwell lacks Nanite's 64-bit atomics; `r.Streaming.PoolSize=3000` alone exceeds the console's entire ~3.2 GB budget). If reopened: 200–300 days plus NSO subscriptions for every player. |
| **Xbox / PS5** | 4–7 months each | Xbox is the friendliest console in 2026 ($0 program, $0 cert, free PlayFab) but fails cert day one on gamepad (XR-130), sessions (XR-067/064/124), and achievements (XR-055 needs 10 achievements / 1000G at launch — the codebase has none). PS5 requires a legal entity and signed GDPA before the requirements are even readable. |
| **VR / Meta Quest** | Rejected | Engine-level infeasible on the UE 5.6 mobile tier, and a design mismatch for a build-then-battle FPS. |
| **Mac App Store** | Rejected | Sandbox conflicts with the blanket ATS exemption; near-zero discovery. Notarization (item 8) delivers the actual benefit. |
| **Native Linux *client*** | Rejected | Valve tests the Linux build **first** when one exists, so an un-QA'd native build can *lower* the Deck rating below what Proton would have earned. The Linux *server* (above) is worth doing; the client is not. |
| **Windows-on-ARM, GOG, Amazon Luna, school labs** | Rejected | No native target in shipping UE 5.6 (WoA); negligible reach for the effort (rest). |

---

## Open decisions

Things that need a human call, not more research.

1. **EOS: yes or no.** [`eos-integration-notes.md`](eos-integration-notes.md) is a 303-line plan for a `USessionService` facade with identity on EOS Connect/PUID, and [`roadmap-post-graybox.md:51-55`](roadmap-post-graybox.md) makes it milestone M4. It directly contradicts a standing rule at [`multiplayer-plan.md:174`](multiplayer-plan.md) — *"No third-party analytics/ad SDKs in the client. Ever. (Also why EOS was skipped.)"* — written for COPPA reasons. **Decide this consciously; do not discover it mid-console-port.** Note that platform online-service mandates, not asset licensing, are what actually block console.

2. **Asset licence register.** There is no per-pack record (tier, version, acquisition date, receipt) for the seven paid packs. Fab grandfathers to terms in force at acquisition; the legacy Epic licence does the opposite. Cheap to build now, and every storefront beyond itch will eventually ask.

3. **Suno music provenance.** `Content/Audio/Music/` is documented only by a README line reading "custom for this project — permission granted", and `DefaultGame.ini:38` force-cooks `/Game/Audio` into every shipping build. Suno's commercial licence attaches only to tracks generated under a paid plan, and every store agreement carries content-rights warranties. Record the account plan and generation dates, or replace the tracks.

4. **Map count vs launch timing.** Steam and EGS launches are one-shot — the launch window, the reviews, and the wishlist curve are permanent. The game currently ships one map with 87 open polish items. A free **Steam Playtest** banks wishlists while that gets fixed, without spending the launch.

---

## Standing constraints

- **`github.com/SourceCodeMan/combatforge` must remain private.** That is a licence condition, not a preference — 23.6 GB of paid content across 3,590 LFS objects.
- **GitHub LFS budget is currently exhausted.** `git pull` dies mid-checkout; recover with `GIT_LFS_SKIP_SMUDGE=1 git pull` and source content out-of-band. Raise the budget before the next clone.
- **Arena JSON must stay asset-path-free.** Fab §6(b)(iii) bars letting third parties incorporate Content via level-editing tools, with no UE-Only carve-out. What protects the project today is that arena records store only piece type and grid coordinates (`PFArenaSerialization.h:36-46`). Steam Workshop as a coordinate-only exchange is fine; anything richer needs legal advice.
- **Mac is Metal SM6 only** — excludes M1 and all Intel Macs. A deliberate min-spec decision that must appear in every store listing's system requirements.

---

*Platform assessments derive from a 14-agent audit of this repository on 2026-07-27, including an adversarial verification pass that revised several initially optimistic estimates. Codebase claims cite file:line; external claims were checked against primary sources except where noted as requiring human verification.*
