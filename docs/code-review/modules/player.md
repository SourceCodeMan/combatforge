# Player module review — Pass 2

| Field | Value |
|-------|-------|
| **Date** | 2026-07-23 |
| **Reviewer** | (agent, independent Pass 2) |
| **Scope** | `Source/CombatForge/Player/` only (current code) |
| **Files** | 10 (5 `.h` + 5 `.cpp`) |
| **Status** | done |
| **Prior reviews** | not read |

## Summary

The Player module is the pawn hub: first-person camera/FOV, Enhanced Input, dual-weapon kit rep, modular Bandit cosmetics, sequence locomotion, AFK kick, melee, plant/defuse, and the prediction-safe CMC (sprint/ADS flags, slide, mantle). Prediction work is generally solid (compressed flags, mantle state in `FSavedMove_PF`, depenetration guard, PawnOwner-null client teardown). Cosmetic and loadout paths show deep playtest scars with good comments.

**One blocker:** `ServerRequestResetToSpawn` is an unauthenticated full heal + ammo refill + spawn teleport available to any owning client mid-match. **Several majors** around mid-fight kit swaps, unbounded kit payloads, and a heavy always-on FP weapon diagnostic. No Source edits in this pass.

## Findings

### P2-P1. `ServerRequestResetToSpawn` = free mid-match heal, full ammo, teleport
- **Severity:** blocker
- **Status:** open
- **File:** `Source/CombatForge/Player/CombatForgeCharacter.cpp:3296-3307` (RPC entry); handler in `CombatForgeGameMode::RequestResetToSpawn` (out of module, no phase/rate gate)
- **Lines (Player):**
```3296:3307:Source/CombatForge/Player/CombatForgeCharacter.cpp
void ACombatForgeCharacter::RequestResetToSpawn()
{
	// Owning-client entry (Options menu button). The server performs the authoritative teleport + heal.
	ServerRequestResetToSpawn();
}

void ACombatForgeCharacter::ServerRequestResetToSpawn_Implementation()
{
	if (ACombatForgeGameMode* GM = GetWorld() ? GetWorld()->GetAuthGameMode<ACombatForgeGameMode>() : nullptr)
	{
		GM->RequestResetToSpawn(this);
	}
}
```
- **Symptom:** Any client that owns the pawn can call the reliable Server RPC at any time (options UI or a modified client). Server fully heals, `ServerResetLoadout()`, and teleports to team spawn — while alive, during Combat.
- **Why:** Player RPC has zero validation (phase, eliminated-only, cooldown, match type). GameMode comment explicitly allows “live pawn in any phase.”
- **Fix:** Gate on phase (e.g. Lobby/Build only, or eliminated/stuck only), add a multi-second cooldown, and/or require host/admin. Never refill + heal as a free combat action.
- **Confidence:** high

