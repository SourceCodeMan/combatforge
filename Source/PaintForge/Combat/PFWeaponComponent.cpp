// Copyright (c) 2026 Tom Chapman. All rights reserved.

#include "Combat/PFWeaponComponent.h"

#include "PaintForge.h"
#include "Combat/PFCombatAudio.h"
#include "Core/PFUserPrefs.h"
#include "Combat/PFHealthComponent.h"
#include "Combat/PFPaintballProjectile.h"
#include "Combat/PFSplatSubsystem.h"
#include "Core/PaintForgeGameState.h"
#include "Core/PaintForgePlayerState.h"
#include "Player/PaintForgeCharacter.h"
#include "Player/PFCameraShakes.h"
#include "Player/PFCharacterMovementComponent.h"
#include "Engine/World.h"
#include "GameFramework/Controller.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "Net/UnrealNetwork.h"
#include "Templates/TypeHash.h"

namespace
{
	// Slightly forgiving for high-latency kids / listen-host (still rejects cheats).
	constexpr float ServerOriginToleranceUU = 200.f;   // 04 §5.1 anti-teleport-fire
	constexpr float ServerDirToleranceDeg = 5.5f;      // 04 §5.1 — ADS micro-desync + jitter
	constexpr float FireTokenCap = 3.f;                // 04 §2.1 token bucket
}

UPFWeaponComponent::UPFWeaponComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	SetIsReplicatedByDefault(true);   // required for component RPCs + owner-only props (§5.2)
}

void UPFWeaponComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME_CONDITION(UPFWeaponComponent, HopperCount, COND_OwnerOnly);
	DOREPLIFETIME_CONDITION(UPFWeaponComponent, bReloading, COND_OwnerOnly);
}

void UPFWeaponComponent::BeginPlay()
{
	Super::BeginPlay();

	// Local loadout marker preset (rate + hopper) before first fill.
	FPFUserPrefs::ApplyMarkerPresetToWeapon(this);

	// Keep the live count honest against a data-only-BP-tuned capacity (§4.1 tunables).
	if (GetOwnerRole() == ROLE_Authority)
	{
		HopperCount = HopperCapacity;
		OnHopperChangedEvent.Broadcast(HopperCount);
	}
}

// ---------------------------------------------------------------- input entry

void UPFWeaponComponent::StartFire()
{
	bWantsFire = true;
	// First shot fires on press instantly — no spin-up (04 §2.1).
	if (const UWorld* World = GetWorld())
	{
		TryFire(World->GetTimeSeconds());
	}
}

void UPFWeaponComponent::StopFire()
{
	bWantsFire = false;
}

void UPFWeaponComponent::StartReload()
{
	APaintForgeCharacter* Char = GetPFCharacter();
	if (Char == nullptr || !Char->IsLocallyControlled())
	{
		return;
	}
	if (bReloading || HopperCount >= HopperCapacity)
	{
		return;
	}
	if (const UPFHealthComponent* Health = Char->GetHealth();
		Health != nullptr && Health->bEliminated)
	{
		return;
	}
	// Sprint/slide would cancel it on the very next tick — reject up front (04 §3).
	if (const UPFCharacterMovementComponent* CMC = Char->GetPFMovement())
	{
		if (CMC->IsSprintingEffective() || CMC->IsSliding())
		{
			return;
		}
	}

	BeginReload(GetWorld()->GetTimeSeconds());
	if (!Char->HasAuthority())
	{
		ServerStartReload();
	}
}

// ---------------------------------------------------------------- tick

void UPFWeaponComponent::TickComponent(float DeltaTime, ELevelTick TickType,
	FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	const UWorld* World = GetWorld();
	if (World == nullptr)
	{
		return;
	}
	const double Now = World->GetTimeSeconds();

	UpdateReload(Now);

	const APaintForgeCharacter* Char = GetPFCharacter();
	if (Char != nullptr && Char->IsLocallyControlled())
	{
		TryFire(Now);
	}
}

// ---------------------------------------------------------------- fire pipeline (client)

