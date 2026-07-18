// Copyright (c) 2026 Tom Chapman. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/ObjectMacros.h"
#include "Engine/EngineTypes.h"
#include "Engine/NetSerialization.h"
#include "Net/Serialization/FastArraySerializer.h"
#include "CombatForgeTypes.generated.h"

// The one shared header (contract §3.1). Types, constants, delegates only — no classes.
// Everyone includes this; it includes only engine headers.

// ---- Enums ----
UENUM(BlueprintType)
enum class EPFMatchPhase : uint8 { Lobby = 0, Build = 1, Combat = 2, Vote = 3, Results = 4 };

UENUM(BlueprintType)
enum class EPFRoundState : uint8 { None = 0, Freeze = 1, Live = 2, Intermission = 3 };

// Fire selector on the marker. Client-local feel only — the server validates cadence (token bucket), not mode.
UENUM(BlueprintType)
enum class EPFFireMode : uint8 { Single = 0, Burst = 1, Auto = 2 };

// Throwables. Frag = non-lethal BB burst; Smoke = visual concealment.
UENUM(BlueprintType)
enum class EPFGrenadeType : uint8 { Frag = 0, Smoke = 1 };

UENUM(BlueprintType)
enum class EPFPieceType : uint8
{
	// Classic structural (0–3) + props (4–6) keep stable wire/disk IDs for existing arena JSON.
	Wall = 0, Floor = 1, Ramp = 2, Roof = 3,
	PropCan = 4, PropDorito = 5, PropSnake = 6,
	// Special structural pieces (runtime actors — window hole, doors, trap floor).
	WallWindow = 7, WallDoor = 8, WallDoorOneWay = 9, FloorTrap = 10,
	MAX_Count = 11 UMETA(Hidden)
};
// Props only — special pieces spend structural budget and occupy wall/floor slots.
FORCEINLINE bool PFIsProp(EPFPieceType T)
{
	return T == EPFPieceType::PropCan || T == EPFPieceType::PropDorito || T == EPFPieceType::PropSnake;
}
FORCEINLINE bool PFIsWallLike(EPFPieceType T)
{
	return T == EPFPieceType::Wall || T == EPFPieceType::WallWindow
		|| T == EPFPieceType::WallDoor || T == EPFPieceType::WallDoorOneWay;
}
FORCEINLINE bool PFIsFloorLike(EPFPieceType T)
{
	return T == EPFPieceType::Floor || T == EPFPieceType::FloorTrap;
}
FORCEINLINE bool PFIsSpecialBuildPiece(EPFPieceType T)
{
	return T == EPFPieceType::WallWindow || T == EPFPieceType::WallDoor
		|| T == EPFPieceType::WallDoorOneWay || T == EPFPieceType::FloorTrap;
}

UENUM(BlueprintType)  // client equip; place tools 0..10 static_cast to EPFPieceType (Delete is not a piece)
enum class EPFBuildTool : uint8
{
	Wall = 0, Floor = 1, Ramp = 2, Roof = 3,
	PropCan = 4, PropDorito = 5, PropSnake = 6,
	WallWindow = 7, WallDoor = 8, WallDoorOneWay = 9, FloorTrap = 10,
	Delete = 11
};

UENUM()
enum class EPFDenyReason : uint8
{
	None = 0, WrongPhase, OutOfBudget, SlotOccupied, Overlapping, OutOfPlot,
	NoAnchor, HeightCap, RateLimited, NotYourTeam, InvalidPiece, NotFound
	// SealsMap removed: full wall-off allowed; breach with the mid-field bomb.
};

// Locational hit model (Tom 2026-07-15): out at 3 head / 5 chest / 8 limb / 10 total hits.
// Numeric layout preserved from the old {Body, Mask, Legs} enum — no wire-format churn.
UENUM()
enum class EPFBodyRegion : uint8 { Chest = 0, Head = 1, Limbs = 2 };

UENUM()
enum class EPFRespawnMode : uint8 { RoundElimination = 0, Respawn = 1 };  // RoundElimination is v1 default; Respawn scores rounds by team tags

// Build Mode = what the BUILD phase does (independent of the match type below).
UENUM(BlueprintType)
enum class EPFBuildMode : uint8
{
	Creative = 0,     // build your half from empty (v1)
	Improvement = 1,  // load a saved community arena, then a full build pass on top
	PlayOnly = 2,     // no build phase — straight to combat
	MAX_Count = 3 UMETA(Hidden)
};

