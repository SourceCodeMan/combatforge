// Copyright (c) 2026 Tom Chapman. All rights reserved.

#include "Combat/PFWeaponCatalog.h"

#include "CombatForge.h"
#include "Player/PFCharacterCustomization.h"   // PFChar::GetActiveSaveSlot — weapon choice is per class slot
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInterface.h"
#include "Misc/ConfigCacheIni.h"
#include "UObject/SoftObjectPath.h"

namespace PFWeapon
{
	namespace
	{
		// NOTE: the FP pose (Loc/Rot/Scale/Muzzle) is per-mesh. SM_Rifle's is verified; every other entry STARTS
		// from that same pose and needs an in-editor tuning pass (use `pf.WeaponFP x y z pitch yaw roll scale`
		// on the equipped weapon, then paste the printed values here). Bandits/Quantum meshes keep their own
		// authored materials (MaterialPath = nullptr); only SM_Rifle force-overrides (it soft-refs a missing Lyra mat).
		// FIRST-PASS poses (from playtest screenshots). SM_Rifle is verified. AK/AKSU render like the rifle so
		// their hold = rifle; only the muzzle (tracer origin) is shortened per barrel length. Pistol is a
		// different class → held closer/higher/centered/larger. Fine-tune any of these with pf.WeaponFP / pf.WeaponADS.
		const FPFWeaponDef GRifles[] = {
			{ TEXT("Rifle (default)"), TEXT("/Game/Weapons/Rifle/Mesh/SM_Rifle.SM_Rifle"),
			  TEXT("/Game/Weapons/Rifle/M_PF_Rifle.M_PF_Rifle"),
			  FVector(-2.56f, -3.19f, 5.81f), FRotator(-1.5f, -90.f, 1.5f), 0.5f, FVector(42.f, 3.5f, -3.5f),
			  FVector(15.49f, 3.42f, -10.59f), FRotator(1.5f, 0.f, -0.5f) },   // Tom drag-tuned 2026-07-15 (pass 2: FP pulled in + raised, ADS lowered)
			{ TEXT("Rifle (Olive)"), TEXT("/Game/QuantumCharacter/Mesh/Rifle/SM_Rifle_Olive.SM_Rifle_Olive"),
			  TEXT("/Game/QuantumCharacter/Materials/M_Rifle_Olive.M_Rifle_Olive"),
			  FVector(-12.64f, 0.12f, -1.01f), FRotator(-1.5f, -90.f, 1.5f), 0.5f, FVector(42.f, 3.5f, -3.5f),
			  FVector(24.44f, 0.20f, -8.75f), FRotator(1.5f, 0.f, -0.5f) },   // Tom drag-tuned 2026-07-17 (Olive is a distinct mesh — its own FP + ADS now)
			// Muzzle + ADS are COMPUTED from mesh geometry (Saved/weapon_geometry.json) via the sight-line formula:
			// AdsLoc = (D,0,0) - FPLoc - R(s*p_sight), R(v)=(v.y,-v.x,v.z), D=20cm, p_sight=(x_center, 0.58*len,
			// boxMax.z - margin). The formula reproduces the verified SM_Rifle ADS. AK hip = Tom's live-tuned pose.
			// (Old note: Bandits AK meshes are authored larger than the Lyra rifle — scale WAY down (0.48 filled the screen)
			// and pull toward center. Still a blind guess; live-tune with pf.WeaponFP.
			// Hand-tuned in-game (pf.WeaponFP).
			{ TEXT("AK (Black)"), TEXT("/Game/Bandits/Mesh/Weapon/Rifle_AK/SM_AK_Black.SM_AK_Black"),
			  nullptr, FVector(1.3f, 6.4f, -8.7f), FRotator(-1.5f, -90.f, 1.5f), 0.48f, FVector(28.7f, 6.4f, 0.f),
			  FVector(5.f, -6.2f, 1.5f), FRotator(1.8f, 0.f, -0.5f) },   // Tom-tuned 2026-07-15
			{ TEXT("AK (Wood)"), TEXT("/Game/Bandits/Mesh/Weapon/Rifle_AK/SM_AK_Wood.SM_AK_Wood"),
			  nullptr, FVector(1.3f, 6.4f, -8.7f), FRotator(-1.5f, -90.f, 1.5f), 0.48f, FVector(28.7f, 6.4f, 0.f),
			  FVector(5.f, -6.2f, 1.5f), FRotator(1.8f, 0.f, -0.5f) },   // Tom-tuned 2026-07-15 (identical to Black)
		};
		// SMG = burst or automatic (NO single); looser accuracy + shorter range than a rifle, but the fastest
		// ROF and a smaller mag (rifle 30/12bps, SMG 25/14bps, pistol 18/10bps - per-weapon identity).
		const FPFWeaponDef GSMGs[] = {
			{ TEXT("AKSU (Black)"), TEXT("/Game/Bandits/Mesh/Weapon/Rifle_AK/SM_AKSU_Black.SM_AKSU_Black"),
			  nullptr, FVector(-17.92f, -5.30f, 5.37f), FRotator(-5.f, -90.f, 5.f), 0.30f, FVector(10.8f, 6.4f, 0.6f),
			  FVector(24.81f, 6.37f, -7.95f), FRotator(5.f, 0.f, -5.f),
			  (1 << 1) | (1 << 2), EPFFireMode::Auto, 2.2f, 0.20f, 8500.f, 1.4f, 3, /*MagSize*/ 25, /*Bps*/ 14.f },   // Tom drag-tuned 2026-07-15
			{ TEXT("AKSU (Wood)"), TEXT("/Game/Bandits/Mesh/Weapon/Rifle_AK/SM_AKSU_Wood.SM_AKSU_Wood"),
			  nullptr, FVector(-17.92f, -5.30f, 5.37f), FRotator(-5.f, -90.f, 5.f), 0.30f, FVector(10.8f, 6.4f, 0.6f),
			  FVector(24.81f, 6.37f, -7.95f), FRotator(5.f, 0.f, -5.f),
			  (1 << 1) | (1 << 2), EPFFireMode::Auto, 2.2f, 0.20f, 8500.f, 1.4f, 3, /*MagSize*/ 25, /*Bps*/ 14.f },   // Tom drag-tuned 2026-07-15 (identical to Black)
		};
		// Pistol = burst or single (NO auto). Sidearm hold; shortest range.
		const FPFWeaponDef GPistols[] = {
			// Pistol pose baked from Tom's in-game drag-tune (pf.WeaponFP/ADS log, 2026-07-15): pulled back +
			// down for the sidearm hold; ADS raised the front sight toward center. Rot/scale/muzzle unchanged.
			{ TEXT("Pistol"), TEXT("/Game/Bandits/Mesh/Weapon/Pistol/SM_Pistol.SM_Pistol"),
			  nullptr, FVector(-0.34f, 5.71f, -7.72f), FRotator(-2.f, -90.f, 2.f), 0.40f, FVector(7.f, 6.4f, 0.5f),
			  FVector(21.1f, -5.54f, 1.29f), FRotator(2.f, 0.f, -2.f),
			  (1 << 0) | (1 << 1), EPFFireMode::Single, 2.0f, 0.12f, 9000.f, 1.2f, 3, /*MagSize*/ 18, /*Bps*/ 10.f },
		};

