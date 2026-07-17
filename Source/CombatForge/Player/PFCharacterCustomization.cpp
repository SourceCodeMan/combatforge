// Copyright (c) 2026 Tom Chapman. All rights reserved.

#include "Player/PFCharacterCustomization.h"

#include "CombatForge.h"
#include "AssetRegistry/AssetData.h"
#include "Engine/ObjectLibrary.h"
#include "Engine/SkeletalMesh.h"
#include "Misc/ConfigCacheIni.h"

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

			Total += GSlotPartsCache[i].Num();
		}
		// Base skin parts that complete the naked SKM_Body base (head + legs).
		GBasePartsCache.Add(FSoftObjectPath(TEXT("/Game/Bandits/Mesh/Body/SKM_Head.SKM_Head")));
		GBasePartsCache.Add(FSoftObjectPath(TEXT("/Game/Bandits/Mesh/Body/SKM_Legs.SKM_Legs")));
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
		Name.ReplaceInline(TEXT("_"), TEXT(" "));
		return Name;
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

	int32 GetActiveSaveSlot()
	{
		int32 Active = 0;
		if (GConfig != nullptr)
		{
			GConfig->GetInt(TEXT("CombatForge"), TEXT("CharActiveSlot"), Active, GGameUserSettingsIni);
		}
		return ClampSaveSlot(Active);
	}

	void SetActiveSaveSlot(int32 SaveSlot)
	{
		if (GConfig == nullptr)
		{
			return;
		}
		// NO Flush here: the dead-scroll class wheel calls this per notch, and a synchronous ini write per
		// notch hitches the respawn screen on slow disks. Every reader goes through the in-memory GConfig
		// cache; durability comes from SaveConfig's flush (menu saves) and the engine's shutdown flush.
		GConfig->SetInt(TEXT("CombatForge"), TEXT("CharActiveSlot"), ClampSaveSlot(SaveSlot), GGameUserSettingsIni);
	}

	void SaveConfig(int32 SaveSlot, const FPFCharacterConfig& Config)
	{
		if (GConfig == nullptr)
		{
			return;
		}
		const int32 S = ClampSaveSlot(SaveSlot);
		GConfig->SetBool(TEXT("CombatForge"), *FString::Printf(TEXT("CharSaved%d"), S), true, GGameUserSettingsIni);
		for (int32 i = 0; i < GSlotCount; ++i)
		{
			const int32 Sel = Config.Slots.IsValidIndex(i) ? Config.Slots[i] : -1;
			const FString Key = FString::Printf(TEXT("CharS%d_%s"), S, GSlots[i].Id);
			GConfig->SetInt(TEXT("CombatForge"), *Key, Sel, GGameUserSettingsIni);
		}
		GConfig->Flush(false, GGameUserSettingsIni);
	}

	FPFCharacterConfig LoadConfig(int32 SaveSlot)
	{
		EnsureBuilt();
		const int32 S = ClampSaveSlot(SaveSlot);
		bool bSaved = false;
		if (GConfig != nullptr)
		{
			GConfig->GetBool(TEXT("CombatForge"), *FString::Printf(TEXT("CharSaved%d"), S), bSaved, GGameUserSettingsIni);
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
			const FString Key = FString::Printf(TEXT("CharS%d_%s"), S, GSlots[i].Id);
			if (GConfig != nullptr)
			{
				GConfig->GetInt(TEXT("CombatForge"), *Key, Sel, GGameUserSettingsIni);
			}
			C.Slots[i] = (Sel >= 0 && Sel < GSlotPartsCache[i].Num()) ? Sel : -1;   // clamp to enumerated
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
