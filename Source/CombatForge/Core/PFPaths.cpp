// Copyright (c) 2026 Tom Chapman. All rights reserved.

#include "Core/PFPaths.h"

#include "CombatForge.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/Paths.h"

const FString& FPFPaths::ArenaDir()
{
	// Function-local static: directory creation + the one-time legacy migration run exactly ONCE, before any
	// first read or write (including EnsureSeedArenas — which would otherwise fill the new dir with seeds and
	// defeat the migrate-only-if-empty check).
	static const FString Dir = []() -> FString
	{
		const FString NewDir = FPaths::ConvertRelativePathToFull(
			FString(FPlatformProcess::UserSettingsDir()) / TEXT("CombatForge") / TEXT("Arenas"));
		IFileManager& FM = IFileManager::Get();
		FM.MakeDirectory(*NewDir, /*Tree=*/true);

		// One-time migration: if the stable dir has no arena records yet but the legacy in-install location
		// does, copy everything over (.json records AND the .png previews beside them — the map picker hides
		// arenas without a preview, so a json-only migration would make every migrated map invisible).
		TArray<FString> NewFiles;
		FM.FindFiles(NewFiles, *(NewDir / TEXT("*.json")), /*Files=*/true, /*Dirs=*/false);
		if (NewFiles.Num() == 0)
		{
			const FString LegacyDir = FPaths::ProjectSavedDir() / TEXT("Arenas");
			TArray<FString> LegacyFiles;
			FM.FindFiles(LegacyFiles, *(LegacyDir / TEXT("*.json")), true, false);
			FM.FindFiles(LegacyFiles, *(LegacyDir / TEXT("*.png")), true, false);
			int32 Copied = 0;
			for (const FString& F : LegacyFiles)
			{
				if (FM.Copy(*(NewDir / F), *(LegacyDir / F)) == COPY_OK)
				{
					++Copied;
				}
			}
			if (Copied > 0)
			{
				UE_LOG(CombatForgeLog, Log, TEXT("PFPaths: migrated %d legacy arena files -> %s"), Copied, *NewDir);
			}
		}
		return NewDir;
	}();
	return Dir;
}
