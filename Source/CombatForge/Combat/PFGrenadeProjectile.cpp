// Copyright (c) 2026 Tom Chapman. All rights reserved.

#include "Combat/PFGrenadeProjectile.h"

#include "CombatForge.h"
#include "Combat/PFSmokeSubsystem.h"
#include "Combat/PFPaintballProjectile.h"
#include "Combat/PFWeaponComponent.h"
#include "Combat/PFSplatSubsystem.h"
#include "Combat/PFCombatAudio.h"
#include "Player/CombatForgeCharacter.h"

#include "Components/SphereComponent.h"
#include "Components/StaticMeshComponent.h"
#include "GameFramework/ProjectileMovementComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Net/UnrealNetwork.h"
#include "TimerManager.h"
#include "UObject/ConstructorHelpers.h"
#include "UObject/SoftObjectPath.h"

APFGrenadeProjectile::APFGrenadeProjectile()
{
	PrimaryActorTick.bCanEverTick = true;              // enabled only for the smoke cloud animation
	PrimaryActorTick.bStartWithTickEnabled = false;    // off during flight/fuse; StartSmokeVisual turns it on
	bReplicates = true;
	SetReplicateMovement(true);
	bAlwaysRelevant = true;   // small arena + short-lived; guarantees OnRep for cosmetics/smoke

	// Collision sphere is the movement root. Bounces off world geometry, passes through pawns and
	// (critically) the PF_ECC_Paintball channel so its own frag BBs don't self-hit the fading grenade.
	Collision = CreateDefaultSubobject<USphereComponent>(TEXT("Collision"));
	Collision->InitSphereRadius(10.f);
	Collision->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	Collision->SetCollisionObjectType(ECC_WorldDynamic);
	Collision->SetCollisionResponseToAllChannels(ECR_Ignore);
	Collision->SetCollisionResponseToChannel(ECC_WorldStatic, ECR_Block);
	Collision->SetCollisionResponseToChannel(ECC_WorldDynamic, ECR_Block);
	Collision->SetCanEverAffectNavigation(false);
	SetRootComponent(Collision);

	Mesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Mesh"));
	Mesh->SetupAttachment(Collision);
	Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Mesh->SetCastShadow(false);
	Mesh->SetRelativeScale3D(FVector(0.15f));   // ~15 uu engine sphere ball
	static ConstructorHelpers::FObjectFinder<UStaticMesh> SphereFinder(TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	if (SphereFinder.Succeeded())
	{
		Mesh->SetStaticMesh(SphereFinder.Object);
	}

	Movement = CreateDefaultSubobject<UProjectileMovementComponent>(TEXT("Movement"));
	Movement->UpdatedComponent = Collision;
	Movement->InitialSpeed = 0.f;                 // set in ServerInit
	Movement->MaxSpeed = 4000.f;
	Movement->ProjectileGravityScale = 1.0f;      // original lob arc (the throw itself was right; only the spawn point was wrong)
	Movement->bShouldBounce = true;
	Movement->Bounciness = 0.35f;
	Movement->Friction = 0.35f;
	Movement->bRotationFollowsVelocity = false;
	Movement->bAutoActivate = false;
}

void APFGrenadeProjectile::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(APFGrenadeProjectile, bDetonated);
	DOREPLIFETIME(APFGrenadeProjectile, KindRep);
	DOREPLIFETIME(APFGrenadeProjectile, BurstSeed);
	DOREPLIFETIME(APFGrenadeProjectile, DetonatePoint);
}

void APFGrenadeProjectile::ServerInit(const FVector& AimDir, uint8 Team, EPFGrenadeType Type,
	UPFWeaponComponent* Thrower)
{
	if (!HasAuthority())
	{
		return;
	}
	TeamId = Team;
	Kind = Type;
	KindRep = static_cast<uint8>(Type);
	ThrowerWeak = Thrower;

	FVector Launch = (AimDir + FVector(0.f, 0.f, 0.35f)).GetSafeNormal();   // original toss arc (spawns from the eyes now)
	if (Launch.IsNearlyZero())
	{
		Launch = FVector(1.f, 0.f, 0.35f).GetSafeNormal();
	}
	if (Movement)
	{
		Movement->SetUpdatedComponent(Collision);
		Movement->Velocity = Launch * ThrowSpeed;
		Movement->SetActive(true);
	}

	GetWorldTimerManager().SetTimer(FuseTimer, this, &APFGrenadeProjectile::ServerDetonate,
		FMath::Max(0.1f, FuseSeconds), false);
}

