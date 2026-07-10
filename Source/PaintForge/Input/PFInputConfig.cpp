// Copyright (c) 2026 Tom Chapman. All rights reserved.

#include "Input/PFInputConfig.h"

#include "Core/PaintForgePlayerController.h"   // complete type for the UObject* outer conversion
#include "PaintForge.h"
#include "InputAction.h"
#include "InputMappingContext.h"
#include "InputModifiers.h"
#include "InputTriggers.h"
#include "InputCoreTypes.h"
#include "Misc/ConfigCacheIni.h"

namespace
{
	// 04 cut #9: PFSensitivity 1.0 == 0.07 degrees per mouse unit via an
	// Enhanced Input scalar modifier (UE5 default input scales are 1.0).
	constexpr float BaseDegreesPerMouseUnit = 0.07f;

	UInputAction* MakeAction(UObject* Outer, const TCHAR* Name, EInputActionValueType ValueType)
	{
		UInputAction* Action = NewObject<UInputAction>(Outer, FName(Name));
		Action->ValueType = ValueType;
		return Action;
	}
}

void UPFInputConfig::Build(APaintForgePlayerController* OuterPC)
{
	if (IMC_Common != nullptr)
	{
		return; // idempotent — never rebuild input objects (02 R1)
	}
	if (OuterPC == nullptr)
	{
		UE_LOG(PaintForgeLog, Error, TEXT("UPFInputConfig::Build called with null PlayerController"));
		return;
	}

	UObject* Outer = OuterPC; // 02 R1 / §5.7: actions + contexts are outered to the PC

	// ---- Actions (22) ----
	IA_Move        = MakeAction(Outer, TEXT("IA_Move"),        EInputActionValueType::Axis2D);
	IA_Look        = MakeAction(Outer, TEXT("IA_Look"),        EInputActionValueType::Axis2D);
	IA_Jump        = MakeAction(Outer, TEXT("IA_Jump"),        EInputActionValueType::Boolean);
	IA_Sprint      = MakeAction(Outer, TEXT("IA_Sprint"),      EInputActionValueType::Boolean);
	IA_CrouchSlide = MakeAction(Outer, TEXT("IA_CrouchSlide"), EInputActionValueType::Boolean);
	IA_Ready       = MakeAction(Outer, TEXT("IA_Ready"),       EInputActionValueType::Boolean);
	IA_Scoreboard  = MakeAction(Outer, TEXT("IA_Scoreboard"),  EInputActionValueType::Boolean);
	IA_MenuBack    = MakeAction(Outer, TEXT("IA_MenuBack"),    EInputActionValueType::Boolean);
	IA_HostStart   = MakeAction(Outer, TEXT("IA_HostStart"),   EInputActionValueType::Boolean);

	IA_Fire        = MakeAction(Outer, TEXT("IA_Fire"),        EInputActionValueType::Boolean);
	IA_ADS         = MakeAction(Outer, TEXT("IA_ADS"),         EInputActionValueType::Boolean);
	IA_Reload      = MakeAction(Outer, TEXT("IA_Reload"),      EInputActionValueType::Boolean);

	IA_Place       = MakeAction(Outer, TEXT("IA_Place"),       EInputActionValueType::Boolean);
	IA_DeleteTool  = MakeAction(Outer, TEXT("IA_DeleteTool"),  EInputActionValueType::Boolean);
	IA_RotatePiece = MakeAction(Outer, TEXT("IA_RotatePiece"), EInputActionValueType::Boolean);
	IA_CyclePiece  = MakeAction(Outer, TEXT("IA_CyclePiece"),  EInputActionValueType::Axis1D);
	IA_EquipWall   = MakeAction(Outer, TEXT("IA_EquipWall"),   EInputActionValueType::Boolean);
	IA_EquipFloor  = MakeAction(Outer, TEXT("IA_EquipFloor"),  EInputActionValueType::Boolean);
	IA_EquipRamp   = MakeAction(Outer, TEXT("IA_EquipRamp"),   EInputActionValueType::Boolean);
	IA_EquipRoof   = MakeAction(Outer, TEXT("IA_EquipRoof"),   EInputActionValueType::Boolean);
	IA_QuickEquip  = MakeAction(Outer, TEXT("IA_QuickEquip"),  EInputActionValueType::Boolean);
	IA_BuildWheel  = MakeAction(Outer, TEXT("IA_BuildWheel"),  EInputActionValueType::Boolean);

	// ---- Contexts ----
	IMC_Common = NewObject<UInputMappingContext>(Outer, TEXT("IMC_Common"));
	IMC_Combat = NewObject<UInputMappingContext>(Outer, TEXT("IMC_Combat"));
	IMC_Build  = NewObject<UInputMappingContext>(Outer, TEXT("IMC_Build"));

	// ================= IMC_Common =================

	// Move: W = +Y (forward), S = -Y, A = -X, D = +X. 1D key values land on X;
	// swizzle YXZ moves them to Y for the forward axis (02 §3.1).
	{
		FEnhancedActionKeyMapping& W = IMC_Common->MapKey(IA_Move, EKeys::W);
		UInputModifierSwizzleAxis* SwzW = NewObject<UInputModifierSwizzleAxis>(IMC_Common);
		SwzW->Order = EInputAxisSwizzle::YXZ;
		W.Modifiers.Add(SwzW);

		FEnhancedActionKeyMapping& S = IMC_Common->MapKey(IA_Move, EKeys::S);
		UInputModifierSwizzleAxis* SwzS = NewObject<UInputModifierSwizzleAxis>(IMC_Common);
		SwzS->Order = EInputAxisSwizzle::YXZ;
		S.Modifiers.Add(SwzS);
		S.Modifiers.Add(NewObject<UInputModifierNegate>(IMC_Common));

		FEnhancedActionKeyMapping& A = IMC_Common->MapKey(IA_Move, EKeys::A);
		A.Modifiers.Add(NewObject<UInputModifierNegate>(IMC_Common));

		IMC_Common->MapKey(IA_Move, EKeys::D);
	}

	// Look: Mouse2D, negate Y only, plus the PFSensitivity scalar.
	{
		float Sensitivity = 1.0f;
		if (GConfig != nullptr)
		{
			GConfig->GetFloat(TEXT("PaintForge"), TEXT("PFSensitivity"), Sensitivity, GGameUserSettingsIni);
		}
		const float Scale = BaseDegreesPerMouseUnit * Sensitivity;

		FEnhancedActionKeyMapping& Look = IMC_Common->MapKey(IA_Look, EKeys::Mouse2D);

		UInputModifierScalar* Scalar = NewObject<UInputModifierScalar>(IMC_Common);
		Scalar->Scalar = FVector(Scale, Scale, 1.0);
		Look.Modifiers.Add(Scalar);

		UInputModifierNegate* NegY = NewObject<UInputModifierNegate>(IMC_Common);
		NegY->bX = false;
		NegY->bY = true;
		NegY->bZ = false;
		Look.Modifiers.Add(NegY);
	}

	IMC_Common->MapKey(IA_Jump,        EKeys::SpaceBar);
	IMC_Common->MapKey(IA_Sprint,      EKeys::LeftShift);
	IMC_Common->MapKey(IA_CrouchSlide, EKeys::LeftControl);
	IMC_Common->MapKey(IA_CrouchSlide, EKeys::C);
	IMC_Common->MapKey(IA_Ready,       EKeys::F);
	IMC_Common->MapKey(IA_Scoreboard,  EKeys::Tab);
	IMC_Common->MapKey(IA_MenuBack,    EKeys::Escape);
	IMC_Common->MapKey(IA_HostStart,   EKeys::Enter);

	// ================= IMC_Combat =================

	// No InputTriggerPulse on Fire — the weapon runs its own 12 bps gate (§3.3).
	IMC_Combat->MapKey(IA_Fire,   EKeys::LeftMouseButton);
	IMC_Combat->MapKey(IA_ADS,    EKeys::RightMouseButton);
	IMC_Combat->MapKey(IA_Reload, EKeys::R); // T26: same key as RotatePiece, contexts never coexist

	// ================= IMC_Build =================

	IMC_Build->MapKey(IA_Place,       EKeys::LeftMouseButton);
	IMC_Build->MapKey(IA_DeleteTool,  EKeys::F5);
	IMC_Build->MapKey(IA_DeleteTool,  EKeys::X);
	IMC_Build->MapKey(IA_RotatePiece, EKeys::R); // T26
	IMC_Build->MapKey(IA_CyclePiece,  EKeys::MouseWheelAxis);
	IMC_Build->MapKey(IA_EquipWall,   EKeys::F1);
	IMC_Build->MapKey(IA_EquipFloor,  EKeys::F2);
	IMC_Build->MapKey(IA_EquipRamp,   EKeys::F3);
	IMC_Build->MapKey(IA_EquipRoof,   EKeys::F4);

	// Q tap (< 0.18 s) = quick-equip last-used piece; Q hold (>= 0.18 s) = wheel.
	{
		FEnhancedActionKeyMapping& Tap = IMC_Build->MapKey(IA_QuickEquip, EKeys::Q);
		UInputTriggerTap* TapTrigger = NewObject<UInputTriggerTap>(IMC_Build);
		TapTrigger->TapReleaseTimeThreshold = 0.18f;
		Tap.Triggers.Add(TapTrigger);

		FEnhancedActionKeyMapping& Hold = IMC_Build->MapKey(IA_BuildWheel, EKeys::Q);
		UInputTriggerHold* HoldTrigger = NewObject<UInputTriggerHold>(IMC_Build);
		HoldTrigger->HoldTimeThreshold = 0.18f;
		HoldTrigger->bIsOneShot = true;
		Hold.Triggers.Add(HoldTrigger);
	}

	UE_LOG(PaintForgeLog, Log, TEXT("UPFInputConfig built: 22 actions, 3 mapping contexts"));
}
