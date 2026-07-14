# CombatForge — Code Contract (v1 Graybox)

**Doc:** 05-code-contract.md · **Owner:** Tech lead · **Status:** LAW for the six parallel implementation packages
**Engine:** Unreal Engine 5.6 · **Module:** `CombatForge` · **Target:** Win64 · C++-first, zero editor-authored assets.

This document supersedes any conflicting statement in docs 01–04. Where this doc is silent, the
authority order is: **01 (game rules) → 03 (build system) → 04 (combat feel) → 02 (structure/netcode)**
for *rules*, and **02 → 03 → 04 → 01** for *code structure*. If an implementer hits an unresolved
ambiguity, they pick the smallest implementation consistent with this contract and leave a
`// CONTRACT-GAP:` comment — they do not invent new systems.

Section map:
1. Reconciliations (binding rulings + tech-lead rulings)
2. File manifest — six work packages, every repo file assigned exactly once
3. Header contract — exact cross-package API for every UCLASS/USTRUCT/UENUM
4. Shared conventions + the numbers table
5. UE 5.6 correctness notes

---

## 1. Reconciliations

### 1.1 Orchestrator rulings (BINDING — already decided, conform everything to these)

| # | Conflict | Ruling |
|---|---|---|
| B1 | Combat format: 01 round-based vs 02 respawn-TDM/score-cap-25 | **Round-based elimination per 01.** First to 4 round wins, max 7 rounds, 90 s rounds, 5 s freeze, 7 s intermission, no in-round respawn, teams swap halves every round, sudden-death tiebreak (1 HP, 60 s, timer tie = match draw). 02's TDM assumption is overruled; the GameMode/GameState loop is shaped for rounds. 04's `EPFRespawnMode` switch is kept so a casual respawn mode stays cheap later; `RoundElimination` is v1-primary. |
| B2 | Hit model: 01 3-HP vs 02 one-hit | **3 HP per round per 01.** Body hit = 1, mask/head hit = 2, no regen. Sudden death = round HP 1 (parameter, not a system). |
| B3 | Shooting: 04 projectiles vs 02 hitscan | **True projectiles per 04.** 10,000 uu/s, gravity scale 0.35, fake-and-verify (instant client cosmetic projectile + server-authoritative non-replicated projectile), shared deterministic spread seed `HashCombine(PlayerId, ShotIndex)`, hitmarker only on `ClientHitConfirm`. Everything else in 02's netcode/replication patterns stands. |
| B4 | Grid: 02 250 uu / 01 "4 m" / 03 exact | **03 wins:** 400 uu cells, 300 uu wall height, 36.87° ramp (300/400 = Fortnite's slope), sub-grid 100 uu, 4 levels. |
| B5 | Arena size | **03's field:** 16×10 cells (6400×4000 uu), height cap 1200 uu, 2-cell neutral no-build midline strip (cells x=7..8), no-build spawn columns (x=0, x=15). |
| B6 | Build economy: 01 team-pooled 40 BP vs 02 60 pieces vs 03 | **03 wins:** per-player budgets, 30 structural + 6 props (tuned 4v4), 100 % refund on build-phase delete, teammates may delete team pieces, enemies never. |
| B7 | Piece palette | **03's 7:** Wall, Floor, Ramp, Roof (cone proxy) + props Can (cyl r60 h220), Dorito (cone r120 h200), Snake (400×120×120). 01's half-wall is deferred, not in v1. |
| B8 | Build-piece replication | **Single `APFBuildGrid` actor + `FFastArraySerializer` of compact integer piece records + InstancedStaticMeshComponents per piece type, server-assigned `PieceId`.** No per-piece actors; 02's dormant-actor sketch is superseded. |
| B9 | Ammo: 01 never-reload vs 02 200-ball vs 04 | **04 wins:** full-auto 12 bps (720 RPM), 100-ball hopper, infinite reserve, 1.0 s reload, auto-reload on empty. |
| B10 | Splat mechanism | **02's pooled squashed engine-sphere meshes** (pool 256, team-color MID) implementing **04's behavior**: server-multicast cosmetic, persist across rounds within a match, victim screen-edge mask splats as UMG. No decals in v1. |
| B11 | Enemy half visibility in BuildPhase: 01 visible vs 03 Curtain | **VISIBLE per 01.** No Curtain actor in v1 (recorded as deferred taste option). Midline is a traversal-blocking but see-through barrier during build. |
| B12 | Friendly fire | **OFF.** 04's model: cosmetic-only splats on teammates, no elim progress, no hitmarker. |
| B13 | Vote/arena persistence: 01 ratings.jsonl + snapshot vs 02 ArenaRatings JSON vs 03 ArenaLayout | **ONE JSON file per match** in `Saved/Arenas/` using 03's ArenaLayout schema v1 (fingerprint hashes + piece list) with 01/02's votes block appended at VotePhase end: thumb + liked/disliked FName category arrays per player, `voterBuiltHalf` / `voterWonMatch` fields. No separate JSONL, no USaveGame. |
| B14 | Movement/camera numbers | **04 is authoritative:** walk 600 / sprint 830, slide with fire-during-slide hipfire-only, FOV 105/70, ADS-in 0.18 s, sprint-out 0.18 s, zero head bob. 02's 900/300/90→65 numbers are overruled. |

### 1.2 Tech-lead rulings (additional conflicts found; authority split applied)

| # | Conflict / gap | Ruling + one-line rationale |
|---|---|---|
| T1 | 04 says v1 hit zone = "single capsule, always Body", but B2 requires mask = 2 dmg | **Mask region ships in v1.** `EPFBodyRegion::Mask` when the impact point Z ≥ victim capsule-top − 35 uu (works standing & crouched); else Body. Rules (01) beat 04's cut list; no skeletal sockets needed in graybox. |
| T2 | BuildPhase duration: 01 = 180 s, 02 diagram = 240 s, 03 = "90–150 s" | **180 s** (01 is rules authority). 30 s / 10 s HUD warnings per 01. |
| T3 | VotePhase/Results: 01 = 20 s / 15 s, 02 diagram = 30 s | **20 s vote, 15 s results** (01). Vote auto-advances early if every connected player has voted. |
| T4 | Build controls: 01 sketches "Q toggles build mode, 1–7 selects", 03 specs always-on build mode + F1–F4 + Q tap/hold wheel | **03 wins** — 01 explicitly delegates exact bindings. Build mode is always-on during BuildPhase; no `ToggleBuildMode` action exists. Final action list lives in §3.2 (`UPFInputConfig`). 02's 15-action list is superseded by this contract's 22-action list. |
| T5 | Death camera: 01 = 3 s free-cam, 04 = 0.5 s locked death cam → teammate spectate, no free-cam | **04 wins** (camera feel is 04's authority; free-cam is also an info exploit 04 explicitly closes). 0.5 s locked death cam at own body → first-person spectate nearest living teammate; Fire = next, ADS = previous. |
| T6 | Round start: 01 = 5 s freeze, 04 = "3-2-1 lock" | **5 s freeze** (01 rules). Movement locked, look/ADS free, breakout horn stub `PlayBreakout()` at 0. Intermission **7 s** (01) over 04's 5 s. |
| T7 | Ghost preview: 03 = 35 % translucent MID, 02 = "BasicShapeMaterial is opaque; color-only validity" | **Opaque ghost, color-only** (02 is authority on zero-asset feasibility; runtime blend-mode switching on an MID is impossible). 03's exact colors kept: valid green (0.1, 0.9, 0.2), invalid red (0.95, 0.1, 0.1), delete-highlight orange (1.0, 0.55, 0.1). Graybox concession, recorded. |
| T8 | B8 says "7 ISMCs", 03 wanted per-instance custom-data team tint — but `BasicShapeMaterial` exposes no per-instance custom data | **7 ISMCs per team = 14 ISMCs total** on `APFBuildGrid`, each with its own team-tinted MID. Keeps B8's spirit (one actor, FastArray, ISM-per-piece-type) with a mechanism that actually renders team color. |
| T9 | 02's `UPFBuildGridSubsystem` + `APFTeamSpawnZone` vs 03's `ABuildGrid` | **Folded into `APFBuildGrid`** (occupancy + validation live on the grid actor; clients mirror from FastArray callbacks). Spawn zones become constants in `CombatForgeTypes.h` + geometry on `APFArenaShell`. Neither 02 class exists. |
| T10 | 02's `APFCosmeticTracer` vs 04's cosmetic projectile | **Superseded by `APFPaintballProjectile` in Cosmetic mode** (04's fake-and-verify needs a real ballistic cosmetic, not a straight-line tracer). Class `APFCosmeticTracer` does not exist. |
| T11 | 02 §1.7 "25 UCLASSes, anything not listed does not exist" | **Superseded by §3 of this contract** — the class list below is the exhaustive one. |
| T12 | 02 puts `PFHealthComponent` in `Player/` | **Moved to `Combat/` and owned by pkg-weapons** (hit/elimination pipeline is one package; folder follows package). |
| T13 | Vote categories: 01 = 8 lowercase IDs, 02 = 6 PascalCase FNames | **01's 8 win** (rules authority): `layout, cover, verticality, flow, balance, sightlines, creativity, pacing`, stable integer IDs 1–8, stored as lowercase FName strings in JSON. |
| T14 | 01 §6 cut the v1 palette to 5 pieces (wall, half-wall, floor, ramp, can) | **Overruled by B7** — full 7-piece palette (incl. Dorito, Snake, Roof) ships; half-wall stays deferred. Props use the 100 uu sub-grid and 90° yaw (01's "4 m grid only" cut is void; 03's sub-grid is in). |
| T15 | 01's small plot (40×30 m) for 1v1–2v2 | **Dropped** — B5 fixes one field (16×10) for all team sizes. 01's *round scaling* survives as config: ≤2v2 → first to 3, max 5 rounds, 60 s round timer. |
| T16 | Weapon class shape: 02 `UPFWeaponComponent` vs 04 `APFMarker` actor | **Component wins** (02 is structure authority). Class names: `UPFWeaponComponent`, `APFPaintballProjectile`. `APFMarker` does not exist. |
| T17 | "Content ships empty" vs 02 D9's `L_Graybox.umap` | **Repo `Content/` ships empty** (`Content/Maps/.gitkeep` only). `L_Graybox.umap` (an empty level container, not authored content) is created once on the PC during first-run setup per 02 D9; the exact click-path is documented in `docs/pc-setup.md`. It is committed from the PC afterward. |
| T18 | Scoring granularity: 01 §3.2 full table vs 01 §6 deferral of assists | Implement **elim 100, round-survival 25, round-win 50**; assists deferred (matches 01 §6). MVP = highest score. |
| T19 | Delete refund recipient | **Original builder's budget** (03's detail survives under B6's "100 % refund"). |
| T20 | Splat pool lifetime | Splats **persist across rounds** (B10/04); pool `ResetPool()` fires on BuildPhase entry (new arena) only — clients trigger it from the phase-change delegate. |
| T21 | Fire gating across phases | Fire is server-valid only in **Lobby (warm-up pen)** and **Combat rounds in `Live` state**. BuildPhase weapons are dead (01 §2.3); Freeze/Intermission fire is rejected server-side. |
| T22 | 01 gives no Ready/host-start bindings | **F = Ready toggle** (Lobby + BuildPhase), **Enter = host force-start** (Lobby, host only), **hold Tab in Lobby** shows roster overlay with cursor; host clicks a player row to cycle team (01's "drag-swap" simplified to click-cycle). |
| T23 | Midline barrier must block traversal yet stay see-through with opaque-only materials | Barrier = **invisible full-height blocking volume** (blocks `Pawn` + `Paintball` during BuildPhase only) + **visible 40 uu-wide gray posts every 400 uu** + painted floor stripe. Reads as a barrier, occludes nothing. |
| T24 | Voter identity | Per-install `FGuid` generated/persisted by `UCombatForgeGameInstance` at `Saved/CombatForge/Identity.json` (02); written into arena JSON as SHA1-hex of the GUID (03's "anonymized hash"). |
| T25 | Prop Z on floors breaks 03's integer sub-grid (floor top was base+20) | **Floor slab top = level base Z** (slab occupies −20..0 relative). All support tops are exact 300-multiples → prop Z stays integer sub-grid. Level-0 floor sits flush in the ground slab; cosmetic only. |
| T26 | Reload key vs rotate key (both "R") | Same physical key, different contexts: `IA_Reload` lives in `IMC_Combat`, `IA_RotatePiece` in `IMC_Build`. Contexts are never active simultaneously. |
| T27 | Fingerprint definition (01 "canonical piece manifest" vs 02 "type+cell+rot") | `arenaId` = SHA1-hex over the grid header + pieces sorted by (Type, X, Y, Z, Rot, Team), **excluding PieceId/Owner** (identical rebuilds hash identically regardless of who placed). `halfHashA/B` = same algorithm over the team-filtered subset excluding Team. |
| T28 | Net update rates (04 §5.3 vs 02 silence) | 04 adopted: pawns `NetUpdateFrequency=60`, `MinNetUpdateFrequency=30`; GameState/BuildGrid 10. Listen server 60 Hz (`NetServerMaxTickRate=60`). |
| T29 | Warm-up pen ownership | `APFArenaShell` (pkg-building) spawns pen geometry + dummy slot transforms; GameMode spawns one `APFTargetDummy` (pkg-weapons) per connected player. |
| T30 | Vote wire format | RPC carries category **integer IDs (uint8, 1–8)**; JSON stores the FName strings. Max 4 chip selections total (liked + disliked combined) per 01 §4.2, enforced in widget and re-enforced server-side. |

---

## 2. File manifest — six work packages

Rules: every file below is owned by **exactly one** package. A package may freely edit only its own
files. Other packages' headers are consumed strictly through the §3 contract — if §3 doesn't show
a member, you may not call it. `Source/CombatForge/` is abbreviated `Src/` below.

**Content/ note (explicit):** the `Content/` folder ships **EMPTY** in v1 — zero editor-authored
assets. The only repo entry is `Content/Maps/.gitkeep`. `L_Graybox.umap` is created once on the
Windows PC during first-run setup (T17, steps in `docs/pc-setup.md`) and committed from there. No
package may add any other `.uasset`/`.umap`.

### pkg-core (21 files) — project skeleton, framework classes, phase/round state machine

| File | Contents |
|---|---|
| `CombatForge.uproject` | EngineAssociation "5.6", module CombatForge, EnhancedInput enabled |
| `Config/DefaultEngine.ini` | Maps/GameMode defaults, net tick rate, collision channels (§4.6), motion blur off |
| `Config/DefaultGame.ini` | Project name/ID, copyright notice |
| `Config/DefaultInput.ini` | Empty guard file (5.6 defaults already Enhanced Input) |
| `Source/CombatForge.Target.cs` | Game target, `BuildSettingsVersion.V5`, `Unreal5_6` include order |
| `Source/CombatForgeEditor.Target.cs` | Editor target, same settings |
| `Src/CombatForge.Build.cs` | Deps: Core, CoreUObject, Engine, InputCore, EnhancedInput, UMG, Slate, SlateCore, NetCore, Json, JsonUtilities |
| `Src/CombatForge.h` / `.cpp` | Module impl, `CombatForgeLog` category, startup engine-asset `ensureMsgf` checks |
| `Src/Core/CombatForgeTypes.h` / `.cpp` | ALL shared enums/structs/constants/delegates (§3.1) |
| `Src/Core/CombatForgeGameInstance.h` / `.cpp` | Per-install GUID identity |
| `Src/Core/CombatForgeGameMode.h` / `.cpp` | Server-only phase + round state machine, spawn logic, validation gates |
| `Src/Core/CombatForgeGameState.h` / `.cpp` | Replicated match truth + UI delegates |
| `Src/Core/CombatForgePlayerState.h` / `.cpp` | Team, ready, budgets, score, roster index |
| `Src/Core/CombatForgePlayerController.h` / `.cpp` | Input-context/mode switching, HUD creation, vote/ready RPCs, spectate |

### pkg-character (8 files) — pawn, movement, camera, Enhanced Input construction

| File | Contents |
|---|---|
| `Src/Player/CombatForgeCharacter.h` / `.cpp` | The one pawn (build + combat), camera, FOV arbiter, input binding hub |
| `Src/Player/PFCharacterMovementComponent.h` / `.cpp` | Sprint/ADS compressed flags, `CMOVE_Slide`, `FSavedMove_PF` |
| `Src/Player/PFCameraShakes.h` / `.cpp` | `UPFLandShake`, `UPFFireShake` (asset-free `UCameraShakeBase` subclasses) |
| `Src/Input/PFInputConfig.h` / `.cpp` | All 22 native `UInputAction`s + 3 `UInputMappingContext`s, built in C++ |

### pkg-weapons (12 files) — marker, projectile, splats, hit/elimination pipeline

| File | Contents |
|---|---|
| `Src/Combat/PFWeaponComponent.h` / `.cpp` | Fire pipeline (client fake + `ServerFire`), hopper/reload, spread/bloom, hit-confirm |
| `Src/Combat/PFPaintballProjectile.h` / `.cpp` | One class, Authoritative (server) and Cosmetic (client) modes; cosmetic pool internal |
| `Src/Combat/PFHealthComponent.h` / `.cpp` | 3-HP paint model, mask region calc, elimination broadcast, victim feedback RPC |
| `Src/Combat/PFSplatSubsystem.h` / `.cpp` | 256-pool squashed-sphere world splats, pending/confirmed reconcile @75 uu |
| `Src/Combat/PFCombatAudio.h` / `.cpp` | Named stub hooks: `PlayHitmarker/PlayElim/PlayMuzzle/PlaySplatIncoming/PlayBreakout/PlayDenied` |
| `Src/Combat/PFTargetDummy.h` / `.cpp` | Warm-up pen dummy (owns a `UPFHealthComponent`, self-resets 1 s after "elim") |

### pkg-building (9 files) — build component, grid actor, ghost, snap math, arena shell, layout serialization

| File | Contents |
|---|---|
| `Src/Building/PFBuildComponent.h` / `.cpp` | Build input handling, equip/wheel state, ghost preview, place/delete RPCs, turbo |
| `Src/Building/PFBuildGrid.h` / `.cpp` | `APFBuildGrid`: FastArray of piece records, 14 ISMCs, occupancy + shared validation |
| `Src/Building/PFGridMath.h` | Header-only static snap/quantize/canonicalize/transform math |
| `Src/Building/PFArenaShell.h` / `.cpp` | Floor slab, perimeter walls, midline barrier (T23), spawn strips, warm-up pen, spawn transforms |
| `Src/Building/PFArenaSerialization.h` / `.cpp` | Fingerprints (T27) + ArenaLayout JSON builder (piece block only) |

### pkg-ui (18 files) — all C++ UMG

| File | Contents |
|---|---|
| `Src/UI/PFRootHUDWidget.h` / `.cpp` | Viewport root, `UWidgetSwitcher` over phase panels, wires wheel↔build component |
| `Src/UI/PFLobbyWidget.h` / `.cpp` | Roster + ready states, host start hint, host team-swap (Tab-hold cursor) |
| `Src/UI/PFBuildHUDWidget.h` / `.cpp` | `▦ 23/30  ◆ 4/6`, equipped piece, phase timer + warnings, ready counts, deny flash |
| `Src/UI/PFBuildWheelWidget.h` / `.cpp` | 8-sector radial (Q-hold), dead zone 90 px, digits 1–8, budget readout |
| `Src/UI/PFCombatHUDWidget.h` / `.cpp` | Live-spread crosshair, hopper `100/∞`, round pips, round timer, alive counts, elim feed |
| `Src/UI/PFCombatFeedbackWidget.h` / `.cpp` | Hitmarkers, damage-direction arcs, mask splats (cap 6), elim confirm text |
| `Src/UI/PFVoteWidget.h` / `.cpp` | Step 1 thumbs → Step 2 8 cycling chips (max 4), 20 s ring, submit |
| `Src/UI/PFResultsWidget.h` / `.cpp` | Winner banner, MVP, thumb tally, top category, host return-to-lobby |
| `Src/UI/PFScoreboardWidget.h` / `.cpp` | Hold-Tab overlay of PlayerStates |

### pkg-meta (12 files) — repo hygiene, docs, vote persistence

| File | Contents |
|---|---|
| `README.md` | Project pitch, build quickstart, package map |
| `docs/pc-setup.md` | 02 §5 checklist + T17 `L_Graybox.umap` creation steps + PIE net-test settings |
| `.gitignore` | `Binaries/ DerivedDataCache/ Intermediate/ Saved/ .vs/ *.sln` |
| `.editorconfig` | Tabs/UTF-8/final-newline per §4 |
| `Src/Voting/PFRatingSubsystem.h` / `.cpp` | `UPFRatingSubsystem` — match record lifecycle, vote collection, JSON file I/O |
| `Content/Maps/.gitkeep` | Keeps the empty Content tree in git (T17) |
| `docs/design/01-game-design.md` … `05-code-contract.md` (5 files) | Pre-existing design docs — read-only for all packages |

**Package balance note:** pkg-core carries the phase machine (biggest single .cpp), pkg-ui carries
the most files (small each), pkg-weapons and pkg-building are the deep-logic packages,
pkg-character is small but precision-critical (prediction), pkg-meta is small by design — its owner
is also the integration reviewer for JSON schema conformance (§3.6).

---

## 3. Header contract

Legend: everything shown here is **exact and binding** (names, types, specifiers, RPC flavors).
Members marked *(intra)* are summarized — their package may shape them freely as long as the
described behavior holds. Every UCLASS/USTRUCT below is `COMBATFORGE_API`. Every replicated
property must be registered in `GetLifetimeReplicatedProps` (§5). Delegates are **non-dynamic**
C++ multicast delegates (no BP); subscribers bind with `AddUObject` and MUST unbind
(`RemoveAll(this)`) in `NativeDestruct`/`EndPlay`.

### 3.1 `Core/CombatForgeTypes.h` (pkg-core) — the one shared header

Everyone includes this. It includes only engine headers. No class in it — types, constants,
delegates only.

```cpp
// ---- Enums ----
UENUM(BlueprintType)
enum class EPFMatchPhase : uint8 { Lobby = 0, Build = 1, Combat = 2, Vote = 3, Results = 4 };

UENUM(BlueprintType)
enum class EPFRoundState : uint8 { None = 0, Freeze = 1, Live = 2, Intermission = 3 };

UENUM(BlueprintType)
enum class EPFPieceType : uint8
{
    Wall = 0, Floor = 1, Ramp = 2, Roof = 3,
    PropCan = 4, PropDorito = 5, PropSnake = 6,
    MAX_Count = 7 UMETA(Hidden)
};
// Structural = Wall..Roof; Prop = PropCan..PropSnake. Helper:
FORCEINLINE bool PFIsProp(EPFPieceType T) { return T >= EPFPieceType::PropCan && T <= EPFPieceType::PropSnake; }

UENUM(BlueprintType)  // client-side equip slot; values 0..6 static_cast to EPFPieceType
enum class EPFBuildTool : uint8
{
    Wall = 0, Floor = 1, Ramp = 2, Roof = 3,
    PropCan = 4, PropDorito = 5, PropSnake = 6, Delete = 7
};

UENUM()
enum class EPFDenyReason : uint8
{
    None = 0, WrongPhase, OutOfBudget, SlotOccupied, Overlapping, OutOfPlot,
    NoAnchor, HeightCap, RateLimited, NotYourTeam, InvalidPiece, NotFound
};

UENUM()
enum class EPFBodyRegion : uint8 { Body = 0, Mask = 1, Legs = 2 };  // Mask ≡ head; Legs reserved (T1)

UENUM()
enum class EPFRespawnMode : uint8 { RoundElimination = 0, Respawn = 1 };  // B1: RoundElimination is v1

UENUM()
enum class EPFThumbVote : uint8 { Abstained = 0, Up = 1, Down = 2 };

// ---- Replication / gameplay structs ----
USTRUCT()
struct COMBATFORGE_API FPFBuildPieceRec : public FFastArraySerializerItem
{
    GENERATED_BODY()
    UPROPERTY() uint16       PieceId  = 0;   // server-assigned, monotonic per match, never reused
    UPROPERTY() EPFPieceType Type     = EPFPieceType::Wall;
    UPROPERTY() int16        X        = 0;   // sub-grid units (100 uu). Structural: multiples of 4 (cell min-corner). Props: center.
    UPROPERTY() int16        Y        = 0;
    UPROPERTY() int16        Z        = 0;   // sub-grid units. Structural: level*3 (0/3/6/9). Props: support-top (T25 keeps it integral).
    UPROPERTY() uint8        Rot      = 0;   // 0-3 = 90° yaw steps. Walls: canonical edge (0=N, 1=E). Floor/Roof: stored but render-invariant.
    UPROPERTY() uint8        OwnerIdx = 0;   // roster index (ACombatForgePlayerState::RosterIndex), for refunds/attribution
    UPROPERTY() uint8        Team     = 0;   // 0 = A, 1 = B
};

USTRUCT()
struct COMBATFORGE_API FPFShotPacket
{
    GENERATED_BODY()
    UPROPERTY() FVector_NetQuantize100    Origin = FVector::ZeroVector;
    UPROPERTY() FVector_NetQuantizeNormal Dir    = FVector::ForwardVector;
    UPROPERTY() uint32                    ShotIndex  = 0;   // per-weapon monotonic; spread-seed input (B3)
    UPROPERTY() float                     ClientTime = 0.f; // reserved for post-v1 rewind (04 §5.2)
};

USTRUCT()
struct COMBATFORGE_API FPFPaintHitInfo
{
    GENERATED_BODY()
    UPROPERTY() TObjectPtr<class ACombatForgePlayerState> ShooterPS = nullptr; // server-side only; not for wire
    UPROPERTY() uint8                     ShooterTeam  = 0;
    UPROPERTY() FVector_NetQuantize       ImpactPoint  = FVector::ZeroVector;
    UPROPERTY() FVector_NetQuantizeNormal ImpactNormal = FVector::UpVector;
    UPROPERTY() EPFBodyRegion             Region       = EPFBodyRegion::Body;
    UPROPERTY() uint8                     Damage       = 1;    // 1 body, 2 mask (server fills)
    UPROPERTY() float                     ServerTime   = 0.f;
};

USTRUCT()
struct COMBATFORGE_API FPFElimEntry
{
    GENERATED_BODY()
    UPROPERTY() FString ShooterName;  UPROPERTY() uint8 ShooterTeam = 0;
    UPROPERTY() FString VictimName;   UPROPERTY() uint8 VictimTeam  = 0;
    UPROPERTY() float   ServerTime = 0.f;
};

USTRUCT()
struct COMBATFORGE_API FPFVoteTally   // replicated on GameState for the Results screen
{
    GENERATED_BODY()
    UPROPERTY() uint8 Up = 0;
    UPROPERTY() uint8 Down = 0;
    UPROPERTY() uint8 Abstained = 0;
    UPROPERTY() uint8 LikedCounts[8]    = {0};   // index = category ID - 1
    UPROPERTY() uint8 DislikedCounts[8] = {0};
};

USTRUCT()
struct COMBATFORGE_API FPFVoteRecord  // server-side aggregation shape (rating subsystem input)
{
    GENERATED_BODY()
    UPROPERTY() FString       VoterGuidHash;         // SHA1-hex of install GUID (T24)
    UPROPERTY() uint8         VoterTeam = 0;
    UPROPERTY() EPFThumbVote  Thumb = EPFThumbVote::Abstained;
    UPROPERTY() TArray<uint8> LikedIds;              // category IDs 1..8, ≤4 total with DislikedIds (T30)
    UPROPERTY() TArray<uint8> DislikedIds;
};

USTRUCT()
struct COMBATFORGE_API FPFMatchResult
{
    GENERATED_BODY()
    UPROPERTY() uint8  WinnerTeam = 255;   // 0/1, 255 = draw
    UPROPERTY() uint8  RoundWinsA = 0;
    UPROPERTY() uint8  RoundWinsB = 0;
    UPROPERTY() uint8  RoundsPlayed = 0;
    UPROPERTY() bool   bSuddenDeath = false;
    UPROPERTY() int32  MatchDurationSec = 0;
};

// ---- Vote category vocabulary (T13) ----
namespace PFVoteCategories
{
    // Fixed IDs 1..8 — stable forever, never renumber. Index in All = ID - 1.
    COMBATFORGE_API extern const TArray<FName>  All;      // layout, cover, verticality, flow, balance, sightlines, creativity, pacing
    COMBATFORGE_API extern const TArray<FText>  HintText; // player-facing hints per 01 §4.1
    COMBATFORGE_API FName FromId(uint8 Id);               // 1..8, NAME_None otherwise
}

// ---- Grid & field constants (compile-time; grid math must be shareable and non-instance) ----
namespace PFGrid
{
    constexpr int32 CellUU = 400;      constexpr int32 SubUU  = 100;
    constexpr int32 WallHeightUU = 300;constexpr int32 SubPerCell = 4;   // CellUU / SubUU
    constexpr int32 CellsX = 16;       constexpr int32 CellsY = 10;
    constexpr int32 Levels = 4;        constexpr int32 HeightCapUU = 1200;
    // Field-local cell columns (x): [0]=A spawn, [1..6]=A plot, [7..8]=neutral, [9..14]=B plot, [15]=B spawn
    constexpr int32 SpawnColA = 0;     constexpr int32 PlotAMinX = 1;  constexpr int32 PlotAMaxX = 6;
    constexpr int32 NeutralMinX = 7;   constexpr int32 NeutralMaxX = 8;
    constexpr int32 PlotBMinX = 9;     constexpr int32 PlotBMaxX = 14; constexpr int32 SpawnColB = 15;
    constexpr int32 SpawnPointsPerTeam = 6;
    constexpr float FieldOriginX = 0.f, FieldOriginY = 0.f;  // field corner at world origin (03 §1)
    constexpr float BuildReachUU = 1200.f;                    // ghost trace length
}

// ---- Collision channels (must match DefaultEngine.ini, §4.6) ----
constexpr ECollisionChannel PF_ECC_Paintball  = ECC_GameTraceChannel1; // projectile sweep/blocking
constexpr ECollisionChannel PF_ECC_BuildTrace = ECC_GameTraceChannel2; // ghost placement trace

// ---- Team colors (02 §3.3) ----
namespace PFColors
{
    inline const FLinearColor TeamA{0.05f, 0.35f, 1.0f};   // blue paint
    inline const FLinearColor TeamB{1.0f, 0.25f, 0.05f};   // orange paint
    inline const FLinearColor GhostValid{0.1f, 0.9f, 0.2f};
    inline const FLinearColor GhostInvalid{0.95f, 0.1f, 0.1f};
    inline const FLinearColor DeleteHighlight{1.0f, 0.55f, 0.1f};
    COMBATFORGE_API FLinearColor ForTeam(uint8 Team);
}

// ---- Cross-package delegates (declared here, owned by the classes noted in §3.x) ----
DECLARE_MULTICAST_DELEGATE_OneParam(FPFOnPhaseChanged, EPFMatchPhase);
DECLARE_MULTICAST_DELEGATE_OneParam(FPFOnRoundStateChanged, EPFRoundState);
DECLARE_MULTICAST_DELEGATE(FPFOnMatchScoreChanged);
DECLARE_MULTICAST_DELEGATE(FPFOnElimFeedChanged);
DECLARE_MULTICAST_DELEGATE(FPFOnVoteTallyChanged);
DECLARE_MULTICAST_DELEGATE(FPFOnAliveCountsChanged);
DECLARE_MULTICAST_DELEGATE(FPFOnPlayerStateFlagsChanged);          // team/ready/budget changed (UI refresh)
DECLARE_MULTICAST_DELEGATE_TwoParams(FPFOnHitConfirmed, uint32 /*ShotIndex*/, bool /*bElim*/);
DECLARE_MULTICAST_DELEGATE_OneParam(FPFOnHopperChanged, int32 /*Count*/);
DECLARE_MULTICAST_DELEGATE_OneParam(FPFOnReloadStateChanged, bool /*bReloading*/);
DECLARE_MULTICAST_DELEGATE_ThreeParams(FPFOnLocalPaintHitTaken, FVector /*ShooterLoc*/, uint8 /*ShooterTeam*/, uint8 /*NewHP*/);
DECLARE_MULTICAST_DELEGATE_TwoParams(FPFOnEliminated, class UPFHealthComponent*, const FPFPaintHitInfo&); // server-side
DECLARE_MULTICAST_DELEGATE_OneParam(FPFOnHPChanged, uint8 /*NewHP*/);
DECLARE_MULTICAST_DELEGATE_OneParam(FPFOnEquippedToolChanged, EPFBuildTool);
DECLARE_MULTICAST_DELEGATE_OneParam(FPFOnPlaceDenied, EPFDenyReason);
DECLARE_MULTICAST_DELEGATE_OneParam(FPFOnBuildWheelRequested, bool /*bOpen*/);
DECLARE_MULTICAST_DELEGATE_OneParam(FPFOnToolSelected, EPFBuildTool);
DECLARE_MULTICAST_DELEGATE_OneParam(FPFOnSlideStateChanged, bool /*bSliding*/);
```

**Log category (owned by pkg-core, declared in `Src/CombatForge.h`, defined in `Src/CombatForge.cpp`):**

```cpp
DECLARE_LOG_CATEGORY_EXTERN(CombatForgeLog, Log, All);
```

Every package logs through `UE_LOG(CombatForgeLog, ...)`. No package declares another category.

### 3.2 pkg-core classes

#### `UCombatForgeGameInstance : UGameInstance`

```cpp
UCLASS()
class COMBATFORGE_API UCombatForgeGameInstance : public UGameInstance
{
    GENERATED_BODY()
public:
    virtual void Init() override;                 // loads/creates identity file
    FGuid   GetLocalPlayerGuid() const;           // per-install identity (T24)
    FString GetLocalPlayerGuidHash() const;       // SHA1-hex of the GUID — what goes on the wire/disk
    // (intra) persistence at Saved/CombatForge/Identity.json
};
```

#### `ACombatForgeGameMode : AGameModeBase` — server-only; THE phase/round writer

```cpp
UCLASS()
class COMBATFORGE_API ACombatForgeGameMode : public AGameModeBase
{
    GENERATED_BODY()
public:
    ACombatForgeGameMode();  // sets DefaultPawnClass=ACombatForgeCharacter, PlayerControllerClass,
                            // GameStateClass, PlayerStateClass

    // ---- Config (UPROPERTY(EditDefaultsOnly, Category="PF|Match") unless noted) ----
    EPFRespawnMode RespawnMode = EPFRespawnMode::RoundElimination;   // B1
    float BuildPhaseDuration   = 180.f;   // T2
    float LobbyStartCountdown  = 5.f;
    float FreezeDuration       = 5.f;     // T6
    float RoundDuration        = 90.f;    // 60 at ≤2v2 (T15)
    float IntermissionDuration = 7.f;
    float SuddenDeathDuration  = 60.f;
    float VotePhaseDuration    = 20.f;    // T3
    float ResultsDuration      = 15.f;
    uint8 RoundWinsToTakeMatch = 4;       // 3 at ≤2v2
    uint8 MaxRounds            = 7;       // 5 at ≤2v2
    float RespawnDelay         = 5.f;     // Respawn mode only (04 Variant B)

    // ---- The only phase mutator in the codebase ----
    void SetPhase(EPFMatchPhase NewPhase);            // server; updates GameState, stamps timers, side effects

    // ---- Cross-package server entry points ----
    // pkg-weapons calls when a player pawn's HP hits 0 (dummies do NOT route here):
    void NotifyPawnEliminated(class ACombatForgeCharacter* Victim, const FPFPaintHitInfo& FinalHit);
    // pkg-core PC calls after PlayerState ready flag flips (Lobby early-start / Build early-end):
    void NotifyReadyChanged();
    // pkg-core PC forwards votes here; GameMode validates, tallies to GameState, forwards to UPFRatingSubsystem:
    void SubmitVote(class ACombatForgePlayerController* Voter, EPFThumbVote Thumb,
                    const TArray<uint8>& LikedIds, const TArray<uint8>& DislikedIds);
    // Host-only actions (from PC RPCs):
    void HostForceStart();                             // Lobby only
    void HostCycleTeam(class ACombatForgePlayerState* Target); // Lobby only (T22)
    void HostReturnToLobby();                          // Results only

    // Spawn transform for a player in the current round (side swap: even rounds swapped — B1):
    FTransform GetSpawnTransform(const class ACombatForgePlayerState* PS) const;

protected:
    // (intra) round loop: StartRound/EndRound/ResolveRoundOnTimer/CheckElimVictory/StartSuddenDeath,
    // team auto-balance + RosterIndex assignment in PostLogin, warm-up pen dummy spawning (one per
    // player, via APFArenaShell dummy slots), pawn teleport/reset per round, freeze via
    // PC->SetIgnoreMoveInput, scoring per T18, Build→Combat side effects (freeze grid, fingerprints,
    // UPFRatingSubsystem::BeginMatchRecord, teleport), Vote→Results (CommitMatchRecord).
};
```

**Phase side-effect contract (server, in `SetPhase`):**
- `Lobby→Build`: `APFBuildGrid::ClearAll()`, splat-pool reset happens client-side on phase delegate (T20), teleport pawns to own plot, reset budgets to 30/6, reset `bReady`.
- `Build→Combat`: `APFBuildGrid::FreezeBuild()`, compute fingerprints + `UPFRatingSubsystem::BeginMatchRecord(...)`, midline barrier off, round 1 `Freeze`.
- `Combat→Vote`: fire disabled (gate flips), pawns frozen, vote UI via phase OnRep.
- `Vote→Results`: tally to GameState, `UPFRatingSubsystem::CommitMatchRecord(...)` writes JSON.
- `Results→Lobby`: same roster kept; scores/budgets reset on next Build.

#### `ACombatForgeGameState : AGameStateBase` — replicated match truth

```cpp
UCLASS()
class COMBATFORGE_API ACombatForgeGameState : public AGameStateBase
{
    GENERATED_BODY()
public:
    // ---- Replicated state (all registered in GetLifetimeReplicatedProps) ----
    UPROPERTY(ReplicatedUsing=OnRep_Phase)      EPFMatchPhase Phase = EPFMatchPhase::Lobby;
    UPROPERTY(Replicated)                       float  PhaseEndServerTime = 0.f;   // 0 = untimed (Lobby)
    UPROPERTY(ReplicatedUsing=OnRep_RoundState) EPFRoundState RoundState = EPFRoundState::None;
    UPROPERTY(Replicated)                       float  RoundStateEndServerTime = 0.f;
    UPROPERTY(Replicated)                       uint8  RoundNumber = 0;            // 1-based during Combat
    UPROPERTY(ReplicatedUsing=OnRep_Score)      uint8  TeamRoundWins[2] = {0, 0};
    UPROPERTY(ReplicatedUsing=OnRep_AliveCounts)uint8  AliveCounts[2]   = {0, 0};
    UPROPERTY(Replicated)                       bool   bSuddenDeath = false;
    UPROPERTY(ReplicatedUsing=OnRep_ElimFeed)   TArray<FPFElimEntry> ElimFeed;     // capped at 50, oldest trimmed
    UPROPERTY(ReplicatedUsing=OnRep_VoteTally)  FPFVoteTally VoteTally;
    UPROPERTY(Replicated)                       FString MatchId;                   // GUID string, set at Lobby→Build

    // ---- Client-safe helpers ----
    float GetPhaseTimeRemaining() const;   // PhaseEndServerTime - GetServerWorldTimeSeconds(), clamped ≥ 0
    float GetRoundTimeRemaining() const;
    bool  IsFireAllowed()  const;          // Lobby || (Combat && Live)   (T21) — same predicate client & server
    bool  IsBuildAllowed() const;          // Build only
    class ACombatForgePlayerState* FindPlayerByRosterIndex(uint8 RosterIndex) const;

    // ---- Server-side setters (set property + manually invoke OnRep on listen host — R7) ----
    void ServerSetPhase(EPFMatchPhase NewPhase, float EndServerTime);       // GameMode only
    void ServerSetRoundState(EPFRoundState NewState, float EndServerTime);  // GameMode only
    // (intra) analogous setters for score/alive/feed/tally

    // ---- UI subscription points (broadcast from OnReps AND from server setters on host) ----
    FPFOnPhaseChanged        OnPhaseChangedEvent;
    FPFOnRoundStateChanged   OnRoundStateChangedEvent;
    FPFOnMatchScoreChanged   OnMatchScoreChangedEvent;
    FPFOnElimFeedChanged     OnElimFeedChangedEvent;
    FPFOnVoteTallyChanged    OnVoteTallyChangedEvent;
    FPFOnAliveCountsChanged  OnAliveCountsChangedEvent;

protected:
    UFUNCTION() void OnRep_Phase();
    UFUNCTION() void OnRep_RoundState();
    UFUNCTION() void OnRep_Score();
    UFUNCTION() void OnRep_AliveCounts();
    UFUNCTION() void OnRep_ElimFeed();
    UFUNCTION() void OnRep_VoteTally();
};
```

#### `ACombatForgePlayerState : APlayerState`

```cpp
UCLASS()
class COMBATFORGE_API ACombatForgePlayerState : public APlayerState
{
    GENERATED_BODY()
public:
    UPROPERTY(ReplicatedUsing=OnRep_Flags) uint8  TeamId = 255;        // 0=A, 1=B, 255=unassigned
    UPROPERTY(ReplicatedUsing=OnRep_Flags) uint8  RosterIndex = 255;   // 0..11; matches FPFBuildPieceRec::OwnerIdx
    UPROPERTY(ReplicatedUsing=OnRep_Flags) bool   bReady = false;      // Lobby + BuildPhase (reset each phase)
    UPROPERTY(Replicated)                  bool   bHasVoted = false;
    UPROPERTY(Replicated)                  bool   bAliveInRound = false;
    UPROPERTY(ReplicatedUsing=OnRep_Flags) uint8  StructuralBudget = 30;  // B6; server-mutated only
    UPROPERTY(ReplicatedUsing=OnRep_Flags) uint8  PropBudget = 6;
    UPROPERTY(Replicated)                  uint16 Eliminations = 0;
    UPROPERTY(Replicated)                  uint16 TimesEliminated = 0;
    UPROPERTY(Replicated)                  int32  MatchScore = 0;         // T18 scoring
    UPROPERTY(Replicated)                  FString PlayerGuidHash;        // set via PC on join (T24)

    FPFOnPlayerStateFlagsChanged OnFlagsChangedEvent;  // broadcast from OnRep_Flags + server setters

    // Server-only mutators (GameMode / APFBuildGrid call these; they broadcast on host):
    void ServerSetTeam(uint8 NewTeam, uint8 NewRosterIndex);
    void ServerSetReady(bool bNewReady);
    void ServerSetBudgets(uint8 Structural, uint8 Props);
    bool ServerTrySpendBudget(EPFPieceType Type);      // false if empty; decrements correct pool
    void ServerRefundBudget(EPFPieceType Type);        // T19: called with the ORIGINAL builder's PS
    void ServerAddScore(int32 Delta);

protected:
    UFUNCTION() void OnRep_Flags();
};
```

#### `ACombatForgePlayerController : APlayerController`

```cpp
UCLASS()
class COMBATFORGE_API ACombatForgePlayerController : public APlayerController
{
    GENERATED_BODY()
public:
    // ---- RPCs (client → server, all Reliable) ----
    UFUNCTION(Server, Reliable) void ServerSetReady(bool bNewReady);
    UFUNCTION(Server, Reliable) void ServerSubmitVote(EPFThumbVote Thumb,
                                    const TArray<uint8>& LikedIds, const TArray<uint8>& DislikedIds);
    UFUNCTION(Server, Reliable) void ServerSetPlayerGuidHash(const FString& GuidHash); // once, on join
    UFUNCTION(Server, Reliable) void ServerHostForceStart();      // ignored if not host
    UFUNCTION(Server, Reliable) void ServerHostCycleTeam(ACombatForgePlayerState* Target);
    UFUNCTION(Server, Reliable) void ServerHostReturnToLobby();
    UFUNCTION(Server, Reliable) void ServerSpectateNext(bool bForward); // dead-only; server retargets ViewTarget

    // ---- Cross-package accessors ----
    class UPFInputConfig* GetInputConfig() const;   // never null after SetupInputComponent

    // ---- Behavior contract (intra, but binding) ----
    // SetupInputComponent: NewObject<UPFInputConfig>(this), InputConfig->Build(this); bind
    //   IA_Ready/IA_Scoreboard/IA_MenuBack + host keys here (PC-level actions).
    // OnPossess/BeginPlayingState (local only): apply IMCs for current phase; retry if
    //   UEnhancedInputLocalPlayerSubsystem not yet alive (02 R1).
    // Phase reaction (binds GameState delegates): input contexts + input mode per §4.5 table;
    //   creates UPFRootHUDWidget once in BeginPlayingState (IsLocalController() guard).
    // Elimination camera (RoundElimination): 0.5 s locked death cam at body, then ViewTarget =
    //   nearest living teammate; Fire/ADS cycle next/prev via ServerSpectateNext (T5).
    // Freeze handling: SetIgnoreMoveInput(true) during Freeze/Intermission/Vote, look stays free in Freeze.
private:
    UPROPERTY() TObjectPtr<class UPFInputConfig>    InputConfig;   // GC root for all input objects (02 R1)
    UPROPERTY() TObjectPtr<class UPFRootHUDWidget>  RootHUD;
};
```

### 3.3 pkg-character classes

#### `ACombatForgeCharacter : ACharacter` — the one pawn, both phases (no pawn swap)

```cpp
UCLASS()
class COMBATFORGE_API ACombatForgeCharacter : public ACharacter
{
    GENERATED_BODY()
public:
    // MUST use ObjectInitializer ctor to swap in the custom CMC (§5):
    ACombatForgeCharacter(const FObjectInitializer& ObjectInitializer);

    // ---- Component accessors (cross-package; never null after construction) ----
    class UPFCharacterMovementComponent* GetPFMovement() const;
    class UPFWeaponComponent*  GetWeapon() const;
    class UPFBuildComponent*   GetBuild()  const;
    class UPFHealthComponent*  GetHealth() const;
    class UPFCombatAudio*      GetCombatAudio() const;
    class UCameraComponent*    GetFirstPersonCamera() const;

    // ---- ADS (owned here; CMC + weapon read through these) ----
    void  SetADS(bool bWantsADS);          // input entry; respects slide rule (queue during slide — 04 §1.2)
    bool  IsADS() const;                   // target state
    float GetADSAlpha() const;             // 0..1 transition alpha (0.18 in / 0.14 out) — weapon spread lerp input

    // ---- Team + elimination cosmetics (pkg-weapons calls these) ----
    void  SetTeamColor(uint8 TeamId);      // MID tint on the graybox mesh
    void  SetEliminatedAppearance(bool bEliminated); // hide mesh; collision handled by health component
    FVector GetMuzzleLocation(bool bCosmetic) const; // 04 §2.2: server = capsule offset; cosmetic = camera+20fwd

    // ---- Behavior contract (intra, but binding) ----
    // Camera: UCameraComponent at capsule-top − 10 uu, bUsePawnControlRotation, mesh hidden to owner.
    // FOV arbiter UpdateTargetFOV(): base 105, ADS 70 (ease-out cubic), sprint +6 (0.15 s), slide +9;
    //   single compose point — effects never fight (04 §1.3). Zero head bob.
    // Landing dip: UPFLandShake on falls > 300 uu.
    // Jump buffer 0.1 s, no coyote time. JumpZ 630, GravityScale 1.5 (set in ctor).
    // SetupPlayerInputComponent: binds Move/Look/Jump/Sprint/CrouchSlide/Fire/ADS/Reload and forwards:
    //   Fire→Weapon->StartFire/StopFire, Reload→Weapon->StartReload, ADS→SetADS,
    //   Sprint→GetPFMovement()->SetWantsToSprint, CrouchSlide→GetPFMovement()->OnCrouchSlidePressed/Released,
    //   then calls GetBuild()->BindInput(EIC, PC->GetInputConfig())  ← pkg-building owns build handling.
    // Sprint blocks fire/ADS; sprint-out 0.18 s with buffered fire release (04 §1.1).
};
```

#### `UPFCharacterMovementComponent : UCharacterMovementComponent`

```cpp
UCLASS()
class COMBATFORGE_API UPFCharacterMovementComponent : public UCharacterMovementComponent
{
    GENERATED_BODY()
public:
    // Custom movement mode value under MOVE_Custom:
    static constexpr uint8 CMOVE_Slide = 0;

    // ---- Input intents (prediction-safe; ride compressed flags, NEVER RPCs — 02 D5) ----
    void SetWantsToSprint(bool bWants);    // FLAG_Custom_0
    void SetWantsToADS(bool bWants);       // FLAG_Custom_1 (speed effect must be in the move stream)
    void OnCrouchSlidePressed();           // sets bWantsToCrouch; CMC decides slide vs crouch deterministically
    void OnCrouchSlideReleased();

    // ---- Cross-package queries ----
    bool IsSliding() const;                // MovementMode == MOVE_Custom && CustomMovementMode == CMOVE_Slide
    bool IsSprintingEffective() const;     // sprint flag AND grounded AND input dot forward > 0.5
    bool IsSlideGlidePhase() const;        // first 0.35 s (slide FOV kick window)

    FPFOnSlideStateChanged OnSlideStateChanged;  // character binds for FOV kick; fired on both sides

    // ---- Config (UPROPERTY(EditDefaultsOnly, Category="PF|Movement"); ctor defaults per 04) ----
    float WalkSpeed = 600.f, SprintSpeed = 830.f, CrouchSpeed = 300.f, ADSSpeedMult = 0.55f;
    float SlideMinEnterSpeed = 750.f, SlideBoostSpeed = 1150.f, SlideSteerDegPerSec = 15.f;
    float SlideGlideTime = 0.35f, SlideGlideFriction = 0.6f, SlideEndFriction = 8.f,
          SlideFrictionRampTime = 0.5f, SlideMinExitSpeed = 320.f, SlideMaxDuration = 1.1f,
          SlideCooldown = 0.5f;

    // ---- Overrides (binding): GetMaxSpeed() is the ONLY speed source; never poke MaxWalkSpeed
    //      externally (02 R6). UpdateFromCompressedFlags/GetCompressedFlags via FSavedMove_PF
    //      (implements Clear/GetCompressedFlags/CanCombineWith/SetMoveFor/PrepMoveFor completely).
    //      Slide physics in PhysCustom(CMOVE_Slide); entry checked in OnMovementUpdated from
    //      bWantsToCrouch + sprint + speed ≥ 750 (deterministic client/server). Slide-jump keeps
    //      horizontal velocity. Ctor sets: MaxAcceleration 4096, GroundFriction 10,
    //      BrakingDecelerationWalking 2500, BrakingFrictionFactor 1.0, JumpZVelocity 630 (on Character),
    //      GravityScale 1.5, AirControl 0.9, PerchRadiusThreshold 15, crouched half-height 58,
    //      NavAgent/crouch interp 0.2 s.
};
// (intra, but must exist under this name): class FSavedMove_PF : public FSavedMove_Character
```

#### `UPFInputConfig : UObject` — every input object, constructed native (02 §3.1, 03 §3)

```cpp
UCLASS()
class COMBATFORGE_API UPFInputConfig : public UObject
{
    GENERATED_BODY()
public:
    void Build(class ACombatForgePlayerController* OuterPC);  // constructs ALL objects below; idempotent guard

    // ---- Actions (22). ALL UPROPERTY(VisibleAnywhere) TObjectPtr<UInputAction> — GC-rooted (02 R1) ----
    // Common (IMC_Common, priority 0):
    TObjectPtr<UInputAction> IA_Move;        // Axis2D, WASD swizzle per 02 §3.1
    TObjectPtr<UInputAction> IA_Look;        // Axis2D, Mouse2D, negate Y
    TObjectPtr<UInputAction> IA_Jump;        // Space
    TObjectPtr<UInputAction> IA_Sprint;      // LeftShift, hold
    TObjectPtr<UInputAction> IA_CrouchSlide; // LeftCtrl + C, hold (slide if sprinting)
    TObjectPtr<UInputAction> IA_Ready;       // F, tap toggle (T22)
    TObjectPtr<UInputAction> IA_Scoreboard;  // Tab, hold
    TObjectPtr<UInputAction> IA_MenuBack;    // Escape
    TObjectPtr<UInputAction> IA_HostStart;   // Enter (host-only effect)
    // Combat (IMC_Combat, priority 1):
    TObjectPtr<UInputAction> IA_Fire;        // LMB, hold (weapon runs its own 12 bps gate — no InputTriggerPulse)
    TObjectPtr<UInputAction> IA_ADS;         // RMB, hold
    TObjectPtr<UInputAction> IA_Reload;      // R (T26)
    // Build (IMC_Build, priority 1):
    TObjectPtr<UInputAction> IA_Place;       // LMB, hold (turbo in component)
    TObjectPtr<UInputAction> IA_DeleteTool;  // F5 and X (both mapped — 03 §3)
    TObjectPtr<UInputAction> IA_RotatePiece; // R (T26)
    TObjectPtr<UInputAction> IA_CyclePiece;  // Mouse wheel axis
    TObjectPtr<UInputAction> IA_EquipWall;   // F1
    TObjectPtr<UInputAction> IA_EquipFloor;  // F2
    TObjectPtr<UInputAction> IA_EquipRamp;   // F3
    TObjectPtr<UInputAction> IA_EquipRoof;   // F4
    TObjectPtr<UInputAction> IA_QuickEquip;  // Q, UInputTriggerTap (threshold 0.18 s) → equip last-used piece
    TObjectPtr<UInputAction> IA_BuildWheel;  // Q, UInputTriggerHold (0.18 s) Started=open, release=commit

    // ---- Contexts (3). UPROPERTY TObjectPtr<UInputMappingContext> ----
    TObjectPtr<UInputMappingContext> IMC_Common;   // priority 0
    TObjectPtr<UInputMappingContext> IMC_Combat;   // priority 1
    TObjectPtr<UInputMappingContext> IMC_Build;    // priority 1
    // Modifiers/triggers are outered to their IMC (mapping array roots them).
    // Mouse sensitivity: scalar modifier on IA_Look, config "PFSensitivity", default 1.0 (04 cut #9).
};
```

### 3.4 pkg-weapons classes

#### `UPFWeaponComponent : UActorComponent` — the marker

```cpp
UCLASS()
class COMBATFORGE_API UPFWeaponComponent : public UActorComponent
{
    GENERATED_BODY()
public:
    UPFWeaponComponent();   // SetIsReplicatedByDefault(true)

    // ---- Input entry (called by character on owning client) ----
    void StartFire();       // holds; internal 12 bps accumulator w/ remainder carry (04 §2.1)
    void StopFire();
    void StartReload();     // manual; also auto-fires on empty (04 §3)

    // ---- RPC surface (binding) ----
    UFUNCTION(Server, Reliable)          void ServerFire(const FPFShotPacket& Shot);
    UFUNCTION(NetMulticast, Unreliable)  void MulticastShotFX(FVector_NetQuantize100 Origin,
                                              FVector_NetQuantizeNormal Dir, uint32 ShotIndex);
        // remote clients spawn a Cosmetic projectile; owning client skips (already fired)
    UFUNCTION(NetMulticast, Unreliable)  void MulticastImpactSplat(FVector_NetQuantize100 Loc,
                                              FVector_NetQuantizeNormal Normal, uint8 Team);
        // all clients → UPFSplatSubsystem::SpawnConfirmedSplat (owning client reconciles pending @75 uu)
    UFUNCTION(Client, Reliable)          void ClientHitConfirm(uint32 ShotIndex, bool bElimHit);
        // ONLY source of hitmarkers (B3). Broadcasts OnHitConfirmedEvent.

    // ---- Replicated (owner-only correction of predicted values) ----
    UPROPERTY(ReplicatedUsing=OnRep_Hopper) uint8 HopperCount = 100;   // COND_OwnerOnly
    UPROPERTY(ReplicatedUsing=OnRep_Reload) bool  bReloading  = false; // COND_OwnerOnly

    // ---- Cross-package reads ----
    float GetCurrentSpreadHalfAngleDeg() const;  // live cone incl. bloom + movement state; crosshair polls per tick
    // Spread-seed contract (client & server MUST both use this — B3):
    static FRandomStream MakeShotStream(int32 PlayerId, uint32 ShotIndex);
        // seed = (int32)HashCombine((uint32)PlayerId, ShotIndex); PlayerId = PlayerState::GetPlayerId()

    // ---- UI subscription points ----
    FPFOnHitConfirmed       OnHitConfirmedEvent;
    FPFOnHopperChanged      OnHopperChangedEvent;
    FPFOnReloadStateChanged OnReloadStateChangedEvent;

    // ---- Config (UPROPERTY(EditDefaultsOnly, Category="PF|Marker"); defaults per 04) ----
    float FireRateBps = 12.f;  uint8 HopperCapacity = 100;  float ReloadTime = 1.f;
    float MuzzleSpeedUU = 10000.f;  float ProjGravityScale = 0.35f;  float ProjLifetime = 2.f;
    float ProjRadiusUU = 7.f;
    float SpreadADS = 0.25f, SpreadHip = 1.5f, SpreadHipMoving = 2.0f;
    float SpreadAirAdd = 1.5f, SpreadSlideAdd = 1.0f, SpreadCrouchMult = 0.8f;
    float BloomPerShot = 0.12f, BloomCap = 1.8f, BloomDecayPerSec = 6.f, BloomDecayDelay = 0.15f,
          BloomADSMult = 0.5f;
    float SprintOutTime = 0.18f;

    // ---- Behavior contract (intra, but binding) ----
    // Client fire: consume local token → spawn Cosmetic APFPaintballProjectile (pool 64) using
    //   MakeShotStream spread → UPFFireShake → predicted HopperCount-- → ServerFire(packet). Zero
    //   perceived latency (04 §5.1).
    // Server fire: token bucket cap 3 / refill 12 s⁻¹; Origin within 150 uu of server muzzle; Dir
    //   within 4° of server view (silent reject + CombatForgeLog warn); phase gate
    //   GameState->IsFireAllowed() (T21); then spawn Authoritative projectile with SAME stream.
    // Friendly fire (B12): server team-hit → MulticastImpactSplat only; no damage, no hit event,
    //   no ClientHitConfirm.
    // Reload: 1.0 s lockout, uninterruptible by fire, cancelled by sprint/slide (restores prior
    //   count), kicks out of ADS, auto re-ADS if still held; auto-reload on empty w/ buffered fire.
};
```

#### `APFPaintballProjectile : AActor` — NEVER replicated (B3)

```cpp
UCLASS()
class COMBATFORGE_API APFPaintballProjectile : public AActor
{
    GENERATED_BODY()
public:
    APFPaintballProjectile();  // bReplicates=false; sphere collision r=7 on PF_ECC_Paintball;
                               // UProjectileMovementComponent (speed 10000, grav 0.35, no bounce);
                               // engine-sphere visual scaled 0.14, emissive team MID

    // Single init point; caller applies spread to Dir BEFORE calling (shared stream — B3):
    void InitProjectile(const FVector& Origin, const FVector& SpreadedDir, uint8 Team,
                        bool bAuthoritative, class UPFWeaponComponent* SourceWeapon, uint32 ShotIndex);

    // ---- Behavior contract ----
    // Authoritative (server only): real blocking sweep vs Pawn/World on PF_ECC_Paintball; shooter in
    //   ignore list (no self-splat). Pawn hit → resolve victim UPFHealthComponent:
    //   enemy → ApplyPaintHit(server) + SourceWeapon->ClientHitConfirm + MulticastImpactSplat;
    //   teammate → MulticastImpactSplat only (B12). World hit → MulticastImpactSplat. Destroy on hit
    //   or 2.0 s.
    // Cosmetic (clients): no gameplay collision; visual-only trace; on predicted impact registers a
    //   PENDING splat via UPFSplatSubsystem::SpawnPendingSplat(ShotIndex,...) (owning client only)
    //   then returns to pool. Pool of 64 per client lives inside pkg-weapons (intra).
};
```

#### `UPFHealthComponent : UActorComponent` — 3-HP paint model + elimination broadcast

```cpp
UCLASS()
class COMBATFORGE_API UPFHealthComponent : public UActorComponent
{
    GENERATED_BODY()
public:
    UPFHealthComponent();   // SetIsReplicatedByDefault(true)

    UPROPERTY(EditDefaultsOnly, Category="PF|Health") uint8 DefaultRoundHP = 3;   // B2

    UPROPERTY(ReplicatedUsing=OnRep_HP)         uint8 HP = 3;
    UPROPERTY(ReplicatedUsing=OnRep_Eliminated) bool  bEliminated = false;

    // ---- Server API ----
    void ApplyPaintHit(const FPFPaintHitInfo& HitTemplate);
        // fills Damage from region (Body 1 / Mask 2 — T1), decrements HP, fires
        // ClientPaintHitTaken; at 0 → bEliminated, OnEliminatedEvent broadcast, corpse blocks
        // paintballs 0.5 s then collision off (04 §2.4), owner SetEliminatedAppearance(true).
        // NOTE: does NOT call GameMode — GameMode subscribes to OnEliminatedEvent for player pawns;
        // APFTargetDummy subscribes to its own component (T29 decoupling).
    void ResetForRound(uint8 RoundHP);   // server: restore HP (3, or 1 in sudden death), un-eliminate,
                                         // restore collision/appearance
    EPFBodyRegion ComputeRegion(const FVector& ImpactPoint) const;  // Mask iff Z ≥ capsule-top − 35 uu (T1)

    // ---- Victim-side feedback (owning client) ----
    UFUNCTION(Client, Reliable) void ClientPaintHitTaken(FVector_NetQuantize ShooterLoc,
                                     uint8 ShooterTeam, uint8 NewHP);
        // broadcasts OnLocalPaintHitTakenEvent → UPFCombatFeedbackWidget (mask splats + direction arc)

    // ---- Subscription points ----
    FPFOnEliminated         OnEliminatedEvent;         // SERVER-side broadcast (GameMode, dummy)
    FPFOnHPChanged          OnHPChangedEvent;          // both sides via OnRep + server set
    FPFOnLocalPaintHitTaken OnLocalPaintHitTakenEvent; // owning client only

protected:
    UFUNCTION() void OnRep_HP();
    UFUNCTION() void OnRep_Eliminated();  // hides pawn locally, no ragdoll (02 §1.3)
};
```

#### `UPFSplatSubsystem : UWorldSubsystem` — client-side world splats (B10)

```cpp
UCLASS()
class COMBATFORGE_API UPFSplatSubsystem : public UWorldSubsystem
{
    GENERATED_BODY()
public:
    static constexpr int32 PoolSize = 256;           // B10; round-robin recycle oldest
    static constexpr float ReconcileRadiusUU = 75.f; // 04 §5.2

    void SpawnConfirmedSplat(const FVector& Loc, const FVector& Normal, uint8 Team);
        // consumes a matching pending splat within 75 uu if one exists (owning client)
    void SpawnPendingSplat(uint32 ShotIndex, const FVector& Loc, const FVector& Normal, uint8 Team);
        // 60% brightness variant; auto-fades 0.2 s if never confirmed within 0.6 s
    void ResetPool();  // clients call on BuildPhase entry (T20); splats persist across rounds

    // (intra) pool of UStaticMeshComponents: engine sphere scaled (0.4, 0.4, 0.02), MID team color,
    // NoCollision, CastShadow=false, +1 uu normal offset, random yaw + 0.8–1.3 scale jitter (02 §3.4).
    // Does nothing on dedicated/server-only worlds.
};
```

#### `UPFCombatAudio : UActorComponent` — stub hooks (04 §4)

```cpp
UCLASS()
class COMBATFORGE_API UPFCombatAudio : public UActorComponent
{
    GENERATED_BODY()
public:
    // v1 bodies: pitch-shifted engine default sound or no-op. CALL SITES ARE THE DELIVERABLE.
    void PlayHitmarker(); void PlayElim(); void PlayMuzzle();
    void PlaySplatIncoming(); void PlayBreakout(); void PlayDenied();
};
```

#### `APFTargetDummy : AActor` — warm-up pen target (T29)

```cpp
UCLASS()
class COMBATFORGE_API APFTargetDummy : public AActor
{
    GENERATED_BODY()
public:
    APFTargetDummy();  // bReplicates=true; engine-cylinder body (r≈40, h≈180) blocking PF_ECC_Paintball;
                       // owns a UPFHealthComponent (DefaultRoundHP=1)
    // Behavior: binds own health OnEliminatedEvent → hide 1.0 s → ResetForRound(1) (self-resetting).
    // Never routes to GameMode; never grants score.
};
```

### 3.5 pkg-building classes

#### `APFBuildGrid : AActor` — the single replicated arena container (B8)

```cpp
// FastArray wrapper — lives in PFBuildGrid.h, only the grid uses it directly:
USTRUCT()
struct COMBATFORGE_API FPFBuildPieceArray : public FFastArraySerializer
{
    GENERATED_BODY()
    UPROPERTY() TArray<FPFBuildPieceRec> Items;
    UPROPERTY(NotReplicated) TObjectPtr<class APFBuildGrid> OwnerGrid;   // set server+client in ctor/BeginPlay

    bool NetDeltaSerialize(FNetDeltaSerializeInfo& DeltaParms)
    { return FFastArraySerializer::FastArrayDeltaSerialize<FPFBuildPieceRec, FPFBuildPieceArray>(Items, DeltaParms, *this); }
    // Client mirror hooks (call into OwnerGrid to update ISM instances + occupancy):
    void PostReplicatedAdd(const TArrayView<int32>& AddedIndices, int32 FinalSize);
    void PreReplicatedRemove(const TArrayView<int32>& RemovedIndices, int32 FinalSize);
    void PostReplicatedChange(const TArrayView<int32>& ChangedIndices, int32 FinalSize); // unused in v1 (immutable)
};
template<> struct TStructOpsTypeTraits<FPFBuildPieceArray>
    : public TStructOpsTypeTraitsBase2<FPFBuildPieceArray> { enum { WithNetDeltaSerializer = true }; };

// Placement query — one struct so client ghost and server validation share ONE predicate (03 §4):
USTRUCT()
struct COMBATFORGE_API FPFPlacementQuery
{
    GENERATED_BODY()
    UPROPERTY() EPFPieceType Type = EPFPieceType::Wall;
    UPROPERTY() int16 X = 0;  UPROPERTY() int16 Y = 0;  UPROPERTY() int16 Z = 0;   // FPFBuildPieceRec semantics
    UPROPERTY() uint8 Rot = 0;
    UPROPERTY() uint8 Team = 0;
};

UCLASS()
class COMBATFORGE_API APFBuildGrid : public AActor
{
    GENERATED_BODY()
public:
    APFBuildGrid();  // bReplicates=true, bAlwaysRelevant=true, NetUpdateFrequency=10 (T28),
                     // SetReplicateMovement(false); creates 14 ISMCs (7 types × 2 teams — T8), engine
                     // primitives per §4.2 dims, team MIDs, collision: block Pawn/Visibility/
                     // PF_ECC_Paintball/PF_ECC_BuildTrace

    UPROPERTY(Replicated) FPFBuildPieceArray Pieces;

    // ---- Shared validation (client ghost tint AND server authority — same function, both sides) ----
    EPFDenyReason QueryPlacement(const FPFPlacementQuery& Q) const;
        // checks: phase (GameState->IsBuildAllowed), plot ownership (Q.Team), height cap, slot
        // occupancy (structural), AABB overlap (props), anchor rule, spawn-column exclusion,
        // neutral strip. Does NOT check budget/rate (server-only, in TryPlacePiece).

    // ---- Server-only mutation (called from UPFBuildComponent server RPCs) ----
    EPFDenyReason TryPlacePiece(class ACombatForgePlayerState* Placer, const FPFPlacementQuery& Q,
                                uint16& OutPieceId);
        // QueryPlacement + Placer budget spend (ServerTrySpendBudget) + 10/s/player rate cap
        // (RateLimited) + assigns PieceId + MarkItemDirty. Never trusts client Team/Owner — reads
        // them from Placer.
    EPFDenyReason TryDeletePiece(class ACombatForgePlayerState* Requester, uint16 PieceId);
        // exists? same team? phase Build? → refund ORIGINAL builder via
        // GameState->FindPlayerByRosterIndex(rec.OwnerIdx)->ServerRefundBudget (T19), remove +
        // MarkArrayDirty
    void FreezeBuild();          // Build→Combat: rejects all further mutation (belt+braces w/ phase gate)
    void ClearAll();             // Lobby→Build (rematch): wipes array + ISMs + occupancy
    const TArray<FPFBuildPieceRec>& GetPieces() const;   // fingerprint/serialization input

    // ---- Lookup for delete-tool highlight (client) ----
    // ISM hit → PieceId: per-ISMC TMap<int32 InstanceIndex, uint16 PieceId> kept in sync (intra).
    bool FindPieceByHit(const FHitResult& Hit, uint16& OutPieceId, FPFBuildPieceRec& OutRec) const;

    // (intra) occupancy maps — TMap<FIntVector,uint16> FloorSlots, InclineSlots (ramp XOR roof);
    // TMap<FIntVector4-equivalent,uint16> WallEdges keyed on canonical (cellX,cellY,level,N|E);
    // prop AABB list. Rebuilt on client from FastArray callbacks (server remains the judge).
};
```

#### `FPFGridMath` — header-only statics (`PFGridMath.h`)

```cpp
struct COMBATFORGE_API FPFGridMath   // all pure statics; NO state. The ONLY snap math in the game.
{
    // World → grid (03 §4 quantize rules; field origin = world origin):
    static FIntVector WorldToCell(const FVector& P);              // floor(P.xy/400), level = clamp(round(P.z/300),0,3)
    static FIntVector WorldToSubGrid(const FVector& P);           // round(P/100)
    static void  SnapWall(const FVector& P, int32& CellX, int32& CellY, int32& Level, uint8& EdgeNE);
        // nearest of 4 edges from frac(P.xy/400); canonicalizes S/W → neighbor's N/E (03 §2);
        // level clamp 0..2 (level-3 wall breaches cap)
    static uint8 RampYawFromCamera(float CameraYawDeg, uint8 ROffset);  // low edge nearest player + R
    // Grid → world transform for rendering/collision (single source of truth for ISM instances AND ghost):
    static FTransform PieceLocalTransform(EPFPieceType Type, int16 X, int16 Y, int16 Z, uint8 Rot);
        // Wall: cube (4.0, 0.2, 3.0) on edge; Floor: (4.0, 4.0, 0.2) top at level Z (T25);
        // Ramp: plank (5.0, 4.0, 0.2) pitched -36.87°, low edge at level base; Roof: cone (4.0, 4.0, 1.5);
        // Can: cyl (1.2, 1.2, 2.2); Dorito: cone (2.4, 2.4, 2.0); Snake: cube (4.0, 1.2, 1.2)
    static bool  PieceAABB(EPFPieceType Type, int16 X, int16 Y, int16 Z, uint8 Rot, FBox& Out); // sub-grid ints
    static bool  IsInsideTeamPlot(EPFPieceType Type, int16 X, int16 Y, uint8 Team);             // PFGrid columns
    static int16 SupportTopSubZ(const UWorld* World, const FVector& XY);  // prop Z: terrain 0 or highest floor top (300-multiples, T25)
};
```

#### `UPFBuildComponent : UActorComponent` — build input, ghost, RPCs

```cpp
UCLASS()
class COMBATFORGE_API UPFBuildComponent : public UActorComponent
{
    GENERATED_BODY()
public:
    UPFBuildComponent();   // SetIsReplicatedByDefault(true) — carries RPCs

    // Called by ACombatForgeCharacter::SetupPlayerInputComponent (§3.3) — binds ALL IMC_Build actions:
    void BindInput(class UEnhancedInputComponent* EIC, const class UPFInputConfig* Cfg);

    // ---- Equip state (client-local) ----
    void EquipTool(EPFBuildTool Tool);         // also wheel-selection entry point (RootHUD wires it)
    EPFBuildTool GetEquippedTool() const;
    // Q-tap = EquipTool(LastUsedPiece) — session default Wall (03 §3). Mouse wheel cycles
    // Wall→…→Snake→Delete→Wall. R: ramp offset resets after placement/switch; prop offset persists.

    // ---- RPC surface (binding) ----
    UFUNCTION(Server, Reliable) void ServerPlacePiece(EPFPieceType Type, int16 X, int16 Y, int16 Z, uint8 Rot);
        // server builds FPFPlacementQuery with Team from PlayerState → APFBuildGrid::TryPlacePiece;
        // deny → ClientPlaceDenied(reason)
    UFUNCTION(Server, Reliable) void ServerDeletePiece(uint16 PieceId);
    UFUNCTION(Client, Reliable) void ClientPlaceDenied(EPFDenyReason Reason);
        // broadcasts OnPlaceDeniedEvent → ghost flashes red 0.2 s + PlayDenied() (03 §4)

    // ---- UI subscription points ----
    FPFOnEquippedToolChanged  OnEquippedToolChangedEvent;
    FPFOnPlaceDenied          OnPlaceDeniedEvent;
    FPFOnBuildWheelRequested  OnBuildWheelRequestedEvent;  // Q-hold started/released → RootHUD opens/commits wheel

    // ---- Behavior contract (intra, but binding) ----
    // Active ONLY during BuildPhase (component checks GameState->IsBuildAllowed(); PC swaps IMCs).
    // Ghost (owning client, per tick): camera trace 1200 uu on PF_ECC_BuildTrace → anchor P = hit +
    //   normal×8 (else camera+fwd×800) → FPFGridMath quantize per piece → grid->QueryPlacement →
    //   opaque ghost mesh at PieceLocalTransform, green/red per T7; NoCollision, never replicated.
    // Delete tool: highlight aimed team piece orange via FindPieceByHit; LMB → ServerDeletePiece.
    // Turbo (03 §4): LMB held → place on snapped-slot change OR every 0.15 s; client self-cap 8 RPC/s
    //   (server hard-caps 10/s). Turbo-delete same cadence.
    // Placement while moving/jumping/sliding allowed; zero movement penalty. Pawn overlap ignored at
    //   validation; server depenetrates any overlapped pawn UPWARD onto the new piece top
    //   (place-and-eject, 03 §4) — implemented server-side right after TryPlacePiece succeeds.
};
```

#### `APFArenaShell : AActor` — static world geometry (T9, T23, T29)

```cpp
UCLASS()
class COMBATFORGE_API APFArenaShell : public AActor
{
    GENERATED_BODY()
public:
    APFArenaShell();  // bReplicates=true (existence only; geometry is constructor-built identically
                      // everywhere): field floor slab 6400×4000×30 top at Z=0, 4 perimeter walls
                      // h=1200, spawn-strip floor tiles team-tinted, midline posts (T23), warm-up pen
                      // 2000×2000 uu at Y = -3000 (south of field) with 12 dummy slots, directional
                      // light + skylight + fog spawned by GameMode alongside (02 §3.5)

    // GameMode (server) drives; clients mirror visibility via GameState phase delegate:
    void SetMidlineBarrierActive(bool bActive);   // BuildPhase: invisible blocker (Pawn + Paintball) ON

    // ---- Spawn transform providers (GameMode consumes; deterministic, index-stable) ----
    FTransform GetTeamSpawnTransform(uint8 TeamSide, int32 SlotIdx) const; // TeamSide = physical side 0/1
                                                                           // (side swap handled by GameMode)
    FTransform GetBuildStartTransform(uint8 Team, int32 SlotIdx) const;    // own-plot placement at Build start
    FTransform GetWarmupSpawnTransform(int32 SlotIdx) const;               // pen player spawns
    FTransform GetWarmupDummyTransform(int32 SlotIdx) const;               // pen dummy slots (T29)
};
```

#### `FPFArenaSerialization` — statics (`PFArenaSerialization.h`)

```cpp
struct COMBATFORGE_API FPFArenaSerialization
{
    // T27 fingerprints — SHA1 hex lowercase:
    static FString ComputeArenaId(const TArray<FPFBuildPieceRec>& Pieces);       // grid header + sorted (t,x,y,z,r,team)
    static FString ComputeHalfHash(const TArray<FPFBuildPieceRec>& Pieces, uint8 Team); // sorted (t,x,y,z,r)
    // Piece/grid block of the match JSON (B13). Vote/result blocks are pkg-meta's (§3.7 schema):
    static TSharedRef<FJsonObject> BuildLayoutJson(const TArray<FPFBuildPieceRec>& Pieces,
                                                   const FString& MatchId, int32 TeamSize,
                                                   const FDateTime& CreatedUtc);
        // emits: schema, game, matchId, createdUtc, teamSize, grid{...}, arenaId, halfHashA/B,
        // pieces[{id,t,x,y,z,r,own,team}] — field names EXACTLY per §3.7
};
```

### 3.6 pkg-ui classes

All widgets: tree built in `RebuildWidget()` (root set **before** `Super::RebuildWidget()` — 02 R3),
data wired in `NativeConstruct` (bind delegates), unbound in `NativeDestruct`. Instantiated only by
`UPFRootHUDWidget` / the PC on local controllers. No property bindings; no widget blueprints;
default engine Roboto text + `FSlateColorBrush` panels only.

```cpp
UCLASS() class COMBATFORGE_API UPFRootHUDWidget : public UUserWidget
{
    GENERATED_BODY()
public:
    // Created once by PC in BeginPlayingState. Owns a UWidgetSwitcher of phase panels + persistent
    // overlays (scoreboard, feedback). Switches on GameState OnPhaseChangedEvent.
    // WIRING DUTY (binding): BuildComponent->OnBuildWheelRequestedEvent → Wheel->Open()/CloseAndCommit();
    //   Wheel->OnToolSelectedEvent → BuildComponent->EquipTool(). Re-wires on pawn change
    //   (possession delegate) — widgets never cache pawn pointers across possession.
private:  // all UPROPERTY() TObjectPtr<...>, created in RebuildWidget:
    // LobbyPanel, BuildPanel(=UPFBuildHUDWidget), CombatPanel(=UPFCombatHUDWidget),
    // VotePanel, ResultsPanel, WheelWidget, FeedbackWidget, ScoreboardWidget
};

UCLASS() class COMBATFORGE_API UPFLobbyWidget : public UUserWidget
{
    GENERATED_BODY()
    // Roster rows from GameState->PlayerArray (poll 0.5 s — PlayerArray has no delegate), per-row:
    // name, team color chip, ready check. Host rows clickable → PC->ServerHostCycleTeam (T22).
    // Footer hints: "F = Ready", host: "Enter = Start". Tab-hold toggles cursor (PC input mode).
};

UCLASS() class COMBATFORGE_API UPFBuildHUDWidget : public UUserWidget
{
    GENERATED_BODY()
    // Bottom-right "▦ N/30  ◆ N/6" (PlayerState OnFlagsChangedEvent) + equipped-piece name
    // (OnEquippedToolChangedEvent) + phase countdown (poll GetPhaseTimeRemaining per tick; flash+tone
    // at 30 s / 10 s — T2) + per-team ready counts "Ready 3/4 — 2/4" + deny flash on OnPlaceDeniedEvent.
};

UCLASS() class COMBATFORGE_API UPFBuildWheelWidget : public UUserWidget
{
    GENERATED_BODY()
public:
    void Open();             // captures mouse to wheel cursor; camera frozen while open (03 §3)
    void CloseAndCommit();   // release: hovered sector → OnToolSelectedEvent; inside dead zone = cancel
    FPFOnToolSelected OnToolSelectedEvent;
    // 8×45° sectors clockwise from top: Wall, Floor, Ramp, Roof, Can, Dorito, Snake, Delete.
    // Dead zone r=90 px; select band 90–280 px; hovered 1.08× + brighten; center readout
    // "Wall — 23 left" (budget from PlayerState). Digits 1–8 = instant select via NativeOnKeyDown (T4).
    // Selection math (atan2 of accumulated mouse delta) in NativeTick. Client-only, zero replication.
};

UCLASS() class COMBATFORGE_API UPFCombatHUDWidget : public UUserWidget
{
    GENERATED_BODY()
    // Crosshair: 4 lines + dot; gap px = 6 + tan(spreadHalfAngle)/tan(FOV/2)·(ViewportW/2), polls
    //   Weapon->GetCurrentSpreadHalfAngleDeg() per tick; hidden in ADS → 2 px dot (04 §4).
    //   [ERRATUM 2026-07-10: original formula had a stray 40· factor (placed a 1.5° hip cone at
    //   ~770 px on 1080p — self-evidently wrong); ratified as true screen projection + 6 px base gap.]
    // Hopper "100/∞" (OnHopperChangedEvent), reload bar (OnReloadStateChangedEvent).
    // Round pips (first-to-4), round timer, alive counts per team, own HP as 3 paint dots
    //   (OnHPChangedEvent), elim feed last 4 lines (OnElimFeedChangedEvent),
    //   freeze/intermission/sudden-death banners (OnRoundStateChangedEvent).
};

UCLASS() class COMBATFORGE_API UPFCombatFeedbackWidget : public UUserWidget
{
    GENERATED_BODY()
    // Binds pawn's Weapon->OnHitConfirmedEvent → hitmarker (4 ticks, 0.15 s, elim variant ×1.4
    //   team-tinted) + CombatAudio->PlayHitmarker().
    // Binds Health->OnLocalPaintHitTakenEvent → damage arc (radius 140 px, 0.75 s fade, yaw from
    //   ShooterLoc vs camera) + mask splats: 2–3 blobs, edge-biased, 60–140 px, 0.85→0.35 opacity
    //   over 1 s, wipe over 6 s or on round reset, CAP 6 concurrent (04 §4).
    // "SPLATTED [name]" center text 0.9 s on own elim confirm.
};

UCLASS() class COMBATFORGE_API UPFVoteWidget : public UUserWidget
{
    GENERATED_BODY()
    // Step 1: 👍/👎 buttons (required; advances instantly). Step 2: 8 chips from
    // PFVoteCategories::All+HintText; click cycles neutral→liked→disliked→neutral; ≤4 total
    // selections enforced (T30); SUBMIT live immediately. 20 s countdown ring from
    // GetPhaseTimeRemaining. Timeout/submit → PC->ServerSubmitVote(...); no thumb at timeout =
    // Abstained. Exactly one submission per player (widget disables after send).
};

UCLASS() class COMBATFORGE_API UPFResultsWidget : public UUserWidget
{
    GENERATED_BODY()
    // Winner banner (TeamRoundWins), MVP = highest PlayerState MatchScore (elims tiebreak), thumb
    // tally "6👍 / 2👎" + top liked/disliked category from GameState VoteTally
    // (OnVoteTallyChangedEvent). Host-only "Return to Lobby" button → PC->ServerHostReturnToLobby.
};

UCLASS() class COMBATFORGE_API UPFScoreboardWidget : public UUserWidget
{
    GENERATED_BODY()
    // Hold-Tab overlay: rows per PlayerState (name, team, elims, deaths, score, alive dot).
    // Visibility driven by IA_Scoreboard from PC. Poll 0.5 s while visible.
};
```

### 3.7 pkg-meta: `UPFRatingSubsystem : UGameInstanceSubsystem` + the JSON schema (B13)

```cpp
UCLASS()
class COMBATFORGE_API UPFRatingSubsystem : public UGameInstanceSubsystem
{
    GENERATED_BODY()
public:
    // SERVER-ONLY lifecycle (host process). All no-ops on pure clients.
    void BeginMatchRecord(const FString& MatchId, const TArray<FPFBuildPieceRec>& FrozenPieces,
                          int32 TeamSize);
        // Build→Combat: calls FPFArenaSerialization (fingerprints + layout json), writes initial file
        // Saved/Arenas/arena_<UTCyyyyMMdd_HHmmss>_<matchId first 8 hex>.json
    void AddVote(const FPFVoteRecord& Vote, bool bBuiltHalfA);   // VotePhase, one per player (GameMode dedupes)
    void CommitMatchRecord(const FPFMatchResult& Result);
        // Vote→Results: appends result + votes blocks (rewrites the same file), clears state
    FString GetCurrentArenaId() const;   // empty outside an active record
};
```

**The match JSON (exact field names — pkg-building writes ①, pkg-meta writes ②③):**

```jsonc
{
  "schema": 1,                                    // ①
  "game": "CombatForge",                           // ①
  "matchId": "8f3a2c1e-....",                     // ① full GUID string
  "createdUtc": "2026-07-09T21:14:03Z",           // ① ISO-8601 Z
  "teamSize": 4,                                  // ①
  "grid": { "cellUU": 400, "subUU": 100, "wallH": 300, "cellsX": 16, "cellsY": 10, "levels": 4 }, // ①
  "arenaId":   "sha1-hex",                        // ① T27
  "halfHashA": "sha1-hex",                        // ①
  "halfHashB": "sha1-hex",                        // ①
  "pieces": [ { "id": 1, "t": 0, "x": 8, "y": 12, "z": 0, "r": 1, "own": 2, "team": 0 } ], // ① mirrors FPFBuildPieceRec
  "result": {                                     // ② from FPFMatchResult
    "winnerTeam": "A",                            //    "A" | "B" | "draw"
    "finalScore": "4-2", "roundsPlayed": 6, "suddenDeath": false, "matchDurationSec": 712
  },
  "votes": [                                      // ③ one per connected player
    { "voterId": "sha1-hex-of-install-guid",      //    T24 anonymized
      "voterTeam": "A", "voterBuiltHalf": "A",    //    B13 fields ("A"|"B")
      "voterWonMatch": true,                      //    false for everyone on draw
      "thumb": "up",                              //    "up" | "down" | "abstained"
      "liked": ["layout", "creativity"],          //    lowercase FName strings (T13), ≤4 combined
      "disliked": ["sightlines"] }
  ]
}
```

Schema rules: append-only evolution (never rename/renumber; bump `schema` on breaking change);
category strings never localized on disk; `arenaId` identical for identical rebuilds; a future
backend ingests this file unmodified.

---

## 4. Shared conventions

### 4.1 Source conventions (every file, every package)

- **First line of every .h/.cpp:** `// Copyright (c) 2026 Tom Chapman. All rights reserved.`
  (`Config/DefaultGame.ini` `CopyrightNotice` uses the same string without slashes.)
- **Include order in .cpp:** own header first, then module headers (`CombatForgeTypes.h` etc.), then
  engine headers. **Headers:** `#pragma once`, `CoreMinimal.h` first, `*.generated.h` LAST (§5).
- **IWYU strict:** headers include only what they use; **forward-declare every cross-package class
  in headers** (`class APFBuildGrid;`), include the real header only in the .cpp. This is
  load-bearing for the six-package parallel build and MSVC/clang portability (02 R8), not style.
- **Naming (02 D8):** framework big-six spell out `CombatForge...`; everything else `PF` prefix
  (`UPF`/`APF`/`FPF`/`EPF`). No new prefixes.
- **Log:** `CombatForgeLog` only (§3.1). Verbosity: `Log` = state transitions, `Verbose` = per-shot/
  per-placement, `Warning` = validation rejects, `Error` = contract violations.
- **`.editorconfig` (pkg-meta):** tabs (width 4) for C++ (UE convention), UTF-8, LF, final newline.
- **No platform-specific code**: no `__builtin_*`, no VLAs, `TEXT()` on every literal reaching
  TCHAR APIs, case-exact `#include` paths (02 R8).
- **Tunables:** gameplay numbers are `UPROPERTY(EditDefaultsOnly, Category="PF|...")` members with
  ctor defaults exactly as tabled below (so designers can tune in a data-only BP subclass later).
  Structural/grid constants are `constexpr` in `PFGrid` (shared math must not depend on instances).
  **Nobody re-invents a number: if it's not in this doc, it doesn't exist.**

### 4.2 Piece geometry table (single source: `FPFGridMath::PieceLocalTransform`)

| Piece | Engine mesh | Component scale | Dims (uu) | Snap | Rot semantics |
|---|---|---|---|---|---|
| Wall | Cube | (4.0, 0.2, 3.0) | 400×20×300 | cell edge + level (0..2) | `Rot`: canonical edge 0=N, 1=E |
| Floor | Cube | (4.0, 4.0, 0.2) | 400×400×20, **top at level Z** (T25) | cell + level | invariant (stored) |
| Ramp | Cube | (5.0, 4.0, 0.2) | 500×400×20 plank, pitch −36.87°, low edge at level base | cell + level (incline slot) | 0–3 yaw; resets after placement |
| Roof | Cone | (4.0, 4.0, 1.5) | r200 h150, base at level Z | incline slot (XOR ramp) | invariant (stored) |
| Can | Cylinder | (1.2, 1.2, 2.2) | r60 h220 | sub-grid center, support top | 0–3 yaw; persists |
| Dorito | Cone | (2.4, 2.4, 2.0) | r120 h200 | sub-grid, support top | 0–3 yaw; persists |
| Snake | Cube | (4.0, 1.2, 1.2) | 400×120×120 | sub-grid, support top | 0–3 yaw; persists |

Meshes/materials referenced ONLY via `ConstructorHelpers::FObjectFinder` in CDO ctors (02 D10):
`/Engine/BasicShapes/Cube.Cube`, `Sphere.Sphere`, `Cylinder.Cylinder`, `Cone.Cone`,
`/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial` (its `Color` vector param is the only
tint knob; verified by module-startup `ensureMsgf`).

### 4.3 The numbers table (contract values; owning class in parentheses)

**Match loop (ACombatForgeGameMode):** Lobby untimed, all-ready/force → 5 s countdown · Build 180 s
(warnings 30/10 s; both-teams-all-ready → 5 s countdown early end; unspent budget discarded) ·
Combat: freeze 5 s, round 90 s, intermission 7 s, first to 4, max 7 rounds; ≤2v2: first to 3, max 5,
60 s rounds (T15) · sudden death on post-max deadlock: HP 1, 60 s, tie = match draw · Vote 20 s
(early-advance when all voted) · Results 15 s → Lobby. Side swap every round: odd = home half.
Round win: enemy team eliminated, or more alive at 0:00; equal = draw round (no point).

**Scoring (T18, GameMode writes PlayerState):** elimination 100 · round survival 25 · round win 50
(every teammate) · MVP = highest MatchScore.

**Movement (UPFCharacterMovementComponent / ACombatForgeCharacter):** walk 600 · sprint 830 ·
crouch 300 · ADS ×0.55 · accel 4096 · ground friction 10 · braking 2500 · braking factor 1.0 ·
JumpZ 630 · gravity ×1.5 · air control 0.9 · perch 15 · jump buffer 0.1 s, no coyote · crouch
interp 0.2 s (88→58) · sprint-out 0.18 s · slide: enter ≥750, boost 1150, steer 15°/s, glide
0.35 s @ friction 0.6 → ramp to 8.0 over 0.5 s, end <320 / crouch-release / jump / 1.1 s, cooldown
0.5 s, slide-jump keeps velocity, downhill (slope dot < −0.15) extends glide · FOV 105 base / 70 ADS
(in 0.18 s, out 0.14 s, ease-out cubic) · sprint kick +6 (0.15 s) · slide kick +9 · zero head bob ·
landing dip shake on falls >300 uu.

**Marker (UPFWeaponComponent / APFPaintballProjectile):** 12 bps full-auto, remainder-carrying
accumulator, first shot instant · hopper 100 / reserve ∞ / reload 1.0 s, auto-on-empty, cancelled by
sprint/slide · projectile 10,000 uu/s, gravity ×0.35, life 2.0 s, radius 7 uu, no bounce/pen, no
inherited velocity · spread (half-angle °): ADS 0.25, hip 1.5, hip-moving (>50 % walk) 2.0, air
+1.5, slide +1.0, crouch ×0.8; ADS-transition lerps by ADS alpha · bloom +0.12/shot cap +1.8, decay
6°/s after 0.15 s, ADS applies bloom ×0.5 · damage body 1 / mask 2, HP 3, mask = impact Z ≥
capsule-top − 35 uu (T1) · corpse blocks paintballs 0.5 s · server validation: token bucket cap 3
refill 12/s, origin ≤150 uu from server muzzle, dir ≤4° from server view · muzzle: server = capsule
center +30 fwd +20 right −10 down; cosmetic = camera +20 fwd.

**Build (APFBuildGrid / UPFBuildComponent):** budgets 30 structural + 6 props per player, 100 %
refund to original builder, Build-phase-only mutation · ghost reach 1200 uu · turbo 0.15 s, client
cap 8/s, server cap 10/s/player · anchor rule: touches terrain or any structural piece; no collapse
propagation (floaters legal) · one Floor per (cell, level), one incline per (cell, level), one Wall
per canonical edge+level, props by AABB overlap · pawn place-and-eject upward · height cap 1200 uu.

**Cosmetics:** world splat pool 256, disc scale (0.4, 0.4, 0.02), jitter 0.8–1.3×, pending-splat
reconcile 75 uu · cosmetic projectile pool 64/client · mask splat cap 6 concurrent · elim feed cap
50 entries · warm-up pen 20×20 m, one dummy per player.

### 4.4 Replication mechanism summary (per system — from 02 §2.5, updated by B3/B8)

| System | Mechanism | Reliability |
|---|---|---|
| Phase / round state / timers | GameState props + OnRep; end-timestamps, never ticking counters | — |
| Piece place/delete request | Server RPC on `UPFBuildComponent` | Reliable |
| Placement denial | Client RPC (owning) | Reliable |
| Piece existence | `FPFBuildPieceArray` FastArray delta on `APFBuildGrid` (B8) | — |
| Fire request | `ServerFire` on `UPFWeaponComponent` | Reliable (revisit post-v1) |
| Remote shot FX / impact splats | NetMulticast on `UPFWeaponComponent` | **Unreliable** (cosmetic) |
| Hitmarker | `ClientHitConfirm` (owning) | Reliable |
| Victim hit feedback | `ClientPaintHitTaken` on `UPFHealthComponent` | Reliable |
| HP / bEliminated | Component props + OnRep | — |
| Elim feed / vote tally / scores | GameState arrays + OnRep (02 D11 — late-join correct) | — |
| Sprint/ADS/slide | CMC compressed flags in the move stream (02 D5) — never RPCs | — |
| Ready / vote / host actions | Server RPCs on PC | Reliable |

Law (02): **state → replicated property + OnRep; requests → Server RPC; pure cosmetics →
unreliable multicast. No gameplay truth travels only by multicast.**

### 4.5 Phase → input contexts / input mode (PC applies; server validation is the real gate)

| Phase | IMCs active | Input mode | Notes |
|---|---|---|---|
| Lobby | Common + Combat | Game (GameAndUI + cursor while Tab held) | warm-up pen fire live (T21); F ready; Enter host-start |
| Build | Common + Build | Game | weapons dead; build always-on (T4) |
| Combat/Freeze | Common + Combat | Game, move input ignored | look/ADS free; fire server-rejected |
| Combat/Live | Common + Combat | Game | dead players: spectate cycle on Fire/ADS |
| Combat/Intermission | Common | Game, move ignored | score strip |
| Vote | none | UIOnly + cursor | 20 s |
| Results | none | UIOnly + cursor | host button |

### 4.6 Collision channels (`Config/DefaultEngine.ini`, owned by pkg-core)

```ini
[/Script/Engine.CollisionProfile]
+DefaultChannelResponses=(Channel=ECC_GameTraceChannel1,DefaultResponse=ECR_Block,bTraceType=False,bStaticObject=False,Name="Paintball")
+DefaultChannelResponses=(Channel=ECC_GameTraceChannel2,DefaultResponse=ECR_Ignore,bTraceType=True,bStaticObject=False,Name="BuildTrace")
```

Usage: pieces/shell/pawns/dummies block Paintball; pieces + field floor block BuildTrace; pawns and
projectiles ignore BuildTrace; ghost meshes collide with nothing. Aliases `PF_ECC_Paintball` /
`PF_ECC_BuildTrace` in `CombatForgeTypes.h` — never write `ECC_GameTraceChannel1` in gameplay code.

---

## 5. UE 5.6 correctness notes (obey; violations are review-blockers)

1. **`TObjectPtr<T>`** for every UObject-typed `UPROPERTY` (members shown as raw `TObjectPtr` in §3
   are all `UPROPERTY()`). Raw pointers only for non-property locals/params.
2. **Replication registration:** any class with `Replicated`/`ReplicatedUsing` props MUST override
   `GetLifetimeReplicatedProps(TArray<FLifetimeProperty>&) const` and `DOREPLIFETIME(...)` /
   `DOREPLIFETIME_CONDITION(..., COND_OwnerOnly)` each one (hopper/reload are owner-only). Actors
   with replicated components: components call `SetIsReplicatedByDefault(true)` in their ctor —
   required for component RPCs and props to work at all.
3. **Constructor vs BeginPlay:** `CreateDefaultSubobject`, `ConstructorHelpers::FObjectFinder`,
   collision setup, and config defaults happen ONLY in constructors (CDO-time). No `GetWorld()`,
   no `NewObject`, no timers in ctors. Runtime object creation (`NewObject`, `SpawnActor`,
   `CreateWidget`) happens in `BeginPlay`/`OnPossess`/`SetupInputComponent` or later.
4. **Custom CMC swap:** `ACombatForgeCharacter(const FObjectInitializer& OI) :
   Super(OI.SetDefaultSubobjectClass<UPFCharacterMovementComponent>(ACharacter::CharacterMovementComponentName))`
   — the ONLY way to substitute the movement component.
5. **`*.generated.h` is the LAST include** in every UCLASS/USTRUCT header, and the file's own
   generated header name must match the file name exactly.
6. **`COMBATFORGE_API`** on every UCLASS/USTRUCT/exported free function. Single module today, but the
   editor target and any future module split depend on it; it costs nothing.
7. **Enhanced Input, native construction (02 R1 — the #1 bug source):** all `UInputAction`/
   `UInputMappingContext`/modifier/trigger objects are `NewObject`s created in
   `UPFInputConfig::Build` (outer = PC), reachable from `UPROPERTY(TObjectPtr)` members — otherwise
   GC kills input mid-match. Apply contexts via `UEnhancedInputLocalPlayerSubsystem` in
   `OnPossess`/`BeginPlayingState` only on local controllers, null-checked, with retry — the
   subsystem may not exist at first call on clients. Never rebuild objects on phase change; only
   `AddMappingContext`/`RemoveMappingContext`.
8. **C++ UMG (02 R3):** construct the tree with `WidgetTree->ConstructWidget<>` inside
   `RebuildWidget()`, assign `WidgetTree->RootWidget` BEFORE `return Super::RebuildWidget()`. Bind
   data in `NativeConstruct`, unbind in `NativeDestruct` (`Delegate.RemoveAll(this)`). Never build
   the tree in `NativeConstruct`. `CreateWidget` only with a valid owning local PC.
9. **OnRep on listen host:** `OnRep_*` does NOT auto-fire where the property is set (the host).
   Every server-side setter in this contract manually invokes the OnRep/broadcast after mutating
   (§3.2 `ServerSet*` pattern). Clients-only logic inside OnReps must be guarded accordingly.
10. **OnRep single-shot tolerance (02 R4):** OnReps must produce correct results when called once
    with final state (join-in-progress) — no reliance on observing intermediate values.
11. **FastArray:** `MarkItemDirty(rec)` after add/change, `MarkArrayDirty()` after remove;
    `TStructOpsTypeTraits` `WithNetDeltaSerializer = true` (§3.5) or nothing replicates. Client
    visuals ONLY from `PostReplicatedAdd`/`PreReplicatedRemove` — never from RPC side channels.
12. **Movement prediction (02 R6):** speed changes ONLY inside `GetMaxSpeed()`; slide/sprint/ADS
    state ONLY via compressed flags + `FSavedMove_PF` (implement `Clear`, `GetCompressedFlags`,
    `CanCombineWith`, `SetMoveFor`, `PrepMoveFor` — all five). Anything outside the move stream
    rubber-bands and is a bug by definition. Test with `p.NetShowCorrections 1` + `Net PktLag=100`.
13. **Authority guards:** every gameplay mutation behind `HasAuthority()`; owner-only cosmetics
    behind `IsLocallyControlled()`; UI creation behind `IsLocalController()`. Server RPC bodies
    (`_Implementation`) re-validate everything — client-side checks are UX, server checks are law.
14. **`FVector_NetQuantize`/`NetQuantize100`/`NetQuantizeNormal`** in RPC payload structs as spec'd
    in §3.1 — do not "upgrade" them to FVector.
15. **Deterministic shared RNG (B3):** client and server construct `FRandomStream` via
    `UPFWeaponComponent::MakeShotStream` and consume in identical order (one `VRandCone` per shot).
    Any extra stream pull on one side desyncs every later shot — add nothing to this path.
16. **Timers & phase logic:** all match timing via `GetWorldTimerManager()` on the GameMode +
    replicated end-timestamps; clients NEVER run authoritative timers (02 R7).
17. **Windows portability (02 R8):** compile clean on clang/Mac before push; no MSVC/clang
    extensions; warnings-as-errors stays on; half a day is budgeted for the first PC compile —
    don't spend the budget on avoidable include-case or TEXT() misses.

— End of contract. Amendments require editing THIS file first; code follows the doc, never the
reverse.





