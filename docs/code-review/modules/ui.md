# UI module review — Pass 2

| Field | Value |
|-------|-------|
| **Date** | 2026-07-23 |
| **Reviewer** | independent Pass 2 (current code only) |
| **Scope** | `/home/josh/projects/combatforge/Source/CombatForge/UI/` |
| **Files** | 22 (11 `.h` + 11 `.cpp`) |
| **Status** | done |

## Summary

The UI package is a pure client-side UMG layer: code-built widget trees (`RebuildWidget` → `BuildTree` before `Super`), phase switcher ownership in `UPFRootHUDWidget`, and pawn/GameState binding pushed down from root (no child-held possession across swaps). Cross-package wiring for the build wheel, scoreboard Tab-hold, and options overlay is centralized and cleaned up on destruct.

**No blockers.** Architecture is solid for a kids/airsoft-tone graybox HUD. Remaining issues are settings dual-controls, rebind capture lifecycle, dead loadout code paths, hotkey/label mismatch on the 12-sector wheel, and a few per-tick / maintenance nits. Host-gated RPCs are UX-gated only with server re-validation (correct).

## Findings

### P2-U1. Key-rebind capture not cancelled when Options closes
- **Severity:** major
- **File:** `Source/CombatForge/UI/PFOptionsWidget.cpp:931-943` (`Close`); `:648-686` (`BeginListen` / `NativeOnKeyDown`); `:945-954` (`NativeDestruct`)
- **Symptom:** Player clicks a rebind row (“press key…”), then Esc / BACK / Quit closes the menu without finishing the capture. On the next open (or if focus returns while `bListeningForKey` is still true), the next keypress can be swallowed as a rebind or leave labels stuck on “press key…”.
- **Why:** `BeginListen` sets `bListeningForKey = true` and `ListeningIndex`. Escape *inside* the focused widget cancels, but `Close()`, `Open()`, and non-embedded `NativeDestruct` never clear those flags. Only `OnResetBinds` resets them.
- **Fix:** Clear `bListeningForKey` / `ListeningIndex` and call `RefreshRebindLabels()` at the start of `Close()` and `Open()` (and non-embedded destruct if open).
- **Confidence:** high

### P2-U2. Fullscreen checkbox and Window-mode control desync / fail to persist
- **Severity:** major
- **File:** `Source/CombatForge/UI/PFOptionsWidget.cpp:271-284` (legacy Fullscreen checkbox); `:998-1001` (`OnFullscreenChanged`); `:1014-1018` (`OnWindowModeClicked`); `:1338-1391` (`PushToSettings`); `:1279-1316` (`PullFromSettings`)
- **Symptom:** Toggling the Fullscreen checkbox often does not match the Window mode button, and “fullscreen” state may apply for the current session but revert next launch.
- **Why:**
  1. `PushToSettings` maps display mode as: windowed only if `WorkingWindowMode == 2 && !bWorkingFullscreen`; otherwise always `WindowedFullscreen`. Checking Fullscreen while Window mode is Windowed applies borderless for this session but still saves `WorkingWindowMode` unchanged (still 2).
  2. `OnFullscreenChanged` only mutates `bWorkingFullscreen` — never `WorkingWindowMode`.
  3. `PullFromSettings` seeds window mode from `UGameUserSettings`, then **overwrites** `WorkingWindowMode` from `FPFUserPrefs::GetWindowModeIndex()` without re-syncing `bWorkingFullscreen` to the prefs value.
- **Fix:** Single source of truth: either drop the legacy Fullscreen checkbox, or have it set `WorkingWindowMode` to 0/2 (and keep `bWorkingFullscreen` derived). After loading prefs, set `bWorkingFullscreen = (WorkingWindowMode == 0)`.
- **Confidence:** high

### P2-U3. Build wheel sector digits mislabeled vs hotkeys (12 sectors, 10 keys)
- **Severity:** minor
- **File:** `Source/CombatForge/UI/PFBuildWheelWidget.cpp:94-96` (digit labels); `:306-325` (`NativeOnKeyDown`); `PFBuildWheelWidget.h:83` (`NumSectors = 12`)
- **Symptom:** Sector widgets show labels `1`…`12`. Keys `1`–`9` and `0` only commit sectors `0`–`9`. Sector 9 is labeled **“10”** but activated by **0**; sectors 10–11 show **“11”/“12”** with no digit binding.
- **Why:** Labels use `i + 1`; key map is a fixed 10-entry table. Comment correctly says digits cover the first 10 sectors only — UI does not.
- **Fix:** Label sectors 0–8 as `1`–`9`, sector 9 as `0`, and leave 10–11 blank (or “—”); or bind extra non-digit keys if 12 hotkeys are required.
- **Confidence:** high

