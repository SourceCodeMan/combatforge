// Copyright (c) 2026 Tom Chapman. All rights reserved.

#include "Input/PFInputConfig.h"
#include "Core/PFPaths.h"

#include "Core/CombatForgePlayerController.h"   // complete type for the UObject* outer conversion
#include "CombatForge.h"
#include "InputAction.h"
#include "InputMappingContext.h"
#include "InputModifiers.h"
#include "InputTriggers.h"
#include "InputCoreTypes.h"
#include "Core/PFUserPrefs.h"
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

void UPFInputConfig::SetLookSensitivity(float Sens)
{
	if (LookSensitivityScalar == nullptr)
	{
		return;
	}
	const float Scale = BaseDegreesPerMouseUnit * FMath::Clamp(Sens, 0.2f, 6.f);
	LookSensitivityScalar->Scalar = FVector(Scale, Scale, 1.0);
}

void UPFInputConfig::SetLookInvertY(bool bInvert)
{
	if (LookNegateY == nullptr)
	{
		return;
	}
	// Default FPS: negate mouse Y so mouse-up looks up. Invert Y = natural mouse (no Y negate).
	LookNegateY->bX = false;
	LookNegateY->bY = !bInvert;
	LookNegateY->bZ = false;
}

void UPFInputConfig::Build(ACombatForgePlayerController* OuterPC)
{
	if (IMC_Common != nullptr)
	{
		return; // idempotent — never rebuild input objects (02 R1)
	}
	if (OuterPC == nullptr)
	{
		UE_LOG(CombatForgeLog, Error, TEXT("UPFInputConfig::Build called with null PlayerController"));
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
	IA_CycleClass  = MakeAction(Outer, TEXT("IA_CycleClass"),  EInputActionValueType::Axis1D);
	IA_WeaponDrag  = MakeAction(Outer, TEXT("IA_WeaponDrag"),  EInputActionValueType::Boolean);
	IA_HostStart   = MakeAction(Outer, TEXT("IA_HostStart"),   EInputActionValueType::Boolean);

	IA_Fire        = MakeAction(Outer, TEXT("IA_Fire"),        EInputActionValueType::Boolean);
	IA_ADS         = MakeAction(Outer, TEXT("IA_ADS"),         EInputActionValueType::Boolean);
	IA_Reload      = MakeAction(Outer, TEXT("IA_Reload"),      EInputActionValueType::Boolean);
	IA_Interact    = MakeAction(Outer, TEXT("IA_Interact"),    EInputActionValueType::Boolean);
	IA_FireSelect  = MakeAction(Outer, TEXT("IA_FireSelect"),  EInputActionValueType::Boolean);
	IA_ThrowFrag   = MakeAction(Outer, TEXT("IA_ThrowFrag"),   EInputActionValueType::Boolean);
	IA_ThrowSmoke  = MakeAction(Outer, TEXT("IA_ThrowSmoke"),  EInputActionValueType::Boolean);
	IA_Melee       = MakeAction(Outer, TEXT("IA_Melee"),       EInputActionValueType::Boolean);
	IA_PlantBomb   = MakeAction(Outer, TEXT("IA_PlantBomb"),   EInputActionValueType::Boolean);

	// Interact shares F with IA_Ready (IMC_Common). Don't consume the key, so the priority-1 Combat mapping
	// lets the priority-0 Ready mapping also fire — the two handlers are phase-disjoint (Ready only acts in
	// Lobby/Build, Interact only finds barrels in Combat), so both firing on F is harmless.
	IA_Interact->bConsumeInput = false;

	IA_Place       = MakeAction(Outer, TEXT("IA_Place"),       EInputActionValueType::Boolean);
	IA_DeleteTool  = MakeAction(Outer, TEXT("IA_DeleteTool"),  EInputActionValueType::Boolean);
	IA_RotatePiece = MakeAction(Outer, TEXT("IA_RotatePiece"), EInputActionValueType::Boolean);
	IA_CyclePiece  = MakeAction(Outer, TEXT("IA_CyclePiece"),  EInputActionValueType::Axis1D);
	IA_EquipWall   = MakeAction(Outer, TEXT("IA_EquipWall"),   EInputActionValueType::Boolean);
	IA_EquipFloor  = MakeAction(Outer, TEXT("IA_EquipFloor"),  EInputActionValueType::Boolean);
	IA_EquipRamp   = MakeAction(Outer, TEXT("IA_EquipRamp"),   EInputActionValueType::Boolean);
	IA_EquipRoof   = MakeAction(Outer, TEXT("IA_EquipRoof"),   EInputActionValueType::Boolean);
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
		float Sensitivity = 3.0f;   // Tom 2026-07-17 default = the old max (range now 0.2..6)
		if (GConfig != nullptr)
		{
			GConfig->GetFloat(TEXT("CombatForge"), TEXT("PFSensitivity"), Sensitivity, FPFPaths::UserPrefsIni());
		}
		// A hand-edited/corrupt INI value must not produce an unusable (0 or absurd) look scale — same
		// clamp SetLookSensitivity applies (issue #19 I2).
		Sensitivity = FMath::Clamp(Sensitivity, 0.2f, 6.f);
		const float Scale = BaseDegreesPerMouseUnit * Sensitivity;

		FEnhancedActionKeyMapping& Look = IMC_Common->MapKey(IA_Look, EKeys::Mouse2D);

		LookSensitivityScalar = NewObject<UInputModifierScalar>(IMC_Common);
		LookSensitivityScalar->Scalar = FVector(Scale, Scale, 1.0);
		Look.Modifiers.Add(LookSensitivityScalar);

		LookNegateY = NewObject<UInputModifierNegate>(IMC_Common);
		LookNegateY->bX = false;
		LookNegateY->bY = !FPFUserPrefs::GetInvertY(); // true = normal FPS look
		LookNegateY->bZ = false;
		Look.Modifiers.Add(LookNegateY);
	}

	IMC_Common->MapKey(IA_Jump,        EKeys::SpaceBar);
	IMC_Common->MapKey(IA_Sprint,      EKeys::LeftShift);
	IMC_Common->MapKey(IA_CrouchSlide, EKeys::LeftControl);
	IMC_Common->MapKey(IA_CrouchSlide, EKeys::C);
	IMC_Common->MapKey(IA_Ready,       EKeys::F);
	IMC_Common->MapKey(IA_Scoreboard,  EKeys::Tab);
	IMC_Common->MapKey(IA_MenuBack,    EKeys::Escape);
	IMC_Common->MapKey(IA_HostStart,   EKeys::Enter);
	// Wheel = class switch, but ONLY while dead (handler gates on OutKind==1). Build mode also maps the wheel
	// (IA_CyclePiece) — no clash: you can't be out while building, so this mapping is inert there.
	IMC_Common->MapKey(IA_CycleClass,  EKeys::MouseWheelAxis);
	// Middle mouse = dev FP pose drag (inert unless pf.WeaponDrag 1). Common context so it works in combat.
	IMC_Common->MapKey(IA_WeaponDrag,  EKeys::MiddleMouseButton);

	// ================= IMC_Combat =================

	// No InputTriggerPulse on Fire — the weapon runs its own 12 bps gate (§3.3).
	IMC_Combat->MapKey(IA_Fire,       EKeys::LeftMouseButton);
	IMC_Combat->MapKey(IA_ADS,        EKeys::RightMouseButton);
	IMC_Combat->MapKey(IA_Reload,     EKeys::R); // T26: same key as RotatePiece, contexts never coexist
	IMC_Combat->MapKey(IA_Interact,   EKeys::F); // ammo barrels — shares F with Ready (non-consuming, phase-disjoint)
	IMC_Combat->MapKey(IA_FireSelect, EKeys::V); // cycle fire mode
	IMC_Combat->MapKey(IA_ThrowFrag,  EKeys::E); // frag grenade (E freed by interact->F)
	IMC_Combat->MapKey(IA_ThrowSmoke, EKeys::Q); // smoke grenade (Q = build wheel in IMC_Build; contexts never coexist)
	IMC_Combat->MapKey(IA_Melee,      EKeys::B);                 // melee tag — keyboard
	IMC_Combat->MapKey(IA_Melee,      EKeys::ThumbMouseButton);  // melee tag — mouse thumb (genre-standard)
	IMC_Combat->MapKey(IA_PlantBomb,  EKeys::G);                 // demolition bomb on the aimed build piece

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

	// Q tap = toggle build wheel (open on first press, commit/cancel on second). No hold threshold —
	// hold-to-open was unreliable and hid the wheel labels until 0.18s had elapsed.
	IMC_Build->MapKey(IA_BuildWheel, EKeys::Q);

	// Rebindable-action registry, then apply any saved key overrides. Must run INSIDE Build() (which is
	// idempotent-guarded) after the default MapKey calls, not via a re-run.
	BuildRebindRegistry();
	ApplySavedKeyOverrides();

	UE_LOG(CombatForgeLog, Log, TEXT("UPFInputConfig built (3 mapping contexts)"));   // no hardcoded action count — it drifted (issue #19 I4)
}

