// Copyright (c) 2026 Tom Chapman. All rights reserved.

#include "Objectives/PFControlPointActor.h"

#include "CombatForge.h"
#include "Core/CombatForgePlayerState.h"
#include "Core/CombatForgeTypes.h"
#include "Player/CombatForgeCharacter.h"

#include "Components/SphereComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/TextRenderComponent.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Net/UnrealNetwork.h"
#include "UObject/ConstructorHelpers.h"

namespace
{
	constexpr float CaptureRadiusUU = 525.f;   // 3x the old visible pad (was 175 uu); the visual pad matches it
	// Engine cylinder is h=100,r=50. XY scale 10.5 → radius 525 uu (matches the capture zone); Z=0.12 → thin pad.
	const FVector PadScale(10.5f, 10.5f, 0.12f);
	const FLinearColor NeutralGray(0.45f, 0.45f, 0.5f);
}

APFControlPointActor::APFControlPointActor()
{
	PrimaryActorTick.bCanEverTick = false;

	bReplicates = true;
	SetReplicateMovement(false);
	bAlwaysRelevant = true;

	// Uniform-scale root: PadMesh (non-uniform PadScale), CaptureSphere, and the flag are SIBLINGS under it, so
	// the sphere's radius is a true world radius (attaching it to the squashed pad shrank it to ~1/8 before).
	SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("SceneRoot"));
	SetRootComponent(SceneRoot);

	static ConstructorHelpers::FObjectFinder<UStaticMesh> CylinderFinder(
		TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));

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

	// Flag marker (pole + cone flag) so the objective reads from across the arena. Team-colored in ApplyVisualState.
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
}

void APFControlPointActor::BeginPlay()
{
	Super::BeginPlay();
	ApplyVisualState();
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

uint8 APFControlPointActor::ServerQueryOccupancy(int32& OutA, int32& OutB) const
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
		const ACombatForgePlayerState* PS = Char->GetPlayerState<ACombatForgePlayerState>();
		if (!PS || !PS->bAliveInRound)
		{
			continue;
		}
		if (PS->TeamId == 0) { ++OutA; }
		else if (PS->TeamId == 1) { ++OutB; }
	}

	if (OutA > 0 && OutB == 0) { return 0; }
	if (OutB > 0 && OutA == 0) { return 1; }
	return 255; // empty or contested
}

void APFControlPointActor::OnRep_VisualState()
{
	ApplyVisualState();
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
	if (PadMID == nullptr && PadMesh->GetMaterial(0) != nullptr)
	{
		PadMID = PadMesh->CreateAndSetMaterialInstanceDynamic(0);
	}
	const FLinearColor Color = (ControllingTeam <= 1)
		? PFColors::ForTeam(ControllingTeam)
		: NeutralGray;
	if (PadMID)
	{
		PadMID->SetVectorParameterValue(TEXT("Color"), Color);
	}

	// Team-color the flag to match the pad (SetActorHiddenInGame above already hides the whole marker when inactive).
	if (FlagMID == nullptr && FlagMesh != nullptr && FlagMesh->GetMaterial(0) != nullptr)
	{
		FlagMID = FlagMesh->CreateAndSetMaterialInstanceDynamic(0);
	}
	if (FlagMID)
	{
		FlagMID->SetVectorParameterValue(TEXT("Color"), Color);
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
}
