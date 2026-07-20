// Copyright (c) 2026 Tom Chapman. All rights reserved.

#include "Core/PFUserPrefs.h"
#include "Core/PFPaths.h"

#include "HAL/IConsoleManager.h"
#include "Misc/ConfigCacheIni.h"

namespace
{
	int32 ReadInt(const TCHAR* Key, int32 Default)
	{
		int32 V = Default;
		if (GConfig)
		{
			GConfig->GetInt(TEXT("CombatForge"), Key, V, FPFPaths::UserPrefsIni());
		}
		return V;
	}

	float ReadFloat(const TCHAR* Key, float Default)
	{
		float V = Default;
		if (GConfig)
		{
			GConfig->GetFloat(TEXT("CombatForge"), Key, V, FPFPaths::UserPrefsIni());
		}
		return V;
	}

	bool ReadBool(const TCHAR* Key, bool Default)
	{
		bool V = Default;
		if (GConfig)
		{
			GConfig->GetBool(TEXT("CombatForge"), Key, V, FPFPaths::UserPrefsIni());
		}
		return V;
	}

	void WriteInt(const TCHAR* Key, int32 V)
	{
		if (GConfig) { GConfig->SetInt(TEXT("CombatForge"), Key, V, FPFPaths::UserPrefsIni()); }
	}

	void WriteFloat(const TCHAR* Key, float V)
	{
		if (GConfig) { GConfig->SetFloat(TEXT("CombatForge"), Key, V, FPFPaths::UserPrefsIni()); }
	}

