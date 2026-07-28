// Copyright (c) 2026 Tom Chapman. All rights reserved.

#include "Combat/PFHealthComponent.h"

#include "CombatForge.h"
#include "Combat/PFCombatAudio.h"
#include "Player/CombatForgeCharacter.h"
#include "Components/CapsuleComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerState.h"
#include "Core/CombatForgePlayerState.h"
#include "Net/UnrealNetwork.h"
#include "TimerManager.h"
#include "Engine/World.h"

namespace
{
	constexpr float MaskBandFromCapsuleTopUU = 35.f;   // T1 (legacy Z-band fallback)
	constexpr float CorpseBlockSeconds = 0.5f;         // 04 §2.4

	/** Bucket a Manny-family bone name into a hit region. Checked in order: head wins over
	 *  chest wins over limbs; anything unrecognized counts as CHEST (locked default). Covers
	 *  the actual SKM_Body skeleton: head/neck_01/02 → Head; pelvis/spine_01..05/clavicle_l/r
	 *  → Chest; upperarm/lowerarm(+twists)/hand + fingers / thigh/calf(+twists)/foot/ball → Limbs. */
	EPFBodyRegion BucketBoneName(const FName& Bone)
	{
		const FString Name = Bone.ToString().ToLower();
		static const TCHAR* HeadPats[]  = { TEXT("head"), TEXT("neck"), TEXT("face"), TEXT("jaw") };
		static const TCHAR* ChestPats[] = { TEXT("pelvis"), TEXT("spine"), TEXT("clavicle"), TEXT("scap"),
		                                    TEXT("hip"), TEXT("chest"), TEXT("torso") };
		static const TCHAR* LimbPats[]  = { TEXT("arm"), TEXT("hand"), TEXT("thumb"), TEXT("index"),
		                                    TEXT("middle"), TEXT("ring"), TEXT("pinky"), TEXT("finger"),
		                                    TEXT("wrist"), TEXT("elbow"), TEXT("thigh"), TEXT("calf"),
		                                    TEXT("foot"), TEXT("ball"), TEXT("knee"), TEXT("leg"),
		                                    TEXT("ankle"), TEXT("toe") };
		for (const TCHAR* P : HeadPats)  { if (Name.Contains(P)) { return EPFBodyRegion::Head; } }
		for (const TCHAR* P : ChestPats) { if (Name.Contains(P)) { return EPFBodyRegion::Chest; } }
		for (const TCHAR* P : LimbPats)  { if (Name.Contains(P)) { return EPFBodyRegion::Limbs; } }
		return EPFBodyRegion::Chest;
	}

	/** Helper/IK bones the nearest-bone scan must never pick. */
	bool IsHelperBone(const FString& LowerName)
	{
		return LowerName == TEXT("root")
			|| LowerName.StartsWith(TEXT("ik_"))
			|| LowerName.Contains(TEXT("interaction"))
			|| LowerName.Contains(TEXT("center_of_mass"));
	}

	const TCHAR* RegionName(EPFBodyRegion R)
	{
		switch (R)
		{
		case EPFBodyRegion::Head:  return TEXT("Head");
		case EPFBodyRegion::Limbs: return TEXT("Limbs");
		default:                   return TEXT("Chest");
		}
	}
}

UPFHealthComponent::UPFHealthComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
	SetIsReplicatedByDefault(true);
}

void UPFHealthComponent::BeginPlay()
{
	Super::BeginPlay();

	if (GetOwnerRole() == ROLE_Authority)
	{
		HeadHits = ChestHits = LimbHits = TotalHits = 0;
		bOneHitMode = (DefaultRoundHP <= 1);   // warm-up dummies configure DefaultRoundHP=1
		OnHitsChangedEvent.Broadcast(HeadHits, ChestHits, LimbHits, TotalHits);
		OnHPChangedEvent.Broadcast(NearestRemaining());   // host UI (R9)
	}
}

void UPFHealthComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (const UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(CorpseCollisionTimer);
	}
	Super::EndPlay(EndPlayReason);
}

void UPFHealthComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME(UPFHealthComponent, HeadHits);
	DOREPLIFETIME(UPFHealthComponent, ChestHits);
	DOREPLIFETIME(UPFHealthComponent, LimbHits);
	DOREPLIFETIME(UPFHealthComponent, TotalHits);
	DOREPLIFETIME(UPFHealthComponent, bOneHitMode);
	DOREPLIFETIME(UPFHealthComponent, bEliminated);
}

void UPFHealthComponent::ApplyPaintHit(const FPFPaintHitInfo& HitTemplate, bool bForceEliminate)
{
	if (GetOwnerRole() != ROLE_Authority)
	{
		UE_LOG(CombatForgeLog, Error, TEXT("ApplyPaintHit called without authority on %s"),
			*GetNameSafe(GetOwner()));
		return;
	}
	// Corpse rule (04 §2.4): eliminated bodies still block paintballs briefly but never
	// generate hit events.
	if (bEliminated)
	{
		return;
	}

	FPFPaintHitInfo Hit = HitTemplate;
	Hit.Region = ResolveHitRegion(Hit.HitBone, FVector(Hit.ImpactPoint));
	// Per-weapon HitValue (sniper HV5–8, most guns 1). Server-only; never trust a client-sent field.
	const uint8 Dmg = static_cast<uint8>(FMath::Clamp<int32>(
		Hit.Damage > 0 ? static_cast<int32>(Hit.Damage) : 1, 1, 10));
	Hit.Damage = Dmg;

	TotalHits = static_cast<uint8>(FMath::Min<int32>(255, static_cast<int32>(TotalHits) + Dmg));
	switch (Hit.Region)
	{
	case EPFBodyRegion::Head:
		HeadHits = static_cast<uint8>(FMath::Min<int32>(255, static_cast<int32>(HeadHits) + Dmg));
		break;
	case EPFBodyRegion::Limbs:
		LimbHits = static_cast<uint8>(FMath::Min<int32>(255, static_cast<int32>(LimbHits) + Dmg));
		break;
	default:
		ChestHits = static_cast<uint8>(FMath::Min<int32>(255, static_cast<int32>(ChestHits) + Dmg));
		break;
	}
	const bool bOut = bForceEliminate || bOneHitMode
		|| HeadHits >= HeadOut || ChestHits >= ChestOut || LimbHits >= LimbOut || TotalHits >= TotalOut;

	OnHitsChangedEvent.Broadcast(HeadHits, ChestHits, LimbHits, TotalHits);
	OnHPChangedEvent.Broadcast(bOut ? 0 : NearestRemaining());   // manual host broadcast (§5.9)

	UE_LOG(CombatForgeLog, Verbose, TEXT("PaintHit on %s: region=%s bone=%s h=%u c=%u l=%u tot=%u%s"),
		*GetNameSafe(GetOwner()), RegionName(Hit.Region), *Hit.HitBone.ToString(),
		static_cast<uint32>(HeadHits), static_cast<uint32>(ChestHits),
		static_cast<uint32>(LimbHits), static_cast<uint32>(TotalHits),
		bOut ? TEXT(" -> OUT") : TEXT(""));

	// Victim-side feedback (owning client): direction arc + mask splats need the shooter's
	// world position; fall back to the impact point if the shooter pawn is gone.
	FVector ShooterLoc = FVector(Hit.ImpactPoint);
	if (Hit.ShooterPS != nullptr)
	{
		if (const APawn* ShooterPawn = Hit.ShooterPS->GetPawn())
		{
			ShooterLoc = ShooterPawn->GetActorLocation();
		}
	}
	// Route victim feedback only where it can land: a locally-controlled pawn (listen host —
	// the client RPC runs in-place) or a pawn with an owning client connection. Server-owned
	// victims (APFTargetDummy) have neither; sending would only log a
	// "no owning connection" net warning per warm-up hit.
	const APawn* OwnerPawn = Cast<APawn>(GetOwner());
	if (OwnerPawn != nullptr &&
		(OwnerPawn->IsLocallyControlled() || OwnerPawn->GetNetConnection() != nullptr))
	{
		ClientPaintHitTaken(ShooterLoc, Hit.ShooterTeam, bOut ? 0 : NearestRemaining());
	}

	if (bOut)
	{
		bEliminated = true;
		ApplyEliminatedAppearance(true);   // server/host visual; clients via OnRep_Eliminated

		// Corpse blocks paintballs 0.5 s, then collision turns off — paintball response and
		// pawn blocking both (04 §2.4). Clients mirror this via OnRep_Eliminated.
		ArmCorpseCollisionOff();

		// GameMode (player pawns) / APFTargetDummy (itself) subscribe here — this component
		// never calls the GameMode directly (T29 decoupling).
		OnEliminatedEvent.Broadcast(this, Hit);
	}
}

