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
	// (critically) NEVER blocks the Paintball channel — smoke/frag canisters must not eat BBs
	// (playtest report: "smoke blocked my shot"). Detonation also disables this collider entirely.
	Collision = CreateDefaultSubobject<USphereComponent>(TEXT("Collision"));
	Collision->InitSphereRadius(10.f);
	Collision->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	Collision->SetCollisionObjectType(ECC_WorldDynamic);
	Collision->SetCollisionResponseToAllChannels(ECR_Ignore);
	Collision->SetCollisionResponseToChannel(ECC_WorldStatic, ECR_Block);
	Collision->SetCollisionResponseToChannel(ECC_WorldDynamic, ECR_Block);
	Collision->SetCollisionResponseToChannel(PF_ECC_Paintball, ECR_Ignore);   // belt-and-braces vs paintballs
	Collision->SetCanEverAffectNavigation(false);
	Collision->SetGenerateOverlapEvents(false);
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
	// Guaranteed ball-skin fallback — hard CDO refs are only reliable for /Engine content (playbook §2);
	// the preferred /Game master soft-resolves later in ApplyBallSkin.
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> MaterialFinder(
		TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
	if (MaterialFinder.Succeeded())
	{
		BallBaseMaterial = MaterialFinder.Object;
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
	DOREPLIFETIME(APFGrenadeProjectile, TeamId);   // cosmetic burst tint on remotes (issue #11 CB1)
}

void APFGrenadeProjectile::BeginPlay()
{
	Super::BeginPlay();
	// Clients may begin play before KindRep's initial value lands (defaults to Frag); OnRep_Kind re-tints.
	ApplyBallSkin();
}

void APFGrenadeProjectile::OnRep_Kind()
{
	ApplyBallSkin();
}

void APFGrenadeProjectile::ApplyBallSkin()
{
	if (Mesh == nullptr)
	{
		return;
	}
	if (BallMID == nullptr)
	{
		// Prefer the proven cooked arena master (carries the "Color" param); fall back to the ctor-loaded
		// /Engine BasicShapeMaterial so the ball can never ride the checkerboard default slot.
		UMaterialInterface* Master = Cast<UMaterialInterface>(
			FSoftObjectPath(TEXT("/Game/Materials/M_PF_ArenaMetal.M_PF_ArenaMetal")).TryLoad());
		if (Master == nullptr)
		{
			UE_LOG(CombatForgeLog, Warning,
				TEXT("[Grenade] M_PF_ArenaMetal missing (run Scripts/gen_arena_materials.py) — ball skin falls back to BasicShapeMaterial"));
			Master = BallBaseMaterial;
		}
		if (Master == nullptr)
		{
			UE_LOG(CombatForgeLog, Warning, TEXT("[Grenade] no ball material available — thrown ball will render the default slot"));
			return;
		}
		BallMID = UMaterialInstanceDynamic::Create(Master, this);
	}
	if (BallMID == nullptr)
	{
		return;
	}
	// Frag = dark olive-black shell, smoke = steel grey canister.
	const bool bSmokeKind = (static_cast<EPFGrenadeType>(KindRep) == EPFGrenadeType::Smoke);
	BallMID->SetVectorParameterValue(TEXT("Color"), bSmokeKind
		? FLinearColor(0.7f, 0.73f, 0.78f, 1.f)
		: FLinearColor(0.05f, 0.07f, 0.05f, 1.f));
	Mesh->SetMaterial(0, BallMID);   // explicit — never rely on the default slot
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
	ApplyBallSkin();   // listen host never gets the OnRep

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
		Movement->Deactivate();
	}
	// Drop ALL gameplay collision the moment it pops — the actor may live for the whole smoke
	// lifetime as a visual host; a lingering WorldDynamic collider must never intercept paintballs.
	if (Collision)
	{
		Collision->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Collision->SetCollisionResponseToAllChannels(ECR_Ignore);
	}

	// Authoritative gameplay: frag spawns the real BB burst on the server (damage + host visuals).
	if (Kind == EPFGrenadeType::Frag)
	{
		SpawnFragBurst(At, BurstSeed);
	}
	// Smoke is CONCEALMENT only (bot LOS via UPFSmokeSubsystem). It never collides with pawns/BBs —
	// cosmetic mesh puffs are NoCollision, and the canister collider is already off above.
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
		Movement->Deactivate();
	}
	// Clients: same post-pop collision kill as the authority (no BB-eating leftover collider).
	if (Collision)
	{
		Collision->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Collision->SetCollisionResponseToAllChannels(ECR_Ignore);
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
			BB->UtilityDamageOverride = 1;   // P2-CB2: frag cloud never inherits the thrower's HitValue
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
	// Prefer the translucent volumetric smoke material (soft, noise-broken edges); fall back to the old unlit
	// puff if it hasn't been generated yet (run Scripts/gen_combat_fx.py).
	UMaterialInterface* SmokeMat = Cast<UMaterialInterface>(
		FSoftObjectPath(TEXT("/Game/Materials/M_PF_SmokeVolume.M_PF_SmokeVolume")).TryLoad());
	const bool bVolumetric = (SmokeMat != nullptr);
	if (SmokeMat == nullptr)
	{
		UE_LOG(CombatForgeLog, Warning,
			TEXT("[Grenade] M_PF_SmokeVolume missing (run Scripts/gen_combat_fx.py) — smoke cloud falls back to M_PF_MuzzleSmoke"));
		SmokeMat = Cast<UMaterialInterface>(
			FSoftObjectPath(TEXT("/Game/Materials/M_PF_MuzzleSmoke.M_PF_MuzzleSmoke")).TryLoad());
	}
	// Third guaranteed fallback: the ctor-hard-ref BasicShapeMaterial tinted grey, so puffs can never
	// ride the checkerboard default slot even with zero generated content.
	if (SmokeMat == nullptr)
	{
		UE_LOG(CombatForgeLog, Warning,
			TEXT("[Grenade] M_PF_MuzzleSmoke missing too — smoke cloud falls back to tinted BasicShapeMaterial"));
		SmokeMat = BallBaseMaterial;
	}
	bSmokeVolumetric = bVolumetric;
	SmokeStartTime = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.f;
	SmokeMIDs.Reset();
	PuffTargetScale.Reset();
	PuffMaxDensity.Reset();
	PuffDissolveWindow.Reset();
	PuffYawRateDeg.Reset();
	PuffDriftRate.Reset();
	PuffWobblePhase.Reset();

	// CoD-style cloud: a TIGHT overlapping stack of non-uniform ellipsoids. Uniform spheres with
	// slow billow read as anime soap bubbles; heavy overlap + axis stretch + instant density hide
	// the individual primitives so it reads as one grey bank.
	// Engine sphere is 50 uu radius; BaseScale maps that to SmokeRadius.
	constexpr int32 NumPuffs = 20;
	const float BaseScale = SmokeRadius / 50.f;
	// Seeded from actor location so every client rebuilds the same shape from OnRep.
	FRandomStream Stream(static_cast<int32>(FMath::RoundToInt(At.X * 0.1f)
		^ (FMath::RoundToInt(At.Y * 0.1f) << 8)
		^ (FMath::RoundToInt(At.Z * 0.1f) << 16)
		^ static_cast<int32>(GetUniqueID())));

	for (int32 i = 0; i < NumPuffs; ++i)
	{
		UStaticMeshComponent* Puff = NewObject<UStaticMeshComponent>(this);
		if (Puff == nullptr)
		{
			continue;
		}
		// Core puffs sit denser near the center; outer shells fill the bank. Tighter radial pack
		// than the old "12 clearly-separated balls" layout.
		const float RingT = static_cast<float>(i) / static_cast<float>(NumPuffs - 1);
		const float Radial = SmokeRadius * FMath::Lerp(0.05f, 0.55f, RingT * RingT);
		const float Yaw = Stream.FRandRange(0.f, 360.f);
		const float Pitch = Stream.FRandRange(-25.f, 55.f);
		const FVector Dir = FRotator(Pitch, Yaw, 0.f).Vector();
		const FVector Offset = Dir * Radial + FVector(0.f, 0.f, SmokeRadius * Stream.FRandRange(0.15f, 0.55f));

		// Size: one big core + many mid/small shells that fill silhouette gaps.
		const float SizeMul = (i < 3)
			? Stream.FRandRange(0.95f, 1.15f)
			: Stream.FRandRange(0.45f, 0.85f);
		const FVector Uniform(BaseScale * SizeMul);
		// Stretch into ellipsoids so no puff reads as a perfect circle from most camera angles.
		const FVector Stretch(
			Stream.FRandRange(0.70f, 1.35f),
			Stream.FRandRange(0.70f, 1.35f),
			Stream.FRandRange(0.55f, 1.10f));   // flatter Z → ground-hugging bank, less "ball stack"
		const FVector TargetScale = Uniform * Stretch;

		Puff->SetupAttachment(Collision);
		Puff->SetStaticMesh(Sphere);
		Puff->SetRelativeLocation(Offset);
		// Random orientation so the stretched axes don't align into a grid of ovals.
		Puff->SetRelativeRotation(FRotator(Stream.FRandRange(-40.f, 40.f),
			Stream.FRandRange(0.f, 360.f), Stream.FRandRange(-30.f, 30.f)));
		// Start almost full-size — CoD pops, it doesn't grow from a pea.
		Puff->SetRelativeScale3D(TargetScale * 0.55f);
		// CRITICAL: smoke meshes never block anything (pawns, paintballs, traces).
		Puff->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Puff->SetCollisionResponseToAllChannels(ECR_Ignore);
		Puff->SetCastShadow(false);
		Puff->SetCanEverAffectNavigation(false);
		Puff->SetGenerateOverlapEvents(false);
		Puff->bReceivesDecals = false;
		Puff->RegisterComponent();

		UMaterialInstanceDynamic* PuffMID = nullptr;
		if (SmokeMat != nullptr)
		{
			PuffMID = UMaterialInstanceDynamic::Create(SmokeMat, this);
			if (PuffMID != nullptr)
			{
				// Cool grey bank — slight per-puff variation so the mass isn't one flat tone.
				const float Tint = Stream.FRandRange(0.52f, 0.68f);
				const FLinearColor Grey(Tint, Tint, Tint + 0.02f, 1.f);
				if (bVolumetric)
				{
					PuffMID->SetVectorParameterValue(TEXT("SmokeColor"), Grey);
					PuffMID->SetScalarParameterValue(TEXT("Density"), 0.f);   // ramps in Tick
				}
				else
				{
					PuffMID->SetVectorParameterValue(TEXT("EmissiveColor"), Grey);
					PuffMID->SetVectorParameterValue(TEXT("Color"), Grey);
				}
				Puff->SetMaterial(0, PuffMID);   // explicit — never rely on the default slot
			}
		}

		// Staggered dissolve: outer shells thin first, core last — cloud frays, doesn't pop off.
		SmokePuffs.Add(Puff);
		SmokeMIDs.Add(PuffMID);
		PuffTargetScale.Add(TargetScale);
		// Dense enough to conceal; material noise keeps it from reading as solid grey balls.
		PuffMaxDensity.Add(Stream.FRandRange(0.95f, 1.25f));
		PuffDissolveWindow.Add(FMath::Lerp(3.2f, 0.9f, RingT));
		// Almost no spin — rotating spheres is what made the old cloud look like anime bubbles.
		PuffYawRateDeg.Add(Stream.FRandRange(0.4f, 1.6f) * (Stream.RandRange(0, 1) == 0 ? 1.f : -1.f));
		PuffDriftRate.Add(Stream.FRandRange(2.f, 5.f));
		PuffWobblePhase.Add(Stream.FRandRange(0.f, 2.f * UE_PI));
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
	// Instant pop (SmoothStep over SmokeAppearDur ≈ 0.12 s) — opaque bank in a blink, CoD-style.
	const float Appear = FMath::SmoothStep(0.f, SmokeAppearDur, Elapsed);
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
		// Volumetric fades via Density so keep size; unlit fallback shrinks away (no opacity param).
		const float FadeScaleTarget = bSmokeVolumetric ? 0.92f : 0.05f;
		// Start at 55% size, hit 100% almost immediately — no slow balloon growth.
		const float ScaleFactor = FMath::Lerp(0.55f, 1.f, Appear) * FMath::Lerp(1.f, FadeScaleTarget, Fade);
		// Tiny breathing only (±3%) — larger wobble made each sphere pulse like a cartoon bubble.
		const float Phase = PuffWobblePhase.IsValidIndex(i) ? PuffWobblePhase[i] : 0.f;
		const float Wobble = 1.f + 0.03f * FMath::Sin(Elapsed * 1.1f + Phase);
		SmokePuffs[i]->SetRelativeScale3D(PuffTargetScale[i] * ScaleFactor * Wobble);
		// Barely-there crawl (keeps the bank alive without spinning balls).
		if (PuffYawRateDeg.IsValidIndex(i))
		{
			SmokePuffs[i]->AddLocalRotation(FRotator(0.f, PuffYawRateDeg[i] * DeltaSeconds, 0.f));
		}
		if (PuffDriftRate.IsValidIndex(i))
		{
			SmokePuffs[i]->AddRelativeLocation(FVector(0.f, 0.f, PuffDriftRate[i] * Appear * DeltaSeconds));
		}
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