// Match Type = the objective / win condition of the combat (independent of the build mode).
UENUM(BlueprintType)
enum class EPFMatchType : uint8
{
	Elimination = 0,  // round-based, last team standing, first-to-N (v1)
	FreeForAll = 1,   // solo, most tags (play-only by nature); per-player TagCount
	Skirmish = 2,     // teams, most tags to a score (the non-lethal "TDM")
	CaptureFlag = 3,  // teams, first to N flag captures (TeamScores)
	Domination = 4,   // teams, hold 3 points — score over time (TeamScores)
	Hardpoint = 5,    // teams, rotating single point — score over time (TeamScores)
	MAX_Count = 6 UMETA(Hidden)
};

// Arena Map = the physical field the match plays on (the 4th host selector beside build mode /
// match type / format). Map identity travels as the spawned shell's ACTOR CLASS — this enum only
// replicates for UI seeding/labels (see FPFArenaMapDef below for the per-map parameters).
UENUM(BlueprintType)
enum class EPFArenaMap : uint8
{
	Warehouse = 0,    // 6400×4000 indoor CQB box (v1 arena)
	Yard = 1,         // 6400×8000 open-air yard beside the warehouse (double width, no roof)
	MAX_Count = 2 UMETA(Hidden)
};

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

/**
 * A player's full class kit: weapon choice + clothing part picks. Class configs live in each player's LOCAL
 * GameUserSettings, so the owning client pushes this up (ServerSetKit) and it replicates to every machine —
 * everyone sees your outfit, and the SERVER runs your weapon's authoritative stats (not the host's saved gun).
 */
USTRUCT()
struct COMBATFORGE_API FPFKitRep
{
	GENERATED_BODY()
	/** Primary weapon (hand when active; sling on back when secondary is drawn). */
	UPROPERTY() uint8 WeaponCategory = 0;
	UPROPERTY() uint8 WeaponIndex = 0;
	/** Secondary weapon (any catalog gun — sling on back when primary is drawn). Default = first pistol. */
	UPROPERTY() uint8 SecondaryCategory = 2;
	UPROPERTY() uint8 SecondaryIndex = 0;
	UPROPERTY() TArray<int16> CharParts;   // one per PFChar customization slot; -1 = none. Empty = not set yet.
};

USTRUCT()
struct COMBATFORGE_API FPFPaintHitInfo
{
	GENERATED_BODY()
	UPROPERTY() TObjectPtr<class ACombatForgePlayerState> ShooterPS = nullptr; // server-side only; not for wire
	UPROPERTY() uint8                     ShooterTeam  = 0;
	UPROPERTY() FVector_NetQuantize       ImpactPoint  = FVector::ZeroVector;
	UPROPERTY() FVector_NetQuantizeNormal ImpactNormal = FVector::UpVector;
	UPROPERTY() FName                     HitBone      = NAME_None;   // FHitResult.BoneName (usually None — capsule hit); server-side
	UPROPERTY() EPFBodyRegion             Region       = EPFBodyRegion::Chest;
	UPROPERTY() uint8                     Damage       = 1;    // every BB = 1 hit; lethality = per-region thresholds
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
	constexpr int32 CellsX = 16;       constexpr int32 CellsY = 10;   // CellsY = the DEFAULT (Warehouse) rows
	constexpr int32 MaxCellsY = 20;    // largest per-map row count (The Yard) — sizes fixed seal-check arrays
	// Vertical stack: Levels base heights at Z = 0, 300, … (Levels-1)*300. Walls use levels 0..Levels-2
	// (a wall on the top base would crown at HeightCap). Warehouse = 4 levels / 3 wall stories / 1200 cap
	// (fits under the roof). The Yard is open air and allows 7 levels / 6 wall stories / 2100 cap.
	constexpr int32 Levels = 4;        constexpr int32 HeightCapUU = 1200;
	constexpr int32 YardLevels = 7;    constexpr int32 YardHeightCapUU = 2100;
	constexpr int32 MaxLevels = YardLevels;   // largest per-map level count
	// Field-local cell columns (x): [0]=A spawn, [1..6]=A plot, [7..8]=neutral, [9..14]=B plot, [15]=B spawn
	constexpr int32 SpawnColA = 0;     constexpr int32 PlotAMinX = 1;  constexpr int32 PlotAMaxX = 6;
	constexpr int32 NeutralMinX = 7;   constexpr int32 NeutralMaxX = 8;
	constexpr int32 PlotBMinX = 9;     constexpr int32 PlotBMaxX = 14; constexpr int32 SpawnColB = 15;
	constexpr int32 SpawnPointsPerTeam = 6;
	constexpr int32 MaxRosterSlots = 12;   // full match roster / warmup pen slots (RosterIndex 0..11, 03 §3.2)
	constexpr float FieldOriginX = 0.f, FieldOriginY = 0.f;  // field corner at world origin (03 §1)
	constexpr float BuildReachUU = 1200.f;                    // ghost trace length
}

