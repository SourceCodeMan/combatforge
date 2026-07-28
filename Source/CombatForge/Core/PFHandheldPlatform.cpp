// Copyright (c) 2026 Tom Chapman. All rights reserved.

#include "Core/PFHandheldPlatform.h"

#include "CombatForge.h"
#include "Core/PFUserPrefs.h"
#include "Engine/Engine.h"
#include "Engine/UserInterfaceSettings.h"
#include "GameFramework/GameUserSettings.h"
#include "HAL/IConsoleManager.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

namespace
{
	// Bump when the preset VALUES change so existing handheld installs pick the new
	// numbers up exactly once more. 0 = never applied (pref default).
	constexpr int32 HandheldPresetVersion = 1;

	// Resolution-index table must stay in lockstep with UPFOptionsWidget::PushToSettings.
	// 3 = 1920x1080 (native on Ally/Legion Go class panels), 0 = 1280x720 (Deck folds to its
	// 800p desktop in borderless).
	constexpr int32 WinHandheldResIndex = 3;
	constexpr int32 DeckResIndex = 0;

	bool CpuBrandLooksHandheld(const FString& Brand)
	{
		// Ryzen Z-series is handheld-only silicon (ROG Ally Z1/Z1 Extreme, Legion Go Z1E,
		// Ally X / Legion Go S Z2 line). Plain "Ryzen"/"Core Ultra" strings are NOT enough —
		// they cover ordinary laptops.
		return Brand.Contains(TEXT("Ryzen Z1")) || Brand.Contains(TEXT("Ryzen Z2"));
	}

	bool CpuBrandLooksSteamDeck(const FString& Brand)
	{
		// "AMD Custom APU 0405" = Deck LCD (Van Gogh), "0932" = Deck OLED (Sephiroth).
		// Covers Windows-installed-on-Deck, where the SteamDeck env var is absent.
		return Brand.Contains(TEXT("Custom APU 0405")) || Brand.Contains(TEXT("Custom APU 0932"));
	}

	EPFHandheldKind DetectUncached()
	{
		const TCHAR* CmdLine = FCommandLine::Get();
		if (FParse::Param(CmdLine, TEXT("pfnothandheld")))
		{
			return EPFHandheldKind::None;
		}
		if (FParse::Param(CmdLine, TEXT("pfsteamdeck")))
		{
			return EPFHandheldKind::SteamDeck;
		}
		if (FParse::Param(CmdLine, TEXT("pfhandheld")))
		{
			return EPFHandheldKind::WindowsHandheld;
		}

		// SteamOS sets SteamDeck=1 for every process (survives Proton into the Windows build).
		if (FPlatformMisc::GetEnvironmentVariable(TEXT("SteamDeck")) == TEXT("1"))
		{
			return EPFHandheldKind::SteamDeck;
		}

		const FString CpuBrand = FPlatformMisc::GetCPUBrand();
		if (CpuBrandLooksSteamDeck(CpuBrand))
		{
			return EPFHandheldKind::SteamDeck;
		}
		if (CpuBrandLooksHandheld(CpuBrand))
		{
			return EPFHandheldKind::WindowsHandheld;
		}
		return EPFHandheldKind::None;
	}

	void LogDetectionSignals()
	{
		// Always logged at boot — Tom has no handheld hardware, so a player's shipped log
		// is the ONLY way to debug detection in the field.
		UE_LOG(CombatForgeLog, Log, TEXT("HandheldDetect: cpu='%s' gpu='%s' SteamDeckEnv='%s' -> %s"),
			*FPlatformMisc::GetCPUBrand(),
			*FPlatformMisc::GetPrimaryGPUBrand(),
			*FPlatformMisc::GetEnvironmentVariable(TEXT("SteamDeck")),
			FPFHandheldPlatform::KindName(FPFHandheldPlatform::Detect()));
	}
}

EPFHandheldKind FPFHandheldPlatform::Detect()
{
	static const EPFHandheldKind Cached = DetectUncached();
	return Cached;
}

const TCHAR* FPFHandheldPlatform::KindName(EPFHandheldKind Kind)
{
	switch (Kind)
	{
	case EPFHandheldKind::WindowsHandheld: return TEXT("WindowsHandheld");
	case EPFHandheldKind::SteamDeck:       return TEXT("SteamDeck");
	default:                               return TEXT("None");
	}
}

void FPFHandheldPlatform::InitAtBoot()
{
	LogDetectionSignals();

	const EPFHandheldKind Kind = Detect();
	if (Kind != EPFHandheldKind::None &&
		FPFUserPrefs::GetHandheldPresetApplied() < HandheldPresetVersion)
	{
		UE_LOG(CombatForgeLog, Log, TEXT("HandheldDetect: first run on %s - applying handheld preset v%d"),
			KindName(Kind), HandheldPresetVersion);
		ApplyHandheldPreset(Kind);
		FPFUserPrefs::SetHandheldPresetApplied(HandheldPresetVersion);
		FPFUserPrefs::Flush();
	}

	ApplyUIScaleFromPrefs();
}