void APFGrenadeProjectile::ServerDetonate()
{
	if (!HasAuthority() || bDetonated)
	{
		return;
	}
	const FVector At = GetActorLocation();
	DetonatePoint = At;
	BurstSeed = static_cast<uint32>(FMath::Rand()) ^ (GetUniqueID() * 2654435761u);
	KindRep = static_cast<uint8>(Kind);
	bDetonated = true;   // replicates -> OnRep_Detonated on clients
	ForceNetUpdate();

	if (Movement)
	{
		Movement->StopMovementImmediately();
	}

	// Authoritative gameplay: frag spawns the real BB burst on the server (damage + host visuals).
	if (Kind == EPFGrenadeType::Frag)
	{
		SpawnFragBurst(At, BurstSeed);
	}
	// Smoke is CONCEALMENT, not just a visual: register the cloud so bot line-of-sight treats it as a
	// sight-blocker for its lifetime. Single bounding sphere over the 9-puff cluster (center lifted, 1.1x).
	else if (Kind == EPFGrenadeType::Smoke)
	{
		if (UPFSmokeSubsystem* Smoke = GetWorld() ? GetWorld()->GetSubsystem<UPFSmokeSubsystem>() : nullptr)
		{
			Smoke->RegisterSmoke(At + FVector(0.f, 0.f, SmokeRadius * 0.3f), SmokeRadius * 1.1f, SmokeDuration);
		}
	}

	// Host-local cosmetics/audio (OnRep never fires on the authority).
	HandleDetonateVisualsLocal();

	// Frag: keep the actor alive briefly so bDetonated reliably replicates for client cosmetics, then go.
	// Smoke: the actor IS the cloud host for its whole lifetime.
	const float Life = (Kind == EPFGrenadeType::Frag) ? 0.5f : FMath::Max(0.5f, SmokeDuration);
	GetWorldTimerManager().SetTimer(DestroyTimer, this, &APFGrenadeProjectile::OnDestroyTimer, Life, false);
}

void APFGrenadeProjectile::OnRep_Detonated()
{
	if (!bDetonated)
	{
		return;
	}
	if (Movement)
	{
		Movement->StopMovementImmediately();
	}
	HandleDetonateVisualsLocal();
}

void APFGrenadeProjectile::HandleDetonateVisualsLocal()
{
	const FVector At = DetonatePoint;
	const EPFGrenadeType K = (static_cast<EPFGrenadeType>(KindRep) == EPFGrenadeType::Smoke)
		? EPFGrenadeType::Smoke : EPFGrenadeType::Frag;

	if (Mesh)
	{
		Mesh->SetVisibility(false);   // hide the thrown ball at the moment it pops
	}

	if (K == EPFGrenadeType::Frag)
	{
		// The host already has the authoritative BBs as its visual; only remote clients need cosmetics.
		if (!HasAuthority())
		{
			SpawnCosmeticFragBurst(At, BurstSeed);
		}
		PlayDetonAudio(At, /*bFrag=*/true);
	}
	else
	{
		StartSmokeVisual(At);
		PlayDetonAudio(At, /*bFrag=*/false);
	}
}

void APFGrenadeProjectile::SpawnFragBurst(const FVector& At, uint32 Seed)
{
	UWorld* World = GetWorld();
	if (World == nullptr)
	{
		return;
	}
	UPFWeaponComponent* SrcWeapon = ThrowerWeak.Get();   // required for damage/splat routing
	APawn* ThrowerPawn = Cast<APawn>(GetOwner());
	FRandomStream Stream(Seed);

	for (int32 i = 0; i < FragBBCount; ++i)
	{
		FVector Dir = Stream.VRand();
		if (Dir.Z < 0.f)
		{
			Dir.Z = -Dir.Z * 0.5f;   // bias the lower hemisphere upward so BBs spray out, not into the floor
		}
		Dir = Dir.GetSafeNormal();

		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		Params.Owner = GetOwner();
		Params.Instigator = ThrowerPawn;
		APFPaintballProjectile* BB = World->SpawnActor<APFPaintballProjectile>(
			APFPaintballProjectile::StaticClass(), At, Dir.Rotation(), Params);
		if (BB != nullptr)
		{
			// Distinct high ShotIndex space so frag hitmarkers don't collide with live-fire indices.
			BB->InitProjectile(At, Dir, TeamId, /*bAuthoritative=*/true, SrcWeapon, Seed + static_cast<uint32>(i) + 1u);
		}
	}
}

