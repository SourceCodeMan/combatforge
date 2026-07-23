// Copyright (c) 2026 Tom Chapman. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/CombatForgeTypes.h"   // EPFFireMode

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

	// ---- Per-weapon THIRD-PERSON grip (where the gun sits in hand_r; what everyone else sees) ----
	// PER-WEAPON for the same reason FPLoc is, and it is the reason a single shared offset could never work:
	// these meshes have their PIVOT in wildly different places relative to the model. Measured with
	// pf.WeaponDump (2026-07-20), pivot->centre ranges from 5.1 (ar_r05) to 21.1 (ar_r02) — so snapping the
	// pivot to the hand throws each gun a DIFFERENT distance away, and one shared WeaponRelativeLocation is
	// mathematically incapable of being right for more than one weapon. That is why the third-person grip
	// failed every attempt, hand-tuned and computed alike.
	// Tune per weapon with `pf.WeaponTP x y z pitch yaw roll [scale]` and paste the printed line here.
	// TPScale <= 0 means "unset": fall back to the character's WeaponRelativeScale.
	FVector  TPLoc   = FVector(-3.f, 4.f, 2.f);
	FRotator TPRot   = FRotator(10.f, 0.f, 90.f);
	float    TPScale = 0.f;

	// ---- Per-class fire behaviour + ballistics (defaults = Assault Rifle; SMG/pistol rows override) ----
	uint8       AllowedFireModes = (1 << 0) | (1 << 1) | (1 << 2);   // bit (1<<EPFFireMode); rifle = all three
	EPFFireMode DefaultFireMode  = EPFFireMode::Auto;
	float       SpreadHipDeg     = 1.2f;      // tighter = more accurate
	float       SpreadADSDeg     = 0.05f;     // near-laser aimed
	float       MuzzleSpeedUU    = 12000.f;   // speed x lifetime ~= effective range
	float       ProjLifetimeSec  = 2.2f;
	uint8       ClassBurstCount  = 3;
	uint8       MagSize          = 30;        // per-weapon magazine
	float       FireRateBps      = 12.f;      // per-weapon rate of fire (BBs/sec) — replaced the global marker preset

	// ---- Per-weapon identity (feel / rank / Stage 2–4 consumers) — keep at END for partial-init safety ----
	const TCHAR* WeaponId = nullptr;   // NEVER nullptr in table rows (stable slug, e.g. "ar_m4")
	uint8  HitValue = 1;
	uint8  Pellets = 1;
	float  PelletSpreadDeg = 0.f;
	float  BloomPerShotDeg = 0.15f;
	float  BloomCapDeg = 2.0f;
	uint8  BloomFreeShots = 5;
	float  BloomDecayDegPerSec = 6.f;
	float  ClimbPitchPerShotDeg = 0.30f;
	float  ClimbYawPerShotDeg = 0.12f;
	float  ClimbRecoverDegPerSec = 14.f;
	float  ADSTimeSec = 0.25f;
	float  SprintOutTime = 0.18f;
	float  MoveSpeedMult = 0.95f;
	float  ReloadTime = 1.0f;
	float  SpinupSec = 0.f;
	float  ReburstDelaySec = 0.f;
	float  MoveSpreadMult = 1.35f;  // multiplies hip for SpreadHipMoving in ApplyWeaponLoadout
	uint8  UnlockRank = 1;

	// ---- Third-person RAISED (fire/ADS) pose correction — what OTHER players see while this pawn shoots ----
	// The raised-pose world-rotation is (Aim.Pitch, Aim.Yaw + TPRaisedYawOffset, TPRaisedRoll). The defaults
	// (-90 / 0) are SM_Rifle's +Y-barrel axis; a mesh whose barrel points down a different local axis (the
	// pistols render UPSIDE DOWN while firing) overrides these. Pure per-weapon; tune in-editor if the guess
	// is off (only two numbers). Hand-carry (running/idle) pose stays the shared WeaponRelative* — it reads
	// correctly across meshes today.
	float  TPRaisedYawOffset = -90.f;
	float  TPRaisedRoll      = 0.f;

	// ---- Optic / scope ADS FOV. 0 = use the character's default ADSFOV (58). A scoped weapon (sniper) sets a
	// much smaller value for a strong magnified sight picture. NOTE: this is the ZOOM half of a scope. The
	// "look through the scope, not the whole screen" MASK (black surround + reticle) is a HUD overlay — see
	// docs note; the FOV here is inert-safe on its own (snipers just zoom harder than an SMG ADS). ----
	float  ScopedADSFOV = 0.f;
};

/** A player's chosen weapon: category + index into that category. */
struct FPFWeaponConfig
{
	int32 Category = 0;
	int32 Index    = 0;
};

/** First-pass hip + ADS pose derived from mesh bounds (no hand tuning). */
struct FPFWeaponAutoPose
{
	FVector  FPLoc   = FVector(3.f, 5.5f, -3.5f);
	FRotator FPRot   = FRotator(-1.5f, -90.f, 1.5f);
	float    FPScale = 0.48f;
	FVector  AdsLoc  = FVector(15.f, -5.5f, -1.5f);
	FRotator AdsRot  = FRotator(1.5f, 0.f, -0.5f);
};

