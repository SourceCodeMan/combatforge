# PaintForge — Technical Architecture (v1 Graybox)

**Doc:** 02-architecture.md · **Owner:** Lead engineering · **Engine:** Unreal Engine 5.6, C++-first · **Module:** `PaintForge` · **Target:** Win64 · **Status:** Locked for v1 build

This document is the coding spec for the v1 graybox milestone: server-authoritative multiplayer, zero editor-authored assets, single match loop `Lobby → BuildPhase → CombatPhase → VotePhase → Results`.

---

## 0. Top-level decisions (read first)

| # | Decision | Rationale (one line) |
|---|----------|----------------------|
| D1 | **Single persistent level + phase state machine** (GameMode drives, GameState replicates). No map travel between phases. | The arena the players build in BuildPhase *is* the combat arena — travel would destroy it; also avoids seamless-travel complexity and keeps PlayerState trivially persistent. |
| D2 | **One GameMode class** (`APaintForgeGameMode`) for the whole match. | Phases share 90% of rules; per-phase behavior is a `switch` on one enum, not four half-empty classes. |
| D3 | **Shooting is hitscan v1** (server line trace), with a client-side cosmetic tracer "projectile". | CoD-snappy feel is a locked requirement; real ballistic drop is a later realism knob, not a v1 need. |
| D4 | **Paint splats v1 = pooled squashed-sphere mesh splats** (engine sphere scaled to a 2 cm disc, MID team color), not decals. | Engine ships no parameterized decal-domain material and materials cannot be authored at runtime — mesh splats are guaranteed-shippable with zero assets. |
| D5 | **Sprint/slide/ADS replicate via custom CMC compressed flags** (`FSavedMove` pattern), not RPCs. | It's the only way movement-speed changes stay inside CMC prediction — RPC-set MaxWalkSpeed rubber-bands under latency. |
| D6 | **Building: client requests via reliable Server RPC → server validates → server spawns replicated `APFBuildPiece` → piece goes dormant.** Ghost preview is 100% client-local. | Server authority with zero per-frame cost for a static arena. |
| D7 | **Vote data persists as JSON files** under `Saved/ArenaRatings/` (not USaveGame). | The record shape is already backend-ready; JSON uploads to a future aggregation service without a translation layer. |
| D8 | **Naming:** framework classes spell out `PaintForge` (`APaintForgeGameMode`, `APaintForgeCharacter`); everything else uses the short `PF` prefix (`UPFBuildComponent`, `APFBuildPiece`). | Matches UE convention of long names on the framework "big six", keeps the other 30 classes readable. |
| D9 | **One editor-created empty map, `L_Graybox.umap`, is the sole content file.** It contains zero authored actors; the floor, spawn zones, lighting, and bounds are all spawned by C++ at `InitGame`. | A `.umap` is a container the engine requires (world settings, PIE entry point); every alternative (`/Engine/Maps/Entry`) couples us to engine-internal content that can change. Create it once: File → New Level → Empty Level → Save As. |
| D10 | **Engine assets are referenced with `ConstructorHelpers::FObjectFinder` in CDO constructors**, never lazy `LoadObject` at runtime. | Constructor-time references force the cooker to include the engine assets in a packaged build; runtime string loads silently fail after packaging. |
| D11 | **Eliminations feed and vote tallies replicate as arrays on GameState (OnRep)**, not multicasts. | Late joiners and reconnects get correct state for free; multicasts are fire-and-forget and lie to anyone who wasn't connected. |
| D12 | No OnlineSubsystem in v1. Listen server, connect by IP/`open <ip>` console command. | Steam/EOS sessions are a lobby-UX feature, not an architecture dependency; adding OSS later touches only the Lobby flow. |

---

## 1. Class map

### 1.1 Framework (the "big six")

| Class | Base | Responsibility |
|---|---|---|
| `UPaintForgeGameInstance` | `UGameInstance` | Process-lifetime state: local player identity (GUID generated on first run, persisted), pending host/join parameters, owns nothing match-scoped. |
| `APaintForgeGameMode` | `AGameModeBase` | **Server-only.** Phase state machine driver (timers, transition rules), spawns the runtime arena shell (floor, walls-of-the-world, team spawn zones), validates build/fire/vote requests delegated up from components, choose-spawn logic per team, elimination + respawn rules. |
| `APaintForgeGameState` | `AGameStateBase` | Replicated match truth: current phase enum + server phase-end timestamp, team scores, elimination feed array, vote tally array, arena piece count. Fires `OnPhaseChanged` delegate from OnRep for UI. |
| `APaintForgePlayerState` | `APlayerState` | Per-player replicated truth: `TeamId` (0/1), eliminations/times-eliminated, `BuildBudgetRemaining`, `bHasVoted`. |
| `APaintForgePlayerController` | `APlayerController` | Owns input setup (constructs native Enhanced Input objects via `UPFInputConfig`), owns the widget stack (creates `UPFRootHUDWidget`), carries the player-facing Server RPCs that aren't pawn-bound: `ServerSubmitVote`, `ServerSetReady`. Handles input-mode switches (game vs UI) on phase change. |
| `APaintForgeCharacter` | `ACharacter` | The pawn for Build and Combat phases (one pawn, two input contexts — no pawn swap). Composes `UPFCharacterMovementComponent`, `UPFWeaponComponent`, `UPFBuildComponent`, `UPFHealthComponent`. First-person camera (`UCameraComponent` at eye height, mesh hidden to owner). |

