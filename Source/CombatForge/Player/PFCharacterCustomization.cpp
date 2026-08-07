// Copyright (c) 2026 Tom Chapman. All rights reserved.

#include "Player/PFCharacterCustomization.h"
#include "Core/PFPaths.h"

#include "CombatForge.h"
#include "AssetRegistry/AssetData.h"
#include "Engine/ObjectLibrary.h"
#include "Engine/SkeletalMesh.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/PackageName.h"

namespace
{
	struct FSlotDef { const TCHAR* Id; const TCHAR* Label; const TCHAR* Dir; };

	// Customizable overlay slots (worn over the SKM_Body base). Order == slot index.
	const FSlotDef GSlots[] = {
		{ TEXT("Head"),     TEXT("Headwear"), TEXT("/Game/Bandits/Mesh/Head_Module")   },
		{ TEXT("Face"),     TEXT("Face"),     TEXT("/Game/Bandits/Mesh/Face_Modul")     },
		{ TEXT("Helmet"),   TEXT("Helmet"),   TEXT("/Game/Bandits/Mesh/Helmet_Module")  },
		{ TEXT("Chest"),    TEXT("Chest"),    TEXT("/Game/Bandits/Mesh/Chest")          },
		{ TEXT("Arms"),     TEXT("Arms"),     TEXT("/Game/Bandits/Mesh/Arms")           },
		{ TEXT("Hips"),     TEXT("Hips"),     TEXT("/Game/Bandits/Mesh/Hips_Module")    },
		{ TEXT("Pants"),    TEXT("Pants"),    TEXT("/Game/Bandits/Mesh/Pants")          },
		{ TEXT("Cloth"),    TEXT("Shirt"),    TEXT("/Game/Bandits/Mesh/Cloth")          },   // pack folder is "Cloth"; players see "Shirt" (internal key unchanged so saved slots keep working)
		{ TEXT("Backpack"), TEXT("Backpack"), TEXT("/Game/Bandits/Mesh/Bacpacks")       },
	};
	constexpr int32 GSlotCount = UE_ARRAY_COUNT(GSlots);

	TArray<TArray<FSoftObjectPath>> GSlotPartsCache;
	TArray<FSoftObjectPath> GBasePartsCache;
	bool GBuilt = false;

	void EnumerateDir(const TCHAR* Dir, TArray<FSoftObjectPath>& Out)
	{
		Out.Reset();
		UObjectLibrary* Lib = UObjectLibrary::CreateLibrary(USkeletalMesh::StaticClass(), /*bHasBlueprintClasses=*/false, GIsEditor);
		if (Lib == nullptr)
		{
			return;
		}
		Lib->AddToRoot();
		Lib->LoadAssetDataFromPath(Dir);   // loads asset DATA only (metadata), not the meshes
		TArray<FAssetData> Data;
		Lib->GetAssetDataList(Data);
		for (const FAssetData& A : Data)
		{
			Out.Add(A.ToSoftObjectPath());
		}
		Out.Sort([](const FSoftObjectPath& X, const FSoftObjectPath& Y) { return X.ToString() < Y.ToString(); });
		Lib->RemoveFromRoot();
	}