void APFGrenadeProjectile::SpawnCosmeticFragBurst(const FVector& At, uint32 Seed)
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
			// SourceWeapon null on remote clients -> InitProjectile falls back to default BB ballistics (fine
			// for a cosmetic tracer); Team is passed explicitly for tint.
			Ball->InitProjectile(At, Dir, TeamId, /*bAuthoritative=*/false, ThrowerWeak.Get(),
				Seed + static_cast<uint32>(i) + 1u);
		}
	}
}

void APFGrenadeProjectile::StartSmokeVisual(const FVector& At)
{
	UStaticMesh* Sphere = Cast<UStaticMesh>(FSoftObjectPath(TEXT("/Engine/BasicShapes/Sphere.Sphere")).TryLoad());
	if (Sphere == nullptr || Collision == nullptr)
	{
		return;
	}
	// Prefer the translucent volumetric smoke material (soft, depth-faded edges); fall back to the old unlit
	// puff if it hasn't been generated yet (run Scripts/gen_combat_fx.py).
	UMaterialInterface* SmokeMat = Cast<UMaterialInterface>(
		FSoftObjectPath(TEXT("/Game/Materials/M_PF_SmokeVolume.M_PF_SmokeVolume")).TryLoad());
	const bool bVolumetric = (SmokeMat != nullptr);
	if (SmokeMat == nullptr)
	{
		SmokeMat = Cast<UMaterialInterface>(
			FSoftObjectPath(TEXT("/Game/Materials/M_PF_MuzzleSmoke.M_PF_MuzzleSmoke")).TryLoad());
	}
	bSmokeVolumetric = bVolumetric;
	SmokeStartTime = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.f;
	SmokeMIDs.Reset();
	PuffTargetScale.Reset();
	PuffMaxDensity.Reset();
	PuffDissolveWindow.Reset();

	// Engine sphere is 50 uu radius; scale so the main puff radius ~= SmokeRadius. A denser overlapping cluster
	// of varied spheres reads as a billowing cloud (not a few hard balls) and conceals better.
	const float BaseScale = SmokeRadius / 50.f;
	const FVector Offsets[9] = {
		FVector(0.f, 0.f, SmokeRadius * 0.35f),
		FVector(SmokeRadius * 0.5f, 0.f, SmokeRadius * 0.1f),
		FVector(-SmokeRadius * 0.5f, 0.f, SmokeRadius * 0.15f),
		FVector(0.f, SmokeRadius * 0.5f, SmokeRadius * 0.2f),
		FVector(0.f, -SmokeRadius * 0.5f, SmokeRadius * 0.1f),
		FVector(SmokeRadius * 0.32f, SmokeRadius * 0.32f, SmokeRadius * 0.55f),
		FVector(-SmokeRadius * 0.32f, SmokeRadius * 0.3f, SmokeRadius * 0.05f),
		FVector(SmokeRadius * 0.28f, -SmokeRadius * 0.34f, SmokeRadius * 0.4f),
		FVector(-SmokeRadius * 0.3f, -SmokeRadius * 0.28f, SmokeRadius * 0.45f),
	};
	const float Scales[9] = { 1.05f, 0.72f, 0.75f, 0.7f, 0.72f, 0.6f, 0.66f, 0.62f, 0.58f };

	for (int32 i = 0; i < 9; ++i)
	{
		UStaticMeshComponent* Puff = NewObject<UStaticMeshComponent>(this);
		if (Puff == nullptr)
		{
			continue;
		}
		const FVector TargetScale(BaseScale * Scales[i]);
		Puff->SetupAttachment(Collision);
		Puff->SetStaticMesh(Sphere);
		Puff->SetRelativeLocation(Offsets[i]);
		Puff->SetRelativeScale3D(TargetScale * 0.2f);   // start small; billows up in Tick
		Puff->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Puff->SetCastShadow(false);
		Puff->SetCanEverAffectNavigation(false);
		Puff->RegisterComponent();

		UMaterialInstanceDynamic* PuffMID = nullptr;
		if (SmokeMat != nullptr)
		{
			PuffMID = Puff->CreateDynamicMaterialInstance(0, SmokeMat);
			if (PuffMID != nullptr)
			{
				const FLinearColor Grey(0.6f, 0.6f, 0.62f, 1.f);
				if (bVolumetric)
				{
					PuffMID->SetVectorParameterValue(TEXT("SmokeColor"), Grey);
					PuffMID->SetScalarParameterValue(TEXT("Density"), 0.f);   // start invisible; ramps up in Tick
				}
				else
				{
					PuffMID->SetVectorParameterValue(TEXT("EmissiveColor"), Grey);
					PuffMID->SetVectorParameterValue(TEXT("Color"), Grey);
				}
			}
		}

		// Staggered dissolve: puffs begin fading at different times-before-end (2.6 s .. 0.7 s), so they thin
		// out one at a time instead of all vanishing on one frame. Keep every window < SmokeDuration.
		SmokePuffs.Add(Puff);
		SmokeMIDs.Add(PuffMID);
		PuffTargetScale.Add(TargetScale);
		PuffMaxDensity.Add(1.3f);   // denser — smoke must read as a real view-blocker (it now blocks bot sight too)
		PuffDissolveWindow.Add(FMath::Lerp(2.6f, 0.7f, static_cast<float>(i) / 8.f));
	}
	SetActorTickEnabled(true);   // begin the appear/dissolve animation
}

