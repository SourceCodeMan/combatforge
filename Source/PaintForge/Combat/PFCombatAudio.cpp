// Copyright (c) 2026 Tom Chapman. All rights reserved.

#include "Combat/PFCombatAudio.h"

#include "PaintForge.h"
#include "Player/PaintForgeCharacter.h"

#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Kismet/GameplayStatics.h"
#include "Sound/SoundAttenuation.h"
#include "Sound/SoundConcurrency.h"
#include "Sound/SoundWaveProcedural.h"

namespace
{
	constexpr int32 kSampleRate = 22050;

	TArray<uint8> PackPcm(const TArray<int16>& Samples)
	{
		TArray<uint8> Bytes;
		Bytes.SetNumUninitialized(Samples.Num() * sizeof(int16));
		FMemory::Memcpy(Bytes.GetData(), Samples.GetData(), Bytes.Num());
		return Bytes;
	}

	TArray<int16> SynthSineBlip(float FreqHz, float DurationSec, float Amplitude,
		float AttackSec = 0.002f, float ReleaseSec = 0.02f)
	{
		const int32 N = FMath::Max(1, FMath::RoundToInt(DurationSec * kSampleRate));
		TArray<int16> Out;
		Out.SetNumUninitialized(N);
		const int32 AttackN = FMath::Max(1, FMath::RoundToInt(AttackSec * kSampleRate));
		const int32 ReleaseN = FMath::Max(1, FMath::RoundToInt(ReleaseSec * kSampleRate));
		for (int32 i = 0; i < N; ++i)
		{
			const float t = static_cast<float>(i) / static_cast<float>(kSampleRate);
			float Env = 1.f;
			if (i < AttackN)
			{
				Env = static_cast<float>(i) / static_cast<float>(AttackN);
			}
			else if (i > N - ReleaseN)
			{
				Env = static_cast<float>(N - i) / static_cast<float>(ReleaseN);
			}
			const float S = FMath::Sin(2.f * PI * FreqHz * t) * Amplitude * Env;
			Out[i] = static_cast<int16>(FMath::Clamp(S, -1.f, 1.f) * 32767.f);
		}
		return Out;
	}

	TArray<int16> SynthNoiseBurst(float DurationSec, float Amplitude, float LowPass = 0.35f)
	{
		const int32 N = FMath::Max(1, FMath::RoundToInt(DurationSec * kSampleRate));
		TArray<int16> Out;
		Out.SetNumUninitialized(N);
		float Prev = 0.f;
		FRandomStream Rng(0xA17F00D);
		for (int32 i = 0; i < N; ++i)
		{
			const float Env = 1.f - static_cast<float>(i) / static_cast<float>(N);
			const float Env2 = Env * Env;
			const float White = Rng.FRandRange(-1.f, 1.f);
			Prev = FMath::Lerp(Prev, White, LowPass);
			const float S = Prev * Amplitude * Env2;
			Out[i] = static_cast<int16>(FMath::Clamp(S, -1.f, 1.f) * 32767.f);
		}
		return Out;
	}

	TArray<int16> Concat(const TArray<int16>& A, const TArray<int16>& B)
	{
		TArray<int16> Out = A;
		Out.Append(B);
		return Out;
	}

	TArray<int16> SynthMuzzle()
	{
		// Short noise snap + tiny mid click — marker "pop", not rifle crack.
		const TArray<int16> Noise = SynthNoiseBurst(0.028f, 0.55f, 0.45f);
		const TArray<int16> Click = SynthSineBlip(1800.f, 0.012f, 0.22f, 0.001f, 0.008f);
		const int32 N = FMath::Max(Noise.Num(), Click.Num());
		TArray<int16> Out;
		Out.SetNumZeroed(N);
		for (int32 i = 0; i < Noise.Num(); ++i) { Out[i] = Noise[i]; }
		for (int32 i = 0; i < Click.Num(); ++i)
		{
			const int32 Mix = static_cast<int32>(Out[i]) + static_cast<int32>(Click[i]);
			Out[i] = static_cast<int16>(FMath::Clamp(Mix, -32767, 32767));
		}
		return Out;
	}

	TArray<int16> SynthHitmarker()
	{
		return SynthSineBlip(1650.f, 0.045f, 0.35f, 0.001f, 0.025f);
	}