### 1.2 Movement

| Class | Base | Responsibility |
|---|---|---|
| `UPFCharacterMovementComponent` | `UCharacterMovementComponent` | Sprint / slide / ADS-walk speeds inside the prediction pipeline via compressed flags (`FLAG_Custom_0..2`) and a `FSavedMove_PF` subclass (see §2.4). Slide = timed crouch + forward impulse. |

### 1.3 Combat

| Class | Base | Responsibility |
|---|---|---|
| `UPFWeaponComponent` | `UActorComponent` | The paintball marker. Client: fires immediately (tracer, recoil, sound stub), enforces local fire rate, sends `ServerFire`. Server: re-validates rate + origin, line-traces, applies elimination, records splat. "Feels-unlimited ammo": 200-round hopper, auto-refills to full 1.5 s after last shot — no reload mechanic in v1. |
| `UPFHealthComponent` | `UActorComponent` | One-hit elimination (paintball rules): `bEliminated` replicated, server-only `Eliminate()`, ragdoll-free v1 (pawn hides + collision off), respawn handled by GameMode after `RespawnDelay=5s`. |
| `APFCosmeticTracer` | `AActor` | **Not replicated.** Client-local fast-moving glowing sphere (engine sphere, MID emissive team color) fired along the shot direction at 150 m/s so shots read as paintballs; destroyed on impact or 0.5 s. Spawned on the firing client immediately and on remote clients from the splat event. |
| `UPFSplatSubsystem` | `UWorldSubsystem` | **Client-side.** Pool of 256 splat mesh components (squashed engine spheres); `SpawnSplat(Location, Normal, TeamColor)` attaches/aligns one, recycles oldest when full. Purely cosmetic; driven by replicated hit events. |

### 1.4 Building

| Class | Base | Responsibility |
|---|---|---|
| `UPFBuildComponent` | `UActorComponent` | On-pawn build mode: toggles build/combat input contexts, runs the client-local ghost preview (grid snap, valid/invalid tint), piece selection + rotation, sends `ServerRequestPlacePiece` / `ServerRequestRemovePiece`. |
| `APFBuildPiece` | `AActor` | A placed wall/ramp/floor/prop. Static mesh (engine cube, scaled per type) + `FPFPieceInitState` replicated `COND_InitialOnly` (type, team, grid cell). `bReplicates=true`, `SetNetDormancy(DORM_DormantAll)` after spawn. Indestructible in v1. |
| `UPFBuildGridSubsystem` | `UWorldSubsystem` | Server-authoritative occupancy map (`TMap<FIntVector, APFBuildPiece*>`): validation queries (cell free, inside team half during BuildPhase, inside arena bounds, budget), registration/unregistration. Client instance mirrors from replicated pieces for snappier ghost tinting (server remains the judge). |
| `APFTeamSpawnZone` | `AActor` | Runtime-spawned (by GameMode) team spawn volume + no-build buffer; not replicated (server-only logic), visualized client-side by a color tile spawned per team from GameState data. |

**Grid + piece constants (v1):** cell = 250 uu (2.5 m). Wall 250×15×250, Floor 250×250×15, Ramp 250×250 rising 250, Prop/Bunker 125×125×125. Arena bounds 6000×3000×1500 uu (60×30×15 m). Build budget: 60 pieces per player. Yaw rotation in 90° steps only.

### 1.5 Voting & persistence

| Class | Base | Responsibility |
|---|---|---|
| `UPFArenaRatingSubsystem` | `UGameInstanceSubsystem` | Server-side collection of vote submissions, computes the arena fingerprint (SHA1 of the sorted piece list: type+cell+rot), writes one JSON record per match to `Saved/ArenaRatings/`, exposes `GetLocalHistory()` for a future "my rated arenas" screen. |

**Backend-ready record shape (one JSON file per match on the host):**

```json
{
  "schemaVersion": 1,
  "matchId": "8f3c…-guid",
  "arenaFingerprint": "sha1-hex",
  "timestampUtc": "2026-07-09T21:14:02Z",
  "pieceCount": 87,
  "pieces": [ { "t": "Wall", "c": [4, -2, 0], "r": 90, "team": 0 } ],
  "votes": [
    {
      "playerId": "guid",
      "thumbsUp": true,
      "liked": ["Layout", "Cover"],
      "disliked": ["Verticality"]
    }
  ]
}
```

Category IDs are a fixed `FName` set: `Layout, Cover, Verticality, Flow, Balance, Sightlines`. Never localize the stored IDs; localize only display text.