### P2-P2. Mid-fight `ServerSetKit` / class cycle can equip a new gun with a full mag
- **Severity:** major
- **Status:** open
- **File:** `CombatForgeCharacter.cpp:3322-3358` (`ServerSetKit`); `:3071-3096` (hopper policy); `:3157-3164` / `:3250-3288` (`PushLocalKit` from options class cycle)
- **Symptom:** Opening options and cycling class while alive pushes a new kit. If the weapon id changes, authority writes `HopperCount = Def.MagSize` (full mag). Same-weapon re-push correctly preserves hopper (issue #68 fixed) — **weapon change still free-refills**.
- **Why:** `ServerSetKit` has no phase/elim gate; only fleet unlock clamping. Full-mag on weapon change is intentional for respawn/scroll first-draw, but the same path is used for live menu class swaps.
- **Fix:** While `Phase == Combat && RoundState == Live` (and not eliminated), reject kit weapon changes or preserve hopper/reserve across kit applies; allow full class swap only on spawn/death-screen.
- **Confidence:** high

### P2-P3. `ServerSetKit` does not clamp `CharParts` size or part indices
- **Severity:** major
- **Status:** open
- **File:** `CombatForgeCharacter.cpp:3322-3358`; `Core/CombatForgeTypes.h:153-163` (`TArray<int16> CharParts`)
- **Symptom:** Malicious client can send a huge `CharParts` array (replication bandwidth / apply cost). Part indices are not clamped server-side; `LoadPart` no-ops OOB, but oversized arrays still replicate and drive `ApplyCharacterConfig` loops.
- **Why:** Fleet gate only clamps weapon slots; clothing array is trusted from the client.
- **Fix:** `Kit.CharParts.SetNum(PFChar::SlotCount())`, clamp each index to `[-1, SlotParts(i).Num()-1]`, drop extras.
- **Confidence:** high

### P2-P4. `EnforceSingleFirstPersonWeapon` always-on full mesh scan + world `TObjectIterator` + Warning spam
- **Severity:** major
- **Status:** open
- **File:** `CombatForgeCharacter.cpp:4060-4222` (body); called from `ApplyWeaponLoadout` at `:3152-3154`
- **Symptom:** Every equip/swap/kit apply on a local human pawn: enumerates all pawn mesh components with `UE_LOG(Warning, …)` per mesh, then `TObjectIterator<UMeshComponent>` over the world (up to 24 nearby). Log flood + hitch risk on spawn waves / rapid scroll-swap.
- **Why:** Diagnostic left from the “two crossed guns” hunt; no `pf.*` cvar / `#if !UE_BUILD_SHIPPING` gate on the hot path.
- **Fix:** Keep the cheap hide of `WeaponMeshComp`/`BackWeaponMeshComp` for owner; put FPSCAN logs + world iterator behind `pf.FPWeaponScan 1` or shipping-off.
- **Confidence:** high

### P2-P5. Melee fallback overlap can tag without a clear line / facing check
- **Severity:** minor
- **Status:** open
- **File:** `CombatForgeCharacter.cpp:2059-2162` (`ServerMelee_Implementation`)
- **Symptom:** If the pawn-channel sphere sweep misses, a second `OverlapMultiByObjectType` sphere (`MeleeRange * 0.55` around a point half-range forward) picks the nearest enemy within `MeleeRange + MeleeRadius` with **no LOS and no “in front of camera” cone**. Tags through thin cover or slight off-angle positions are possible.
- **Why:** Fallback was added for capsule response quirks; authority still re-checks team/phase/cooldown/elim, but not geometry.
- **Fix:** Require a blocking LOS trace eye→victim chest, and/or `Dot(Aim, ToVictim) > 0.5` on the fallback path.
- **Confidence:** med

### P2-P6. Reload ADS suppress is owner-only; server trusts move-stream flag
- **Severity:** minor
- **Status:** open
- **File:** `CombatForgeCharacter.cpp:2331-2351` (`IsADS`); `:2364-2377` (`UpdateMovementIntents`)
- **Symptom:** Honest clients clear ADS speed/spread while reloading (`IsLocallyControlled() && bReloading → false`). A modified client can keep `FLAG_Custom_1` set during reload and retain ADS movement mult / spread on the server (server `IsADS()` only reads `WantsToADS()` for non-local).
- **Why:** Reload gate intentionally not dual-simulated on the CMC path; depends on client honesty for the compressed flag.
- **Fix:** On authority, `SetWantsToADS(false)` (or force flag off) while `WeaponComponent->bReloading`.
- **Confidence:** med

### P2-P7. `GetInteractPromptText` only covers doors
- **Severity:** minor
- **Status:** open
- **File:** `CombatForgeCharacter.cpp:1625-1664` vs interact handling `:1666-1766`
- **Symptom:** HUD prompt never shows “F to refill”, “F to pick up bomb”, or “Hold F to defuse” even when `OnInteractPressed` would act. Only door open/close text is returned.
- **Why:** Prompt helper was only wired for doors; interact stack grew (barrel, pickup, defuse) without parallel prompt branches.
- **Fix:** Mirror the priority order of `OnInteractPressed` (defuse → bomb pickup → door → barrel) with short strings.
- **Confidence:** high

### P2-P8. Slide sim timers still not in `FSavedMove_PF` (mantle is)
- **Severity:** minor
- **Status:** open (documented v1 tolerance)
- **File:** `PFCharacterMovementComponent.h:132-149`, `:141-145` comments; slide state advanced in `PhysSlide` / `OnMovementUpdated`
- **Symptom:** Mid-slide server corrections replay with current `SlideElapsed` / glide flags rather than the move’s original timers → possible short friction/end-condition desync under packet loss.
- **Why:** Explicit design trade-off; mantle was fixed into saved moves (`SavedMantle*`) after a correction storm, slide left as “within v1 tolerance.”
- **Fix:** Mirror mantle: save `SlideElapsed`, `SlideRampStartElapsed`, `bSlideGlideActive` (and optionally cooldown) in `FSavedMove_PF`.
- **Confidence:** med

### P2-P9. `EndPlay` clears FP session poses but not TP session map
- **Severity:** nit
- **Status:** open
- **File:** `CombatForgeCharacter.cpp:886-901`
- **Symptom:** `SessionWeaponPoses.Empty()` only; `SessionWeaponTPs` survives until GC of the pawn (usually destroyed). Long PIE reuses / edge lifetimes can keep stale TP tunes if the actor is recycled oddly.
- **Fix:** Also `SessionWeaponTPs.Empty()`; clear `FPArmsReturnTimer` / `DeathHideTimer` for symmetry.
- **Confidence:** med

### P2-P10. Header/docs drift on ADS times and fire shake
- **Severity:** nit
- **Status:** open
- **File:** `CombatForgeCharacter.h:59` (“0.18 in / 0.14 out”) vs defaults `:628-629` (`ADSInTime = ADSOutTime = 0.14f`); `PFCameraShakes.h:24` (0.3° / 0.15°) vs `PFCameraShakes.cpp:59-65` (0.42 / 0.2)
- **Symptom:** Readers trust comments over live UPROPERTY defaults.
- **Fix:** Align comments with defaults (or vice versa).
- **Confidence:** high

### P2-P11. Preview actor header claims `pf.ArmedAnims` defaults 0
- **Severity:** nit
- **Status:** open
- **File:** `PFCharacterPreviewActor.h:71-75`; actual default `CombatForgeCharacter.cpp:69-71` (`pf.ArmedAnims` default **1**)
- **Symptom:** Stale comment; `PickIdleAnim()` correctly reads the live CVar (armed idle when on).
- **Fix:** Update the header note so it matches default ON + stretch risk.
- **Confidence:** high

### P2-P12. `ServerSetPlayerName` is length-only; no rate limit
- **Severity:** nit
- **Status:** open
- **File:** `CombatForgeCharacter.cpp:3310-3319`
- **Symptom:** Owning client can spam name changes (scoreboard flicker). Sanitization is `Trim` + `Left(24)` only.
- **Fix:** Accept once per possess / rate-limit; optional printable-character filter.
- **Confidence:** med

---

## Subsystems audited clean

| Subsystem | Verdict |
|-----------|---------|
| **CMC speed single-source** (`GetMaxSpeed` only) | Clean — sprint/ADS/crouch/weapon mult stack correctly; ADS+sprint mutually guarded |
| **Compressed flags** (sprint / ADS / mantle) | Clean — `FSavedMove_PF` combine + Prep/SetMove |
| **Mantle prediction** | Clean — state in saved moves; stale-target bail; standing-capsule clearance; `OnTeleported` abort |
| **Depenetration / bot roof-launch** | Clean — Z zeroed vs pawns; step magnitude clamp; lowered MaxDepenetration |
| **ADS α on all roles** | Clean for honest clients — server spread uses `GetADSAlpha()`; local FOV compose single point |
| **Sprint-out fire buffer** | Clean — tap during raise window fires one shot (`bSprintOutTapBuffered`) |
| **Bot weapon roll** | Clean — weighted low-rank/low-HitValue pool; no sniper/LMG; once per kit |
| **Ammo authority on equip** | Clean for same-weapon re-push; client does not write hopper counts |
| **Weapon swap stash** | Clean — hoppers preserved across primary↔secondary |
| **AFK kick fingerprint** | Clean design — location + aim + gameplay counters; skips bots/host/elim/Vote/Results |
| **Listen-host melee cooldown** | Clean — separate client/server timestamps |
| **Owner TP gun hide vs muzzle** | Clean — `SetVisibility` not `bHiddenInGame` so auth muzzle stays on barrel |
| **FP arms seating** | Clean — two-anchor similarity; shipping-gated verify harness |
| **PFChar registry** | Clean — lazy enumerate, weapon-cosmetic strip, legacy PaintForge migrate, pants→hide legs |
| **Preview RT aspect** | Clean — 600×800 matches 0.75 UI; modular skin mirrors pawn |
| **Camera shakes** | Clean — asset-free wave patterns; land single-instance; fire additive |

---

## File checklist

| File | Reviewed | Notes |
|------|----------|-------|
| `CombatForgeCharacter.h` | yes | ~700 lines; kit/melee/AFK/art surface |
| `CombatForgeCharacter.cpp` | yes | ~5610 lines; lifecycle, input, RPCs, loadout, cosmetics |
| `PFCharacterMovementComponent.h` | yes | slide/mantle/saved-move contract |
| `PFCharacterMovementComponent.cpp` | yes | PhysSlide/PhysMantle/flags/depenetration |
| `PFCharacterCustomization.h` | yes | `FPFCharacterConfig`, `PFChar` API |
| `PFCharacterCustomization.cpp` | yes | enumerate/save/load/migrate |
| `PFCharacterPreviewActor.h` | yes | studio capture actor |
| `PFCharacterPreviewActor.cpp` | yes | config/weapon/idle/RT |
| `PFCameraShakes.h` | yes | three shake types |
| `PFCameraShakes.cpp` | yes | wave pattern constructors |

---

## Suggested fix order

1. **P2-P1** — gate or remove free reset-to-spawn in live combat (blocker).
2. **P2-P2 / P2-P3** — phase-gate kit weapon changes; clamp `CharParts`.
3. **P2-P4** — strip or cvar-gate FPSCAN hot path.
4. **P2-P5–P2-P8** — melee LOS, server reload-ADS, prompts, optional slide save.
5. **P2-P9–P2-P12** — cleanup/docs/nits when touching those areas.
