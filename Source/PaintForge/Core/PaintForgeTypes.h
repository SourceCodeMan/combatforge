// Copyright (c) 2026 Tom Chapman. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/ObjectMacros.h"
#include "Engine/EngineTypes.h"
#include "Engine/NetSerialization.h"
#include "Net/Serialization/FastArraySerializer.h"
#include "PaintForgeTypes.generated.h"

// The one shared header (contract §3.1). Types, constants, delegates only — no classes.
// Everyone includes this; it includes only engine headers.

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

// Build Mode = what the BUILD phase does (independent of the match type below).
UENUM(BlueprintType)
enum class EPFBuildMode : uint8
{
	Creative = 0,     // build your half from empty (v1)
	Improvement = 1,  // start from a community map, then a build pass on top   [wiring pending]
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

UENUM()
enum class EPFThumbVote : uint8 { Abstained = 0, Up = 1, Down = 2 };

// ---- Replication / gameplay structs ----
USTRUCT()
struct PAINTFORGE_API FPFBuildPieceRec : public FFastArraySerializerItem
{
	GENERATED_BODY()
	UPROPERTY() uint16       PieceId  = 0;   // server-assigned, monotonic per match, never reused
	UPROPERTY() EPFPieceType Type     = EPFPieceType::Wall;
	UPROPERTY() int16        X        = 0;   // sub-grid units (100 uu). Structural: multiples of 4 (cell min-corner). Props: center.
	UPROPERTY() int16        Y        = 0;
	UPROPERTY() int16        Z        = 0;   // sub-grid units. Structural: level*3 (0/3/6/9). Props: support-top (T25 keeps it integral).
	UPROPERTY() uint8        Rot      = 0;   // 0-3 = 90° yaw steps. Walls: canonical edge (0=N, 1=E). Floor/Roof: stored but render-invariant.
	UPROPERTY() uint8        OwnerIdx = 0;   // roster index (APaintForgePlayerState::RosterIndex), for refunds/attribution
	UPROPERTY() uint8        Team     = 0;   // 0 = A, 1 = B
};

USTRUCT()
struct PAINTFORGE_API FPFShotPacket
{
	GENERATED_BODY()
	UPROPERTY() FVector_NetQuantize100    Origin = FVector::ZeroVector;
	UPROPERTY() FVector_NetQuantizeNormal Dir    = FVector::ForwardVector;
	UPROPERTY() uint32                    ShotIndex  = 0;   // per-weapon monotonic; spread-seed input (B3)
	UPROPERTY() float                     ClientTime = 0.f; // reserved for post-v1 rewind (04 §5.2)
};

USTRUCT()
struct PAINTFORGE_API FPFPaintHitInfo
{
	GENERATED_BODY()
	UPROPERTY() TObjectPtr<class APaintForgePlayerState> ShooterPS = nullptr; // server-side only; not for wire
	UPROPERTY() uint8                     ShooterTeam  = 0;
	UPROPERTY() FVector_NetQuantize       ImpactPoint  = FVector::ZeroVector;
	UPROPERTY() FVector_NetQuantizeNormal ImpactNormal = FVector::UpVector;
	UPROPERTY() EPFBodyRegion             Region       = EPFBodyRegion::Body;
	UPROPERTY() uint8                     Damage       = 1;    // 1 body, 2 mask (server fills)
	UPROPERTY() float                     ServerTime   = 0.f;
};

USTRUCT()
struct PAINTFORGE_API FPFElimEntry
{
	GENERATED_BODY()
	UPROPERTY() FString ShooterName;  UPROPERTY() uint8 ShooterTeam = 0;
	UPROPERTY() FString VictimName;   UPROPERTY() uint8 VictimTeam  = 0;
	UPROPERTY() float   ServerTime = 0.f;
};

USTRUCT()
struct PAINTFORGE_API FPFVoteTally   // replicated on GameState for the Results screen
{
	GENERATED_BODY()
	UPROPERTY() uint8 Up = 0;
	UPROPERTY() uint8 Down = 0;
	UPROPERTY() uint8 Abstained = 0;
	UPROPERTY() uint8 LikedCounts[8]    = {0};   // index = category ID - 1
	UPROPERTY() uint8 DislikedCounts[8] = {0};
};

USTRUCT()
struct PAINTFORGE_API FPFVoteRecord  // server-side aggregation shape (rating subsystem input)
{
	GENERATED_BODY()
	UPROPERTY() FString       VoterGuidHash;         // SHA1-hex of install GUID (T24)
	UPROPERTY() uint8         VoterTeam = 0;
	UPROPERTY() EPFThumbVote  Thumb = EPFThumbVote::Abstained;
	UPROPERTY() TArray<uint8> LikedIds;              // category IDs 1..8, ≤4 total with DislikedIds (T30)
	UPROPERTY() TArray<uint8> DislikedIds;
};

USTRUCT()
struct PAINTFORGE_API FPFMatchResult
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
	PAINTFORGE_API extern const TArray<FName>  All;      // layout, cover, verticality, flow, balance, sightlines, creativity, pacing
	PAINTFORGE_API extern const TArray<FText>  HintText; // player-facing hints per 01 §4.1
	PAINTFORGE_API FName FromId(uint8 Id);               // 1..8, NAME_None otherwise
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
	constexpr int32 MaxRosterSlots = 12;   // full match roster / warmup pen slots (RosterIndex 0..11, 03 §3.2)
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
	PAINTFORGE_API FLinearColor ForTeam(uint8 Team);
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