		struct FCatEntry { const TCHAR* Label; const FPFWeaponDef* Defs; int32 Count; };
		const FCatEntry GCats[] = {
			{ TEXT("Assault Rifle"), GRifles,  UE_ARRAY_COUNT(GRifles) },
			{ TEXT("SMG"),           GSMGs,    UE_ARRAY_COUNT(GSMGs) },
			{ TEXT("Pistol"),        GPistols, UE_ARRAY_COUNT(GPistols) },
		};
		constexpr int32 GCatCount = UE_ARRAY_COUNT(GCats);
	}

	int32 CategoryCount() { return GCatCount; }

	FString CategoryLabel(int32 Category)
	{
		Category = FMath::Clamp(Category, 0, GCatCount - 1);
		return GCats[Category].Label;
	}

	int32 WeaponCount(int32 Category)
	{
		Category = FMath::Clamp(Category, 0, GCatCount - 1);
		return GCats[Category].Count;
	}

	const FPFWeaponDef& Weapon(int32 Category, int32 Index)
	{
		Category = FMath::Clamp(Category, 0, GCatCount - 1);
		Index = FMath::Clamp(Index, 0, GCats[Category].Count - 1);
		return GCats[Category].Defs[Index];
	}

	FString WeaponDisplayName(int32 Category, int32 Index)
	{
		return Weapon(Category, Index).DisplayName;
	}