### 1.6 UI (all C++ UMG, zero widget blueprints)

| Class | Base | Responsibility |
|---|---|---|
| `UPFRootHUDWidget` | `UUserWidget` | Single widget added to viewport at possess; owns a `UWidgetSwitcher` of the per-phase panels below; switches on `OnPhaseChanged`. |
| `UPFLobbyWidget` | `UUserWidget` | Player list (from GameState PlayerArray), ready toggle, host "start match" button. |
| `UPFBuildHUDWidget` | `UUserWidget` | Budget remaining, selected piece name, grid crosshair, phase countdown. |
| `UPFBuildWheelWidget` | `UUserWidget` | Fortnite-style radial: hold key → wheel of piece types (drawn with UMG `UImage` slices + `UTextBlock`s laid out in a `UCanvasPanel`), release selects; pure C++ layout in `RebuildWidget`. |
| `UPFCombatHUDWidget` | `UUserWidget` | Crosshair, hopper count, team scores, phase timer, elimination feed (bound to GameState array OnRep delegate). |
| `UPFVoteWidget` | `UUserWidget` | Thumbs up/down buttons, then multi-select category checkboxes (`UCheckBox` per category), submit → `PC->ServerSubmitVote`. |
| `UPFResultsWidget` | `UUserWidget` | Winner banner, vote tally bars, "play again / return to lobby" buttons (host-only actions). |
| `UPFScoreboardWidget` | `UUserWidget` | Hold-Tab overlay listing PlayerStates. |

### 1.7 Input

| Class | Base | Responsibility |
|---|---|---|
| `UPFInputConfig` | `UObject` | Owns every native-constructed `UInputAction` and the three `UInputMappingContext`s (`IMC_Common`, `IMC_Combat`, `IMC_Build`) as `UPROPERTY` members (GC-rooted). Built once in `APaintForgePlayerController::SetupInputComponent`, outer = the PC. See §3.1. |

Total: **6 framework + 1 CMC + 4 combat + 4 building + 1 persistence subsystem + 8 widgets + 1 input config = 25 UCLASSes.** Anything not listed does not exist in v1.

---

## 2. Replication strategy

### 2.1 Where state lives

| State | Home | Why |
|---|---|---|
| Match phase, phase-end server time, team scores, elimination feed, vote tallies, piece count | `APaintForgeGameState` | Match-truth every client needs, always relevant, survives player churn. |
| Team, eliminations, build budget, has-voted | `APaintForgePlayerState` | Per-player, must survive pawn death/respawn. |
| Position/velocity/movement flags, bEliminated, hopper count | Pawn (+components) | Pawn-lifetime state; dies with the pawn, which is correct. |
| Ghost preview, selected piece, camera FOV, splat pool | Client-only, never replicated | Cosmetic/local intent; the server hears about it only via request RPCs. |

**Phase timer pattern:** GameState replicates `PhaseEndServerTime` (a `float` in server world seconds). Clients render `PhaseEndServerTime - GetServerWorldTimeSeconds()`. Never replicate a ticking countdown.

### 2.2 Building (client asks, server decides)

```
Client (owning)                    Server
──────────────                     ──────
Ghost preview (local tint)
ServerRequestPlacePiece(          ── reliable RPC on UPFBuildComponent
   EPFPieceType, FIntVector Cell,
   uint8 RotSteps)
                                   Validate: phase==Build, budget>0,
                                   cell free (GridSubsystem), in-bounds,
                                   in own team half, ≤20 requests/s
                                   ↓ pass
                                   SpawnActorDeferred<APFBuildPiece>
                                   fill FPFPieceInitState → FinishSpawning
                                   piece replicates to all; budget-- on PlayerState
                                   SetNetDormancy(DORM_DormantAll)
                                   ↓ fail
                                   ClientPlaceDenied(EPFDenyReason)  ── owning-client RPC
```

- The request sends a **grid cell, not a transform** — the server computes the transform. Clients can never place off-grid by construction.
- `FPFPieceInitState` (`type:uint8, team:uint8, cell:FIntVector, rot:uint8`) replicates `COND_InitialOnly` with `ReplicatedUsing=OnRep_InitState`; `OnRep_InitState` sets mesh scale + MID team tint on clients. Because it's set before `FinishSpawning`, it arrives in the actor's initial bunch — no two-step pop-in.
- Removal (own pieces only, BuildPhase only) mirrors the same pattern; server `Destroy()`s the piece, destruction replicates natively.

### 2.3 Shooting (fire locally, server validates)

v1-pragmatic, **no lag compensation, no rewind**:

1. **Client (owning), on trigger:** enforce local fire-rate gate (600 RPM = 100 ms), play fire feedback instantly (tracer via `APFCosmeticTracer`, camera kick, sound stub), decrement local hopper display, send `ServerFire(FVector_NetQuantize100 Origin, FVector_NetQuantizeNormal Dir)` — **reliable** RPC (at ≤10 Hz per player this is cheap; revisit to unreliable-with-sequence only if profiling says so).
2. **Server:** validate rate (per-player timestamp, tolerance 0.9× interval), validate `Origin` is within **150 uu** of the server's view location for that pawn (anti-teleport-fire), then `LineTraceSingleByChannel(ECC_GameTraceChannel1 /*Paintball*/)` from *server-accepted* origin along `Dir`, max range 15000 uu. Server trusts the client's aim direction (v1 tradeoff — acceptable; without rewind, tracing against server-side positions from a client ray is the standard budget answer).
3. **On hit pawn:** `UPFHealthComponent::Eliminate()` (server), which (a) appends `FPFEliminationEntry{Shooter, Victim, ServerTime}` to the GameState feed array, (b) sets victim `bEliminated` (OnRep hides pawn locally), (c) bumps PlayerState/team score.
4. **On any hit:** server appends `FPFSplatEvent{Loc(NetQuantize100), Normal(NetQuantizeNormal), Team}` and broadcasts `MulticastSplat` — **unreliable** NetMulticast (cosmetic only; a dropped splat is invisible damage nobody took). Receiving clients call `UPFSplatSubsystem::SpawnSplat`, and non-owning clients also spawn a cosmetic tracer from the shooter's muzzle so remote fire reads correctly.
5. **Misprediction handling:** the firing client shows its tracer/splat immediately; if the server disagrees (no elimination), nothing else happens — worst case the shooter saw a splat that didn't count. Explicitly accepted for v1.

**What we are NOT building in v1 (recorded so nobody "helpfully" adds it):** lag-compensated rewind hitboxes, projectile ballistics with drop, per-pellet spread patterns replicated, weapon inventory. Hitscan + generous paintball hit sphere (pawn capsule + 10 uu) is the graybox truth.

### 2.4 Movement (sprint / slide / ADS)

`UPFCharacterMovementComponent` + `FSavedMove_PF`:

- `FLAG_Custom_0` = WantsToSprint → `GetMaxSpeed()` returns 900 (vs 600 walk, 300 ADS).
- `FLAG_Custom_1` = WantsToSlide → on transition while sprinting: crouch + one forward impulse (1200 uu/s decaying over 0.8 s), implemented inside `PhysWalking`-adjacent logic so it's replayed identically in prediction and server sim.
- `FLAG_Custom_2` = WantsToADS → speed cap 300; FOV lerp (90→65 in 0.15 s) is client-cosmetic on the camera, not part of the move.
- `FSavedMove_PF` packs/unpacks the flags (`GetCompressedFlags`/`UpdateFromCompressedFlags`), `SetMoveFor`/`PrepMoveFor` copy the wants-bits, `CanCombineWith` rejects combining across flag changes.
- Jump and crouch use stock ACharacter paths (already prediction-safe).

This is the one deliberately "expensive-to-write" system in v1, because RPC-driven speed changes visibly rubber-band and would poison the CoD-feel requirement.

### 2.5 RPC vs OnRep — per system summary

| System | Mechanism | Reliability |
|---|---|---|
| Phase changes | `GameState` property + `OnRep_Phase` | replicated (reliable by nature) |
| Phase timer | replicated end-timestamp, client-derived countdown | — |
| Place/remove piece request | Server RPC on `UPFBuildComponent` | reliable |
| Placement denial feedback | Client RPC (owning) | reliable |
| Piece existence/init | Actor replication + `COND_InitialOnly` struct OnRep | — |
| Fire request | Server RPC on `UPFWeaponComponent` | reliable (revisit) |
| Splat + remote tracer | NetMulticast RPC | **unreliable** (cosmetic) |
| Elimination / feed | GameState replicated array OnRep | — (late-join correct) |
| bEliminated on pawn | pawn property OnRep | — |
| Sprint/slide/ADS | CMC compressed flags in the move stream | — |
| Vote submit | Server RPC on `APaintForgePlayerController` | reliable |
| Vote tallies (Results screen) | GameState replicated array OnRep | — |
| Ready-up (Lobby) | Server RPC on PC → PlayerState bool OnRep | reliable |

Rule of thumb enforced in review: **state → replicated property + OnRep; requests → Server RPC; pure cosmetics → unreliable multicast.** No gameplay truth ever travels only by multicast.

### 2.6 Relevancy & dormancy for build pieces

- Arena is ≤ 60 m across, so distance culling buys nothing: pieces keep default relevancy (`bAlwaysRelevant=false`, default `NetCullDistanceSquared` = 225 m — never triggers in-bounds).
- **Dormancy is the win:** every `APFBuildPiece` calls `SetNetDormancy(DORM_DormantAll)` after `FinishSpawning`. Pieces are immutable in v1, so after the initial bunch they cost the net driver *zero* per-tick consideration. If pieces ever become destructible, mutate → `FlushNetDormancy()` first.
- `NetUpdateFrequency = 1.0`, `SetReplicateMovement(false)` (transform ships in the spawn bunch; nothing moves).
- Late joiners get dormant actors' initial state automatically — dormancy does not hide actors from new connections.
- Burst control: with 8 players placing hard, worst-case ~160 spawns/s of tiny actors — well inside default 60 Hz listen-server budget; the per-client 20 req/s cap in §2.2 bounds it regardless.