// ---------------------------------------------------------------- key rebinding

void UPFInputConfig::BuildRebindRegistry()
{
	RebindEntries.Reset();
	auto Add = [this](FName Id, const FString& Label, UInputAction* Action, UInputMappingContext* Ctx, FKey DefaultKey)
	{
		if (Action == nullptr || Ctx == nullptr)
		{
			return;
		}
		FRebindEntry E;
		E.Id = Id;
		E.Label = Label;
		E.Action = Action;
		E.Context = Ctx;
		E.DefaultKey = DefaultKey;
		E.CurrentKey = DefaultKey;
		RebindEntries.Add(E);
	};
	// Keyboard, single-key, no-modifier boolean actions. Move/Look are excluded (they carry Swizzle/Negate/
	// Scalar modifiers a naive remap would drop); CrouchSlide is excluded (dual-mapped LCtrl + C).
	Add(FName(TEXT("Jump")),       TEXT("Jump"),         IA_Jump,       IMC_Common, EKeys::SpaceBar);
	Add(FName(TEXT("Sprint")),     TEXT("Sprint"),       IA_Sprint,     IMC_Common, EKeys::LeftShift);
	Add(FName(TEXT("Reload")),     TEXT("Reload"),       IA_Reload,     IMC_Combat, EKeys::R);
	Add(FName(TEXT("Interact")),   TEXT("Use / Refill"), IA_Interact,   IMC_Combat, EKeys::F);
	Add(FName(TEXT("FireSelect")), TEXT("Fire Mode"),    IA_FireSelect, IMC_Combat, EKeys::V);
	Add(FName(TEXT("ThrowFrag")),  TEXT("Throw Frag"),   IA_ThrowFrag,  IMC_Combat, EKeys::E);
	Add(FName(TEXT("ThrowSmoke")), TEXT("Throw Smoke"),  IA_ThrowSmoke, IMC_Combat, EKeys::Q);
	// Punch (the former "melee") — Tom 2026-07-18: expose it in the rebind UI so it can be set to a mouse
	// button OR a key. Its default keyboard bind is B; the ThumbMouseButton default (mapped above) stays as a
	// second bind, so a rebind ADDS the chosen key alongside the thumb button (a feature here, unlike CrouchSlide).
	Add(FName(TEXT("Punch")),      TEXT("Punch"),        IA_Melee,      IMC_Combat, EKeys::B);
}

