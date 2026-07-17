// Copyright (c) 2026 Tom Chapman. All rights reserved.

#include "Combat/PFBombActor.h"

#include "CombatForge.h"
#include "Building/PFBuildGrid.h"
#include "Building/PFGridMath.h"
#include "Combat/PFCombatAudio.h"
#include "Combat/PFHealthComponent.h"
#include "Combat/PFPaintballProjectile.h"
#include "Combat/PFSplatSubsystem.h"
#include "Combat/PFWeaponComponent.h"
#include "Core/CombatForgeGameState.h"
#include "Core/CombatForgePlayerState.h"
#include "Core/CombatForgeTypes.h"
#include "Player/CombatForgeCharacter.h"

#include "CollisionQueryParams.h"
#include "Components/StaticMeshComponent.h"
#include "Components/TextRenderComponent.h"
#include "Engine/OverlapResult.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Kismet/GameplayStatics.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Net/UnrealNetwork.h"
#include "TimerManager.h"
#include "UObject/ConstructorHelpers.h"
#include "UObject/SoftObjectPath.h"

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
	Mesh->SetCastShadow(true);
	// Hard CDO fallback only — SoftLoadMesh swaps in the Bandits grenade when content is present.
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
	DOREPLIFETIME(APFBombActor, BurstAxis);
}

void APFBombActor::BeginPlay()
{
	Super::BeginPlay();
	SoftLoadMesh();
}

void APFBombActor::SoftLoadMesh()
{
	if (!Mesh)
	{
		return;
	}
	// Planted charge = real bomb mesh from the Modern Weapons pack (Explosives), not a throwable grenade.
	// Fallbacks only if the pack is missing from Content/.
	const TCHAR* Paths[] = {
		TEXT("/Game/MarketplaceBlockout/Modern/Weapons/Assets/Explosives/01/SM_Modern_Weapons_Explosive_01.SM_Modern_Weapons_Explosive_01"),
		TEXT("/Game/MarketplaceBlockout/Modern/Weapons/Assets/Explosives/02/SM_Modern_Weapons_Explosive_02.SM_Modern_Weapons_Explosive_02"),
		TEXT("/Game/MarketplaceBlockout/Modern/Weapons/Assets/Explosives/03/SM_Modern_Weapons_Explosive_03.SM_Modern_Weapons_Explosive_03"),
		TEXT("/Game/Bandits/Mesh/Weapon/Hand_Granate/SM_Hand_Granate.SM_Hand_Granate"),
	};
	for (const TCHAR* Path : Paths)
	{
		if (UStaticMesh* M = Cast<UStaticMesh>(FSoftObjectPath(Path).TryLoad()))
		{
			Mesh->SetStaticMesh(M);
			// Scale so the tallest axis ≈ 70 uu — C4/charge sized, readable on a wall cell.
			const FBoxSphereBounds B = M->GetBounds();
			const float MaxDim = FMath::Max3(B.BoxExtent.X, B.BoxExtent.Y, B.BoxExtent.Z) * 2.f;
			const float Sc = 70.f / FMath::Max(MaxDim, 1.f);
			Mesh->SetRelativeScale3D(FVector(Sc));
			// Center the mesh on the bomb actor (piece AABB center) so dual-side burst still lines up.
			Mesh->SetRelativeLocation(FVector(-B.Origin.X * Sc, -B.Origin.Y * Sc, -B.Origin.Z * Sc));
			// Keep authored materials — no BasicShape "Color" override.
			UE_LOG(CombatForgeLog, Log, TEXT("Bomb: skinned with %s (scale %.2f)"), Path, Sc);
			return;
		}
	}
	// Cylinder fallback: danger-red via the verified BasicShapeMaterial "Color" param.
	if (UMaterialInstanceDynamic* MID = Mesh->CreateAndSetMaterialInstanceDynamic(0))
	{
		MID->SetVectorParameterValue(TEXT("Color"), FLinearColor(0.55f, 0.04f, 0.03f));
	}
	UE_LOG(CombatForgeLog, Warning,
		TEXT("Bomb: Modern Weapons Explosive meshes missing — using red cylinder fallback"));
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
	float FuseRemain = FuseSeconds;
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
			FuseRemain = FMath::Max(0.f, DetonateServerTime - GS->GetServerWorldTimeSeconds());
			const int32 Secs = FMath::CeilToInt(FuseRemain);
			CountdownText->SetText(FText::FromString(FString::Printf(TEXT("BOMB  %d"), Secs)));
		}
	}
	// Fuse timer spins a full 360° over the 15 s fuse (elapsed fraction of FuseSeconds).
	// Defusing freezes the spin so the player can still read the % label.
	if (DefuseAccumSeconds <= 0.05f)
	{
		const float Elapsed = FMath::Clamp(FuseSeconds - FuseRemain, 0.f, FuseSeconds);
		const float SpinYaw = (Elapsed / FuseSeconds) * 360.f;
		CountdownText->SetWorldRotation(FRotator(0.f, SpinYaw, 0.f));
	}
	else if (const APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr)
	{
		// While defusing: face the local viewer so the % reads cleanly.
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
	// Resolve face axis WHILE the piece still exists so dual-side origins match the real wall/floor plane.
	FVector Axis = FVector::UpVector;
	if (APFBuildGrid* Grid = GridWeak.Get())
	{
		FPFBuildPieceRec Rec;
		if (Grid->FindPieceById(TargetPieceId, Rec))
		{
			Axis = FaceAxisForPiece(Rec.Type, Rec.Rot);
		}
	}
	// Seed + axis BEFORE flipping bDetonated so the OnRep bunch carries matching dual-origin data for tracers.
	BurstSeed = static_cast<uint32>(FMath::Rand()) ^ (GetUniqueID() * 2654435761u);
	BurstAxis = Axis;
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
	// Proximity paint is instant (cheap). BB spray is multi-frame so detonation never hitch-spawns 1000 actors.
	const FVector At = GetActorLocation();
	ApplyProximityPaint(At);
	BeginFragBurst(At);
	PlayDetonationLocal();   // host cosmetics/audio; clients replay via OnRep_Detonated
	ForceNetUpdate();
	// Actor must outlive the batched burst (~13 frames) + OnRep delivery; Destroy when the spray finishes.
}

