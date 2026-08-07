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
			FVector(26.22f, 0.97f, -15.37f), FRotator(-5.00f, -90.00f, 5.00f), 1.000f, FVector(42.00f, 3.50f, -3.50f),
			FVector(9.44f, 0.29f, 3.14f), FRotator(5.17f, -0.21f, 2.64f),
			Mode_SBA, EPFFireMode::Auto,
			1.1f, 0.05f, 12000.f, 2.2f, 3, 30, 12.f,
			TEXT("ar_m4"), 1, 1, 0.f,
			0.12f, 1.8f, 5, 6.f,
			0.30f, 0.12f, 14.f,
			0.25f, 0.18f, 0.95f, 1.0f,
			0.f, 0.f, 1.35f, 1);

		const FPFWeaponDef ArAkBlack = MakeW(
			TEXT("AK (Black)"), TEXT("/Game/Bandits/Mesh/Weapon/Rifle_AK/SM_AK_Black.SM_AK_Black"),
			nullptr, FVector(8.71f, -0.98f, -17.68f), FRotator(-5.00f, -90.00f, 5.00f), 1.000f, FVector(28.70f, 6.40f, 0.00f),
			FVector(11.55f, 2.41f, 1.50f), FRotator(5.66f, -0.12f, 2.64f),
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
				FVector(7.72f, 1.16f, -20.73f), FRotator(-5.00f, -90.00f, 5.00f), 1.000f, FVector(42.00f, 3.50f, -3.50f),
				FVector(13.92f, 0.75f, -1.20f), FRotator(7.21f, -0.25f, 2.64f),
				TEXT("ar_m4_olive"), 4),
			ArAkBlack,
			SkinOf(ArAkBlack, TEXT("AK (Wood)"),
				TEXT("/Game/Bandits/Mesh/Weapon/Rifle_AK/SM_AK_Wood.SM_AK_Wood"),
				nullptr, FVector(9.18f, 0.75f, -20.26f), FRotator(-5.00f, -90.00f, 5.00f), 1.000f, FVector(28.70f, 6.40f, 0.00f),
				FVector(11.01f, 0.90f, 3.89f), FRotator(5.17f, -0.21f, 2.64f),
				TEXT("ar_ak_wood"), 17),
			MakeW(TEXT("Rifle 01"), TEXT("/Game/MarketplaceBlockout/Modern/Weapons/Assets/Rifles/01/SM_Modern_Weapons_Rifle_01.SM_Modern_Weapons_Rifle_01"),
				nullptr, FVector(11.56f, 0.53f, -16.98f), FRotator(-5.00f, -90.00f, 5.00f), 1.000f, FVector(35.00f, 6.40f, 0.00f),
				FVector(13.84f, 0.98f, 0.13f), FRotator(5.17f, -0.21f, 2.64f),
				Mode_SA, EPFFireMode::Auto,
				1.0f, 0.06f, 10500.f, 1.8f, 3, 30, 13.5f,
				TEXT("ar_r01"), 1, 1, 0.f,
				0.15f, 2.0f, 5, 6.f,
				0.28f, 0.10f, 14.f,
				0.20f, 0.18f, 1.00f, 1.0f,
				0.f, 0.f, 1.35f, 9),
			MakeW(TEXT("Rifle 02"), TEXT("/Game/MarketplaceBlockout/Modern/Weapons/Assets/Rifles/02/SM_Modern_Weapons_Rifle_02.SM_Modern_Weapons_Rifle_02"),
				nullptr, FVector(10.28f, 0.55f, -15.49f), FRotator(-5.00f, -90.00f, 5.00f), 1.000f, FVector(35.00f, 6.40f, 0.00f),
				FVector(7.75f, 0.74f, 0.35f), FRotator(5.17f, -0.21f, 2.64f),
				Mode_SBA, EPFFireMode::Auto,
				1.3f, 0.03f, 13500.f, 2.6f, 3, 30, 10.5f,
				TEXT("ar_r02"), 1, 1, 0.f,
				0.10f, 1.5f, 5, 6.f,
				0.26f, 0.10f, 14.f,
				0.32f, 0.18f, 0.95f, 1.0f,
				0.f, 0.f, 1.35f, 14),
			MakeW(TEXT("Rifle 03"), TEXT("/Game/MarketplaceBlockout/Modern/Weapons/Assets/Rifles/03/SM_Modern_Weapons_Rifle_03.SM_Modern_Weapons_Rifle_03"),
				nullptr, FVector(9.73f, 2.54f, -18.45f), FRotator(-5.00f, -90.00f, 5.00f), 1.000f, FVector(35.00f, 6.40f, 0.00f),
				FVector(17.13f, -0.99f, 1.40f), FRotator(4.00f, -0.17f, 2.64f),
				Mode_B, EPFFireMode::Burst,
				1.6f, 0.04f, 13000.f, 2.6f, 3, 21, 14.0f,
				TEXT("ar_r03"), 2, 1, 0.f,
				0.08f, 1.5f, 3, 6.f,
				0.35f, 0.10f, 14.f,
				0.25f, 0.18f, 0.95f, 1.0f,
				0.f, 0.38f, 1.35f, 35),
			MakeW(TEXT("Rifle 04"), TEXT("/Game/MarketplaceBlockout/Modern/Weapons/Assets/Rifles/04/SM_Modern_Weapons_Rifle_04.SM_Modern_Weapons_Rifle_04"),
				nullptr, FVector(12.88f, 0.77f, -11.86f), FRotator(-5.00f, -90.00f, 5.00f), 1.000f, FVector(35.00f, 6.40f, 0.00f),
				FVector(12.48f, 0.34f, -3.17f), FRotator(4.52f, -0.17f, 2.64f),
				Mode_SA, EPFFireMode::Auto,
				1.5f, 0.06f, 12000.f, 2.4f, 3, 24, 6.0f,
				TEXT("ar_r04"), 2, 1, 0.f,
				0.20f, 2.0f, 4, 6.f,
				0.40f, 0.14f, 14.f,
				0.32f, 0.18f, 0.90f, 1.0f,
				0.f, 0.f, 1.35f, 27),
			MakeW(TEXT("Rifle 05"), TEXT("/Game/MarketplaceBlockout/Modern/Weapons/Assets/Rifles/05/SM_Modern_Weapons_Rifle_05.SM_Modern_Weapons_Rifle_05"),
				nullptr, FVector(14.01f, 0.73f, -11.68f), FRotator(-5.00f, -90.00f, 5.00f), 1.000f, FVector(35.00f, 6.40f, 0.00f),
				FVector(21.24f, 0.48f, -4.33f), FRotator(5.17f, -0.21f, 2.64f),
				Mode_SBA, EPFFireMode::Auto,
				1.1f, 0.05f, 12500.f, 2.2f, 3, 30, 11.0f,
				TEXT("ar_r05"), 1, 1, 0.f,
				0.08f, 1.2f, 6, 6.f,
				0.18f, 0.06f, 14.f,
				0.25f, 0.18f, 0.95f, 1.0f,
				0.f, 0.f, 1.35f, 20),
			MakeW(TEXT("Rifle 06"), TEXT("/Game/MarketplaceBlockout/Modern/Weapons/Assets/Rifles/06/SM_Modern_Weapons_Rifle_06.SM_Modern_Weapons_Rifle_06"),
				nullptr, FVector(13.66f, 2.02f, -10.06f), FRotator(-6.80f, -89.54f, 1.31f), 1.000f, FVector(35.00f, 6.40f, 0.00f),
				FVector(18.37f, -0.65f, -5.08f), FRotator(5.17f, -0.21f, 2.64f),
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
			nullptr, FVector(11.41f, 0.59f, -17.48f), FRotator(-5.00f, -90.00f, 5.00f), 1.000f, FVector(10.80f, 6.40f, 0.60f),
			FVector(17.04f, 0.98f, 0.57f), FRotator(5.17f, -0.21f, 2.64f),
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
				nullptr, FVector(9.77f, 1.25f, -15.75f), FRotator(-5.00f, -90.00f, 5.00f), 1.000f, FVector(10.80f, 6.40f, 0.60f),
				FVector(17.04f, 0.20f, -1.31f), FRotator(5.17f, -0.21f, 2.64f),
				TEXT("smg_aksu_wood"), 11),
			MakeW(TEXT("SMG 01"), TEXT("/Game/MarketplaceBlockout/Modern/Weapons/Assets/SMGs/01/SM_Modern_Weapons_SMG_01.SM_Modern_Weapons_SMG_01"),
				nullptr, FVector(9.82f, 2.54f, -12.31f), FRotator(-5.00f, -90.00f, 5.00f), 1.000f, FVector(18.00f, 6.00f, 0.50f),
				FVector(28.57f, -1.38f, -1.29f), FRotator(4.39f, -0.23f, 2.64f),
				Mode_BA, EPFFireMode::Auto,
				1.4f, 0.09f, 9000.f, 1.5f, 3, 30, 15.0f,
				TEXT("smg_01"), 1, 1, 0.f,
				0.10f, 2.2f, 6, 8.f,
				0.22f, 0.08f, 14.f,
				0.20f, 0.18f, 1.00f, 0.9f,
				0.f, 0.f, 1.15f, 3),
			MakeW(TEXT("SMG 02"), TEXT("/Game/MarketplaceBlockout/Modern/Weapons/Assets/SMGs/02/SM_Modern_Weapons_SMG_02.SM_Modern_Weapons_SMG_02"),
				nullptr, FVector(12.57f, 1.20f, -13.49f), FRotator(-5.00f, -90.00f, 5.00f), 1.000f, FVector(18.00f, 6.00f, 0.50f),
				FVector(21.90f, 0.20f, -4.76f), FRotator(5.01f, -0.25f, 2.64f),
				Mode_A, EPFFireMode::Auto,
				1.6f, 0.12f, 8800.f, 1.3f, 3, 25, 16.0f,
				TEXT("smg_02"), 1, 1, 0.f,
				0.12f, 2.4f, 6, 8.f,
				0.24f, 0.10f, 14.f,
				0.18f, 0.18f, 1.00f, 0.9f,
				0.f, 0.f, 1.05f, 8),
			MakeW(TEXT("SMG 03"), TEXT("/Game/MarketplaceBlockout/Modern/Weapons/Assets/SMGs/03/SM_Modern_Weapons_SMG_03.SM_Modern_Weapons_SMG_03"),
				nullptr, FVector(14.56f, 1.69f, -8.33f), FRotator(-5.00f, -90.00f, 5.00f), 1.000f, FVector(18.00f, 6.00f, 0.50f),
				FVector(29.51f, -0.69f, -6.34f), FRotator(5.17f, -0.21f, 2.64f),
				Mode_SBA, EPFFireMode::Auto,
				1.2f, 0.07f, 10000.f, 1.8f, 3, 30, 13.0f,
				TEXT("smg_03"), 1, 1, 0.f,
				0.08f, 1.6f, 6, 8.f,
				0.18f, 0.06f, 14.f,
				0.20f, 0.18f, 1.00f, 0.9f,
				0.f, 0.f, 1.15f, 13),
			MakeW(TEXT("SMG 04"), TEXT("/Game/MarketplaceBlockout/Modern/Weapons/Assets/SMGs/04/SM_Modern_Weapons_SMG_04.SM_Modern_Weapons_SMG_04"),
				nullptr, FVector(26.51f, 0.00f, -11.39f), FRotator(-5.00f, -90.00f, 5.00f), 1.000f, FVector(18.00f, 6.00f, 0.50f),
				FVector(26.12f, 1.26f, -6.19f), FRotator(5.17f, -0.21f, 2.64f),
				Mode_SA, EPFFireMode::Auto,
				1.7f, 0.10f, 9500.f, 1.6f, 3, 20, 5.5f,
				TEXT("smg_04"), 2, 1, 0.f,
				0.25f, 2.4f, 4, 8.f,
				0.40f, 0.14f, 14.f,
				0.25f, 0.18f, 1.00f, 0.9f,
				0.f, 0.f, 1.15f, 19),
			MakeW(TEXT("SMG 05"), TEXT("/Game/MarketplaceBlockout/Modern/Weapons/Assets/SMGs/05/SM_Modern_Weapons_SMG_05.SM_Modern_Weapons_SMG_05"),
				nullptr, FVector(16.07f, -0.83f, -5.96f), FRotator(-5.16f, -90.56f, 3.43f), 1.000f, FVector(18.00f, 6.00f, 0.50f),
				FVector(23.83f, 1.57f, -5.82f), FRotator(3.17f, 0.35f, 1.97f),
				Mode_A, EPFFireMode::Auto,
				1.8f, 0.12f, 9000.f, 1.4f, 3, 50, 15.0f,
				TEXT("smg_05"), 1, 1, 0.f,
				0.12f, 2.6f, 8, 8.f,
				0.26f, 0.10f, 14.f,
				0.32f, 0.18f, 0.95f, 0.9f,
				0.f, 0.f, 1.15f, 29),
			MakeW(TEXT("SMG 06"), TEXT("/Game/MarketplaceBlockout/Modern/Weapons/Assets/SMGs/06/SM_Modern_Weapons_SMG_06.SM_Modern_Weapons_SMG_06"),
				nullptr, FVector(14.55f, 0.18f, -12.96f), FRotator(-5.00f, -90.00f, -3.79f), 1.000f, FVector(18.00f, 6.00f, 0.50f),
				FVector(23.00f, 1.25f, -3.04f), FRotator(-4.46f, 0.19f, 0.78f),
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
			nullptr, FVector(15.69f, -0.40f, -8.25f), FRotator(-5.00f, -90.00f, 5.00f), 1.000f, FVector(8.00f, 6.40f, 0.50f),
			FVector(35.96f, 1.20f, -2.08f), FRotator(5.17f, -0.21f, 2.64f),
			Mode_S, EPFFireMode::Single,
			1.2f, 0.12f, 9200.f, 1.2f, 3, 15, 10.0f,
			TEXT("pis_01"), 1, 1, 0.f,
			0.28f, 2.6f, 2, 10.f,
			0.45f, 0.15f, 14.f,
			0.17f, 0.18f, 1.00f, 0.8f,
			0.f, 0.f, 1.2f, 2);

		const FPFWeaponDef GPistols[] = {
			MakeW(TEXT("Pistol"), TEXT("/Game/Bandits/Mesh/Weapon/Pistol/SM_Pistol.SM_Pistol"),
				nullptr, FVector(15.17f, 1.32f, -18.53f), FRotator(-5.00f, -90.00f, 5.00f), 1.000f, FVector(7.00f, 6.40f, 0.50f),
			FVector(35.48f, 0.18f, 0.95f), FRotator(5.17f, -0.21f, 2.64f),
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
				nullptr, FVector(16.62f, 1.44f, -7.58f), FRotator(-5.00f, -90.00f, 5.00f), 1.000f, FVector(8.00f, 6.40f, 0.50f),
			FVector(35.84f, -0.63f, -3.45f), FRotator(5.17f, -0.21f, 2.64f),
				TEXT("pis_02"), 24),
			MakeW(TEXT("Pistol 03"), TEXT("/Game/MarketplaceBlockout/Modern/Weapons/Assets/Pistols/03/SM_Modern_Weapons_Pistol_03.SM_Modern_Weapons_Pistol_03"),
				nullptr, FVector(18.16f, 1.29f, -4.45f), FRotator(-7.94f, -89.18f, -12.83f), 1.000f, FVector(8.00f, 6.40f, 0.50f),
			FVector(37.12f, -0.53f, -0.55f), FRotator(-11.18f, 0.28f, 4.46f),
				Mode_SB, EPFFireMode::Single,
				1.4f, 0.14f, 9200.f, 1.2f, 3, 17, 9.0f,
				TEXT("pis_03"), 1, 1, 0.f,
				0.30f, 2.6f, 2, 10.f,
				0.45f, 0.15f, 14.f,
				0.17f, 0.18f, 1.00f, 0.8f,
				0.f, 0.f, 1.2f, 12),
			MakeW(TEXT("Pistol 04"), TEXT("/Game/MarketplaceBlockout/Modern/Weapons/Assets/Pistols/04/SM_Modern_Weapons_Pistol_04.SM_Modern_Weapons_Pistol_04"),
				nullptr, FVector(16.34f, -0.32f, -8.53f), FRotator(-5.00f, -90.00f, 5.00f), 1.000f, FVector(8.00f, 6.40f, 0.50f),
			FVector(34.85f, 1.19f, -2.89f), FRotator(5.17f, -0.21f, 2.64f),
				Mode_S, EPFFireMode::Single,
				0.9f, 0.08f, 9200.f, 1.3f, 3, 15, 9.0f,
				TEXT("pis_04"), 1, 1, 0.f,
				0.15f, 2.0f, 3, 10.f,
				0.30f, 0.10f, 14.f,
				0.17f, 0.18f, 1.00f, 0.8f,
				0.f, 0.f, 1.2f, 33),
			MakeW(TEXT("Revolver 01"), TEXT("/Game/MarketplaceBlockout/Modern/Weapons/Assets/Revolvers/01/SM_Modern_Weapons_Revolver_01.SM_Modern_Weapons_Revolver_01"),
				nullptr, FVector(18.67f, 0.89f, -5.22f), FRotator(-5.00f, -90.00f, 5.00f), 1.000f, FVector(9.00f, 6.40f, 0.50f),
			FVector(31.66f, 0.01f, -8.80f), FRotator(5.17f, -0.21f, 2.64f),
				Mode_S, EPFFireMode::Single,
				1.6f, 0.10f, 9500.f, 1.6f, 1, 6, 4.0f,
				TEXT("rev_01"), 2, 1, 0.f,
				0.30f, 2.6f, 1, 10.f,
				0.80f, 0.20f, 14.f,
				0.17f, 0.18f, 1.00f, 0.8f,
				0.f, 0.f, 1.2f, 16),
			MakeW(TEXT("Revolver 02"), TEXT("/Game/MarketplaceBlockout/Modern/Weapons/Assets/Revolvers/02/SM_Modern_Weapons_Revolver_02.SM_Modern_Weapons_Revolver_02"),
				nullptr, FVector(19.41f, 1.52f, -6.02f), FRotator(-5.00f, -90.00f, 5.00f), 1.000f, FVector(9.00f, 6.40f, 0.50f),
				FVector(34.38f, -0.66f, -7.90f), FRotator(5.17f, -0.21f, 2.64f),
				Mode_S, EPFFireMode::Single,
				1.9f, 0.14f, 9000.f, 1.2f, 1, 6, 5.0f,
				TEXT("rev_02"), 2, 1, 0.f,
				0.30f, 2.6f, 1, 10.f,
				0.80f, 0.20f, 14.f,
				0.14f, 0.18f, 1.00f, 0.8f,
				0.f, 0.f, 1.2f, 41),
		};

		// ---- Category 3: Shotgun (4) — ReloadTime 1.4 (sg_03: 2.2), pellets@spread ----
		// Recoil re-tuned 2026-08-06 (smoke alpha.17): hip fire was a laser (SpreadHip 0.9° + climb
		// only 1.2° vs sniper's 3–4° scoped punch). Every shell now kicks hard (~5–6° climb, slow
		// recover) so you must reset between shots; wider hip cone + bloom free-shots=0 (no soft
		// first shell). ADS still tames visual kick via ADSRecoilMult, but aim climb stays full.
		const FPFWeaponDef GShotguns[] = {
			MakeW(TEXT("Shotgun 01"), TEXT("/Game/MarketplaceBlockout/Modern/Weapons/Assets/Shotguns/01/SM_Modern_Weapons_Shotgun_01.SM_Modern_Weapons_Shotgun_01"),
				nullptr, FVector(13.10f, -0.87f, -13.61f), FRotator(-5.00f, -90.00f, 5.00f), 1.000f, FVector(38.00f, 6.40f, 0.00f),
				FVector(9.37f, 2.01f, 2.42f), FRotator(5.17f, -0.21f, 2.64f),
				Mode_S, EPFFireMode::Single,
				2.8f, 0.35f, 7000.f, 0.30f, 1, 6, 1.3f,
				TEXT("sg_01"), 1, 8, 2.2f,
				1.5f, 4.0f, 0, 6.f,
				5.5f, 1.1f, 9.f,
				0.28f, 0.18f, 0.95f, 1.4f,
				0.f, 0.f, 1.35f, 5),
			MakeW(TEXT("Shotgun 02"), TEXT("/Game/MarketplaceBlockout/Modern/Weapons/Assets/Shotguns/02/SM_Modern_Weapons_Shotgun_02.SM_Modern_Weapons_Shotgun_02"),
				nullptr, FVector(13.88f, -0.86f, -9.78f), FRotator(-5.00f, -90.00f, 5.00f), 1.000f, FVector(38.00f, 6.40f, 0.00f),
				FVector(10.91f, 1.90f, -4.11f), FRotator(5.17f, -0.21f, 2.64f),
				Mode_S, EPFFireMode::Single,
				2.6f, 0.30f, 7500.f, 0.35f, 1, 5, 1.1f,
				TEXT("sg_02"), 1, 6, 1.4f,
				1.4f, 3.8f, 0, 6.f,
				5.2f, 1.0f, 9.f,
				0.28f, 0.18f, 0.95f, 1.4f,
				0.f, 0.f, 1.35f, 15),
			MakeW(TEXT("Shotgun 03"), TEXT("/Game/MarketplaceBlockout/Modern/Weapons/Assets/Shotguns/03/SM_Modern_Weapons_Shotgun_03.SM_Modern_Weapons_Shotgun_03"),
				nullptr, FVector(7.14f, -0.07f, -14.28f), FRotator(-5.00f, -90.00f, 5.00f), 1.000f, FVector(38.00f, 6.40f, 0.00f),
				FVector(12.19f, 1.21f, 4.07f), FRotator(4.90f, -0.25f, 2.64f),
				Mode_S, EPFFireMode::Single,
				// Double-barrel cadence (3 bps) — harder climb so the second shell doesn't land free.
				3.0f, 0.40f, 7000.f, 0.30f, 1, 2, 3.0f,
				TEXT("sg_03"), 1, 8, 2.8f,
				1.6f, 4.2f, 0, 5.f,
				6.0f, 1.3f, 8.f,
				0.22f, 0.18f, 1.00f, 2.2f,
				0.f, 0.f, 1.35f, 22),
			MakeW(TEXT("Shotgun 04"), TEXT("/Game/MarketplaceBlockout/Modern/Weapons/Assets/Shotguns/04/SM_Modern_Weapons_Shotgun_04.SM_Modern_Weapons_Shotgun_04"),
				nullptr, FVector(13.33f, 2.84f, -12.63f), FRotator(-5.00f, -90.00f, 5.00f), 1.000f, FVector(38.00f, 6.40f, 0.00f),
				FVector(19.86f, -1.58f, -5.35f), FRotator(5.17f, -0.21f, 2.64f),
				Mode_SBA, EPFFireMode::Auto,
				// Playtest 2026-08-06: full-auto shotgun (was Mode_SB / Single with ClassBurstCount 3).
				// FireRateBps stays 2.8 shells/sec — full-auto at that cadence with the existing ~5° climb
				// is the intended feel; tune only after play. ClassBurstCount 3 is kept for Burst mode
				// on the selector (18 pellets per burst pull at close range) — turn pellets down first
				// if this dominates CQB.
				2.8f, 0.35f, 7200.f, 0.30f, 3, 10, 2.8f,
				TEXT("sg_04"), 1, 6, 2.6f,
				1.5f, 4.0f, 0, 6.f,
				5.0f, 1.0f, 9.f,
				0.28f, 0.18f, 0.90f, 1.4f,
				0.f, 0.f, 1.35f, 37),
		};

		// ---- Category 4: Sniper (4) — no bloom chain; ClimbRecover ~20 ----
		const FPFWeaponDef GSnipers[] = {
			MakeW(TEXT("Sniper 01"), TEXT("/Game/MarketplaceBlockout/Modern/Weapons/Assets/Snipers/01/SM_Modern_Weapons_Sniper_01.SM_Modern_Weapons_Sniper_01"),
				nullptr, FVector(12.96f, 1.55f, -13.06f), FRotator(-5.00f, -90.00f, 5.00f), 1.000f, FVector(48.00f, 6.40f, 0.00f),
				FVector(2.63f, -0.15f, -4.69f), FRotator(5.43f, -0.24f, 2.64f),
				Mode_S, EPFFireMode::Single,
				4.5f, 0.02f, 16000.f, 3.0f, 1, 5, 0.90f,
				TEXT("snp_01"), 5, 1, 0.f,
				0.f, 0.5f, 1, 10.f,
				3.0f, 0.5f, 20.f,
				0.45f, 0.18f, 0.90f, 1.0f,
				0.f, 0.f, 1.35f, 7),
			MakeW(TEXT("Sniper 02"), TEXT("/Game/MarketplaceBlockout/Modern/Weapons/Assets/Snipers/02/SM_Modern_Weapons_Sniper_02.SM_Modern_Weapons_Sniper_02"),
				nullptr, FVector(13.14f, 0.54f, -10.96f), FRotator(-5.00f, -90.00f, 5.00f), 1.000f, FVector(48.00f, 6.40f, 0.00f),
				FVector(-1.02f, 0.44f, -0.38f), FRotator(5.17f, -0.21f, 2.64f),
				Mode_S, EPFFireMode::Single,
				4.5f, 0.02f, 16000.f, 2.8f, 1, 5, 1.10f,
				TEXT("snp_02"), 5, 1, 0.f,
				0.f, 0.5f, 1, 10.f,
				3.0f, 0.5f, 20.f,
				0.30f, 0.18f, 0.90f, 1.0f,
				0.f, 0.f, 1.35f, 18),
			MakeW(TEXT("Sniper 03"), TEXT("/Game/MarketplaceBlockout/Modern/Weapons/Assets/Snipers/03/SM_Modern_Weapons_Sniper_03.SM_Modern_Weapons_Sniper_03"),
				nullptr, FVector(13.85f, -5.30f, -5.69f), FRotator(-5.00f, -90.00f, 5.00f), 1.000f, FVector(48.00f, 6.40f, 0.00f),
				FVector(9.21f, 6.02f, -6.34f), FRotator(5.17f, -0.21f, 2.64f),
				Mode_S, EPFFireMode::Single,
				4.0f, 0.03f, 15000.f, 2.8f, 1, 10, 3.00f,
				TEXT("snp_03"), 3, 1, 0.f,
				0.f, 0.5f, 1, 10.f,
				1.5f, 0.3f, 20.f,
				0.35f, 0.18f, 0.90f, 1.0f,
				0.f, 0.f, 1.35f, 31),
			MakeW(TEXT("Sniper 04"), TEXT("/Game/MarketplaceBlockout/Modern/Weapons/Assets/Snipers/04/SM_Modern_Weapons_Sniper_04.SM_Modern_Weapons_Sniper_04"),
				nullptr, FVector(11.38f, 3.34f, -15.37f), FRotator(-5.00f, -90.00f, 5.00f), 1.000f, FVector(48.00f, 6.40f, 0.00f),
				FVector(15.96f, -1.90f, -2.40f), FRotator(5.17f, -0.21f, 2.64f),
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
			// FP hip + ADS hand-tuned by Tom 2026-07-23 (pf.WeaponFP / pf.WeaponADS live session, baked from
			// the log). FP scale 1.0 = his call (life-size viewmodel read). MuzzleFP stays authored — lmg_01
			// resolves its tracer tip via the bounds auto-tip, so that field is inert here.
			MakeW(TEXT("LMG 01"), TEXT("/Game/MarketplaceBlockout/Modern/Weapons/Assets/LMGs/01/SM_Modern_Weapons_LMG_01.SM_Modern_Weapons_LMG_01"),
				nullptr, FVector(10.80f, -1.24f, -14.29f), FRotator(-5.00f, -90.00f, 5.00f), 1.000f, FVector(42.00f, 6.40f, 0.00f),
				FVector(8.19f, 2.68f, -1.13f), FRotator(5.17f, -0.21f, 2.64f),
				Mode_A, EPFFireMode::Auto,
				2.2f, 0.15f, 11000.f, 2.0f, 3, 200, 10.0f,
				TEXT("lmg_01"), 1, 1, 0.f,
				0.05f, 1.2f, 10, 4.f,
				0.35f, 0.12f, 14.f,
				0.40f, 0.18f, 0.90f, 2.5f,
				0.f, 0.f, 1.35f, 10),
			MakeW(TEXT("LMG 02"), TEXT("/Game/MarketplaceBlockout/Modern/Weapons/Assets/LMGs/02/SM_Modern_Weapons_LMG_02.SM_Modern_Weapons_LMG_02"),
				nullptr, FVector(12.17f, 1.76f, -18.13f), FRotator(-5.00f, -90.00f, 5.00f), 1.000f, FVector(42.00f, 6.40f, 0.00f),
				FVector(5.80f, 0.07f, -3.65f), FRotator(5.17f, -0.21f, 2.64f),
				Mode_A, EPFFireMode::Auto,
				2.4f, 0.16f, 12000.f, 2.2f, 3, 180, 6.0f,
				TEXT("lmg_02"), 2, 1, 0.f,
				0.05f, 1.2f, 10, 4.f,
				0.50f, 0.16f, 14.f,
				0.45f, 0.18f, 0.88f, 2.5f,
				0.f, 0.f, 1.35f, 25),
			MakeW(TEXT("LMG 03"), TEXT("/Game/MarketplaceBlockout/Modern/Weapons/Assets/LMGs/03/SM_Modern_Weapons_LMG_03.SM_Modern_Weapons_LMG_03"),
				nullptr, FVector(10.37f, 0.79f, -20.26f), FRotator(-5.00f, -90.00f, 5.00f), 1.000f, FVector(42.00f, 6.40f, 0.00f),
				FVector(7.27f, 0.99f, 0.22f), FRotator(4.47f, -0.21f, 2.64f),
				Mode_A, EPFFireMode::Auto,
				2.6f, 0.18f, 11000.f, 2.0f, 3, 200, 12.0f,
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
				nullptr, FVector(7.80f, 2.86f, -20.64f), FRotator(-5.00f, -90.00f, 5.00f), 1.000f, FVector(40.00f, 6.40f, -8.70f),
				FVector(10.65f, 0.42f, 2.98f), FRotator(5.17f, -0.21f, 2.64f),
				Mode_A, EPFFireMode::Auto,
				3.0f, 0.5f, 9000.f, 1.5f, 3, 250, 18.0f,
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

	// ---- Auto third-person grip ----------------------------------------------------------------------
	// Global anchor: one live correction shared by every AUTO-posed weapon. pf.WeaponTPCalibrate solves it
	// from a single hand-tuned gun (pf.WeaponTP one weapon → calibrate → all untuned rows inherit the fix)
	// and prints a paste-ready line for these three constants. Identity by default so the struct-default
	// base pose is the out-of-the-box behavior.
	// BAKED 2026-07-23: Tom's pf.WeaponTPCalibrate solve, dialed on lmg_01 in PIE (grip seated in the
	// right palm at true 1.0 scale; rotation solve came out identity — the base orientation was right).
	// ScaleMult 1.176 = his 1.0 real-size choice over the 0.85 base, applied to every auto gun.
	static FVector  GAutoTPAnchorLoc = FVector(-0.58f, -6.84f, 3.29f);
	static FRotator GAutoTPAnchorRot = FRotator(0.00f, 0.00f, 0.00f);
	static float    GAutoTPAnchorScaleMult = 1.176f;

	void SetAutoTPAnchor(const FVector& Loc, const FRotator& Rot, float ScaleMult)
	{
		GAutoTPAnchorLoc = Loc;
		GAutoTPAnchorRot = Rot;
		GAutoTPAnchorScaleMult = (ScaleMult > 0.f) ? ScaleMult : 1.f;
	}

	void GetAutoTPAnchor(FVector& OutLoc, FRotator& OutRot, float& OutScaleMult)
	{
		OutLoc = GAutoTPAnchorLoc;
		OutRot = GAutoTPAnchorRot;
		OutScaleMult = GAutoTPAnchorScaleMult;
	}

	bool HasTunedTP(const FPFWeaponDef& Def)
	{
		// A pf.WeaponTP paste always includes D.TPScale (> 0), and any loc/rot different from the struct
		// defaults means a human placed this row. Tuned = the whole TP row is manual.
		static const FPFWeaponDef Defaults;
		return Def.TPScale > 0.f
			|| !Def.TPLoc.Equals(Defaults.TPLoc, KINDA_SMALL_NUMBER)
			|| !Def.TPRot.Equals(Defaults.TPRot, KINDA_SMALL_NUMBER);
	}

	bool ComputeAutoTPGrip(const UStaticMesh* Mesh, float AppliedScale,
		const FVector& BaseLoc, const FRotator& BaseRot,
		float GripAlongFrac, float GripDownFrac, bool bApplyAnchor, FPFWeaponAutoTP& Out)
	{
		if (Mesh == nullptr || AppliedScale <= 0.f)
		{
			return false;
		}
		const FBoxSphereBounds B = Mesh->GetBounds();
		const FVector O = B.Origin;    // pivot -> centre: the per-mesh scatter this exists to cancel
		const FVector E = B.BoxExtent;
		if (E.IsNearlyZero())
		{
			return false;
		}
		const float LenX = FMath::Max(E.X * 2.f, 1.f);
		const float LenY = FMath::Max(E.Y * 2.f, 1.f);
		const bool bAlongY = (LenY >= LenX);   // same axis test as ComputeAutoPose / the muzzle auto-tip
		const float BarrelHalf = (bAlongY ? LenY : LenX) * 0.5f;
		Out.bBarrelAlongY = bAlongY;

		// BaseRot is authored for a +Y-barrel mesh. A +X-barrel mesh gets a +90° yaw PRE-rotation in mesh
		// space so its barrel takes the same hand-space line (FRotator(0,90,0) maps +X onto +Y — the same
		// convention ComputeAutoPose uses when it hands +Y-barrel meshes yaw -90 and +X-barrel meshes yaw 0).
		const FQuat BaseQ(BaseRot);
		const FQuat MeshQ = bAlongY ? BaseQ : BaseQ * FQuat(FRotator(0.f, 90.f, 0.f));

		// Anchor point in mesh space: rear of the gun along the barrel, below the bore — the grip region,
		// the exact recipe the FP auto-pose uses. For a pivot-at-grip mesh this is ~zero, so BaseLoc/BaseRot
		// are reproduced unchanged; every other pivot is compensated instead of thrown to the wrong distance.
		const FVector Along = bAlongY ? FVector(0.f, 1.f, 0.f) : FVector(1.f, 0.f, 0.f);
		const FVector GripMesh = O - Along * (BarrelHalf * GripAlongFrac) + FVector(0.f, 0.f, -E.Z * GripDownFrac);
		Out.GripLocalMesh = GripMesh;
		Out.BarrelAxisLocal = Along;

		FVector  Loc = BaseLoc - MeshQ.RotateVector(GripMesh) * AppliedScale;
		FQuat    RotQ = MeshQ;

		if (bApplyAnchor)
		{
			const FQuat AnchorQ(GAutoTPAnchorRot);
			RotQ = AnchorQ * RotQ;
			Loc = GAutoTPAnchorLoc + AnchorQ.RotateVector(Loc);
		}
		Out.TPLoc = Loc;
		Out.TPRot = RotQ.Rotator();
		return true;
	}

	// ---- True-scale FP derivation (reference = lmg_01) -----------------------------------------------
	namespace
	{
		// Shared mesh-geometry anchors (the ComputeAutoPose recipe, factored so the true-scale path uses
		// byte-identical formulas).
		struct FPFMeshGunGeom
		{
			FVector Along = FVector(0.f, 1.f, 0.f);   // unit barrel axis (mesh local)
			FVector GripMesh = FVector::ZeroVector;   // rear-below-bore grip region
			FVector SightMesh = FVector::ZeroVector;  // top-of-receiver sight line
			bool    bAlongY = true;
		};

		bool PFComputeGunGeom(const UStaticMesh* Mesh, FPFMeshGunGeom& Out)
		{
			if (Mesh == nullptr)
			{
				return false;
			}
			const FBoxSphereBounds B = Mesh->GetBounds();
			const FVector O = B.Origin;
			const FVector E = B.BoxExtent;
			if (E.IsNearlyZero())
			{
				return false;
			}
			const float LenX = FMath::Max(E.X * 2.f, 1.f);
			const float LenY = FMath::Max(E.Y * 2.f, 1.f);
			Out.bAlongY = (LenY >= LenX);
			const float BarrelHalf = (Out.bAlongY ? LenY : LenX) * 0.5f;
			Out.Along = Out.bAlongY ? FVector(0.f, 1.f, 0.f) : FVector(1.f, 0.f, 0.f);
			Out.GripMesh = O - Out.Along * (BarrelHalf * 0.55f) + FVector(0.f, 0.f, -E.Z * 0.55f);
			Out.SightMesh = O + Out.Along * (BarrelHalf * 0.15f) + FVector(0.f, 0.f, E.Z * 0.82f);
			return true;
		}

		// Lazily-derived reference targets, all read off the BAKED lmg_01 catalog row + its mesh — so if
		// Tom ever re-tunes that row and rebuilds, every derived gun follows on next launch.
		struct FPFTrueScaleRef
		{
			bool  bTried = false;
			bool  bValid = false;
			FQuat RotQ = FQuat::Identity;        // reference viewmodel rotation (for a ref-axis mesh)
			bool  bRefAlongY = true;
			float Scale = 1.f;
			FVector GripView = FVector::ZeroVector;   // where the grip sits in ViewModelRoot space (hip)
			FQuat AdsRotQ = FQuat::Identity;
			FRotator AdsRot = FRotator::ZeroRotator;
			FVector SightCam = FVector::ZeroVector;   // where the sight sits in camera space at full ADS
		};

		FPFTrueScaleRef GTrueScaleRef;
	}

	bool HasTrueScaleFP(const FPFWeaponDef& Def)
	{
		if (Def.WeaponId == nullptr)
		{
			return false;
		}
		// Rows dialed by hand at FPScale 1.0 (used verbatim, never derived). lmg_01 = the reference itself;
		// the rest = Tom's 2026-07-23 life-size pass, recovered from the editor log after a force-close.
		// Append as more guns get tuned. Still OFF the list (auto-derived): pis_02/03/04, sg_01-04, snp_01-04.
		static const TCHAR* const TunedAtTrueScale[] = {
			TEXT("lmg_01"), TEXT("lmg_02"), TEXT("lmg_03"), TEXT("lmg_minigun"),
			TEXT("ar_m4"), TEXT("ar_m4_olive"), TEXT("ar_ak_black"), TEXT("ar_ak_wood"),
			TEXT("ar_r01"), TEXT("ar_r02"), TEXT("ar_r03"), TEXT("ar_r04"), TEXT("ar_r05"), TEXT("ar_r06"),
			TEXT("smg_aksu_black"), TEXT("smg_aksu_wood"),
			TEXT("smg_01"), TEXT("smg_02"), TEXT("smg_03"), TEXT("smg_04"), TEXT("smg_05"), TEXT("smg_06"),
			TEXT("pis_std"), TEXT("pis_01"), TEXT("pis_02"), TEXT("pis_03"), TEXT("pis_04"),
			TEXT("rev_01"), TEXT("rev_02"),
			TEXT("sg_01"), TEXT("sg_02"), TEXT("sg_03"), TEXT("sg_04"),
			TEXT("snp_01"), TEXT("snp_02"), TEXT("snp_03"), TEXT("snp_04"),
		};   // full catalog now hand-tuned at life size (Tom 2026-07-23)
		for (const TCHAR* Id : TunedAtTrueScale)
		{
			if (FCString::Strcmp(Def.WeaponId, Id) == 0)
			{
				return true;
			}
		}
		return false;
	}

	bool ComputeTrueScaleFP(const UStaticMesh* Mesh, FPFWeaponAutoPose& Out)
	{
		FPFMeshGunGeom Geom;
		if (!PFComputeGunGeom(Mesh, Geom))
		{
			return false;
		}

		FPFTrueScaleRef& Ref = GTrueScaleRef;
		if (!Ref.bTried)
		{
			Ref.bTried = true;
			const FPFWeaponConfig RefCfg = FindById(TEXT("lmg_01"));
			const FPFWeaponDef& RefDef = Weapon(RefCfg.Category, RefCfg.Index);
			FPFMeshGunGeom RefGeom;
			if (RefDef.WeaponId != nullptr && FCString::Strcmp(RefDef.WeaponId, TEXT("lmg_01")) == 0
				&& PFComputeGunGeom(LoadMesh(RefDef), RefGeom))
			{
				Ref.RotQ = FQuat(RefDef.FPRot);
				Ref.bRefAlongY = RefGeom.bAlongY;
				Ref.Scale = RefDef.FPScale;
				Ref.GripView = RefDef.FPLoc + Ref.RotQ.RotateVector(RefGeom.GripMesh * Ref.Scale);
				Ref.AdsRotQ = FQuat(RefDef.AdsRot);
				Ref.AdsRot = RefDef.AdsRot;
				const FVector HipSightRef = RefDef.FPLoc + Ref.RotQ.RotateVector(RefGeom.SightMesh * Ref.Scale);
				Ref.SightCam = RefDef.AdsLoc + Ref.AdsRotQ.RotateVector(HipSightRef);
				Ref.bValid = true;
			}
		}
		if (!Ref.bValid)
		{
			return false;
		}

		// Axis fix: rotate this mesh in its own space so ITS barrel takes the role the REFERENCE mesh's
		// barrel has under the reference rotation (yaw +/-90 maps +X<->+Y, same convention as the TP grip).
		FQuat RotQ = Ref.RotQ;
		if (Geom.bAlongY != Ref.bRefAlongY)
		{
			RotQ = Ref.RotQ * FQuat(FRotator(0.f, Ref.bRefAlongY ? 90.f : -90.f, 0.f));
		}

		Out.FPRot = RotQ.Rotator();
		Out.FPScale = Ref.Scale;
		Out.FPLoc = Ref.GripView - RotQ.RotateVector(Geom.GripMesh * Ref.Scale);

		// ADS: park THIS gun's estimated sight exactly where the reference's sight sits in camera space.
		Out.AdsRot = Ref.AdsRot;
		const FVector HipSight = Out.FPLoc + RotQ.RotateVector(Geom.SightMesh * Ref.Scale);
		Out.AdsLoc = Ref.SightCam - Ref.AdsRotQ.RotateVector(HipSight);
		// Sane band (same guard idea as ComputeAutoPose, widened for life-size meshes).
		Out.AdsLoc.X = FMath::Clamp(Out.AdsLoc.X, -5.f, 45.f);
		Out.AdsLoc.Y = FMath::Clamp(Out.AdsLoc.Y, -30.f, 30.f);
		Out.AdsLoc.Z = FMath::Clamp(Out.AdsLoc.Z, -30.f, 25.f);
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
		TEXT("WPNDUMP  %-22s %-34s %9s %26s %9s  %s"),
		TEXT("id"), TEXT("mesh"), TEXT("natLen"), TEXT("pivot->centre (X,Y,Z)"), TEXT("offsetLen"),
		TEXT("autoTP loc / rot (axis, tuned rows say TUNED)"));
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
			// What the auto grip would do to this row (0.85 = the character-default TP scale). Tuned rows
			// never receive it — flagged so the table shows which guns are manual.
			FString AutoStr;
			FPFWeaponAutoTP Auto;
			FVector AnchorLoc;
			FRotator AnchorRot;
			float AnchorMult = 1.f;
			PFWeapon::GetAutoTPAnchor(AnchorLoc, AnchorRot, AnchorMult);
			const float DumpScale = (D.TPScale > 0.f) ? D.TPScale : (PFWeapon::AutoTPBaseScale * AnchorMult);
			if (PFWeapon::HasTunedTP(D))
			{
				AutoStr = TEXT("TUNED (row is manual)");
			}
			else if (PFWeapon::ComputeAutoTPGrip(M, DumpScale, D.TPLoc, D.TPRot, 0.55f, 0.55f, true, Auto))
			{
				AutoStr = FString::Printf(TEXT("(%6.1f,%6.1f,%6.1f) / (%5.1f,%5.1f,%5.1f) (%s)"),
					Auto.TPLoc.X, Auto.TPLoc.Y, Auto.TPLoc.Z,
					Auto.TPRot.Pitch, Auto.TPRot.Yaw, Auto.TPRot.Roll,
					Auto.bBarrelAlongY ? TEXT("+Y") : TEXT("+X"));
			}
			else
			{
				AutoStr = TEXT("auto FAILED (degenerate bounds)");
			}
			UE_LOG(CombatForgeLog, Warning,
				TEXT("WPNDUMP  %-22s %-34s %9.1f   (%7.1f,%7.1f,%7.1f) %9.1f  %s"),
				D.WeaponId ? D.WeaponId : TEXT("?"), *M->GetName(),
				NatLen, B.Origin.X, B.Origin.Y, B.Origin.Z, B.Origin.Size(), *AutoStr);
		}
	}
}
static FAutoConsoleCommandWithWorldAndArgs GPFWeaponDumpCmd(
	TEXT("pf.WeaponDump"),
	TEXT("Print measured bounds for every catalog weapon: natural length and length at FP/TP scale."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&PFWeaponDumpCmd));