void UPFHealthComponent::ApplyFallDeath()
{
	if (GetOwnerRole() != ROLE_Authority)
	{
		return;
	}
	if (bEliminated)
	{
		return;
	}

	TotalHits = FMath::Max<uint8>(TotalOut, 1);   // fall = instantly out, whatever the counters said
	OnHitsChangedEvent.Broadcast(HeadHits, ChestHits, LimbHits, TotalHits);
	OnHPChangedEvent.Broadcast(0);

	bEliminated = true;
	ApplyEliminatedAppearance(true);
	ArmCorpseCollisionOff();

	FPFPaintHitInfo Hit;
	Hit.ShooterPS = nullptr;
	Hit.ShooterTeam = 255;   // sentinel: fall / environment (GameMode treats as instant respawn)
	if (const AActor* Owner = GetOwner())
	{
		Hit.ImpactPoint = Owner->GetActorLocation();
	}
	Hit.ImpactNormal = FVector::UpVector;
	Hit.Region = EPFBodyRegion::Chest;
	Hit.Damage = TotalOut;
	if (const UWorld* World = GetWorld())
	{
		Hit.ServerTime = World->GetTimeSeconds();
	}

	UE_LOG(CombatForgeLog, Log, TEXT("FallDeath on %s"), *GetNameSafe(GetOwner()));
	OnEliminatedEvent.Broadcast(this, Hit);
}

void UPFHealthComponent::ResetForRound(uint8 RoundHP)
{
	if (GetOwnerRole() != ROLE_Authority)
	{
		return;
	}

	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(CorpseCollisionTimer);
	}

	HeadHits = ChestHits = LimbHits = TotalHits = 0;
	// Sudden-death showdown (GameMode passes 1) and warm-up dummies = one-hit mode; any RoundHP
	// >= 2 means the full locational thresholds (3/5/8/10) — the parameter is a mode, not a pool.
	bOneHitMode = (RoundHP <= 1);
	bEliminated = false;
	RestoreCorpseCollision();
	ApplyEliminatedAppearance(false);
	OnHitsChangedEvent.Broadcast(HeadHits, ChestHits, LimbHits, TotalHits);
	OnHPChangedEvent.Broadcast(NearestRemaining());   // manual host broadcast (§5.9)
}

EPFBodyRegion UPFHealthComponent::ComputeRegion(const FVector& ImpactPoint) const
{
	const AActor* Owner = GetOwner();
	if (Owner == nullptr)
	{
		return EPFBodyRegion::Chest;
	}

	// Legacy Z-band: Head iff impact Z ≥ capsule-top − 35 uu (T1; works standing and crouched
	// because the capsule top follows the crouch interp). Non-character owners (target dummies)
	// use their bounds top for the same 35 uu band. Everything else counts as Chest.
	float TopZ = 0.f;
	if (const ACharacter* OwnerChar = Cast<ACharacter>(Owner))
	{
		const UCapsuleComponent* Capsule = OwnerChar->GetCapsuleComponent();
		TopZ = Capsule->GetComponentLocation().Z + Capsule->GetScaledCapsuleHalfHeight();
	}
	else
	{
		TopZ = Owner->GetComponentsBoundingBox().Max.Z;
	}

	return (ImpactPoint.Z >= TopZ - MaskBandFromCapsuleTopUU) ? EPFBodyRegion::Head : EPFBodyRegion::Chest;
}

