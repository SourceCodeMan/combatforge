// Copyright (c) 2026 Tom Chapman. All rights reserved.

#include "Core/CombatForgeGameInstance.h"

#include "CombatForge.h"
#include "Core/PFHandheldPlatform.h"
#include "Core/PFUserPrefs.h"
#include "Dom/JsonObject.h"
#include "Framework/Application/SlateApplication.h"
#include "Input/PFGamepadCursor.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/GameUserSettings.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/FileHelper.h"
#include "Misc/NetworkVersion.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

void UCombatForgeGameInstance::Init()
{
	Super::Init();
	LoadOrCreateIdentity();

	// Multiplayer build gate: fold our engine version + game build number (PFBuild::NetProtocol) into the
	// network version so a client on a DIFFERENT CombatForge build fails the join handshake with a clean
	// version-mismatch error, instead of connecting and mirroring pieces through stale code (the giant-box /
	// missing-skin version-skew bug). Same build both ends → same value → compatible. Bound at boot, before
	// any networking, so the first GetLocalNetworkVersion() computation picks it up.
	if (!FNetworkVersion::GetLocalNetworkVersionOverride.IsBound())
	{
		FNetworkVersion::GetLocalNetworkVersionOverride.BindLambda([]() -> uint32
		{
			uint32 V = FCrc::StrCrc32(TEXT("CombatForge"));
			V = FCrc::TypeCrc32(static_cast<uint32>(ENGINE_MAJOR_VERSION), V);
			V = FCrc::TypeCrc32(static_cast<uint32>(ENGINE_MINOR_VERSION), V);
			V = FCrc::TypeCrc32(static_cast<uint32>(PFBuild::NetProtocol), V);
			return V;
		});
	}

	// Handheld detect + first-run preset BEFORE the frame-cap/quality applies below, so the
	// prefs the preset writes are what this boot then applies. Also pushes the saved UI scale.
	FPFHandheldPlatform::InitAtBoot();

	// Gamepad-as-pointer for every mouse-driven menu (boot menu, lobby, options, vote).
	RegisterGamepadCursor();

	// Frame cap from OUR pref (default 144) — uncapped rendering pegs any GPU at ~100% (an RTX 5090 sat at
	// 86-90% drawing 300+ fps of a simple arena) for zero gameplay gain. Applied every boot so players who
	// never open Options still get the cap; the Options FPS-limit row edits the same pref.
	if (GEngine != nullptr)
	{
		// Screen debug text OFF by default (Tom: cluttered playtest). Re-enable with console
		// `EnableAllScreenMessages` or `GEngine->bEnableOnScreenDebugMessages = true`.
		GEngine->bEnableOnScreenDebugMessages = false;

		if (UGameUserSettings* S = GEngine->GetGameUserSettings())
		{
			S->SetFrameRateLimit(FPFUserPrefs::FrameRateLimitForIndex(FPFUserPrefs::GetFrameRateLimitIndex()));
			// NON-resolution apply only: full ApplySettings re-requested the SAVED window mode/resolution,
			// stomping -WINDOWED -ResX/-ResY launches (broke the 2-player playtest launcher).
			S->ApplyNonResolutionSettings();
		}
	}
	// Pipeline method switches (Lumen/VSM/TSR vs the budget path) for the saved quality — these cvars are
	// not scalability-flagged, so the ini can't set them (see FPFUserPrefs::ApplyQualityMethodCVars).
	FPFUserPrefs::ApplyQualityMethodCVars(FPFUserPrefs::GetQualityLevel());
}

void UCombatForgeGameInstance::RegisterGamepadCursor()
{
	// Clients only: dedicated servers have no Slate, commandlets (cook) must stay clean.
	if (IsRunningDedicatedServer() || IsRunningCommandlet() || !FSlateApplication::IsInitialized())
	{
		return;
	}
	if (!GamepadCursor.IsValid())
	{
		GamepadCursor = MakeShared<FPFGamepadCursor>(this);
		FSlateApplication::Get().RegisterInputPreProcessor(GamepadCursor);
		UE_LOG(CombatForgeLog, Log, TEXT("Gamepad UI cursor registered"));
	}
}

void UCombatForgeGameInstance::UnregisterGamepadCursor()
{
	if (GamepadCursor.IsValid())
	{
		if (FSlateApplication::IsInitialized())
		{
			FSlateApplication::Get().UnregisterInputPreProcessor(GamepadCursor);
		}
		GamepadCursor.Reset();
	}
}

