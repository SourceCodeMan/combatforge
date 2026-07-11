# Breachworks (working title) — First-Playable Art-Integration Playbook

> Generated 2026-07-10 by a code-grounded research pass (workflow wg6ww12fv). Companion to [`roadmap-post-graybox.md`](roadmap-post-graybox.md) milestone **M1**.
>
> Each section was written after **reading the actual code seams** — the `file:line` refs are real (verify against current code before executing; line numbers drift as code changes).


## Overview

## Breachworks First-Playable Art Pass — Playbook

This is a **skin, not a rewrite**. The graybox compiles today as engine `BasicShape` primitives + runtime MIDs + native C++ UMG/Input, with `L_Graybox` the only editor asset. Five areas turn that into something that "looks like a game": **build pieces + arena shell** (Megascans triplanar concrete), **characters** (MetaHuman body, 2 teams), **weapons** (Lyra/Starter-Pack mesh + anim harvest), **themed map shell** (warehouse reskin of `APFArenaShell`), and **VFX + audio** (Niagara impacts/muzzle + sound one-shots).

Two facts shape the entire sequence:

1. **Everything hangs off the character skeleton.** Weapons attach to a hand socket and FP arms that don't exist yet — the pawn's visible body is still two `SetOwnerNoSee` static-mesh cubes (`PaintForgeCharacter.h:102-103`) and `ACharacter::GetMesh()` is force-hidden and asset-less. So **CHARACTERS is the critical-path prerequisite for WEAPONS**, and neither can start until MetaHuman + an IK Retargeter exist.

2. **Realistic PBR fights whole-surface team tint** — the project's stated constraint. Today team identity is a flat `Color` vector param tinted across the whole mesh/piece (`PFBuildGrid.cpp:163`, `PFArenaShell` `ApplyTint`, character `SetTeamColor`). Every area must **move team identity to an emissive trim/overlay** (glowing spawn stripe, uniform band, recolored paint burst) and leave the base material team-neutral. Keeping the existing param **name** (`Color`) or renaming *all* call sites is mandatory, or `SetVectorParameterValue` silently no-ops and team color vanishes.

The two lowest-risk, highest-visual-payoff areas (**arena shell concrete** and **VFX/audio**) require zero character work and touch no replication, so they front-load the schedule while MetaHuman/anim retargeting — the genuinely hard, perf-sensitive work — proceeds in parallel.


## Vertical slice — smallest path to a "looks like a real game" moment

## Vertical Slice — smallest path to the first "looks like a real game" match

Goal: **one match where the arena has real concrete + a themed shell, a MetaHuman stands on each team distinguished by an emissive trim, one Lyra weapon is visible in-hand, and shots throw a recolored paint burst with a bang.** Do exactly this, in this order — each step is the cheapest unblock for the next.

1. **Master concrete material + emissive team accent** (`M_PF_BuildPiece`). Author one triplanar (`WorldAlignedTexture`) master over a Megascans concrete surface, exposing a vector param **still named `Color`** for the team accent. This is the first real content asset and unblocks both build pieces and the shell. *Why first: pure material work, no code, no replication, no character.*

2. **Repoint the two `FObjectFinder` loads** in `PFArenaShell.cpp:49-50` (and later `PFBuildGrid.cpp:97`) to the new master; keep `ApplyTint`'s `SetVectorParameterValue("Color")` writing the **accent** param. Convert the `SpawnStrip` cosmetic parts (already `EPFShellCollision::Cosmetic`) to emissive team stripes. *Now the arena reads as a real space with team-colored spawns — and nothing about collision, transforms, or grid math changed.*

3. **One assembled MetaHuman body on `GetMesh()`**, single mesh + two MIDs differing only in an emissive `TeamTrim` param. Retire the two cubes; set the ctor `-90 Z / -90 yaw` on `GetMesh()`; retarget **one** Game Animation Sample idle/locomotion via IK Retargeter and bake it. Rework `SetTeamColor(uint8)` (signature unchanged) to write `TeamTrim`. Keep `SetOwnerNoSee(true)`. *Why here: this is the critical-path unblock for weapons and the single biggest "it's a game" moment. Do ONE anim, LOD-forced, correctives off — perf tuning comes later.*

4. **One Lyra rifle mesh on the TP hand socket** (`SetOwnerNoSee`, owner sees nothing yet — accept weapon-only-TP for the slice). No montages yet; a static weapon in-hand already sells it. *Why after character: the socket literally doesn't exist until step 3.*

5. **One recolored Niagara impact + one fire sound.** Add `"Niagara"` to `PaintForge.Build.cs` deps; load the Examples Pack impact via a soft ref; recolor to `PFColors::ForTeam` with `bAutoActivate=false` → set `User.Color` → `Activate`, fired on the `Slot == INDEX_NONE` reconcile branch (`PFSplatSubsystem.cpp:110`) to avoid double-burst. Fill `PFCombatAudio::PlayMuzzle()` with one `SpawnSoundAtLocation`. *Why last: it dresses the already-working fire pipeline and needs no character/weapon dependency, but landing it here completes the sensory loop.*

**Result after ~3-4 focused days:** concrete arena, two visually-distinct teams, a weapon in hand, colored splats, and a fire bang — a recognizable match. Everything deferred (FP arms, montages, reload anims, warehouse decorators, decal splats, full perf pass) is additive from here.


## Recommended sequencing

## Full recommended sequence (with dependencies)