EPFBodyRegion UPFHealthComponent::ResolveHitRegion(const FName& HitBone, const FVector& ImpactPoint) const
{
	// (a) A real bone name from the hit result wins outright (only happens if the skeletal mesh
	// intercepted before the capsule — rare, but then it is the ground truth).
	if (!HitBone.IsNone())
	{
		return BucketBoneName(HitBone);
	}

	// (b) Capsule hits carry BoneName=None (the capsule IS the authoritative hit surface —
	// see CombatForgeCharacter collision setup). Derive the bone by scanning the visual
	// skeleton for the closest bone to the impact point, skipping root/ik_/helper bones.
	if (const ACharacter* OwnerChar = Cast<ACharacter>(GetOwner()))
	{
		if (const USkeletalMeshComponent* Mesh = OwnerChar->GetMesh())
		{
			const USkeletalMesh* MeshAsset = Mesh->GetSkeletalMeshAsset();
			const TArray<FTransform>& Pose = Mesh->GetComponentSpaceTransforms();
			if (MeshAsset != nullptr && Pose.Num() > 0)
			{
				const FReferenceSkeleton& RefSkel = MeshAsset->GetRefSkeleton();
				const FVector LocalPt = Mesh->GetComponentTransform().InverseTransformPosition(ImpactPoint);
				const int32 NumBones = FMath::Min(Pose.Num(), RefSkel.GetNum());
				float BestDistSq = TNumericLimits<float>::Max();
				int32 BestBone = INDEX_NONE;
				for (int32 i = 0; i < NumBones; ++i)
				{
					const FString LowerName = RefSkel.GetBoneName(i).ToString().ToLower();
					if (IsHelperBone(LowerName))
					{
						continue;
					}
					const float DistSq = FVector::DistSquared(Pose[i].GetLocation(), LocalPt);
					if (DistSq < BestDistSq)
					{
						BestDistSq = DistSq;
						BestBone = i;
					}
				}
				if (BestBone != INDEX_NONE)
				{
					return BucketBoneName(RefSkel.GetBoneName(BestBone));
				}
			}
		}
	}

	// (c) No skeleton (target dummies, edge cases): the old Z-band keeps working.
	return ComputeRegion(ImpactPoint);
}

uint8 UPFHealthComponent::NearestRemaining() const
{
	if (bEliminated)
	{
		return 0;
	}
	if (bOneHitMode)
	{
		return (TotalHits > 0) ? 0 : 1;
	}
	const int32 Remaining = FMath::Min(
		FMath::Min<int32>(HeadOut - HeadHits, ChestOut - ChestHits),
		FMath::Min<int32>(LimbOut - LimbHits, TotalOut - TotalHits));
	return static_cast<uint8>(FMath::Max(0, Remaining));
}

void UPFHealthComponent::ClientPaintHitTaken_Implementation(FVector_NetQuantize ShooterLoc,
	uint8 ShooterTeam, uint8 NewHP)
{
	OnLocalPaintHitTakenEvent.Broadcast(FVector(ShooterLoc), ShooterTeam, NewHP);

	if (const AActor* Owner = GetOwner())
	{
		if (UPFCombatAudio* Audio = Owner->FindComponentByClass<UPFCombatAudio>())
		{
			Audio->PlaySplatIncoming();
			// Own death stinger (shooter already gets PlayElim on hit confirm).
			if (NewHP == 0)
			{
				Audio->PlayElim();
			}
		}
	}
}

