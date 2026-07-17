// Copyright (c) 2026 Tom Chapman. All rights reserved.

#include "Objectives/PFControlPointActor.h"

#include "CombatForge.h"
#include "Core/CombatForgePlayerState.h"
#include "Core/CombatForgeTypes.h"
#include "Combat/PFHealthComponent.h"
#include "Player/CombatForgeCharacter.h"

#include "Components/SphereComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/TextRenderComponent.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Net/UnrealNetwork.h"
#include "UObject/ConstructorHelpers.h"
#include "UObject/SoftObjectPath.h"

namespace
{
	constexpr float CaptureRadiusUU = 525.f;   // 3x the old visible pad (was 175 uu); the visual pad matches it
	// Engine cylinder is h=100,r=50. XY scale 10.5 → radius 525 uu (matches the capture zone); Z=0.12 → thin pad.
	const FVector PadScale(10.5f, 10.5f, 0.12f);
	// Bright white-grey — ArenaMark multiplies BaseColor x0.55, so 0.45 rendered as mud; 0.85 reads "unclaimed".
	const FLinearColor NeutralGray(0.85f, 0.85f, 0.88f);
	const FLinearColor NeutralFlagRed(1.f, 0.05f, 0.05f);   // neutral zone flies a red "take me" cloth
	const FLinearColor PoleBlack(0.02f, 0.02f, 0.02f);
	constexpr int32 NumRingPillars = 12;
	constexpr int32 MaxCaptureContributors = 3;   // CoD: more teammates = faster, capped so a stack can't insta-cap
}

