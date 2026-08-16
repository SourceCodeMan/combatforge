// Copyright (c) 2026 Tom Chapman. All rights reserved.

#include "Input/PFGamepadCursor.h"

#include "CombatForge.h"
#include "Core/CombatForgeGameInstance.h"
#include "Core/CombatForgeGameState.h"
#include "Core/CombatForgePlayerController.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Application/SlateUser.h"
#include "InputCoreTypes.h"

bool FPFInputDevice::bGamepadPrimary = false;

namespace
{
	// Right-stick scroll speed over ScrollBoxes/lists, in mouse-wheel notches per second
	// at full deflection.
	constexpr float ScrollNotchesPerSec = 6.f;
	constexpr float ScrollStickDeadZone = 0.25f;
}

FPFGamepadCursor::FPFGamepadCursor(UCombatForgeGameInstance* InGameInstance)
	: GameInstance(InGameInstance)
{
	// Defaults tuned for 1080p menu targets; sticky slowdown helps land on buttons.
	SetAcceleration(1400.f);
	SetMaxSpeed(1400.f);
	SetStickySlowdown(0.6f);
	SetDeadZone(0.2f);
}

void FPFGamepadCursor::RefreshEnabled()
{
	bool bNewEnabled = false;
	if (UCombatForgeGameInstance* GI = GameInstance.Get())
	{
		if (const APlayerController* PC = GI->GetFirstLocalPlayerController())
		{
			bNewEnabled = PC->ShouldShowMouseCursor();
		}
	}

	if (bNewEnabled != bEnabled)
	{
		bEnabled = bNewEnabled;
		// Entering or leaving cursor mode: drop any half-deflected stick state so the
		// cursor doesn't coast, and re-arm the lobby toggle to its default (cursor).
		ClearAnalogValues();
		CurrentSpeed = FVector2D::ZeroVector;
		ScrollAccum = 0.f;
		bPlayMode = false;
	}
}

bool FPFGamepadCursor::CanTogglePlayMode() const
{
	// Only the Lobby mixes live gameplay (warm-up pen) with a cursor-driven panel. The
	// boot menu and the options overlay are pure UI — D-pad down must not flip the pad
	// back to a game that isn't interactable behind them.
	UCombatForgeGameInstance* GI = GameInstance.Get();
	if (GI == nullptr)
	{
		return false;
	}
	const UWorld* World = GI->GetWorld();
	const ACombatForgeGameState* GS = World ? World->GetGameState<ACombatForgeGameState>() : nullptr;
	if (GS == nullptr || GS->Phase != EPFMatchPhase::Lobby)
	{
		return false;
	}
	const ACombatForgePlayerController* PC = Cast<ACombatForgePlayerController>(GI->GetFirstLocalPlayerController());
	return PC != nullptr && !PC->IsOptionsMenuOpen() && !PC->IsBootMenuActive();
}

void FPFGamepadCursor::Tick(const float DeltaTime, FSlateApplication& SlateApp, TSharedRef<ICursor> Cursor)
{
	RefreshEnabled();

	if (!IsCursorActive())
	{
		return;
	}

	FAnalogCursor::Tick(DeltaTime, SlateApp, Cursor);

	// Right stick = mouse wheel over whatever the cursor hovers (stored Y is pre-negated
	// by the base class, so stick-up is negative here; wheel-up is a positive delta).
	const FVector2D RightStick = GetAnalogValues(EAnalogStick::Right);
	if (FMath::Abs(RightStick.Y) > ScrollStickDeadZone)
	{
		ScrollAccum += (-RightStick.Y) * ScrollNotchesPerSec * DeltaTime;
	}
	if (FMath::Abs(ScrollAccum) > 0.05f)
	{
		EmitScroll(SlateApp, ScrollAccum);
		ScrollAccum = 0.f;
	}
}

bool FPFGamepadCursor::HandleKeyDownEvent(FSlateApplication& SlateApp, const FKeyEvent& InKeyEvent)
{
	const FKey Key = InKeyEvent.GetKey();
	if (Key.IsGamepadKey())
	{
		FPFInputDevice::NoteGamepad();
	}
	else
	{
		FPFInputDevice::NoteKeyboardMouse();
	}

	if (!bEnabled || !Key.IsGamepadKey() || !IsRelevantInput(InKeyEvent))
	{
		return false;
	}

	// Lobby: D-pad down flips between cursor mode and pen play. Handled in BOTH modes so
	// it can always flip back; consumed so it never reaches IA_PlantBomb.
	if (Key == EKeys::Gamepad_DPad_Down && CanTogglePlayMode())
	{
		if (!InKeyEvent.IsRepeat())
		{
			bPlayMode = !bPlayMode;
			ClearAnalogValues();
			CurrentSpeed = FVector2D::ZeroVector;
			UE_LOG(CombatForgeLog, Log, TEXT("GamepadCursor: lobby %s mode"), bPlayMode ? TEXT("PLAY") : TEXT("CURSOR"));
		}
		return true;
	}

	if (bPlayMode)
	{
		return false;   // pad belongs to the game (warm-up pen)
	}

	if (Key == EKeys::Gamepad_FaceButton_Bottom)
	{
		SynthesizeAcceptKey(SlateApp, InKeyEvent, /*bDown=*/true);
		return true;
	}
	if (Key == EKeys::Gamepad_FaceButton_Right)
	{
		if (!InKeyEvent.IsRepeat())
		{
			SynthesizeEscapeKey(SlateApp, /*bDown=*/true);
		}
		return true;
	}

	// Sticks-as-buttons etc. — let the base consume what it owns…
	if (FAnalogCursor::HandleKeyDownEvent(SlateApp, InKeyEvent))
	{
		return true;
	}
	// …and swallow every remaining gamepad key so nothing leaks into gameplay actions
	// behind the open menu (RT must not fire through the options overlay).
	return true;
}