### P2-U4. Combat HUD refreshes full score strip every tick for FFA / objective modes
- **Severity:** minor
- **File:** `Source/CombatForge/UI/PFCombatHUDWidget.cpp:1385-1394` (`NativeTick` → `HandleScoreChanged`); `:573-602` (FFA branch rebuilds strings / pips)
- **Symptom:** During FreeForAll, CTF, Domination, and Hardpoint, every frame rebuilds score/pip text even when values are unchanged (TagCount / team scores tick via PS replication without a GS multicast).
- **Why:** Polling is intentional (no GS score event for those fields), but `HandleScoreChanged` always writes widgets with no dirty check.
- **Fix:** Cache last `MyTags`/`LeadTags`/`TeamScores`/`RoundWinsToTake`/`MatchType` and early-out when unchanged; or throttle to 5–10 Hz.
- **Confidence:** high

### P2-U5. Build budget readout hardcodes 30 / 6 caps
- **Severity:** minor
- **File:** `Source/CombatForge/UI/PFBuildHUDWidget.cpp:344-346` (`UpdateBudgetText`)
- **Symptom:** HUD always shows `▦ N/30   ◆ N/6` regardless of any future GameMode / balance change to structural or prop caps.
- **Why:** Caps are string literals (“contract B6”) rather than shared constants or replicated limits.
- **Fix:** Use the same constants the build rules use (or read remaining from PlayerState + cap fields if exposed).
- **Confidence:** high

### P2-U6. FOV slider is not live-applied (unlike sens / brightness)
- **Severity:** minor
- **File:** `Source/CombatForge/UI/PFOptionsWidget.cpp:1103-1107` (`OnFovChanged`); `:1515-1523` (`ApplyFieldOfView` only from `PushToSettings`)
- **Symptom:** Dragging FOV only updates the label until APPLY / Close; sens, brightness, and contrast preview immediately.
- **Why:** `OnFovChanged` clamps + `RefreshLabels` only; `ApplyFieldOfView` runs solely in `PushToSettings`.
- **Fix:** Call `ApplyFieldOfView(WorkingFov)` from `OnFovChanged` (same pattern as `OnSensChanged`), still persist on Apply/Close.
- **Confidence:** high

### P2-U7. Dead loadout overlay / empty boot-menu switcher page
- **Severity:** minor
- **File:** `Source/CombatForge/UI/PFLobbyWidget.cpp:192-195`, `:316-418` (`BuildLoadoutOverlay` never called); `:457-478` (handlers); `:597-607` (tick still guards `LoadoutOverlay`); `Source/CombatForge/UI/PFLoadingMenuWidget.cpp:535-621` (`BuildLoadoutPage` unused); `:1903-1932` (empty switcher index 2)
- **Symptom:** Dead code and an empty `MenuSwitcher` page (index 2) left after loadout was merged into CLASS. No player-facing break (no tab opens the empty page), but increases maintenance risk and binary size.
- **Why:** Comments document the merge; tree still constructs a placeholder page and retains full unused builders/handlers (`OnLoadout*`, `MakeConfigButton` in lobby).
- **Fix:** Delete unused builders/handlers/members; collapse switcher to real pages only (or repoint CLASS to index 2 and drop the empty slot).
- **Confidence:** high

### P2-U8. Tool display names duplicated (Build HUD vs piece visuals)
- **Severity:** minor
- **File:** `Source/CombatForge/UI/PFBuildHUDWidget.cpp:30-48` (`ToolDisplayName`); `Source/CombatForge/UI/PFBuildWheelWidget.cpp:43-46` (uses `PFBuildPieceVisuals::DisplayName`)
- **Symptom:** Renaming a piece in one path (e.g. Roof → “Ceiling”) can leave the wheel and bottom reel disagreeing.
- **Why:** Build HUD keeps a private switch; wheel already shares `PFBuildPieceVisuals`.
- **Fix:** Route Build HUD through `PFBuildPieceVisuals::DisplayName` (same as wheel).
- **Confidence:** high

### P2-U9. Elim-confirm / death board victim lookup is name-string based
- **Severity:** minor
- **File:** `Source/CombatForge/UI/PFCombatFeedbackWidget.cpp:294-314` (`LookUpRecentVictimName`); `Source/CombatForge/UI/PFCombatHUDWidget.cpp:1090-1107` (elim overlay “Eliminated by”)
- **Symptom:** Duplicate display names (two “Player”, bots, etc.) can attach the wrong victim/killer line for a short window; race with feed OnRep is already handled via `bElimTextNamePending`.
- **Why:** `OnHitConfirmedEvent` carries no victim id; feed entries match `ShooterName` / `VictimName` strings + 3–10 s time window.
- **Fix:** Prefer player id / net id on elim feed entries (core change) and match on that from UI.
- **Confidence:** med

### P2-U10. Unused SmallFormat pip constants left after RoundWinsToTake replication
- **Severity:** nit
- **File:** `Source/CombatForge/UI/PFCombatHUDWidget.h:136-138` (`SmallFormatPips`, `SmallFormatMaxPlayers`)
- **Symptom:** Dead constants; comment still mentions inferring first-to-3 from roster size, but `HandleScoreChanged` correctly uses replicated `RoundWinsToTake`.
- **Fix:** Remove unused constexprs (or use them only if a local fallback is still needed).
- **Confidence:** high