bool UPFWeaponComponent::PassesCommonFireGates(const APaintForgeCharacter& Char) const
{
	// Phase gate (T21): fire is valid only in Lobby (warm-up pen) and Combat/Live.
	// Same predicate client and server; the client check is UX, the server check is law.
	const APaintForgeGameState* GS = GetPFGameState();
	if (GS == nullptr || !GS->IsFireAllowed())
	{
		return false;
	}
	const UPFHealthComponent* Health = Char.GetHealth();
	if (Health != nullptr && Health->bEliminated)
	{
		return false;
	}
	if (bReloading)   // uninterruptible by fire (04 §3); bWantsFire buffers through it
	{
		return false;
	}
	return true;
}

void UPFWeaponComponent::TryFire(double Now)
{
	if (!bWantsFire)
	{
		return;
	}
	APaintForgeCharacter* Char = GetPFCharacter();
	if (Char == nullptr || !Char->IsLocallyControlled())
	{
		return;
	}
	if (!PassesCommonFireGates(*Char))
	{
		return;
	}

	// Sprint blocks fire: the fire input cancels sprint and starts the 0.18 s raise;
	// the (still held) fire buffers and releases when the raise ends (04 §1.1).
	UPFCharacterMovementComponent* CMC = Char->GetPFMovement();
	if (CMC != nullptr && CMC->IsSprintingEffective())
	{
		CMC->SetWantsToSprint(false);
		SprintOutReadyTime = Now + SprintOutTime;
		return;
	}
	if (Now < SprintOutReadyTime || Now < NextFireTime)
	{
		return;
	}

	if (HopperCount == 0)
	{
		// Auto-reload on empty with buffered fire (04 §3).
		BeginReload(Now);
		if (!Char->HasAuthority())
		{
			ServerStartReload();
		}
		return;
	}

	FireOneShot(Now);

	// 12 bps accumulator with remainder carry — no frame-quantized ROF, no catch-up
	// bursts after a pause (04 §2.1).
	const double Interval = 1.0 / static_cast<double>(FMath::Max(FireRateBps, 1.f));
	NextFireTime = (Now - NextFireTime > Interval) ? (Now + Interval) : (NextFireTime + Interval);
}

void UPFWeaponComponent::FireOneShot(double Now)
{
	APaintForgeCharacter* Char = GetPFCharacter();
	const APlayerState* PS = (Char != nullptr) ? Char->GetPlayerState() : nullptr;
	if (Char == nullptr || PS == nullptr)
	{
		return;   // PlayerState is the spread-seed identity (B3) — no seed, no shot
	}

	++ShotIndexCounter;

	const AController* Controller = Char->GetController();
	const FVector BaseDir = (Controller != nullptr)
		? Controller->GetControlRotation().Vector()
		: Char->GetBaseAimRotation().Vector();

	// DO-NOT-TOUCH (B3, §5.15): exactly ONE VRandCone pull per shot from the shared
	// deterministic stream. Any extra pull on either side desyncs every later shot.
	const float HalfAngleDeg = GetCurrentSpreadHalfAngleDeg();
	FRandomStream Stream = MakeShotStream(PS->GetPlayerId(), ShotIndexCounter);
	const FVector SpreadedDir = Stream.VRandCone(BaseDir, FMath::DegreesToRadians(HalfAngleDeg));

	// Raise TP gun + FP recoil BEFORE sampling muzzle so balls leave the aim-line barrel,
	// not the hip-carry tip.
	Char->OnFireCosmetic();

	if (!Char->HasAuthority())
	{
		// Owning-client cosmetic: instant tracer from the camera muzzle (04 §5.1). A listen
		// host skips this — its authoritative projectile spawns this same frame and IS the
		// visual (no double tracer).
		SpawnCosmeticProjectile(Char->GetMuzzleLocation(true), SpreadedDir, GetOwnerTeam(),
			ShotIndexCounter);

		// Predicted hopper; COND_OwnerOnly replication corrects any divergence.
		HopperCount = (HopperCount > 0) ? static_cast<uint8>(HopperCount - 1) : 0;
		OnHopperChangedEvent.Broadcast(HopperCount);
	}

	if (!Char->HasAuthority())
	{
		// Client-side bloom for the next predicted shot. A listen host skips this:
		// ServerFire_Implementation (invoked synchronously below) is its single accumulation —
		// registering here too would double the host's bloom (+0.24°/shot vs +0.12°).
		RegisterShotBloom(Now);
	}

	// Muzzle feel (04 §4): fire shake doubles as recoil (feel-only; bloom is the spray cost).
	if (APlayerController* PC = Cast<APlayerController>(Char->GetController()))
	{
		PC->ClientStartCameraShake(UPFFireShake::StaticClass());
	}
	if (UPFCombatAudio* Audio = Char->GetCombatAudio())
	{
		Audio->PlayMuzzle();
	}

	FPFShotPacket Packet;
	Packet.Origin = Char->GetMuzzleLocation(false);   // server-muzzle convention (04 §2.2)
	Packet.Dir = BaseDir;                              // BASE dir — server applies the same spread
	Packet.ShotIndex = ShotIndexCounter;
	Packet.ClientTime = static_cast<float>(Now);       // reserved for post-v1 rewind (04 §5.2)
	ServerFire(Packet);

	if (!Char->HasAuthority() && HopperCount == 0)
	{
		BeginReload(Now);   // predicted auto-reload; server mirrors on its own decrement
	}
}

