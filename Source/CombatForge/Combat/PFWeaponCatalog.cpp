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
			  nullptr, FVector(-17.92f, -5.30f, 5.37f), FRotator(-5.f, -90.f, 0.f), 0.30f, FVector(10.8f, 6.4f, 0.6f),
			  FVector(24.81f, 6.37f, -7.95f), FRotator(5.f, 0.f, 0.f),   // Tom 2026-07-17: zeroed roll (was tilted "barrel roll left")
			  (1 << 1) | (1 << 2), EPFFireMode::Auto, 2.2f, 0.20f, 8500.f, 1.4f, 3, /*MagSize*/ 25, /*Bps*/ 14.f },   // Tom drag-tuned 2026-07-15
			{ TEXT("AKSU (Wood)"), TEXT("/Game/Bandits/Mesh/Weapon/Rifle_AK/SM_AKSU_Wood.SM_AKSU_Wood"),
			  nullptr, FVector(-17.92f, -5.30f, 5.37f), FRotator(-5.f, -90.f, 0.f), 0.30f, FVector(10.8f, 6.4f, 0.6f),
			  FVector(24.81f, 6.37f, -7.95f), FRotator(5.f, 0.f, 0.f),   // Tom 2026-07-17: zeroed roll (was tilted "barrel roll left")
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
			// Modern Weapons pack (MarketplaceBlockout) — first-pass FP pose (tune with pf.WeaponFP / pf.WeaponADS).
			{ TEXT("Pistol 01"), TEXT("/Game/MarketplaceBlockout/Modern/Weapons/Assets/Pistols/01/SM_Modern_Weapons_Pistol_01.SM_Modern_Weapons_Pistol_01"),
			  nullptr, FVector(-0.34f, 5.71f, -7.72f), FRotator(-2.f, -90.f, 2.f), 0.38f, FVector(8.f, 6.4f, 0.5f),
			  FVector(21.f, -5.5f, 1.3f), FRotator(2.f, 0.f, -2.f),
			  (1 << 0) | (1 << 1), EPFFireMode::Single, 1.9f, 0.12f, 9200.f, 1.2f, 3, 15, 10.f },
			{ TEXT("Pistol 02"), TEXT("/Game/MarketplaceBlockout/Modern/Weapons/Assets/Pistols/02/SM_Modern_Weapons_Pistol_02.SM_Modern_Weapons_Pistol_02"),
			  nullptr, FVector(-0.34f, 5.71f, -7.72f), FRotator(-2.f, -90.f, 2.f), 0.38f, FVector(8.f, 6.4f, 0.5f),
			  FVector(21.f, -5.5f, 1.3f), FRotator(2.f, 0.f, -2.f),
			  (1 << 0) | (1 << 1), EPFFireMode::Single, 1.9f, 0.12f, 9200.f, 1.2f, 3, 15, 10.f },
			{ TEXT("Pistol 03"), TEXT("/Game/MarketplaceBlockout/Modern/Weapons/Assets/Pistols/03/SM_Modern_Weapons_Pistol_03.SM_Modern_Weapons_Pistol_03"),
			  nullptr, FVector(-0.34f, 5.71f, -7.72f), FRotator(-2.f, -90.f, 2.f), 0.38f, FVector(8.f, 6.4f, 0.5f),
			  FVector(21.f, -5.5f, 1.3f), FRotator(2.f, 0.f, -2.f),
			  (1 << 0) | (1 << 1), EPFFireMode::Single, 1.9f, 0.12f, 9200.f, 1.2f, 3, 17, 10.f },
			{ TEXT("Pistol 04"), TEXT("/Game/MarketplaceBlockout/Modern/Weapons/Assets/Pistols/04/SM_Modern_Weapons_Pistol_04.SM_Modern_Weapons_Pistol_04"),
			  nullptr, FVector(-0.34f, 5.71f, -7.72f), FRotator(-2.f, -90.f, 2.f), 0.38f, FVector(8.f, 6.4f, 0.5f),
			  FVector(21.f, -5.5f, 1.3f), FRotator(2.f, 0.f, -2.f),
			  (1 << 0) | (1 << 1), EPFFireMode::Single, 1.9f, 0.12f, 9200.f, 1.2f, 3, 17, 10.f },
			{ TEXT("Revolver 01"), TEXT("/Game/MarketplaceBlockout/Modern/Weapons/Assets/Revolvers/01/SM_Modern_Weapons_Revolver_01.SM_Modern_Weapons_Revolver_01"),
			  nullptr, FVector(-0.34f, 5.71f, -7.72f), FRotator(-2.f, -90.f, 2.f), 0.40f, FVector(9.f, 6.4f, 0.5f),
			  FVector(20.f, -5.5f, 1.3f), FRotator(2.f, 0.f, -2.f),
			  (1 << 0), EPFFireMode::Single, 2.4f, 0.18f, 9500.f, 1.3f, 1, 6, 6.f },
			{ TEXT("Revolver 02"), TEXT("/Game/MarketplaceBlockout/Modern/Weapons/Assets/Revolvers/02/SM_Modern_Weapons_Revolver_02.SM_Modern_Weapons_Revolver_02"),
			  nullptr, FVector(-0.34f, 5.71f, -7.72f), FRotator(-2.f, -90.f, 2.f), 0.40f, FVector(9.f, 6.4f, 0.5f),
			  FVector(20.f, -5.5f, 1.3f), FRotator(2.f, 0.f, -2.f),
			  (1 << 0), EPFFireMode::Single, 2.4f, 0.18f, 9500.f, 1.3f, 1, 6, 6.f },
		};

		// ---- Modern Weapons pack (MarketplaceBlockout) — rifles / SMGs / shotguns / snipers / LMGs ----
		// Paths are the non-HQ static meshes (authored materials). FP poses start from the verified SM_Rifle
		// hold; live-tune with pf.WeaponFP / pf.WeaponADS.
		const FVector  MW_RLoc(1.3f, 6.4f, -8.7f);
		const FRotator MW_RRot(-1.5f, -90.f, 1.5f);
		const FVector  MW_RMuz(35.f, 6.4f, 0.f);
		const FVector  MW_RAds(8.f, -6.2f, 1.5f);
		const FRotator MW_RAdsR(1.8f, 0.f, -0.5f);

		const FPFWeaponDef GModernRifles[] = {
			{ TEXT("Rifle 01"), TEXT("/Game/MarketplaceBlockout/Modern/Weapons/Assets/Rifles/01/SM_Modern_Weapons_Rifle_01.SM_Modern_Weapons_Rifle_01"),
			  nullptr, MW_RLoc, MW_RRot, 0.42f, MW_RMuz, MW_RAds, MW_RAdsR },
			{ TEXT("Rifle 02"), TEXT("/Game/MarketplaceBlockout/Modern/Weapons/Assets/Rifles/02/SM_Modern_Weapons_Rifle_02.SM_Modern_Weapons_Rifle_02"),
			  nullptr, MW_RLoc, MW_RRot, 0.42f, MW_RMuz, MW_RAds, MW_RAdsR },
			{ TEXT("Rifle 03"), TEXT("/Game/MarketplaceBlockout/Modern/Weapons/Assets/Rifles/03/SM_Modern_Weapons_Rifle_03.SM_Modern_Weapons_Rifle_03"),
			  nullptr, MW_RLoc, MW_RRot, 0.42f, MW_RMuz, MW_RAds, MW_RAdsR },
			{ TEXT("Rifle 04"), TEXT("/Game/MarketplaceBlockout/Modern/Weapons/Assets/Rifles/04/SM_Modern_Weapons_Rifle_04.SM_Modern_Weapons_Rifle_04"),
			  nullptr, MW_RLoc, MW_RRot, 0.42f, MW_RMuz, MW_RAds, MW_RAdsR },
			{ TEXT("Rifle 05"), TEXT("/Game/MarketplaceBlockout/Modern/Weapons/Assets/Rifles/05/SM_Modern_Weapons_Rifle_05.SM_Modern_Weapons_Rifle_05"),
			  nullptr, MW_RLoc, MW_RRot, 0.42f, MW_RMuz, MW_RAds, MW_RAdsR },
			{ TEXT("Rifle 06"), TEXT("/Game/MarketplaceBlockout/Modern/Weapons/Assets/Rifles/06/SM_Modern_Weapons_Rifle_06.SM_Modern_Weapons_Rifle_06"),
			  nullptr, MW_RLoc, MW_RRot, 0.42f, MW_RMuz, MW_RAds, MW_RAdsR },
		};
		const FPFWeaponDef GModernSMGs[] = {
			{ TEXT("SMG 01"), TEXT("/Game/MarketplaceBlockout/Modern/Weapons/Assets/SMGs/01/SM_Modern_Weapons_SMG_01.SM_Modern_Weapons_SMG_01"),
			  nullptr, FVector(-10.f, 4.f, -4.f), FRotator(-5.f, -90.f, 0.f), 0.36f, FVector(18.f, 6.f, 0.5f),
			  FVector(20.f, 4.f, -6.f), FRotator(5.f, 0.f, 0.f),
			  (1 << 1) | (1 << 2), EPFFireMode::Auto, 2.0f, 0.18f, 9000.f, 1.4f, 3, 30, 14.f },
			{ TEXT("SMG 02"), TEXT("/Game/MarketplaceBlockout/Modern/Weapons/Assets/SMGs/02/SM_Modern_Weapons_SMG_02.SM_Modern_Weapons_SMG_02"),
			  nullptr, FVector(-10.f, 4.f, -4.f), FRotator(-5.f, -90.f, 0.f), 0.36f, FVector(18.f, 6.f, 0.5f),
			  FVector(20.f, 4.f, -6.f), FRotator(5.f, 0.f, 0.f),
			  (1 << 1) | (1 << 2), EPFFireMode::Auto, 2.0f, 0.18f, 9000.f, 1.4f, 3, 30, 14.f },
			{ TEXT("SMG 03"), TEXT("/Game/MarketplaceBlockout/Modern/Weapons/Assets/SMGs/03/SM_Modern_Weapons_SMG_03.SM_Modern_Weapons_SMG_03"),
			  nullptr, FVector(-10.f, 4.f, -4.f), FRotator(-5.f, -90.f, 0.f), 0.36f, FVector(18.f, 6.f, 0.5f),
			  FVector(20.f, 4.f, -6.f), FRotator(5.f, 0.f, 0.f),
			  (1 << 1) | (1 << 2), EPFFireMode::Auto, 2.0f, 0.18f, 9000.f, 1.4f, 3, 28, 14.f },
			{ TEXT("SMG 04"), TEXT("/Game/MarketplaceBlockout/Modern/Weapons/Assets/SMGs/04/SM_Modern_Weapons_SMG_04.SM_Modern_Weapons_SMG_04"),
			  nullptr, FVector(-10.f, 4.f, -4.f), FRotator(-5.f, -90.f, 0.f), 0.36f, FVector(18.f, 6.f, 0.5f),
			  FVector(20.f, 4.f, -6.f), FRotator(5.f, 0.f, 0.f),
			  (1 << 1) | (1 << 2), EPFFireMode::Auto, 2.0f, 0.18f, 9000.f, 1.4f, 3, 28, 15.f },
			{ TEXT("SMG 05"), TEXT("/Game/MarketplaceBlockout/Modern/Weapons/Assets/SMGs/05/SM_Modern_Weapons_SMG_05.SM_Modern_Weapons_SMG_05"),
			  nullptr, FVector(-10.f, 4.f, -4.f), FRotator(-5.f, -90.f, 0.f), 0.36f, FVector(18.f, 6.f, 0.5f),
			  FVector(20.f, 4.f, -6.f), FRotator(5.f, 0.f, 0.f),
			  (1 << 1) | (1 << 2), EPFFireMode::Auto, 2.0f, 0.18f, 9000.f, 1.4f, 3, 32, 15.f },
			{ TEXT("SMG 06"), TEXT("/Game/MarketplaceBlockout/Modern/Weapons/Assets/SMGs/06/SM_Modern_Weapons_SMG_06.SM_Modern_Weapons_SMG_06"),
			  nullptr, FVector(-10.f, 4.f, -4.f), FRotator(-5.f, -90.f, 0.f), 0.36f, FVector(18.f, 6.f, 0.5f),
			  FVector(20.f, 4.f, -6.f), FRotator(5.f, 0.f, 0.f),
			  (1 << 1) | (1 << 2), EPFFireMode::Auto, 2.0f, 0.18f, 9000.f, 1.4f, 3, 32, 15.f },
		};
		const FPFWeaponDef GShotguns[] = {
			{ TEXT("Shotgun 01"), TEXT("/Game/MarketplaceBlockout/Modern/Weapons/Assets/Shotguns/01/SM_Modern_Weapons_Shotgun_01.SM_Modern_Weapons_Shotgun_01"),
			  nullptr, MW_RLoc, MW_RRot, 0.40f, FVector(38.f, 6.4f, 0.f), MW_RAds, MW_RAdsR,
			  (1 << 0), EPFFireMode::Single, 3.5f, 0.8f, 7000.f, 0.9f, 1, 8, 5.f },
			{ TEXT("Shotgun 02"), TEXT("/Game/MarketplaceBlockout/Modern/Weapons/Assets/Shotguns/02/SM_Modern_Weapons_Shotgun_02.SM_Modern_Weapons_Shotgun_02"),
			  nullptr, MW_RLoc, MW_RRot, 0.40f, FVector(38.f, 6.4f, 0.f), MW_RAds, MW_RAdsR,
			  (1 << 0), EPFFireMode::Single, 3.5f, 0.8f, 7000.f, 0.9f, 1, 8, 5.f },
			{ TEXT("Shotgun 03"), TEXT("/Game/MarketplaceBlockout/Modern/Weapons/Assets/Shotguns/03/SM_Modern_Weapons_Shotgun_03.SM_Modern_Weapons_Shotgun_03"),
			  nullptr, MW_RLoc, MW_RRot, 0.40f, FVector(38.f, 6.4f, 0.f), MW_RAds, MW_RAdsR,
			  (1 << 0), EPFFireMode::Single, 3.2f, 0.7f, 7200.f, 0.9f, 1, 6, 6.f },
			{ TEXT("Shotgun 04"), TEXT("/Game/MarketplaceBlockout/Modern/Weapons/Assets/Shotguns/04/SM_Modern_Weapons_Shotgun_04.SM_Modern_Weapons_Shotgun_04"),
			  nullptr, MW_RLoc, MW_RRot, 0.40f, FVector(38.f, 6.4f, 0.f), MW_RAds, MW_RAdsR,
			  (1 << 0) | (1 << 1), EPFFireMode::Single, 3.0f, 0.6f, 7200.f, 0.9f, 1, 10, 7.f },
		};
		const FPFWeaponDef GSnipers[] = {
			{ TEXT("Sniper 01"), TEXT("/Game/MarketplaceBlockout/Modern/Weapons/Assets/Snipers/01/SM_Modern_Weapons_Sniper_01.SM_Modern_Weapons_Sniper_01"),
			  nullptr, MW_RLoc, MW_RRot, 0.40f, FVector(48.f, 6.4f, 0.f), FVector(12.f, -5.f, 0.5f), MW_RAdsR,
			  (1 << 0), EPFFireMode::Single, 2.5f, 0.02f, 16000.f, 3.0f, 1, 5, 1.5f },
			{ TEXT("Sniper 02"), TEXT("/Game/MarketplaceBlockout/Modern/Weapons/Assets/Snipers/02/SM_Modern_Weapons_Sniper_02.SM_Modern_Weapons_Sniper_02"),
			  nullptr, MW_RLoc, MW_RRot, 0.40f, FVector(48.f, 6.4f, 0.f), FVector(12.f, -5.f, 0.5f), MW_RAdsR,
			  (1 << 0), EPFFireMode::Single, 2.5f, 0.02f, 16000.f, 3.0f, 1, 5, 1.5f },
			{ TEXT("Sniper 03"), TEXT("/Game/MarketplaceBlockout/Modern/Weapons/Assets/Snipers/03/SM_Modern_Weapons_Sniper_03.SM_Modern_Weapons_Sniper_03"),
			  nullptr, MW_RLoc, MW_RRot, 0.40f, FVector(48.f, 6.4f, 0.f), FVector(12.f, -5.f, 0.5f), MW_RAdsR,
			  (1 << 0), EPFFireMode::Single, 2.3f, 0.02f, 16500.f, 3.0f, 1, 5, 1.6f },
			{ TEXT("Sniper 04"), TEXT("/Game/MarketplaceBlockout/Modern/Weapons/Assets/Snipers/04/SM_Modern_Weapons_Sniper_04.SM_Modern_Weapons_Sniper_04"),
			  nullptr, MW_RLoc, MW_RRot, 0.40f, FVector(48.f, 6.4f, 0.f), FVector(12.f, -5.f, 0.5f), MW_RAdsR,
			  (1 << 0), EPFFireMode::Single, 2.2f, 0.02f, 17000.f, 3.2f, 1, 5, 1.4f },
		};
		const FPFWeaponDef GLMGs[] = {
			{ TEXT("LMG 01"), TEXT("/Game/MarketplaceBlockout/Modern/Weapons/Assets/LMGs/01/SM_Modern_Weapons_LMG_01.SM_Modern_Weapons_LMG_01"),
			  nullptr, MW_RLoc, MW_RRot, 0.38f, FVector(42.f, 6.4f, 0.f), MW_RAds, MW_RAdsR,
			  (1 << 2), EPFFireMode::Auto, 1.8f, 0.15f, 11000.f, 2.0f, 3, 75, 11.f },
			{ TEXT("LMG 02"), TEXT("/Game/MarketplaceBlockout/Modern/Weapons/Assets/LMGs/02/SM_Modern_Weapons_LMG_02.SM_Modern_Weapons_LMG_02"),
			  nullptr, MW_RLoc, MW_RRot, 0.38f, FVector(42.f, 6.4f, 0.f), MW_RAds, MW_RAdsR,
			  (1 << 2), EPFFireMode::Auto, 1.8f, 0.15f, 11000.f, 2.0f, 3, 80, 11.f },
			{ TEXT("LMG 03"), TEXT("/Game/MarketplaceBlockout/Modern/Weapons/Assets/LMGs/03/SM_Modern_Weapons_LMG_03.SM_Modern_Weapons_LMG_03"),
			  nullptr, MW_RLoc, MW_RRot, 0.38f, FVector(42.f, 6.4f, 0.f), MW_RAds, MW_RAdsR,
			  (1 << 2), EPFFireMode::Auto, 1.7f, 0.14f, 11000.f, 2.0f, 3, 100, 12.f },
			{ TEXT("Minigun"), TEXT("/Game/MarketplaceBlockout/Modern/Weapons/Assets/Miniguns/01/SM_Modern_Weapons_Minigun_01.SM_Modern_Weapons_Minigun_01"),
			  nullptr, MW_RLoc, MW_RRot, 0.32f, FVector(40.f, 6.4f, 0.f), MW_RAds, MW_RAdsR,
			  (1 << 2), EPFFireMode::Auto, 2.5f, 0.4f, 9000.f, 1.5f, 3, 150, 18.f },
		};

		// Merge legacy rifles with modern pack into one Assault Rifle category for the picker.
		const FPFWeaponDef GAllRifles[] = {
			GRifles[0], GRifles[1], GRifles[2], GRifles[3],
			GModernRifles[0], GModernRifles[1], GModernRifles[2],
			GModernRifles[3], GModernRifles[4], GModernRifles[5],
		};
		const FPFWeaponDef GAllSMGs[] = {
			GSMGs[0], GSMGs[1],
			GModernSMGs[0], GModernSMGs[1], GModernSMGs[2],
			GModernSMGs[3], GModernSMGs[4], GModernSMGs[5],
		};

		struct FCatEntry { const TCHAR* Label; const FPFWeaponDef* Defs; int32 Count; };
		const FCatEntry GCats[] = {
			{ TEXT("Assault Rifle"), GAllRifles, UE_ARRAY_COUNT(GAllRifles) },
			{ TEXT("SMG"),           GAllSMGs,   UE_ARRAY_COUNT(GAllSMGs) },
			{ TEXT("Pistol"),        GPistols,   UE_ARRAY_COUNT(GPistols) },
			{ TEXT("Shotgun"),       GShotguns,  UE_ARRAY_COUNT(GShotguns) },
			{ TEXT("Sniper"),        GSnipers,   UE_ARRAY_COUNT(GSnipers) },
			{ TEXT("LMG"),           GLMGs,      UE_ARRAY_COUNT(GLMGs) },
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
