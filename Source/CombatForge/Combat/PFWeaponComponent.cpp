// Copyright (c) 2026 Tom Chapman. All rights reserved.

#include "Combat/PFWeaponComponent.h"

#include "CombatForge.h"
#include "Combat/PFCombatAudio.h"
#include "Combat/PFCombatVFX.h"
#include "Combat/PFHealthComponent.h"
#include "Combat/PFPaintballProjectile.h"
#include "Combat/PFGrenadeProjectile.h"
#include "Combat/PFSplatSubsystem.h"
#include "Core/CombatForgeGameState.h"
#include "Core/CombatForgePlayerState.h"
#include "Player/CombatForgeCharacter.h"
#include "Player/PFCameraShakes.h"
#include "Player/PFCharacterMovementComponent.h"
#include "Engine/World.h"
#include "Perception/AISense_Hearing.h"   // gunfire noise → bots hear + investigate
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
	constexpr float ServerDirToleranceDeg = 9.f;       // 04 §5.1 — ADS micro-desync + jitter; wider since crosshair
	                                                   // convergence amplifies small client/server aim skew at close range
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
	DOREPLIFETIME_CONDITION(UPFWeaponComponent, ReserveAmmo, COND_OwnerOnly);
	DOREPLIFETIME_CONDITION(UPFWeaponComponent, bReloading, COND_OwnerOnly);
	DOREPLIFETIME_CONDITION(UPFWeaponComponent, FragCount, COND_OwnerOnly);
	DOREPLIFETIME_CONDITION(UPFWeaponComponent, SmokeCount, COND_OwnerOnly);
}

void UPFWeaponComponent::BeginPlay()
{
	Super::BeginPlay();

	// (The old global "marker preset" is gone — mag size, ROF, spread, and range are all per-weapon now,
	// applied from PFWeaponCatalog in ApplyWeaponLoadout.)

	// Start with a full mag + full reserve + grenade loadout on authority (clients get COND_OwnerOnly rep).
	if (GetOwnerRole() == ROLE_Authority)
	{
		HopperCount = HopperCapacity;
		ReserveAmmo = MaxReserveAmmo;
		FragCount  = MaxFrag;
		SmokeCount = MaxSmoke;
		OnHopperChangedEvent.Broadcast(HopperCount);
		OnGrenadeCountChangedEvent.Broadcast(FragCount, SmokeCount);
	}
}

// ---------------------------------------------------------------- input entry

void UPFWeaponComponent::StartFire()
{
	const UWorld* World = GetWorld();
	const double Now = World ? World->GetTimeSeconds() : 0.0;

	// Burst-DMR re-burst gate (Rifle 03): refuse to arm a new pull until the delay elapses.
	if (ReburstDelaySec > 0.f && CurrentFireMode == EPFFireMode::Burst && Now < NextBurstAllowedTime)
	{
		return;
	}

	bWantsFire = true;
	ShotsThisPull = 0;   // new trigger pull re-arms Single/Burst caps

	// Minigun spin-up: cold start waits SpinupSec; feathering within grace keeps barrels hot.
	if (SpinupSec > 0.f)
	{
		if (Now > SpinGraceUntil)
		{
			SpinReadyTime = Now + static_cast<double>(SpinupSec);
		}
		else
		{
			SpinReadyTime = Now;   // still spinning — fire immediately
		}
	}
	else
	{
		SpinReadyTime = 0.0;
		if (World)
		{
			TryFire(Now);
		}
	}
}

void UPFWeaponComponent::StopFire()
{
	bWantsFire = false;
	if (const UWorld* World = GetWorld())
	{
		// Short grace so feathering the trigger doesn't re-spin the minigun every time.
		if (SpinupSec > 0.f)
		{
			SpinGraceUntil = World->GetTimeSeconds() + 0.4;
		}
	}
}

void UPFWeaponComponent::StartReload()
{
	ACombatForgeCharacter* Char = GetPFCharacter();
	if (Char == nullptr || !Char->IsLocallyControlled())
	{
		return;
	}
	// Need room in the mag AND spare balls to load.
	if (bReloading || HopperCount >= HopperCapacity || ReserveAmmo <= 0)
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

	const ACombatForgeCharacter* Char = GetPFCharacter();
	if (Char != nullptr && Char->IsLocallyControlled())
	{
		TryFire(Now);

		// Recoil-climb recovery: once the trigger is released, ease the accumulated climb back so the aim
		// returns to where it started — reward for firing in bursts instead of holding.
		if ((RecoilClimbPitch > 0.f || !FMath::IsNearlyZero(RecoilClimbYaw))
			&& !bWantsFire && (Now - LastClimbShotTime) > 0.12)
		{
			if (APlayerController* PC = Cast<APlayerController>(Char->GetController()))
			{
				const float Total = RecoilClimbPitch + FMath::Abs(RecoilClimbYaw);
				const float Step = FMath::Min(Total, ClimbRecoverDegPerSec * DeltaTime);
				const float FracP = (Total > KINDA_SMALL_NUMBER) ? RecoilClimbPitch / Total : 0.f;
				const float StepP = Step * FracP;
				const float StepY = Step * (1.f - FracP) * FMath::Sign(RecoilClimbYaw);
				FRotator CR = PC->GetControlRotation().GetNormalized();
				CR.Pitch -= StepP;
				CR.Yaw -= StepY;
				PC->SetControlRotation(CR);
				RecoilClimbPitch = FMath::Max(0.f, RecoilClimbPitch - StepP);
				RecoilClimbYaw -= StepY;
				if (RecoilClimbPitch < 0.02f && FMath::Abs(RecoilClimbYaw) < 0.02f)
				{
					RecoilClimbPitch = 0.f;
					RecoilClimbYaw = 0.f;
				}
			}
		}
	}
}

// ---------------------------------------------------------------- fire pipeline (client)

