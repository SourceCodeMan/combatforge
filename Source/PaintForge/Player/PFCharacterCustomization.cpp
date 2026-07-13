// Copyright (c) 2026 Tom Chapman. All rights reserved.

#include "Player/PFCharacterCustomization.h"

#include "PaintForge.h"
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
		{ TEXT("Cloth"),    TEXT("Cloth"),    TEXT("/Game/Bandits/Mesh/Cloth")          },
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
			Total += GSlotPartsCache[i].Num();
		}
		// Base skin parts that complete the naked SKM_Body base (head + legs).
		GBasePartsCache.Add(FSoftObjectPath(TEXT("/Game/Bandits/Mesh/Body/SKM_Head.SKM_Head")));
		GBasePartsCache.Add(FSoftObjectPath(TEXT("/Game/Bandits/Mesh/Body/SKM_Legs.SKM_Legs")));
		UE_LOG(PaintForgeLog, Log, TEXT("PFChar: enumerated %d parts across %d slots."), Total, GSlotCount);
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

	void SaveConfig(const FPFCharacterConfig& Config)
	{
		if (GConfig == nullptr)
		{
			return;
		}
		GConfig->SetBool(TEXT("PaintForge"), TEXT("CharConfigSaved"), true, GGameUserSettingsIni);
		for (int32 i = 0; i < GSlotCount; ++i)
		{
			const int32 Sel = Config.Slots.IsValidIndex(i) ? Config.Slots[i] : -1;
			const FString Key = FString::Printf(TEXT("CharSlot_%s"), GSlots[i].Id);
			GConfig->SetInt(TEXT("PaintForge"), *Key, Sel, GGameUserSettingsIni);
		}
		GConfig->Flush(false, GGameUserSettingsIni);
	}

	FPFCharacterConfig LoadConfig()
	{
		EnsureBuilt();
		bool bSaved = false;
		if (GConfig != nullptr)
		{
			GConfig->GetBool(TEXT("PaintForge"), TEXT("CharConfigSaved"), bSaved, GGameUserSettingsIni);
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
			const FString Key = FString::Printf(TEXT("CharSlot_%s"), GSlots[i].Id);
			if (GConfig != nullptr)
			{
				GConfig->GetInt(TEXT("PaintForge"), *Key, Sel, GGameUserSettingsIni);
			}
			C.Slots[i] = (Sel >= 0 && Sel < GSlotPartsCache[i].Num()) ? Sel : -1;   // clamp to enumerated
		}
		return C;
	}
}