	void WriteBool(const TCHAR* Key, bool V)
	{
		if (GConfig) { GConfig->SetBool(TEXT("CombatForge"), Key, V, FPFPaths::UserPrefsIni()); }
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

bool FPFUserPrefs::GetADSToggle()
{
	return ReadBool(TEXT("ADSToggle"), false);   // default hold
}

void FPFUserPrefs::SetADSToggle(bool bToggle)
{
	WriteBool(TEXT("ADSToggle"), bToggle);
}

bool FPFUserPrefs::GetCrouchToggle()
{
	return ReadBool(TEXT("CrouchToggle"), false);   // default hold
}

void FPFUserPrefs::SetCrouchToggle(bool bToggle)
{
	WriteBool(TEXT("CrouchToggle"), bToggle);
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

float FPFUserPrefs::GetBrightnessEV()
{
	return FMath::Clamp(ReadFloat(TEXT("BrightnessEV"), 0.f), -1.f, 1.f);
}

void FPFUserPrefs::SetBrightnessEV(float EV)
{
	WriteFloat(TEXT("BrightnessEV"), FMath::Clamp(EV, -1.f, 1.f));
}

float FPFUserPrefs::GetContrastScale()
{
	return FMath::Clamp(ReadFloat(TEXT("ContrastScale"), 1.f), 0.85f, 1.2f);
}

void FPFUserPrefs::SetContrastScale(float C)
{
	WriteFloat(TEXT("ContrastScale"), FMath::Clamp(C, 0.85f, 1.2f));
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
		GConfig->GetString(TEXT("CombatForge"), TEXT("LastJoinIp"), Ip, FPFPaths::UserPrefsIni());
	}
	return Ip;
}

void FPFUserPrefs::SetLastJoinIp(const FString& Ip)
{
	if (GConfig)
	{
		GConfig->SetString(TEXT("CombatForge"), TEXT("LastJoinIp"), *Ip, FPFPaths::UserPrefsIni());
	}
}

TArray<FString> FPFUserPrefs::GetFavoriteMapIds()
{
	// Single CSV key: arenaIds are lowercase hex and filenames are arena_*.json — no commas.
	FString Csv;
	if (GConfig)
	{
		GConfig->GetString(TEXT("CombatForge"), TEXT("FavoriteMapIds"), Csv, FPFPaths::UserPrefsIni());
	}
	TArray<FString> Raw;
	Csv.ParseIntoArray(Raw, TEXT(","), /*bCullEmpty=*/true);
	TArray<FString> Ids;
	for (FString& Id : Raw)
	{
		Id.TrimStartAndEndInline();
		if (!Id.IsEmpty() && !Ids.Contains(Id))
		{
			Ids.Add(Id);
		}
	}
	if (Ids.Num() > MaxFavoriteMaps)
	{
		Ids.SetNum(MaxFavoriteMaps);
	}
	return Ids;
}

void FPFUserPrefs::SetFavoriteMapIds(const TArray<FString>& InIds)
{
	TArray<FString> Ids;
	for (const FString& Id : InIds)
	{
		const FString Trimmed = Id.TrimStartAndEnd();
		if (!Trimmed.IsEmpty() && !Ids.Contains(Trimmed))
		{
			Ids.Add(Trimmed);
		}
		if (Ids.Num() >= MaxFavoriteMaps)
		{
			break;   // hard cap, defensively enforced here too
		}
	}
	if (GConfig)
	{
		GConfig->SetString(TEXT("CombatForge"), TEXT("FavoriteMapIds"),
			*FString::Join(Ids, TEXT(",")), FPFPaths::UserPrefsIni());
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
	// Default 4 (1440p, the table max) not 3 (1080p). In BORDERLESS the resolution index is folded into the
	// render scale (PFOptionsWidget PushToSettings), so a 1080p default silently rendered SUB-NATIVE on every
	// 1440p/4K monitor -> TSR upscale -> the "blurry since the AAA pass" complaint (Tom 2026-07-18). At index 4
	// the fold clamps to native on 1080p AND 1440p desktops (H>=DesktopY => scale 1.0); 4K still caps at 1440p
	// by design. Users who WANT the perf downscale just pick a lower resolution — the knob is preserved.
	return FMath::Clamp(ReadInt(TEXT("ResolutionIndex"), 4), 0, 4);
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
		GConfig->GetFloat(TEXT("CombatForge"), TEXT("ResolutionScalePct"), V, FPFPaths::UserPrefsIni());
	}
	return FMath::Clamp(V, 50.f, 100.f);
}

void FPFUserPrefs::SetResolutionScalePct(float Pct)
{
	if (GConfig)
	{
		GConfig->SetFloat(TEXT("CombatForge"), TEXT("ResolutionScalePct"), FMath::Clamp(Pct, 50.f, 100.f), FPFPaths::UserPrefsIni());
	}
}

int32 FPFUserPrefs::GetFrameRateLimitIndex()
{
	return FMath::Clamp(ReadInt(TEXT("FrameRateLimitIndex"), 2), 0, 4);
}

void FPFUserPrefs::SetFrameRateLimitIndex(int32 Idx)
{
	WriteInt(TEXT("FrameRateLimitIndex"), FMath::Clamp(Idx, 0, 4));
}

float FPFUserPrefs::FrameRateLimitForIndex(int32 Idx)
{
	static const float Caps[] = { 60.f, 120.f, 144.f, 240.f, 0.f };   // 0 = uncapped
	return Caps[FMath::Clamp(Idx, 0, 4)];
}

void FPFUserPrefs::ApplyQualityMethodCVars(int32 QualityLevel)
{
	const int32 Q = FMath::Clamp(QualityLevel, 0, 3);
	// {GI method (0 none / 2 SSGI / 1 Lumen), SSGI enable, reflections (0 none / 2 SSR / 1 Lumen),
	//  VSM enable, AA method (1 FXAA / 2 TAA / 4 TSR)} per quality level.
	// NO LUMEN at any tier: the project never generates mesh distance fields, so Lumen has no data to trace
	// ("Lumen is enabled but has no ray tracing data" warning) and silently produces no GI. Screen-space GI +
	// SSR actually work here, are far cheaper (the whole point of the budget path), and clear the warning.
	// (To ever enable real Lumen: turn on Generate Mesh Distance Fields project-wide — heavier cook + GPU.)
	static const int32 GIMethod[4]   = { 0, 2, 2, 2 };
	static const int32 SSGIEnable[4] = { 0, 1, 1, 1 };
	static const int32 ReflMethod[4] = { 0, 2, 2, 2 };
	static const int32 VSMEnable[4]  = { 0, 0, 1, 1 };
	static const int32 AAMethod[4]   = { 1, 2, 4, 4 };

	auto SetCVar = [](const TCHAR* Name, int32 Value)
	{
		if (IConsoleVariable* Var = IConsoleManager::Get().FindConsoleVariable(Name))
		{
			// SetByGameSetting: below SetByConsole, so pf.* / console experiments still win over the menu.
			Var->Set(Value, ECVF_SetByGameSetting);
		}
	};
	SetCVar(TEXT("r.DynamicGlobalIlluminationMethod"), GIMethod[Q]);
	SetCVar(TEXT("r.SSGI.Enable"), SSGIEnable[Q]);
	SetCVar(TEXT("r.ReflectionMethod"), ReflMethod[Q]);
	SetCVar(TEXT("r.Shadow.Virtual.Enable"), VSMEnable[Q]);
	SetCVar(TEXT("r.AntiAliasingMethod"), AAMethod[Q]);
}

FKey FPFUserPrefs::GetKeyOverride(FName ActionId)
{
	if (GConfig)
	{
		FString KeyName;
		const FString CfgKey = FString::Printf(TEXT("Bind_%s"), *ActionId.ToString());
		if (GConfig->GetString(TEXT("CombatForge"), *CfgKey, KeyName, FPFPaths::UserPrefsIni()) && !KeyName.IsEmpty())
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
		GConfig->SetString(TEXT("CombatForge"), *CfgKey, *Key.GetFName().ToString(), FPFPaths::UserPrefsIni());
	}
}

void FPFUserPrefs::ClearKeyOverride(FName ActionId)
{
	if (GConfig)
	{
		const FString CfgKey = FString::Printf(TEXT("Bind_%s"), *ActionId.ToString());
		GConfig->RemoveKey(TEXT("CombatForge"), *CfgKey, FPFPaths::UserPrefsIni());
	}
}

void FPFUserPrefs::Flush()
{
	if (GConfig)
	{
		GConfig->Flush(false, FPFPaths::UserPrefsIni());
	}
}

