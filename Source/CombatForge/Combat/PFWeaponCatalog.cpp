// Copyright (c) 2026 Tom Chapman. All rights reserved.

#include "Combat/PFWeaponCatalog.h"
#include "Core/PFPaths.h"

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
		// Fire-mode bitmasks: S=bit0 B=bit1 A=bit2 (EPFFireMode).
		constexpr uint8 Mode_S   = (1u << 0);
		constexpr uint8 Mode_B   = (1u << 1);
		constexpr uint8 Mode_A   = (1u << 2);
		constexpr uint8 Mode_SB  = Mode_S | Mode_B;
		constexpr uint8 Mode_SA  = Mode_S | Mode_A;
		constexpr uint8 Mode_BA  = Mode_B | Mode_A;
		constexpr uint8 Mode_SBA = Mode_S | Mode_B | Mode_A;

		/** Build a fully-specified catalog row (identity stats + preserved mesh/pose). */
		FPFWeaponDef MakeW(
			const TCHAR* DisplayName,
			const TCHAR* MeshPath,
			const TCHAR* MaterialPath,
			const FVector& FPLoc, const FRotator& FPRot, float FPScale, const FVector& MuzzleFP,
			const FVector& AdsLoc, const FRotator& AdsRot,
			uint8 AllowedFireModes, EPFFireMode DefaultFireMode,
			float SpreadHipDeg, float SpreadADSDeg,
			float MuzzleSpeedUU, float ProjLifetimeSec,
			uint8 ClassBurstCount, uint8 MagSize, float FireRateBps,
			const TCHAR* WeaponId,
			uint8 HitValue,
			uint8 Pellets, float PelletSpreadDeg,
			float BloomPerShotDeg, float BloomCapDeg, uint8 BloomFreeShots, float BloomDecayDegPerSec,
			float ClimbPitchPerShotDeg, float ClimbYawPerShotDeg, float ClimbRecoverDegPerSec,
			float ADSTimeSec, float SprintOutTime, float MoveSpeedMult, float ReloadTime,
			float SpinupSec, float ReburstDelaySec, float MoveSpreadMult, uint8 UnlockRank)
		{
			FPFWeaponDef D;
			D.DisplayName = DisplayName;
			D.MeshPath = MeshPath;
			D.MaterialPath = MaterialPath;
			D.FPLoc = FPLoc;
			D.FPRot = FPRot;
			D.FPScale = FPScale;
			D.MuzzleFP = MuzzleFP;
			D.AdsLoc = AdsLoc;
			D.AdsRot = AdsRot;
			D.AllowedFireModes = AllowedFireModes;
			D.DefaultFireMode = DefaultFireMode;
			D.SpreadHipDeg = SpreadHipDeg;
			D.SpreadADSDeg = SpreadADSDeg;
			D.MuzzleSpeedUU = MuzzleSpeedUU;
			D.ProjLifetimeSec = ProjLifetimeSec;
			D.ClassBurstCount = ClassBurstCount;
			D.MagSize = MagSize;
			D.FireRateBps = FireRateBps;
			D.WeaponId = WeaponId;
			D.HitValue = HitValue;
			D.Pellets = Pellets;
			D.PelletSpreadDeg = PelletSpreadDeg;
			D.BloomPerShotDeg = BloomPerShotDeg;
			D.BloomCapDeg = BloomCapDeg;
			D.BloomFreeShots = BloomFreeShots;
			D.BloomDecayDegPerSec = BloomDecayDegPerSec;
			D.ClimbPitchPerShotDeg = ClimbPitchPerShotDeg;
			D.ClimbYawPerShotDeg = ClimbYawPerShotDeg;
			D.ClimbRecoverDegPerSec = ClimbRecoverDegPerSec;
			D.ADSTimeSec = ADSTimeSec;
			D.SprintOutTime = SprintOutTime;
			D.MoveSpeedMult = MoveSpeedMult;
			D.ReloadTime = ReloadTime;
			D.SpinupSec = SpinupSec;
			D.ReburstDelaySec = ReburstDelaySec;
			D.MoveSpreadMult = MoveSpreadMult;
			D.UnlockRank = UnlockRank;
			// Sniper WeaponIds (snp_*) get CoD whole-screen ADS zoom by default; catalog rows can still
			// override via a direct D.ScopedADSFOV assignment after MakeW if needed.
			if (WeaponId != nullptr && FCString::Strncmp(WeaponId, TEXT("snp_"), 4) == 0)
			{
				D.ScopedADSFOV = 18.f;
			}
			return D;
		}

		/** Skin / reskin: copy base combat stats, override presentation + WeaponId/Rank. */
		FPFWeaponDef SkinOf(
			const FPFWeaponDef& Base,
			const TCHAR* DisplayName,
			const TCHAR* MeshPath,
			const TCHAR* MaterialPath,
			const FVector& FPLoc, const FRotator& FPRot, float FPScale, const FVector& MuzzleFP,
			const FVector& AdsLoc, const FRotator& AdsRot,
			const TCHAR* WeaponId, uint8 UnlockRank)
		{
			FPFWeaponDef D = Base;
			D.DisplayName = DisplayName;
			D.MeshPath = MeshPath;
			D.MaterialPath = MaterialPath;
			D.FPLoc = FPLoc;
			D.FPRot = FPRot;
			D.FPScale = FPScale;
			D.MuzzleFP = MuzzleFP;
			D.AdsLoc = AdsLoc;
			D.AdsRot = AdsRot;
			D.WeaponId = WeaponId;
			D.UnlockRank = UnlockRank;
			return D;
		}

		// NOTE: the FP pose (Loc/Rot/Scale/Muzzle) is per-mesh. SM_Rifle's is verified; every other entry STARTS
		// from that same pose and needs an in-editor tuning pass (use `pf.WeaponFP x y z pitch yaw roll scale`
		// on the equipped weapon, then paste the printed values here). Bandits/Quantum meshes keep their own
		// authored materials (MaterialPath = nullptr); only SM_Rifle force-overrides (it soft-refs a missing Lyra mat).
		// FIRST-PASS poses (from playtest screenshots). SM_Rifle is verified. AK/AKSU render like the rifle so
		// their hold = rifle; only the muzzle (tracer origin) is shortened per barrel length. Pistol is a
		// different class → held closer/higher/centered/larger. Fine-tune any of these with pf.WeaponFP / pf.WeaponADS.

		// ---- Category 0: Assault Rifle (10) — ReloadTime 1.0, MoveSpreadMult 1.35 ----
		// Hip + ADS poses baked from Tom's 2026-07-17 drag session (CombatForge.log WEAPON POSE dumps).
		// Further fine-tunes: pf.WeaponNext → Ctrl+MMB pitch → release → bake again.
		const FPFWeaponDef ArM4 = MakeW(
			TEXT("Rifle (default)"), TEXT("/Game/Weapons/Rifle/Mesh/SM_Rifle.SM_Rifle"),
			TEXT("/Game/Weapons/Rifle/M_PF_Rifle.M_PF_Rifle"),
			FVector(15.35f, 8.38f, -6.8f), FRotator(-2.0f, -90.0f, 0.0f), 0.495f, FVector(42.0f, 3.5f, -3.5f),
			FVector(1.5f, -8.18f, 1.71f), FRotator(0.98f, -0.07f, -0.4f),   // baked from log 2026-07-17
			Mode_SBA, EPFFireMode::Auto,
			1.1f, 0.05f, 12000.f, 2.2f, 3, 30, 12.f,
			TEXT("ar_m4"), 1, 1, 0.f,
			0.12f, 1.8f, 5, 6.f,
			0.30f, 0.12f, 14.f,
			0.25f, 0.18f, 0.95f, 1.0f,
			0.f, 0.f, 1.35f, 1);

		const FPFWeaponDef ArAkBlack = MakeW(
			TEXT("AK (Black)"), TEXT("/Game/Bandits/Mesh/Weapon/Rifle_AK/SM_AK_Black.SM_AK_Black"),
			nullptr, FVector(1.3f, 2.72f, -7.05f), FRotator(-1.5f, -90.0f, 1.5f), 0.48f, FVector(28.7f, 6.4f, 0.0f),
			FVector(5.0f, -2.52f, -0.22f), FRotator(1.8f, 0.0f, -0.5f),   // Tom-tuned 2026-07-15
			Mode_SA, EPFFireMode::Auto,
			1.4f, 0.06f, 11000.f, 2.4f, 3, 30, 6.5f,
			TEXT("ar_ak_black"), 2, 1, 0.f,
			0.22f, 2.2f, 4, 6.f,
			0.45f, 0.15f, 14.f,
			0.32f, 0.18f, 0.95f, 1.0f,
			0.f, 0.f, 1.35f, 6);

		const FPFWeaponDef GAllRifles[] = {
			ArM4,
			SkinOf(ArM4, TEXT("Rifle (Olive)"),
				TEXT("/Game/QuantumCharacter/Mesh/Rifle/SM_Rifle_Olive.SM_Rifle_Olive"),
				TEXT("/Game/QuantumCharacter/Materials/M_Rifle_Olive.M_Rifle_Olive"),
				FVector(5.91f, 14.31f, -14.19f), FRotator(-2.0f, -90.0f, 0.0f), 0.495f, FVector(42.0f, 3.5f, -3.5f),
				FVector(-0.29f, -13.92f, 3.6f), FRotator(2.15f, -0.16f, -0.4f),   // baked 2026-07-17 (from ar_m4)
				TEXT("ar_m4_olive"), 4),
			ArAkBlack,
			SkinOf(ArAkBlack, TEXT("AK (Wood)"),
				TEXT("/Game/Bandits/Mesh/Weapon/Rifle_AK/SM_AK_Wood.SM_AK_Wood"),
				nullptr, FVector(1.3f, 2.72f, -7.05f), FRotator(-1.5f, -90.0f, 1.5f), 0.48f, FVector(28.7f, 6.4f, 0.0f),
				FVector(5.0f, -2.49f, -0.22f), FRotator(1.8f, 0.0f, -0.5f),   // baked 2026-07-17 (same as ar_ak_black)
				TEXT("ar_ak_wood"), 17),
			MakeW(TEXT("Rifle 01"), TEXT("/Game/MarketplaceBlockout/Modern/Weapons/Assets/Rifles/01/SM_Modern_Weapons_Rifle_01.SM_Modern_Weapons_Rifle_01"),
				nullptr, FVector(1.3f, 6.54f, -7.79f), FRotator(-1.5f, -90.0f, 1.5f), 0.42f, FVector(35.0f, 6.4f, 0.0f),
				FVector(8.0f, -6.35f, 1.03f), FRotator(1.8f, 0.0f, -0.5f),
				Mode_SA, EPFFireMode::Auto,
				1.0f, 0.06f, 10500.f, 1.8f, 3, 30, 13.5f,
				TEXT("ar_r01"), 1, 1, 0.f,
				0.15f, 2.0f, 5, 6.f,
				0.28f, 0.10f, 14.f,
				0.20f, 0.18f, 1.00f, 1.0f,
				0.f, 0.f, 1.35f, 9),
			MakeW(TEXT("Rifle 02"), TEXT("/Game/MarketplaceBlockout/Modern/Weapons/Assets/Rifles/02/SM_Modern_Weapons_Rifle_02.SM_Modern_Weapons_Rifle_02"),
				nullptr, FVector(1.3f, 6.4f, -8.7f), FRotator(-1.5f, -90.0f, 1.5f), 0.42f, FVector(35.0f, 6.4f, 0.0f),
				FVector(8.0f, -6.28f, 2.68f), FRotator(1.8f, 0.0f, -0.5f),
				Mode_SBA, EPFFireMode::Auto,
				1.3f, 0.03f, 13500.f, 2.6f, 3, 30, 10.5f,
				TEXT("ar_r02"), 1, 1, 0.f,
				0.10f, 1.5f, 5, 6.f,
				0.26f, 0.10f, 14.f,
				0.32f, 0.18f, 0.95f, 1.0f,
				0.f, 0.f, 1.35f, 14),
			MakeW(TEXT("Rifle 03"), TEXT("/Game/MarketplaceBlockout/Modern/Weapons/Assets/Rifles/03/SM_Modern_Weapons_Rifle_03.SM_Modern_Weapons_Rifle_03"),
				nullptr, FVector(1.3f, 6.4f, -8.7f), FRotator(-1.5f, -90.0f, 1.5f), 0.42f, FVector(35.0f, 6.4f, 0.0f),
				FVector(8.0f, -6.24f, 1.66f), FRotator(1.8f, 0.0f, -0.5f),
				Mode_B, EPFFireMode::Burst,
				1.6f, 0.04f, 13000.f, 2.6f, 3, 21, 14.0f,
				TEXT("ar_r03"), 2, 1, 0.f,
				0.08f, 1.5f, 3, 6.f,
				0.35f, 0.10f, 14.f,
				0.25f, 0.18f, 0.95f, 1.0f,
				0.f, 0.38f, 1.35f, 35),
			MakeW(TEXT("Rifle 04"), TEXT("/Game/MarketplaceBlockout/Modern/Weapons/Assets/Rifles/04/SM_Modern_Weapons_Rifle_04.SM_Modern_Weapons_Rifle_04"),
				nullptr, FVector(1.3f, 6.4f, -8.7f), FRotator(-1.5f, -90.0f, 1.5f), 0.42f, FVector(35.0f, 6.4f, 0.0f),
				FVector(8.0f, -6.29f, 2.73f), FRotator(1.8f, 0.0f, -0.5f),
				Mode_SA, EPFFireMode::Auto,
				1.5f, 0.06f, 12000.f, 2.4f, 3, 24, 6.0f,
				TEXT("ar_r04"), 2, 1, 0.f,
				0.20f, 2.0f, 4, 6.f,
				0.40f, 0.14f, 14.f,
				0.32f, 0.18f, 0.90f, 1.0f,
				0.f, 0.f, 1.35f, 27),
			MakeW(TEXT("Rifle 05"), TEXT("/Game/MarketplaceBlockout/Modern/Weapons/Assets/Rifles/05/SM_Modern_Weapons_Rifle_05.SM_Modern_Weapons_Rifle_05"),
				nullptr, FVector(1.3f, 6.4f, -8.7f), FRotator(-1.5f, -90.0f, 1.96f), 0.42f, FVector(35.0f, 6.4f, 0.0f),
				FVector(8.0f, -6.29f, 2.58f), FRotator(1.8f, 0.0f, -0.5f),
				Mode_SBA, EPFFireMode::Auto,
				1.1f, 0.05f, 12500.f, 2.2f, 3, 30, 11.0f,
				TEXT("ar_r05"), 1, 1, 0.f,
				0.08f, 1.2f, 6, 6.f,
				0.18f, 0.06f, 14.f,
				0.25f, 0.18f, 0.95f, 1.0f,
				0.f, 0.f, 1.35f, 20),
			MakeW(TEXT("Rifle 06"), TEXT("/Game/MarketplaceBlockout/Modern/Weapons/Assets/Rifles/06/SM_Modern_Weapons_Rifle_06.SM_Modern_Weapons_Rifle_06"),
				nullptr, FVector(1.3f, 6.84f, -5.65f), FRotator(-1.5f, -90.0f, 1.5f), 0.42f, FVector(35.0f, 6.4f, 0.0f),
				FVector(8.0f, -6.7f, 0.02f), FRotator(1.8f, 0.0f, -0.5f),
				Mode_SA, EPFFireMode::Auto,
				1.3f, 0.06f, 12000.f, 2.2f, 3, 40, 12.0f,
				TEXT("ar_r06"), 1, 1, 0.f,
				0.14f, 2.0f, 5, 6.f,
				0.30f, 0.12f, 14.f,
				0.32f, 0.18f, 0.90f, 1.0f,
				0.f, 0.f, 1.35f, 45),
		};

		// ---- Category 1: SMG (8) — ReloadTime 0.9, MoveSpreadMult 1.15 (smg_02: 1.05) ----
		const FPFWeaponDef SmgAksuBlack = MakeW(
			TEXT("AKSU (Black)"), TEXT("/Game/Bandits/Mesh/Weapon/Rifle_AK/SM_AKSU_Black.SM_AKSU_Black"),
			nullptr, FVector(-17.92f, -3.49f, 4.26f), FRotator(-5.0f, -89.84f, 3.36f), 0.3f, FVector(10.8f, 6.4f, 0.6f),
			FVector(26.33f, 3.81f, -7.61f), FRotator(5.0f, 0.0f, 0.0f),   // Tom 2026-07-17: zeroed roll
			Mode_BA, EPFFireMode::Auto,
			1.5f, 0.10f, 8500.f, 1.4f, 3, 25, 14.0f,
			TEXT("smg_aksu_black"), 1, 1, 0.f,
			0.10f, 2.2f, 6, 8.f,
			0.22f, 0.08f, 14.f,
			0.20f, 0.18f, 1.00f, 0.9f,
			0.f, 0.f, 1.15f, 1);

		const FPFWeaponDef GAllSMGs[] = {
			SmgAksuBlack,
			SkinOf(SmgAksuBlack, TEXT("AKSU (Wood)"),
				TEXT("/Game/Bandits/Mesh/Weapon/Rifle_AK/SM_AKSU_Wood.SM_AKSU_Wood"),
				nullptr, FVector(-17.92f, -4.13f, 4.75f), FRotator(-3.69f, -90.07f, 5.45f), 0.3f, FVector(10.8f, 6.4f, 0.6f),
				FVector(24.81f, 4.38f, -7.82f), FRotator(5.0f, 0.0f, 0.13f),
				TEXT("smg_aksu_wood"), 11),
			MakeW(TEXT("SMG 01"), TEXT("/Game/MarketplaceBlockout/Modern/Weapons/Assets/SMGs/01/SM_Modern_Weapons_SMG_01.SM_Modern_Weapons_SMG_01"),
				nullptr, FVector(-10.0f, 2.6f, 1.42f), FRotator(-5.0f, -90.0f, -0.0f), 0.36f, FVector(18.0f, 6.0f, 0.5f),
				FVector(20.0f, -2.17f, -6.06f), FRotator(0.44f, 0.05f, 0.0f),
				Mode_BA, EPFFireMode::Auto,
				1.4f, 0.09f, 9000.f, 1.5f, 3, 30, 15.0f,
				TEXT("smg_01"), 1, 1, 0.f,
				0.10f, 2.2f, 6, 8.f,
				0.22f, 0.08f, 14.f,
				0.20f, 0.18f, 1.00f, 0.9f,
				0.f, 0.f, 1.15f, 3),
			MakeW(TEXT("SMG 02"), TEXT("/Game/MarketplaceBlockout/Modern/Weapons/Assets/SMGs/02/SM_Modern_Weapons_SMG_02.SM_Modern_Weapons_SMG_02"),
				nullptr, FVector(-10.0f, 3.74f, -0.46f), FRotator(-5.0f, -90.0f, -0.0f), 0.36f, FVector(18.0f, 6.0f, 0.5f),
				FVector(20.0f, -3.14f, -5.67f), FRotator(-0.12f, 0.25f, 0.0f),
				Mode_A, EPFFireMode::Auto,
				1.6f, 0.12f, 8800.f, 1.3f, 3, 25, 16.0f,
				TEXT("smg_02"), 1, 1, 0.f,
				0.12f, 2.4f, 6, 8.f,
				0.24f, 0.10f, 14.f,
				0.18f, 0.18f, 1.00f, 0.9f,
				0.f, 0.f, 1.05f, 8),
			MakeW(TEXT("SMG 03"), TEXT("/Game/MarketplaceBlockout/Modern/Weapons/Assets/SMGs/03/SM_Modern_Weapons_SMG_03.SM_Modern_Weapons_SMG_03"),
				nullptr, FVector(-10.0f, 2.1f, 2.45f), FRotator(-5.0f, -90.0f, 6.5f), 0.36f, FVector(18.0f, 6.0f, 0.5f),
				FVector(21.07f, -1.7f, -6.1f), FRotator(6.06f, 0.14f, 0.56f),
				Mode_SBA, EPFFireMode::Auto,
				1.2f, 0.07f, 10000.f, 1.8f, 3, 30, 13.0f,
				TEXT("smg_03"), 1, 1, 0.f,
				0.08f, 1.6f, 6, 8.f,
				0.18f, 0.06f, 14.f,
				0.20f, 0.18f, 1.00f, 0.9f,
				0.f, 0.f, 1.15f, 13),
			MakeW(TEXT("SMG 04"), TEXT("/Game/MarketplaceBlockout/Modern/Weapons/Assets/SMGs/04/SM_Modern_Weapons_SMG_04.SM_Modern_Weapons_SMG_04"),
				nullptr, FVector(-12.53f, -0.02f, 2.81f), FRotator(-5.0f, -90.0f, 6.21f), 0.36f, FVector(18.0f, 6.0f, 0.5f),
				FVector(20.0f, 0.45f, -7.24f), FRotator(5.0f, 0.0f, 0.58f),
				Mode_SA, EPFFireMode::Auto,
				1.7f, 0.10f, 9500.f, 1.6f, 3, 20, 5.5f,
				TEXT("smg_04"), 2, 1, 0.f,
				0.25f, 2.4f, 4, 8.f,
				0.40f, 0.14f, 14.f,
				0.25f, 0.18f, 1.00f, 0.9f,
				0.f, 0.f, 1.15f, 19),
			MakeW(TEXT("SMG 05"), TEXT("/Game/MarketplaceBlockout/Modern/Weapons/Assets/SMGs/05/SM_Modern_Weapons_SMG_05.SM_Modern_Weapons_SMG_05"),
				nullptr, FVector(-10.0f, 4.4f, 1.52f), FRotator(-5.0f, -90.0f, -0.0f), 0.36f, FVector(18.0f, 6.0f, 0.5f),
				FVector(20.0f, -4.05f, -5.36f), FRotator(0.04f, 0.04f, 0.0f),
				Mode_A, EPFFireMode::Auto,
				1.8f, 0.12f, 9000.f, 1.4f, 3, 50, 15.0f,
				TEXT("smg_05"), 1, 1, 0.f,
				0.12f, 2.6f, 8, 8.f,
				0.26f, 0.10f, 14.f,
				0.32f, 0.18f, 0.95f, 0.9f,
				0.f, 0.f, 1.15f, 29),
			MakeW(TEXT("SMG 06"), TEXT("/Game/MarketplaceBlockout/Modern/Weapons/Assets/SMGs/06/SM_Modern_Weapons_SMG_06.SM_Modern_Weapons_SMG_06"),
				nullptr, FVector(-10.0f, 4.82f, -0.73f), FRotator(-5.0f, -90.0f, -0.0f), 0.36f, FVector(18.0f, 6.0f, 0.5f),
				FVector(20.0f, -4.27f, -5.53f), FRotator(-0.54f, 0.11f, 0.0f),
				Mode_BA, EPFFireMode::Auto,
				1.5f, 0.10f, 8800.f, 1.2f, 3, 22, 17.0f,
				TEXT("smg_06"), 1, 1, 0.f,
				0.14f, 2.4f, 6, 8.f,
				0.26f, 0.10f, 14.f,
				0.15f, 0.18f, 1.00f, 0.9f,
				0.f, 0.f, 1.15f, 43),
		};

		// ---- Category 2: Pistol (7) — ReloadTime 0.8, MoveSpreadMult 1.2, MoveSpeedMult 1.0 ----
		const FPFWeaponDef Pis01 = MakeW(
			TEXT("Pistol 01"), TEXT("/Game/MarketplaceBlockout/Modern/Weapons/Assets/Pistols/01/SM_Modern_Weapons_Pistol_01.SM_Modern_Weapons_Pistol_01"),
			nullptr, FVector(-0.34f, 8.8f, -7.4f), FRotator(-2.0f, -90.0f, 2.0f), 0.38f, FVector(8.0f, 6.4f, 0.5f),
			FVector(21.0f, -8.85f, 3.71f), FRotator(2.0f, 0.0f, -2.0f),
			Mode_S, EPFFireMode::Single,
			1.2f, 0.12f, 9200.f, 1.2f, 3, 15, 10.0f,
			TEXT("pis_01"), 1, 1, 0.f,
			0.28f, 2.6f, 2, 10.f,
			0.45f, 0.15f, 14.f,
			0.17f, 0.18f, 1.00f, 0.8f,
			0.f, 0.f, 1.2f, 2);

		const FPFWeaponDef GPistols[] = {
			MakeW(TEXT("Pistol"), TEXT("/Game/Bandits/Mesh/Weapon/Pistol/SM_Pistol.SM_Pistol"),
				nullptr, FVector(-0.34f, 14.22f, -9.46f), FRotator(-2.0f, -90.0f, 2.0f), 0.4f, FVector(7.0f, 6.4f, 0.5f),
			FVector(21.1f, -14.11f, 2.78f), FRotator(2.0f, 0.0f, -2.0f),
				Mode_SB, EPFFireMode::Single,
				1.3f, 0.15f, 9000.f, 1.2f, 3, 18, 8.0f,
				TEXT("pis_std"), 1, 1, 0.f,
				0.30f, 2.6f, 2, 10.f,
				0.45f, 0.15f, 14.f,
				0.17f, 0.18f, 1.00f, 0.8f,
				0.f, 0.f, 1.2f, 1),
			Pis01,
			SkinOf(Pis01, TEXT("Pistol 02"),
				TEXT("/Game/MarketplaceBlockout/Modern/Weapons/Assets/Pistols/02/SM_Modern_Weapons_Pistol_02.SM_Modern_Weapons_Pistol_02"),
				nullptr, FVector(-0.34f, 5.71f, -7.72f), FRotator(-2.0f, -90.0f, 2.0f), 0.38f, FVector(8.0f, 6.4f, 0.5f),
			FVector(21.0f, -5.74f, 3.99f), FRotator(2.0f, 0.0f, -2.0f),
				TEXT("pis_02"), 24),
			MakeW(TEXT("Pistol 03"), TEXT("/Game/MarketplaceBlockout/Modern/Weapons/Assets/Pistols/03/SM_Modern_Weapons_Pistol_03.SM_Modern_Weapons_Pistol_03"),
				nullptr, FVector(-0.34f, 5.71f, -7.72f), FRotator(-2.0f, -90.0f, 2.0f), 0.38f, FVector(8.0f, 6.4f, 0.5f),
			FVector(21.0f, -5.78f, 4.36f), FRotator(2.0f, 0.0f, -2.0f),
				Mode_SB, EPFFireMode::Single,
				1.4f, 0.14f, 9200.f, 1.2f, 3, 17, 9.0f,
				TEXT("pis_03"), 1, 1, 0.f,
				0.30f, 2.6f, 2, 10.f,
				0.45f, 0.15f, 14.f,
				0.17f, 0.18f, 1.00f, 0.8f,
				0.f, 0.f, 1.2f, 12),
			MakeW(TEXT("Pistol 04"), TEXT("/Game/MarketplaceBlockout/Modern/Weapons/Assets/Pistols/04/SM_Modern_Weapons_Pistol_04.SM_Modern_Weapons_Pistol_04"),
				nullptr, FVector(-0.34f, 5.71f, -7.72f), FRotator(-2.0f, -90.0f, 2.0f), 0.38f, FVector(8.0f, 6.4f, 0.5f),
			FVector(21.0f, -5.69f, 3.75f), FRotator(2.0f, 0.0f, -2.0f),
				Mode_S, EPFFireMode::Single,
				0.9f, 0.08f, 9200.f, 1.3f, 3, 15, 9.0f,
				TEXT("pis_04"), 1, 1, 0.f,
				0.15f, 2.0f, 3, 10.f,
				0.30f, 0.10f, 14.f,
				0.17f, 0.18f, 1.00f, 0.8f,
				0.f, 0.f, 1.2f, 33),
			MakeW(TEXT("Revolver 01"), TEXT("/Game/MarketplaceBlockout/Modern/Weapons/Assets/Revolvers/01/SM_Modern_Weapons_Revolver_01.SM_Modern_Weapons_Revolver_01"),
				nullptr, FVector(-0.34f, 5.71f, -7.72f), FRotator(-2.0f, -90.0f, 2.0f), 0.4f, FVector(9.0f, 6.4f, 0.5f),
			FVector(20.0f, -5.65f, 2.58f), FRotator(2.0f, 0.0f, -2.0f),
				Mode_S, EPFFireMode::Single,
				1.6f, 0.10f, 9500.f, 1.6f, 1, 6, 4.0f,
				TEXT("rev_01"), 2, 1, 0.f,
				0.30f, 2.6f, 1, 10.f,
				0.80f, 0.20f, 14.f,
				0.17f, 0.18f, 1.00f, 0.8f,
				0.f, 0.f, 1.2f, 16),
			MakeW(TEXT("Revolver 02"), TEXT("/Game/MarketplaceBlockout/Modern/Weapons/Assets/Revolvers/02/SM_Modern_Weapons_Revolver_02.SM_Modern_Weapons_Revolver_02"),
				nullptr, FVector(-0.34f, 5.71f, -7.72f), FRotator(-2.0f, -90.0f, 2.0f), 0.4f, FVector(9.0f, 6.4f, 0.5f),
				FVector(20.0f, -5.66f, 2.71f), FRotator(2.0f, 0.0f, -2.0f),
				Mode_S, EPFFireMode::Single,
				1.9f, 0.14f, 9000.f, 1.2f, 1, 6, 5.0f,
				TEXT("rev_02"), 2, 1, 0.f,
				0.30f, 2.6f, 1, 10.f,
				0.80f, 0.20f, 14.f,
				0.14f, 0.18f, 1.00f, 0.8f,
				0.f, 0.f, 1.2f, 41),
		};

		// ---- Category 3: Shotgun (4) — ReloadTime 1.4 (sg_03: 2.2), pellets@spread ----
		const FPFWeaponDef GShotguns[] = {
			MakeW(TEXT("Shotgun 01"), TEXT("/Game/MarketplaceBlockout/Modern/Weapons/Assets/Shotguns/01/SM_Modern_Weapons_Shotgun_01.SM_Modern_Weapons_Shotgun_01"),
				nullptr, FVector(1.3f, 8.51f, -8.26f), FRotator(-1.5f, -90.0f, 1.5f), 0.4f, FVector(38.0f, 6.4f, 0.0f),
				FVector(8.0f, -8.45f, 4.25f), FRotator(1.8f, 0.0f, -0.5f),
				Mode_S, EPFFireMode::Single,
				0.9f, 0.15f, 7000.f, 0.30f, 1, 6, 1.3f,
				TEXT("sg_01"), 1, 8, 2.2f,
				0.50f, 2.0f, 1, 8.f,
				1.20f, 0.30f, 14.f,
				0.28f, 0.18f, 0.95f, 1.4f,
				0.f, 0.f, 1.35f, 5),
			MakeW(TEXT("Shotgun 02"), TEXT("/Game/MarketplaceBlockout/Modern/Weapons/Assets/Shotguns/02/SM_Modern_Weapons_Shotgun_02.SM_Modern_Weapons_Shotgun_02"),
				nullptr, FVector(1.3f, 7.69f, -8.43f), FRotator(-1.5f, -90.0f, 1.5f), 0.4f, FVector(38.0f, 6.4f, 0.0f),
				FVector(8.0f, -7.63f, 3.24f), FRotator(1.8f, 0.0f, -0.5f),
				Mode_S, EPFFireMode::Single,
				0.9f, 0.15f, 7500.f, 0.35f, 1, 5, 1.1f,
				TEXT("sg_02"), 1, 6, 1.4f,
				0.50f, 2.0f, 1, 8.f,
				1.20f, 0.30f, 14.f,
				0.28f, 0.18f, 0.95f, 1.4f,
				0.f, 0.f, 1.35f, 15),
			MakeW(TEXT("Shotgun 03"), TEXT("/Game/MarketplaceBlockout/Modern/Weapons/Assets/Shotguns/03/SM_Modern_Weapons_Shotgun_03.SM_Modern_Weapons_Shotgun_03"),
				nullptr, FVector(1.3f, 8.2f, -8.75f), FRotator(-1.5f, -90.0f, 1.5f), 0.4f, FVector(38.0f, 6.4f, 0.0f),
				FVector(8.0f, -8.19f, 4.68f), FRotator(1.8f, 0.0f, -0.5f),
				Mode_S, EPFFireMode::Single,
				0.9f, 0.15f, 7000.f, 0.30f, 1, 2, 3.0f,
				TEXT("sg_03"), 1, 8, 2.8f,
				0.50f, 2.0f, 1, 8.f,
				1.20f, 0.30f, 14.f,
				0.22f, 0.18f, 1.00f, 2.2f,
				0.f, 0.f, 1.35f, 22),
			MakeW(TEXT("Shotgun 04"), TEXT("/Game/MarketplaceBlockout/Modern/Weapons/Assets/Shotguns/04/SM_Modern_Weapons_Shotgun_04.SM_Modern_Weapons_Shotgun_04"),
				nullptr, FVector(1.3f, 9.69f, -6.43f), FRotator(-1.5f, -90.0f, 1.5f), 0.4f, FVector(38.0f, 6.4f, 0.0f),
				FVector(8.0f, -9.51f, -0.41f), FRotator(1.8f, 0.0f, -0.5f),
				Mode_SB, EPFFireMode::Single,
				// ClassBurstCount was 1, which made BURST fire exactly one shell — indistinguishable from
				// SINGLE, so the mode looked broken (Tom 2026-07-20). This is the ONLY burst-capable
				// shotgun (the other three are Mode_S) and was the only weapon in the catalog below 2.
				// 3 to match every other burst weapon (Tom's call). That is 18 pellets per pull at close
				// range, which against the 10-hit-out model is lethal fast — intended, but the number to
				// turn down first if the shotgun starts dominating CQB.
				0.9f, 0.15f, 7200.f, 0.30f, 3, 10, 2.8f,
				TEXT("sg_04"), 1, 6, 2.6f,
				0.50f, 2.0f, 1, 8.f,
				1.00f, 0.25f, 14.f,
				0.28f, 0.18f, 0.90f, 1.4f,
				0.f, 0.f, 1.35f, 37),
		};

		// ---- Category 4: Sniper (4) — no bloom chain; ClimbRecover ~20 ----
		const FPFWeaponDef GSnipers[] = {
			MakeW(TEXT("Sniper 01"), TEXT("/Game/MarketplaceBlockout/Modern/Weapons/Assets/Snipers/01/SM_Modern_Weapons_Sniper_01.SM_Modern_Weapons_Sniper_01"),
				nullptr, FVector(1.3f, 8.22f, -7.47f), FRotator(-1.5f, -90.0f, 1.5f), 0.4f, FVector(48.0f, 6.4f, 0.0f),
				FVector(12.0f, -8.06f, 0.9f), FRotator(1.8f, 0.0f, -0.5f),
				Mode_S, EPFFireMode::Single,
				4.5f, 0.02f, 16000.f, 3.0f, 1, 5, 0.90f,
				TEXT("snp_01"), 5, 1, 0.f,
				0.f, 0.5f, 1, 10.f,
				3.0f, 0.5f, 20.f,
				0.45f, 0.18f, 0.90f, 1.0f,
				0.f, 0.f, 1.35f, 7),
			MakeW(TEXT("Sniper 02"), TEXT("/Game/MarketplaceBlockout/Modern/Weapons/Assets/Snipers/02/SM_Modern_Weapons_Sniper_02.SM_Modern_Weapons_Sniper_02"),
				nullptr, FVector(1.3f, 7.94f, -8.09f), FRotator(-1.5f, -90.0f, 1.5f), 0.4f, FVector(48.0f, 6.4f, 0.0f),
				FVector(12.0f, -7.9f, 3.92f), FRotator(1.8f, 0.0f, -0.5f),
				Mode_S, EPFFireMode::Single,
				4.5f, 0.02f, 16000.f, 2.8f, 1, 5, 1.10f,
				TEXT("snp_02"), 5, 1, 0.f,
				0.f, 0.5f, 1, 10.f,
				3.0f, 0.5f, 20.f,
				0.30f, 0.18f, 0.90f, 1.0f,
				0.f, 0.f, 1.35f, 18),
			MakeW(TEXT("Sniper 03"), TEXT("/Game/MarketplaceBlockout/Modern/Weapons/Assets/Snipers/03/SM_Modern_Weapons_Sniper_03.SM_Modern_Weapons_Sniper_03"),
				nullptr, FVector(1.3f, 6.47f, -8.67f), FRotator(-1.5f, -90.0f, 1.5f), 0.4f, FVector(48.0f, 6.4f, 0.0f),
				FVector(12.0f, -6.44f, 4.51f), FRotator(1.8f, 0.0f, -0.5f),
				Mode_S, EPFFireMode::Single,
				4.0f, 0.03f, 15000.f, 2.8f, 1, 10, 3.00f,
				TEXT("snp_03"), 3, 1, 0.f,
				0.f, 0.5f, 1, 10.f,
				1.5f, 0.3f, 20.f,
				0.35f, 0.18f, 0.90f, 1.0f,
				0.f, 0.f, 1.35f, 31),
			MakeW(TEXT("Sniper 04"), TEXT("/Game/MarketplaceBlockout/Modern/Weapons/Assets/Snipers/04/SM_Modern_Weapons_Sniper_04.SM_Modern_Weapons_Sniper_04"),
				nullptr, FVector(1.3f, 6.4f, -8.7f), FRotator(-1.5f, -90.0f, 1.5f), 0.4f, FVector(48.0f, 6.4f, 0.0f),
				FVector(12.0f, -6.3f, 1.91f), FRotator(1.8f, 0.0f, -0.5f),
				Mode_S, EPFFireMode::Single,
				4.5f, 0.02f, 17000.f, 3.2f, 1, 4, 0.65f,
				TEXT("snp_04"), 8, 1, 0.f,
				0.f, 0.5f, 1, 10.f,
				4.0f, 0.6f, 20.f,
				0.55f, 0.18f, 0.85f, 1.0f,
				0.f, 0.f, 1.35f, 47),
		};

		// ---- Category 5: LMG (4) — ReloadTime 2.5 ----
		const FPFWeaponDef GLMGs[] = {
			MakeW(TEXT("LMG 01"), TEXT("/Game/MarketplaceBlockout/Modern/Weapons/Assets/LMGs/01/SM_Modern_Weapons_LMG_01.SM_Modern_Weapons_LMG_01"),
				nullptr, FVector(1.3f, 6.39f, -8.72f), FRotator(-1.5f, -90.0f, 1.5f), 0.38f, FVector(42.0f, 6.4f, 0.0f),
				FVector(8.0f, -6.21f, 3.2f), FRotator(0.95f, 0.31f, -0.03f),
				Mode_A, EPFFireMode::Auto,
				2.2f, 0.15f, 11000.f, 2.0f, 3, 75, 10.0f,
				TEXT("lmg_01"), 1, 1, 0.f,
				0.05f, 1.2f, 10, 4.f,
				0.35f, 0.12f, 14.f,
				0.40f, 0.18f, 0.90f, 2.5f,
				0.f, 0.f, 1.35f, 10),
			MakeW(TEXT("LMG 02"), TEXT("/Game/MarketplaceBlockout/Modern/Weapons/Assets/LMGs/02/SM_Modern_Weapons_LMG_02.SM_Modern_Weapons_LMG_02"),
				nullptr, FVector(1.3f, 6.4f, -8.7f), FRotator(-1.5f, -90.0f, 1.5f), 0.38f, FVector(42.0f, 6.4f, 0.0f),
				FVector(8.0f, -6.22f, 0.8f), FRotator(1.49f, 0.2f, -0.5f),
				Mode_A, EPFFireMode::Auto,
				2.4f, 0.16f, 12000.f, 2.2f, 3, 60, 6.0f,
				TEXT("lmg_02"), 2, 1, 0.f,
				0.05f, 1.2f, 10, 4.f,
				0.50f, 0.16f, 14.f,
				0.45f, 0.18f, 0.88f, 2.5f,
				0.f, 0.f, 1.35f, 25),
			MakeW(TEXT("LMG 03"), TEXT("/Game/MarketplaceBlockout/Modern/Weapons/Assets/LMGs/03/SM_Modern_Weapons_LMG_03.SM_Modern_Weapons_LMG_03"),
				nullptr, FVector(1.3f, 6.4f, -8.7f), FRotator(-1.5f, -90.0f, 1.5f), 0.38f, FVector(42.0f, 6.4f, 0.0f),
				FVector(8.0f, -6.22f, 1.29f), FRotator(1.2f, 0.0f, -0.5f),
				Mode_A, EPFFireMode::Auto,
				2.6f, 0.18f, 11000.f, 2.0f, 3, 100, 12.0f,
				TEXT("lmg_03"), 1, 1, 0.f,
				0.06f, 1.4f, 12, 4.f,
				0.35f, 0.12f, 14.f,
				0.40f, 0.18f, 0.88f, 2.5f,
				0.f, 0.f, 1.35f, 39),
			// Minigun: negative BloomPerShot, BloomCap as FLOOR, spin-up 0.6s, no climb.
			MakeW(TEXT("Minigun"), TEXT("/Game/MarketplaceBlockout/Modern/Weapons/Assets/Miniguns/01/SM_Modern_Weapons_Minigun_01.SM_Modern_Weapons_Minigun_01"),
				// MuzzleFP is in ViewModelRoot space (+X = camera forward) and is consumed ONLY by this row, via
				// bMuzzleFromAuthoredFP (CombatForgeCharacter.cpp). Every other row's MuzzleFP is dead except as a
				// graybox-marker fallback, so those 36 values are evidence of nothing. There is NO "muzzle Z ==
				// FPLoc Z" convention — the verified reference row ar_m4 has FPLoc.Z=-6.8 / MuzzleFP.Z=-3.5, i.e.
				// the muzzle sits ABOVE the mesh origin. The exact relation is
				//   MuzzleFP = FPLoc + FPRot.RotateVector(FPScale * TipMeshLocal)
				// and with FPRot pitch -1.5deg the 38.7uu of forward reach alone drops Z by 38.7*sin(1.5) = 1.01uu.
				// So Z=-8.7 is only correct if the barrel-cluster centre sits ~3.2uu (= 1.01/0.32) above the mesh
				// origin — nobody has measured that. -8.7 is an eyeballed correction for tracers spawning visibly
				// high (Tom: "rounds a bit high"), NOT a derived number. Do not copy it to other rows. To retire the
				// guess: author a "Muzzle" socket on SM_Modern_Weapons_Minigun_01 — GetMuzzleLocation prefers a real
				// socket over this value and will then ignore it entirely.
				nullptr, FVector(1.3f, 6.4f, -8.7f), FRotator(-1.5f, -90.0f, 1.5f), 0.32f, FVector(40.0f, 6.4f, -8.7f),
				FVector(8.0f, -6.39f, 3.86f), FRotator(8.15f, -0.11f, -0.5f),
				Mode_A, EPFFireMode::Auto,
				3.0f, 0.5f, 9000.f, 1.5f, 3, 150, 18.0f,
				TEXT("lmg_minigun"), 1, 1, 0.f,
				-0.02f, 2.2f, 0, 4.f,
				0.f, 0.f, 14.f,
				0.5f, 0.18f, 0.80f, 2.5f,
				0.6f, 0.f, 1.35f, 50),
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

		/** True if WeaponId at (Cat,Idx) equals Slug (case-insensitive). */
		bool IdMatches(int32 Cat, int32 Idx, FStringView Slug)
		{
			const TCHAR* Id = GCats[Cat].Defs[Idx].WeaponId;
			return Id != nullptr && Slug.Equals(Id, ESearchCase::IgnoreCase);
		}
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

	bool ComputeAutoPose(const UStaticMesh* Mesh, int32 Category, FPFWeaponAutoPose& Out)
	{
		if (Mesh == nullptr)
		{
			return false;
		}

		// Categories: 0 AR, 1 SMG, 2 Pistol, 3 Shotgun, 4 Sniper, 5 LMG
		const bool bPistol  = (Category == 2);
		const bool bCompact = (Category == 1 || Category == 2);   // SMG / pistol — shorter hold
		const bool bLong    = (Category == 4 || Category == 5);   // sniper / LMG

		const FBoxSphereBounds B = Mesh->GetBounds();
		const FVector O = B.Origin;
		const FVector E = B.BoxExtent;
		const float LenX = FMath::Max(E.X * 2.f, 1.f);
		const float LenY = FMath::Max(E.Y * 2.f, 1.f);
		const float LenZ = FMath::Max(E.Z * 2.f, 1.f);
		const bool bAlongY = (LenY >= LenX);   // SM_Rifle / many Bandits use +Y barrel
		const float BarrelLen = bAlongY ? LenY : LenX;
		const float BarrelHalf = BarrelLen * 0.5f;

		// Fit longest horizontal axis into a target viewmodel length (uu).
		float TargetLen = 48.f;
		if (bPistol)       { TargetLen = 26.f; }
		else if (bCompact) { TargetLen = 36.f; }
		else if (bLong)    { TargetLen = 52.f; }
		Out.FPScale = FMath::Clamp(TargetLen / BarrelLen, 0.12f, 0.85f);

		// Point barrel along ViewModelRoot +X (camera forward).
		// Mesh +Y barrel → yaw -90; mesh +X barrel → yaw 0. Slight pitch tucks the muzzle down a hair.
		Out.FPRot = bAlongY
			? FRotator(-2.f, -90.f, 0.f)
			: FRotator(-2.f, 0.f, 0.f);

		const FVector AlongMesh = bAlongY ? FVector(0.f, 1.f, 0.f) : FVector(1.f, 0.f, 0.f);
		// Grip: rear of the gun, below the bore (stock / pistol grip region).
		const FVector GripMesh = O - AlongMesh * (BarrelHalf * 0.55f) + FVector(0.f, 0.f, -E.Z * 0.55f);
		// Iron-sight / optic line: top of receiver, a bit forward of center.
		const FVector SightMesh = O + AlongMesh * (BarrelHalf * 0.15f) + FVector(0.f, 0.f, E.Z * 0.82f);

		const FTransform MeshToParent(Out.FPRot, FVector::ZeroVector, FVector(Out.FPScale));
		const FVector GripParent = MeshToParent.TransformPosition(GripMesh);
		const FVector SightParentNoLoc = MeshToParent.TransformPosition(SightMesh);

		// Desired grip in ViewModelRoot space (classic FPS hold: forward-right-down of eye).
		FVector HoldGrip = bPistol
			? FVector(10.f, 7.f, -11.f)
			: (bCompact ? FVector(6.f, 8.f, -12.f) : FVector(4.f, 9.f, -13.f));
		Out.FPLoc = HoldGrip - GripParent;

		// Sight after hip pose (mesh + FPLoc).
		const FVector SightAtHip = SightParentNoLoc + Out.FPLoc;

		// ADS: move ViewModelRoot so the estimated sight sits on the camera forward axis at SightDist.
		Out.AdsRot = FRotator(1.2f, 0.f, -0.4f);
		const float SightDist = bPistol ? 14.f : 18.f;
		const FVector DesiredSightCam(SightDist, 0.f, 0.f);
		Out.AdsLoc = DesiredSightCam - Out.AdsRot.RotateVector(SightAtHip);

		// Keep ADS root in a sane band (avoid flipping the gun behind the camera).
		Out.AdsLoc.X = FMath::Clamp(Out.AdsLoc.X, -5.f, 40.f);
		Out.AdsLoc.Y = FMath::Clamp(Out.AdsLoc.Y, -25.f, 25.f);
		Out.AdsLoc.Z = FMath::Clamp(Out.AdsLoc.Z, -25.f, 20.f);

		return true;
	}

	FPFWeaponConfig DefaultConfig()
	{
		return FPFWeaponConfig{ 0, 0 };   // SM_Rifle / ar_m4
	}

	FPFWeaponConfig FindById(FStringView WeaponId)
	{
		if (WeaponId.IsEmpty())
		{
			return DefaultConfig();
		}
		for (int32 Cat = 0; Cat < GCatCount; ++Cat)
		{
			for (int32 Idx = 0; Idx < GCats[Cat].Count; ++Idx)
			{
				if (IdMatches(Cat, Idx, WeaponId))
				{
					return FPFWeaponConfig{ Cat, Idx };
				}
			}
		}
		return DefaultConfig();
	}

	FString IdOf(int32 Category, int32 Index)
	{
		const FPFWeaponDef& Def = Weapon(Category, Index);
		return Def.WeaponId != nullptr ? FString(Def.WeaponId) : FString();
	}

	uint8 UnlockRankOf(int32 Category, int32 Index)
	{
		return Weapon(Category, Index).UnlockRank;
	}

	void SaveConfig(int32 ClassSlot, const FPFWeaponConfig& Config)
	{
		if (GConfig == nullptr)
		{
			return;
		}
		const int32 Cat = FMath::Clamp(Config.Category, 0, GCatCount - 1);
		const int32 Idx = FMath::Clamp(Config.Index, 0, GCats[Cat].Count - 1);
		const FString CatKey = FString::Printf(TEXT("WeaponCat_%d"), ClassSlot);
		const FString IdxKey = FString::Printf(TEXT("WeaponIdx_%d"), ClassSlot);
		const FString IdKey  = FString::Printf(TEXT("WeaponId_%d"), ClassSlot);
		GConfig->SetInt(TEXT("CombatForge"), *CatKey, Cat, FPFPaths::UserPrefsIni());
		GConfig->SetInt(TEXT("CombatForge"), *IdxKey, Idx, FPFPaths::UserPrefsIni());
		if (const TCHAR* Wid = GCats[Cat].Defs[Idx].WeaponId)
		{
			GConfig->SetString(TEXT("CombatForge"), *IdKey, Wid, FPFPaths::UserPrefsIni());
		}
		GConfig->Flush(false, FPFPaths::UserPrefsIni());
	}

	FPFWeaponConfig LoadConfig(int32 ClassSlot)
	{
		FPFWeaponConfig C = DefaultConfig();
		if (GConfig != nullptr)
		{
			// Prefer stable slug (survives category reorders); fall back to int cat/idx for one version.
			FString SavedId;
			const FString IdKey = FString::Printf(TEXT("WeaponId_%d"), ClassSlot);
			if (GConfig->GetString(TEXT("CombatForge"), *IdKey, SavedId, FPFPaths::UserPrefsIni()) && !SavedId.IsEmpty())
			{
				const FPFWeaponConfig ById = FindById(SavedId);
				if (IdOf(ById.Category, ById.Index).Equals(SavedId, ESearchCase::IgnoreCase))
				{
					return ById;
				}
			}

			const FString CatKey = FString::Printf(TEXT("WeaponCat_%d"), ClassSlot);
			const FString IdxKey = FString::Printf(TEXT("WeaponIdx_%d"), ClassSlot);
			if (!GConfig->GetInt(TEXT("CombatForge"), *CatKey, C.Category, FPFPaths::UserPrefsIni()))
			{
				// Legacy single-weapon keys (pre class-slots) seed the first read of any slot.
				GConfig->GetInt(TEXT("CombatForge"), TEXT("WeaponCat"), C.Category, FPFPaths::UserPrefsIni());
			}
			if (!GConfig->GetInt(TEXT("CombatForge"), *IdxKey, C.Index, FPFPaths::UserPrefsIni()))
			{
				GConfig->GetInt(TEXT("CombatForge"), TEXT("WeaponIdx"), C.Index, FPFPaths::UserPrefsIni());
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

	void SaveSecondaryConfig(int32 ClassSlot, const FPFWeaponConfig& Config)
	{
		if (GConfig == nullptr)
		{
			return;
		}
		const int32 Cat = FMath::Clamp(Config.Category, 0, GCatCount - 1);
		const int32 Idx = FMath::Clamp(Config.Index, 0, GCats[Cat].Count - 1);
		const FString CatKey = FString::Printf(TEXT("Weapon2Cat_%d"), ClassSlot);
		const FString IdxKey = FString::Printf(TEXT("Weapon2Idx_%d"), ClassSlot);
		const FString IdKey  = FString::Printf(TEXT("Weapon2Id_%d"), ClassSlot);
		GConfig->SetInt(TEXT("CombatForge"), *CatKey, Cat, FPFPaths::UserPrefsIni());
		GConfig->SetInt(TEXT("CombatForge"), *IdxKey, Idx, FPFPaths::UserPrefsIni());
		if (const TCHAR* Wid = GCats[Cat].Defs[Idx].WeaponId)
		{
			GConfig->SetString(TEXT("CombatForge"), *IdKey, Wid, FPFPaths::UserPrefsIni());
		}
		GConfig->Flush(false, FPFPaths::UserPrefsIni());
	}

	FPFWeaponConfig LoadSecondaryConfig(int32 ClassSlot)
	{
		// Default second weapon: first pistol (still freely changeable via prefs / kit).
		FPFWeaponConfig C{ 2, 0 };
		if (GConfig != nullptr)
		{
			FString SavedId;
			const FString IdKey = FString::Printf(TEXT("Weapon2Id_%d"), ClassSlot);
			if (GConfig->GetString(TEXT("CombatForge"), *IdKey, SavedId, FPFPaths::UserPrefsIni()) && !SavedId.IsEmpty())
			{
				const FPFWeaponConfig ById = FindById(SavedId);
				if (IdOf(ById.Category, ById.Index).Equals(SavedId, ESearchCase::IgnoreCase))
				{
					return ById;
				}
			}

			const FString CatKey = FString::Printf(TEXT("Weapon2Cat_%d"), ClassSlot);
			const FString IdxKey = FString::Printf(TEXT("Weapon2Idx_%d"), ClassSlot);
			GConfig->GetInt(TEXT("CombatForge"), *CatKey, C.Category, FPFPaths::UserPrefsIni());
			GConfig->GetInt(TEXT("CombatForge"), *IdxKey, C.Index, FPFPaths::UserPrefsIni());
		}
		C.Category = FMath::Clamp(C.Category, 0, GCatCount - 1);
		C.Index = FMath::Clamp(C.Index, 0, GCats[C.Category].Count - 1);
		return C;
	}

	void SaveSecondaryConfig(const FPFWeaponConfig& Config)
	{
		SaveSecondaryConfig(PFChar::GetActiveSaveSlot(), Config);
	}

	FPFWeaponConfig LoadSecondaryConfig()
	{
		return LoadSecondaryConfig(PFChar::GetActiveSaveSlot());
	}
}

// ---------------------------------------------------------------------------
// pf.WeaponDump — real measured geometry for every catalog weapon.
// ---------------------------------------------------------------------------
// Added 2026-07-20 during the third-person weapon reset. Third-person guns looked roughly half size and
// nobody could say why, because the FP scale is per-weapon and tuned (0.32-0.50) while the TP scale was a
// single 0.85 applied to all 36 meshes — one of those is wrong for almost every gun, and arguing about
// which from screenshots is how the last six attempts went. This prints the actual bounds so the answer is
// measured instead of asserted. A real rifle is ~70-90cm, i.e. 70-90 uu.
static void PFWeaponDumpCmd(const TArray<FString>& /*Args*/, UWorld* /*World*/)
{
	// Bounds ORIGIN is the offset from the mesh PIVOT to the mesh centre. It is the number that decides
	// whether attaching a gun to a hand bone can ever look right: if the pivot is not at the grip, snapping
	// the pivot to hand_r throws the visible gun that far away from the hand, no matter what small relative
	// offset you tune. A gun 30cm above the hand on a 3cm offset (Tom, 2026-07-20) is this, not the offset.
	UE_LOG(CombatForgeLog, Warning,
		TEXT("WPNDUMP  %-22s %-34s %9s %26s %9s"),
		TEXT("id"), TEXT("mesh"), TEXT("natLen"), TEXT("pivot->centre (X,Y,Z)"), TEXT("offsetLen"));
	for (int32 Cat = 0; Cat < PFWeapon::CategoryCount(); ++Cat)
	{
		for (int32 Idx = 0; Idx < PFWeapon::WeaponCount(Cat); ++Idx)
		{
			const FPFWeaponDef& D = PFWeapon::Weapon(Cat, Idx);
			UStaticMesh* M = PFWeapon::LoadMesh(D);
			if (M == nullptr)
			{
				UE_LOG(CombatForgeLog, Warning, TEXT("WPNDUMP  %-22s MESH FAILED TO LOAD"),
					D.WeaponId ? D.WeaponId : TEXT("?"));
				continue;
			}
			const FBoxSphereBounds B = M->GetBounds();
			// Longest axis = barrel length for every gun in this catalog.
			const float NatLen = FMath::Max3(B.BoxExtent.X, B.BoxExtent.Y, B.BoxExtent.Z) * 2.f;
			UE_LOG(CombatForgeLog, Warning,
				TEXT("WPNDUMP  %-22s %-34s %9.1f   (%7.1f,%7.1f,%7.1f) %9.1f"),
				D.WeaponId ? D.WeaponId : TEXT("?"), *M->GetName(),
				NatLen, B.Origin.X, B.Origin.Y, B.Origin.Z, B.Origin.Size());
		}
	}
}
static FAutoConsoleCommandWithWorldAndArgs GPFWeaponDumpCmd(
	TEXT("pf.WeaponDump"),
	TEXT("Print measured bounds for every catalog weapon: natural length and length at FP/TP scale."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&PFWeaponDumpCmd));