void APFGrenadeProjectile::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (SmokeStartTime < 0.f || SmokePuffs.Num() == 0)
	{
		return;
	}
	const float Now = GetWorld() ? GetWorld()->GetTimeSeconds() : SmokeStartTime;
	const float Elapsed = Now - SmokeStartTime;
	const float Appear = FMath::SmoothStep(0.f, SmokeAppearDur, Elapsed);   // billow-in over ~0.6 s
	const float Remain = SmokeDuration - Elapsed;
	for (int32 i = 0; i < SmokePuffs.Num(); ++i)
	{
		if (SmokePuffs[i] == nullptr || !PuffTargetScale.IsValidIndex(i))
		{
			continue;
		}
		const float Window = PuffDissolveWindow.IsValidIndex(i) ? PuffDissolveWindow[i] : 1.f;
		const float Diss = 1.f - FMath::Clamp(Remain / FMath::Max(0.1f, Window), 0.f, 1.f);
		const float Fade = FMath::SmoothStep(0.f, 1.f, Diss);   // this puff's own dissolve 0..1
		// Volumetric fades via opacity (Density) so keep near full size; the unlit fallback has no opacity param,
		// so shrink it away instead.
		const float FadeScaleTarget = bSmokeVolumetric ? 0.85f : 0.05f;
		const float ScaleFactor = FMath::Lerp(0.2f, 1.f, Appear) * FMath::Lerp(1.f, FadeScaleTarget, Fade);
		SmokePuffs[i]->SetRelativeScale3D(PuffTargetScale[i] * ScaleFactor);
		if (bSmokeVolumetric && SmokeMIDs.IsValidIndex(i) && SmokeMIDs[i] != nullptr && PuffMaxDensity.IsValidIndex(i))
		{
			SmokeMIDs[i]->SetScalarParameterValue(TEXT("Density"), PuffMaxDensity[i] * Appear * (1.f - Fade));
		}
	}
}

void APFGrenadeProjectile::PlayDetonAudio(const FVector& At, bool bFrag)
{
	UWorld* World = GetWorld();
	if (World == nullptr)
	{
		return;
	}
	APlayerController* PC = World->GetFirstPlayerController();
	if (PC == nullptr)
	{
		return;
	}
	ACombatForgeCharacter* LocalChar = Cast<ACombatForgeCharacter>(PC->GetPawn());
	if (LocalChar == nullptr)
	{
		return;
	}
	if (UPFCombatAudio* Audio = LocalChar->GetCombatAudio())
	{
		if (bFrag)
		{
			Audio->PlayFragBurstAt(At);
		}
		else
		{
			Audio->PlaySmokeHissAt(At);
		}
	}
}

void APFGrenadeProjectile::OnDestroyTimer()
{
	Destroy();
}
