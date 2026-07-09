// Copyright (c) 2026 Tom Chapman. All rights reserved.

#include "Player/PFCameraShakes.h"

#include "Camera/Shakes/WaveOscillatorCameraShakePattern.h"

namespace
{
	// Zero every oscillator so only the axes we explicitly configure move.
	void ZeroAllOscillators(UWaveOscillatorCameraShakePattern* Pattern)
	{
		Pattern->X.Amplitude = 0.f;
		Pattern->Y.Amplitude = 0.f;
		Pattern->Z.Amplitude = 0.f;
		Pattern->Pitch.Amplitude = 0.f;
		Pattern->Yaw.Amplitude = 0.f;
		Pattern->Roll.Amplitude = 0.f;
		Pattern->FOV.Amplitude = 0.f;
	}
}

UPFLandShake::UPFLandShake(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	// One dip at a time — re-landing restarts rather than stacks.
	bSingleInstance = true;

	UWaveOscillatorCameraShakePattern* Pattern =
		ObjectInitializer.CreateDefaultSubobject<UWaveOscillatorCameraShakePattern>(this, TEXT("LandShakePattern"));
	ZeroAllOscillators(Pattern);

	Pattern->Duration = 0.12f;
	Pattern->BlendInTime = 0.02f;
	Pattern->BlendOutTime = 0.06f;

	// 1.5 degree pitch dip: ~half a wave over the 0.12 s life reads as one dip.
	Pattern->Pitch.Amplitude = 1.5f;
	Pattern->Pitch.Frequency = 4.f;
	Pattern->Pitch.InitialOffsetType = EInitialWaveOscillatorOffsetType::Zero;

	SetRootShakePattern(Pattern);
}

UPFFireShake::UPFFireShake(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	// Per-shot instances stack additively at 12 bps (04 §4).
	bSingleInstance = false;

	UWaveOscillatorCameraShakePattern* Pattern =
		ObjectInitializer.CreateDefaultSubobject<UWaveOscillatorCameraShakePattern>(this, TEXT("FireShakePattern"));
	ZeroAllOscillators(Pattern);

	Pattern->Duration = 0.06f;
	Pattern->BlendInTime = 0.01f;
	Pattern->BlendOutTime = 0.03f;

	// 0.3 degree pitch kick: quarter-wave rise across the 0.06 s life.
	Pattern->Pitch.Amplitude = 0.3f;
	Pattern->Pitch.Frequency = 4.f;
	Pattern->Pitch.InitialOffsetType = EInitialWaveOscillatorOffsetType::Zero;

	// 0.15 degree yaw with a random phase per instance = random yaw direction.
	Pattern->Yaw.Amplitude = 0.15f;
	Pattern->Yaw.Frequency = 15.f;
	Pattern->Yaw.InitialOffsetType = EInitialWaveOscillatorOffsetType::Random;

	SetRootShakePattern(Pattern);
}