void UPFInputConfig::ApplySavedKeyOverrides()
{
	for (FRebindEntry& E : RebindEntries)
	{
		const FKey Saved = FPFUserPrefs::GetKeyOverride(E.Id);
		if (Saved.IsValid() && Saved != E.CurrentKey && E.Context && E.Action)
		{
			E.Context->UnmapKey(E.Action, E.CurrentKey);
			E.Context->MapKey(E.Action, Saved);
			E.CurrentKey = Saved;
		}
	}
}

UPFInputConfig::FRebindEntry* UPFInputConfig::FindRebind(FName Id)
{
	return RebindEntries.FindByPredicate([Id](const FRebindEntry& E) { return E.Id == Id; });
}

const UPFInputConfig::FRebindEntry* UPFInputConfig::FindRebind(FName Id) const
{
	return RebindEntries.FindByPredicate([Id](const FRebindEntry& E) { return E.Id == Id; });
}

void UPFInputConfig::GetRebindables(TArray<FRebindInfo>& Out) const
{
	Out.Reset();
	for (const FRebindEntry& E : RebindEntries)
	{
		Out.Add(FRebindInfo{ E.Id, E.Label, E.CurrentKey });
	}
}

FKey UPFInputConfig::GetActionKey(FName Id) const
{
	const FRebindEntry* E = FindRebind(Id);
	return E ? E->CurrentKey : FKey();
}