**Phase 0 — Foundations (do first, unblocks everything).**
- Acquire Fab packs into project content: Megascans concrete surface → `/Game/Megascans/Surfaces`; Niagara Examples Pack; 50 Free Game Sounds + UI SFX Free; Game Animation Sample; Animation Starter Pack; Lyra (mesh+anim harvest only). Enable the **Niagara** and **MetaHuman** plugins in `PaintForge.uproject`.
- Add `"Niagara"` to `PublicDependencyModuleNames` (`PaintForge.Build.cs:17`) and delete the stale "Niagara (post-v1)" comment at `:26`. (Audio needs nothing new — `UGameplayStatics` is in Engine.)
- Author the shared **triplanar concrete master** `M_PF_BuildPiece` (+ optional `M_PF_Shell`) with a vector param named `Color` (accent) so existing tint call sites keep working. This is the first real content asset besides `L_Graybox`.

**Phase 1 — Arena Shell + Build Pieces (parallel-safe, no character dependency).** *~2-3 days shell, ~1 day pieces.*
- Repoint `FObjectFinder`s: `PFArenaShell.cpp:49-50` and `PFBuildGrid.cpp:97`.
- Keep the 14-ISMC team split (`ISMCIndexFor`) — do **not** collapse to 7 + `PerInstanceCustomData` for first-playable.
- Convert `SpawnStrips`/`MidlineStripe` (already `EPFShellCollision::Cosmetic`) to emissive team trim; leave walls/floor team-neutral concrete.
- Add cosmetic warehouse decorators (ceiling, trusses, dock doors) at **Z ≥ 1400-1600uu** (above `HeightCapUU=1200`), all `EPFShellCollision::Cosmetic` (NoCollision) so they never become BuildTrace snap targets or blockers. Enable selective `CastShadow` on big form pieces.
- Depends on: Phase 0 master material; shell actor spawned at world origin `(0,0,0)` for triplanar to tile.

**Phase 2 — VFX + Audio (parallel-safe, no character dependency).** *~3-4 days.*
- Upgrade splat sphere → deferred **decal** (fixes the `PFSplatSubsystem.cpp:319` fade contract-gap via `SetFadeOut`). Recolor impact/paint burst to team color via soft-ref Niagara (`bAutoActivate=false` → `SetVariableLinearColor("User.Color", …)` → `Activate`), fired on the `Slot == INDEX_NONE` branch (`:110`) to avoid owner double-burst. Keep muzzle flash **neutral** (white/orange), not team-tinted.
- Fill `PFCombatAudio`'s six no-op methods with `SpawnSoundAtLocation` (3D) / `SpawnSound2D` (UI), add one `USoundAttenuation` + one `USoundConcurrency` (cap voices for auto-fire). Keep the component **non-replicated** — every call site is already client-correct.
- Depends on: Phase 0 Niagara module/plugin + imported sounds. **Independent of character** — footsteps are out of scope here (they belong on locomotion AnimNotifies from Phase 3).

**Phase 3 — Characters (CRITICAL PATH — gates all weapon work).** *~2 days.*
- Assemble one MetaHuman; take **only** the body SK + skeleton + a body Anim BP onto the existing `GetMesh()`. Copy `ik_foot_root`/`ik_foot_l/r`/`ik_hand` virtual bones in from `SKM_Manny_Simple` (MetaHuman skeleton ships without them) **before** building the IK Rig.
- Build a **5.6-specific IK Retargeter** (source = UE Mannequin, target = MetaHuman); bake retargeted Game Animation Sample locomotion to new sequences rather than retargeting live.
- Rework three seams: mesh setup (retire cubes, set ctor `-90 Z/-90 yaw` on `GetMesh()`, `SetOwnerNoSee(true)`); `SetTeamColor(uint8)` → write emissive `TeamTrim` param (signature unchanged); `SetEliminatedAppearance` → `GetMesh()->SetHiddenInGame` (or death anim later).
- Perf tuning + 4-player listen-server verify: disable Body+Neck correctives, hair cards/none, force lower LODs, consider Skeletal Mesh Merge.
- Depends on: MetaHuman plugin + assembled asset; imported anim packs; existing `PFColors::ForTeam`.

**Phase 4 — Weapons (LARGE; hard-depends on Phase 3).** *~3.5-4 days after skeleton exists.*
- Harvest **flat assets only** from Lyra — rifle/pistol meshes + TP anim clips. **Do not** adopt `B_WeaponInstance_Base`, linked anim layers, or GAS — they collide with the server-authoritative `ServerFire` pipeline.
- Net-new C++ in `PFWeaponComponent`: add TP weapon mesh (`SetOwnerNoSee`) + FP arms + FP weapon mesh (`SetOnlyOwnerSee`) — budget **two** weapon mesh components. Route only the cosmetic tracer/flash through a Muzzle socket; keep `Packet.Origin` fed by the existing math `GetMuzzleLocation(false)` (anti-cheat validates within 150uu — do not desync it).
- Montages: trigger local FP montage on `IsLocallyControlled()` **regardless of authority** (the `!HasAuthority` cosmetic gate would skip it on a listen host). For TP reload visibility, add a `NetMulticast` reload-FX RPC — `bReloading` is `COND_OwnerOnly` (`PFWeaponComponent.h:55`) so remote viewers never see peer reloads today.
- Keep weapon material team-neutral (team = emissive uniform, not weapon tint). Reload is a hopper "pod flip," not a mag swap — Lyra mag-change clips look wrong; source/author a hopper-flip or keep reload abstract. Non-lethal framing: hopper + orange muzzle tip, recolor away from mil-black.
- Depends on: Phase 3 skeleton + IK Retargeter; TP-reload replication decision.

**Ordering rationale:** Phases 1 and 2 run immediately in parallel (no character dependency, no replication risk, high visual payoff) while Phase 3 — the slow, perf-critical retarget work — proceeds. Phase 4 is strictly last because the hand socket and FP arms it needs are created only in Phase 3.


