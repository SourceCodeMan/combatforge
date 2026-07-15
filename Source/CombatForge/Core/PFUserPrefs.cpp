// Copyright (c) 2026 Tom Chapman. All rights reserved.

#include "Core/PFUserPrefs.h"

#include "Misc/ConfigCacheIni.h"

namespace
{
	int32 ReadInt(const TCHAR* Key, int32 Default)
	{
		int32 V = Default;
		if (GConfig)
		{
			GConfig->GetInt(TEXT("CombatForge"), Key, V, GGameUserSettingsIni);
		}
		return V;
	}

	float ReadFloat(const TCHAR* Key, float Default)
	{
		float V = Default;
		if (GConfig)
		{
			GConfig->GetFloat(TEXT("CombatForge"), Key, V, GGameUserSettingsIni);
		}
		return V;
	}

	bool ReadBool(const TCHAR* Key, bool Default)
	{
		bool V = Default;
		if (GConfig)
		{
			GConfig->GetBool(TEXT("CombatForge"), Key, V, GGameUserSettingsIni);
		}
		return V;
	}

	void WriteInt(const TCHAR* Key, int32 V)
	{
		if (GConfig) { GConfig->SetInt(TEXT("CombatForge"), Key, V, GGameUserSettingsIni); }
	}

	void WriteFloat(const TCHAR* Key, float V)
	{
		if (GConfig) { GConfig->SetFloat(TEXT("CombatForge"), Key, V, GGameUserSettingsIni); }
	}

	void WriteBool(const TCHAR* Key, bool V)
	{
		if (GConfig) { GConfig->SetBool(TEXT("CombatForge"), Key, V, GGameUserSettingsIni); }
	}
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

FString FPFUserPrefs::GetLastJoinIp()
{
	FString Ip;
	if (GConfig)
	{
		GConfig->GetString(TEXT("CombatForge"), TEXT("LastJoinIp"), Ip, GGameUserSettingsIni);
	}
	return Ip;
}

void FPFUserPrefs::SetLastJoinIp(const FString& Ip)
{
	if (GConfig)
	{
		GConfig->SetString(TEXT("CombatForge"), TEXT("LastJoinIp"), *Ip, GGameUserSettingsIni);
	}
}

int32 FPFUserPrefs::GetQualityLevel()
{
	return FMath::Clamp(ReadInt(TEXT("QualityLevel"), 2), 0, 3);
}

void FPFUserPrefs::SetQualityLevel(int32 Level)
{
	WriteInt(TEXT("QualityLevel"), FMath::Clamp(Level, 0, 3));
}

int32 FPFUserPrefs::GetResolutionIndex()
{
	return FMath::Clamp(ReadInt(TEXT("ResolutionIndex"), 3), 0, 4);
}

void FPFUserPrefs::SetResolutionIndex(int32 Idx)
{
	WriteInt(TEXT("ResolutionIndex"), FMath::Clamp(Idx, 0, 4));
}

float FPFUserPrefs::GetResolutionScalePct()
{
	float V = 100.f;
	if (GConfig)
	{
		GConfig->GetFloat(TEXT("CombatForge"), TEXT("ResolutionScalePct"), V, GGameUserSettingsIni);
	}
	return FMath::Clamp(V, 50.f, 100.f);
}

void FPFUserPrefs::SetResolutionScalePct(float Pct)
{
	if (GConfig)
	{
		GConfig->SetFloat(TEXT("CombatForge"), TEXT("ResolutionScalePct"), FMath::Clamp(Pct, 50.f, 100.f), GGameUserSettingsIni);
	}
}

FKey FPFUserPrefs::GetKeyOverride(FName ActionId)
{
	if (GConfig)
	{
		FString KeyName;
		const FString CfgKey = FString::Printf(TEXT("Bind_%s"), *ActionId.ToString());
		if (GConfig->GetString(TEXT("CombatForge"), *CfgKey, KeyName, GGameUserSettingsIni) && !KeyName.IsEmpty())
		{
			return FKey(FName(*KeyName));
		}
	}
	return FKey();   // EKeys::Invalid
}

void FPFUserPrefs::SetKeyOverride(FName ActionId, FKey Key)
{
	if (GConfig && Key.IsValid())
	{
		const FString CfgKey = FString::Printf(TEXT("Bind_%s"), *ActionId.ToString());
		GConfig->SetString(TEXT("CombatForge"), *CfgKey, *Key.GetFName().ToString(), GGameUserSettingsIni);
	}
}

void FPFUserPrefs::ClearKeyOverride(FName ActionId)
{
	if (GConfig)
	{
		const FString CfgKey = FString::Printf(TEXT("Bind_%s"), *ActionId.ToString());
		GConfig->RemoveKey(TEXT("CombatForge"), *CfgKey, GGameUserSettingsIni);
	}
}

void FPFUserPrefs::Flush()
{
	if (GConfig)
	{
		GConfig->Flush(false, GGameUserSettingsIni);
	}
}