APFControlPointActor::APFControlPointActor()
{
	// Ticks only for the capture pulse; enabled on demand in ApplyVisualState.
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = false;

	bReplicates = true;
	SetReplicateMovement(false);
	bAlwaysRelevant = true;

	// Uniform-scale root: PadMesh (non-uniform PadScale), CaptureSphere, and the flag are SIBLINGS under it, so
	// the sphere's radius is a true world radius (attaching it to the squashed pad shrank it to ~1/8 before).
	SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("SceneRoot"));
	SetRootComponent(SceneRoot);

	static ConstructorHelpers::FObjectFinder<UStaticMesh> CylinderFinder(
		TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));

	// Guaranteed material fallback — hard CDO refs are only reliable for /Engine content (playbook §2);
	// the /Game masters soft-resolve in EnsureObjectiveMaterials once the world is up.
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> BaseMatFinder(
		TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
	if (BaseMatFinder.Succeeded())
	{
		FallbackBaseMaterial = BaseMatFinder.Object;
	}

	PadMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("PadMesh"));
	PadMesh->SetupAttachment(SceneRoot);
	PadMesh->SetRelativeScale3D(PadScale);
	PadMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	PadMesh->SetCastShadow(false);
	if (CylinderFinder.Succeeded())
	{
		PadMesh->SetStaticMesh(CylinderFinder.Object);
	}

	CaptureSphere = CreateDefaultSubobject<USphereComponent>(TEXT("CaptureSphere"));
	CaptureSphere->SetupAttachment(SceneRoot);   // sibling of the pad -> uniform scale, true 525 uu radius
	CaptureSphere->SetSphereRadius(CaptureRadiusUU);
	CaptureSphere->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	CaptureSphere->SetCollisionObjectType(ECC_WorldDynamic);
	CaptureSphere->SetCollisionResponseToAllChannels(ECR_Ignore);
	CaptureSphere->SetCollisionResponseToChannel(ECC_Pawn, ECR_Overlap);
	CaptureSphere->SetGenerateOverlapEvents(true);

	// Flag marker (pole + banner) so the objective reads from across the arena. Team-colored in ApplyVisualState.
	PoleMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("PoleMesh"));
	PoleMesh->SetupAttachment(SceneRoot);
	PoleMesh->SetRelativeScale3D(FVector(0.1f, 0.1f, 4.0f));   // thin, ~400 uu tall
	PoleMesh->SetRelativeLocation(FVector(0.f, 0.f, 200.f));   // base on the pad
	PoleMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	PoleMesh->SetCastShadow(false);
	if (CylinderFinder.Succeeded())
	{
		PoleMesh->SetStaticMesh(CylinderFinder.Object);
	}

	// Banner (flat team-colored quad hanging off the pole top) — was a cone, which read as a construction marker,
	// not a flag ("just a cone on a pole"). Matches the CTF flag's look.
	static ConstructorHelpers::FObjectFinder<UStaticMesh> CubeFinder(TEXT("/Engine/BasicShapes/Cube.Cube"));
	FlagMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("FlagMesh"));
	FlagMesh->SetupAttachment(SceneRoot);
	FlagMesh->SetRelativeScale3D(FVector(1.3f, 0.06f, 0.8f));   // ~130 wide x 6 thin x 80 tall
	FlagMesh->SetRelativeLocation(FVector(65.f, 0.f, 350.f));   // hangs off the pole top (+X), inner edge on the pole
	FlagMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	FlagMesh->SetCastShadow(false);
	if (CubeFinder.Succeeded())
	{
		FlagMesh->SetStaticMesh(CubeFinder.Object);
	}

	// Zone-boundary ring: 12 short posts around the capture radius so the zone EDGE reads in-world (players
	// kept guessing where the zone ended — BO6 marks its larger zones on the ground for the same reason).
	RingPillars.Reserve(NumRingPillars);
	for (int32 i = 0; i < NumRingPillars; ++i)
	{
		UStaticMeshComponent* P = CreateDefaultSubobject<UStaticMeshComponent>(
			*FString::Printf(TEXT("RingPillar%d"), i));
		P->SetupAttachment(SceneRoot);
		const float Rad = 2.f * PI * static_cast<float>(i) / static_cast<float>(NumRingPillars);
		P->SetRelativeLocation(FVector(FMath::Cos(Rad) * CaptureRadiusUU,
			FMath::Sin(Rad) * CaptureRadiusUU, 55.f));
		P->SetRelativeScale3D(FVector(0.14f, 0.14f, 1.1f));   // 14x14x110 posts
		P->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		P->SetCastShadow(false);
		if (CubeFinder.Succeeded())
		{
			P->SetStaticMesh(CubeFinder.Object);
		}
		RingPillars.Add(P);
	}

	// A/B/C letter above the flag, readable across the arena. Two back-to-back faces instead of billboarding
	// (text render's readable face is -X and the actor never ticks). Text is set in ApplyVisualState — the
	// PointIndex isn't known until ServerInit / replication.
	auto MakeLetter = [this](const TCHAR* Name, float Yaw) -> UTextRenderComponent*
	{
		UTextRenderComponent* T = CreateDefaultSubobject<UTextRenderComponent>(Name);
		T->SetupAttachment(SceneRoot);
		T->SetRelativeLocation(FVector(0.f, 0.f, 520.f));
		T->SetRelativeRotation(FRotator(0.f, Yaw, 0.f));
		T->SetHorizontalAlignment(EHTA_Center);
		T->SetVerticalAlignment(EVRTA_TextCenter);
		T->SetWorldSize(180.f);
		return T;
	};
	LetterFront = MakeLetter(TEXT("LetterFront"), 0.f);
	LetterBack  = MakeLetter(TEXT("LetterBack"), 180.f);
}

void APFControlPointActor::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(APFControlPointActor, PointIndex);
	DOREPLIFETIME(APFControlPointActor, ControllingTeam);
	DOREPLIFETIME(APFControlPointActor, bActive);
	DOREPLIFETIME(APFControlPointActor, CapturingTeam);
	DOREPLIFETIME(APFControlPointActor, CaptureProgress01);
	DOREPLIFETIME(APFControlPointActor, bContested);
}

void APFControlPointActor::BeginPlay()
{
	Super::BeginPlay();
	EnsureObjectiveMaterials();
	ApplyVisualState();
}