---

# Per-area integration


## Build-piece + Shell Art Pass: gray primitive -> concrete wall with team-colored emissive trim

**Key finding first:** the code does NOT use per-instance custom data. Team is encoded by which of the 14 ISMCs an instance lives in (`APFBuildGrid::ISMCIndexFor`, PFBuildGrid.cpp:177), and each ISMC already owns a per-team MID whose `Color` param is set in `BeginPlay` (PFBuildGrid.cpp:154-168). Coloring by team is already solved at the component level - the art pass just needs a better master material behind those MIDs. Do not introduce PerInstanceCustomData unless you later collapse to 7 ISMCs (Step 6).

### Step 1 - Acquire the concrete surface (in-editor, no code)
In Fab/Quixel Bridge, download one tiling concrete Megascans **Surface** (e.g. rough cast concrete) at 1K-2K into `/Game/Megascans/Surfaces/`. You need its textures (BaseColor, Normal, packed ORD/roughness); you will NOT use the auto-generated MI directly - you feed its textures into your own master.

### Step 2 - Author master material `M_PF_BuildPiece` (in-editor)
Create `/Game/Materials/M_PF_BuildPiece` (first real content asset besides `L_Graybox`). Graph:
1. **Triplanar base:** `WorldAlignedTexture` for BaseColor and (separately) Normal, driven by a `WorldAlignedTextureScale` scalar param (world tiles, e.g. 1 tile / 256 uu). Tiling becomes independent of each instance's non-uniform scale and adjacent pieces line up. A wall scaled `(2.0, 0.05, 3.0)` then shows correctly-scaled concrete on every face.
2. **Roughness/AO:** sample the packed ORD map through the same world projection (or a cheaper single-axis sample) into Roughness + AO.
3. **Team accent (the trim):** add a `TeamAccent` **vector param** (default black). Build a scale-invariant mask = max(top-band, rim): top-band = smoothstep on distance from the object's bounds-top in WORLD Z (constant uu thickness); rim = `Fresnel` (power ~4). `Emissive = TeamAccent * AccentBoost * mask`, `AccentBoost` a scalar param (~5-15). Base albedo stays neutral concrete - team identity lives ONLY in emissive (satisfies the realistic-material readability constraint).
4. Expose the accent param **named `Color`** so the existing `SetVectorParameterValue(TEXT("Color"), ...)` keeps working with zero call-site churn, OR rename it and update both call sites (PFBuildGrid.cpp:163, PFArenaShell.cpp:253). Pick one, be consistent.

### Step 3 - Repoint the build grid's material load (code)
In `APFBuildGrid::APFBuildGrid()` change the finder at PFBuildGrid.cpp:97 from `/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial` to `/Game/Materials/M_PF_BuildPiece.M_PF_BuildPiece`; keep assigning to `ShapeMaterial` (:98) and keep the per-ISMC `SetMaterial(0, ShapeMaterial)` (:123). No other ctor change - the 14-component split, meshes, and collision stay as-is.

### Step 4 - Drive the accent per team (code, minimal)
`BeginPlay` (PFBuildGrid.cpp:154-168) already creates one MID per ISMC and sets the team color. Keep the loop; it now sets your accent param instead of a full-surface tint. If you added extra params (`WorldAlignedTextureScale`, `AccentBoost`) set them here via `MID->SetScalarParameterValue`. Because the accent is emissive-only, `PFColors::ForTeam(Team)` (PaintForgeTypes.cpp:47) stays the single source of truth shared with HUD/paintballs.

### Step 5 - Same for the arena shell (code + material)
`APFArenaShell` uses plain `UStaticMeshComponent`s. `MakeShapePart` already tags parts with `EPFShellCollision` (PFArenaShell.cpp:135-152) - a clean material seam:
- **Structure** (`SolidBuildable` field floor, `Solid` perimeter walls + pen): repoint the ctor finder (PFArenaShell.cpp:50) to a concrete master (`M_PF_BuildPiece`, or a shell-specific `M_PF_Shell` with a larger world tile for the 6400x4000 floor slab). Triplanar matters most here - the floor slab is scaled `(64, 40, 0.3)` and would smear any UV-tiled texture.
- **Markers** (`Cosmetic` spawn strips + midline stripe/posts, tinted in `ApplyTint`, PFArenaShell.cpp:161-171/246-256): keep these on a simple emissive team/gray material so `SpawnStripA/B` stay strongly team-colored and the midline reads gray. `ApplyTint` already sets `Color`; point it at an emissive-heavy master or reuse `M_PF_BuildPiece` with a high `AccentBoost`.

### Step 6 (OPTIONAL, later) - Collapse 14 -> 7 ISMCs to halve draw calls
Only if profiling shows the extra components hurt. Then: one ISMC per piece type, `NumCustomDataFloats = 1` in the ctor, and in `AddPieceLocal` (PFBuildGrid.cpp:555-566) call `ISMC->SetCustomDataValue(InstanceIdx, 0, (float)Rec.Team, /*bMarkRenderStateDirty=*/true)` right after `AddInstance`. In the master read team via `PerInstanceCustomData` -> `VertexInterpolator` (vertex-only) -> `lerp(TeamAAccent, TeamBAccent, teamFloat)`. This touches `ISMCIndexFor`, the `[14]` arrays, and the swap-remove bookkeeping (:568-597) - a real refactor, not first-playable work.