void UPFWeaponComponent::SpawnCosmeticProjectile(const FVector& Origin, const FVector& SpreadedDir,
	uint8 Team, uint32 ShotIndex)
{
	UWorld* World = GetWorld();
	if (World == nullptr)
	{
		return;
	}
	UPFSplatSubsystem* Splats = World->GetSubsystem<UPFSplatSubsystem>();
	APFPaintballProjectile* Ball = (Splats != nullptr) ? Splats->AcquireCosmeticProjectile() : nullptr;
	if (Ball != nullptr)
	{
		Ball->InitProjectile(Origin, SpreadedDir, Team, /*bAuthoritative=*/false, this, ShotIndex);
	}
}

// ---------------------------------------------------------------- fire pipeline (server)

void UPFWeaponComponent::ServerFire_Implementation(const FPFShotPacket& Shot)
{
	UWorld* World = GetWorld();
	APaintForgeCharacter* Char = GetPFCharacter();
	if (World == nullptr || Char == nullptr)
	{
		return;
	}
	APaintForgePlayerState* PS = Char->GetPlayerState<APaintForgePlayerState>();
	if (PS == nullptr)
	{
		return;
	}
	const double Now = World->GetTimeSeconds();

	// Monotonic shot index — rejects replays/reordering abuse.
	if (Shot.ShotIndex <= LastServerShotIndex)
	{
		UE_LOG(PaintForgeLog, Warning, TEXT("ServerFire reject (%s): non-monotonic ShotIndex %u <= %u"),
			*GetNameSafe(Char), Shot.ShotIndex, LastServerShotIndex);
		return;
	}

	// Token bucket: cap 3, refill 12/s — tolerates jitter bursts, rejects macros (04 §2.1).
	const float RefillRate = FMath::Max(1.f, FireRateBps);
	FireTokens = FMath::Min(FireTokenCap,
		FireTokens + static_cast<float>(Now - LastTokenRefillTime) * RefillRate);
	LastTokenRefillTime = Now;
	if (FireTokens < 1.f)
	{
		UE_LOG(PaintForgeLog, Warning, TEXT("ServerFire reject (%s): ROF token bucket empty"),
			*GetNameSafe(Char));
		return;
	}

	// Phase gate (T21): Freeze/Intermission/Build fire dies here regardless of client UX.
	const APaintForgeGameState* GS = GetPFGameState();
	if (GS == nullptr || !GS->IsFireAllowed())
	{
		UE_LOG(PaintForgeLog, Verbose, TEXT("ServerFire reject (%s): fire not allowed in phase"),
			*GetNameSafe(Char));
		return;
	}

	const UPFHealthComponent* Health = Char->GetHealth();
	if (Health != nullptr && Health->bEliminated)
	{
		return;
	}
	if (bReloading)
	{
		return;   // client/server reload-window skew — silently drop
	}
	if (HopperCount == 0)
	{
		BeginReload(Now);   // authoritative auto-reload
		return;
	}

	// Origin within 150 uu of the server-side muzzle (anti-teleport-fire).
	const FVector ServerMuzzle = Char->GetMuzzleLocation(false);
	if (FVector::DistSquared(FVector(Shot.Origin), ServerMuzzle) >
		FMath::Square(ServerOriginToleranceUU))
	{
		UE_LOG(PaintForgeLog, Warning, TEXT("ServerFire reject (%s): origin %.0f uu from muzzle"),
			*GetNameSafe(Char), FVector::Dist(FVector(Shot.Origin), ServerMuzzle));
		return;
	}

	// Dir within 4° of the server's view of the client aim.
	const FVector BaseDir = FVector(Shot.Dir).GetSafeNormal();
	const FVector ServerView = Char->GetBaseAimRotation().Vector();
	if (FVector::DotProduct(BaseDir, ServerView) <
		FMath::Cos(FMath::DegreesToRadians(ServerDirToleranceDeg)))
	{
		UE_LOG(PaintForgeLog, Warning, TEXT("ServerFire reject (%s): dir outside %g deg of server view"),
			*GetNameSafe(Char), ServerDirToleranceDeg);
		return;
	}

	FireTokens -= 1.f;
	LastServerShotIndex = Shot.ShotIndex;

	// DO-NOT-TOUCH (B3, §5.15): SAME stream, SAME single VRandCone pull as the owning client.
	const float HalfAngleDeg = GetCurrentSpreadHalfAngleDeg();
	FRandomStream Stream = MakeShotStream(PS->GetPlayerId(), Shot.ShotIndex);
	const FVector SpreadedDir = Stream.VRandCone(BaseDir, FMath::DegreesToRadians(HalfAngleDeg));

	RegisterShotBloom(Now);

	HopperCount = static_cast<uint8>(HopperCount - 1);
	OnHopperChangedEvent.Broadcast(HopperCount);   // host UI (§5.9); remote owner via rep

	// Authoritative, never-replicated projectile (B3).
	FActorSpawnParameters SpawnParams;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	SpawnParams.Owner = Char;
	SpawnParams.Instigator = Char;
	APFPaintballProjectile* Ball = World->SpawnActor<APFPaintballProjectile>(
		APFPaintballProjectile::StaticClass(), FVector(Shot.Origin), SpreadedDir.Rotation(),
		SpawnParams);
	if (Ball != nullptr)
	{
		Ball->InitProjectile(FVector(Shot.Origin), SpreadedDir, PS->TeamId, /*bAuthoritative=*/true,
			this, Shot.ShotIndex);
	}

	UE_LOG(PaintForgeLog, Verbose, TEXT("ServerFire accept (%s): shot %u, spread %.2f deg"),
		*GetNameSafe(Char), Shot.ShotIndex, HalfAngleDeg);

	// Remote viewers spawn their own cosmetic (unreliable — a lost tracer is just a lost tracer).
	MulticastShotFX(Shot.Origin, Shot.Dir, Shot.ShotIndex);

	if (HopperCount == 0)
	{
		BeginReload(Now);
	}
}

