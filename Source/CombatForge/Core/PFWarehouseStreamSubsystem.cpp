// Copyright (c) 2026 Tom Chapman. All rights reserved.

#include "Core/PFWarehouseStreamSubsystem.h"

#include "CombatForge.h"
#include "Building/PFArenaShell.h"
#include "Core/CombatForgeGameState.h"

#include "Engine/LevelStreamingDynamic.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "HAL/IConsoleManager.h"
#include "Misc/PackageName.h"
#include "TimerManager.h"

namespace
{
	// Soft path — does not hard-fail cook if the map is missing from a given checkout.
	const TCHAR* WarehouseMapPath = TEXT("/Game/Scene_Warehouse/Maps/Industrial_Warehouse");

	// OFF by default: first PIE of Industrial_Warehouse builds 100+ Nanite meshes (minutes of
	// freeze at ~100% CPU). Enable after opening the map once in the editor (or when cooked).
	//   pf.StreamWarehouseMap 1
	static TAutoConsoleVariable<int32> CVarStreamWarehouseMap(
		TEXT("pf.StreamWarehouseMap"),
		0,
		TEXT("1 = stream Industrial_Warehouse as environment (very heavy first load). 0 = shell materials only."),
		ECVF_Default);
}

bool UPFWarehouseStreamSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

void UPFWarehouseStreamSubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);

	// Dedicated servers don't render — skip the multi-GB visual stream (listen host still loads).
	if (InWorld.GetNetMode() == NM_DedicatedServer)
	{
		return;
	}

	if (CVarStreamWarehouseMap.GetValueOnGameThread() == 0)
	{
		UE_LOG(CombatForgeLog, Log,
			TEXT("WarehouseStream: full map OFF (pf.StreamWarehouseMap=0). Arena uses shell + materials. "
			     "Set pf.StreamWarehouseMap 1 after first editor open of Industrial_Warehouse if you want the full map."));
		return;
	}

	TryStreamWarehouse(InWorld);

	// ArenaShell is spawned from GameMode::BeginPlay (order vs subsystems varies) — retry backdrop
	// a few times so floor/wall cubes hide once the shell exists.
	if (bStreamSucceeded)
	{
		ApplyShellBackdrop(InWorld);
		TWeakObjectPtr<UPFWarehouseStreamSubsystem> WeakThis(this);
		TWeakObjectPtr<UWorld> WeakWorld(&InWorld);
		InWorld.GetTimerManager().SetTimer(BackdropRetryHandle,
			FTimerDelegate::CreateLambda([WeakThis, WeakWorld]()
			{
				if (WeakThis.IsValid() && WeakWorld.IsValid())
				{
					WeakThis->ApplyShellBackdrop(*WeakWorld.Get());
				}
			}),
			0.25f, /*bLoop=*/true);
		// Stop retrying after a few seconds.
		FTimerHandle StopHandle;
		InWorld.GetTimerManager().SetTimer(StopHandle,
			FTimerDelegate::CreateLambda([WeakThis, WeakWorld]()
			{
				if (WeakThis.IsValid() && WeakWorld.IsValid())
				{
					WeakWorld->GetTimerManager().ClearTimer(WeakThis->BackdropRetryHandle);
				}
			}),
			3.f, false);
	}
}

void UPFWarehouseStreamSubsystem::Deinitialize()
{
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(BackdropRetryHandle);
		UnloadWarehouse(*World);
	}
	StreamedWarehouse = nullptr;
	bStreamSucceeded = false;
	Super::Deinitialize();
}

void UPFWarehouseStreamSubsystem::OnArenaMapChanged(UWorld& World)
{
	// Dedicated servers never stream the visual backdrop.
	if (World.GetNetMode() == NM_DedicatedServer)
	{
		return;
	}

	const ACombatForgeGameState* GS = World.GetGameState<ACombatForgeGameState>();
	const bool bWantWarehouse = GS && GS->ArenaMap == EPFArenaMap::Warehouse
		&& CVarStreamWarehouseMap.GetValueOnGameThread() != 0;

	if (!bWantWarehouse)
	{
		UnloadWarehouse(World);
		return;
	}

	if (StreamedWarehouse == nullptr)
	{
		TryStreamWarehouse(World);
		if (bStreamSucceeded)
		{
			// Shell may still be mid-swap on the server when ArenaMap replicates — retry backdrop.
			ApplyShellBackdrop(World);
			TWeakObjectPtr<UPFWarehouseStreamSubsystem> WeakThis(this);
			TWeakObjectPtr<UWorld> WeakWorld(&World);
			World.GetTimerManager().SetTimer(BackdropRetryHandle,
				FTimerDelegate::CreateLambda([WeakThis, WeakWorld]()
				{
					if (WeakThis.IsValid() && WeakWorld.IsValid())
					{
						WeakThis->ApplyShellBackdrop(*WeakWorld.Get());
					}
				}),
				0.25f, /*bLoop=*/true);
			FTimerHandle StopHandle;
			World.GetTimerManager().SetTimer(StopHandle,
				FTimerDelegate::CreateLambda([WeakThis, WeakWorld]()
				{
					if (WeakThis.IsValid() && WeakWorld.IsValid())
					{
						WeakWorld->GetTimerManager().ClearTimer(WeakThis->BackdropRetryHandle);
					}
				}),
				3.f, false);
		}
	}
}