// ---- Per-map field parameters (task #40; per-map GRID rows since 2026-07-17) ----
// The build grid is 16 × CellsY per map (Warehouse 16×10, The Yard 16×20). CellsY is threaded through the
// grid math, objectives, seal-check, and the arenaId grid-header, so a map built on one grid REJECTS on the
// other (Tom's rule: Yard maps ≠ Warehouse maps). CellsX + all X-column math stay frozen 16-wide on every map.
struct COMBATFORGE_API FPFArenaMapDef
{
	EPFArenaMap MapId = EPFArenaMap::Warehouse;
	int32   CellsY = PFGrid::CellsY;    // build-grid rows for THIS map (Warehouse 10, Yard 20). FieldY = CellsY*CellUU.
	float   FieldX = static_cast<float>(PFGrid::CellsX * PFGrid::CellUU);   // 6400
	float   FieldY = static_cast<float>(PFGrid::CellsY * PFGrid::CellUU);   // 4000
	int32   Levels = PFGrid::Levels;           // vertical bases (Warehouse 4 = 3 wall stories; Yard 7 = 6)
	int32   HeightCapUU = PFGrid::HeightCapUU; // world-Z top of the build volume
	bool    bRoof = true;               // roof deck + trusses + skylights (Warehouse); false = open air
	bool    bPerimeter = true;          // arena walls + escape lid + wall dressing; false = wide-open field
	bool    bWarehouseScenery = false;  // Yard: non-enterable warehouse building beside the field
	float   SunIntensity = 6.f;         // per-map sun knob — exposure is LOCKED, this is the only safe dial
	FString Label;                      // player-facing card label ("WAREHOUSE" / "THE YARD")
};
COMBATFORGE_API const FPFArenaMapDef& PFGetArenaMapDef(EPFArenaMap Map);

// ---- Collision channels (must match DefaultEngine.ini, §4.6) ----
constexpr ECollisionChannel PF_ECC_Paintball  = ECC_GameTraceChannel1; // projectile sweep/blocking
constexpr ECollisionChannel PF_ECC_BuildTrace = ECC_GameTraceChannel2; // ghost placement trace

// ---- Team colors (02 §3.3) ----
namespace PFColors
{
	inline const FLinearColor TeamA{0.05f, 0.35f, 1.0f};   // blue paint
	inline const FLinearColor TeamB{1.0f, 0.25f, 0.05f};   // orange paint
	// Ghosts are 50% opaque so you can still read the grid / one-way door facing under them.
	inline const FLinearColor GhostValid{0.15f, 0.95f, 0.25f, 0.5f};
	inline const FLinearColor GhostInvalid{0.95f, 0.12f, 0.12f, 0.5f};
	inline const FLinearColor DeleteHighlight{1.0f, 0.55f, 0.1f, 0.5f};
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
DECLARE_MULTICAST_DELEGATE_OneParam(FPFOnFireModeChanged, EPFFireMode /*Mode*/);
DECLARE_MULTICAST_DELEGATE_TwoParams(FPFOnGrenadeCountChanged, uint8 /*Frag*/, uint8 /*Smoke*/);
DECLARE_MULTICAST_DELEGATE_ThreeParams(FPFOnLocalPaintHitTaken, FVector /*ShooterLoc*/, uint8 /*ShooterTeam*/, uint8 /*NewHP*/);
DECLARE_MULTICAST_DELEGATE_TwoParams(FPFOnEliminated, class UPFHealthComponent*, const FPFPaintHitInfo&); // server-side
DECLARE_MULTICAST_DELEGATE_OneParam(FPFOnHPChanged, uint8 /*NearestRemaining — legacy "HP" for feedback/stingers*/);
DECLARE_MULTICAST_DELEGATE_FourParams(FPFOnHitsChanged, uint8 /*Head*/, uint8 /*Chest*/, uint8 /*Limbs*/, uint8 /*Total*/);
DECLARE_MULTICAST_DELEGATE_OneParam(FPFOnEquippedToolChanged, EPFBuildTool);
DECLARE_MULTICAST_DELEGATE_OneParam(FPFOnPlaceDenied, EPFDenyReason);
DECLARE_MULTICAST_DELEGATE_OneParam(FPFOnBuildWheelRequested, bool /*bOpen*/);
DECLARE_MULTICAST_DELEGATE_OneParam(FPFOnToolSelected, EPFBuildTool);
DECLARE_MULTICAST_DELEGATE_OneParam(FPFOnSlideStateChanged, bool /*bSliding*/);
DECLARE_MULTICAST_DELEGATE_OneParam(FPFOnMantleStateChanged, bool /*bMantling*/);