	TArray<int16> SynthElim()
	{
		return Concat(
			SynthSineBlip(980.f, 0.06f, 0.4f, 0.002f, 0.03f),
			SynthSineBlip(620.f, 0.09f, 0.38f, 0.002f, 0.05f));
	}

	TArray<int16> SynthSplatIncoming()
	{
		const TArray<int16> Thump = SynthSineBlip(140.f, 0.07f, 0.45f, 0.003f, 0.05f);
		const TArray<int16> Noise = SynthNoiseBurst(0.05f, 0.25f, 0.25f);
		const int32 N = FMath::Max(Thump.Num(), Noise.Num());
		TArray<int16> Out;
		Out.SetNumZeroed(N);
		for (int32 i = 0; i < Thump.Num(); ++i) { Out[i] = Thump[i]; }
		for (int32 i = 0; i < Noise.Num(); ++i)
		{
			const int32 Mix = static_cast<int32>(Out[i]) + static_cast<int32>(Noise[i] / 2);
			Out[i] = static_cast<int16>(FMath::Clamp(Mix, -32767, 32767));
		}
		return Out;
	}

	TArray<int16> SynthBreakout()
	{
		return Concat(
			SynthSineBlip(440.f, 0.18f, 0.4f, 0.01f, 0.04f),
			SynthSineBlip(660.f, 0.28f, 0.42f, 0.01f, 0.08f));
	}

	TArray<int16> SynthDenied()
	{
		return SynthSineBlip(180.f, 0.12f, 0.4f, 0.002f, 0.04f);
	}

	USoundWaveProcedural* MakeWaveShell(UObject* Outer, float DurationSec)
	{
		USoundWaveProcedural* Wave = NewObject<USoundWaveProcedural>(Outer);
		Wave->SetSampleRate(kSampleRate);
		Wave->NumChannels = 1;
		Wave->bLooping = false;
		Wave->SoundGroup = SOUNDGROUP_Default;
		Wave->Duration = DurationSec;
		return Wave;
	}
}

UPFCombatAudio::UPFCombatAudio()
{
	PrimaryComponentTick.bCanEverTick = false;
	SetIsReplicatedByDefault(false);   // pure local cosmetics; callers are already side-correct
}

bool UPFCombatAudio::CanPlay() const
{
	const UWorld* World = GetWorld();
	return World != nullptr && World->GetNetMode() != NM_DedicatedServer;
}

void UPFCombatAudio::QueuePcm(USoundWaveProcedural* Wave, const TArray<uint8>& Pcm)
{
	if (Wave == nullptr || Pcm.Num() == 0)
	{
		return;
	}
	Wave->ResetAudio();
	Wave->QueueAudio(Pcm.GetData(), Pcm.Num());
}

void UPFCombatAudio::EnsureSounds()
{
	if (bSoundsReady)
	{
		return;
	}
	bSoundsReady = true;

	PcmHitmarker = PackPcm(SynthHitmarker());
	PcmElim = PackPcm(SynthElim());
	PcmMuzzle = PackPcm(SynthMuzzle());
	PcmSplatIncoming = PackPcm(SynthSplatIncoming());
	PcmBreakout = PackPcm(SynthBreakout());
	PcmDenied = PackPcm(SynthDenied());

	auto Dur = [](const TArray<uint8>& Pcm) -> float
	{
		return static_cast<float>(Pcm.Num() / sizeof(int16)) / static_cast<float>(kSampleRate);
	};

	SndHitmarker = MakeWaveShell(this, Dur(PcmHitmarker));
	SndElim = MakeWaveShell(this, Dur(PcmElim));
	SndMuzzle = MakeWaveShell(this, Dur(PcmMuzzle));
	SndSplatIncoming = MakeWaveShell(this, Dur(PcmSplatIncoming));
	SndBreakout = MakeWaveShell(this, Dur(PcmBreakout));
	SndDenied = MakeWaveShell(this, Dur(PcmDenied));

	// 3D falloff across the 64×40 m field.
	CombatAttenuation = NewObject<USoundAttenuation>(this);
	{
		FSoundAttenuationSettings& S = CombatAttenuation->Attenuation;
		S.bAttenuate = true;
		S.bSpatialize = true;
		S.DistanceAlgorithm = EAttenuationDistanceModel::Linear;
		S.AttenuationShape = EAttenuationShape::Sphere;
		S.FalloffDistance = 4500.f;
		S.AttenuationShapeExtents = FVector(600.f);   // full volume within ~6 m
	}

	// Cap auto-fire voices (12 bps × several players).
	MuzzleConcurrency = NewObject<USoundConcurrency>(this);
	{
		FSoundConcurrencySettings& C = MuzzleConcurrency->Concurrency;
		C.MaxCount = 6;
		C.bLimitToOwner = false;
		C.ResolutionRule = EMaxConcurrentResolutionRule::StopFarthestThenPreventNew;
		C.RetriggerTime = 0.f;
	}
}