### 2.7 Net driver settings

Listen server @ 60 Hz tick (`NetServerMaxTickRate=60` in DefaultEngine.ini), default replication graph OFF (stock net driver is correct at 2–8 players; RepGraph is a >50-actor-churn tool we don't need).

---

## 3. Zero-editor-asset strategy

### 3.1 Enhanced Input, constructed in C++

All input objects are plain `NewObject`s owned by `UPFInputConfig` (UPROPERTY-rooted, outer = PlayerController). Built in `SetupInputComponent`; contexts applied in `OnPossess` (the `UEnhancedInputLocalPlayerSubsystem` needs a valid LocalPlayer, which is not guaranteed earlier on clients).

```cpp
// UPFInputConfig::Build(APaintForgePlayerController* PC)
IA_Move = NewObject<UInputAction>(PC, TEXT("IA_Move"));
IA_Move->ValueType = EInputActionValueType::Axis2D;

IMC_Common = NewObject<UInputMappingContext>(PC, TEXT("IMC_Common"));

// W = +Y: swizzle YXZ so the 1D key value lands on the Y axis
FEnhancedActionKeyMapping& W = IMC_Common->MapKey(IA_Move, EKeys::W);
UInputModifierSwizzleAxis* Swz = NewObject<UInputModifierSwizzleAxis>(IMC_Common);
Swz->Order = EInputAxisSwizzle::YXZ;
W.Modifiers.Add(Swz);
// S = swizzle + UInputModifierNegate; A = negate; D = raw. Mouse: IA_Look with
// UInputModifierNegate (Y only) on EKeys::Mouse2D.
```

Binding: `Cast<UEnhancedInputComponent>(InputComponent)->BindAction(Cfg->IA_Fire, ETriggerEvent::Started, this, &ThisClass::OnFirePressed);`

**Contexts & priorities:** `IMC_Common` (prio 0: Move, Look, Jump, Sprint, Slide/Crouch, Scoreboard, ToggleBuildMode), `IMC_Combat` (prio 1: Fire, ADS), `IMC_Build` (prio 1: PlacePiece, RemovePiece, RotatePiece, CyclePiece, BuildWheel-hold). Phase/mode switches call `AddMappingContext`/`RemoveMappingContext` — never rebuild objects.

**Actions (complete v1 list, 15):** Move (Axis2D), Look (Axis2D), Jump, Sprint (hold), CrouchSlide (hold; slide if sprinting), Fire (hold, `UInputTriggerPulse` for auto at 100 ms), ADS (hold), ToggleBuildMode (Q), BuildWheel (hold Tab-adjacent key, default B), PlacePiece (LMB in build), RemovePiece (X), RotatePiece (R), CyclePiece (scroll), Scoreboard (hold Tab), MenuBack (Esc).

**GC warning (this is the #1 native-Enhanced-Input bug):** every `UInputAction`, `UInputMappingContext`, modifier, and trigger must be reachable from a `UPROPERTY()`. `UPFInputConfig` declares `UPROPERTY() TObjectPtr<UInputAction>` members for all 15 and `TObjectPtr<UInputMappingContext>` for all 3; modifiers/triggers are outered to their IMC (the IMC's mapping array holds them).

### 3.2 UMG in C++ (no widget blueprints)

Pattern — build the tree in `RebuildWidget()`, wire data in `NativeConstruct()`:

```cpp
TSharedRef<SWidget> UPFCombatHUDWidget::RebuildWidget()
{
    UCanvasPanel* Root = WidgetTree->ConstructWidget<UCanvasPanel>(
        UCanvasPanel::StaticClass(), TEXT("Root"));
    WidgetTree->RootWidget = Root;               // MUST be set before Super::

    Crosshair = WidgetTree->ConstructWidget<UTextBlock>();
    Crosshair->SetText(FText::FromString(TEXT("+")));
    UCanvasPanelSlot* Slot = Root->AddChildToCanvas(Crosshair);
    Slot->SetAnchors(FAnchors(0.5f, 0.5f));
    Slot->SetAlignment(FVector2D(0.5f, 0.5f));

    return Super::RebuildWidget();               // builds Slate from WidgetTree
}
```

- Instantiation: `CreateWidget<UPFRootHUDWidget>(PC, UPFRootHUDWidget::StaticClass())` → `AddToViewport()` in `APaintForgePlayerController::BeginPlayingState` (client-only guard: `IsLocalController()`).
- Fonts/brushes: default UMG text works out of the box (Roboto embedded in engine slate content); solid-color panels use `UImage` + `SetColorAndOpacity` / `FSlateColorBrush` — no texture assets needed.
- The build wheel is a `UCanvasPanel` with 6 text+background segments positioned by angle in C++; selection math (`atan2` of mouse delta) lives in `NativeTick`. Fancy radial materials come with the art pass, not v1.
- Data binding: no property bindings (BP-ism); widgets subscribe to delegates (`GameState->OnPhaseChanged`, `OnElimFeedChanged`) in `NativeConstruct`, unsubscribe in `NativeDestruct`.

### 3.3 Safe engine content, and how it's referenced

| Purpose | Path | Notes |
|---|---|---|
| Cube (walls, floors, ramps-as-wedged-cubes v1, bunkers) | `/Engine/BasicShapes/Cube.Cube` | 100 uu cube; scale per piece type. Ramps v1 = cube pitched 45° (a true wedge needs an authored mesh — later). |
| Sphere (splats, tracer) | `/Engine/BasicShapes/Sphere.Sphere` | Splat = scale (0.4, 0.4, 0.02). |
| Cylinder (marker barrel stub, if desired) | `/Engine/BasicShapes/Cylinder.Cylinder` | Optional cosmetics. |
| Parameterized base material | `/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial` | Has a **`Color`** vector parameter — the one engine material safely MID-tintable. Verified at startup (see below). |

Reference pattern (per D10 — constructor only):

```cpp
static ConstructorHelpers::FObjectFinder<UStaticMesh>
    CubeFinder(TEXT("/Engine/BasicShapes/Cube.Cube"));
MeshComp->SetStaticMesh(CubeFinder.Object);
```

All finders live in one place: `FPFEngineAssets` (static struct populated by the CDOs that use them). At module startup (`FPaintForgeModule::StartupModule`) an `ensureMsgf` verifies `BasicShapeMaterial` exposes the `Color` parameter, so an engine rename fails loudly on boot, not silently as white-on-white.

Team tinting everywhere: `UMaterialInstanceDynamic::Create(BasicShapeMaterial, this)` → `SetVectorParameterValue("Color", TeamColor)`. Team colors: Team 0 = `FLinearColor(0.05, 0.35, 1.0)` (blue paint), Team 1 = `FLinearColor(1.0, 0.25, 0.05)` (orange paint). Ghost preview tints: valid = team color @ 40% opacity flag via scalar? No — BasicShapeMaterial is opaque; ghost validity is shown by color (green `0.1,1,0.1` / red `1,0.1,0.1`) not translucency. Recorded as a graybox concession.

### 3.4 Paint splats with zero assets (D4 detail)

- **Chosen:** `UPFSplatSubsystem` pool of 256 `UStaticMeshComponent`s (engine sphere, scale 0.4×0.4×0.02 = ~40 cm disc, MID team color, `SetCollisionEnabled(NoCollision)`, `CastShadow=false`). On splat event: take next pool entry (round-robin recycle), set world location = hit + normal×1 uu, orient Z along the surface normal, random yaw + scale jitter (0.8–1.3×) so walls don't look tiled.
- **Rejected for v1 — decals:** `UDecalComponent` needs a decal-domain material; the engine's `DefaultDeferredDecalMaterial` has no exposed texture/color parameters, and UMaterials cannot be authored at runtime. A proper splat decal (SDF blob texture + parameterized decal material) is the *first* item in the PC art pass and drops into the same `SpawnSplat` seam — the subsystem API is designed so decals replace meshes without touching callers.
- Splats are cosmetic, client-side, and never replicated as actors (only the §2.3 unreliable splat event travels). Cap + pool means worst-case memory is fixed and there is nothing to clean up between phases except `ResetPool()` at phase change.

### 3.5 The runtime-spawned arena shell

`APaintForgeGameMode::InitGame` (server) spawns, in order: floor slab (cube scaled 60×30×0.3 m at Z=0), four boundary walls (5 m tall, cosmetic-blocking), two `APFTeamSpawnZone`s at the short ends, a `ADirectionalLight` + `ASkyLight` + `AExponentialHeightFog` (all spawnable natively, no assets), and sets `WorldSettings->KillZ = -1000`. Clients receive the floor/walls as replicated actors (they're `APFBuildPiece`s with `bSystemPiece=true`, exempt from budget/removal). One code path builds the world everywhere; `L_Graybox.umap` stays empty forever.

---

## 4. Module & project structure

### 4.1 Single runtime module

One runtime module `PaintForge` (+ auto editor target). No plugin split, no editor module in v1 — nothing needs editor customization.

```
PaintForge/
├── PaintForge.uproject          # EngineAssociation "5.6", module PaintForge, EnhancedInput plugin enabled (default in 5.6)
├── Source/
│   ├── PaintForge.Target.cs
│   ├── PaintForgeEditor.Target.cs
│   └── PaintForge/
│       ├── PaintForge.Build.cs
│       ├── PaintForge.h / .cpp              # module impl + startup asset verification
│       ├── Core/        # GameInstance, GameMode, GameState, PlayerState, PlayerController, EPFMatchPhase, PFTypes.h (shared structs/enums)
│       ├── Player/      # PaintForgeCharacter, PFCharacterMovementComponent, PFHealthComponent
│       ├── Combat/      # PFWeaponComponent, PFCosmeticTracer, PFSplatSubsystem
│       ├── Building/    # PFBuildComponent, PFBuildPiece, PFBuildGridSubsystem, PFTeamSpawnZone
│       ├── Voting/      # PFArenaRatingSubsystem, PFRatingTypes.h (JSON record structs)
│       ├── Input/       # PFInputConfig
│       └── UI/          # all 8 widgets
├── Content/
│   └── Maps/L_Graybox.umap      # the ONE content file (empty level, D9)
└── Config/
    ├── DefaultEngine.ini
    ├── DefaultGame.ini
    └── DefaultInput.ini
```

Rule: headers include only what they use (IWYU); forward-declare in headers, include in .cpp. This is load-bearing for risk R8 (MSVC vs clang), not style preference.

### 4.2 PaintForge.Build.cs

```csharp
public class PaintForge : ModuleRules
{
    public PaintForge(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicDependencyModuleNames.AddRange(new string[] {
            "Core", "CoreUObject", "Engine", "InputCore",
            "EnhancedInput",
            "UMG", "Slate", "SlateCore",
            "NetCore",                       // push-model / net serialization helpers
            "Json", "JsonUtilities"          // arena rating records
        });
        // No private-only deps in v1. NOT needed: OnlineSubsystem (D12),
        // Niagara (post-v1 art), GameplayAbilities (overkill), AIModule.
    }
}
```

Targets: `PaintForge.Target.cs` → `TargetType.Game`; `PaintForgeEditor.Target.cs` → `TargetType.Editor`; both `DefaultBuildSettings = BuildSettingsVersion.V5; IncludeOrderVersion = EngineIncludeOrderVersion.Unreal5_6;`.

### 4.3 Config files (the lines that matter)

**Config/DefaultEngine.ini**

```ini
[/Script/EngineSettings.GameMapsSettings]
EditorStartupMap=/Game/Maps/L_Graybox.L_Graybox
GameDefaultMap=/Game/Maps/L_Graybox.L_Graybox
GlobalDefaultGameMode=/Script/PaintForge.PaintForgeGameMode

[/Script/OnlineSubsystemUtils.IpNetDriver]
NetServerMaxTickRate=60

[/Script/Engine.RendererSettings]
r.DefaultFeature.MotionBlur=False        ; graybox readability; everything else default
```

**Config/DefaultInput.ini** — nothing. UE 5.6 already defaults `DefaultPlayerInputClass=EnhancedPlayerInput` and `DefaultInputComponentClass=EnhancedInputComponent`; we add ini lines only if a stale template overrides them.

**Config/DefaultGame.ini**

```ini
[/Script/EngineSettings.GeneralProjectSettings]
ProjectID=<generated>
ProjectName=PaintForge
CopyrightNotice=Copyright Tom Chapman 2026
```

**bUseUnity:** leave unity builds ON (default) for full-build speed; do not set `bUseUnity=false` project-wide. Discipline instead: IWYU-clean headers so the code also compiles non-unity (adaptive unity will bite otherwise on the file you're iterating on). If a missing-include bug ships, repro with `-DisableUnity` on the build command line rather than flipping the project default.

---

## 5. Windows dev loop (PC with the RTX 5090)

Checklist, in order:

1. **Visual Studio 2022** (17.10+, Community fine). Workloads: **Game development with C++** (ensure "Unreal Engine installer" unchecked, "Windows 11 SDK" + "MSVC v143" checked) and **.NET desktop development** (UBT/UHT tooling). Optional but recommended component: "MSVC v143 – Address Sanitizer".
2. **Epic Games Launcher → Unreal Engine 5.6.x** install (include "Engine Source" optional component OFF; debug symbols OFF to save 40 GB — add later if callstack digging is needed).
3. Clone repo (code authored on the Mac; the `.uproject`, `Source/`, `Config/`, `Content/Maps/` all in git). `.gitignore`: `Binaries/ DerivedDataCache/ Intermediate/ Saved/ .vs/ *.sln`.
4. Right-click `PaintForge.uproject` → **Generate Visual Studio project files**.
5. Open `PaintForge.sln` → configuration **Development Editor | Win64** → Build → set as startup project → F5 launches the editor.
6. First run: editor opens `L_Graybox`. **PIE net testing:** Editor Preferences → Play: Number of Players = 2–3, Net Mode = **Play as Listen Server**; periodically test with **Run Under One Process = OFF** (separate client processes surface real replication bugs one-process PIE hides).
7. Iteration: prefer full editor restart over Live Coding for anything touching headers/UPROPERTYs (Live Coding + reinstancing on native-only projects causes ghost state); Live Coding (Ctrl+Alt+F11) is fine for .cpp-only tweaks.
8. **Packaging (later, for the installable build):** Project Settings → Packaging → Use Pak File ✓, Use Io Store ✓ (defaults); List of maps to include: `L_Graybox`. Platforms → Windows → Package (Shipping). Engine assets referenced via `FObjectFinder` cook automatically (D10). Output folder zips + ships as v1's "installer"; NSIS/MSIX wrapper is a post-v1 nicety. Smoke-test a **Shipping** package on the first playable milestone, not the last week — packaging failures are the classic end-of-project landmine.

---

## 6. Risk register — top 8 first-compile / first-PIE breakers

| # | Risk | Symptom | Mitigation |
|---|------|---------|------------|
| R1 | **Native Enhanced Input objects garbage-collected or context applied too early** | Input works for 60 s then dies (GC), or clients spawn with dead controls | All IA/IMC objects behind `UPROPERTY(TObjectPtr)` in `UPFInputConfig` (D8, §3.1); `AddMappingContext` in `OnPossess`, guarded by `IsLocalController()` and a null-check on `UEnhancedInputLocalPlayerSubsystem`; retry via `BeginPlayingState` if subsystem not yet alive. |
| R2 | **Engine asset paths missing from cooked build** | Everything works in PIE, packaged build has invisible meshes / default-gray materials | `ConstructorHelpers::FObjectFinder` only (D10) — cooker follows CDO references. Startup `ensureMsgf` on every `FPFEngineAssets` entry + the `Color` param check so breakage is a boot-time log line. |
| R3 | **WidgetTree root not set before Slate build** | Blank/invisible widgets, or crash in `TakeWidget` | Canonical pattern in §3.2: construct tree and assign `WidgetTree->RootWidget` inside `RebuildWidget()` *before* `Super::RebuildWidget()`; never build the tree in `NativeConstruct`. |
| R4 | **Replicated actor state visible before init data arrives** | Build pieces flash white/unscaled on clients for a frame, or OnRep fires with garbage | `SpawnActorDeferred` + fill `FPFPieceInitState` + `FinishSpawning` (§2.2) so init state rides the initial bunch; OnRep functions must tolerate being called once with final state only. |
| R5 | **Listen-server host vs client asymmetry** | Feature works for the host, breaks for joining clients (or vice versa) — the classic | Host executes server+client paths in one process, so every feature is tested with a *second* PIE client from day one; all gameplay writes behind `HasAuthority()`; cosmetics keyed off OnRep/multicast (which also run on the host via direct calls). Add `IsLocallyControlled()` guards for owner-only cosmetics. |
| R6 | **Custom CMC saved-move bugs** | Sprint/slide rubber-banding, "moving in molasses", or server/client position fights | Implement `FSavedMove_PF` completely (`Clear`, `GetCompressedFlags`, `CanCombineWith`, `SetMoveFor`, `PrepMoveFor`) and the CMC's `UpdateFromCompressedFlags`; change speeds ONLY inside CMC (`GetMaxSpeed()` override), never by poking `MaxWalkSpeed` externally; test at `p.NetShowCorrections 1` + emulated 100 ms lag (`Net PktLag=100` in PIE net emulation) before calling it done. |
| R7 | **Phase state machine double-drives or skips on clients** | HUD stuck on BuildPhase, timers desync, vote screen never appears | Single writer: only `APaintForgeGameMode` mutates phase (server); clients react only in `OnRep_Phase`; host reacts via the same OnRep called manually after setting (standard listen-server OnRep-doesn't-fire-on-server gotcha). Phase timestamp pattern (§2.1) kills timer drift. |
| R8 | **Code authored on macOS/clang fails MSVC on the PC** | First Windows compile explodes with hundreds of errors | IWYU-strict headers (§4.1); no compiler extensions (VLA, statement expressions); `TEXT()` on every literal reaching TCHAR APIs; no case-sensitive include mismatches (clang on APFS is case-insensitive-ish too, but verify); avoid `__builtin_*`; keep `BuildSettingsVersion.V5` warnings-as-errors and fix warnings on Mac before pushing. Budget half a day for the first PC compile regardless. |

---

## 7. Phase state machine (reference)

```
                    all ready / host start
   Lobby ───────────────────────────────► BuildPhase (240 s)
                                              │ timer or all-players-done
                                              ▼
   Results ◄────────────── VotePhase (30 s) ◄── CombatPhase (300 s / score cap 25)
      │  host: rematch → BuildPhase (same teams, fresh arena)
      └─ host: return → Lobby
```

`EPFMatchPhase : uint8 { Lobby, BuildPhase, CombatPhase, VotePhase, Results }` lives in `Core/PFTypes.h`. GameMode transition function `SetPhase(EPFMatchPhase)` is the only mutator: it updates GameState, stamps `PhaseEndServerTime`, swaps default input contexts via a client-effecting OnRep, enables/disables fire (`CombatPhase` only) and building (`BuildPhase` only) at the **server validation layer** — UI/input gating is convenience, server checks are law.

Transition side effects (server): `BuildPhase→CombatPhase`: freeze grid, compute arena fingerprint, teleport all pawns to team spawns, reset eliminations. `CombatPhase→VotePhase`: disable damage, show vote UI (via phase OnRep). `VotePhase→Results`: `UPFArenaRatingSubsystem::CommitMatchRecord()` writes the JSON.
