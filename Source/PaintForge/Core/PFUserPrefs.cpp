// Copyright (c) 2026 Tom Chapman. All rights reserved.

#include "Core/PFUserPrefs.h"

#include "Combat/PFWeaponComponent.h"
#include "Misc/ConfigCacheIni.h"

namespace
{
	int32 ReadInt(const TCHAR* Key, int32 Default)
	{
		int32 V = Default;
		if (GConfig)
		{
			GConfig->GetInt(TEXT("PaintForge"), Key, V, GGameUserSettingsIni);
		}
		return V;
	}

	float ReadFloat(const TCHAR* Key, float Default)
	{
		float V = Default;
		if (GConfig)
		{
			GConfig->GetFloat(TEXT("PaintForge"), Key, V, GGameUserSettingsIni);
		}
		return V;
	}

	bool ReadBool(const TCHAR* Key, bool Default)
	{
		bool V = Default;
		if (GConfig)
		{
			GConfig->GetBool(TEXT("PaintForge"), Key, V, GGameUserSettingsIni);
		}
		return V;
	}

	void WriteInt(const TCHAR* Key, int32 V)
	{
		if (GConfig) { GConfig->SetInt(TEXT("PaintForge"), Key, V, GGameUserSettingsIni); }
	}

	void WriteFloat(const TCHAR* Key, float V)
	{
		if (GConfig) { GConfig->SetFloat(TEXT("PaintForge"), Key, V, GGameUserSettingsIni); }
	}

	void WriteBool(const TCHAR* Key, bool V)
	{
		if (GConfig) { GConfig->SetBool(TEXT("PaintForge"), Key, V, GGameUserSettingsIni); }
	}
}

int32 FPFUserPrefs::GetMarkerPreset()
{
	return FMath::Clamp(ReadInt(TEXT("MarkerPreset"), 0), 0, 2);
}

void FPFUserPrefs::SetMarkerPreset(int32 Preset)
{
	WriteInt(TEXT("MarkerPreset"), FMath::Clamp(Preset, 0, 2));
}

int32 FPFUserPrefs::GetCrosshairStyle()
{
	return FMath::Clamp(ReadInt(TEXT("CrosshairStyle"), 0), 0, 2);
}

void FPFUserPrefs::SetCrosshairStyle(int32 Style)
{
	WriteInt(TEXT("CrosshairStyle"), FMath::Clamp(Style, 0, 2));
}

bool FPFUserPrefs::GetInvertY()
{
	return ReadBool(TEXT("InvertY"), false);
}

void FPFUserPrefs::SetInvertY(bool bInvert)
{
	WriteBool(TEXT("InvertY"), bInvert);
}

float FPFUserPrefs::GetFieldOfView()
{
	return FMath::Clamp(ReadFloat(TEXT("FieldOfView"), 105.f), 80.f, 110.f);
}

void FPFUserPrefs::SetFieldOfView(float Fov)
{
	WriteFloat(TEXT("FieldOfView"), FMath::Clamp(Fov, 80.f, 110.f));
}

float FPFUserPrefs::GetAmbientVolume()
{
	return FMath::Clamp(ReadFloat(TEXT("AmbientVolume"), 0.22f), 0.f, 1.f);
}

void FPFUserPrefs::SetAmbientVolume(float V)
{
	WriteFloat(TEXT("AmbientVolume"), FMath::Clamp(V, 0.f, 1.f));
}

int32 FPFUserPrefs::GetWindowModeIndex()
{
	return FMath::Clamp(ReadInt(TEXT("WindowModeIndex"), 0), 0, 2);
}

void FPFUserPrefs::SetWindowModeIndex(int32 Idx)
{
	WriteInt(TEXT("WindowModeIndex"), FMath::Clamp(Idx, 0, 2));
}

void FPFUserPrefs::Flush()
{
	if (GConfig)
	{
		GConfig->Flush(false, GGameUserSettingsIni);
	}
}

void FPFUserPrefs::ApplyMarkerPresetToWeapon(UPFWeaponComponent* Weapon)
{
	if (!Weapon)
	{
		return;
	}
	switch (GetMarkerPreset())
	{
	case 1: // Rapid
		Weapon->FireRateBps = 14.f;
		Weapon->HopperCapacity = 80;
		break;
	case 2: // Tournament
		Weapon->FireRateBps = 10.f;
		Weapon->HopperCapacity = 140;
		break;
	default: // Standard
		Weapon->FireRateBps = 12.f;
		Weapon->HopperCapacity = 100;
		break;
	}
	// Cap live hopper to new capacity (don't expand mid-round for free).
	if (Weapon->HopperCount > Weapon->HopperCapacity)
	{
		Weapon->HopperCount = Weapon->HopperCapacity;
	}
}