void APFControlPointActor::EnsureObjectiveMaterials()
{
	if (bTriedObjectiveMaterials)
	{
		return;
	}
	bTriedObjectiveMaterials = true;
	MarkMaterial = Cast<UMaterialInterface>(
		FSoftObjectPath(TEXT("/Game/Materials/M_PF_ArenaMark.M_PF_ArenaMark")).TryLoad());
	if (MarkMaterial == nullptr)
	{
		UE_LOG(CombatForgeLog, Warning,
			TEXT("[CP] M_PF_ArenaMark missing (run Scripts/gen_arena_materials.py) — pad/pillars/flag fall back to BasicShapeMaterial"));
	}
	MetalMaterial = Cast<UMaterialInterface>(
		FSoftObjectPath(TEXT("/Game/Materials/M_PF_ArenaMetal.M_PF_ArenaMetal")).TryLoad());
	if (MetalMaterial == nullptr)
	{
		UE_LOG(CombatForgeLog, Warning,
			TEXT("[CP] M_PF_ArenaMetal missing (run Scripts/gen_arena_materials.py) — pole falls back to BasicShapeMaterial"));
	}
	if (FallbackBaseMaterial == nullptr)
	{
		UE_LOG(CombatForgeLog, Warning,
			TEXT("[CP] BasicShapeMaterial fallback missing — objective meshes may render the default checker"));
	}
}

void APFControlPointActor::ServerInit(int32 InPointIndex, const FVector& WorldLoc, bool bInitiallyActive)
{
	if (!HasAuthority())
	{
		return;
	}
	PointIndex = InPointIndex;
	ControllingTeam = 255;
	bActive = bInitiallyActive;
	CapturingTeam = 255;
	CaptureProgress01 = 0.f;
	bContested = false;
	SetActorLocation(WorldLoc);
	SetActorHiddenInGame(!bActive);
	SetActorEnableCollision(bActive);
	ApplyVisualState();
	ForceNetUpdate();
}

void APFControlPointActor::ServerSetActive(bool bNewActive)
{
	if (!HasAuthority() || bActive == bNewActive)
	{
		return;
	}
	bActive = bNewActive;
	if (!bActive)
	{
		ControllingTeam = 255;
		CapturingTeam = 255;
		CaptureProgress01 = 0.f;
		bContested = false;
	}
	SetActorHiddenInGame(!bActive);
	SetActorEnableCollision(bActive);
	ApplyVisualState();
	ForceNetUpdate();
}

void APFControlPointActor::ServerSetControllingTeam(uint8 Team)
{
	if (!HasAuthority())
	{
		return;
	}
	const uint8 Clamped = (Team <= 1) ? Team : 255;
	if (ControllingTeam == Clamped)
	{
		return;
	}
	ControllingTeam = Clamped;
	ApplyVisualState();
	ForceNetUpdate();
}

bool APFControlPointActor::ServerTickCapture(int32 CountA, int32 CountB, float DeltaSeconds, float CaptureSeconds)
{
	if (!HasAuthority() || !bActive || CaptureSeconds <= 0.f)
	{
		return false;
	}

	const uint8 PrevOwner = ControllingTeam;
	const uint8 PrevCapper = CapturingTeam;
	const float PrevProgress = CaptureProgress01;
	const bool PrevContested = bContested;

	bContested = (CountA > 0 && CountB > 0);
	if (bContested)
	{
		// CoD: contested freezes the meter — no gain, no unwind, until one side clears the other out.
	}
	else if (CountA == 0 && CountB == 0)
	{
		// Empty: progress PERSISTS (Tom's spec — CoD slowly decays here; deliberately not copied).
	}
	else
	{
		const uint8 T = (CountA > 0) ? 0 : 1;
		const int32 N = FMath::Min((CountA > 0) ? CountA : CountB, MaxCaptureContributors);
		const float Step = (static_cast<float>(N) * DeltaSeconds) / CaptureSeconds;

		if (T == ControllingTeam)
		{
			// Owner standing on their own zone: wipes any enemy partial progress ("take it back").
			if (CapturingTeam != 255)
			{
				CaptureProgress01 = FMath::Max(0.f, CaptureProgress01 - Step);
				if (CaptureProgress01 <= 0.f)
				{
					CapturingTeam = 255;
				}
			}
		}
		else if (CapturingTeam == T)
		{
			// Continue this team's capture chain.
			CaptureProgress01 += Step;
			if (CaptureProgress01 >= 1.f)
			{
				if (ControllingTeam != 255)
				{
					// Stage 1 complete on an enemy-owned zone: NEUTRALIZED (owner income stops). Stage 2
					// (the actual capture) restarts the meter — enemy flags take 2x total, exactly CoD.
					ControllingTeam = 255;
					CaptureProgress01 = 0.f;
				}
				else
				{
					ControllingTeam = T;
					CapturingTeam = 255;
					CaptureProgress01 = 0.f;
				}
			}
		}
		else if (CapturingTeam == 255)
		{
			CapturingTeam = T;
			CaptureProgress01 = Step;
		}
		else
		{
			// Opposing team's stored progress unwinds before this team's own capture starts.
			CaptureProgress01 -= Step;
			if (CaptureProgress01 <= 0.f)
			{
				CapturingTeam = T;
				CaptureProgress01 = -CaptureProgress01;   // leftover step rolls into the new chain
			}
		}
	}

	const bool bOwnerChanged = (ControllingTeam != PrevOwner);
	if (bOwnerChanged || CapturingTeam != PrevCapper || bContested != PrevContested)
	{
		ApplyVisualState();
		ForceNetUpdate();
	}
	else if (CaptureProgress01 != PrevProgress)
	{
		ForceNetUpdate();   // progress-only change: HUD bars need it, visuals already pulse
	}
	return bOwnerChanged;
}

