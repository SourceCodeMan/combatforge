// Copyright (c) 2026 Tom Chapman. All rights reserved.

#include "Combat/PFWeaponCatalog.h"

#include "PaintForge.h"
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
			  FVector(3.f, 5.5f, -3.5f), FRotator(-1.5f, -90.f, 1.5f), 0.48f, FVector(42.f, 3.5f, -3.5f) },
			{ TEXT("Rifle (Olive)"), TEXT("/Game/QuantumCharacter/Mesh/Rifle/SM_Rifle_Olive.SM_Rifle_Olive"),
			  TEXT("/Game/QuantumCharacter/Materials/M_Rifle_Olive.M_Rifle_Olive"),
			  FVector(3.f, 5.5f, -3.5f), FRotator(-1.5f, -90.f, 1.5f), 0.48f, FVector(42.f, 3.5f, -3.5f) },
			// Bandits AK meshes are authored larger than the Lyra rifle — scale WAY down (0.48 filled the screen)
			// and pull toward center. Still a blind guess; live-tune with pf.WeaponFP.
			// Hand-tuned in-game (pf.WeaponFP).
			{ TEXT("AK (Black)"), TEXT("/Game/Bandits/Mesh/Weapon/Rifle_AK/SM_AK_Black.SM_AK_Black"),
			  nullptr, FVector(-3.f, 6.f, -2.8f), FRotator(-5.f, -90.f, 5.f), 0.31f, FVector(20.f, 3.f, -3.f) },
			{ TEXT("AK (Wood)"), TEXT("/Game/Bandits/Mesh/Weapon/Rifle_AK/SM_AK_Wood.SM_AK_Wood"),
			  nullptr, FVector(-3.f, 6.f, -2.8f), FRotator(-5.f, -90.f, 5.f), 0.31f, FVector(20.f, 3.f, -3.f) },
		};
		// SMG = burst or automatic (NO single); looser accuracy + shorter range than a rifle, but the fastest
		// ROF and a smaller mag (rifle 30/12bps, SMG 25/14bps, pistol 18/10bps - per-weapon identity).
		const FPFWeaponDef GSMGs[] = {
			{ TEXT("AKSU (Black)"), TEXT("/Game/Bandits/Mesh/Weapon/Rifle_AK/SM_AKSU_Black.SM_AKSU_Black"),
			  nullptr, FVector(2.f, 4.f, -4.f), FRotator(-1.5f, -90.f, 1.5f), 0.30f, FVector(20.f, 2.5f, -4.f),
			  FVector(15.f, -5.5f, -1.5f), FRotator(1.5f, 0.f, -1.5f),
			  (1 << 1) | (1 << 2), EPFFireMode::Auto, 2.2f, 0.20f, 8500.f, 1.4f, 3, /*MagSize*/ 25, /*Bps*/ 14.f },
			{ TEXT("AKSU (Wood)"), TEXT("/Game/Bandits/Mesh/Weapon/Rifle_AK/SM_AKSU_Wood.SM_AKSU_Wood"),
			  nullptr, FVector(2.f, 4.f, -4.f), FRotator(-1.5f, -90.f, 1.5f), 0.30f, FVector(20.f, 2.5f, -4.f),
			  FVector(15.f, -5.5f, -1.5f), FRotator(1.5f, 0.f, -1.5f),
			  (1 << 1) | (1 << 2), EPFFireMode::Auto, 2.2f, 0.20f, 8500.f, 1.4f, 3, /*MagSize*/ 25, /*Bps*/ 14.f },
		};
		// Pistol = burst or single (NO auto). Sidearm hold; shortest range.
		const FPFWeaponDef GPistols[] = {
			// Pistol floated way out front — pull it back hard (base viewmodel already sits 26 uu forward). ADS was
			// too high + aimed off the trigger not the sight: drop it and bring the front sight back toward center.
			{ TEXT("Pistol"), TEXT("/Game/Bandits/Mesh/Weapon/Pistol/SM_Pistol.SM_Pistol"),
			  nullptr, FVector(-12.f, 3.f, -4.f), FRotator(-2.f, -90.f, 2.f), 0.40f, FVector(6.f, 2.f, -3.f),
			  FVector(9.f, -3.f, -4.f), FRotator(1.5f, 0.f, -1.5f),
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

	void SaveConfig(const FPFWeaponConfig& Config)
	{
		if (GConfig == nullptr)
		{
			return;
		}
		GConfig->SetInt(TEXT("PaintForge"), TEXT("WeaponCat"), FMath::Clamp(Config.Category, 0, GCatCount - 1), GGameUserSettingsIni);
		GConfig->SetInt(TEXT("PaintForge"), TEXT("WeaponIdx"), FMath::Max(0, Config.Index), GGameUserSettingsIni);
		GConfig->Flush(false, GGameUserSettingsIni);
	}

	FPFWeaponConfig LoadConfig()
	{
		FPFWeaponConfig C = DefaultConfig();
		if (GConfig != nullptr)
		{
			GConfig->GetInt(TEXT("PaintForge"), TEXT("WeaponCat"), C.Category, GGameUserSettingsIni);
			GConfig->GetInt(TEXT("PaintForge"), TEXT("WeaponIdx"), C.Index, GGameUserSettingsIni);
		}
		C.Category = FMath::Clamp(C.Category, 0, GCatCount - 1);
		C.Index = FMath::Clamp(C.Index, 0, GCats[C.Category].Count - 1);
		return C;
	}
}
