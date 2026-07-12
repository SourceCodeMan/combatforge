// Copyright (c) 2026 Tom Chapman. All rights reserved.

#include "Combat/PFPaintballProjectile.h"

#include "PaintForge.h"
#include "Core/PaintForgeTypes.h"
#include "Core/PaintForgePlayerState.h"
#include "Combat/PFHealthComponent.h"
#include "Combat/PFSplatSubsystem.h"
#include "Combat/PFWeaponComponent.h"
#include "CollisionQueryParams.h"
#include "Components/SphereComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/CollisionProfile.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/ProjectileMovementComponent.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "UObject/ConstructorHelpers.h"

namespace
{
	// Elongated tracer along velocity (+X with bRotationFollowsVelocity).
	// Sized for ~1/3 original BB (radius ~2.33 uu → mesh scale ~1/3 of the old trail).
	constexpr float TracerLenScale = 0.10f;    // ~10 uu long
	constexpr float TracerRadScale = 0.037f;   // ~3.7 uu diameter
	constexpr float EmissiveBoost = 5.5f;      // hot team tracer read (04 §2.2)
	constexpr float DefaultRadiusUU = 2.33f;
}

APFPaintballProjectile::APFPaintballProjectile()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = false;

	bReplicates = false;   // B3: paintballs NEVER replicate — cosmetic or server-only
	SetCanBeDamaged(false);

	CollisionComp = CreateDefaultSubobject<USphereComponent>(TEXT("Collision"));
	CollisionComp->InitSphereRadius(DefaultRadiusUU);
	CollisionComp->SetCollisionObjectType(PF_ECC_Paintball);
	CollisionComp->SetCollisionEnabled(ECollisionEnabled::NoCollision);   // InitProjectile arms per mode
	CollisionComp->SetCollisionResponseToAllChannels(ECR_Block);
	CollisionComp->SetCollisionResponseToChannel(PF_ECC_Paintball, ECR_Ignore);    // balls pass balls
	CollisionComp->SetCollisionResponseToChannel(PF_ECC_BuildTrace, ECR_Ignore);   // §4.6
	CollisionComp->SetCollisionResponseToChannel(ECC_Visibility, ECR_Ignore);
	CollisionComp->SetCollisionResponseToChannel(ECC_Camera, ECR_Ignore);
	CollisionComp->SetGenerateOverlapEvents(false);
	CollisionComp->CanCharacterStepUpOn = ECB_No;
	SetRootComponent(CollisionComp);

	BallMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Ball"));
	BallMesh->SetupAttachment(CollisionComp);
	BallMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	BallMesh->SetCollisionProfileName(UCollisionProfile::NoCollision_ProfileName);
	BallMesh->SetCastShadow(false);
	// Stretch along +X (flight direction when Movement->bRotationFollowsVelocity).
	BallMesh->SetRelativeScale3D(FVector(TracerLenScale, TracerRadScale, TracerRadScale));

	static ConstructorHelpers::FObjectFinder<UStaticMesh> SphereFinder(
		TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	if (SphereFinder.Succeeded())
	{
		BallMesh->SetStaticMesh(SphereFinder.Object);
	}
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> MaterialFinder(
		TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
	if (MaterialFinder.Succeeded())
	{
		BaseMaterial = MaterialFinder.Object;
	}

	Movement = CreateDefaultSubobject<UProjectileMovementComponent>(TEXT("Movement"));
	Movement->UpdatedComponent = CollisionComp;
	Movement->InitialSpeed = 10000.f;
	Movement->MaxSpeed = 10000.f;
	Movement->ProjectileGravityScale = 0.35f;
	Movement->bShouldBounce = false;            // every impact breaks (04 §2.2)
	Movement->bRotationFollowsVelocity = true;
	Movement->bInitialVelocityInLocalSpace = false;
	Movement->bAutoActivate = false;
	Movement->OnProjectileStop.AddDynamic(this, &APFPaintballProjectile::HandleProjectileStop);
}

void APFPaintballProjectile::InitProjectile(const FVector& Origin, const FVector& SpreadedDir,
	uint8 Team, bool bAuthoritative, UPFWeaponComponent* SourceWeapon, uint32 ShotIndex)
{
	TeamId = Team;
	ShotIndexStored = ShotIndex;
	bAuthoritativeMode = bAuthoritative;
	SourceWeaponWeak = SourceWeapon;
	ShooterActorWeak = SourceWeapon ? SourceWeapon->GetOwner() : nullptr;

	// Ballistic numbers come from the weapon config so a data-only tune reaches both modes.
	float Speed = 10000.f;
	float GravScale = 0.35f;
	LifetimeSec = 2.f;
	RadiusUU = DefaultRadiusUU;
	if (SourceWeapon != nullptr)
	{
		Speed = SourceWeapon->MuzzleSpeedUU;
		GravScale = SourceWeapon->ProjGravityScale;
		LifetimeSec = SourceWeapon->ProjLifetime;
		RadiusUU = SourceWeapon->ProjRadiusUU;
	}

	const FVector Dir = SpreadedDir.GetSafeNormal();
	SetActorLocationAndRotation(Origin, Dir.Rotation(), false, nullptr, ETeleportType::TeleportPhysics);
	PrevLocation = Origin;
	LifeElapsed = 0.f;
	bInFlight = true;
	SetActorHiddenInGame(false);

	// Team-emissive tracer tint (04 §2.2): Color ×3 on the one tintable engine material.
	if (BallMID == nullptr && BaseMaterial != nullptr)
	{
		BallMID = UMaterialInstanceDynamic::Create(BaseMaterial, this);
		BallMesh->SetMaterial(0, BallMID);
	}
	if (BallMID != nullptr)
	{
		BallMID->SetVectorParameterValue(TEXT("Color"), PFColors::ForTeam(Team) * EmissiveBoost);
	}

	// No self-splat, ever (04 §2.4): shooter rides the ignore list in both modes.
	CollisionComp->ClearMoveIgnoreActors();
	if (AActor* Shooter = ShooterActorWeak.Get())
	{
		CollisionComp->IgnoreActorWhenMoving(Shooter, true);
	}

	if (bAuthoritativeMode)
	{
		// Real blocking sweep vs Pawn/World on the Paintball channel; server only.
		CollisionComp->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
		SetLifeSpan(LifetimeSec);   // belt-and-braces destroy on a whiffed 2 s lob
	}
	else
	{
		// Cosmetic: no gameplay collision; the per-tick visual sweep predicts the impact.
		CollisionComp->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	}

	Movement->SetUpdatedComponent(CollisionComp);   // pooled reuse: stop nulls this
	Movement->InitialSpeed = Speed;
	Movement->MaxSpeed = Speed;
	Movement->ProjectileGravityScale = GravScale;
	Movement->Velocity = Dir * Speed;   // no inherited shooter velocity (04 §2.2)
	Movement->SetActive(true, true);

	SetActorTickEnabled(true);
}

void APFPaintballProjectile::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	if (!bInFlight)
	{
		return;
	}

	LifeElapsed += DeltaSeconds;

	if (!bAuthoritativeMode)
	{
		// Visual-only impact prediction: sweep the segment travelled this frame.
		const FVector Current = GetActorLocation();
		if (!Current.Equals(PrevLocation))
		{
			FCollisionQueryParams Params(SCENE_QUERY_STAT(PFCosmeticBall), false, this);
			if (const AActor* Shooter = ShooterActorWeak.Get())
			{
				Params.AddIgnoredActor(Shooter);
			}
			FHitResult Hit;
			if (GetWorld()->SweepSingleByChannel(Hit, PrevLocation, Current, FQuat::Identity,
					PF_ECC_Paintball, FCollisionShape::MakeSphere(RadiusUU), Params))
			{
				HandleCosmeticImpact(Hit);
				return;
			}
			PrevLocation = Current;
		}
	}

	if (LifeElapsed >= LifetimeSec)
	{
		if (bAuthoritativeMode)
		{
			Destroy();
		}
		else
		{
			Deactivate();   // back to the pool
		}
	}
}

