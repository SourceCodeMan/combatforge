# Project Code Review Tracker

| Field | Value |
|-------|-------|
| **Started** | 2026-07-17 |
| **Completed full pass** | 2026-07-17 |
| **Criteria** | See [RUBRIC.md](./RUBRIC.md) |
| **Baseline** | [BASELINE.md](./BASELINE.md) |
| **Contract** | `docs/design/05-code-contract.md` |

## Progress summary

| Metric | Value |
|--------|------:|
| Modules done | **12 / 12** |
| Modules in progress | 0 |
| Open **blocker** | **0** |
| Open **major** | **9** (see fix queue; Core B1/C1/C4 fixed 2026-07-18; U2 was C1 consumer) |
| Baseline still open | B6, B7, B8, B9, B10 (B1 fixed) |
| Last updated | 2026-07-18 |

### Counts by module (open only)

| Module | Major | Minor | Nit | Report |
|--------|------:|------:|----:|--------|
| Core | 0 | 1 | 1 | [core.md](./modules/core.md) |
| Combat | 0 | 8 | 2 | [combat.md](./modules/combat.md) |
| Building | 1 | 3 | 2 | [building.md](./modules/building.md) |
| Player | 1 | 4 | 1 | [player.md](./modules/player.md) |
| UI | 0* | 1 | 2 | [ui.md](./modules/ui.md) (*U2 tracks Core C1) |
| Objectives | 1 | 1 | 2 | [objectives.md](./modules/objectives.md) |
| Voting | 0 | 2 | 2 | [voting.md](./modules/voting.md) |
| AI | 2 | 1 | 2 | [ai.md](./modules/ai.md) |
| Online | 1 | 2 | 2 | [online.md](./modules/online.md) |
| Input+Audio+root | 0 | 3 | 4 | [input-audio-root.md](./modules/input-audio-root.md) |
| Scripts | 2 | 3 | 3 | [scripts.md](./modules/scripts.md) |
| Deploy+Config | 1 | 5 | 4 | [deploy-config.md](./modules/deploy-config.md) |

---

## Fix queue (blocker + major only)

Root-cause list. Prefer this order for fix passes.

| ID | Title | Module | Status |
|----|-------|--------|--------|
| **B1** | Respawn mode: timed rounds always draw / match 0–0 | Core | **fixed** |
| **C1** | Domination HUD “first to N” shows 50; match ends at 200 | Core | **fixed** |
| **C4** | Fall death soft-respawns in Elimination (free mid-round reset) | Core | **fixed** |
| **P1** | Listen-host melee always rejected (shared `LastMeleeTime`) | Player | open |
| **BD1** | Yard shell vertical extents use Warehouse HeightCap (short midline/bounds) | Building | open |
| **O1** | CTF drop/return needs re-overlap (BeginOverlap only) | Objectives | open |
| **AI1** | Bots keep targeting corpses in continuous modes (`bEliminated` ignored) | AI | open |
| **AI2** | `ProximityAwareUU` 360° close-sense never used | AI | open |
| **ON1** | Fleet heartbeat 409 never re-registers → directory ghost | Online | open |
| **S1** | `Package-Mac.command` no Saved/pdb scrub | Scripts | open |
| **S2** | `retarget_rifle_to_bandit.py` hard-codes personal Claude temp path | Scripts | open |
| **D1** | `package-playtest.ps1` no Saved/pdb privacy scrub | Deploy | open |

**Not separate fix items:** U2 (UI correctly reads `RoundWinsToTake`; fix C1).

---

## Queue status

| # | Module | Files | Status | Reviewed | GitHub issue | Notes |
|---|--------|------:|--------|----------|--------------|-------|
| 1 | Core | 21 | **done** | 2026-07-18 | [#10](https://github.com/SourceCodeMan/combatforge/issues/10) | B1, C1, C4 major **fixed** |
| 2 | Combat | 26 | **done** | 2026-07-17 | [#11](https://github.com/SourceCodeMan/combatforge/issues/11) | B6 open; minors only |
| 3 | Building | 17 | **done** | 2026-07-17 | [#12](https://github.com/SourceCodeMan/combatforge/issues/12) | BD1 major; B9 open |
| 4 | Player | 10 | **done** | 2026-07-17 | [#13](https://github.com/SourceCodeMan/combatforge/issues/13) | P1 major; B7/B8/B10 open |
| 5 | UI | 22 | **done** | 2026-07-17 | [#14](https://github.com/SourceCodeMan/combatforge/issues/14) | B2/B5 fixed |
| 6 | Objectives | 5 | **done** | 2026-07-17 | [#15](https://github.com/SourceCodeMan/combatforge/issues/15) | O1 major (CTF) |
| 7 | Voting | 2 | **done** | 2026-07-17 | [#16](https://github.com/SourceCodeMan/combatforge/issues/16) | minors |
| 8 | AI | 4 | **done** | 2026-07-17 | [#17](https://github.com/SourceCodeMan/combatforge/issues/17) | AI1, AI2 major |
| 9 | Online | 2 | **done** | 2026-07-17 | [#18](https://github.com/SourceCodeMan/combatforge/issues/18) | ON1 major (fleet) |
| 10 | Input+Audio+root | 7 | **done** | 2026-07-17 | [#19](https://github.com/SourceCodeMan/combatforge/issues/19) | minors/nits |
| 11 | Scripts | 17 | **done** | 2026-07-17 | [#20](https://github.com/SourceCodeMan/combatforge/issues/20) | S1, S2 major |
| 12 | Deploy+Config | ~20 | **done** | 2026-07-17 | [#21](https://github.com/SourceCodeMan/combatforge/issues/21) | D1 major |

---

## File inventory (status = done for all listed)

### 1. Core — done
`CombatForgeGameMode`, `GameState`, `PlayerController`, `PlayerState`, `GameInstance`, `Types`, `PFPaths`, `PFUserPrefs`, `PFLightingSubsystem`, `PFWarehouseStreamSubsystem`, `PFClientLogShip`

### 2. Combat — done
`PFAmmoBarrel`, `PFBombActor`, `PFBombPickup`, `PFCombatAudio`, `PFCombatVFX`, `PFGrenadeProjectile`, `PFHealthComponent`, `PFPaintballProjectile`, `PFSmokeSubsystem`, `PFSplatSubsystem`, `PFTargetDummy`, `PFWeaponCatalog`, `PFWeaponComponent`

### 3. Building — done
`PFArenaSeed`, `PFArenaSerialization`, `PFArenaShell`, `PFBuildComponent`, `PFBuildGrid`, `PFBuildPieceActor`, `PFBuildPieceVisuals`, `PFGridMath`, `PFYardShell`

### 4. Player — done
`CombatForgeCharacter`, `PFCameraShakes`, `PFCharacterCustomization`, `PFCharacterMovementComponent`, `PFCharacterPreviewActor`

### 5. UI — done
All 11 widget pairs under `UI/`

### 6–9. Objectives, Voting, AI, Online — done

### 10. Input + Audio + root — done
`PFInputConfig`, `PFMusicSubsystem`, `CombatForge` module, Target.cs files

### 11–12. Scripts, Deploy, Config — done

---

## Recommended next steps

1. **Fix pass A (gameplay majors):** B1, C1, P1, BD1, O1, AI1  
2. **Fix pass B (ops/privacy):** S1, D1, S2  
3. **Fix pass C (online/AI polish):** ON1, AI2  
4. **Re-review** touched modules after fixes  
5. Optionally grind minors from Combat/Player (prediction, fire predict desync)

Or ask: **“fix majors”** / **“fix P1 C1”** / **“re-review Core after fixes”**.