void UPFWeaponComponent::MulticastShotFX_Implementation(FVector_NetQuantize100 Origin,
	FVector_NetQuantizeNormal Dir, uint32 ShotIndex)
{
	AActor* Owner = GetOwner();
	if (Owner == nullptr || Owner->HasAuthority())
	{
		return;   // server / listen host: the authoritative projectile is already the visual
	}
	APawn* OwnerPawn = Cast<APawn>(Owner);
	if (OwnerPawn != nullptr && OwnerPawn->IsLocallyControlled())
	{
		return;   // owning client already fired its cosmetic (04 §5.1 step 4)
	}
	const UWorld* World = GetWorld();
	if (World == nullptr)
	{
		return;
	}

	// Track the shooter's bloom on this viewer so the estimated cone stays honest.
	RegisterShotBloom(World->GetTimeSeconds());

	int32 PlayerId = 0;
	if (OwnerPawn != nullptr)
	{
		if (const APlayerState* PS = OwnerPawn->GetPlayerState())
		{
			PlayerId = PS->GetPlayerId();
		}
	}

	// Same seed as shooter/server; the cone half-angle is this viewer's best local estimate
	// (cosmetic-only divergence, accepted by B3).
	const float HalfAngleDeg = GetCurrentSpreadHalfAngleDeg();
	FRandomStream Stream = MakeShotStream(PlayerId, ShotIndex);
	const FVector SpreadedDir =
		Stream.VRandCone(FVector(Dir).GetSafeNormal(), FMath::DegreesToRadians(HalfAngleDeg));

	SpawnCosmeticProjectile(FVector(Origin), SpreadedDir, GetOwnerTeam(), ShotIndex);

	if (APaintForgeCharacter* Char = GetPFCharacter())
	{
		if (UPFCombatAudio* Audio = Char->GetCombatAudio())
		{
			Audio->PlayMuzzle();
		}
		Char->OnRemoteFireCosmetic();   // airsoft: no flash (hook reserved)
	}
}

