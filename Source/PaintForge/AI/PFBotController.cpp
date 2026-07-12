// Copyright (c) 2026 Tom Chapman. All rights reserved.

#include "AI/PFBotController.h"

#include "PaintForge.h"
#include "Player/PaintForgeCharacter.h"
#include "Core/PaintForgeGameState.h"
#include "Core/PaintForgePlayerState.h"
#include "Core/PaintForgeTypes.h"
#include "Combat/PFWeaponComponent.h"

#include "Engine/World.h"

APFBotController::APFBotController()
{
	bWantsPlayerState = true;                       // → APaintForgePlayerState in PlayerArray (team/alive/spread-seed)
	bSetControlRotationFromPawnOrientation = false; // we aim by SetControlRotation each tick; don't fight it
	PrimaryActorTick.bCanEverTick = true;
}

APaintForgeCharacter* APFBotController::GetBotCharacter() const
{
	return Cast<APaintForgeCharacter>(GetPawn());
}

void APFBotController::OnPossess(APawn* InPawn)
{
	Super::OnPossess(InPawn);
	CurrentTarget = nullptr;
	bFiring = false;
	FireHoldTimer = 0.f;
	ApplySkill();
}

void APFBotController::OnUnPossess()
{
	SetFiring(false);
	Super::OnUnPossess();
}

void APFBotController::ApplySkill()
{
	// One knob for the whole bot difficulty. Rookie is deliberately soft for the kids' session: wide
	// aim error, a long "notice" delay before it opens up, and a shorter engage range so it doesn't
	// snipe them across the field. Regular is the original brain; Sharpshooter tightens it for scrims.
	switch (Skill)
	{
	case EPFBotSkill::Rookie:
		AimErrorDeg = 8.0f;  ReactionDelay = 0.60f;  EngageRangeUU = 3200.f;  break;
	case EPFBotSkill::Sharpshooter:
		AimErrorDeg = 1.5f;  ReactionDelay = 0.12f;  EngageRangeUU = 5500.f;  break;
	case EPFBotSkill::Regular:
	default:
		AimErrorDeg = 3.5f;  ReactionDelay = 0.30f;  EngageRangeUU = 4500.f;  break;
	}
}

void APFBotController::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	APaintForgeCharacter* Bot = GetBotCharacter();
	APaintForgeGameState* GS = GetWorld() ? GetWorld()->GetGameState<APaintForgeGameState>() : nullptr;
	if (Bot == nullptr || GS == nullptr)
	{
		return;
	}
	const APaintForgePlayerState* PS = GetPlayerState<APaintForgePlayerState>();

	// Only fight during a live combat round while alive; otherwise idle (freeze/build/vote/results/out).
	const bool bAlive = (PS != nullptr) && PS->bAliveInRound;
	const bool bCombatLive = (GS->Phase == EPFMatchPhase::Combat)
		&& (GS->RoundState == EPFRoundState::Live) && GS->IsFireAllowed();
	if (!bAlive || !bCombatLive)
	{
		SetFiring(false);
		return;
	}

	// Refresh the target periodically (and immediately if the cached one died/despawned). Re-roll the
	// aim error on each acquisition so bots miss believably instead of being perfect.
	TargetRefreshTimer -= DeltaSeconds;
	FireHoldTimer -= DeltaSeconds;
	if (TargetRefreshTimer <= 0.f || !CurrentTarget.IsValid())
	{
		APaintForgeCharacter* PrevTarget = CurrentTarget.Get();
		CurrentTarget = AcquireNearestEnemy();
		TargetRefreshTimer = TargetRefreshInterval;
		AimJitterYaw = FMath::FRandRange(-AimErrorDeg, AimErrorDeg);
		AimJitterPitch = FMath::FRandRange(-AimErrorDeg, AimErrorDeg) * 0.5f;
		if (CurrentTarget.IsValid() && CurrentTarget.Get() != PrevTarget)
		{
			FireHoldTimer = ReactionDelay;   // a fresh target isn't shot at until the bot "notices" it
		}
	}

	APaintForgeCharacter* Target = CurrentTarget.Get();
	if (Target == nullptr)
	{
		SetFiring(false);
		return;
	}

	const FVector BotEye = Bot->GetActorLocation() + FVector(0.f, 0.f, 60.f);
	const FVector TargetChest = Target->GetActorLocation() + FVector(0.f, 0.f, 40.f);
	const FVector ToTarget = TargetChest - BotEye;
	const float Dist = ToTarget.Size();

	// Aim: control rotation toward the target chest + a small per-acquisition error. The weapon and the
	// server dir-gate both read control rotation, so this IS the shot direction.
	FRotator LookAt = ToTarget.Rotation();
	LookAt.Yaw += AimJitterYaw;
	LookAt.Pitch = FMath::Clamp(LookAt.Pitch + AimJitterPitch, -80.f, 80.f);
	SetControlRotation(LookAt);

	// Movement: hold a stand-off band — close if too far, back up if too close, strafe in-band. Simple
	// steering (no navmesh); the bot may bump built cover but keeps pressure. Navmesh pathing is a later
	// upgrade that won't touch this brain.
	StrafeTimer -= DeltaSeconds;
	if (StrafeTimer <= 0.f)
	{
		StrafeSign = (FMath::FRand() < 0.5f) ? -1.f : 1.f;
		StrafeTimer = StrafeSwitchInterval;
	}
	const FVector Flat = FVector(ToTarget.X, ToTarget.Y, 0.f).GetSafeNormal();
	const FVector Right = FVector::CrossProduct(FVector::UpVector, Flat);
	if (Dist > PreferredRangeUU)
	{
		Bot->AddMovementInput(Flat, 1.f);
		Bot->AddMovementInput(Right * StrafeSign, 0.4f);
	}
	else if (Dist < MinRangeUU)
	{
		Bot->AddMovementInput(-Flat, 1.f);
	}
	else
	{
		Bot->AddMovementInput(Right * StrafeSign, 1.f);
	}

	// Fire only in range, past the reaction gap, and with a clear line of sight (don't hose cover).
	const bool bWantFire = (Dist <= EngageRangeUU) && (FireHoldTimer <= 0.f) && HasLineOfSight(Target);
	SetFiring(bWantFire);
}