### P2-U11. Crosshair “halo” is a solid square, not a ring
- **Severity:** nit
- **File:** `Source/CombatForge/UI/PFCombatHUDWidget.cpp:82-94`, `:957-978` (`CrosshairHalo`)
- **Symptom:** Bloom-near-cap tell is a translucent filled square sized to spread diameter — reads as a blob, not a ring.
- **Why:** `MakeSolidImage` + size change; no rounded/ring brush (feedback blobs use `RoundedBox` half-height).
- **Fix:** Reuse the rounded-box circle brush from feedback, or a thin ring material.
- **Confidence:** high

### P2-U12. How-to-Play copy duplicated (boot menu + Options tab)
- **Severity:** nit
- **File:** `Source/CombatForge/UI/PFLoadingMenuWidget.cpp:259-288`; `Source/CombatForge/UI/PFOptionsWidget.cpp:816-852`
- **Symptom:** Control/mode text drifts between boot menu and in-game Options (already diverged on scroll-weapon wording, Dom scores, etc.).
- **Fix:** Shared helper or single data table of howto lines.
- **Confidence:** high

### P2-U13. `ApplyWindowAndResolution` is an empty stub
- **Severity:** nit
- **File:** `Source/CombatForge/UI/PFOptionsWidget.h:109`; `PFOptionsWidget.cpp:1526-1529`
- **Symptom:** Dead API surface; all work is inlined in `PushToSettings`.
- **Fix:** Remove declaration/definition or actually factor resolution apply into it.
- **Confidence:** high

## Subsystems audited clean

| Subsystem | Notes |
|-----------|--------|
| **Root phase switcher** | Child index == `EPFMatchPhase` (Lobby..Results = 0..4); late GS bind + join-in-progress catch-up; wheel forced cancel off Build. |
| **Pawn rebind** | Root `WirePawn` + tick belt-and-braces; children unbind previous components; wheel look-input ignore paired on close/destruct. |
| **Build wheel open gate** | Root rejects open when `!IsBuildAllowed()` and rolls back `NotifyBuildWheelClosed`. |
| **Scoreboard Tab hold** | Hidden in Lobby (T22); `HitTestInvisible` when shown; signature-gated row rebuild. |
| **Vote flow** | Thumb → chips → submit; max 4 selections; auto-submit 0.5 s before phase end; disable after send; ring uses `PhaseDuration`. |
| **Results** | Mode-aware winner/MVP/scoreboard; host Return button follows `OnMatchLeaderChanged`; arena id cached per `MatchId`. |
| **Combat feedback** | Hitmarker/elim audio, damage arcs, mask splat pool + wipe on elim/Freeze; name pending on feed race. |
| **Combat HUD locational HP** | Pip drain uses max region/total damage fraction; empty when eliminated; showdown collapses to 1 pip. |
| **Domination chips / capture bar** | Partial zone cache rejected until 3 actors; capture meter only while local on-point; Hardpoint correctly leaves capture idle (no false Dom UI). |
| **Lobby roster** | Ghost PS skipped; signature rebuild; host-only team cycle RPC (server re-validates). |
| **Options embed** | Boot menu embeds options without stealing input mode; saves on embedded destruct. |
| **Loading menu weapons** | Browse locked guns freely; equip/save only when unlocked; secondary any-category; lock lines full-width. |

## File checklist

| File | Reviewed | Notes |
|------|----------|-------|
| `PFRootHUDWidget.h/.cpp` | yes | Phase switcher, wheel/scoreboard/options wiring, pawn push-down |
| `PFCombatHUDWidget.h/.cpp` | yes | Crosshair FOV projection, multi-mode score, Dom HUD, out overlay |
| `PFCombatFeedbackWidget.h/.cpp` | yes | Hitmarker, arcs, splats, elim text |
| `PFBuildHUDWidget.h/.cpp` | yes | Budget, reel, deny flash, ready poll, timer warnings |
| `PFBuildWheelWidget.h/.cpp` | yes | 12 sectors, mouse delta hover, digit commit, look-input |
| `PFLobbyWidget.h/.cpp` | yes | Roster, read-only setup, dead loadout path |
| `PFOptionsWidget.h/.cpp` | yes | Video/audio/controls/class/howto, rebinds, embed mode |
| `PFResultsWidget.h/.cpp` | yes | Winner/MVP/scoreboard/tally/arena id/return |
| `PFScoreboardWidget.h/.cpp` | yes | Mode-aware hold-Tab board |
| `PFVoteWidget.h/.cpp` | yes | Two-step vote + ring paint + auto-submit lead |
| `PFLoadingMenuWidget.h/.cpp` | yes | Warmup, setup cards, maps, online panel, class/weapon, options embed |

## Severity tally

| Severity | Count |
|----------|-------|
| blocker | 0 |
| major | 2 |
| minor | 7 |
| nit | 4 |

## Suggested fix order

1. **P2-U1** — rebind capture clear on Close/Open (small, high UX impact).
2. **P2-U2** — collapse window-mode dual controls to one persisted source.
3. **P2-U3** — wheel digit labels.
4. **P2-U7 / U8 / U10 / U13** — dead code and display-name single source.
5. **P2-U4 / U6** — tick dirty-check and live FOV if desired.
