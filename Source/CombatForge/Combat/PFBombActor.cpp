// Copyright (c) 2026 Tom Chapman. All rights reserved.

#include "Combat/PFBombActor.h"

#include "CombatForge.h"
#include "Building/PFBuildGrid.h"
#include "Combat/PFCombatAudio.h"
#include "Combat/PFHealthComponent.h"
#include "Combat/PFPaintballProjectile.h"
#include "Combat/PFSplatSubsystem.h"
#include "Combat/PFWeaponComponent.h"
#include "Core/CombatForgeGameState.h"
#include "Core/CombatForgePlayerState.h"
#include "Core/CombatForgeTypes.h"
#include "Player/CombatForgeCharacter.h"

#include "Components/StaticMeshComponent.h"
#include "Components/TextRenderComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Kismet/GameplayStatics.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Net/UnrealNetwork.h"
#include "TimerManager.h"
#include "UObject/ConstructorHelpers.h"

APFBombActor::APFBombActor()
{
	PrimaryActorTick.bCanEverTick = true;
	bReplicates = true;
	bAlwaysRelevant = true;   // countdown must be visible across the arena
	SetReplicatingMovement(false);   // placed once at arm; initial replication carries the location

	Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	SetRootComponent(Root);

	Mesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Mesh"));
	Mesh->SetupAttachment(Root);
	Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Mesh->SetCastShadow(false);
	static ConstructorHelpers::FObjectFinder<UStaticMesh> CylFinder(TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	if (CylFinder.Succeeded())
	{
		Mesh->SetStaticMesh(CylFinder.Object);
		Mesh->SetRelativeScale3D(FVector(0.35f, 0.35f, 0.22f));
	}

	CountdownText = CreateDefaultSubobject<UTextRenderComponent>(TEXT("Countdown"));
	CountdownText->SetupAttachment(Root);
	CountdownText->SetRelativeLocation(FVector(0.f, 0.f, 60.f));
	CountdownText->SetHorizontalAlignment(EHTA_Center);
	CountdownText->SetVerticalAlignment(EVRTA_TextCenter);
	CountdownText->SetWorldSize(42.f);
	CountdownText->SetTextRenderColor(FColor(255, 60, 40));
	CountdownText->SetText(FText::FromString(TEXT("BOMB")));
}

void APFBombActor::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(APFBombActor, PlanterTeam);
	DOREPLIFETIME(APFBombActor, DetonateServerTime);
	DOREPLIFETIME(APFBombActor, DefuseAccumSeconds);
	DOREPLIFETIME(APFBombActor, bDetonated);
	DOREPLIFETIME(APFBombActor, bArmed);
	DOREPLIFETIME(APFBombActor, BurstSeed);
}

void APFBombActor::BeginPlay()
{
	Super::BeginPlay();
	// Danger-red body via the verified BasicShapeMaterial "Color" param (module boot-checks it exists).
	if (Mesh)
	{
		if (UMaterialInstanceDynamic* MID = Mesh->CreateAndSetMaterialInstanceDynamic(0))
		{
			MID->SetVectorParameterValue(TEXT("Color"), FLinearColor(0.55f, 0.04f, 0.03f));
		}
	}
}

void APFBombActor::ServerArm(APFBuildGrid* Grid, uint16 PieceId, const FVector& WorldLoc,
                             uint8 InPlanterTeam, ACombatForgePlayerState* InPlanterPS)
{
	if (!HasAuthority() || bArmed)
	{
		return;
	}
	bArmed        = true;
	GridWeak      = Grid;
	TargetPieceId = PieceId;
	PlanterTeam   = InPlanterTeam;
	PlanterPS     = InPlanterPS;
	SetActorLocation(WorldLoc);

	const ACombatForgeGameState* GS = GetWorld() ? GetWorld()->GetGameState<ACombatForgeGameState>() : nullptr;
	DetonateServerTime = (GS ? GS->GetServerWorldTimeSeconds() : 0.f) + FuseSeconds;
	GetWorldTimerManager().SetTimer(FuseTimer, this, &APFBombActor::ServerDetonate, FuseSeconds, false);
	if (CountdownText)
	{
		CountdownText->SetTextRenderColor(PFColors::ForTeam(PlanterTeam % 2).ToFColor(true));
	}
	ForceNetUpdate();
	UE_LOG(CombatForgeLog, Log, TEXT("Bomb armed on piece #%u by team %u (%.0fs fuse)"),
		PieceId, static_cast<uint32>(PlanterTeam), FuseSeconds);
}

void APFBombActor::ServerSetDefuser(APawn* Defuser, bool bActive)
{
	if (!HasAuthority() || bDetonated)
	{
		return;
	}
	if (bActive)
	{
		if (DefuserWeak.Get() != Defuser)
		{
			DefuseAccumSeconds = 0.f;   // a takeover never inherits the previous holder's progress
		}
		DefuserWeak = Defuser;
		bDefuserHeld = true;
	}
	else if (DefuserWeak.Get() == Defuser)
	{
		bDefuserHeld = false;
		DefuserWeak.Reset();
		DefuseAccumSeconds = 0.f;   // continuous hold required — releasing resets progress
		ForceNetUpdate();
	}
}