APaintForgeCharacter* APFBotController::AcquireNearestEnemy() const
{
	const APaintForgeCharacter* Bot = GetBotCharacter();
	const APaintForgePlayerState* MyPS = GetPlayerState<APaintForgePlayerState>();
	const APaintForgeGameState* GS = GetWorld() ? GetWorld()->GetGameState<APaintForgeGameState>() : nullptr;
	if (Bot == nullptr || MyPS == nullptr || GS == nullptr)
	{
		return nullptr;
	}
	const uint8 MyTeam = MyPS->TeamId;
	// Free-for-All has no teams — every other living combatant is a target. Team modes keep the
	// teammate/unassigned filter. (Reads MatchType only; the FFA rules themselves live in GameMode.)
	const bool bFFA = (GS->MatchType == EPFMatchType::FreeForAll);

	APaintForgeCharacter* Best = nullptr;
	float BestDistSq = TNumericLimits<float>::Max();
	for (APlayerState* PSBase : GS->PlayerArray)
	{
		const APaintForgePlayerState* OtherPS = Cast<APaintForgePlayerState>(PSBase);
		if (OtherPS == nullptr || OtherPS == MyPS)
		{
			continue;
		}
		if (!OtherPS->bAliveInRound)
		{
			continue;   // already out
		}
		if (!bFFA && (OtherPS->TeamId > 1 || OtherPS->TeamId == MyTeam))
		{
			continue;   // team modes: skip teammate / unassigned
		}
		APaintForgeCharacter* OtherChar = Cast<APaintForgeCharacter>(OtherPS->GetPawn());
		if (OtherChar == nullptr)
		{
			continue;
		}
		const float DistSq = FVector::DistSquared(Bot->GetActorLocation(), OtherChar->GetActorLocation());
		if (DistSq < BestDistSq)
		{
			BestDistSq = DistSq;
			Best = OtherChar;
		}
	}
	return Best;
}

bool APFBotController::HasLineOfSight(const APaintForgeCharacter* Target) const
{
	const APaintForgeCharacter* Bot = GetBotCharacter();
	UWorld* World = GetWorld();
	if (Bot == nullptr || Target == nullptr || World == nullptr)
	{
		return false;
	}
	const FVector Start = Bot->GetActorLocation() + FVector(0.f, 0.f, 60.f);
	const FVector End = Target->GetActorLocation() + FVector(0.f, 0.f, 40.f);
	FCollisionQueryParams Params(FName(TEXT("BotLOS")), /*bTraceComplex=*/false, Bot);
	Params.AddIgnoredActor(Target);
	FHitResult Hit;
	// Trace ignores self + target: a blocking hit means cover/geometry is in the way → no line of sight.
	const bool bBlocked = World->LineTraceSingleByChannel(Hit, Start, End, ECC_Visibility, Params);
	return !bBlocked;
}

void APFBotController::SetFiring(bool bFire)
{
	if (bFire == bFiring)
	{
		return;
	}
	bFiring = bFire;
	if (APaintForgeCharacter* Bot = GetBotCharacter())
	{
		if (UPFWeaponComponent* Weapon = Bot->GetWeapon())
		{
			if (bFire)
			{
				Weapon->StartFire();
			}
			else
			{
				Weapon->StopFire();
			}
		}
	}
}
