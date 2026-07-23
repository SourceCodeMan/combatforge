// Copyright (c) 2026 Tom Chapman. All rights reserved.

#include "Audio/PFMusicSubsystem.h"

#include "CombatForge.h"
#include "Combat/PFCombatAudio.h"
#include "Core/CombatForgeGameState.h"
#include "Core/PFUserPrefs.h"
#include "Player/CombatForgeCharacter.h"

#include "Components/AudioComponent.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GameFramework/PlayerController.h"
#include "Sound/SoundBase.h"
#include "Sound/SoundWave.h"

void UPFMusicSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	ActiveTrack = EPFMusicTrack::None;
}

void UPFMusicSubsystem::Deinitialize()
{
	StopMusic();
	if (IsValid(MusicAnchor))
	{
		MusicAnchor->Destroy();
		MusicAnchor = nullptr;
	}
	MusicComp = nullptr;
	Super::Deinitialize();
}

float UPFMusicSubsystem::ResolveVolume() const
{
	// Music rides the Ambient slider so one control covers bed + tracks. Slight headroom so SFX stay on top.
	const float Ambient = FPFUserPrefs::GetAmbientVolume();
	return FMath::Clamp(Ambient * 0.85f, 0.f, 1.f);
}

USoundBase* UPFMusicSubsystem::TrackSound(EPFMusicTrack Track)
{
	// Imported SoundWave assets (NOT the loose .mp3, which no media player can open). Soft-loaded by path and
	// cached. The /Game/Audio dir is force-cooked (DirectoriesToAlwaysCook) since this is a code-string load.
	switch (Track)
	{
	case EPFMusicTrack::Build:
		if (!BuildMusic)  { BuildMusic  = LoadObject<USoundBase>(nullptr, TEXT("/Game/Audio/Music/PF_Music_Build.PF_Music_Build")); }
		return BuildMusic;
	case EPFMusicTrack::Combat:
		if (!CombatMusic) { CombatMusic = LoadObject<USoundBase>(nullptr, TEXT("/Game/Audio/Music/PF_Music_Combat.PF_Music_Combat")); }
		return CombatMusic;
	default:
		return nullptr;
	}
}

void UPFMusicSubsystem::EnsurePlayer()
{
	UWorld* World = GetGameInstance() ? GetGameInstance()->GetWorld() : nullptr;
	if (!World || World->GetNetMode() == NM_DedicatedServer)
	{
		return;
	}

	if (!IsValid(MusicAnchor) || MusicAnchor->GetWorld() != World)
	{
		if (IsValid(MusicAnchor))
		{
			MusicAnchor->Destroy();
		}
		FActorSpawnParameters Params;
		Params.Name = TEXT("PFMusicAnchor");
		Params.ObjectFlags |= RF_Transient;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		MusicAnchor = World->SpawnActor<AActor>(AActor::StaticClass(), FTransform::Identity, Params);
		if (MusicAnchor)
		{
			MusicAnchor->SetActorHiddenInGame(true);
			MusicAnchor->SetReplicates(false);
		}
		MusicComp = nullptr;   // recreate on the new anchor
	}

	if (MusicAnchor && !MusicComp)
	{
		MusicComp = NewObject<UAudioComponent>(MusicAnchor, TEXT("PFMusicComp"));
		MusicComp->bAutoActivate = false;
		MusicComp->bIsUISound = true;             // survives gameplay pause + plays in menus
		MusicComp->bAllowSpatialization = false;  // 2D bed
		MusicComp->RegisterComponent();
	}
}

void UPFMusicSubsystem::PlayTrack(EPFMusicTrack Track)
{
	if (Track == EPFMusicTrack::None)
	{
		StopMusic();
		return;
	}
	if (Track == ActiveTrack && MusicComp && MusicComp->IsPlaying())
	{
		ApplyVolumeFromPrefs();
		return;
	}

	USoundBase* Sound = TrackSound(Track);
	if (!Sound)
	{
		UE_LOG(CombatForgeLog, Warning,
			TEXT("Music: SoundWave asset missing for track %d — expected /Game/Audio/Music/PF_Music_* (cooked?)"),
			static_cast<int32>(Track));
		StopMusic();
		return;
	}

	EnsurePlayer();
	if (!MusicComp)
	{
		return;
	}

	// Loop the bed. The wave's own loop flag is the seamless path; harmless if the asset already loops.
	if (USoundWave* Wave = Cast<USoundWave>(Sound))
	{
		Wave->bLooping = true;
	}

	ActiveTrack = Track;
	// Duck the soft wind bed so phase tracks aren't muddy.
	if (UWorld* World = GetGameInstance() ? GetGameInstance()->GetWorld() : nullptr)
	{
		if (APlayerController* PC = World->GetFirstPlayerController())
		{
			if (ACombatForgeCharacter* Char = Cast<ACombatForgeCharacter>(PC->GetPawn()))
			{
				if (UPFCombatAudio* Audio = Char->GetCombatAudio())
				{
					Audio->StopAmbientBed();
				}
			}
		}
	}

	MusicComp->SetSound(Sound);
	MusicComp->SetVolumeMultiplier(ResolveVolume());
	MusicComp->Play();
	UE_LOG(CombatForgeLog, Log, TEXT("Music: playing %s (vol %.2f)"), *Sound->GetName(), ResolveVolume());
}

void UPFMusicSubsystem::StopMusic()
{
	if (MusicComp)
	{
		MusicComp->Stop();
	}
	const bool bWasPlaying = (ActiveTrack != EPFMusicTrack::None);
	ActiveTrack = EPFMusicTrack::None;
	// Lobby / vote: restore quiet wind bed on the local pawn if present.
	if (bWasPlaying)
	{
		if (UWorld* World = GetGameInstance() ? GetGameInstance()->GetWorld() : nullptr)
		{
			if (APlayerController* PC = World->GetFirstPlayerController())
			{
				if (ACombatForgeCharacter* Char = Cast<ACombatForgeCharacter>(PC->GetPawn()))
				{
					if (UPFCombatAudio* Audio = Char->GetCombatAudio())
					{
						Audio->StartAmbientBed();
					}
				}
			}
		}
	}
}

void UPFMusicSubsystem::ApplyVolumeFromPrefs()
{
	// Volume 0 just mutes (no stop/restart — avoids a jarring music restart when the slider crosses zero).
	if (MusicComp && ActiveTrack != EPFMusicTrack::None)
	{
		MusicComp->SetVolumeMultiplier(ResolveVolume());
	}
}

void UPFMusicSubsystem::SetPhaseMusic(EPFMatchPhase Phase)
{
	const UWorld* World = GetGameInstance() ? GetGameInstance()->GetWorld() : nullptr;
	if (!World || World->GetNetMode() == NM_DedicatedServer)
	{
		return;
	}

	switch (Phase)
	{
	case EPFMatchPhase::Build:
		PlayTrack(EPFMusicTrack::Build);
		break;
	case EPFMatchPhase::Combat:
		PlayTrack(EPFMusicTrack::Combat);
		break;
	case EPFMatchPhase::Lobby:
	case EPFMatchPhase::Vote:
	case EPFMatchPhase::Results:
	default:
		// Soft wind ambient (character) covers lobby; custom music is phase-specific.
		StopMusic();
		break;
	}
}