uint8 APFControlPointActor::ServerQueryOccupancy(int32& OutA, int32& OutB,
	TArray<ACombatForgePlayerState*>* OutOccupants) const
{
	OutA = 0;
	OutB = 0;
	if (!HasAuthority() || !bActive || !CaptureSphere)
	{
		return 255;
	}

	TArray<AActor*> Overlapping;
	CaptureSphere->GetOverlappingActors(Overlapping, ACombatForgeCharacter::StaticClass());
	for (AActor* Actor : Overlapping)
	{
		const ACombatForgeCharacter* Char = Cast<ACombatForgeCharacter>(Actor);
		if (!Char)
		{
			continue;
		}
		ACombatForgePlayerState* PS = Char->GetPlayerState<ACombatForgePlayerState>();
		// bAliveInRound isn't cleared on elimination in Skirmish; also reject eliminated-but-respawning
		// pawns so a corpse can't hold/contest a Domination point during its respawn window.
		const UPFHealthComponent* Health = Char->GetHealth();
		if (!PS || !PS->bAliveInRound || (Health && Health->bEliminated))
		{
			continue;
		}
		if (PS->TeamId == 0) { ++OutA; }
		else if (PS->TeamId == 1) { ++OutB; }
		else { continue; }
		if (OutOccupants != nullptr)
		{
			OutOccupants->Add(PS);
		}
	}

	if (OutA > 0 && OutB == 0) { return 0; }
	if (OutB > 0 && OutA == 0) { return 1; }
	return 255; // empty or contested
}

void APFControlPointActor::OnRep_VisualState()
{
	ApplyVisualState();
}

FLinearColor APFControlPointActor::CurrentTeamColor() const
{
	return (ControllingTeam <= 1) ? PFColors::ForTeam(ControllingTeam) : NeutralGray;
}

