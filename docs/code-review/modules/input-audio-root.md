# Input + Audio + Module Root — Pass 2 | Date 2026-07-23 | Status done

## Summary

Native Enhanced Input construction (`UPFInputConfig`), local phase BGM (`UPFMusicSubsystem`), and the game module shell (log category, net-protocol gate, engine-asset boot probes, UBT targets) are mostly sound and well-commented. Input rebinding has real conflict-coverage gaps; Build.cs still declares Media Framework deps after the SoundWave migration; music mutates a shared `USoundWave` loop flag at runtime. No blockers for the default play path.

**Counts:** 0 blocker · 1 major · 5 minor · 3 nit

## Findings

### 1. Rebind conflict check misses non-rebindable fixed keys
- **ID:** P2-I1
- **Severity:** major
- **Status:** open
- **File:** `Source/CombatForge/Input/PFInputConfig.cpp:288-320`
- **Symptom:** Player rebinds Jump/Sprint/Reload/etc. onto LMB, RMB, WASD, Space (if not Jump), R (if not Reload), Q, E, or other live mappings; two actions fire on one key with no Options rejection.
- **Why:** `SetActionKey` only rejects a small reserved list (`Escape`, `Tab`, `Enter`, `F`) and keys already held by other **rebindable** entries. Fire/ADS/Move/Look/Place/Build equip keys are not in the rebind registry, so they never participate in the conflict scan.
- **Fix:** Build a full “occupied keys” set from all live mappings on IMC_Common/IMC_Combat/IMC_Build (or at least a hard deny-list of fire/look/move/build hotkeys) before accepting a rebind; surface rejection in Options UI.
- **Confidence:** high
- **Source:** this-pass

### 2. Reserved `F` blocks restoring Interact to its default
- **ID:** P2-I2
- **Severity:** minor
- **Status:** open
- **File:** `Source/CombatForge/Input/PFInputConfig.cpp:299-307`
- **Symptom:** Player rebinds Interact off F, then cannot set it back to F (“reserved”).
- **Why:** Reserve check is `NewKey == R && E->CurrentKey != R`. Once Interact’s current key is not F, F is permanently rejected for every rebindable entry, including Interact itself (whose default is F).
- **Fix:** Allow the reserved key when it is that entry’s `DefaultKey`, or reserve F only for non-Interact ids.
- **Confidence:** high
- **Source:** this-pass

### 3. Dead MediaAssets / AudioMixer module dependencies
- **ID:** P2-I3
- **Severity:** minor
- **Status:** open
- **File:** `Source/CombatForge/CombatForge.Build.cs:33-34`
- **Symptom:** Extra modules linked/cooked for a BGM path that no longer exists; comments still claim MediaPlayer/`UMediaSoundComponent`.
- **Why:** `UPFMusicSubsystem` loads `USoundBase` / `USoundWave` and plays via `UAudioComponent`. Repo-wide search shows no `MediaPlayer` / `MediaSound` / MediaAssets includes outside Build.cs.
- **Fix:** Drop `MediaAssets` and `AudioMixer` from `PublicDependencyModuleNames` unless something reintroduces Media Framework; update the comment to SoundWave + AudioComponent.
- **Confidence:** high
- **Source:** this-pass

### 4. Runtime mutation of cooked SoundWave `bLooping`
- **ID:** P2-I4
- **Severity:** minor
- **Status:** open
- **File:** `Source/CombatForge/Audio/PFMusicSubsystem.cpp:127-131`
- **Symptom:** In editor/PIE, music assets may dirty or permanently flip loop; any other consumer of the same wave inherits looping.
- **Why:** `PlayTrack` does `Cast<USoundWave>(Sound)` then `Wave->bLooping = true` on the loaded asset object instead of configuring the `UAudioComponent` / a `USoundCue` / asset default.
- **Fix:** Set loop on the asset offline, or use `MusicComp` looping APIs / a dedicated looping SoundCue; avoid writing UAsset fields at runtime.
- **Confidence:** med
- **Source:** this-pass

### 5. Ambient bed duck uses `GetFirstPlayerController` only
- **ID:** P2-I5
- **Severity:** minor
- **Status:** open
- **File:** `Source/CombatForge/Audio/PFMusicSubsystem.cpp:135-147`, `155-178`
- **Symptom:** On edge setups where the local pawn is not the first PC’s pawn (rare; splitscreen / odd net), wind bed is not ducked/restored when phase music starts/stops.
- **Why:** Phase music stops/starts ambient via the first PC’s character combat audio only.
- **Fix:** Prefer the local player controller for this game instance (`GetFirstLocalPlayerController` / iterate local PCs) and null-check net mode (already dedicated-server gated at `SetPhaseMusic`).
- **Confidence:** med
- **Source:** this-pass