	void EnsureBuilt()
	{
		if (GBuilt)
		{
			return;
		}
		GBuilt = true;
		GSlotPartsCache.SetNum(GSlotCount);
		int32 Total = 0;
		for (int32 i = 0; i < GSlotCount; ++i)
		{
			EnumerateDir(GSlots[i].Dir, GSlotPartsCache[i]);

			// Strip parts that must never ride the modular character:
			//  - Jacket tails (frozen coat bones → rigid "hip fin")
			//  - Weapon cosmetics (AK_Drops chest hangers, etc.): LeaderPose freezes them into a FIXED
			//    second gun on the body while WeaponMeshComp is the real in-hand gun (Tom: everyone
			//    carries two rifles — hands + hip). Gameplay weapons only come from PFWeaponCatalog.
			{
				static const TCHAR* ExcludeSubstrings[] = {
					TEXT("Jacket"),
					TEXT("AK_Drops"),
					TEXT("AK_Drop"),
					TEXT("/Weapon/"),
					TEXT("SM_AK"),
					TEXT("SM_Pistol"),
					TEXT("SM_Rifle"),
				};
				GSlotPartsCache[i].RemoveAll([](const FSoftObjectPath& P)
				{
					const FString S = P.ToString();
					for (const TCHAR* Bad : ExcludeSubstrings)
					{
						if (S.Contains(Bad)) { return true; }
					}
					return false;
				});
			}

			// Procedural running shorts (Scripts/gen_running_shorts.py). ObjectLibrary + the asset-registry
			// cache can miss brand-new packages until a full editor rescan — pin them on the Pants slot
			// so the class menu always offers them when the assets exist on disk.
			if (i == PFChar::kSlotPants)
			{
				static const TCHAR* ForcedShorts[] = {
					TEXT("/Game/Bandits/Mesh/Pants/RunningShorts/SKM_RunningShorts_Hearts.SKM_RunningShorts_Hearts"),
					TEXT("/Game/Bandits/Mesh/Pants/RunningShorts/SKM_RunningShorts_Leopard.SKM_RunningShorts_Leopard"),
				};
				for (const TCHAR* Path : ForcedShorts)
				{
					const FSoftObjectPath Soft(Path);
					const bool bAlready = GSlotPartsCache[i].ContainsByPredicate(
						[&Soft](const FSoftObjectPath& P) { return P == Soft; });
					if (!bAlready && FPackageName::DoesPackageExist(Soft.GetLongPackageName()))
					{
						GSlotPartsCache[i].Add(Soft);
						UE_LOG(CombatForgeLog, Log, TEXT("PFChar: forced pants option %s"), Path);
					}
				}
				GSlotPartsCache[i].Sort([](const FSoftObjectPath& X, const FSoftObjectPath& Y)
				{
					return X.ToString() < Y.ToString();
				});
			}

			Total += GSlotPartsCache[i].Num();
		}
		// Visible base SKIN as a MODULAR set (head + torso + arms + legs). SKM_Body is a ONE-PIECE naked body
		// (torso + arms + LEGS — material M_Body_Full, PA_Body_PhysicsAsset has thigh/calf/foot bodies) and is
		// now only the skeleton/anim/bounds carrier, never rendered. Splitting the skin is what lets the LEG
		// region be hidden under trousers; previously SKM_Body's own legs kept rendering under the jeans and
		// poked through at the inner thigh no matter what we hid (Tom 2026-07-18).
		// Order MUST match PFChar::kBaseHead/kBaseTorso/kBaseArms/kBaseLegs.
		GBasePartsCache.Add(FSoftObjectPath(TEXT("/Game/Bandits/Mesh/Body/SKM_Head.SKM_Head")));       // kBaseHead
		GBasePartsCache.Add(FSoftObjectPath(TEXT("/Game/Bandits/Mesh/Body/SKM_Torso.SKM_Torso")));     // kBaseTorso
		GBasePartsCache.Add(FSoftObjectPath(TEXT("/Game/Bandits/Mesh/Arms/Arms/SKM_Arms.SKM_Arms")));  // kBaseArms
		GBasePartsCache.Add(FSoftObjectPath(TEXT("/Game/Bandits/Mesh/Body/SKM_Legs.SKM_Legs")));       // kBaseLegs
		UE_LOG(CombatForgeLog, Log, TEXT("PFChar: enumerated %d parts across %d slots."), Total, GSlotCount);
	}
}

namespace PFChar
{
	int32 SlotCount() { return GSlotCount; }

	FName SlotId(int32 Slot)
	{
		return (Slot >= 0 && Slot < GSlotCount) ? FName(GSlots[Slot].Id) : NAME_None;
	}

	FString SlotLabel(int32 Slot)
	{
		return (Slot >= 0 && Slot < GSlotCount) ? FString(GSlots[Slot].Label) : FString();
	}

	const TArray<FSoftObjectPath>& SlotParts(int32 Slot)
	{
		EnsureBuilt();
		static const TArray<FSoftObjectPath> Empty;
		return (Slot >= 0 && Slot < GSlotPartsCache.Num()) ? GSlotPartsCache[Slot] : Empty;
	}

	const TArray<FSoftObjectPath>& BaseParts()
	{
		EnsureBuilt();
		return GBasePartsCache;
	}

	USkeletalMesh* LoadPart(int32 Slot, int32 Index)
	{
		const TArray<FSoftObjectPath>& Parts = SlotParts(Slot);
		if (Index < 0 || Index >= Parts.Num())
		{
			return nullptr;
		}
		return Cast<USkeletalMesh>(Parts[Index].TryLoad());
	}

