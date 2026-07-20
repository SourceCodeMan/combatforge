// Copyright (c) 2026 Tom Chapman. All rights reserved.

#include "Core/PFPaths.h"

#include "CombatForge.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
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
		//   1. -ArenaDir=<abs path> command line  (DEDICATED SERVER override — preferred)
		//   2. Machine-stable ProgramData (or /var/lib) when -NOHOMEDIR / dedicated / nullrhi —
		//      UserSettingsDir under -NOHOMEDIR resolves INSIDE the extracted build and is wiped on
		//      every redeploy, which silently dropped every community map (#9 / Tom 2026-07-18).
		//   3. UserSettingsDir()/CombatForge/Arenas  (desktop listen host / solo default)
		FString OverrideDir;
		FString BaseDir;
		if (FParse::Value(FCommandLine::Get(), TEXT("ArenaDir="), OverrideDir) && !OverrideDir.TrimStartAndEnd().IsEmpty())
		{
			BaseDir = OverrideDir.TrimStartAndEnd();
			// FParse leaves surrounding quotes when the arg was quoted on the cmdline.
			BaseDir.TrimStartAndEndInline();
			if (BaseDir.StartsWith(TEXT("\"")) && BaseDir.EndsWith(TEXT("\"")) && BaseDir.Len() >= 2)
			{
				BaseDir = BaseDir.Mid(1, BaseDir.Len() - 2);
			}
		}
		else
		{
			const TCHAR* Cmd = FCommandLine::Get();
			const bool bNoHome = FParse::Param(Cmd, TEXT("NOHOMEDIR"));
			const bool bNullRhi = FParse::Param(Cmd, TEXT("nullrhi"));
			const bool bServerFlag = FParse::Param(Cmd, TEXT("server"));
			if (bNoHome || bNullRhi || bServerFlag)
			{
#if PLATFORM_WINDOWS
				FString ProgData = FPlatformMisc::GetEnvironmentVariable(TEXT("ProgramData"));
				if (ProgData.IsEmpty())
				{
					ProgData = TEXT("C:/ProgramData");
				}
				BaseDir = ProgData / TEXT("CombatForge") / TEXT("Arenas");
#else
				BaseDir = TEXT("/var/lib/combatforge/Arenas");
#endif
			}
			else
			{
				BaseDir = FString(FPlatformProcess::UserSettingsDir()) / TEXT("CombatForge") / TEXT("Arenas");
			}
		}
		const FString NewDir = FPaths::ConvertRelativePathToFull(BaseDir);
		IFileManager& FM = IFileManager::Get();
		FM.MakeDirectory(*NewDir, /*Tree=*/true);

		// One-time migration: if the stable dir has no arena records yet but a legacy location does,
		// copy everything over (.json records AND .png previews). Check both the in-install Saved/Arenas
		// and (when on the ProgramData path) the per-user LocalAppData dir so a box that once wrote
		// maps under UserSettingsDir still finds them after the -NOHOMEDIR fix.
		TArray<FString> NewFiles;
		FM.FindFiles(NewFiles, *(NewDir / TEXT("*.json")), /*Files=*/true, /*Dirs=*/false);
		if (NewFiles.Num() == 0)
		{
			TArray<FString> LegacyRoots;
			LegacyRoots.Add(FPaths::ProjectSavedDir() / TEXT("Arenas"));
			const FString UserArenas =
				FString(FPlatformProcess::UserSettingsDir()) / TEXT("CombatForge") / TEXT("Arenas");
			if (!UserArenas.Equals(NewDir, ESearchCase::IgnoreCase))
			{
				LegacyRoots.Add(UserArenas);
			}
			int32 Copied = 0;
			for (const FString& LegacyDir : LegacyRoots)
			{
				TArray<FString> LegacyFiles;
				FM.FindFiles(LegacyFiles, *(LegacyDir / TEXT("*.json")), true, false);
				FM.FindFiles(LegacyFiles, *(LegacyDir / TEXT("*.png")), true, false);
				for (const FString& F : LegacyFiles)
				{
					if (FM.Copy(*(NewDir / F), *(LegacyDir / F)) == COPY_OK)
					{
						++Copied;
					}
				}
			}
			if (Copied > 0)
			{
				UE_LOG(CombatForgeLog, Log, TEXT("PFPaths: migrated %d legacy arena files -> %s"), Copied, *NewDir);
			}
		}
		UE_LOG(CombatForgeLog, Warning, TEXT("PFPaths: ArenaDir = %s"), *NewDir);
		return NewDir;
	}();
	return Dir;
}

FString FPFPaths::ServerDataDir()
{
	// Parent of the (persistent, possibly -ArenaDir-overridden) arena dir. Created lazily by callers as needed.
	return FPaths::GetPath(ArenaDir());
}