void UPFWarehouseStreamSubsystem::UnloadWarehouse(UWorld& World)
{
	World.GetTimerManager().ClearTimer(BackdropRetryHandle);
	if (StreamedWarehouse)
	{
		StreamedWarehouse->SetShouldBeVisible(false);
		StreamedWarehouse->SetShouldBeLoaded(false);
		StreamedWarehouse = nullptr;
		UE_LOG(CombatForgeLog, Log, TEXT("WarehouseStream: unloaded environment stream (left Warehouse or cvar off)."));
	}
	bStreamSucceeded = false;
	// Restore shell cubes if a Warehouse shell is still present (map swap may destroy it first).
	for (TActorIterator<APFArenaShell> It(&World); It; ++It)
	{
		if (It->GetMapDef().MapId == EPFArenaMap::Warehouse)
		{
			It->SetMapBackdropActive(false);
		}
	}
}

void UPFWarehouseStreamSubsystem::TryStreamWarehouse(UWorld& World)
{
	if (StreamedWarehouse != nullptr)
	{
		return;
	}

	// The stream places Industrial_Warehouse at identity over the WAREHOUSE field (0..6400 ×
	// 0..4000). On any other map (The Yard is open-air and twice as wide) it would intersect the
	// field, so skip. Boot default is Warehouse; map switches happen later in the lobby.
	if (const ACombatForgeGameState* GS = World.GetGameState<ACombatForgeGameState>())
	{
		if (GS->ArenaMap != EPFArenaMap::Warehouse)
		{
			UE_LOG(CombatForgeLog, Log,
				TEXT("WarehouseStream: active map is not the Warehouse — skipping environment stream."));
			return;
		}
	}

	// Package may be absent (gitignored Fab dump) — soft fail keeps L_Graybox playable.
	if (!FPackageName::DoesPackageExist(WarehouseMapPath))
	{
		UE_LOG(CombatForgeLog, Warning,
			TEXT("WarehouseStream: package missing (%s) — keeping procedural arena shell visuals."),
			WarehouseMapPath);
		return;
	}

	UE_LOG(CombatForgeLog, Warning,
		TEXT("WarehouseStream: loading %s — first time can freeze for minutes while meshes compile. "
		     "If stuck, stop PIE and set pf.StreamWarehouseMap 0."),
		WarehouseMapPath);

	bool bSuccess = false;
	// Identity transform: shell play field is 0..6400 x 0..4000 at Z=0.
	ULevelStreamingDynamic* Stream = ULevelStreamingDynamic::LoadLevelInstance(
		&World,
		WarehouseMapPath,
		FVector::ZeroVector,
		FRotator::ZeroRotator,
		bSuccess);

	if (!bSuccess || Stream == nullptr)
	{
		UE_LOG(CombatForgeLog, Warning,
			TEXT("WarehouseStream: LoadLevelInstance failed for %s"), WarehouseMapPath);
		return;
	}

	Stream->SetShouldBeLoaded(true);
	Stream->SetShouldBeVisible(true);
	StreamedWarehouse = Stream;
	bStreamSucceeded = true;

	UE_LOG(CombatForgeLog, Log,
		TEXT("WarehouseStream: streaming %s as environment backdrop (gameplay shell kept)."),
		WarehouseMapPath);
}

void UPFWarehouseStreamSubsystem::ApplyShellBackdrop(UWorld& World)
{
	if (!bStreamSucceeded)
	{
		return;
	}

	bool bFound = false;
	for (TActorIterator<APFArenaShell> It(&World); It; ++It)
	{
		if (It->GetMapDef().MapId != EPFArenaMap::Warehouse)
		{
			continue;   // never hide a non-Warehouse shell behind the warehouse stream
		}
		It->SetMapBackdropActive(true);
		bFound = true;
	}
	if (bFound)
	{
		World.GetTimerManager().ClearTimer(BackdropRetryHandle);
	}
}