void FPFHandheldPlatform::ApplyHandheldPreset(EPFHandheldKind Kind)
{
	if (Kind == EPFHandheldKind::None)
	{
		return;
	}
	const bool bDeck = (Kind == EPFHandheldKind::SteamDeck);

	// Preset targets. Windows handhelds (Z1-class RDNA3): Medium + 65% render scale at
	// 1080p ~= a steady 60 on this content's budget. Deck (Van Gogh RDNA2, ~half the GPU):
	// Low + 75% of its 800p desktop. Both cap at 60 fps — handheld batteries buy nothing
	// above that. UI scale up for 7-8" panels at arm's length.
	const int32 Quality   = bDeck ? 0 : 1;
	const int32 ResIndex  = bDeck ? DeckResIndex : WinHandheldResIndex;
	const float ResScale  = bDeck ? 75.f : 65.f;
	const float UIScale   = bDeck ? 1.10f : 1.15f;

	FPFUserPrefs::SetQualityLevel(Quality);
	FPFUserPrefs::SetResolutionIndex(ResIndex);
	FPFUserPrefs::SetResolutionScalePct(ResScale);
	FPFUserPrefs::SetFrameRateLimitIndex(0);   // 60
	FPFUserPrefs::SetWindowModeIndex(0);       // fullscreen (borderless)
	FPFUserPrefs::SetUIScale(UIScale);

	// Engine-side apply — mirrors UPFOptionsWidget::PushToSettings exactly (borderless
	// only; the resolution pick folds into render scale against the desktop height).
	if (UGameUserSettings* S = GEngine ? GEngine->GetGameUserSettings() : nullptr)
	{
		S->SetFullscreenMode(EWindowMode::WindowedFullscreen);
		S->SetOverallScalabilityLevel(Quality);
		FPFUserPrefs::ApplyQualityMethodCVars(Quality);
		S->SetFrameRateLimit(FPFUserPrefs::FrameRateLimitForIndex(0));

		static const int32 W[] = { 1280, 1366, 1600, 1920, 2560 };
		static const int32 H[] = { 720,  768,  900,  1080, 1440 };
		const int32 i = FMath::Clamp(ResIndex, 0, static_cast<int32>(UE_ARRAY_COUNT(W)) - 1);
		float Scale = FMath::Clamp(ResScale / 100.f, 0.5f, 1.f);
		const FIntPoint Desktop = S->GetDesktopResolution();
		if (Desktop.Y > 0)
		{
			Scale = FMath::Clamp(Scale * (static_cast<float>(H[i]) / static_cast<float>(Desktop.Y)), 0.5f, 1.f);
		}
		S->SetResolutionScaleNormalized(Scale);
		S->SetScreenResolution(FIntPoint(W[i], H[i]));
		S->ApplySettings(false);
		S->SaveSettings();
	}

	ApplyUIScaleFromPrefs();
	UE_LOG(CombatForgeLog, Log, TEXT("Handheld preset applied (%s): quality=%d resIdx=%d resScale=%.0f%% uiScale=%.2f cap=60"),
		KindName(Kind), Quality, ResIndex, ResScale, UIScale);
}

void FPFHandheldPlatform::ApplyUIScaleFromPrefs()
{
	// ApplicationScale multiplies the whole Slate DPI on top of the resolution curve —
	// the supported runtime knob for "make everything bigger on a 7-inch panel".
	if (UUserInterfaceSettings* UI = GetMutableDefault<UUserInterfaceSettings>())
	{
		UI->ApplicationScale = FPFUserPrefs::GetUIScale();
	}
}

// ---------------------------------------------------------------- console helpers

static FAutoConsoleCommand GPFHandheldInfoCmd(
	TEXT("pf.HandheldInfo"),
	TEXT("Log handheld detection signals (cpu/gpu/env) and the detected kind."),
	FConsoleCommandDelegate::CreateStatic([]()
	{
		LogDetectionSignals();
	}));

static FAutoConsoleCommand GPFHandheldPresetCmd(
	TEXT("pf.HandheldPreset"),
	TEXT("Apply the handheld performance preset now. Args: none = detected kind; '1' = Windows handheld; '2' = Steam Deck."),
	FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& Args)
	{
		EPFHandheldKind Kind = FPFHandheldPlatform::Detect();
		if (Args.Num() > 0)
		{
			const int32 Forced = FCString::Atoi(*Args[0]);
			Kind = (Forced == 2) ? EPFHandheldKind::SteamDeck
			     : (Forced == 1) ? EPFHandheldKind::WindowsHandheld
			     : Kind;
		}
		if (Kind == EPFHandheldKind::None)
		{
			UE_LOG(CombatForgeLog, Log, TEXT("pf.HandheldPreset: no handheld detected - pass 1 (Windows handheld) or 2 (Steam Deck) to force"));
			return;
		}
		FPFHandheldPlatform::ApplyHandheldPreset(Kind);
	}));