void APFBombActor::ServerDefused()
{
	if (!HasAuthority() || bDetonated)
	{
		return;
	}
	GetWorldTimerManager().ClearTimer(FuseTimer);
	GetWorldTimerManager().ClearTimer(BurstTimer);
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
		SpawnCosmeticFragBurst(At, BurstAxis, BurstSeed);
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

FVector APFBombActor::FaceAxisForPiece(EPFPieceType Type, uint8 Rot)
{
	switch (Type)
	{
	case EPFPieceType::Wall:
	case EPFPieceType::WallWindow:
	case EPFPieceType::WallDoor:
	case EPFPieceType::WallDoorOneWay:
		// N edge (rot 0): plane in XZ → both rooms along ±Y. E edge (rot 1): plane in YZ → ±X.
		return (Rot == 1) ? FVector(1.f, 0.f, 0.f) : FVector(0.f, 1.f, 0.f);
	case EPFPieceType::Floor:
	case EPFPieceType::FloorTrap:
	case EPFPieceType::Roof:
		return FVector(0.f, 0.f, 1.f);
	case EPFPieceType::Ramp:
	{
		// Ascent along Rot×90° (0=+X,1=+Y,2=-X,3=-Y). "Both sides" of the plank = horizontal
		// left/right of the run so rooms on either side of the ramp get hit.
		const FVector Along = FRotator(0.f, static_cast<float>(Rot) * 90.f, 0.f).Vector();
		const FVector Side(-Along.Y, Along.X, 0.f);
		return Side.GetSafeNormal();
	}
	default:
		return FVector(0.f, 0.f, 1.f);
	}
}

void APFBombActor::BeginFragBurst(const FVector& Center)
{
	if (!HasAuthority())
	{
		return;
	}
	BurstCenter = Center;
	BurstNextIndex = 0;
	BurstWeaponWeak.Reset();
	BurstPlanterPawnWeak.Reset();
	if (ACombatForgePlayerState* PS = PlanterPS.Get())
	{
		if (APawn* PlanterPawn = PS->GetPawn())
		{
			BurstPlanterPawnWeak = PlanterPawn;
			if (ACombatForgeCharacter* Char = Cast<ACombatForgeCharacter>(PlanterPawn))
			{
				BurstWeaponWeak = Char->GetWeapon();
			}
		}
	}
	// First batch on the detonation frame (instant spray start); remainder on a ~1-frame timer.
	SpawnFragBurstBatch();
	if (BurstNextIndex < FragBBCount)
	{
		GetWorldTimerManager().SetTimer(BurstTimer, this, &APFBombActor::SpawnFragBurstBatch,
			BurstBatchInterval, /*bLoop=*/true);
	}
}

void APFBombActor::SpawnFragBurstBatch()
{
	UWorld* World = GetWorld();
	if (World == nullptr || !HasAuthority())
	{
		GetWorldTimerManager().ClearTimer(BurstTimer);
		return;
	}

	APawn* PlanterPawn = BurstPlanterPawnWeak.Get();
	UPFWeaponComponent* SrcWeapon = BurstWeaponWeak.Get();
	const FVector Axis = BurstAxis.GetSafeNormal();
	const FVector Origins[3] = {
		BurstCenter + Axis * BurstSideOffsetUU,
		BurstCenter - Axis * BurstSideOffsetUU,
		BurstCenter,
	};

	const int32 End = FMath::Min(BurstNextIndex + BurstBatchSize, FragBBCount);
	for (int32 i = BurstNextIndex; i < End; ++i)
	{
		// Per-index stream so batches stay deterministic without replaying prior VRands.
		FRandomStream Stream(BurstSeed + static_cast<uint32>(i) * 2654435761u + 1u);
		const FVector Dir = Stream.VRand().GetSafeNormal();
		const FVector Origin = Origins[i % 3];

		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		Params.Owner = PlanterPawn ? static_cast<AActor*>(PlanterPawn) : this;
		Params.Instigator = PlanterPawn;
		APFPaintballProjectile* BB = World->SpawnActor<APFPaintballProjectile>(
			APFPaintballProjectile::StaticClass(), Origin, Dir.Rotation(), Params);
		if (BB != nullptr)
		{
			BB->InitProjectile(Origin, Dir, PlanterTeam, /*bAuthoritative=*/true, SrcWeapon,
				BurstSeed + static_cast<uint32>(i) + 1u, /*bIgnoreShooter=*/false);
		}
	}
	BurstNextIndex = End;

	if (BurstNextIndex >= FragBBCount)
	{
		GetWorldTimerManager().ClearTimer(BurstTimer);
		// Spray done — linger briefly so late OnReps still fire cosmetics, then go.
		SetLifeSpan(0.6f);
	}
}

void APFBombActor::ApplyProximityPaint(const FVector& Center)
{
	UWorld* World = GetWorld();
	if (World == nullptr || !HasAuthority())
	{
		return;
	}
	APawn* PlanterPawn = nullptr;
	if (ACombatForgePlayerState* PS = PlanterPS.Get())
	{
		PlanterPawn = PS->GetPawn();
	}

	TArray<FOverlapResult> Overlaps;
	FCollisionObjectQueryParams ObjParams;
	ObjParams.AddObjectTypesToQuery(ECC_Pawn);
	FCollisionQueryParams Params(SCENE_QUERY_STAT(PFBombProximity), /*bTraceComplex=*/false, this);
	if (!World->OverlapMultiByObjectType(Overlaps, Center, FQuat::Identity,
			ObjParams, FCollisionShape::MakeSphere(ProximityPaintRadiusUU), Params))
	{
		return;
	}

	for (const FOverlapResult& O : Overlaps)
	{
		AActor* HitActor = O.GetActor();
		if (!HitActor)
		{
			continue;
		}
		UPFHealthComponent* Health = HitActor->FindComponentByClass<UPFHealthComponent>();
		if (!Health || Health->bEliminated)
		{
			continue;
		}
		// Teammates other than the planter stay immune (B12). Planter self-risk + enemies take the hit.
		const bool bIsPlanter = (HitActor == PlanterPawn);
		if (ACombatForgeCharacter* Vic = Cast<ACombatForgeCharacter>(HitActor))
		{
			if (const ACombatForgePlayerState* VPS = Vic->GetPlayerState<ACombatForgePlayerState>())
			{
				if (VPS->TeamId == PlanterTeam && !bIsPlanter)
				{
					continue;
				}
			}
		}

		FPFPaintHitInfo PaintHit;
		if (PlanterPawn)
		{
			PaintHit.ShooterPS = PlanterPawn->GetPlayerState<ACombatForgePlayerState>();
		}
		PaintHit.ShooterTeam = PlanterTeam;
		PaintHit.ImpactPoint = HitActor->GetActorLocation();
		PaintHit.ImpactNormal = (HitActor->GetActorLocation() - Center).GetSafeNormal();
		if (PaintHit.ImpactNormal.IsNearlyZero())
		{
			PaintHit.ImpactNormal = FVector::UpVector;
		}
		PaintHit.ServerTime = static_cast<float>(World->GetTimeSeconds());
		Health->ApplyPaintHit(PaintHit);
	}
}

void APFBombActor::SpawnCosmeticFragBurst(const FVector& Center, const FVector& FaceAxis, uint32 Seed)
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
	// Cap at the cosmetic pool size — looping FragBBCount (1000) Acquire calls hitch remotes for no gain.
	const FVector Axis = FaceAxis.GetSafeNormal();
	const FVector Origins[3] = {
		Center + Axis * BurstSideOffsetUU,
		Center - Axis * BurstSideOffsetUU,
		Center,
	};
	const int32 N = FMath::Min(CosmeticTracerCount, FragBBCount);
	for (int32 c = 0; c < N; ++c)
	{
		// Stride through the full index space so cosmetics sample the same dual-side sphere as the server.
		const int32 i = (c * FragBBCount) / N;
		FRandomStream Stream(Seed + static_cast<uint32>(i) * 2654435761u + 1u);
		const FVector Dir = Stream.VRand().GetSafeNormal();
		const FVector Origin = Origins[i % 3];
		if (APFPaintballProjectile* Ball = Splats->AcquireCosmeticProjectile())
		{
			Ball->InitProjectile(Origin, Dir, PlanterTeam, /*bAuthoritative=*/false, /*SourceWeapon=*/nullptr,
				Seed + static_cast<uint32>(i) + 1u);
		}
	}
}

void APFBombActor::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	GetWorldTimerManager().ClearTimer(FuseTimer);
	GetWorldTimerManager().ClearTimer(BurstTimer);
	Super::EndPlay(EndPlayReason);
}