void APFBombActor::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	UpdateLabel();

	if (!HasAuthority() || bDetonated || !bArmed)
	{
		return;
	}

	// Server: accumulate the continuous hold-F defuse, re-validating everything every frame.
	if (bDefuserHeld)
	{
		bool bValid = false;
		if (ACombatForgeCharacter* Defuser = Cast<ACombatForgeCharacter>(DefuserWeak.Get()))
		{
			const UPFHealthComponent* Health = Defuser->GetHealth();
			const ACombatForgePlayerState* PS = Defuser->GetPlayerState<ACombatForgePlayerState>();
			// ANYONE except the planter may defuse (review wf_e923820a): a team gate broke FFA entirely
			// (roster TeamIds > 1) and left a griefer's OWN team with zero counterplay against a bomb on
			// their own fort. The planter alone can never defuse their own bomb.
			bValid = (Health == nullptr || !Health->bEliminated)
				&& PS != nullptr && PS != PlanterPS.Get()
				&& FVector::DistSquared(Defuser->GetActorLocation(), GetActorLocation())
					<= FMath::Square(DefuseRangeUU);
		}
		if (bValid)
		{
			DefuseAccumSeconds += DeltaSeconds;
			if (DefuseAccumSeconds >= DefuseHoldSeconds)
			{
				ServerDefused();
			}
		}
		else if (DefuseAccumSeconds > 0.f)
		{
			DefuseAccumSeconds = 0.f;   // stepped away / died — progress resets
			ForceNetUpdate();
		}
	}
}

void APFBombActor::UpdateLabel()
{
	if (!CountdownText || bDetonated)
	{
		return;
	}
	const UWorld* World = GetWorld();
	const ACombatForgeGameState* GS = World ? World->GetGameState<ACombatForgeGameState>() : nullptr;
	if (PlanterTeam <= 1)
	{
		// Tint from the REPLICATED team here (not just in server-side ServerArm) so clients see it too.
		CountdownText->SetTextRenderColor(PFColors::ForTeam(PlanterTeam).ToFColor(true));
	}
	if (GS)
	{
		if (DefuseAccumSeconds > 0.05f)
		{
			CountdownText->SetText(FText::FromString(FString::Printf(
				TEXT("DEFUSING %d%%"),
				FMath::Clamp(FMath::RoundToInt(100.f * DefuseAccumSeconds / DefuseHoldSeconds), 0, 100))));
		}
		else
		{
			const int32 Secs = FMath::Max(0, FMath::CeilToInt(DetonateServerTime - GS->GetServerWorldTimeSeconds()));
			CountdownText->SetText(FText::FromString(FString::Printf(TEXT("BOMB  %d"), Secs)));
		}
	}
	// Face the local viewer so the countdown reads from any angle (cosmetic, per-machine).
	if (const APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr)
	{
		FVector CamLoc; FRotator CamRot;
		PC->GetPlayerViewPoint(CamLoc, CamRot);
		const FVector ToCam = CamLoc - CountdownText->GetComponentLocation();
		CountdownText->SetWorldRotation(ToCam.GetSafeNormal2D().Rotation());
	}
}

void APFBombActor::ServerDetonate()
{
	if (!HasAuthority() || bDetonated)
	{
		return;
	}
	// Seed BEFORE flipping bDetonated so the OnRep bunch carries a matching BurstSeed for client tracers.
	BurstSeed = static_cast<uint32>(FMath::Rand()) ^ (GetUniqueID() * 2654435761u);
	bDetonated = true;
	// Belt-and-braces phase guard: only remove the piece while the match is still in COMBAT. If a phase
	// transition raced the fuse (bombs are normally destroyed at Combat exit), detonating into a cleared /
	// rebuilt grid would delete a recycled-id piece that was never bombed.
	const ACombatForgeGameState* GS = GetWorld() ? GetWorld()->GetGameState<ACombatForgeGameState>() : nullptr;
	if (GS && GS->Phase == EPFMatchPhase::Combat)
	{
		if (APFBuildGrid* Grid = GridWeak.Get())
		{
			Grid->ServerRemovePieceForMatch(TargetPieceId);   // also clears the piece's bomb registry entry
		}
	}
	// Same radial BB burst as the frag grenade (cover blocks, teammates immune) — 300 pellets for a
	// breach charge so anyone camping the wall gets painted when it goes.
	const FVector At = GetActorLocation();
	SpawnFragBurst(At, BurstSeed);
	PlayDetonationLocal();   // host cosmetics/audio; clients replay via OnRep_Detonated
	ForceNetUpdate();
	SetLifeSpan(0.8f);       // linger long enough for the OnRep to land on clients
}