### Verify
Run the listen server + one client. Confirm: (1) placed walls show tiled concrete with no stretching on thin/tall pieces, (2) adjacent walls line up, (3) the emissive trim reads clearly as team A vs B from across the field AND on the CLIENT (proves the AddPieceLocal path colors on both), (4) the field floor slab is not smeared, (5) spawn strips and midline still read team/gray. Watch GPU cost with `stat GPU` given the triplanar sample count.


## Weapons Art Pass — Lyra Rifle/Pistol Harvest → PFWeaponComponent (UE 5.6)

**Reality check first.** `UPFWeaponComponent` today has **zero** visual surface: no weapon mesh, no skeleton, no animation. "Fire feel" is a camera shake (`PFWeaponComponent.cpp:247`) plus `PlayMuzzle()` audio (`cpp:251`). The character is static-cube graybox (`PaintForgeCharacter.h:102-103`; ctor `cpp:62-97`) with the inherited skeletal `GetMesh()` explicitly hidden (`cpp:95`). This is an **additive build**, and it **blocks on the CHARACTER pass** delivering a real skeletal body + hand socket first.

**Harvest honesty.** Take Lyra's rifle/pistol **meshes** and **raw animation sequences** as flat assets. Do **not** adopt `B_WeaponInstance_Base`, linked anim layers, `ULyraAnimInstance`, GAS abilities, or GameFeature plugins — they're entangled with GAS and would collide with the existing server-authoritative fire pipeline. Roughly 40% of Lyra's weapon value (meshes + TP clips) migrates cleanly; the FP anims and the whole runtime framework do not.

### Ordered plan

1. **Migrate flat assets only.** Right-click Lyra `SK_Rifle` / pistol skeletal meshes + their fire/ADS/idle anim sequences → **Asset Actions ▸ Migrate**. Uncheck AnimNotify sound deps and any `B_*`/`GA_*`/`ABP_*` that get pulled in. Skip the weapon Blueprints and ability assets entirely.

2. **Pick FP vs TP anim sources honestly.** Lyra's FP anims are linked-anim-layers keyed to weapon tags — not standalone clips. Use **Animation Starter Pack** rifle (ADS/fire/reload) for **first-person**, and **Game Animation Sample** for **third-person** locomotion. Note the FP source is UE4-skeleton and will need retarget.

3. **Retarget.** Build IK Rigs for source (Lyra UE5 / Starter-Pack UE4) and your target skeleton, then an **IK Retargeter** mapping spine/arm/leg/root chains. Fix the retarget pose before baking — UE4 (3 spine) vs UE5 (5 spine) mismatch causes stuck feet / rotated limbs otherwise.

4. **Character mesh scaffolding (C++, `PaintForgeCharacter`).** Convert the inherited `GetMesh()` into the **TP skeletal body** (`SetOwnerNoSee(true)`), and add a new **FP arms** `USkeletalMeshComponent` attached to `FirstPersonCamera` (`SetOnlyOwnerSee(true)`). Retire/keep the static `BodyMesh`/`HeadMesh` cubes as the fallback graybox behind a flag.

5. **Weapon mesh + sockets.** Add a weapon skeletal-mesh component attached to a `hand_r` socket. FPS-correct setup needs **two** (FP-arms hand `SetOnlyOwnerSee`, TP-body hand `SetOwnerNoSee`) since one mesh can't be both; for first-playable you may ship weapon-on-TP-only. Add a cosmetic **`Muzzle`** socket for flash/tracer origin.

6. **Wire montages into the fire pipeline.**
   - **FP fire montage:** trigger on `IsLocallyControlled()` inside `FireOneShot` — but **not** inside the `!HasAuthority` cosmetic block (`cpp:223`), or a listen host sees no FP anim.
   - **TP fire montage:** trigger in `MulticastShotFX_Implementation` (`cpp:403`), alongside the existing cosmetic spawn + `PlayMuzzle` (`cpp:441-449`).
   - **Reload montage:** trigger from `BeginReload` (`cpp:500`). Remote viewers can't see it today (`bReloading` is `COND_OwnerOnly`, `OnRep_Reload` owner-only) — add a `NetMulticast` reload-FX RPC mirroring `MulticastShotFX` for TP reloads.

7. **Muzzle rule — do not touch netcode.** Route only the cosmetic flash/tracer through the `Muzzle` socket (`GetMuzzleLocation(true)` branch, `cpp:557-561`). Leave `GetMuzzleLocation(false)` math untouched (`cpp:563-570`) — `ServerFire` validates client origin ≤150uu against it (`cpp:344-351`) and the spread stream is deterministic (B3). A socket-derived origin risks desync + false anti-cheat rejects.

8. **Team readability + non-lethal framing.** Keep the weapon material **team-neutral** — move team identity to an emissive uniform/trim overlay, not `SetTeamColor`'s whole-body MID tint (`h:49`). Frame the gun as an airsoft/paintball **marker**: hopper + orange tip (the code's `HopperCapacity 100` / "pod flip" reload, `cpp:547`, is a marker, not a mag). Lyra's mag-swap reload clip will read wrong against a hopper flip — source/author a hopper clip or keep reload feel abstract.

**Effort:** ~3.5-4 focused days *after* the character skeleton lands (migrate+retarget ~1.5d, FP/TP/weapon C++ scaffolding ~1d, montage wiring + TP-reload multicast ~1d, team overlay + marker framing ~0.5d).