void UPFCombatAudio::PlayUI(USoundWaveProcedural* Wave, const TArray<uint8>& Pcm,
	float Volume, float Pitch)
{
	if (!CanPlay() || Wave == nullptr)
	{
		return;
	}
	QueuePcm(Wave, Pcm);
	UGameplayStatics::PlaySound2D(this, Wave, Volume, Pitch);
}

void UPFCombatAudio::PlayWorld(USoundWaveProcedural* /*Wave*/, const TArray<uint8>& Pcm,
	float Volume, float Pitch, USoundConcurrency* Concurrency)
{
	if (!CanPlay() || Pcm.Num() == 0)
	{
		return;
	}
	// Fresh wave per shot so auto-fire concurrency doesn't share one drained FIFO.
	const float Dur = static_cast<float>(Pcm.Num() / sizeof(int16)) / static_cast<float>(kSampleRate);
	USoundWaveProcedural* Wave = MakeWaveShell(GetTransientPackage(), Dur);
	QueuePcm(Wave, Pcm);

	FVector Loc = FVector::ZeroVector;
	if (const APaintForgeCharacter* Char = Cast<APaintForgeCharacter>(GetOwner()))
	{
		Loc = Char->GetMuzzleLocation(Char->IsLocallyControlled());
	}
	else if (const AActor* Owner = GetOwner())
	{
		Loc = Owner->GetActorLocation();
	}

	UGameplayStatics::SpawnSoundAtLocation(this, Wave, Loc, FRotator::ZeroRotator,
		Volume, Pitch, 0.f, CombatAttenuation, Concurrency);
}

void UPFCombatAudio::PlayHitmarker()
{
	EnsureSounds();
	PlayUI(SndHitmarker, PcmHitmarker, 0.7f, FMath::FRandRange(0.98f, 1.05f));
	UE_LOG(PaintForgeLog, Verbose, TEXT("[Audio] Hitmarker (%s)"), *GetNameSafe(GetOwner()));
}

void UPFCombatAudio::PlayElim()
{
	EnsureSounds();
	PlayUI(SndElim, PcmElim, 0.85f, 1.f);
	UE_LOG(PaintForgeLog, Verbose, TEXT("[Audio] Elim (%s)"), *GetNameSafe(GetOwner()));
}

void UPFCombatAudio::PlayMuzzle()
{
	EnsureSounds();
	PlayWorld(SndMuzzle, PcmMuzzle, 0.75f, FMath::FRandRange(0.92f, 1.08f), MuzzleConcurrency);
	UE_LOG(PaintForgeLog, Verbose, TEXT("[Audio] Muzzle (%s)"), *GetNameSafe(GetOwner()));
}

void UPFCombatAudio::PlaySplatIncoming()
{
	EnsureSounds();
	PlayUI(SndSplatIncoming, PcmSplatIncoming, 0.8f, FMath::FRandRange(0.95f, 1.05f));
	UE_LOG(PaintForgeLog, Verbose, TEXT("[Audio] SplatIncoming (%s)"), *GetNameSafe(GetOwner()));
}

void UPFCombatAudio::PlayBreakout()
{
	EnsureSounds();
	PlayUI(SndBreakout, PcmBreakout, 0.9f, 1.f);
	UE_LOG(PaintForgeLog, Verbose, TEXT("[Audio] Breakout (%s)"), *GetNameSafe(GetOwner()));
}

void UPFCombatAudio::PlayDenied()
{
	EnsureSounds();
	PlayUI(SndDenied, PcmDenied, 0.65f, 1.f);
	UE_LOG(PaintForgeLog, Verbose, TEXT("[Audio] Denied (%s)"), *GetNameSafe(GetOwner()));
}
