// Copyright (c) 2026 Tom Chapman. All rights reserved.

#include "Core/CombatForgeGameInstance.h"

#include "CombatForge.h"
#include "Core/PFUserPrefs.h"
#include "Dom/JsonObject.h"
#include "GameFramework/GameUserSettings.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

void UCombatForgeGameInstance::Init()
{
	Super::Init();
	LoadOrCreateIdentity();

	// Frame cap from OUR pref (default 144) — uncapped rendering pegs any GPU at ~100% (an RTX 5090 sat at
	// 86-90% drawing 300+ fps of a simple arena) for zero gameplay gain. Applied every boot so players who
	// never open Options still get the cap; the Options FPS-limit row edits the same pref.
	if (GEngine != nullptr)
	{
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
	return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("CombatForge"), TEXT("Identity.json"));
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