	int32 TotalPartCount()
	{
		EnsureBuilt();
		int32 Total = 0;
		for (const TArray<FSoftObjectPath>& S : GSlotPartsCache)
		{
			Total += S.Num();
		}
		return Total;
	}

	FPFCharacterConfig DefaultConfig()
	{
		EnsureBuilt();
		FPFCharacterConfig C;
		C.Slots.Init(-1, GSlotCount);
		// Starting outfit: first option for the key visible slots (guarded to what actually enumerated).
		auto SetFirst = [&C](const TCHAR* Id)
		{
			for (int32 i = 0; i < GSlotCount; ++i)
			{
				if (FCString::Stricmp(GSlots[i].Id, Id) == 0)
				{
					C.Slots[i] = GSlotPartsCache[i].Num() > 0 ? 0 : -1;
					return;
				}
			}
		};
		SetFirst(TEXT("Head"));
		SetFirst(TEXT("Chest"));
		SetFirst(TEXT("Pants"));
		SetFirst(TEXT("Arms"));
		return C;
	}

	FString PartDisplayName(int32 Slot, int32 Index)
	{
		if (Index < 0)
		{
			return TEXT("None");
		}
		const TArray<FSoftObjectPath>& Parts = SlotParts(Slot);
		if (!Parts.IsValidIndex(Index))
		{
			return TEXT("None");
		}
		FString Name = Parts[Index].GetAssetName();   // e.g. "SKM_Arafatka_Bege"
		Name.RemoveFromStart(TEXT("SKM_"));
		// Procedural running shorts — friendlier labels than "Running Shorts Hearts".
		if (Name.Equals(TEXT("RunningShorts_Hearts"), ESearchCase::IgnoreCase))
		{
			return TEXT("Running Shorts (Hearts)");
		}
		if (Name.Equals(TEXT("RunningShorts_Leopard"), ESearchCase::IgnoreCase))
		{
			return TEXT("Running Shorts (Leopard)");
		}
		Name.ReplaceInline(TEXT("_"), TEXT(" "));
		return Name;
	}

	bool PantsLeaveLegsVisible(int32 PantsPartIndex)
	{
		if (PantsPartIndex < 0)
		{
			return false;
		}
		const TArray<FSoftObjectPath>& Parts = SlotParts(kSlotPants);
		if (!Parts.IsValidIndex(PantsPartIndex))
		{
			return false;
		}
		// Content lives under …/Pants/RunningShorts/SKM_RunningShorts_*. Generated by
		// Scripts/gen_running_shorts.py — knee-mesh + procedural hearts/leopard materials.
		const FString Path = Parts[PantsPartIndex].ToString();
		return Path.Contains(TEXT("RunningShorts"), ESearchCase::IgnoreCase);
	}

	static constexpr int32 kSaveSlots = 5;

	static int32 ClampSaveSlot(int32 SaveSlot)
	{
		return FMath::Clamp(SaveSlot, 0, kSaveSlots - 1);
	}

	int32 SaveSlotCount()
	{
		return kSaveSlots;
	}

	// Prefs section: project was renamed PaintForge → CombatForge. Older installs still have class data under
	// [PaintForge]; new code only wrote weapons under [CombatForge], so clothing looked "never saved".
	// Readers try CombatForge first, then PaintForge; writers always land in CombatForge.
	static const TCHAR* PrefSectionPrimary = TEXT("CombatForge");
	static const TCHAR* PrefSectionLegacy  = TEXT("PaintForge");

	static bool ReadIntEither(const TCHAR* Key, int32& Out)
	{
		if (GConfig == nullptr)
		{
			return false;
		}
		if (GConfig->GetInt(PrefSectionPrimary, Key, Out, FPFPaths::UserPrefsIni()))
		{
			return true;
		}
		return GConfig->GetInt(PrefSectionLegacy, Key, Out, FPFPaths::UserPrefsIni());
	}

	static bool ReadBoolEither(const TCHAR* Key, bool& Out)
	{
		if (GConfig == nullptr)
		{
			return false;
		}
		if (GConfig->GetBool(PrefSectionPrimary, Key, Out, FPFPaths::UserPrefsIni()))
		{
			return true;
		}
		return GConfig->GetBool(PrefSectionLegacy, Key, Out, FPFPaths::UserPrefsIni());
	}

