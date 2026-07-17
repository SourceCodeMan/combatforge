// Copyright (c) 2026 Tom Chapman. All rights reserved.

#include "Audio/PFMusicSubsystem.h"

#include "CombatForge.h"
#include "Combat/PFCombatAudio.h"
#include "Core/CombatForgeGameState.h"
#include "Core/PFUserPrefs.h"
#include "Player/CombatForgeCharacter.h"

#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "FileMediaSource.h"
#include "GameFramework/Actor.h"
#include "GameFramework/PlayerController.h"
#include "MediaPlayer.h"
#include "MediaSoundComponent.h"
#include "Misc/Paths.h"

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
	MediaSound = nullptr;
	MediaPlayer = nullptr;
	MediaSource = nullptr;
	Super::Deinitialize();
}

float UPFMusicSubsystem::ResolveVolume() const
{
	// Music rides the Ambient slider so one control covers bed + tracks.
	const float Ambient = FPFUserPrefs::GetAmbientVolume();
	// Slight headroom so SFX stay on top of the bed.
	return FMath::Clamp(Ambient * 0.85f, 0.f, 1.f);
}

FString UPFMusicSubsystem::TrackFilePath(EPFMusicTrack Track) const
{
	const TCHAR* File = nullptr;
	switch (Track)
	{
	case EPFMusicTrack::Build:  File = TEXT("PF_Music_Build.mp3"); break;
	case EPFMusicTrack::Combat: File = TEXT("PF_Music_Combat.mp3"); break;
	default: return FString();
	}
	// Content/Audio/Music — shipped next to the project (no .uasset import required).
	return FPaths::ConvertRelativePathToFull(
		FPaths::ProjectContentDir() / TEXT("Audio") / TEXT("Music") / File);
}

void UPFMusicSubsystem::EnsurePlayer()
{
	UWorld* World = GetGameInstance() ? GetGameInstance()->GetWorld() : nullptr;
	if (!World || World->GetNetMode() == NM_DedicatedServer)
	{
		return;
	}

	if (!MediaPlayer)
	{
		MediaPlayer = NewObject<UMediaPlayer>(this, TEXT("PFMusicPlayer"));
		MediaPlayer->SetLooping(true);
		MediaPlayer->PlayOnOpen = true;
	}
	if (!MediaSource)
	{
		MediaSource = NewObject<UFileMediaSource>(this, TEXT("PFMusicSource"));
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
		MediaSound = nullptr;   // re-create on new anchor
	}
	if (MusicAnchor && !MediaSound)
	{
		MediaSound = NewObject<UMediaSoundComponent>(MusicAnchor, TEXT("PFMusicSound"));
		MediaSound->bAutoActivate = true;
		MediaSound->SetMediaPlayer(MediaPlayer);
		MediaSound->RegisterComponent();
		MediaSound->bIsUISound = true;
		MediaSound->bAllowSpatialization = false;
	}
}

void UPFMusicSubsystem::PlayTrack(EPFMusicTrack Track)
{
	if (Track == EPFMusicTrack::None)
	{
		StopMusic();
		return;
	}
	if (Track == ActiveTrack && MediaPlayer && MediaPlayer->IsPlaying())
	{
		ApplyVolumeFromPrefs();
		return;
	}

	const FString Path = TrackFilePath(Track);
	if (Path.IsEmpty() || !FPaths::FileExists(Path))
	{
		UE_LOG(CombatForgeLog, Warning,
			TEXT("Music: missing track file for %d (%s) — place MP3s in Content/Audio/Music/"),
			static_cast<int32>(Track), *Path);
		StopMusic();
		return;
	}

	EnsurePlayer();
	if (!MediaPlayer || !MediaSource || !MediaSound)
	{
		return;
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
	MediaSource->SetFilePath(Path);
	MediaPlayer->Close();
	if (!MediaPlayer->OpenSource(MediaSource))
	{
		UE_LOG(CombatForgeLog, Warning, TEXT("Music: OpenSource failed for %s"), *Path);
		ActiveTrack = EPFMusicTrack::None;
		return;
	}
	ApplyVolumeFromPrefs();
	MediaPlayer->Play();
	UE_LOG(CombatForgeLog, Log, TEXT("Music: playing %s (vol %.2f)"), *FPaths::GetCleanFilename(Path), ResolveVolume());
}

void UPFMusicSubsystem::StopMusic()
{
	if (MediaPlayer)
	{
		MediaPlayer->Close();
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
	const float Vol = ResolveVolume();
	if (MediaSound)
	{
		MediaSound->SetVolumeMultiplier(Vol);
	}
	// Mute by pausing when volume is zero (saves decode work).
	if (MediaPlayer && ActiveTrack != EPFMusicTrack::None)
	{
		if (Vol <= 0.001f)
		{
			if (MediaPlayer->IsPlaying())
			{
				MediaPlayer->Pause();
			}
		}
		else if (!MediaPlayer->IsPlaying() && MediaPlayer->IsReady())
		{
			MediaPlayer->Play();
		}
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

void UPFMusicSubsystem::RefreshFromWorld(UWorld* World)
{
	if (!World)
	{
		return;
	}
	const ACombatForgeGameState* GS = World->GetGameState<ACombatForgeGameState>();
	if (GS)
	{
		SetPhaseMusic(GS->Phase);
	}
	else
	{
		StopMusic();
	}
}
