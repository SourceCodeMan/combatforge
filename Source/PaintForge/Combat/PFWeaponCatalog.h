// Copyright (c) 2026 Tom Chapman. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/PaintForgeTypes.h"   // EPFFireMode

class UStaticMesh;
class UMaterialInterface;

/**
 * One selectable weapon: which mesh + material it uses and how it sits in the first-person viewmodel.
 * The FP pose (Loc/Rot/Scale/Muzzle) is PER MESH — every weapon has its own origin/scale, so these are
 * hand-tuned constants in the catalog table (start from SM_Rifle's known-good pose, then dial in-editor).
 */
struct FPFWeaponDef
{
	const TCHAR* DisplayName = TEXT("Rifle");
	const TCHAR* MeshPath    = nullptr;   // static mesh (FP + TP)
	const TCHAR* MaterialPath = nullptr;  // nullptr = keep the mesh's own authored materials
	FVector  FPLoc   = FVector(3.f, 5.5f, -3.5f);
	FRotator FPRot   = FRotator(-1.5f, -90.f, 1.5f);
	float    FPScale = 0.48f;
	FVector  MuzzleFP = FVector(42.f, 3.5f, -3.5f);
	// Per-weapon aim-down-sight pose (the viewmodel anchor at full ADS). Each weapon's sight sits differently,
	// so ADS alignment is per-weapon. Defaults to the verified SM_Rifle ADS; tune others with pf.WeaponADS.
	FVector  AdsLoc  = FVector(15.f, -5.5f, -1.5f);
	FRotator AdsRot  = FRotator(1.5f, 0.f, -1.5f);

	// ---- Per-class fire behaviour + ballistics (defaults = Assault Rifle; SMG/pistol rows override) ----
	uint8       AllowedFireModes = (1 << 0) | (1 << 1) | (1 << 2);   // bit (1<<EPFFireMode); rifle = all three
	EPFFireMode DefaultFireMode  = EPFFireMode::Auto;
	float       SpreadHipDeg     = 1.2f;      // tighter = more accurate
	float       SpreadADSDeg     = 0.05f;     // near-laser aimed
	float       MuzzleSpeedUU    = 12000.f;   // speed x lifetime ~= effective range
	float       ProjLifetimeSec  = 2.2f;
	uint8       ClassBurstCount  = 3;
};

/** A player's chosen weapon: category + index into that category. */
struct FPFWeaponConfig
{
	int32 Category = 0;
	int32 Index    = 0;
};

/**
 * Curated weapon registry (categories -> weapons). NOT UObjectLibrary-enumerated like the character parts,
 * because each weapon needs its own FP pose + material handling. Mirrors the PFChar API shape so the loadout
 * UI can drive it the same way.
 */
namespace PFWeapon
{
	int32 CategoryCount();
	FString CategoryLabel(int32 Category);
	int32 WeaponCount(int32 Category);
	const FPFWeaponDef& Weapon(int32 Category, int32 Index);     // clamped; category 0 idx 0 = SM_Rifle default
	FString WeaponDisplayName(int32 Category, int32 Index);

	UStaticMesh* LoadMesh(const FPFWeaponDef& Def);
	UMaterialInterface* LoadMaterial(const FPFWeaponDef& Def);   // nullptr if the def preserves authored mats

	FPFWeaponConfig DefaultConfig();

	// ---- Persistence (GGameUserSettings.ini [PaintForge]) ----
	void SaveConfig(const FPFWeaponConfig& Config);
	FPFWeaponConfig LoadConfig();
}