FKey UPFInputConfig::GetActionDefaultKey(FName Id) const
{
	const FRebindEntry* E = FindRebind(Id);
	return E ? E->DefaultKey : FKey();
}

bool UPFInputConfig::SetActionKey(FName Id, FKey NewKey)
{
	FRebindEntry* E = FindRebind(Id);
	if (E == nullptr || E->Action == nullptr || E->Context == nullptr || !NewKey.IsValid())
	{
		return false;
	}
	// Conflict check (issue #19 I1): without it a duplicate bind silently made two actions fire on one
	// key (or stole a FIXED key like Escape) with no feedback — the classic "my controls broke" report.
	// P2-I1: the reserved set now covers EVERY fixed (non-rebindable) mapping — fire/ADS, WASD,
	// crouch, the build hotkeys, the wheel — not just the old menu four; landing a rebind on any
	// of them double-fired two actions on one key. P2-I2: a key that is THIS entry's shipped
	// DEFAULT is always legal — the shipped state itself pairs F (Ready + Interact) and Q (wheel
	// + smoke) across contexts, and rejecting defaults locked players out of restoring them.
	// A key held by ANOTHER rebindable entry is rejected too (predictable beats auto-swap).
	static const FKey FixedKeys[] = {
		EKeys::Escape, EKeys::Tab, EKeys::Enter, EKeys::F,                                // menu/ready
		EKeys::LeftMouseButton, EKeys::RightMouseButton, EKeys::MiddleMouseButton,        // fire/ADS/drag
		EKeys::W, EKeys::A, EKeys::S, EKeys::D, EKeys::LeftControl, EKeys::C,             // move/crouch
		EKeys::F1, EKeys::F2, EKeys::F3, EKeys::F4, EKeys::F5, EKeys::X, EKeys::Q         // build kit
	};
	if (NewKey != E->DefaultKey)
	{
		for (const FKey& R : FixedKeys)
		{
			if (NewKey == R && E->CurrentKey != R)
			{
				UE_LOG(CombatForgeLog, Warning, TEXT("Rebind rejected: %s is reserved"), *NewKey.GetDisplayName().ToString());
				return false;
			}
		}
	}
	for (const FRebindEntry& Other : RebindEntries)
	{
		if (Other.Id != E->Id && Other.CurrentKey == NewKey)
		{
			UE_LOG(CombatForgeLog, Warning, TEXT("Rebind rejected: %s is already bound to %s"),
				*NewKey.GetDisplayName().ToString(), *Other.Label);
			return false;
		}
	}
	E->Context->UnmapKey(E->Action, E->CurrentKey);
	E->Context->MapKey(E->Action, NewKey);
	E->CurrentKey = NewKey;
	return true;
}

void UPFInputConfig::ResetActionKeysToDefaults()
{
	for (FRebindEntry& E : RebindEntries)
	{
		if (E.Context && E.Action)
		{
			E.Context->UnmapKey(E.Action, E.CurrentKey);
			E.Context->MapKey(E.Action, E.DefaultKey);
			E.CurrentKey = E.DefaultKey;
		}
	}
}