void UPFHealthComponent::OnRep_Hits()
{
	OnHitsChangedEvent.Broadcast(HeadHits, ChestHits, LimbHits, TotalHits);
	OnHPChangedEvent.Broadcast(NearestRemaining());
}

void UPFHealthComponent::OnRep_Eliminated()
{
	// Single-shot tolerant (§5.10): applying the final value directly is always correct.
	ApplyEliminatedAppearance(bEliminated);

	// Mirror the server's corpse collision-off locally: client-predicted movement must agree
	// with authority that a corpse stops blocking pawns, or living players rubber-band on an
	// invisible capsule that no longer exists server-side.
	if (bEliminated)
	{
		ArmCorpseCollisionOff();
	}
	else if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(CorpseCollisionTimer);
		RestoreCorpseCollision();
	}
}

void UPFHealthComponent::ArmCorpseCollisionOff()
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	if (bSkipCorpseBlock)
	{
		// The owner hides the instant it is eliminated, so a blocking window would just be
		// invisible geometry eating shots. Drop collision on the same frame. (P2-CB10)
		World->GetTimerManager().ClearTimer(CorpseCollisionTimer);
		DisableCorpseCollision();
		return;
	}
	World->GetTimerManager().SetTimer(CorpseCollisionTimer, this,
		&UPFHealthComponent::DisableCorpseCollision, CorpseBlockSeconds, false);
}

void UPFHealthComponent::DisableCorpseCollision()
{
	AActor* Owner = GetOwner();
	if (Owner == nullptr)
	{
		return;
	}

	PaintballBlockersDisabled.Reset();
	PawnBlockersDisabled.Reset();

	TInlineComponentArray<UPrimitiveComponent*> Prims(Owner);
	for (UPrimitiveComponent* Prim : Prims)
	{
		if (Prim == nullptr)
		{
			continue;
		}
		if (Prim->GetCollisionResponseToChannel(PF_ECC_Paintball) == ECR_Block)
		{
			Prim->SetCollisionResponseToChannel(PF_ECC_Paintball, ECR_Ignore);
			PaintballBlockersDisabled.Add(Prim);
		}
		// "Then collision off" (04 §2.4): a hidden corpse must not remain a pawn-blocking
		// invisible collider (doorway body-blocking). Only the pawn response is dropped —
		// world responses stay so the hidden pawn does not fall out of the level.
		if (Prim->GetCollisionResponseToChannel(ECC_Pawn) == ECR_Block)
		{
			Prim->SetCollisionResponseToChannel(ECC_Pawn, ECR_Ignore);
			PawnBlockersDisabled.Add(Prim);
		}
	}
}

void UPFHealthComponent::RestoreCorpseCollision()
{
	for (const TWeakObjectPtr<UPrimitiveComponent>& WeakPrim : PaintballBlockersDisabled)
	{
		if (UPrimitiveComponent* Prim = WeakPrim.Get())
		{
			Prim->SetCollisionResponseToChannel(PF_ECC_Paintball, ECR_Block);
		}
	}
	PaintballBlockersDisabled.Reset();

	for (const TWeakObjectPtr<UPrimitiveComponent>& WeakPrim : PawnBlockersDisabled)
	{
		if (UPrimitiveComponent* Prim = WeakPrim.Get())
		{
			Prim->SetCollisionResponseToChannel(ECC_Pawn, ECR_Block);
		}
	}
	PawnBlockersDisabled.Reset();
}

void UPFHealthComponent::ApplyEliminatedAppearance(bool bNewEliminated)
{
	if (ACombatForgeCharacter* OwnerChar = Cast<ACombatForgeCharacter>(GetOwner()))
	{
		OwnerChar->SetEliminatedAppearance(bNewEliminated);
	}
	// Target dummies drive their own hide/show from OnEliminatedEvent.
}