bool UPFWeaponComponent::PassesCommonFireGates(const ACombatForgeCharacter& Char) const
{
	// Phase gate (T21): fire is valid only in Lobby (warm-up pen) and Combat/Live.
	// Same predicate client and server; the client check is UX, the server check is law.
	const ACombatForgeGameState* GS = GetPFGameState();
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
	ACombatForgeCharacter* Char = GetPFCharacter();
	if (Char == nullptr || !Char->IsLocallyControlled())
	{
		return;
	}
	if (!PassesCommonFireGates(*Char))
	{
		return;
	}

	// Fire selector (client-local feel): cap shots per trigger pull. Single = 1, Burst = BurstCount,
	// Auto = unlimited. Latched until the trigger is released and pulled again (StartFire resets the count).
	const uint8 PullCap = (CurrentFireMode == EPFFireMode::Single) ? 1
	                    : (CurrentFireMode == EPFFireMode::Burst)  ? BurstCount
	                    : TNumericLimits<uint8>::Max();
	if (ShotsThisPull >= PullCap)
	{
		return;
	}
	if (ReburstDelaySec > 0.f && CurrentFireMode == EPFFireMode::Burst && Now < NextBurstAllowedTime)
	{
		return;
	}
	// Minigun spin-up must complete before the first BB.
	if (SpinupSec > 0.f && Now < SpinReadyTime)
	{
		return;
	}

	// Sprint blocks fire: the fire input cancels sprint and starts the 0.18 s raise;
	// the (still held) fire buffers and releases when the raise ends (04 §1.1).
	UPFCharacterMovementComponent* CMC = Char->GetPFMovement();
	if (CMC != nullptr && CMC->IsSprintingEffective())
	{
		CMC->SetWantsToSprint(false);
		SprintOutReadyTime = Now + static_cast<double>(SprintOutTime);
		return;
	}
	if (Now < SprintOutReadyTime || Now < NextFireTime)
	{
		return;
	}

	if (HopperCount == 0)
	{
		// A Single/Burst pull interrupted by an empty mag does NOT resume after the auto-reload — one pull
		// is one shot/burst. Latch it closed so a held trigger doesn't fire again when the reload finishes.
		if (CurrentFireMode != EPFFireMode::Auto && ShotsThisPull > 0)
		{
			ShotsThisPull = PullCap;
		}
		// Auto-reload on empty with buffered fire (04 §3).
		BeginReload(Now);
		if (!Char->HasAuthority())
		{
			ServerStartReload();
		}
		return;
	}

	FireOneShot(Now);
	if (CurrentFireMode != EPFFireMode::Auto)
	{
		++ShotsThisPull;   // Auto never counts, so it can't reach the uint8 cap and stall mid-hold
		// Rifle 03 re-burst: after a full 3-shot pull, block the next pull for ReburstDelaySec.
		if (CurrentFireMode == EPFFireMode::Burst && ReburstDelaySec > 0.f && ShotsThisPull >= PullCap)
		{
			NextBurstAllowedTime = Now + static_cast<double>(ReburstDelaySec);
		}
	}

	// ROF accumulator with remainder carry — no frame-quantized ROF, no catch-up
	// bursts after a pause (04 §2.1).
	const double Interval = 1.0 / static_cast<double>(FMath::Max(FireRateBps, 1.f));
	NextFireTime = (Now - NextFireTime > Interval) ? (Now + Interval) : (NextFireTime + Interval);
}

// Where the CROSSHAIR points in the world: trace from the shooter's eye along their aim to the first hit
// (or a far point if none). Shots converge on THIS so they land on the reticle instead of flying parallel
// from an offset muzzle. Uses GetBaseAimRotation() (= control rotation for a possessed pawn) so the client
// and server compute the same point for the shot's owner.
static FVector PFAimConvergePoint(UWorld* World, ACombatForgeCharacter* Char)
{
	const FVector Eye = Char->GetEyeWorldLocation();
	FVector End = Eye + Char->GetBaseAimRotation().Vector() * 100000.f;
	if (World != nullptr)
	{
		FHitResult Hit;
		FCollisionQueryParams Q(FName(TEXT("PFAimConverge")), /*bTraceComplex=*/false, Char);
		if (World->LineTraceSingleByChannel(Hit, Eye, End, ECC_Visibility, Q))
		{
			End = Hit.ImpactPoint;
		}
	}
	return End;
}

// The converged shot direction: from the muzzle toward the crosshair's world target. The convergence point is
// clamped to at least MinConvergeDist AHEAD of the muzzle (along the aim) so extreme point-blank (a wall closer
// than the muzzle) can't produce a degenerate or wildly-angled dir. That clamp is deterministic — client and
// server compute it identically — so the anti-cheat dir-gate stays aligned, and it also bounds the max
// convergence angle so close-range client/server aim skew can't blow up past the gate tolerance.
static FVector PFConvergedShotDir(UWorld* World, ACombatForgeCharacter* Char, const FVector& Origin)
{
	const FVector AimFwd = Char->GetBaseAimRotation().Vector();
	FVector AimPoint = PFAimConvergePoint(World, Char);
	constexpr float MinConvergeDist = 120.f;
	const float Ahead = (AimPoint - Origin) | AimFwd;   // signed distance of the aim point ahead of the muzzle
	if (Ahead < MinConvergeDist)
	{
		AimPoint = Origin + AimFwd * MinConvergeDist;
	}
	return (AimPoint - Origin).GetSafeNormal();
}

// Shot-origin adjudication (playtest 2026-07-24: "rounds couldn't leave the barrel" at windows —
// a sweet spot where you had to poke the gun all the way through or stand back). The ball must be
// able to LEAVE from where the player believes they are shooting, which is the crosshair line.
// Two muzzle-parallax failures both re-originate the shot at the EYE:
//   1. barrel embedded — the eye→muzzle-tip path is blocked (muzzle poked into a frame/wall);
//   2. near-lip clip — the muzzle's first stretch toward the aim point hits geometry the eye's
//      line does not (window sills, ledges: the eye sees over the lip, the lower muzzle doesn't).
// Distant cover still blocks: case 2 only fires inside NearLipUU. Traces run on the paintball
// channel (what actually stops the ball), ignoring the shooter. The server accepts an origin near
// EITHER candidate — muzzle or eye — see the tolerance gate in ServerFire.
static FVector PFAdjudicateShotOrigin(UWorld* World, ACombatForgeCharacter* Char, const FVector& Muzzle)
{
	if (World == nullptr)
	{
		return Muzzle;
	}
	const FVector Eye = Char->GetEyeWorldLocation();
	FCollisionQueryParams Q(FName(TEXT("PFShotOrigin")), /*bTraceComplex=*/false, Char);

	FHitResult EmbedHit;
	if (World->LineTraceSingleByChannel(EmbedHit, Eye, Muzzle, PF_ECC_Paintball, Q))
	{
		return Eye;   // case 1: the barrel tip is on the far side of something solid
	}

	constexpr float NearLipUU = 250.f;
	const FVector AimPoint = PFAimConvergePoint(World, Char);
	FHitResult MuzzleHit;
	const bool bMuzzleClippedNear =
		World->LineTraceSingleByChannel(MuzzleHit, Muzzle, AimPoint, PF_ECC_Paintball, Q) &&
		MuzzleHit.Distance < NearLipUU;
	if (bMuzzleClippedNear)
	{
		FHitResult EyeHit;
		const bool bEyeClippedNear =
			World->LineTraceSingleByChannel(EyeHit, Eye, AimPoint, PF_ECC_Paintball, Q) &&
			EyeHit.Distance < NearLipUU;
		if (!bEyeClippedNear)
		{
			return Eye;   // case 2: the crosshair clears the lip, only the muzzle doesn't
		}
	}
	return Muzzle;
}

