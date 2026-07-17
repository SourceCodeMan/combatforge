// Copyright (c) 2026 Tom Chapman. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Core/CombatForgeTypes.h"
#include "PFMusicSubsystem.generated.h"

class UMediaPlayer;
class UMediaSoundComponent;
class UFileMediaSource;
class AActor;

/**
 * Local-only phase music (Build vs Combat). Plays friend-made Suno tracks from
 * Content/Audio/Music/*.mp3 via Media Framework so no editor import is required.
 * Volume follows AmbientVolume pref. Silent no-op on dedicated server.
 */
UCLASS()
class COMBATFORGE_API UPFMusicSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	/** Switch bed for the current match phase (local client only). */
	void SetPhaseMusic(EPFMatchPhase Phase);

	/** Re-read GameState phase and AmbientVolume (options apply / pawn spawn). */
	void RefreshFromWorld(UWorld* World);

	/** Live volume update from Options slider (0..1 ambient pref). */
	void ApplyVolumeFromPrefs();

	void StopMusic();

private:
	enum class EPFMusicTrack : uint8 { None = 0, Build, Combat };

	void EnsurePlayer();
	void PlayTrack(EPFMusicTrack Track);
	FString TrackFilePath(EPFMusicTrack Track) const;
	float ResolveVolume() const;

	UPROPERTY() TObjectPtr<UMediaPlayer> MediaPlayer;
	UPROPERTY() TObjectPtr<UFileMediaSource> MediaSource;
	UPROPERTY() TObjectPtr<UMediaSoundComponent> MediaSound;
	/** Transient holder so MediaSoundComponent has a world context. */
	UPROPERTY() TObjectPtr<AActor> MusicAnchor;

	EPFMusicTrack ActiveTrack = EPFMusicTrack::None;
};