/** Computed third-person grip for one weapon mesh (hand_r bone space unless noted). */
struct FPFWeaponAutoTP
{
	FVector  TPLoc = FVector(-3.f, 4.f, 2.f);
	FRotator TPRot = FRotator(10.f, 0.f, 90.f);
	bool     bBarrelAlongY = true;   // mesh-local +Y is the barrel (SM_Rifle family); false = +X (Modern Weapons)
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

	/**
	 * Auto hip-carry + ADS from mesh bounds. Category biases scale/hold (pistol vs rifle).
	 * Returns false if Mesh is null (Out left unchanged). Catalog values remain the manual override path
	 * when pf.WeaponAutoPose is 0.
	 */
	bool ComputeAutoPose(const UStaticMesh* Mesh, int32 Category, FPFWeaponAutoPose& Out);

	/**
	 * The scale every AUTO-gripped weapon starts from (the shipped character default). A NAMED constant on
	 * purpose: TuneWeaponTP mutates the character's WeaponRelativeScale live, so deriving the auto base from
	 * that member would let a pf.WeaponTP session silently shift what the calibrate solve and the baked
	 * GAutoTPAnchorScaleMult mean. Everything auto (RecomputeTPGrip, CalibrateAutoTPFromCurrent,
	 * pf.WeaponDump's autoTP column) keys off this one number so the round-trip is exact.
	 */
	inline constexpr float AutoTPBaseScale = 0.85f;

	/**
	 * True if this row carries hand-tuned third-person values (pf.WeaponTP paste). A tuned row is FULLY
	 * manual — the auto default never touches it. Untuned rows (all still at the struct defaults) get
	 * ComputeAutoTPGrip instead when pf.WeaponAutoTP is on.
	 */
	bool HasTunedTP(const FPFWeaponDef& Def);

	/**
	 * Per-weapon third-person grip computed from mesh bounds — the same grip-anchor recipe ComputeAutoPose
	 * uses for FP, aimed at the hand instead of the camera. BaseLoc/BaseRot describe where a mesh whose
	 * PIVOT sits at its grip should attach (the catalog struct defaults, authored for a +Y-barrel gun); the
	 * function then (a) pre-rotates +X-barrel meshes 90° so every barrel takes the same hand-space line and
	 * (b) offsets the attach so the mesh's GRIP REGION (rear of gun, below the bore) lands exactly where
	 * BaseLoc puts it — cancelling the per-mesh pivot scatter (5–21uu, pf.WeaponDump 2026-07-20) that made
	 * one shared offset mathematically incapable of fitting more than one gun.
	 * GripAlongFrac/GripDownFrac pick the anchor point (0.55/0.55 = grip, 0/0 = mesh centre — the back sling).
	 * bApplyAnchor folds in the global live correction (pf.WeaponTPAnchor / pf.WeaponTPCalibrate).
	 * Returns false on null mesh / degenerate bounds / non-positive scale (Out untouched).
	 */
	bool ComputeAutoTPGrip(const UStaticMesh* Mesh, float AppliedScale,
		const FVector& BaseLoc, const FRotator& BaseRot,
		float GripAlongFrac, float GripDownFrac, bool bApplyAnchor, FPFWeaponAutoTP& Out);

	/**
	 * Global correction applied on top of every AUTO TP grip (never to hand-tuned rows). Solved live by
	 * pf.WeaponTPCalibrate from ONE hand-tuned gun so the whole catalog inherits the fix; paste the printed
	 * constants into PFWeaponCatalog.cpp (GAutoTPAnchor*) to bake.
	 */
	void SetAutoTPAnchor(const FVector& Loc, const FRotator& Rot, float ScaleMult);
	void GetAutoTPAnchor(FVector& OutLoc, FRotator& OutRot, float& OutScaleMult);

	FPFWeaponConfig DefaultConfig();

	/** Find by WeaponId slug (e.g. "ar_m4"). Returns DefaultConfig() if missing. */
	FPFWeaponConfig FindById(FStringView WeaponId);
	/** WeaponId of category/index or empty. */
	FString IdOf(int32 Category, int32 Index);
	/** Unlock rank for display (1 if missing). */
	uint8 UnlockRankOf(int32 Category, int32 Index);

	// ---- Persistence (GGameUserSettings.ini [CombatForge]) ----
	// The weapon choice is PER CLASS SLOT (the same 5 slots as the character save slots) — each class is a full
	// kit: clothing + gun. The parameterless overloads route through PFChar::GetActiveSaveSlot().
	void SaveConfig(int32 ClassSlot, const FPFWeaponConfig& Config);
	FPFWeaponConfig LoadConfig(int32 ClassSlot);
	void SaveConfig(const FPFWeaponConfig& Config);
	FPFWeaponConfig LoadConfig();

	/** Second weapon (sling / swap target) — any category, not forced to pistol. */
	void SaveSecondaryConfig(int32 ClassSlot, const FPFWeaponConfig& Config);
	FPFWeaponConfig LoadSecondaryConfig(int32 ClassSlot);
	void SaveSecondaryConfig(const FPFWeaponConfig& Config);
	FPFWeaponConfig LoadSecondaryConfig();
}
