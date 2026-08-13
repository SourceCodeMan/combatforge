// Copyright (c) 2026 Tom Chapman. All rights reserved.

#include "Combat/PFPaintballProjectile.h"

#include "CombatForge.h"
#include "Core/CombatForgeTypes.h"
#include "Core/CombatForgePlayerState.h"
#include "Combat/PFHealthComponent.h"
#include "Combat/PFSplatSubsystem.h"
#include "Combat/PFWeaponComponent.h"
#include "Player/CombatForgeCharacter.h"
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
	// Elongated tracer along velocity (+X with bRotationFollowsVelocity). Halved a THIRD time per Tom
	// 2026-07-20 (0.10/0.037 -> 0.067/0.025 -> 0.0335/0.0125 -> now). The VISUAL only: CollisionComp keeps
	// DefaultRadiusUU below, so hit registration is unchanged and stays forgiving. Shrinking the mesh does
	// not make BBs harder to land, only harder to see as blobs.
	constexpr float TracerLenScale = 0.01675f;  // ~1.7 uu long
	constexpr float TracerRadScale = 0.00625f;  // ~0.63 uu diameter
	// 5.5 -> 2.5: at 5.5 the 90-BB frag burst read as an orange blob storm; 2.5 keeps team-color
	// readability while tracers read as lit BBs, not glowing orbs.
	constexpr float EmissiveBoost = 2.5f;      // team tracer read (04 §2.2)
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
	uint8 Team, bool bAuthoritative, UPFWeaponComponent* SourceWeapon, uint32 ShotIndex,
	bool bIgnoreShooter, bool bPointBlankRescue)
{
	TeamId = Team;
	ShotIndexStored = ShotIndex;
	bAuthoritativeMode = bAuthoritative;
	SourceWeaponWeak = SourceWeapon;
	// Prefer the weapon owner; fall back to Instigator so weaponless sources (bomb after planter
	// recycled) still credit the right shooter.
	ShooterActorWeak = SourceWeapon ? SourceWeapon->GetOwner() : GetInstigator();

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

	// Live-fire never self-splats (04 §2.4). Breach-bomb BBs pass bIgnoreShooter=false so the
	// planter can be painted by their own charge (standing on a planted bomb was a free lunch).
	CollisionComp->ClearMoveIgnoreActors();
	if (bIgnoreShooter)
	{
		if (AActor* Shooter = ShooterActorWeak.Get())
		{
			CollisionComp->IgnoreActorWhenMoving(Shooter, true);
		}
	}

	if (bAuthoritativeMode)
	{
		// Real blocking sweep vs Pawn/World on the Paintball channel; server only.
		CollisionComp->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
		SetLifeSpan(LifetimeSec);   // belt-and-braces destroy on a whiffed 2 s lob

		// Point-blank rescue (playtest 2026-08-06: sub-3-feet shots did ~nothing). The ball spawns
		// at the MUZZLE, 26-48uu in front of the camera — a target hugging the shooter sits between
		// the eye and that spawn point, or the spawn point sits inside their body. A sweep that
		// starts in penetration depenetrates without a blocking hit, so the ball sails away and the
		// whole volley whiffs (worst on shotguns: 38uu muzzle x 6-8 pellets). Cover the eye→muzzle
		// dead zone with one explicit sweep before flight; a live enemy pawn in it resolves as an
		// immediate impact. Live-fire only (bPointBlankRescue + bIgnoreShooter, both default true).
		// bIgnoreShooter=false is the bomb (planter self-splat). Frag keeps bIgnoreShooter=true
		// (no self-splat) and passes bPointBlankRescue=false so the cloud does not hitscan
		// Eye → Origin. A world hit in the gap (gun through a window frame) falls through to
		// normal flight, same as before.
		if (bPointBlankRescue && SourceWeapon != nullptr && bIgnoreShooter)
		{
			if (const ACombatForgeCharacter* ShooterPawn = Cast<ACombatForgeCharacter>(ShooterActorWeak.Get()))
			{
				const FVector Eye = ShooterPawn->GetEyeWorldLocation();
				const FVector End = Origin + Dir * FMath::Max(3.f * RadiusUU, 12.f);
				FCollisionQueryParams Q(SCENE_QUERY_STAT(PFPointBlankRescue), false, ShooterPawn);
				Q.AddIgnoredActor(this);
				FHitResult PB;
				if (GetWorld()->SweepSingleByChannel(PB, Eye, End, FQuat::Identity, PF_ECC_Paintball,
						FCollisionShape::MakeSphere(RadiusUU), Q)
					&& Cast<APawn>(PB.GetActor()) != nullptr)
				{
					bInFlight = false;
					ResolveAuthoritativeImpact(PB);
					Destroy();
					return;
				}
			}
		}
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
		// B12: teammates are immune — but the SHOOTER themselves can still be tagged (own grenade /
		// breach charge self-risk). Without this exception a planter standing on their bomb took 0 dmg.
		const bool bHitSelf = (HitActor != nullptr && HitActor == ShooterActorWeak.Get());
		if (VictimTeam != 255 && VictimTeam == TeamId && !bHitSelf)
		{
			// Friendly fire: cosmetic splat only — no damage, no hit event, no hitmarker.
			if (Weapon != nullptr)
			{
				Weapon->MulticastImpactSplat(ImpactPoint, ImpactNormal, TeamId);
			}
			return;
		}

		FPFPaintHitInfo PaintHit;
		if (const APawn* ShooterPawn = Cast<APawn>(ShooterActorWeak.Get()))
		{
			PaintHit.ShooterPS = ShooterPawn->GetPlayerState<ACombatForgePlayerState>();
		}
		PaintHit.ShooterTeam = TeamId;
		PaintHit.ImpactPoint = ImpactPoint;
		PaintHit.ImpactNormal = ImpactNormal;
		// Forward the hit bone (None for capsule hits — ApplyPaintHit derives the region via
		// nearest-bone scan; a real bone name from a mesh intercept wins outright). Region is
		// resolved in ApplyPaintHit; Damage is the shooter's per-weapon HitValue (server resolve).
		PaintHit.HitBone = Hit.BoneName;
		PaintHit.ServerTime = static_cast<float>(GetWorld()->GetTimeSeconds());
		if (UtilityDamageOverride > 0)
		{
			// P2-CB1/CB2: bomb/frag balls carry their own fixed damage — the planter's equipped
			// weapon rides along only as the hitmarker/splat channel.
			PaintHit.Damage = UtilityDamageOverride;
		}
		else if (Weapon != nullptr)
		{
			PaintHit.Damage = Weapon->HitValue;
		}

		VictimHealth->ApplyPaintHit(PaintHit);

		// Hitmarker/splat need a live weapon channel. Weaponless sources (bomb after planter
		// recycled) still apply damage above so nearby players get painted.
		if (Weapon != nullptr)
		{
			Weapon->SendHitConfirmCoalesced(ShotIndexStored, VictimHealth->bEliminated);
			Weapon->MulticastImpactSplat(ImpactPoint, ImpactNormal, TeamId);
		}
		return;
	}

	// World hit (or corpse): splat only when we have a weapon multicast channel.
	if (Weapon != nullptr)
	{
		Weapon->MulticastImpactSplat(ImpactPoint, ImpactNormal, TeamId);
	}
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
		if (const ACombatForgePlayerState* PS = VictimPawn->GetPlayerState<ACombatForgePlayerState>())
		{
			return PS->TeamId;
		}
	}
	return 255;   // teamless (target dummy) — always a valid target, never "friendly"
}