	UStaticMesh* LoadMesh(const FPFWeaponDef& Def)
	{
		if (Def.MeshPath == nullptr)
		{
			return nullptr;
		}
		return Cast<UStaticMesh>(FSoftObjectPath(Def.MeshPath).TryLoad());
	}

	UMaterialInterface* LoadMaterial(const FPFWeaponDef& Def)
	{
		if (Def.MaterialPath == nullptr)
		{
			return nullptr;   // preserve the mesh's authored materials
		}
		return Cast<UMaterialInterface>(FSoftObjectPath(Def.MaterialPath).TryLoad());
	}

	FPFWeaponConfig DefaultConfig()
	{
		return FPFWeaponConfig{ 0, 0 };   // SM_Rifle
	}

	void SaveConfig(int32 ClassSlot, const FPFWeaponConfig& Config)
	{
		if (GConfig == nullptr)
		{
			return;
		}
		const FString CatKey = FString::Printf(TEXT("WeaponCat_%d"), ClassSlot);
		const FString IdxKey = FString::Printf(TEXT("WeaponIdx_%d"), ClassSlot);
		GConfig->SetInt(TEXT("CombatForge"), *CatKey, FMath::Clamp(Config.Category, 0, GCatCount - 1), GGameUserSettingsIni);
		GConfig->SetInt(TEXT("CombatForge"), *IdxKey, FMath::Max(0, Config.Index), GGameUserSettingsIni);
		GConfig->Flush(false, GGameUserSettingsIni);
	}

	FPFWeaponConfig LoadConfig(int32 ClassSlot)
	{
		FPFWeaponConfig C = DefaultConfig();
		if (GConfig != nullptr)
		{
			const FString CatKey = FString::Printf(TEXT("WeaponCat_%d"), ClassSlot);
			const FString IdxKey = FString::Printf(TEXT("WeaponIdx_%d"), ClassSlot);
			if (!GConfig->GetInt(TEXT("CombatForge"), *CatKey, C.Category, GGameUserSettingsIni))
			{
				// Legacy single-weapon keys (pre class-slots) seed the first read of any slot.
				GConfig->GetInt(TEXT("CombatForge"), TEXT("WeaponCat"), C.Category, GGameUserSettingsIni);
			}
			if (!GConfig->GetInt(TEXT("CombatForge"), *IdxKey, C.Index, GGameUserSettingsIni))
			{
				GConfig->GetInt(TEXT("CombatForge"), TEXT("WeaponIdx"), C.Index, GGameUserSettingsIni);
			}
		}
		C.Category = FMath::Clamp(C.Category, 0, GCatCount - 1);
		C.Index = FMath::Clamp(C.Index, 0, GCats[C.Category].Count - 1);
		return C;
	}

	void SaveConfig(const FPFWeaponConfig& Config)
	{
		SaveConfig(PFChar::GetActiveSaveSlot(), Config);
	}

	FPFWeaponConfig LoadConfig()
	{
		return LoadConfig(PFChar::GetActiveSaveSlot());
	}
}
