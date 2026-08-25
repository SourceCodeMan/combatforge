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
#include "Templates/UnrealTemplate.h"
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
		MusicComp->bAutoDestroy = false;
		MusicComp->bIsUISound = true;             // survives gameplay pause + plays in menus
		MusicComp->bAllowSpatialization = false;  // 2D bed
		// Loop backstop: cooked builds loop via the wave's own flag and never reach this, but the
		// editor path deliberately does not touch the asset, so restart on finish there. (P2-I4)
		MusicComp->OnAudioFinished.AddDynamic(this, &UPFMusicSubsystem::HandleMusicFinished);
		MusicComp->RegisterComponent();
	}
}

void UPFMusicSubsystem::HandleMusicFinished()
{
	// Only meaningful when a track is still meant to be playing — StopMusic clears ActiveTrack
	// and latches bStoppingMusic first, so a deliberate Stop() never restarts here.
	if (bStoppingMusic)
	{
		return;
	}
	if (ActiveTrack != EPFMusicTrack::None && MusicComp && !MusicComp->IsPlaying())
	{
		MusicComp->Play();
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

	// Loop the bed. The wave's own loop flag is the seamless path — but writing a UAsset field at
	// runtime dirties the asset in-editor and would leak looping to any other consumer of the same
	// wave, so the write is cooked-build only. In the editor the OnAudioFinished restart bound in
	// EnsurePlayer carries the loop instead (one buffer-boundary gap, editor only). (P2-I4)
#if !WITH_EDITOR
	if (USoundWave* Wave = Cast<USoundWave>(Sound))
	{
		Wave->bLooping = true;
	}
#endif

	ActiveTrack = Track;
	// Duck the soft wind bed so phase tracks aren't muddy.
	SetLocalAmbientBed(false);

	MusicComp->SetSound(Sound);
	bMatchEndDucked = false;   // a fresh track always starts at full pref volume
	MusicComp->SetVolumeMultiplier(ResolveVolume());
	MusicComp->Play();
	UE_LOG(CombatForgeLog, Log, TEXT("Music: playing %s (vol %.2f)"), *Sound->GetName(), ResolveVolume());
}

void UPFMusicSubsystem::StopMusic()
{
	const bool bWasPlaying = (ActiveTrack != EPFMusicTrack::None);
	ActiveTrack = EPFMusicTrack::None;
	if (MusicComp)
	{
		// Latch before Stop() so a sync OnAudioFinished broadcast cannot Play() the old bed.
		TGuardValue<bool> StoppingGuard(bStoppingMusic, true);
		MusicComp->Stop();
	}
	// Lobby / vote: restore quiet wind bed on the local pawn if present.
	if (bWasPlaying)
	{
		SetLocalAmbientBed(true);
	}
}

void UPFMusicSubsystem::SetLocalAmbientBed(bool bOn)
{
	// GetFirstPlayerController is whichever PC the world lists first, which is not necessarily the
	// one that owns THIS game instance's local pawn — on those setups the wind bed was never ducked
	// or never came back. Walk this instance's own local players instead. (P2-I5)
	const UGameInstance* GI = GetGameInstance();
	if (!GI)
	{
		return;
	}
	for (const ULocalPlayer* LP : GI->GetLocalPlayers())
	{
		APlayerController* PC = LP ? LP->GetPlayerController(GI->GetWorld()) : nullptr;
		ACombatForgeCharacter* Char = PC ? Cast<ACombatForgeCharacter>(PC->GetPawn()) : nullptr;
		UPFCombatAudio* Audio = Char ? Char->GetCombatAudio() : nullptr;
		if (!Audio)
		{
			continue;
		}
		if (bOn) { Audio->StartAmbientBed(); }
		else     { Audio->StopAmbientBed(); }
	}
}

void UPFMusicSubsystem::ApplyVolumeFromPrefs()
{
	// Volume 0 just mutes (no stop/restart — avoids a jarring music restart when the slider crosses zero).
	// While the final-30s duck holds, the slider must not blast the bed back in — the un-duck restores it.
	if (MusicComp && ActiveTrack != EPFMusicTrack::None && !bMatchEndDucked)
	{
		MusicComp->SetVolumeMultiplier(ResolveVolume());
	}
}

void UPFMusicSubsystem::SetMatchEndDucked(bool bDucked)
{
	if (bMatchEndDucked == bDucked)
	{
		return;
	}
	bMatchEndDucked = bDucked;
	if (!MusicComp || ActiveTrack == EPFMusicTrack::None || !MusicComp->IsPlaying())
	{
		return;
	}
	if (bDucked)
	{
		// Fade to (near) silence — 0 exactly can virtualize/stop the sound on some platforms.
		MusicComp->AdjustVolume(1.2f, 0.0001f);
	}
	else
	{
		MusicComp->AdjustVolume(0.8f, FMath::Max(ResolveVolume(), 0.0001f));
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