### 6. Header action count / comment drift
- **ID:** P2-I6
- **Severity:** nit
- **Status:** open
- **File:** `Source/CombatForge/Input/PFInputConfig.h:17-18`
- **Symptom:** Docs say “25 UInputActions”; `Build` creates 29 (`IA_*` members through `IA_BuildWheel`).
- **Why:** Comment not updated when actions were added (PlantBomb, WeaponDrag, etc.).
- **Fix:** “All native IA_* + 3 IMCs” without a hard count (cpp already avoids hardcoding counts at line 205).
- **Confidence:** high
- **Source:** this-pass

### 7. `NetProtocol` vs `ProjectVersion` metadata skew
- **ID:** P2-I7
- **Severity:** nit
- **Status:** open
- **File:** `Source/CombatForge/CombatForge.h:26` (`NetProtocol = 15`); packaging metadata lives in Config (see Pack C)
- **Symptom:** Binary speaks protocol 15; project settings may still show `0.1.0` without alpha suffix — operators can misread “version” if they look at Project Settings instead of the header.
- **Why:** Join gate correctly uses `PFBuild::NetProtocol`; ProjectVersion is separate cosmetic packaging field.
- **Fix:** Keep single source of truth documentation; optionally derive display version from NetProtocol in packaging docs only (do not dual-source the gate).
- **Confidence:** high
- **Source:** this-pass

### 8. Bare `AActor` music anchor
- **ID:** P2-I8
- **Severity:** nit
- **Status:** open
- **File:** `Source/CombatForge/Audio/PFMusicSubsystem.cpp:75-95`
- **Symptom:** None observed today; slightly fragile holder for a registered `UAudioComponent`.
- **Why:** Spawns `AActor::StaticClass()` with no root component, then `NewObject<UAudioComponent>(MusicAnchor)` + `RegisterComponent`.
- **Fix:** Optional: use a lightweight scene component root or `UGameplayStatics::SpawnSound2D` pattern; current approach is acceptable for 2D UI sound.
- **Confidence:** low
- **Source:** this-pass

### 9. Punch rebind leaves ThumbMouseButton as second mapping (documented feature)
- **ID:** P2-I9
- **Severity:** nit
- **Status:** open (wontfix candidate)
- **File:** `Source/CombatForge/Input/PFInputConfig.cpp:237-240`, `318-319`
- **Symptom:** After rebinding Punch, both the new key and mouse-thumb fire punch.
- **Why:** Registry only tracks the keyboard default (B); thumb mapping is never unmapped on rebind — intentional per comment.
- **Fix:** None required if intentional; if players report double-binds, either expose dual-bind UI or unmap all keys for the action before MapKey.
- **Confidence:** high
- **Source:** this-pass

## Subsystems audited clean

| Area | Notes |
|------|--------|
| **UPFInputConfig::Build** | Idempotent guard (`IMC_Common != nullptr`); outered to PC; GC roots via UPROPERTY; sensitivity clamp on load and `SetLookSensitivity`; invert-Y correct. |
| **IMC phase separation** | Combat vs Build shared keys (LMB, R, Q) documented and safe if contexts never coexist (controller responsibility). |
| **IA_Interact `bConsumeInput=false`** | Correct for sharing F with Ready under priority. |
| **Rebind registry scope** | Explicit exclusion of Move/Look/dual-map CrouchSlide is reasonable. |
| **UPFMusicSubsystem** | Dedicated-server no-op; SoftLoad + force-cook path documented in Config; volume mute without restart; Deinitialize destroys anchor. |
| **CombatForge module startup** | Commandlet skip; BasicShapes + Color param `ensureMsgf` probes. |
| **Targets** | Game/Editor/Server targets consistent UE 5.6 include order; Server target documents Launcher limitation. |
| **Build.cs** | `bWarningsAsErrors`; flat `PublicIncludePaths`; NetCore/Sockets/AI/Nav/HTTP/OpenSSL justified. |
| **DefaultInput.ini (cross-ref)** | Enhanced input default classes set (Pack C) — required for packaged binds. |

## File checklist

| File | Reviewed |
|------|----------|
| `Source/CombatForge/Input/PFInputConfig.h` | yes |
| `Source/CombatForge/Input/PFInputConfig.cpp` | yes |
| `Source/CombatForge/Audio/PFMusicSubsystem.h` | yes |
| `Source/CombatForge/Audio/PFMusicSubsystem.cpp` | yes |
| `Source/CombatForge/CombatForge.h` | yes |
| `Source/CombatForge/CombatForge.cpp` | yes |
| `Source/CombatForge/CombatForge.Build.cs` | yes |
| `Source/CombatForge.Target.cs` | yes |
| `Source/CombatForgeEditor.Target.cs` | yes |
| `Source/CombatForgeServer.Target.cs` | yes |
