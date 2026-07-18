// Copyright (c) 2026 Tom Chapman. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Core/CombatForgeTypes.h"
#include "PFMusicSubsystem.generated.h"

class USoundBase;
class UAudioComponent;
class AActor;

/**
 * Local-only phase music (Build vs Combat). Plays the imported SoundWave assets
 * (Content/Audio/Music/PF_Music_*) through the normal audio engine.
 *
 * NOT Media Framework: a loose .mp3 can't play — no enabled media player supports the extension
 * ("WmfMedia: URI scheme or file extension not supported"), and WmfMedia has no packaged runtime binary
 * on the Launcher engine anyway. SoundWave assets cook + play everywhere with zero plugin dependency.
 * Volume follows the AmbientVolume pref. Silent no-op on dedicated server.
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
	USoundBase* TrackSound(EPFMusicTrack Track);   // soft-loads + caches the SoundWave asset
	float ResolveVolume() const;

	/** 2D looping music component on the hidden anchor (UI sound so it survives pause / menus). */
	UPROPERTY() TObjectPtr<UAudioComponent> MusicComp;
	/** Transient holder so the AudioComponent has a world context. */
	UPROPERTY() TObjectPtr<AActor> MusicAnchor;
	UPROPERTY() TObjectPtr<USoundBase> BuildMusic;
	UPROPERTY() TObjectPtr<USoundBase> CombatMusic;

	EPFMusicTrack ActiveTrack = EPFMusicTrack::None;
};
