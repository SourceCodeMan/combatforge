# Breachworks (working title) — Post-Graybox Roadmap

> Status snapshot (2026-07-10): v1 graybox **compiles & links clean** (VS 2022 Community + UE 5.6.1).
> `L_Graybox.umap` created & committed. **First 2-player PIE playtest pending** (do this first, next session).
> Formerly "PaintForge" — renaming to **Breachworks** (see naming notes below; not yet locked).

This roadmap is the durable plan so work can resume fast across sessions. Detailed art-integration
steps live in [`art-integration-playbook.md`](art-integration-playbook.md) (generated separately).

---

## M0 — Validate the loop (do this first)
The graybox has **never been played**. Run the 2-player listen-server PIE test (setup in `pc-setup.md` §8):
Number of Players = 2, Net Mode = Play As Listen Server → walk Lobby→Build→Combat→Vote→Results.
- **Success signal:** full loop completes + a match JSON lands in `Saved/Arenas/`.
- **Watch for:** listen-server replication asymmetry (host works / client breaks — risk R5, the #1 bug class).
  Tail `Saved/Logs/PaintForge.log` during play.
- If bugs appear, fix them before any art work. Everything below assumes a green playtest.

## M1 — First-playable ART PASS (the big next step)
Turn the primitive graybox into the realistic-military, non-lethal look — **materials/assets on the
existing primitives**, not a rewrite. Assets are mostly already acquired (see table). Do a **vertical
slice first** (one weapon + one skinned material + one character + one impact VFX visible in a match)
to get a "this looks like a real game" moment before going wide. Detailed steps in the playbook doc.

**Acquired free assets → integration targets:**

| Asset (owned/queued) | Source | Plugs into (code seam) |
|---|---|---|
| Quixel **Megascans** surfaces (concrete/metal) | Quixel Bridge (grab in-editor) | `PFBuildGrid` 7 ISMs + `APFArenaShell` — skin primitives; per-instance custom-data float already carries team → add tint/emissive accent for team readability |
| **Lyra** rifle+pistol + FP/TP anims | Epic Launcher → Samples (grab) | `PFWeaponComponent`, `PaintForgeCharacter` (harvest assets, not the framework) |
| **MetaHuman** bodies (2 teams) | MetaHuman Creator (grab) | `PaintForgeCharacter` skeletal mesh + anim BP; teams via material/faction variant |
| **Game Animation Sample** + **Animation Starter Pack** | Fab library ✅ | character locomotion + rifle fire/ADS/reload (retarget to chosen skeleton) |
| **Niagara Examples Pack** | Fab library ✅ | `PFSplatSubsystem` (stub) — recolor impacts → paint splat/decals |
| **50 Free Game Sounds** + **UI SFX Free** | Fab library ✅ | `PFCombatAudio` (stub) + build-menu/HUD SFX |
| First **themed empty map shell** (warehouse) | build from Megascans-on-primitives (paid kits deferred) | reskin `APFArenaShell` (keep functional collision, drape visuals); constraints: ceiling ≥12m, clear ~64×40m floor, spawn/neutral mapping |

**Team-readability constraint (carry forward):** realistic materials fight the team-tint. Move team
identity to an **overlay** (emissive trim / player uniform) — don't tint the whole surface.

## M2 — Non-lethal wording + naming (small, quick)
Only on-screen lethal word is **"SUDDEN DEATH"** (3 UI spots) → rename (Overtime / Final Round /
Showdown). Optional internal renames (`corpse`, `death cam`) for consistency — low priority.
Wire the chosen **display title** (Breachworks) — separate from the code module (`PaintForge`/`PF*`
prefixes can stay). **Do these AFTER the playtest** (don't change gameplay code before validating).

## M3 — Feel & balance tuning
Needs playtest data — can't pre-research well. Movement feel (sprint/slide/ADS under `Net PktLag=100`),
build economy, round timings, hit registration. Tune against the numbers in `04-combat-feel.md` / `03-build-system.md`.

## M4 — Netcode hardening + EOS planning (future-proofing for crossplay/console)
Move match discovery from raw IP-connect toward **EOS (Epic Online Services)** — free, cross-platform,
the standard indie crossplay path; additive (GameMode phase logic unchanged). Prerequisite for the
Phase-2 console goal (Xbox ID@Xbox + PlayStation Partners, both gated + cert). Research EOS session
integration scope. See memory: platform-targets note.

## M5 — v1.1 deferred features
From `03-build-system.md` §8/§9: Edit tool (G, reserved), path/reachability flood-fill warning,
45° prop rotation. Post-first-playable.

## M6 — Packaging / Shipping smoke test
Project Settings → Packaging (Pak + IoStore, maps = L_Graybox), Shipping/Win64. Smoke-test EARLY —
packaging failures are the classic end-of-project landmine. Engine `FObjectFinder` assets cook automatically.

## M7 — Arena-pool backend (the north-star vision)
The vote loop already writes per-match JSON keyed by content-derived arena fingerprints. Future: aggregate
"great arenas (and great halves)" into a community pool that can be rated/recombined. Backend design TBD;
the JSON format in `03-build-system.md` §7 is the contract.

---

## Prep that does NOT depend on the playtest (safe to research/queue now)
- Art-integration playbook (UE 5.6 workflows per asset) — **in progress**, see playbook doc.
- EOS integration scope/research (M4).
- Finalize the **Breachworks** name: USPTO Class 9/41 search; register `breachworks.gg` + `playbreachworks.com`
  (`.com` held by an unrelated cybersecurity firm). Traffic angle explored & rejected (keep as Steam-tag/SEO play).