	int32 GetActiveSaveSlot()
	{
		int32 Active = 0;
		ReadIntEither(TEXT("CharActiveSlot"), Active);
		return ClampSaveSlot(Active);
	}

	void SetActiveSaveSlot(int32 SaveSlot)
	{
		if (GConfig == nullptr)
		{
			return;
		}
		// Flush so a menu slot switch survives a quick process kill (class setup was "never saved").
		GConfig->SetInt(PrefSectionPrimary, TEXT("CharActiveSlot"), ClampSaveSlot(SaveSlot), FPFPaths::UserPrefsIni());
		GConfig->Flush(false, FPFPaths::UserPrefsIni());
	}

	void SaveConfig(int32 SaveSlot, const FPFCharacterConfig& Config)
	{
		if (GConfig == nullptr)
		{
			return;
		}
		const int32 S = ClampSaveSlot(SaveSlot);
		GConfig->SetBool(PrefSectionPrimary, *FString::Printf(TEXT("CharSaved%d"), S), true, FPFPaths::UserPrefsIni());
		for (int32 i = 0; i < GSlotCount; ++i)
		{
			const int32 Sel = Config.Slots.IsValidIndex(i) ? Config.Slots[i] : -1;
			const FString Key = FString::Printf(TEXT("CharS%d_%s"), S, GSlots[i].Id);
			GConfig->SetInt(PrefSectionPrimary, *Key, Sel, FPFPaths::UserPrefsIni());
		}
		// Mirror active slot so LoadConfig() without an explicit slot stays coherent.
		GConfig->SetInt(PrefSectionPrimary, TEXT("CharActiveSlot"), S, FPFPaths::UserPrefsIni());
		GConfig->Flush(false, FPFPaths::UserPrefsIni());
	}

	FPFCharacterConfig LoadConfig(int32 SaveSlot)
	{
		EnsureBuilt();
		const int32 S = ClampSaveSlot(SaveSlot);
		bool bSaved = false;
		const FString SavedKey = FString::Printf(TEXT("CharSaved%d"), S);
		ReadBoolEither(*SavedKey, bSaved);

		// One-time migrate: pre-slot PaintForge single outfit (CharConfigSaved + CharSlot_*) → class slot 0.
		if (!bSaved && S == 0 && GConfig != nullptr)
		{
			bool bLegacySingle = false;
			if (GConfig->GetBool(PrefSectionLegacy, TEXT("CharConfigSaved"), bLegacySingle, FPFPaths::UserPrefsIni())
				&& bLegacySingle)
			{
				bSaved = true;   // fall through and read CharSlot_* below as CharS0
			}
		}

		if (!bSaved)
		{
			return DefaultConfig();
		}
		FPFCharacterConfig C;
		C.Slots.Init(-1, GSlotCount);
		for (int32 i = 0; i < GSlotCount; ++i)
		{
			int32 Sel = -1;
			const FString PerSlotKey = FString::Printf(TEXT("CharS%d_%s"), S, GSlots[i].Id);
			if (!ReadIntEither(*PerSlotKey, Sel) && S == 0)
			{
				// Legacy single-outfit keys (CharSlot_Head, …) only seed class 0.
				const FString LegacyKey = FString::Printf(TEXT("CharSlot_%s"), GSlots[i].Id);
				ReadIntEither(*LegacyKey, Sel);
			}
			C.Slots[i] = (Sel >= 0 && Sel < GSlotPartsCache[i].Num()) ? Sel : -1;   // clamp to enumerated
		}

		// Promote any legacy-only read into the CombatForge section so the next boot is a clean load.
		if (GConfig != nullptr)
		{
			bool bPrimarySaved = false;
			GConfig->GetBool(PrefSectionPrimary, *SavedKey, bPrimarySaved, FPFPaths::UserPrefsIni());
			if (!bPrimarySaved)
			{
				SaveConfig(S, C);
			}
		}
		return C;
	}

	// Convenience wrappers on the active slot (pawn + existing callers use these).
	void SaveConfig(const FPFCharacterConfig& Config)
	{
		SaveConfig(GetActiveSaveSlot(), Config);
	}

	FPFCharacterConfig LoadConfig()
	{
		return LoadConfig(GetActiveSaveSlot());
	}
}