void UPFWeaponComponent::MulticastImpactSplat_Implementation(FVector_NetQuantize100 Loc,
	FVector_NetQuantizeNormal Normal, uint8 Team)
{
	UWorld* World = GetWorld();
	if (World == nullptr || World->GetNetMode() == NM_DedicatedServer)
	{
		return;
	}
	if (UPFSplatSubsystem* Splats = World->GetSubsystem<UPFSplatSubsystem>())
	{
		// Owning client reconciles its pending splat @75 uu inside SpawnConfirmedSplat.
		Splats->SpawnConfirmedSplat(FVector(Loc), FVector(Normal), Team);
	}
}

void UPFWeaponComponent::ClientHitConfirm_Implementation(uint32 ShotIndex, bool bElimHit)
{
	// The ONLY source of hitmarkers (B3). UPFCombatFeedbackWidget renders + plays audio.
	OnHitConfirmedEvent.Broadcast(ShotIndex, bElimHit);
}

// ---------------------------------------------------------------- reload

void UPFWeaponComponent::ServerStartReload_Implementation()
{
	const APaintForgeCharacter* Char = GetPFCharacter();
	if (Char == nullptr || bReloading || HopperCount >= HopperCapacity)
	{
		return;
	}
	if (const UPFHealthComponent* Health = Char->GetHealth();
		Health != nullptr && Health->bEliminated)
	{
		return;
	}
	if (const UPFCharacterMovementComponent* CMC = Char->GetPFMovement())
	{
		if (CMC->IsSprintingEffective() || CMC->IsSliding())
		{
			return;
		}
	}
	if (const UWorld* World = GetWorld())
	{
		BeginReload(World->GetTimeSeconds());
	}
}

void UPFWeaponComponent::BeginReload(double Now)
{
	if (bReloading || HopperCount >= HopperCapacity)
	{
		return;
	}
	bReloading = true;
	ReloadEndTime = Now + ReloadTime;

	APaintForgeCharacter* Char = GetPFCharacter();
	if (Char != nullptr && Char->IsLocallyControlled())
	{
		// Reload kicks you out of ADS for the 1.0 s (04 §3).
		bWasADSBeforeReload = Char->IsADS();
		if (bWasADSBeforeReload)
		{
			Char->SetADS(false);
		}
		if (UPFCombatAudio* Audio = Char->GetCombatAudio())
		{
			Audio->PlayReload();
		}
	}

	OnReloadStateChangedEvent.Broadcast(true);   // predicted/host broadcast; remote owner via OnRep
}

void UPFWeaponComponent::FinishReload()
{
	HopperCount = HopperCapacity;
	bReloading = false;
	OnHopperChangedEvent.Broadcast(HopperCount);
	OnReloadStateChangedEvent.Broadcast(false);

	APaintForgeCharacter* Char = GetPFCharacter();
	if (Char != nullptr && Char->IsLocallyControlled() && bWasADSBeforeReload)
	{
		// CONTRACT-GAP: 04 §3 says "auto re-ADS if still held", but §3.3 exposes no raw
		// ADS-held query on the character. Approximated as "held when the reload began";
		// a release during the 1.0 s window is corrected by the next ADS input event.
		Char->SetADS(true);
	}
	bWasADSBeforeReload = false;
}

void UPFWeaponComponent::CancelReload()
{
	if (!bReloading)
	{
		return;
	}
	// "Restores prior count" (04 §3) is free: the hopper only fills on FinishReload.
	bReloading = false;
	bWasADSBeforeReload = false;
	OnReloadStateChangedEvent.Broadcast(false);
}

void UPFWeaponComponent::UpdateReload(double Now)
{
	if (!bReloading)
	{
		return;
	}
	const APaintForgeCharacter* Char = GetPFCharacter();
	if (Char == nullptr)
	{
		return;
	}
	// Reload state is managed where it was started: authority, and the predicting owner.
	if (!Char->HasAuthority() && !Char->IsLocallyControlled())
	{
		return;
	}

	// Sprint/slide cancels the pod flip (04 §3).
	if (const UPFCharacterMovementComponent* CMC = Char->GetPFMovement())
	{
		if (CMC->IsSprintingEffective() || CMC->IsSliding())
		{
			CancelReload();
			return;
		}
	}

	if (Now >= ReloadEndTime)
	{
		FinishReload();
	}
}

