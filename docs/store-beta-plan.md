# Store beta plan — Mac on itch, then Epic Games Store

Written 2026-07-26. Covers roadmap steps 5 (Mac binaries → itch) and 6 (Epic beta).
Steps 1–4 (review → fix → push to `mac-port` → review there) are done; see the session notes
at the top of the `project-paintforge` memory and commits `f85950d` / `2150bb9`.

---

## Step 5 — Mac build → itch.io

### What must happen on the MacBook

Unreal cannot cross-compile macOS from Windows. The `.app` has to be produced on the Mac.
Everything below runs there.

```bash
cd ~/projects/combatforge && git checkout mac-port && git pull && git lfs pull
```

```bash
./Scripts/check-mac-env.command
```

The preflight catches the recurring breakages (the Epic Launcher silently reverting the
`Apple_SDK.json` `MaxVersion` cap, missing Metal toolchain, unhydrated LFS). Run it after **any**
Launcher Verify/update. Needs ≥30 GB free before a cook.

```bash
./Scripts/Package-Mac.command Shipping
```

Shipping was the call for this build: it closes the `~` console and disables every `ECVF_Cheat`
cvar, including `pf.SetRank`. See "Why Shipping matters" below.

### Facts that matter for this build

- **NetProtocol stays 16 — do not bump it.** Verified 2026-07-26: every commit since the alpha-16
  ship (`8525b6d`) is client-side UI/input plus one server-side GameMode fix. **No replicated
  property, RPC signature, or GameState/PlayerState field changed.** So the Mac build at protocol 16
  cross-plays with the *live* Windows alpha-16 and the running Vultr fleet, and you do **not** need
  to re-push Windows to make Mac work. If you touch replication before packaging, bump it and ship
  both sides together.
- **The script already handles the Shipping traps.** No `-distribution` flag (it flips UAT into
  ModernXcode `.xcarchive` mode and dies without an Xcode archive), config-aware `.app` name
  detection, a pak-less-archive guard that swaps in the real staged build, and the privacy scrub.
- **The Shipping app is named `CombatForge-Mac-Shipping.app`.** That is what players will see in
  the itch app and their Applications folder. Worth renaming to `CombatForge.app` before pushing —
  the bundle directory name is independent of `Info.plist`'s `CFBundleExecutable`, so a plain `mv`
  is normally safe. **Verify it still launches after renaming** before pushing.
- **Mac is Metal SM6 only.** `Config/DefaultEngine.ini` sets `TargetedRHIs=SF_METAL_SM6` and removes
  the SM5 entry (needed for Nanite, and it halves shader-compile time). **This excludes M1 Macs and
  all Intel Macs.** That is a min-spec decision, not a bug — but it must go in the itch and Epic
  store listings as a system requirement.
- **Unsigned → Gatekeeper.** First launch needs right-click → Open. The proper fix is an Apple
  Developer account ($99/yr) for signing + notarization. Fine to defer for a beta; not fine to
  leave unexplained on the store page.

### Publishing to itch

Windows and Mac coexist on one itch page — butler infers the platform from the **channel name**,
so the Mac build goes to a *new* channel and never disturbs the live Windows one.

```bash
butler push "Packaged/Mac/CombatForge.app" thathorseslayer/combatforge:osx-alpha --userversion 0.1.0-alpha.16
```

Match the `--userversion` to the Windows build it cross-plays with.

> Per the standing rule I did not push anything to itch or redeploy the server. Both are yours to trigger.

---

## Step 6 — Epic Games Store

### Correction to the earlier estimate

I initially told you Epic needs "real SDK integration (EOS)". **Reading Epic's actual requirements,
that is wrong, and the correction is in your favour.** Per the
[Store Requirements](https://dev.epicgames.com/docs/epic-games-store/requirements-guidelines/distribution-requirements/requirements-overview)
page, EOS sits under **"Epic Online Services *Recommendations*"** — not requirements.

The one hard *technical* requirement for a multiplayer game is crossplay:

> "Products with online multiplayer functionality must support crossplay across all PC storefronts
> where the product is distributed. **You may use your preferred solution for crossplay, such as the
> free Epic Online Services Crossplay functionality, your own method, or any third-party system**
> that works across PC storefronts."

**CombatForge is architecturally positioned to satisfy this, but release evidence is still required.**
The game deliberately uses no OnlineSubsystem at all
(`CombatForge.Build.cs`: *"NOT needed: OnlineSubsystem (02 D12) — the backend is plain REST"*).
Accounts, XP, unlocks and the server browser all go through `api.playcombatforge.com`, and every
client joins the same Vultr fleet regardless of where it was downloaded. An itch Windows player, a
Mac player and an EGS player use the same infrastructure. Before submission, test an itch Windows
build and the exact Epic artifact together against the same fleet, including account login, server
browse/quick-play, joining, a full match, and progression. Both artifacts must use the same
`PFBuild::NetProtocol`.

**Achievements are also not a blocker.** They are required only "if a product supports achievements
through other PC storefronts". CombatForge has none on itch, so it is exempt — and Early Access
products get a further carve-out until full release.

### What actually stands between you and an Epic beta

Only you can do the first four — they need your identity, agreements, and legal pages.

| # | Item | Who | Notes |
|---|------|-----|-------|
| 1 | Epic dev account + publishing agreements | **You** | Agreements, organization verification, tax/payout setup, and Epic's recoupable US $100 submission fee gate release. |
| 2 | Create the product in the Dev Portal | **You** | dev.epicgames.com/portal |
| 3 | IARC age rating | **You** | Free automated questionnaire. Required *before* you can pick distribution regions. A non-lethal airsoft shooter should rate low, but answer honestly about the violence depiction. |
| 4 | Privacy policy + EULA/ToS, publicly hosted | **You** | Release-candidate pages live in `combatforge-site/public`; confirm the operator/contact and obtain legal review before deployment. |
| 5 | Store page assets | Me + you | Copy, screenshots, trailer, key art. I can write the copy; the capture is yours. |
| 6 | Early Access designation | **You** | The right lane for a beta — Epic explicitly welcomes it and it relaxes the achievements rule. |
| 7 | Shipping-config build + min specs | Me | See below. |
| 8 | Optional: EOS for the Epic overlay / achievements / ownership checks | Me, later | Recommended, not required. Worth doing *after* launch. Ownership verification on a trusted server is Epic's anti-piracy recommendation and your backend is already the right place for it. |

### Why Shipping matters before any store beta

`Deploy/playtest/package-playtest.ps1` defaults to Development for playtests, but it now has one
authoritative Shipping path with clean/distribution/IoStore/compression/prerequisite flags. Older
itch builds have an open `~` console and development cvars; the Epic artifact must not.

For the public storefront build, run `Deploy/playtest/package-playtest.ps1 -Config Shipping` and use
only `Packaged/Release/Windows` as the BuildPatchTool source.

---

## Still open (not blocking, tracked)

- 49 minors + 38 nits from code-review Pass 2 — [issue #24](https://github.com/SourceCodeMan/combatforge/issues/24),
  details in `docs/code-review/modules/*.md`. None are ship-blockers.
- No Apple code signing (Gatekeeper warning on Mac).
- Steam remains unstarted by choice — Epic first.