void UCombatForgeGameInstance::Shutdown()
{
	UnregisterGamepadCursor();

	// Process/PIE teardown: always drop listen-server / client net drivers so the next boot never inherits
	// a hosted session. HOST LAN is opt-in only (menu HOST button or an explicit ?listen launch URL).
	if (UWorld* World = GetWorld())
	{
		if (World->GetNetMode() != NM_Standalone && GEngine != nullptr)
		{
			UE_LOG(CombatForgeLog, Log, TEXT("GameInstance::Shutdown: tearing down net driver"));
			GEngine->ShutdownWorldNetDriver(World);
		}
	}
	Super::Shutdown();
}

FGuid UCombatForgeGameInstance::GetLocalPlayerGuid() const
{
	return LocalPlayerGuid;
}

FString UCombatForgeGameInstance::GetLocalPlayerGuidHash() const
{
	return LocalPlayerGuidHash;
}

FString UCombatForgeGameInstance::GetIdentityFilePath() const
{
	// Per-user location (NOT ProjectSavedDir, which is inside a portable/packaged build) so the install GUID is
	// never shipped and each machine gets its own. Matches Auth.json + arenas under %LOCALAPPDATA%/CombatForge.
	// NOTE: contract T24 still writes this as Saved/CombatForge/Identity.json. UserSettingsDir is correct and
	// deliberate (the Saved/ path leaked a session token in alpha-4/5); the contract text is the stale one. (P2-C11)
	return FPaths::Combine(FString(FPlatformProcess::UserSettingsDir()), TEXT("CombatForge"), TEXT("Identity.json"));
}

void UCombatForgeGameInstance::LoadOrCreateIdentity()
{
	const FString FilePath = GetIdentityFilePath();

	// Load an existing identity if the file parses to a valid GUID.
	FString FileContents;
	if (FFileHelper::LoadFileToString(FileContents, *FilePath))
	{
		TSharedPtr<FJsonObject> Root;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(FileContents);
		if (FJsonSerializer::Deserialize(Reader, Root) && Root.IsValid())
		{
			FString GuidString;
			FGuid ParsedGuid;
			if (Root->TryGetStringField(TEXT("installGuid"), GuidString) &&
				FGuid::Parse(GuidString, ParsedGuid) && ParsedGuid.IsValid())
			{
				LocalPlayerGuid = ParsedGuid;
			}
		}

		if (!LocalPlayerGuid.IsValid())
		{
			UE_LOG(CombatForgeLog, Warning,
				TEXT("Identity file at %s is unreadable or invalid - generating a new install GUID"), *FilePath);
		}
	}

	// First run (or corrupt file): mint and persist a new install GUID.
	if (!LocalPlayerGuid.IsValid())
	{
		LocalPlayerGuid = FGuid::NewGuid();

		const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetNumberField(TEXT("schema"), 1);
		Root->SetStringField(TEXT("installGuid"), LocalPlayerGuid.ToString(EGuidFormats::DigitsWithHyphens).ToLower());

		FString Output;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output);
		FJsonSerializer::Serialize(Root, Writer);

		IFileManager::Get().MakeDirectory(*FPaths::GetPath(FilePath), /*Tree=*/true);
		if (!FFileHelper::SaveStringToFile(Output, *FilePath))
		{
			UE_LOG(CombatForgeLog, Error,
				TEXT("Failed to persist identity file to %s - identity will change next run"), *FilePath);
		}
	}

	// Cache the anonymized wire/disk form: lowercase SHA1-hex of the lowercase hyphenated GUID
	// string (T24). This exact derivation must stay stable forever - it keys vote records.
	const FString GuidString = LocalPlayerGuid.ToString(EGuidFormats::DigitsWithHyphens).ToLower();
	const FTCHARToUTF8 Utf8(*GuidString);
	constexpr int32 Sha1DigestSize = 20;
	uint8 Digest[Sha1DigestSize];
	FSHA1::HashBuffer(Utf8.Get(), Utf8.Length(), Digest);
	LocalPlayerGuidHash = BytesToHex(Digest, Sha1DigestSize).ToLower();

	UE_LOG(CombatForgeLog, Log, TEXT("Install identity ready (hash %s...)"), *LocalPlayerGuidHash.Left(8));
}