Sources: [Adapting Lyra animation to your UE5 game](https://www.unrealengine.com/en-US/tech-blog/adapting-lyra-animation-to-your-ue5-game), [Lyra weapon system (X157 dev notes)](https://x157.github.io/UE5/LyraStarterGame/Weapons/), [Lyra core animation breakdown](https://www.jaydengames.com/posts/ue5-black-magic-game-core-animation/), [FP arms + TP body multiplayer skeleton hierarchy (Epic forums)](https://forums.unrealengine.com/t/how-should-skeleton-mesh-hierarchy-be-structured-for-a-multiplayer-fps-1p-arms-3p-full-body-weapons-replication/2730033), [Retargeting animations in UE5](https://baemincheon.github.io/2022/10/04/retargeting-animations-in-unreal-engine-5/).


## CHARACTERS: MetaHuman body + two teams (Breachworks art pass)

**Where this plugs in.** The pawn `APaintForgeCharacter` already reserves the right slot for a skeletal body: `ACharacter::GetMesh()` exists but is deliberately asset-less and hidden (`PaintForgeCharacter.cpp:95-98`). The visible graybox is two static-mesh cubes — `BodyMesh`/`HeadMesh` (`.h:102-103`, built `cpp:62-92`) — both `SetOwnerNoSee(true)`. So this is a swap: MetaHuman goes on `GetMesh()`, the cubes retire. The owner is first-person and never sees their own body; the MetaHuman is the third-person proxy opponents see.

### Step 1 — Get a MetaHuman body into the project (editor)
1. Enable the MetaHuman plugin (5.6 pipeline is fully in-editor now — no cloud/Bridge round-trip).
2. In MetaHuman Creator make one character; hit **Assemble**. This emits a Blueprint plus ~12 components. **Ignore the Blueprint.** Harvest only: the **body skeletal mesh** (`SKM_*`), its **skeleton**, and the generated **IK Rig / IK Retargeter**.
3. Drop the face for v1 (military mask/helmet covers it and the face rig is the main CPU cost). Body-only.

### Step 2 — Fix the skeleton for gameplay anims (editor)
The MetaHuman body skeleton is naming-compatible with Manny/Quinn but ships **without** the `ik_foot_root`, `ik_foot_l/r`, `ik_hand_*` virtual bones. Copy those in from `SKM_Manny_Simple` before building the IK Rig, or foot IK and any `ik_`-driven anim silently break.

### Step 3 — Retarget the free anims (editor)
The **Game Animation Sample** and **Animation Starter Pack / Lyra** anims are all authored on the standard UE5 Mannequin. Build an IK Retargeter: source = UE Mannequin IK Rig, target = MetaHuman IK Rig. **Bake** the locomotion + rifle fire/ADS/reload sets to new sequences on the MetaHuman skeleton (don't retarget live per-frame — extra CPU on a listen-server). Note 5.6 retarget results differ from 5.5; use a 5.6-era setup. Author a body **Anim BP** (locomotion blendspace + fire/reload slots) against the MetaHuman skeleton.

### Step 4 — Wire the mesh in C++ (`PaintForgeCharacter.cpp` ctor)
1. Delete the cube block `cpp:58-92` and the `BodyMesh`/`HeadMesh`/`BodyMID`/`HeadMID` members (`.h:102-103,110-111`); drop the `CubeFinder`/`MatFinder`/`StaticMesh` includes.
2. On `GetMesh()`: set the body SK, set the Anim BP class (`GetMesh()->SetAnimInstanceClass(...)` or a soft class ref), set `SetOwnerNoSee(true)`, and set the **relative transform** — `SetRelativeLocation(FVector(0,0,-90))` + `SetRelativeRotation(FRotator(0,-90,0))` (the ACharacter mannequin convention; the cubes hand-placed their own offsets at `cpp:73,89` so there's nothing to inherit).
3. Remove the `GetMesh()->SetVisibility(false)` defensive hide at `cpp:97`.

### Step 5 — Two teams via emissive overlay, not tint (the readability fix)
Realistic PBR fights a full-body color tint (the project's stated constraint). Rework `SetTeamColor(uint8)` (`cpp:518-539`): instead of creating cube MIDs and writing `"Color"` on the whole body, create one MID on `GetMesh()` and write an **emissive `TeamTrim`** param bound to a dedicated region (chest/back panel + helmet band) — either a second material slot or a thin overlay. Keep the signature and keep pulling the color from `PFColors::ForTeam` (`Core/PaintForgeTypes.cpp:47`), so every existing caller (pkg-weapons, elim feed, HUD) is unaffected. Cheapest reliable two teams = **one body mesh + two material instances differing only in `TeamTrim`**.

### Step 6 — Elimination + muzzle
- `SetEliminatedAppearance(bool)` (`cpp:541-553`): retarget the two `SetHiddenInGame` calls to `GetMesh()->SetHiddenInGame`, or (better) trigger a death montage/ragdoll. Collision timing stays the health component's job (unchanged).
- `GetMuzzleLocation(bool)` (`cpp:555-570`): server/proxy path is a capsule-relative offset and needs no change. Optionally later swap the *cosmetic* branch to a hand/muzzle **socket** on the MetaHuman for correct third-person muzzle FX.

### Step 7 — Perf pass (ship-blocker for multiplayer)
Stock LOD0 MetaHuman ≈ 3-6ms GPU/char; only 1-4 on screen hold 60fps, and the anim thread's **corrective bones** are the CPU bottleneck. Before calling it done on a several-player listen-server: disable **Body + Neck correctives** (~40% CPU back), use **hair cards or none** (helmet hides it; strand hair cost ~800MB VRAM at 50 chars), force **lower LODs** / tune LODSync, and evaluate the **Skeletal Mesh Merge** plugin (~5x game-thread win at 32 chars). Verify with a 4-player PIE listen-server that host and clients both see correct team trim and anims replicate.

**Effort:** ~1 day editor (assemble + retarget/bake + Anim BP + 2 material instances), ~0.5 day C++ (steps 4-6), ~0.5 day perf + multiplayer verify.


## Themed Map Shell: Warehouse Reskin of `APFArenaShell`

**Goal:** drape a themed empty-warehouse interior over the existing functional shell without touching one line of collision setup, spawn-transform math, or grid constants. `APFArenaShell` already isolates FORM from FUNCTION perfectly — every visible part funnels through `MakeShapePart` (`PFArenaShell.cpp:122`) and gets `SetMaterial(0, ShapeMaterial)` at line 129, and only three call sites tint anything (`ApplyTint`, lines 163-170). That is the entire seam.

**Three spatial constraints, and how they already map:**
- Ceiling ≥12m over play area → the play boundary is the h=1200uu (=12m) perimeter walls; the cosmetic ceiling goes ABOVE that at Z≈1500 (15m).
- Clear ~64×40m empty floor → `FieldX=6400`/`FieldY=4000` (=64×40m) already matches exactly; keep it clutter-free, all dressing goes to walls/ceiling/perimeter.
- Spawns/neutral mapped to features → SpawnStripA/B (west/east, `PFArenaShell.cpp:74-79`) become loading-dock bays; the midline (`:81-91`) becomes a painted floor hazard line + I-beam columns.

### Ordered steps

1. **Acquire surfaces (Fab / Megascans free set).** In the Epic Launcher Fab tab or the Fab window inside UE 5.6, grab from the free Megascans library: a concrete/epoxy floor, corrugated-metal or painted-concrete wall, a scuffed metal for columns/trim, and a grime/decal or two. These are licensed UE-Only Content under the UE EULA — legal for this game, not reusable outside UE. Import into `/Game/Art/Warehouse/Textures`.

2. **Build the triplanar master material `M_PFSurface`.** This is the load-bearing step. `BasicShapes/Cube` has per-face 0-1 UVs, so the shell's non-uniform scales (FieldFloor `64×40×0.3`, walls `0.2` thin, posts `0.4×0.4×12`) would smear any normal tiling material. Use `WorldAlignedTexture` + `WorldAlignedNormal` (modernized, World space) driven by a `TextureSize` scalar so texel density is real-world constant regardless of mesh scale. **Expose a `Color` vector param** (preserves `ApplyTint` @ `PFArenaShell.cpp:253`, which will otherwise silently no-op) plus an `Emissive`/`EmissiveColor` param for team trim. Anchor projection in world space — this requires the shell actor to spawn at world origin (verify in the GameMode; if it can't, switch the function to Local space).

3. **Author the child instances (MICs):** `MI_ConcreteFloor`, `MI_WarehouseWall`, `MI_Column`, `MI_SpawnTrim` (emissive, low diffuse), `MI_MidlineStripe`. Vary only params — one master graph.

4. **Wire materials in per role (code, ~1 hr, additive).** In the ctor, add `FObjectFinder` lines next to the existing ones (`PFArenaShell.cpp:49-52`) for each MIC. Then give `MakeShapePart` an optional material argument (or a small role→material switch) so line 129 assigns the right instance: FieldFloor→`MI_ConcreteFloor`, PerimeterWalls→`MI_WarehouseWall`, MidlinePosts→`MI_Column`, etc. **Do not touch** the scale, transform, or collision block (`:135-152`) — the `EPFShellCollision` modes and the SolidBuildable BuildTrace block (`:150-151`) are gameplay contracts.

5. **Keep team identity as an emissive overlay (readability constraint).** The shell already puts team color only on thin Cosmetic SpawnStrip decals (`:74-79`), never on walls — ideal. Repoint `ApplyTint` (`:163-164`) to drive the `Emissive` param of `MI_SpawnTrim` instead of a flat diffuse tint, so team zones glow as dock-bay outlines. Midline stripe/posts (`:166-170`) take `MI_MidlineStripe`/`MI_Column`. Never tint wall or floor surfaces by team.

6. **Add cosmetic decorator geometry (NoCollision only).** Reuse `EPFShellCollision::Cosmetic` (already NoCollision, `MakeShapePart` `:135-138`) — or a small separate decorator actor — to add: a ceiling plane at Z≈1500 (15m, safely above the 12m `HeightCapUU` build cap), roof trusses/purlins, wall ribs/pilasters aligned to the h=1200 perimeter, roll-up dock doors on the spawn walls, and pen dressing at the south warm-up pen (`:105-119`). Keep all of it OFF the 64×40 play floor and OUT of the buildable volume. Selectively enable `SetCastShadow(true)` on these (line 132 currently forces shadows off everywhere → flat look).

7. **Coordinate lighting with the GameMode.** Per the header comment (`PFArenaShell.h:24`), lights/fog are spawned by the GameMode (02 §3.5), not here. A warehouse wants a skylight-dim + spotlight-grid feel; make sure the new cosmetic ceiling doesn't clip the light rig and the h=1200 walls still occlude (they already block `ECC_Visibility`, `:145`).

8. **Replication sanity.** Geometry is constructor-built identically on host + client (`bReplicates` existence-only, `:40`) and MIDs are created in `BeginPlay` on both — so the material swap needs no replication. Build every new decorator component in the constructor too, and never gate visuals on `HasAuthority()`, or clients desync from the host's warehouse.

### Free path summary
No paid warehouse kit needed: **Megascans-on-primitives (triplanar) + a handful of engine/free modular cosmetic pieces** gets a convincing empty warehouse. The functional shell — grid math, BuildTrace snap, h=1200 boundary, spawn transforms — stays byte-for-byte untouched; only the paint and non-colliding dressing change.

**Sources:** [Quixel on Fab](https://quixel.com/news/quixel-on-fab-new-megascans-and-megaplants), [Megascans free-until-2024→now Fab (CG Channel)](https://www.cgchannel.com/2024/10/epic-games-has-made-megascans-free-to-all-but-only-until-the-end-of-2024/), [UE5 Triplanar Deep Dive (80.lv)](https://80.lv/articles/ue5-triplanar-deep-dive-from-worldalignedtexture-to-high-quality-normals-part-1), [WorldAlignedTexture tutorial (Epic Dev Community)](https://dev.epicgames.com/community/learning/tutorials/55ep/unreal-engine-ue5-triplanar-deep-dive-from-worldalignedtexture-to-high-quality-normals).


## First-Playable Art Pass — VFX + Audio Integration Plan

Both subsystems were written to be skinned without touching callers. The splat pool, team-color MIDs, reconcile logic, and all six audio call sites are the deliverable; this pass only fills bodies and swaps primitives for assets.

### 0. Prerequisites (do first)
1. Import **Niagara Examples Pack**, **50 Free Game Sounds**, **UI SFX Free** into project content (e.g. `/Game/FX/`, `/Game/Audio/`). Enable the **Niagara** plugin in `PaintForge.uproject`.
2. `PaintForge.Build.cs:17` — add `"Niagara"` to `PublicDependencyModuleNames`; delete the stale `// NOT needed: … Niagara` comment at `:25-26`.
3. Author two tiny assets: a **paint-splat Deferred Decal material** (Color vector param + `DecalLifetimeOpacity`) and one **USoundAttenuation** + one **USoundConcurrency** for combat 3D sound.

### 1. Asset references — soft, not CDO
Do **not** copy the `ConstructorHelpers::FObjectFinder` pattern at `PFSplatSubsystem.cpp:38-49`; that hard-refs at class-default time and is only safe for `/Engine` content. Add soft refs and resolve them lazily in `EnsureInfrastructure()` (`PFSplatSubsystem.cpp:189`), which already only runs on rendering worlds:
```cpp
// PFSplatSubsystem.h — new members
UPROPERTY() TObjectPtr<UNiagaraSystem>   ImpactFX;      // loaded lazily
UPROPERTY() TObjectPtr<UMaterialInterface> SplatDecalMat;
TSoftObjectPtr<UNiagaraSystem> ImpactFXRef =
    TSoftObjectPtr<UNiagaraSystem>(FSoftObjectPath(TEXT("/Game/FX/NS_PaintImpact.NS_PaintImpact")));
// EnsureInfrastructure(): if (!ImpactFX) ImpactFX = ImpactFXRef.LoadSynchronous();
```
(Muzzle system + sound bases follow the same soft-ref pattern; put the shared `USoundBase` refs on a `UDeveloperSettings`/GameInstance subsystem, not on `UPFCombatAudio` — that component has one instance per character.)

### 2. Splat: squashed sphere → deferred decal
Replace the disc-mesh placement in `PlaceSplat()` (`PFSplatSubsystem.cpp:265`) with a pooled `UDecalComponent`. Decals conform to walls/ramps/floors (the flat disc z-fights on angled build geometry) and, critically, `SetFadeOut(0.0f, 0.2f)` finally delivers the 0.2 s pending fade the contract wanted — closing the **CONTRACT-GAP** logged at `PFSplatSubsystem.cpp:319-321`.
- Keep the existing pool/round-robin (`TakeNextSlot`, `SplatComps`→`DecalComps`), the normal-alignment math (`FRotationMatrix::MakeFromZ`), and the jitter.
- Per-team color: create the decal MID from `SplatDecalMat`, `SetVectorParameterValue("Color", PFColors::ForTeam(Team))`, pending = `* 0.6f` (reuse `PendingBrightness`).
- In `TickPendingExpiry()` (`:305`), swap `SetVisibility(false)` for `Decal->SetFadeOut(0.f, 0.2f)`; the confirmed reconcile path stays byte-for-byte the same.

### 3. Impact burst — recolored Niagara on the right branch
Spawn the recolored Examples-Pack impact system in both spawn methods, but respect the reconcile model so the owning client never double-bursts:
```cpp
UNiagaraComponent* Burst = UNiagaraFunctionLibrary::SpawnSystemAtLocation(
    World, ImpactFX, Loc, SafeNormal.Rotation(), FVector(1),
    /*bAutoDestroy=*/true, /*bAutoActivate=*/false);        // <-- false: latch params first
if (Burst) { Burst->SetVariableLinearColor(FName("User.Color"), PFColors::ForTeam(Team)); Burst->Activate(true); }
```
- `SpawnPendingSplat` (`:117`): fire the burst always (owning-client immediate feel).
- `SpawnConfirmedSplat` (`:89`): fire the burst **only inside the `Slot == INDEX_NONE` branch** (`:110`) — i.e. remote shots. A confirm that consumes a pending must NOT burst again.
- Niagara latches `User.*` at activation; if you let `SpawnSystemAtLocation` auto-activate, the recolor is lost. Verify the real param name in the pack (often `Color`/`User.Color`).

### 4. Muzzle VFX — neutral, socket-attached
At the `PlayMuzzle()` sites (`PFWeaponComponent.cpp:251` local, `:447` remote cosmetic), spawn the muzzle system with `UNiagaraFunctionLibrary::SpawnSystemAttached` to the character muzzle socket (`Char->GetMuzzleLocation`). **Keep it neutral (white/orange)** — muzzle isn't team-identifying, and tinting it fights the realistic-material team-readability constraint (team identity lives in the paint splat + emissive uniform trim, not the flash). Both local and remote are covered because both paths already call `PlayMuzzle`.

### 5. Audio — fill the six bodies (no caller changes, stay non-replicated)
Every call site already runs on the correct client, so just spawn locally. Keep `SetIsReplicatedByDefault(false)` (`PFCombatAudio.cpp:10`).

| Method | Impl (UE 5.6) | Notes |
|---|---|---|
| `PlayMuzzle` (`:26`) | `SpawnSoundAtLocation` at owner muzzle, +Attenuation +**Concurrency** | auto-fire → cap voices (max ~4-8, StopFarthestOnMax) or it floods |
| `PlaySplatIncoming` (`:31`) | `SpawnSoundAtLocation` at owner (or 2D) | owning client only, already gated at `PFHealthComponent.cpp:178` |
| `PlayHitmarker` (`:16`) | `SpawnSound2D` | UI, non-positional |
| `PlayElim` (`:21`) | `SpawnSound2D` | UI |
| `PlayBreakout` (`:36`) | `SpawnSound2D` horn | round-start |
| `PlayDenied` (`:41`) | `SpawnSound2D` (UI SFX Free) | build denial |

Load the `USoundBase` refs once (shared settings object), not per component instance. On dedicated servers these are safe no-ops, but avoid loading the assets there.

### 6. Out of scope for these files
- **Footsteps**: there is no `PlayFootstep` hook in `UPFCombatAudio`. Wire them as `AnimNotify_PlaySound` on the Game Animation Sample locomotion montages — a separate anim task, not this component.
- **Server authority**: unchanged. Confirmed splats/bursts ride the existing multicast (`MulticastImpactSplat`→`SpawnConfirmedSplat`), pendings ride the owning-client cosmetic impact (`HandleCosmeticImpact`). The fake-and-verify model is already correct; you're only skinning it.


---

## Cross-cutting risks

## Cross-cutting risks

**1. Multiplayer visual replication (listen-server host/client parity).**
- All visual code must run identically on host and clients. `AddPieceLocal`/`ApplyTint`/splat spawns already do — **keep it that way**: create MIDs in `BeginPlay`, never in constructors; never gate a visual on `HasAuthority()`. New warehouse decorators must be constructor-built and existence-replicated only.
- **Listen-host FP-anim trap:** the owner cosmetic block in `FireOneShot` is gated by `!HasAuthority`, so on a listen host the local player's cosmetic block is skipped — a naive "play FP montage in the cosmetic block" means the **host sees no first-person fire anim**. Trigger local FP montages on `IsLocallyControlled()` regardless of authority.
- **TP reload is invisible to peers:** `bReloading` is `COND_OwnerOnly` (`PFWeaponComponent.h:55`), so remote viewers never learn a peer is reloading. A TP reload montage needs a `NetMulticast` FX RPC (mirror the existing multicast shot-FX) or a rep-condition change.
- **Splat double-burst:** the owning client fires a pending burst *and* a confirmed one; only burst on `SpawnConfirmedSplat` when `Slot == INDEX_NONE` (`PFSplatSubsystem.cpp:110`, the remote-shooter case) or every own shot bursts twice.
- **Anti-cheat desync:** `ServerFire` validates client origin within 150uu of the *math* muzzle. Feeding a weapon socket transform into `Packet.Origin` or the authoritative spawn risks false rejects — route sockets to cosmetics only.

**2. Perf with MetaHuman + Megascans across several players.** This is the ship-blocker, not plumbing. Stock LOD0 MetaHuman is ~3-6ms GPU/char; the dominant CPU cost is the anim thread evaluating corrective bones. For a 4-player listen host on a gaming PC: disable Body+Neck correctives (~40% recovery), use hair cards or none (a helmet hides hair anyway; strand hair ate ~800MB VRAM at scale), force lower LODs, and consider the Skeletal Mesh Merge plugin. On the material side, triplanar samples the texture 3× per map — pack Roughness/AO/Height into one ORDH texture, cap at 1-2K, and prefer a 2-plane/dominant-axis projection since all pieces are axis-aligned boxes. Cap auto-fire audio voices with a `USoundConcurrency` (StopFarthestOnMax) or shots stack dozens of voices. **Verify at 4 players before calling any area done.**

**3. Retarget skeleton mismatches.** Game Animation Sample and Lyra clips are UE5 skeleton (5 spine bones); Animation Starter Pack is UE4 (3 spine) — mixing them without a corrected retarget pose gives stuck feet / rotated limbs. The MetaHuman body skeleton is naming-compatible with Manny **but ships without `ik_*` virtual bones** — copy them from `SKM_Manny_Simple` before building the IK Rig or foot IK and any `ik_`-driving anim break. Use a 5.6-specific IK Retargeter (behavior changed vs 5.5) and **bake** to new sequences rather than retargeting live every frame. Reload-anim semantics also mismatch: the game reloads a hopper "pod flip," not a magazine swap, so Lyra/Starter-Pack reload clips look wrong.

**4. Keeping warnings-as-errors clean.** `bWarningsAsErrors = true` is on (`PaintForge.Build.cs:10`, contract §5.17) — every code edit must compile warning-free. Watch: `FObjectFinder` needs a cooked, *referenced* `/Game` path — a bad path silently leaves `.Object` null (no warning, just missing material), so verify new assets are referenced/loaded. Do **not** reuse the CDO-time `FObjectFinder` pattern for marketplace/Niagara content (only reliable for `/Engine`); use `TSoftObjectPtr` + `LoadSynchronous` guarded so dedicated servers never touch render packages. Renaming the `Color` tint param without updating **all** call sites (`PFBuildGrid.cpp:163`, `PFArenaShell` `ApplyTint`, character `SetTeamColor`) compiles clean but silently no-ops the team color — treat it as a correctness bug the compiler won't catch.