void APFBombActor::ServerDefused()
{
	if (!HasAuthority() || bDetonated)
	{
		return;
	}
	GetWorldTimerManager().ClearTimer(FuseTimer);
	if (APFBuildGrid* Grid = GridWeak.Get())
	{
		Grid->ClearPieceBomb(TargetPieceId);
	}
	UE_LOG(CombatForgeLog, Log, TEXT("Bomb on piece #%u defused"), TargetPieceId);
	Destroy();
}

void APFBombActor::OnRep_Detonated()
{
	if (bDetonated)
	{
		PlayDetonationLocal();
	}
}

void APFBombActor::PlayDetonationLocal()
{
	if (Mesh)          { Mesh->SetVisibility(false); }
	if (CountdownText) { CountdownText->SetVisibility(false); }
	const FVector At = GetActorLocation();
	// Host already has the authoritative BB spray as its visual; only remote clients need cosmetics.
	if (!HasAuthority())
	{
		SpawnCosmeticFragBurst(At, BurstSeed);
	}
	// Boom audio through the LOCAL player's combat audio (the grenade detonation pattern).
	const UWorld* World = GetWorld();
	APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
	ACombatForgeCharacter* LocalChar = PC ? Cast<ACombatForgeCharacter>(PC->GetPawn()) : nullptr;
	if (LocalChar)
	{
		if (UPFCombatAudio* Audio = LocalChar->GetCombatAudio())
		{
			Audio->PlayFragBurstAt(At);
		}
	}
}

void APFBombActor::SpawnFragBurst(const FVector& At, uint32 Seed)
{
	UWorld* World = GetWorld();
	if (World == nullptr || !HasAuthority())
	{
		return;
	}

	// Damage/splat routing needs a live weapon component (ResolveAuthoritativeImpact early-outs without
	// one). Prefer the planter's current pawn — after a mid-fuse respawn that is the new loadout weapon.
	UPFWeaponComponent* SrcWeapon = nullptr;
	APawn* PlanterPawn = nullptr;
	if (ACombatForgePlayerState* PS = PlanterPS.Get())
	{
		PlanterPawn = PS->GetPawn();
		if (ACombatForgeCharacter* Char = Cast<ACombatForgeCharacter>(PlanterPawn))
		{
			SrcWeapon = Char->GetWeapon();
		}
	}

	FRandomStream Stream(Seed);
	for (int32 i = 0; i < FragBBCount; ++i)
	{
		FVector Dir = Stream.VRand();
		if (Dir.Z < 0.f)
		{
			Dir.Z = -Dir.Z * 0.5f;   // bias lower hemisphere upward so BBs spray out, not into the floor
		}
		Dir = Dir.GetSafeNormal();

		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		Params.Owner = PlanterPawn ? static_cast<AActor*>(PlanterPawn) : this;
		Params.Instigator = PlanterPawn;
		APFPaintballProjectile* BB = World->SpawnActor<APFPaintballProjectile>(
			APFPaintballProjectile::StaticClass(), At, Dir.Rotation(), Params);
		if (BB != nullptr)
		{
			// Distinct high ShotIndex space so bomb hitmarkers don't collide with live-fire indices.
			BB->InitProjectile(At, Dir, PlanterTeam, /*bAuthoritative=*/true, SrcWeapon,
				Seed + static_cast<uint32>(i) + 1u);
		}
	}
}

void APFBombActor::SpawnCosmeticFragBurst(const FVector& At, uint32 Seed)
{
	UWorld* World = GetWorld();
	if (World == nullptr)
	{
		return;
	}
	UPFSplatSubsystem* Splats = World->GetSubsystem<UPFSplatSubsystem>();
	if (Splats == nullptr)
	{
		return;   // dedicated/non-rendering world
	}
	// SAME seed + SAME bias as SpawnFragBurst so cosmetic tracers line up with the authoritative BBs.
	// Cosmetic pool is 64 — extras re-use slots (same tradeoff as the frag grenade).
	FRandomStream Stream(Seed);
	for (int32 i = 0; i < FragBBCount; ++i)
	{
		FVector Dir = Stream.VRand();
		if (Dir.Z < 0.f)
		{
			Dir.Z = -Dir.Z * 0.5f;
		}
		Dir = Dir.GetSafeNormal();
		if (APFPaintballProjectile* Ball = Splats->AcquireCosmeticProjectile())
		{
			Ball->InitProjectile(At, Dir, PlanterTeam, /*bAuthoritative=*/false, /*SourceWeapon=*/nullptr,
				Seed + static_cast<uint32>(i) + 1u);
		}
	}
}

void APFBombActor::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	GetWorldTimerManager().ClearTimer(FuseTimer);
	Super::EndPlay(EndPlayReason);
}