void UPFWeaponComponent::FireOneShot(double Now)
{
	ACombatForgeCharacter* Char = GetPFCharacter();
	const APlayerState* PS = (Char != nullptr) ? Char->GetPlayerState() : nullptr;
	if (Char == nullptr || PS == nullptr)
	{
		return;   // PlayerState is the spread-seed identity (B3) — no seed, no shot
	}

	++ShotIndexCounter;

	// Origin: the listen host's OWN shots spawn from the first-person barrel it actually sees (tracers come
	// from the visible gun, not the hidden hand weapon). Everyone else uses the server-reproducible TP muzzle
	// so the client-sent origin and the server's validation agree.
	// IsPlayerControlled excludes bots: an AI controller is "locally controlled" too, but a bot has no real FP
	// viewmodel — it must keep the TP hand muzzle its own LOS/fire checks use.
	// HasAuthority() was in this test, which meant it was TRUE on a listen host and FALSE for every client
	// on the dedicated server — so a dedicated-server player's shots left the INVISIBLE third-person gun
	// (posed per-frame off the animated hand_r->hand_l line) instead of the viewmodel they are aiming, while
	// the same player hosting locally got the correct origin. That is a ~15-25uu discrepancy that appears
	// only in the build most people actually play, and it hid the minigun muzzle fix completely there
	// (bMuzzleFromAuthoredFP lives inside the bCosmetic branch). Authority is irrelevant to WHICH GUN THE
	// PLAYER IS LOOKING AT; only "is this my own player pawn" is. Dropping it makes host and client agree.
	//
	// Safe against the anti-teleport gate: the server re-derives the TP muzzle and compares against
	// ServerOriginToleranceUU (200uu) — the FP/TP separation measured across the catalog is 15-25uu, so
	// there is ~175uu of headroom and no legitimate shot can be rejected.
	//
	// IsPlayerControlled excludes bots: an AI controller is "locally controlled" too, but a bot has no real
	// FP viewmodel — it must keep the TP hand muzzle its own LOS/fire checks use.
	const bool bUseOwnViewmodelMuzzle = Char->IsLocallyControlled() && Char->IsPlayerControlled();
	const FVector RawMuzzle = bUseOwnViewmodelMuzzle
		? Char->GetMuzzleLocation(true) : Char->GetMuzzleLocation(false);
	// Windows/ledges: fall back to the eye when the muzzle itself can't clear (see the helper).
	const FVector ShotOrigin = PFAdjudicateShotOrigin(GetWorld(), Char, RawMuzzle);
	// Converge on the crosshair: the muzzle sits below/right of the camera, so flying PARALLEL to the aim (the
	// old BaseDir = camera forward) splatted low-right of the reticle. Aim from the muzzle THROUGH the
	// crosshair's world target so shots land on the reticle regardless of the muzzle offset.
	const FVector BaseDir = PFConvergedShotDir(GetWorld(), Char, ShotOrigin);

	// DO-NOT-TOUCH (B3, §5.15): exactly ONE VRandCone pull per shot from the shared
	// deterministic stream. Any extra pull on either side desyncs every later shot.
	// StampT is sampled ONCE and reused for the cone, the packet, and the bloom advance — the server reuses
	// Packet.ClientTime for its own cone, so both sides compute the identical half-angle for this exact shot.
	const float StampT = static_cast<float>(Now);
	const float HalfAngleDeg = GetSpreadHalfAngleDeg(StampT);
	// DO-NOT-TOUCH (B3): exactly ONE VRandCone pull per shot from the main stream.
	FRandomStream Stream = MakeShotStream(PS->GetPlayerId(), ShotIndexCounter);
	const FVector SpreadedDir = Stream.VRandCone(BaseDir, FMath::DegreesToRadians(HalfAngleDeg));

	// Recoil kick multiplier: softer while aimed (ADSRecoilMult), and low for the first RecoilRampFreeShots of the
	// mag then ramping up. Pure client-local FEEL (camera shake + viewmodel kick) — never touches the authoritative
	// spread cone or the deterministic VRandCone stream, so no netcode risk.
	const float ADSA = FMath::Clamp(Char->GetADSAlpha(), 0.f, 1.f);
	const float ADSMult = FMath::Lerp(1.f, ADSRecoilMult, ADSA);
	float MagMult;
	if (ShotsThisMag < RecoilRampFreeShots)
	{
		MagMult = RecoilRampLowMult;
	}
	else
	{
		const float A = FMath::Clamp(
			static_cast<float>(ShotsThisMag - RecoilRampFreeShots) / FMath::Max(1.f, static_cast<float>(RecoilRampShots)),
			0.f, 1.f);
		MagMult = FMath::Lerp(RecoilRampLowMult, RecoilRampHighMult, A);
	}
	// Shotguns read soft on the screen relative to their blast (playtest 2026-07-24): +25% on the
	// VISIBLE kick only — viewmodel spring + camera shake below. Aim climb stays per-catalog so
	// the balance tuning (sg climb is already the hottest in the catalog) is untouched.
	const float ShotgunVisualKick = (Pellets > 1) ? 1.25f : 1.f;
	const float RecoilMult = ADSMult * MagMult * ShotgunVisualKick;
	++ShotsThisMag;

	// FP recoil kick (pure feel). NOTE: this must never move/re-parent the TP rifle — the muzzle is sampled a
	// few lines below, and a one-frame pose swap here once sent every third-person shot out of the shooter's eyes.
	Char->OnFireCosmetic(RecoilMult);

	if (!Char->HasAuthority())
	{
		// Owning-client cosmetic: instant tracer(s). Shotgun: N pellets with sub-seeds (frag-burst pattern).
		// Listen host skips this — its authoritative projectile(s) ARE the visual.
		if (Pellets > 1)
		{
			SpawnPelletVolley(ShotOrigin, SpreadedDir, GetOwnerTeam(), ShotIndexCounter,
				PS->GetPlayerId(), /*bAuthoritative=*/false, Char);
		}
		else
		{
			SpawnCosmeticProjectile(ShotOrigin, SpreadedDir, GetOwnerTeam(), ShotIndexCounter);
		}

		// Predicted hopper; COND_OwnerOnly replication corrects any divergence. ONE ammo per packet.
		HopperCount = (HopperCount > 0) ? static_cast<uint8>(HopperCount - 1) : 0;
		OnHopperChangedEvent.Broadcast(HopperCount);
	}

	if (!Char->HasAuthority())
	{
		// Client-side bloom for the next predicted shot. A listen host skips this:
		// ServerFire_Implementation (invoked synchronously below) is its single accumulation —
		// advancing here too would double the host's bloom chain.
		AdvanceBloom(StampT);
	}

	// Muzzle feel (04 §4): fire shake doubles as recoil (feel-only; bloom is the spray cost).
	if (APlayerController* PC = Cast<APlayerController>(Char->GetController()))
	{
		PC->ClientStartCameraShake(UPFFireShake::StaticClass(), RecoilMult);   // scale the view-punch (ADS + ramp)

		// Recoil CLIMB: each shot walks the aim up + slightly right (gentler inside the free-shot window).
		// Applied AFTER this shot's dir was sampled, so it shapes the NEXT shot — like real muzzle rise.
		// Recovery back to the original aim runs in TickComponent once the trigger is released.
		const float Mult = (ConsecShots < BloomFreeShots) ? ClimbFreeShotsMult : 1.f;
		const float StepP = ClimbPitchPerShotDeg * Mult;
		const float StepY = ClimbYawPerShotDeg * Mult;
		FRotator CR = PC->GetControlRotation().GetNormalized();
		CR.Pitch = FMath::Clamp(CR.Pitch + StepP, -88.f, 88.f);
		CR.Yaw += StepY;
		PC->SetControlRotation(CR);
		RecoilClimbPitch += StepP;
		RecoilClimbYaw += StepY;
		LastClimbShotTime = Now;
	}
	if (UPFCombatAudio* Audio = Char->GetCombatAudio())
	{
		Audio->PlayMuzzle();
	}
	if (UPFCombatVFX* Vfx = Char->GetCombatVFX())
	{
		// Local owner: FP-scale flash at cosmetic muzzle. Listen host uses FP barrel too.
		const bool bFP = Char->IsLocallyControlled() && Char->IsPlayerControlled();
		Vfx->PlayMuzzleFX(ShotOrigin, SpreadedDir, bFP);
	}

	FPFShotPacket Packet;
	Packet.Origin = ShotOrigin;                        // FP barrel for the local host, TP muzzle otherwise
	Packet.Dir = BaseDir;                              // BASE dir — server applies the same spread
	Packet.ShotIndex = ShotIndexCounter;
	Packet.ClientTime = StampT;                        // the bloom/spread clock — server reuses it (see ServerFire)
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
	ACombatForgeCharacter* Char = GetPFCharacter();
	if (World == nullptr || Char == nullptr)
	{
		return;
	}
	ACombatForgePlayerState* PS = Char->GetPlayerState<ACombatForgePlayerState>();
	if (PS == nullptr)
	{
		return;
	}
	const double Now = World->GetTimeSeconds();

	// Monotonic shot index — rejects replays/reordering abuse.
	if (Shot.ShotIndex <= LastServerShotIndex)
	{
		UE_LOG(CombatForgeLog, Warning, TEXT("ServerFire reject (%s): non-monotonic ShotIndex %u <= %u"),
			*GetNameSafe(Char), Shot.ShotIndex, LastServerShotIndex);
		return;
	}

	// ClientTime drives the bloom clock (both sides key the cone off the SHOOTER's stamp), so sanity-check it:
	// strictly increasing, and the CLAIMED inter-shot gap may not exceed the server-observed gap (+jitter) —
	// a forged large gap would reset bloom for free. A forged small gap only inflates the cheater's own bloom.
	float StampT = Shot.ClientTime;
	if (LastAcceptedClientTime > -999.f)
	{
		if (StampT <= LastAcceptedClientTime)
		{
			UE_LOG(CombatForgeLog, Warning, TEXT("ServerFire reject (%s): non-increasing ClientTime"),
				*GetNameSafe(Char));
			return;
		}
		const float MaxGap = static_cast<float>(Now - LastServerAcceptTime) + 0.1f;
		StampT = FMath::Min(StampT, LastAcceptedClientTime + MaxGap);
	}

	// Token bucket: cap 3, refill 12/s — tolerates jitter bursts, rejects macros (04 §2.1).
	const float RefillRate = FMath::Max(1.f, FireRateBps);
	FireTokens = FMath::Min(FireTokenCap,
		FireTokens + static_cast<float>(Now - LastTokenRefillTime) * RefillRate);
	LastTokenRefillTime = Now;
	if (FireTokens < 1.f)
	{
		UE_LOG(CombatForgeLog, Warning, TEXT("ServerFire reject (%s): ROF token bucket empty"),
			*GetNameSafe(Char));
		return;
	}

	// Phase gate (T21): Freeze/Intermission/Build fire dies here regardless of client UX.
	const ACombatForgeGameState* GS = GetPFGameState();
	if (GS == nullptr || !GS->IsFireAllowed())
	{
		UE_LOG(CombatForgeLog, Verbose, TEXT("ServerFire reject (%s): fire not allowed in phase"),
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

	// Anti-teleport-fire: the claimed origin must be within ServerOriginToleranceUU of where the server
	// thinks this pawn's muzzle is.
	//
	// This DELIBERATELY does not mirror FireOneShot's choice any more. A shooter now always sends its own
	// FIRST-PERSON barrel (the gun it is actually aiming), but a dedicated server has no viewmodel for a
	// remote pawn and physically cannot reproduce that — GetMuzzleLocation(true) on a remote pawn resolves
	// through the TP path anyway. So the server validates against the reproducible TP muzzle and lets the
	// tolerance absorb the difference: FP-vs-TP separation measured across the catalog is 15-25uu against a
	// 200uu gate, leaving ~175uu of headroom. Tightening this gate below ~50uu would start rejecting honest
	// shots, so treat that number as load-bearing.
	const bool bLocalAuth = Char->HasAuthority() && Char->IsLocallyControlled() && Char->IsPlayerControlled();
	const FVector ServerMuzzle = bLocalAuth ? Char->GetMuzzleLocation(true) : Char->GetMuzzleLocation(false);
	// The client may legally re-originate a shot at its EYE when the muzzle can't clear a window
	// lip / frame (PFAdjudicateShotOrigin). Accept an origin near EITHER candidate: the eye sits
	// well inside the same ~1m trust bubble the muzzle gate already grants, so this widens the
	// honest-shot acceptance without meaningfully widening the cheat surface.
	const FVector ServerEye = Char->GetEyeWorldLocation();
	const float MuzzleDistSq = FVector::DistSquared(FVector(Shot.Origin), ServerMuzzle);
	const float EyeDistSq = FVector::DistSquared(FVector(Shot.Origin), ServerEye);
	if (FMath::Min(MuzzleDistSq, EyeDistSq) > FMath::Square(ServerOriginToleranceUU))
	{
		UE_LOG(CombatForgeLog, Warning, TEXT("ServerFire reject (%s): origin %.0f uu from muzzle / %.0f uu from eye"),
			*GetNameSafe(Char), FVector::Dist(FVector(Shot.Origin), ServerMuzzle),
			FVector::Dist(FVector(Shot.Origin), ServerEye));
		return;
	}

	// Dir gate: with crosshair convergence the shot dir is muzzle→reticle (NOT camera-forward), so validate
	// against the server's OWN converged expectation — the ball aims from the (already-validated) origin
	// toward where the server sees this pawn looking. For the host these match exactly; for a remote shooter
	// they differ only by replicated-aim quantization + trace-state skew, covered by the tolerance.
	const FVector BaseDir = FVector(Shot.Dir).GetSafeNormal();
	const FVector ExpectedDir = PFConvergedShotDir(World, Char, FVector(Shot.Origin));
	if (FVector::DotProduct(BaseDir, ExpectedDir) <
		FMath::Cos(FMath::DegreesToRadians(ServerDirToleranceDeg)))
	{
		UE_LOG(CombatForgeLog, Warning, TEXT("ServerFire reject (%s): dir outside %g deg of server aim"),
			*GetNameSafe(Char), ServerDirToleranceDeg);
		return;
	}

	// Server mirror of the CLIENT-ONLY cadence gates (issue #11 CB2): the token bucket enforces only the
	// AVERAGE rate, so a modified client could skip the minigun cold-start and the DMR burst rhythm.
	// Both gates anchor to the VALIDATED monotonic client clock (strictly increasing + gap-clamped above),
	// so network latency never punishes honest shots.
	const double ClientGap = static_cast<double>(StampT) - static_cast<double>(LastAcceptedClientTime);
	if (SpinupSec > 0.f && LastAcceptedClientTime > -999.f)
	{
		// Honest client-time gaps for a spin-up weapon are either WARM (continuous fire / a <=0.4 s
		// feather, i.e. < ~0.45 s) or COLD-and-fully-spun (>= 0.4 grace + SpinupSec — see StartFire).
		// A gap INSIDE that band is a shot the client took without spinning up.
		const double SpunGap = 0.4 + static_cast<double>(SpinupSec) - 0.05;   // small tolerance
		if (ClientGap > 0.45 && ClientGap < SpunGap)
		{
			UE_LOG(CombatForgeLog, Warning,
				TEXT("ServerFire reject (%s): spin-up skipped (gap %.2fs, need >= %.2fs)"),
				*GetNameSafe(Char), ClientGap, SpunGap);
			return;
		}
	}
	const bool bAutoAllowed = (AllowedFireModeMask & (1u << static_cast<uint8>(EPFFireMode::Auto))) != 0;
	if (ReburstDelaySec > 0.f && !bAutoAllowed && ServerBurstRun >= BurstCount
		&& ClientGap < static_cast<double>(ReburstDelaySec) * 0.9)
	{
		UE_LOG(CombatForgeLog, Warning,
			TEXT("ServerFire reject (%s): re-burst delay skipped (gap %.2fs after %u-shot burst)"),
			*GetNameSafe(Char), ClientGap, static_cast<uint32>(BurstCount));
		return;
	}
	// Burst-run bookkeeping: consecutive shots (< 0.3 s apart in client time) grow the run; any real
	// pause starts a new one.
	ServerBurstRun = (ClientGap < 0.3) ? static_cast<uint8>(FMath::Min<int32>(ServerBurstRun + 1, 250)) : 1;

	FireTokens -= 1.f;
	LastServerShotIndex = Shot.ShotIndex;
	LastAcceptedClientTime = StampT;
	LastServerAcceptTime = Now;

	// DO-NOT-TOUCH (B3, §5.15): SAME stream, SAME single VRandCone pull as the owning client — and the SAME
	// StampT (the client's own ClientTime), so both sides evaluate the bloom chain on bit-identical operands.
	const float HalfAngleDeg = GetSpreadHalfAngleDeg(StampT);
	FRandomStream Stream = MakeShotStream(PS->GetPlayerId(), Shot.ShotIndex);
	const FVector SpreadedDir = Stream.VRandCone(BaseDir, FMath::DegreesToRadians(HalfAngleDeg));

	AdvanceBloom(StampT);

	HopperCount = static_cast<uint8>(HopperCount - 1);
	OnHopperChangedEvent.Broadcast(HopperCount);   // host UI (§5.9); remote owner via rep

	// Authoritative projectile(s). Shotgun: N pellets, each with sub-seed (not extra main-stream pulls).
	// ONE token + ONE ammo per packet regardless of pellet count.
	if (Pellets > 1)
	{
		SpawnPelletVolley(FVector(Shot.Origin), SpreadedDir, PS->TeamId, Shot.ShotIndex,
			PS->GetPlayerId(), /*bAuthoritative=*/true, Char);
	}
	else
	{
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
	}

	// Gunfire noise → AI hearing: bots within range turn toward / investigate the shot (perception). Instigator
	// is the shooter pawn so a listening bot resolves friend/foe via its GetTeamAttitudeTowards. Host-authoritative
	// (this runs on the listen server where bot perception lives), so every human + bot shot is heard.
	UAISense_Hearing::ReportNoiseEvent(World, ServerMuzzle, /*Loudness=*/1.f, Char, /*MaxRange=*/5000.f, TEXT("Gunfire"));

	UE_LOG(CombatForgeLog, Verbose, TEXT("ServerFire accept (%s): shot %u, spread %.2f deg"),
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
	if (Owner == nullptr)
	{
		return;
	}
	const UWorld* World = GetWorld();
	if (World == nullptr || World->GetNetMode() == NM_DedicatedServer)
	{
		return;   // pure dedicated: no cosmetics / audio
	}

	APawn* OwnerPawn = Cast<APawn>(Owner);
	if (OwnerPawn != nullptr && OwnerPawn->IsLocallyControlled())
	{
		return;   // owning client / listen-host local already fired cosmetics in FireOneShot
	}

	// Track the shooter's bloom on this viewer so the estimated cone stays honest. Remote viewers can't know the
	// shooter's clock, so they chain on their own (cosmetic-only divergence, accepted by B3).
	AdvanceBloom(static_cast<float>(World->GetTimeSeconds()));

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

	// Listen host: authoritative ball(s) already ARE the tracer — skip cosmetics.
	// Pure clients: spawn the full cosmetic volley / ball.
	if (!Owner->HasAuthority())
	{
		if (Pellets > 1)
		{
			SpawnPelletVolley(FVector(Origin), SpreadedDir, GetOwnerTeam(), ShotIndex,
				PlayerId, /*bAuthoritative=*/false, GetPFCharacter());
		}
		else
		{
			SpawnCosmeticProjectile(FVector(Origin), SpreadedDir, GetOwnerTeam(), ShotIndex);
		}
	}

	// Muzzle report for every non-local viewer (including listen host watching bots / remotes).
	if (ACombatForgeCharacter* Char = GetPFCharacter())
	{
		if (UPFCombatAudio* Audio = Char->GetCombatAudio())
		{
			Audio->PlayMuzzle();
		}
		if (UPFCombatVFX* Vfx = Char->GetCombatVFX())
		{
			Vfx->PlayMuzzleFX(FVector(Origin), SpreadedDir, /*bFirstPerson=*/false);
		}
		Char->OnRemoteFireCosmetic();
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
	const ACombatForgeCharacter* Char = GetPFCharacter();
	if (Char == nullptr || bReloading || HopperCount >= HopperCapacity || ReserveAmmo <= 0)
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
	if (bReloading || HopperCount >= HopperCapacity || ReserveAmmo <= 0)
	{
		return;
	}
	bReloading = true;
	ReloadEndTime = Now + ReloadTime;

	ACombatForgeCharacter* Char = GetPFCharacter();
	if (Char != nullptr && Char->IsLocallyControlled())
	{
		// Reload kicks you out of ADS for the reload's duration (04 §3). IsADS() self-suppresses while
		// bReloading, so we only need to push the intent into the move stream — bADSHeld (the real aim-button
		// record) is left untouched, so a release during the reload is honored and re-ADS on finish is exact.
		Char->NotifyReloadStateChanged();
		if (UPFCombatAudio* Audio = Char->GetCombatAudio())
		{
			Audio->PlayReload();
		}
	}

	OnReloadStateChangedEvent.Broadcast(true);   // predicted/host broadcast; remote owner via OnRep
}

void UPFWeaponComponent::FinishReload()
{
	// Pull from reserve into the magazine (no infinite pods).
	const int32 Room = static_cast<int32>(HopperCapacity) - static_cast<int32>(HopperCount);
	const int32 Take = FMath::Clamp(Room, 0, ReserveAmmo);
	if (Take > 0)
	{
		HopperCount = static_cast<uint8>(HopperCount + Take);
		ReserveAmmo -= Take;
	}
	ShotsThisMag = 0;   // fresh mag re-arms the low-recoil window
	bReloading = false;
	OnHopperChangedEvent.Broadcast(HopperCount);
	OnReloadStateChangedEvent.Broadcast(false);

	ACombatForgeCharacter* Char = GetPFCharacter();
	if (Char != nullptr && Char->IsLocallyControlled())
	{
		// bReloading is now false, so IsADS() resumes reporting the player's live intent (bADSHeld):
		// ADS re-engages iff the button is still held (or toggle is still latched). No stale snapshot.
		Char->NotifyReloadStateChanged();
	}
}

void UPFWeaponComponent::CancelReload()
{
	if (!bReloading)
	{
		return;
	}
	// "Restores prior count" (04 §3) is free: the hopper only fills on FinishReload.
	bReloading = false;
	OnReloadStateChangedEvent.Broadcast(false);
	// A cancelled reload must also lift ADS suppression (IsADS() reads bReloading, now false).
	if (ACombatForgeCharacter* Char = GetPFCharacter())
	{
		if (Char->IsLocallyControlled())
		{
			Char->NotifyReloadStateChanged();
		}
	}
}

void UPFWeaponComponent::UpdateReload(double Now)
{
	if (!bReloading)
	{
		return;
	}
	const ACombatForgeCharacter* Char = GetPFCharacter();
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
	// Crosshair/UI convenience: evaluate the cone "now" on the local clock. Correct on the owning client
	// because its shots are stamped from this same clock.
	const UWorld* World = GetWorld();
	return GetSpreadHalfAngleDeg(World != nullptr ? static_cast<float>(World->GetTimeSeconds()) : 0.f);
}

float UPFWeaponComponent::GetSpreadHalfAngleDeg(float StampT) const
{
	const ACombatForgeCharacter* Char = GetPFCharacter();
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

	// Recoil bloom, keyed to the shot stamp (deterministic across client/server); softened while ADS.
	const float Bloom = GetBloomDegForStamp(StampT);
	Spread += Bloom * FMath::Lerp(1.f, BloomADSMult, ADSAlpha);

	return Spread;
}

FRandomStream UPFWeaponComponent::MakeShotStream(int32 PlayerId, uint32 ShotIndex)
{
	// DO-NOT-TOUCH (B3): seed = (int32)HashCombine((uint32)PlayerId, ShotIndex).
	return FRandomStream(static_cast<int32>(HashCombine(static_cast<uint32>(PlayerId), ShotIndex)));
}

float UPFWeaponComponent::DecayBloomOverGap(float Bloom, float GapSec) const
{
	if (GapSec > BloomResetGap)
	{
		return 0.f;   // full reset after a real release
	}
	if (GapSec <= BloomDecayStartSec || BloomDecayDegPerSec <= 0.f)
	{
		return Bloom;
	}
	const float Decay = BloomDecayDegPerSec * (GapSec - BloomDecayStartSec);
	if (BloomPerShot >= 0.f)
	{
		return FMath::Max(0.f, Bloom - Decay);
	}
	// Negative bloom (minigun): decay relaxes BACK toward zero (opens back up when idle).
	return FMath::Min(0.f, Bloom + Decay);
}

float UPFWeaponComponent::GetBloomDegForStamp(float StampT) const
{
	// Bloom at StampT BEFORE this shot is counted — continuous decay over the stamp gap, full reset after
	// BloomResetGap. Free-shot window still returns 0 until free shots are burned (positive bloom only).
	const float Gap = StampT - LastShotStampT;
	if (Gap > BloomResetGap)
	{
		return 0.f;
	}
	const uint16 N = ConsecShots;
	float Bloom = DecayBloomOverGap(BloomCurrent, Gap);
	if (BloomPerShot >= 0.f)
	{
		if (N < BloomFreeShots)
		{
			return 0.f;
		}
		return FMath::Clamp(Bloom, 0.f, BloomCap);
	}
	// Minigun: bloom is ≤ 0 and floors so hip+bloom ≥ BloomCap (BloomCap used as total-spread floor).
	const float FloorBloom = BloomCap - SpreadHip;
	return FMath::Clamp(Bloom, FloorBloom, 0.f);
}

void UPFWeaponComponent::AdvanceBloom(float StampT)
{
	const float Gap = StampT - LastShotStampT;
	if (Gap > BloomResetGap)
	{
		ConsecShots = 0;
		BloomCurrent = 0.f;
	}
	else
	{
		BloomCurrent = DecayBloomOverGap(BloomCurrent, Gap);
	}
	++ConsecShots;
	if (ConsecShots > BloomFreeShots)
	{
		if (BloomPerShot >= 0.f)
		{
			BloomCurrent = FMath::Min(BloomCurrent + BloomPerShot, BloomCap);
		}
		else
		{
			// Tighten toward floor (more negative until BloomCap is the total-spread floor).
			const float FloorBloom = BloomCap - SpreadHip;
			BloomCurrent = FMath::Max(BloomCurrent + BloomPerShot, FloorBloom);
		}
	}
	LastShotStampT = StampT;
}

float UPFWeaponComponent::GetCurrentBloomDeg() const
{
	const UWorld* World = GetWorld();
	const float Now = World ? static_cast<float>(World->GetTimeSeconds()) : 0.f;
	return GetBloomDegForStamp(Now);
}

FVector UPFWeaponComponent::PelletDir(const FVector& BaseSpreadedDir, uint32 ShotSeed, int32 PelletIdx) const
{
	if (Pellets <= 1 || PelletSpreadDeg <= KINDA_SMALL_NUMBER)
	{
		return BaseSpreadedDir;
	}
	// Deterministic sub-seed — never an extra pull on the main B3 stream (frag-burst pattern).
	FRandomStream PelletStream(static_cast<int32>(HashCombine(ShotSeed, static_cast<uint32>(PelletIdx + 1))));
	return PelletStream.VRandCone(BaseSpreadedDir, FMath::DegreesToRadians(PelletSpreadDeg));
}

void UPFWeaponComponent::SpawnPelletVolley(const FVector& Origin, const FVector& BaseSpreadedDir, uint8 Team,
	uint32 ShotIndex, int32 PlayerId, bool bAuthoritative, ACombatForgeCharacter* Char)
{
	UWorld* World = GetWorld();
	if (!World || Pellets <= 1)
	{
		return;
	}
	const uint32 ShotSeed = static_cast<uint32>(HashCombine(static_cast<uint32>(PlayerId), ShotIndex));
	const int32 N = FMath::Clamp(static_cast<int32>(Pellets), 1, 12);
	for (int32 i = 0; i < N; ++i)
	{
		const FVector Dir = PelletDir(BaseSpreadedDir, ShotSeed, i);
		if (bAuthoritative)
		{
			FActorSpawnParameters SpawnParams;
			SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			SpawnParams.Owner = Char;
			SpawnParams.Instigator = Char;
			APFPaintballProjectile* Ball = World->SpawnActor<APFPaintballProjectile>(
				APFPaintballProjectile::StaticClass(), Origin, Dir.Rotation(), SpawnParams);
			if (Ball)
			{
				Ball->InitProjectile(Origin, Dir, Team, /*bAuthoritative=*/true, this, ShotIndex);
			}
		}
		else
		{
			SpawnCosmeticProjectile(Origin, Dir, Team, ShotIndex);
		}
	}
}

// ---------------------------------------------------------------- OnReps & helpers

void UPFWeaponComponent::OnRep_Hopper()
{
	OnHopperChangedEvent.Broadcast(HopperCount);
}

void UPFWeaponComponent::OnRep_Reserve()
{
	// Reuse hopper event so the combat HUD refreshes mag+reserve display.
	OnHopperChangedEvent.Broadcast(HopperCount);
}

void UPFWeaponComponent::OnRep_Reload()
{
	OnReloadStateChangedEvent.Broadcast(bReloading);
}

void UPFWeaponComponent::OnRep_Grenades()
{
	OnGrenadeCountChangedEvent.Broadcast(FragCount, SmokeCount);
}

bool UPFWeaponComponent::ServerRefillFromPickup()
{
	ACombatForgeCharacter* Char = GetPFCharacter();
	if (Char == nullptr || !Char->HasAuthority())
	{
		return false;
	}
	const bool bMagNeed  = HopperCount < HopperCapacity;
	const bool bResNeed  = ReserveAmmo < MaxReserveAmmo;
	const bool bNadeNeed = (FragCount < MaxFrag) || (SmokeCount < MaxSmoke);
	if (!bMagNeed && !bResNeed && !bNadeNeed)
	{
		return false;
	}
	HopperCount = HopperCapacity;
	ReserveAmmo = MaxReserveAmmo;
	OnHopperChangedEvent.Broadcast(HopperCount);
	if (bNadeNeed)
	{
		FragCount  = MaxFrag;
		SmokeCount = MaxSmoke;
		OnGrenadeCountChangedEvent.Broadcast(FragCount, SmokeCount);
	}
	return true;
}

void UPFWeaponComponent::ServerResetLoadout()
{
	if (GetOwnerRole() != ROLE_Authority)
	{
		return;
	}
	HopperCount = HopperCapacity;
	ReserveAmmo = MaxReserveAmmo;
	FragCount   = MaxFrag;
	SmokeCount  = MaxSmoke;
	bReloading  = false;
	// Respawn returns you to the PRIMARY weapon (drops any drawn pistol + its ammo stash).
	if (ACombatForgeCharacter* Char = GetPFCharacter())
	{
		Char->ResetToPrimaryWeapon();
	}
	OnHopperChangedEvent.Broadcast(HopperCount);
	OnGrenadeCountChangedEvent.Broadcast(FragCount, SmokeCount);
	OnReloadStateChangedEvent.Broadcast(false);
}

// ---------------------------------------------------------------- fire selector + grenades

void UPFWeaponComponent::SetAllowedFireModes(uint8 Mask, EPFFireMode Default)
{
	AllowedFireModeMask = (Mask == 0) ? static_cast<uint8>(1 << 2) : Mask;   // never leave zero (would strand the selector)
	SetFireMode(Default);
}

void UPFWeaponComponent::SetFireMode(EPFFireMode Mode)
{
	if (!IsFireModeAllowed(Mode))
	{
		for (uint8 m = 0; m < 3; ++m)   // fall back to the lowest allowed mode
		{
			if (AllowedFireModeMask & (1u << m)) { Mode = static_cast<EPFFireMode>(m); break; }
		}
	}
	CurrentFireMode = Mode;
	OnFireModeChangedEvent.Broadcast(CurrentFireMode);
}

void UPFWeaponComponent::CycleFireMode()
{
	// Advance to the next ALLOWED mode in Single->Burst->Auto->(wrap) order, skipping ones this weapon can't use.
	for (int32 i = 1; i <= 3; ++i)
	{
		const uint8 Next = (static_cast<uint8>(CurrentFireMode) + i) % 3;
		if (AllowedFireModeMask & (1u << Next))
		{
			CurrentFireMode = static_cast<EPFFireMode>(Next);
			break;
		}
	}
	OnFireModeChangedEvent.Broadcast(CurrentFireMode);
	if (ACombatForgeCharacter* Char = GetPFCharacter())
	{
		if (Char->IsLocallyControlled())
		{
			if (UPFCombatAudio* Audio = Char->GetCombatAudio())
			{
				Audio->PlayFireSelect();
			}
		}
	}
}

void UPFWeaponComponent::StartThrow(EPFGrenadeType Type)
{
	ACombatForgeCharacter* Char = GetPFCharacter();
	if (Char == nullptr || !Char->IsLocallyControlled())
	{
		return;
	}
	// Local supply gate (server re-validates as law): don't spam the RPC when out.
	const uint8 Have = (Type == EPFGrenadeType::Frag) ? FragCount : SmokeCount;
	if (Have == 0)
	{
		return;
	}
	if (const ACombatForgeGameState* GS = GetPFGameState())
	{
		if (!GS->IsFireAllowed())
		{
			return;
		}
	}
	if (const UPFHealthComponent* Health = Char->GetHealth(); Health != nullptr && Health->bEliminated)
	{
		return;
	}
	// Throw from the camera forward (not the TP rifle barrel) so the grenade leaves screen center along the
	// crosshair instead of squirting out of the hand.
	const FVector AimDir = Char->GetControlRotation().Vector();
	const FVector Origin = Char->GetEyeWorldLocation() + AimDir * 60.f;
	if (UPFCombatAudio* Audio = Char->GetCombatAudio())
	{
		Audio->PlayGrenadeThrow();   // instant local feel; world detonation FX comes from the grenade
	}
	ServerThrowGrenade(Origin, AimDir, static_cast<uint8>(Type));
}

void UPFWeaponComponent::ServerThrowGrenade_Implementation(FVector_NetQuantize100 Origin,
	FVector_NetQuantizeNormal AimDir, uint8 Type)
{
	if (GetOwnerRole() != ROLE_Authority)
	{
		return;
	}
	const EPFGrenadeType Kind = (static_cast<EPFGrenadeType>(Type) == EPFGrenadeType::Smoke)
		? EPFGrenadeType::Smoke : EPFGrenadeType::Frag;
	uint8& Count = (Kind == EPFGrenadeType::Frag) ? FragCount : SmokeCount;
	if (Count == 0)
	{
		return;
	}
	ACombatForgeCharacter* Char = GetPFCharacter();
	UWorld* World = GetWorld();
	if (Char == nullptr || World == nullptr)
	{
		return;
	}
	if (const ACombatForgeGameState* GS = GetPFGameState())
	{
		if (!GS->IsFireAllowed())
		{
			return;
		}
	}
	if (const UPFHealthComponent* Health = Char->GetHealth(); Health != nullptr && Health->bEliminated)
	{
		return;
	}
	FVector Dir = FVector(AimDir);
	if (!Dir.Normalize())
	{
		Dir = Char->GetActorForwardVector();
	}
	// Origin anti-spoof: must be near the pawn, else fall back to the camera-forward spawn (mirrors the client).
	FVector SpawnOrigin = Origin;
	if (FVector::DistSquared(SpawnOrigin, Char->GetActorLocation()) > FMath::Square(250.f))
	{
		SpawnOrigin = Char->GetEyeWorldLocation() + Dir * 60.f;
	}

	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	Params.Owner = Char;
	Params.Instigator = Char;
	APFGrenadeProjectile* Nade = World->SpawnActor<APFGrenadeProjectile>(
		APFGrenadeProjectile::StaticClass(), SpawnOrigin, Dir.Rotation(), Params);
	if (Nade == nullptr)
	{
		return;
	}
	Nade->ServerInit(Dir, GetOwnerTeam(), Kind, this);

	--Count;
	OnGrenadeCountChangedEvent.Broadcast(FragCount, SmokeCount);   // host HUD; owning client via OnRep
}

ACombatForgeCharacter* UPFWeaponComponent::GetPFCharacter() const
{
	return Cast<ACombatForgeCharacter>(GetOwner());
}

ACombatForgeGameState* UPFWeaponComponent::GetPFGameState() const
{
	const UWorld* World = GetWorld();
	return (World != nullptr) ? World->GetGameState<ACombatForgeGameState>() : nullptr;
}

uint8 UPFWeaponComponent::GetOwnerTeam() const
{
	if (const ACombatForgeCharacter* Char = GetPFCharacter())
	{
		if (const ACombatForgePlayerState* PS = Char->GetPlayerState<ACombatForgePlayerState>())
		{
			return PS->TeamId;
		}
	}
	return 255;
}
