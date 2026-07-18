// Copyright (c) 2026 Tom Chapman. All rights reserved.

#include "Core/PFPaths.h"

#include "CombatForge.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"

const FString& FPFPaths::ArenaDir()
{
	// Function-local static: directory creation + the one-time legacy migration run exactly ONCE, before any
	// first read or write (including EnsureSeedArenas — which would otherwise fill the new dir with seeds and
	// defeat the migrate-only-if-empty check).
	static const FString Dir = []() -> FString
	{
		// Save location, in priority order:
		//   1. -ArenaDir=<abs path> command line  (DEDICATED SERVER override)
		//   2. UserSettingsDir()/CombatForge/Arenas  (per-user, the desktop default)
		// The override exists for the Linux VPS: UserSettingsDir() resolves under $HOME/.config there, which a
		// redeploy can wipe if it resets $HOME. Passing -ArenaDir=/opt/combatforge-data/Arenas (a dir OUTSIDE the
		// extracted build tree) guarantees built maps survive every server binary update (Tom 2026-07-18, #9).
		FString OverrideDir;
		FString BaseDir;
		if (FParse::Value(FCommandLine::Get(), TEXT("ArenaDir="), OverrideDir) && !OverrideDir.TrimStartAndEnd().IsEmpty())
		{
			BaseDir = OverrideDir.TrimStartAndEnd();
		}
		else
		{
			BaseDir = FString(FPlatformProcess::UserSettingsDir()) / TEXT("CombatForge") / TEXT("Arenas");
		}
		const FString NewDir = FPaths::ConvertRelativePathToFull(BaseDir);
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

FString FPFPaths::ServerDataDir()
{
	// Parent of the (persistent, possibly -ArenaDir-overridden) arena dir. Created lazily by callers as needed.
	return FPaths::GetPath(ArenaDir());
}
