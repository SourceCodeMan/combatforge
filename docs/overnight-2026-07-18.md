# CombatForge — Overnight Work Report (2026-07-17 → 18)

Worked through your bug + feature list as **code changes only** — nothing packaged, nothing pushed to itch,
the live server was NOT touched. You decide when to ship. I compiled the whole game module
(`CombatForgeEditor Development`) to verify everything builds; see **Build status** at the bottom.

Legend: ✅ done in code · 🎨 needs you in-editor (asset/socket/anim) · ☁️ needs backend deploy (I wrote it, you `wrangler deploy`) · 🩺 diagnosis / needs your call · 🧱 scaffold + plan (bigger feature)

---

## BUGS

**✅ Blurry / cloudy visuals (#3).** Three real causes fixed, all code:
- The default resolution was **1080p folded into a sub-native render scale** on any 1440p/4K monitor → TSR
  upscaled → blur. Defaulted it to native (`PFUserPrefs.cpp`), so it renders 1:1 out of the box; the dropdown
  still works as a perf knob if you lower it.
- TSR shipped with **zero sharpening** → added `r.TSR.Sharpen=0.6` + `r.Tonemapper.Sharpen=0.4`
  (`DefaultEngine.ini [SystemSettings]`).
- The "cloudy" was **volumetric fog haze** — halved it (density 0.008→0.005, max-opacity 0.28→0.14) but kept
  the god-ray shafts you like, and dropped bloom 0.28→0.15 (`PFLightingSubsystem.cpp`). Also a small GPU win.

**✅ Pistol upside-down / on the crotch when firing (#5, part of #45).** Root cause: the third-person "raised"
(fire/ADS) pose hardcoded the rifle's barrel axis (`Aim.Yaw-90, roll 0`) for **every** gun. Made it per-weapon
(`FPFWeaponDef::TPRaisedYawOffset/Roll`, applied in `ApplyWeaponLoadout`, consumed in `ApplyRaisedWeaponPose`),
rifles unchanged, and auto-flip pistols/revolvers 180°. ⚠️ The pistol mesh's exact axis I couldn't verify
without the editor — if it's still off, it's now **two numbers** to tweak in the catalog, not a rewrite.

**🩺 "Gun drops to the waist when they shoot" (rifle) (#4/#5).** This is a deeper thing: the TP animation has
no aim-offset, so the code detaches the gun and floats it to eye-height while the arms stay down — reads as
disconnected/low. I deliberately did **not** rewrite this blind (high regression risk). The proper fix is a
TP aim-offset pose in an AnimBP (or reduce the raise). Left rifle behavior exactly as-is. Flagging for a
focused pass with you watching the result.

**✅ Legs printing through pants / "privates showing" (#10).** The bare-legs skin (`SKM_Legs`) was always
visible under the pants. Now hidden whenever a Pants garment is worn — in **both** the class preview and the
in-game body (`PFCharacterPreviewActor.cpp` + `CombatForgeCharacter::ApplyCharacterConfig`).

**🎨 Preview torso still stretched (#6).** The render target is already 600×800 (0.75 aspect) and the mesh has
no scale distortion — so any residual stretch is the **UMG image widget's** on-screen aspect not being 0.75.
Check the `CharPreviewImage` box in the widget: it must be 3:4 (e.g. 300×400). That's an editor/asset tweak.

**✅ Class menu: "weapon not attached" for a whole category + can't scroll all categories (#11).** The rank-lock
gate reverted the *entire* category step when its first weapon was locked, making that category unreachable.
Decoupled browsing from equipping — you can now scroll every category/weapon freely (with 🔒 rank badges),
and a locked pick just isn't saved/equipped (`PFLoadingMenuWidget::NotifyWeaponStep`).

**✅ Disconnect from server (#12).** Added a **DISCONNECT FROM SERVER** button to the boot menu when you're
connected as a client (it mirrors STOP HOSTING). Returns you to a standalone boot menu so you can host or play
local. (The underlying disconnect already existed; it just had no button.)

**✅ Maps persist on the server across updates (#9).** Maps already save to a persistent per-user dir on
desktop, but on the box the build runs with `-NOHOMEDIR`, so that dir resolved **inside the extracted build**
and got wiped every redeploy. Added a `-ArenaDir=<abs path>` override (`PFPaths.cpp`) and pointed the box
launcher at `C:\ProgramData\CombatForge\Arenas` (`Start-Server-OnBox.ps1`) — outside the build, survives
redeploys. The "no maps exist when connected" is a *separate* thing: the map picker is host-local, and a
connected client can't see the server's maps (that needs a replicated map list — noted as a follow-up).

**🩺 XP "resets" every version push (#8).** Investigated hard: XP is **100% backend-authoritative** (D1 ledger),
the client stores no XP, and a version/package update **cannot** wipe it — a logged-in account re-fetches the
same XP every boot. The realistic causes are (a) testing while **not logged in** (anonymous installs never earn
XP), or (b) the box's `ServerKey.txt` / un-sent reports living in the wiped `-NOHOMEDIR` dir, so no XP was ever
minted after a redeploy. I moved **ServerKey.txt, PendingReports (un-sent XP), and JoinCode** to the persistent
`ServerDataDir` (same `-ArenaDir` root) so a redeploy can't drop un-minted XP (`PFBackendSubsystem.cpp` +
`CombatForgeGameMode.cpp`). **Please confirm:** are your testers logged in? Check D1 `xp_events` after a match.

**☁️ XP for local/offline play too (#13).** Currently only fleet servers mint XP. Added a new **`/v1/casual-report`**
endpoint (`combatforge-api/src/casual.ts`): session-authed (honor-system), **halved** vs fleet, hard **daily
cap** (1500), idempotent, every event tagged `casual_*` so it's fully auditable/voidable, and a
`CASUAL_XP_ENABLED=0` kill-switch. The game now self-reports the **host's own** result for solo/bots/listen
matches (`CombatForgeCharacter`/`GameMode` + `PFBackendSubsystem::SendCasualReport`). *Needs you to
`wrangler deploy` the backend.* Follow-up: joined clients on a listen server self-reporting (each from their
own machine) — needs a small replicated "is-fleet" flag so it doesn't double-count on real servers.

**🩺 Barrels → black checkers for SOME clients on v7 (#2/#89).** This is **not** a missing asset (the warehouse
dir is force-cooked, so it'd checker for *everyone* if missing). Per-client-on-the-same-build = a shader-library
/ GPU-permutation / partial-pak issue. A blanket material override would wreck the good look for everyone and
still not fix it, so I left the barrel code alone. To diagnose: get the affected client's log and look for
shader-compile / "using default material" lines; confirm they're on the identical pak; check `r.VirtualTextures`
on their machine (the warehouse mats use VT). Happy to add a "force solid barrels" toggle if you want a
guaranteed-no-checker fallback.

**🩺 Missing cone piece (#1).** There is deliberately **no cone build-piece** anymore — the old "PropDorito"
cone was intentionally changed to a crate, and nothing in the catalog is a "cone with a roof attached." I did
**not** guess-change build pieces (it can break saved maps). **Tell me exactly which piece you mean** (a
traffic-cone prop? a peaked roof? bring the crate-cone back?) and I'll add it as a new piece safely.

**🎨 Bots open doors + climb windows (#7).** Doors: ✅ **done in code** — bots now open any closed door they
stall against (`PFBotController` calls the door's `AuthorityTryToggleDoor`; one-way doors self-validate the
front face). Windows: left as a follow-up — window frames keep collision so there's no nav path through them;
a "vault the window" bot heuristic is the clean next step (pure code, just wanted to land doors first).

---

## FEATURES

**✅/🎨 Sniper scope zoom (F1).** Zoom half is done: per-weapon `ScopedADSFOV`, snipers now zoom **hard**
(FOV 28 vs the normal 58) — `PFWeaponCatalog.h` + `ApplyWeaponLoadout`. The **"in the scope, not the whole
screen" mask** (black surround + reticle) is the remaining piece — I did NOT ship untested runtime-texture HUD
code I couldn't verify tonight (a HUD crash would be bad). Two clean ways to finish it: a scope PNG overlay
(simple), or a runtime-generated circular mask texture (no asset). Recommend the PNG; ~1 hr with you.

**🧱 Weapon attachments (F2).** Wrote a full design + data-model plan: `docs/attachments-plan.md`. The gameplay
half (attachment stat-deltas that modify recoil/accuracy/ADS/range, CoD-style tradeoffs) is pure C++ layered on
the existing per-weapon stats; the **meshes + weapon sockets** are your in-editor work. Recommended first PR:
ship it **stat-only** (no meshes) so you feel the tradeoffs immediately, meshes drop in later. Didn't half-build
a UI that can't show real attachment meshes.

**✅/🧱 Store in the menu + AAA menu (F3).** Store: ✅ added a **STORE** button (opens the web store — checkout is
web-side by design, no card entry in a kids client). A fuller in-game store *panel* (lists packs/unlocks from
the existing `/v1/store`) is a clean follow-up. AAA menu polish (video/image background, more interactive) is a
bigger design pass — noted, mostly asset-driven (a looping background video needs a media asset, which is the
same Launcher-media limitation we hit before; an image carousel or a live scene-capture background are the
code-friendly options).

**✅ Class behind an account (F4).** No account → **1 soldier** (class slot 0). Logged in → all **5**
(`NotifySaveSlotSelected` gate + the class-page subtitle explains it). Enforced even offline (it's the nudge to
make an account). **Website updated** too — `combatforge-site` account card now states "one soldier free, all
five with a free account." (Follow-up: the in-game Options class-cycle + true server-side enforcement share the
same seam as weapon-unlock enforcement, which is also still advisory — noted.)

**✅ Melee → Punch, bindable to mouse + keyboard (F5).** Reframed the melee as **"Punch,"** and it's now in the
rebind list so you can assign it to a mouse button OR a key (defaults: B + thumb-mouse). Left a precise hook
where your punch **animation montage** plugs in later (the punch works now, just animation-less).

**✅ AFK kick after 3 min (F6).** Server-authoritative idle detection (samples the pawn's position + view every
second; no reliance on client input, so it's anti-farm). A **remote** player idle for 3 full minutes is
returned to the main menu. Never kicks bots, the host, or standalone.

---

## Server-side / D1 (your hands — I'm blocked from prod D1)

- **Rename the server off "tom-home-pilot" (#87).** The name lives in D1, set at provisioning. Run:
  `UPDATE game_servers SET name = 'CombatForge — Tom''s Server' WHERE name = 'tom-home-pilot';`
  (The player-count 0/12 half is already fixed in alpha-7.)
- **Deploy the backend for local XP (#13):** `cd combatforge-api && wrangler deploy` (adds `/v1/casual-report`).
  Optional kill-switch: `wrangler secret put CASUAL_XP_ENABLED` → `0` to disable, remove/`1` to enable.

## Do NOT forget on the next package
- Bump `PFBuild::NetProtocol` **7 → 8** in `Source/CombatForge/CombatForge.h` (several of these change content /
  networked behavior; the version gate must step so mixed 7/8 clients don't mis-render or mis-count XP).
- Re-copy `ServerKey.txt` to the box isn't needed if you pass `-PFServerKey` (the launcher does).

## Files touched (game): Config/DefaultEngine.ini, DefaultGame(n/c), Core/{PFPaths,PFUserPrefs,PFLightingSubsystem,CombatForgeGameMode}, Player/{CombatForgeCharacter,PFCharacterPreviewActor}, Combat/PFWeaponCatalog, Online/PFBackendSubsystem, UI/PFLoadingMenuWidget, Input/PFInputConfig, AI/PFBotController, Deploy/pilot/Start-Server-OnBox.ps1. Backend: combatforge-api/src/{casual.ts,index.ts}. Site: combatforge-site/public/index.html. Docs: attachments-plan.md, this file.