void APFControlPointActor::ApplyVisualState()
{
	if (PadMesh)
	{
		PadMesh->SetVisibility(bActive);
	}
	SetActorHiddenInGame(!bActive);

	if (!PadMesh)
	{
		return;
	}
	// All MIDs come from the BeginPlay-loaded MASTERS with an explicit SetMaterial —
	// CreateAndSetMaterialInstanceDynamic(0) re-parents to whatever already sits in the slot
	// (MID-of-MID, silently dead params; see 8f631b5) and trusts the engine checker default.
	EnsureObjectiveMaterials();
	UMaterialInterface* MarkMaster = MarkMaterial ? MarkMaterial.Get() : FallbackBaseMaterial.Get();
	if (PadMID == nullptr && MarkMaster != nullptr)
	{
		PadMID = UMaterialInstanceDynamic::Create(MarkMaster, this);
		PadMesh->SetMaterial(0, PadMID);
	}
	const FLinearColor Color = CurrentTeamColor();
	if (PadMID)
	{
		PadMID->SetVectorParameterValue(TEXT("Color"), Color);
	}

	// Flag cloth: bright RED while neutral (a "take me" marker readable across the arena), owner color once
	// captured (SetActorHiddenInGame above already hides the whole marker when inactive).
	if (FlagMID == nullptr && FlagMesh != nullptr && MarkMaster != nullptr)
	{
		FlagMID = UMaterialInstanceDynamic::Create(MarkMaster, this);
		FlagMesh->SetMaterial(0, FlagMID);
	}
	if (FlagMID)
	{
		FlagMID->SetVectorParameterValue(TEXT("Color"),
			(ControllingTeam <= 1) ? PFColors::ForTeam(ControllingTeam) : NeutralFlagRed);
	}

	// Pole: near-black metal. It previously got NO material at all — the checkerboard default slot in cooked builds.
	if (PoleMID == nullptr && PoleMesh != nullptr)
	{
		if (UMaterialInterface* PoleMaster = MetalMaterial ? MetalMaterial.Get() : FallbackBaseMaterial.Get())
		{
			PoleMID = UMaterialInstanceDynamic::Create(PoleMaster, this);
			PoleMID->SetVectorParameterValue(TEXT("Color"), PoleBlack);
			PoleMesh->SetMaterial(0, PoleMID);
		}
	}

	// Ring pillars share the ownership color; the capture pulse (Tick) brightens them toward the capper's color.
	if (PillarMIDs.Num() != RingPillars.Num())
	{
		PillarMIDs.SetNum(RingPillars.Num());
	}
	for (int32 i = 0; i < RingPillars.Num(); ++i)
	{
		UStaticMeshComponent* P = RingPillars[i];
		if (P == nullptr)
		{
			continue;
		}
		P->SetVisibility(bActive);
		if (PillarMIDs[i] == nullptr && MarkMaster != nullptr)
		{
			PillarMIDs[i] = UMaterialInstanceDynamic::Create(MarkMaster, this);
			P->SetMaterial(0, PillarMIDs[i]);
		}
		if (PillarMIDs[i])
		{
			PillarMIDs[i]->SetVectorParameterValue(TEXT("Color"), Color);
		}
	}

	// Point letter: index 0/1/2 → A/B/C, tinted with ownership like the pad + flag. PointIndex replicates with
	// the same OnRep as team/active, so every server/client refresh funnels through here.
	const FString Letter = FString::Chr(static_cast<TCHAR>(TEXT('A') + FMath::Clamp(PointIndex, 0, 2)));
	for (UTextRenderComponent* T : { LetterFront.Get(), LetterBack.Get() })
	{
		if (T != nullptr)
		{
			T->SetText(FText::FromString(Letter));
			T->SetTextRenderColor(Color.ToFColor(/*bSRGB=*/true));
			T->SetVisibility(bActive);
		}
	}

	// Pulse only while a capture chain is running (contested keeps the last pulse frame frozen — reads as "stuck").
	SetActorTickEnabled(bActive && CapturingTeam <= 1 && !bContested);
	if (CapturingTeam > 1)
	{
		PulsePhase = 0.f;
	}
}

void APFControlPointActor::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	// Capture pulse: pillars + pad throb between the ownership color and the CAPTURING team's color so an
	// in-progress take reads from across the map (CoD pulses its flag icons the same way).
	if (CapturingTeam > 1)
	{
		return;
	}
	PulsePhase += DeltaSeconds * 2.f * PI * 1.25f;   // ~1.25 Hz
	const float Alpha = 0.5f + 0.5f * FMath::Sin(PulsePhase);
	const FLinearColor Pulse = FMath::Lerp(CurrentTeamColor(), PFColors::ForTeam(CapturingTeam), Alpha);
	if (PadMID)
	{
		PadMID->SetVectorParameterValue(TEXT("Color"), Pulse);
	}
	for (UMaterialInstanceDynamic* MID : PillarMIDs)
	{
		if (MID != nullptr)
		{
			MID->SetVectorParameterValue(TEXT("Color"), Pulse);
		}
	}
}