void APFPaintballProjectile::Deactivate()
{
	bInFlight = false;
	Movement->StopMovementImmediately();
	Movement->Deactivate();
	CollisionComp->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	SetActorHiddenInGame(true);
	SetActorTickEnabled(false);
}

void APFPaintballProjectile::HandleProjectileStop(const FHitResult& ImpactResult)
{
	if (!bInFlight)
	{
		return;
	}

	if (bAuthoritativeMode && HasAuthority())
	{
		bInFlight = false;
		ResolveAuthoritativeImpact(ImpactResult);
		Destroy();
	}
	else
	{
		Deactivate();
	}
}

void APFPaintballProjectile::ResolveAuthoritativeImpact(const FHitResult& Hit)
{
	UPFWeaponComponent* Weapon = SourceWeaponWeak.Get();
	if (Weapon == nullptr)
	{
		// Shooter left mid-flight: no channel to confirm or splat through. Ball just breaks.
		return;
	}

	const FVector ImpactPoint = Hit.ImpactPoint;
	const FVector ImpactNormal = Hit.ImpactNormal;
	AActor* HitActor = Hit.GetActor();
	UPFHealthComponent* VictimHealth =
		HitActor ? HitActor->FindComponentByClass<UPFHealthComponent>() : nullptr;

	// Eliminated corpses block for 0.5 s but never generate hit events (04 §2.4) —
	// treat them exactly like world geometry.
	if (VictimHealth != nullptr && !VictimHealth->bEliminated)
	{
		const uint8 VictimTeam = VictimTeamOf(HitActor);
		if (VictimTeam != 255 && VictimTeam == TeamId)
		{
			// Friendly fire (B12): cosmetic splat only — no damage, no hit event, no hitmarker.
			Weapon->MulticastImpactSplat(ImpactPoint, ImpactNormal, TeamId);
			return;
		}

		FPFPaintHitInfo PaintHit;
		if (const APawn* ShooterPawn = Cast<APawn>(ShooterActorWeak.Get()))
		{
			PaintHit.ShooterPS = ShooterPawn->GetPlayerState<APaintForgePlayerState>();
		}
		PaintHit.ShooterTeam = TeamId;
		PaintHit.ImpactPoint = ImpactPoint;
		PaintHit.ImpactNormal = ImpactNormal;
		PaintHit.Region = VictimHealth->ComputeRegion(ImpactPoint);
		PaintHit.Damage = (PaintHit.Region == EPFBodyRegion::Mask) ? 2 : 1;
		PaintHit.ServerTime = static_cast<float>(GetWorld()->GetTimeSeconds());

		VictimHealth->ApplyPaintHit(PaintHit);

		// Hitmarker only via ClientHitConfirm (B3); elim variant if this ball finished them.
		Weapon->ClientHitConfirm(ShotIndexStored, VictimHealth->bEliminated);
		Weapon->MulticastImpactSplat(ImpactPoint, ImpactNormal, TeamId);
		return;
	}

	// World hit (or corpse): splat only.
	Weapon->MulticastImpactSplat(ImpactPoint, ImpactNormal, TeamId);
}

void APFPaintballProjectile::HandleCosmeticImpact(const FHitResult& Hit)
{
	// Owning client only: register the PENDING splat for the @75 uu reconcile (04 §5.2).
	// Remote-viewer cosmetics just break; their truth arrives via MulticastImpactSplat.
	const APawn* ShooterPawn = Cast<APawn>(ShooterActorWeak.Get());
	if (ShooterPawn != nullptr && ShooterPawn->IsLocallyControlled())
	{
		if (UPFSplatSubsystem* Splats = GetWorld()->GetSubsystem<UPFSplatSubsystem>())
		{
			Splats->SpawnPendingSplat(ShotIndexStored, Hit.ImpactPoint, Hit.ImpactNormal, TeamId);
		}
	}
	Deactivate();
}

uint8 APFPaintballProjectile::VictimTeamOf(const AActor* HitActor)
{
	if (const APawn* VictimPawn = Cast<APawn>(HitActor))
	{
		if (const APaintForgePlayerState* PS = VictimPawn->GetPlayerState<APaintForgePlayerState>())
		{
			return PS->TeamId;
		}
	}
	return 255;   // teamless (target dummy) — always a valid target, never "friendly"
}