bool FPFGamepadCursor::HandleKeyUpEvent(FSlateApplication& SlateApp, const FKeyEvent& InKeyEvent)
{
	const FKey Key = InKeyEvent.GetKey();
	if (!bEnabled || !Key.IsGamepadKey() || !IsRelevantInput(InKeyEvent))
	{
		return false;
	}
	if (Key == EKeys::Gamepad_DPad_Down && CanTogglePlayMode())
	{
		return true;   // matching down was ours
	}
	if (bPlayMode)
	{
		return false;
	}
	if (Key == EKeys::Gamepad_FaceButton_Bottom)
	{
		SynthesizeAcceptKey(SlateApp, InKeyEvent, /*bDown=*/false);
		return true;
	}
	if (Key == EKeys::Gamepad_FaceButton_Right)
	{
		if (!InKeyEvent.IsRepeat())
		{
			SynthesizeEscapeKey(SlateApp, /*bDown=*/false);
		}
		return true;
	}
	if (FAnalogCursor::HandleKeyUpEvent(SlateApp, InKeyEvent))
	{
		return true;
	}
	return true;
}

bool FPFGamepadCursor::HandleAnalogInputEvent(FSlateApplication& SlateApp, const FAnalogInputEvent& InAnalogInputEvent)
{
	if (FMath::Abs(InAnalogInputEvent.GetAnalogValue()) > 0.25f &&
		InAnalogInputEvent.GetKey().IsGamepadKey())
	{
		FPFInputDevice::NoteGamepad();
	}

	if (!IsCursorActive() || !IsRelevantInput(InAnalogInputEvent))
	{
		return false;
	}
	// Base stores left+right stick values (cursor + scroll) and consumes them so the
	// camera doesn't spin under an open menu; trigger axes fall through to the game,
	// where the trigger BUTTON keys are already swallowed above.
	return FAnalogCursor::HandleAnalogInputEvent(SlateApp, InAnalogInputEvent);
}

bool FPFGamepadCursor::HandleMouseMoveEvent(FSlateApplication& SlateApp, const FPointerEvent& MouseEvent)
{
	// Real mouse motion hands hint priority back to keyboard+mouse.
	if (!MouseEvent.GetCursorDelta().IsNearlyZero())
	{
		FPFInputDevice::NoteKeyboardMouse();
	}
	return false;
}

void FPFGamepadCursor::SynthesizeAcceptKey(FSlateApplication& SlateApp, const FKeyEvent& SourceEvent, bool bDown)
{
	// FAnalogCursor's click path listens for Virtual_Accept, which Windows never sends
	// for a pad — translate the A button into it and reuse the base implementation
	// (synthesized left-mouse press/release at the cursor position).
	const FKeyEvent AcceptEvent(
		EKeys::Virtual_Accept,
		SourceEvent.GetModifierKeys(),
		SourceEvent.GetUserIndex(),
		SourceEvent.IsRepeat(),
		0, 0);
	if (bDown)
	{
		FAnalogCursor::HandleKeyDownEvent(SlateApp, AcceptEvent);
	}
	else
	{
		FAnalogCursor::HandleKeyUpEvent(SlateApp, AcceptEvent);
	}
}

void FPFGamepadCursor::SynthesizeEscapeKey(FSlateApplication& SlateApp, bool bDown)
{
	// B = menu back. Escape already routes correctly everywhere a menu is open (widget
	// key handlers in UIOnly, IA_MenuBack via IMC_Common in GameAndUI), so a synthetic
	// Escape press is the smallest faithful translation.
	const FKeyEvent EscapeEvent(EKeys::Escape, FModifierKeysState(), GetOwnerUserIndex(), false, 0, 0);
	if (bDown)
	{
		SlateApp.ProcessKeyDownEvent(EscapeEvent);
	}
	else
	{
		SlateApp.ProcessKeyUpEvent(EscapeEvent);
	}
	// ProcessKey* re-enters input preprocessors with a non-gamepad Escape event. Restore
	// the real source device so pressing B does not flip all UI hints back to keyboard.
	FPFInputDevice::NoteGamepad();
}

void FPFGamepadCursor::EmitScroll(FSlateApplication& SlateApp, float NotchDelta)
{
	if (TSharedPtr<FSlateUser> SlateUser = SlateApp.GetUser(GetOwnerUserIndex()))
	{
		const bool bIsPrimaryUser = FSlateApplication::CursorUserIndex == SlateUser->GetUserIndex();
		FPointerEvent WheelEvent(
			SlateUser->GetUserIndex(),
			FSlateApplication::CursorPointerIndex,
			SlateUser->GetCursorPosition(),
			SlateUser->GetCursorPosition(),
			bIsPrimaryUser ? SlateApp.GetPressedMouseButtons() : FTouchKeySet::EmptySet,
			EKeys::MouseWheelAxis,
			NotchDelta,
			bIsPrimaryUser ? SlateApp.GetModifierKeys() : FModifierKeysState());
		SlateApp.ProcessMouseWheelOrGestureEvent(WheelEvent, nullptr);
	}
}