// ---------------------------------------------------------------- spread & bloom

float UPFWeaponComponent::GetCurrentSpreadHalfAngleDeg() const
{
	const APaintForgeCharacter* Char = GetPFCharacter();
	const UWorld* World = GetWorld();
	if (Char == nullptr || World == nullptr)
	{
		return SpreadHip;
	}
	const UPFCharacterMovementComponent* CMC = Char->GetPFMovement();
	// B3 requires client and server to compute the SAME half-angle. That holds only if
	// pkg-character advances GetADSAlpha() on ALL roles (server-side from the CMC's
	// replicated ADS compressed flag), not just on the locally-controlled instance.
	const float ADSAlpha = FMath::Clamp(Char->GetADSAlpha(), 0.f, 1.f);

	// Base: hip 1.5 / hip-moving (>50% walk) 2.0, lerped to tight ADS by the transition
	// alpha (04 §2.3 — fire is allowed at any point of the ADS transition).
	float HipBase = SpreadHip;
	if (CMC != nullptr && Char->GetVelocity().Size2D() > CMC->WalkSpeed * 0.5f)
	{
		HipBase = SpreadHipMoving;
	}
	float Spread = FMath::Lerp(HipBase, SpreadADS, ADSAlpha);

	if (Char->bIsCrouched)
	{
		Spread *= SpreadCrouchMult;
	}
	// Air / slide penalties fade out as you ADS so aimed shots stay precise.
	const float HipOnly = 1.f - ADSAlpha;
	if (CMC != nullptr && CMC->IsFalling())
	{
		Spread += SpreadAirAdd * HipOnly;
	}
	if (CMC != nullptr && CMC->IsSliding())
	{
		Spread += SpreadSlideAdd * HipOnly;   // slide fire is mostly hipfire (04 §1.2)
	}

	// Bloom nearly vanishes while fully ADS (BloomADSMult ~0.12).
	const float Bloom = GetEffectiveBloomDeg(World->GetTimeSeconds());
	Spread += Bloom * FMath::Lerp(1.f, BloomADSMult, ADSAlpha);

	return Spread;
}

FRandomStream UPFWeaponComponent::MakeShotStream(int32 PlayerId, uint32 ShotIndex)
{
	// DO-NOT-TOUCH (B3): seed = (int32)HashCombine((uint32)PlayerId, ShotIndex).
	return FRandomStream(static_cast<int32>(HashCombine(static_cast<uint32>(PlayerId), ShotIndex)));
}

float UPFWeaponComponent::GetEffectiveBloomDeg(double Now) const
{
	const double SinceLastShot = Now - LastShotTime;
	if (SinceLastShot <= BloomDecayDelay)
	{
		return BloomAccumDeg;
	}
	// Decays 6°/s starting 0.15 s after the last shot (04 §2.3).
	return FMath::Max(0.f,
		BloomAccumDeg - BloomDecayPerSec * static_cast<float>(SinceLastShot - BloomDecayDelay));
}

void UPFWeaponComponent::RegisterShotBloom(double Now)
{
	BloomAccumDeg = FMath::Min(GetEffectiveBloomDeg(Now) + BloomPerShot, BloomCap);
	LastShotTime = Now;
}

// ---------------------------------------------------------------- OnReps & helpers

void UPFWeaponComponent::OnRep_Hopper()
{
	OnHopperChangedEvent.Broadcast(HopperCount);
}

void UPFWeaponComponent::OnRep_Reload()
{
	OnReloadStateChangedEvent.Broadcast(bReloading);
}

APaintForgeCharacter* UPFWeaponComponent::GetPFCharacter() const
{
	return Cast<APaintForgeCharacter>(GetOwner());
}

APaintForgeGameState* UPFWeaponComponent::GetPFGameState() const
{
	const UWorld* World = GetWorld();
	return (World != nullptr) ? World->GetGameState<APaintForgeGameState>() : nullptr;
}

uint8 UPFWeaponComponent::GetOwnerTeam() const
{
	if (const APaintForgeCharacter* Char = GetPFCharacter())
	{
		if (const APaintForgePlayerState* PS = Char->GetPlayerState<APaintForgePlayerState>())
		{
			return PS->TeamId;
		}
	}
	return 255;
}
