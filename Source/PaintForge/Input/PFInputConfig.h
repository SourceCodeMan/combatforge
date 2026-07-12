// Copyright (c) 2026 Tom Chapman. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "PFInputConfig.generated.h"

class APaintForgePlayerController;
class UInputAction;
class UInputMappingContext;
class UInputModifierNegate;
class UInputModifierScalar;

/**
 * Owns every native-constructed Enhanced Input object (22 UInputActions + 3
 * UInputMappingContexts). All objects are reachable from UPROPERTY members so
 * GC never collects them mid-match (02 R1). Built once by
 * APaintForgePlayerController::SetupInputComponent with the PC as outer.
 *
 * Contexts are applied/removed by the PlayerController per phase (§4.5);
 * this class only constructs. Priorities at apply time: IMC_Common = 0,
 * IMC_Combat = 1, IMC_Build = 1.
 */
UCLASS()
class PAINTFORGE_API UPFInputConfig : public UObject
{
	GENERATED_BODY()

public:
	/** Constructs ALL actions, contexts, modifiers and triggers. Idempotent. */
	void Build(APaintForgePlayerController* OuterPC);

	/** Update mouse look scale (1.0 = default). Safe anytime after Build(). */
	void SetLookSensitivity(float Sens);

	/** Invert vertical look (true = mouse-up looks down). Safe after Build(). */
	void SetLookInvertY(bool bInvert);

	// ---- Common (IMC_Common, priority 0) ----
	UPROPERTY(VisibleAnywhere) TObjectPtr<UInputAction> IA_Move;        // Axis2D, WASD swizzle
	UPROPERTY(VisibleAnywhere) TObjectPtr<UInputAction> IA_Look;        // Axis2D, Mouse2D, negate Y, sensitivity scalar
	UPROPERTY(VisibleAnywhere) TObjectPtr<UInputAction> IA_Jump;        // Space
	UPROPERTY(VisibleAnywhere) TObjectPtr<UInputAction> IA_Sprint;      // LeftShift, hold
	UPROPERTY(VisibleAnywhere) TObjectPtr<UInputAction> IA_CrouchSlide; // LeftCtrl + C, hold (slide if sprinting)
	UPROPERTY(VisibleAnywhere) TObjectPtr<UInputAction> IA_Ready;       // F, tap toggle (T22)
	UPROPERTY(VisibleAnywhere) TObjectPtr<UInputAction> IA_Scoreboard;  // Tab, hold
	UPROPERTY(VisibleAnywhere) TObjectPtr<UInputAction> IA_MenuBack;    // Escape
	UPROPERTY(VisibleAnywhere) TObjectPtr<UInputAction> IA_HostStart;   // Enter (host-only effect)

	// ---- Combat (IMC_Combat, priority 1) ----
	UPROPERTY(VisibleAnywhere) TObjectPtr<UInputAction> IA_Fire;        // LMB, hold (weapon runs its own 12 bps gate)
	UPROPERTY(VisibleAnywhere) TObjectPtr<UInputAction> IA_ADS;         // RMB, hold
	UPROPERTY(VisibleAnywhere) TObjectPtr<UInputAction> IA_Reload;      // R (T26)

	// ---- Build (IMC_Build, priority 1) ----
	UPROPERTY(VisibleAnywhere) TObjectPtr<UInputAction> IA_Place;       // LMB, hold (turbo in component)
	UPROPERTY(VisibleAnywhere) TObjectPtr<UInputAction> IA_DeleteTool;  // F5 and X (both mapped — 03 §3)
	UPROPERTY(VisibleAnywhere) TObjectPtr<UInputAction> IA_RotatePiece; // R (T26)
	UPROPERTY(VisibleAnywhere) TObjectPtr<UInputAction> IA_CyclePiece;  // Mouse wheel axis
	UPROPERTY(VisibleAnywhere) TObjectPtr<UInputAction> IA_EquipWall;   // F1
	UPROPERTY(VisibleAnywhere) TObjectPtr<UInputAction> IA_EquipFloor;  // F2
	UPROPERTY(VisibleAnywhere) TObjectPtr<UInputAction> IA_EquipRamp;   // F3
	UPROPERTY(VisibleAnywhere) TObjectPtr<UInputAction> IA_EquipRoof;   // F4
	UPROPERTY(VisibleAnywhere) TObjectPtr<UInputAction> IA_QuickEquip;  // Q, tap (0.18 s) → equip last-used piece
	UPROPERTY(VisibleAnywhere) TObjectPtr<UInputAction> IA_BuildWheel;  // Q, hold (0.18 s) open, release = commit

	// ---- Contexts ----
	UPROPERTY(VisibleAnywhere) TObjectPtr<UInputMappingContext> IMC_Common;
	UPROPERTY(VisibleAnywhere) TObjectPtr<UInputMappingContext> IMC_Combat;
	UPROPERTY(VisibleAnywhere) TObjectPtr<UInputMappingContext> IMC_Build;

	/** Live look-sensitivity scalar; Options menu updates this. */
	UPROPERTY() TObjectPtr<UInputModifierScalar> LookSensitivityScalar;
	/** Vertical look negate; Options Invert Y flips bY. */
	UPROPERTY() TObjectPtr<UInputModifierNegate> LookNegateY;
};
