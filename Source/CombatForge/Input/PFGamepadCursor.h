// Copyright (c) 2026 Tom Chapman. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "Framework/Application/AnalogCursor.h"
#include "UObject/WeakObjectPtr.h"

class UCombatForgeGameInstance;

/**
 * Last-used input device tracker, fed by FPFGamepadCursor (which sees every input event
 * as a Slate preprocessor). Widgets read it to swap key hints ("hold Q" vs "hold LB").
 */
struct COMBATFORGE_API FPFInputDevice
{
	static bool IsGamepadPrimary() { return bGamepadPrimary; }
	static void NoteGamepad()       { bGamepadPrimary = true; }
	static void NoteKeyboardMouse() { bGamepadPrimary = false; }

private:
	static bool bGamepadPrimary;
};

/**
 * Gamepad-driven UI cursor (Slate input preprocessor, client-only).
 *
 * Every CombatForge menu is a mouse-click UMG tree, so instead of retrofitting focus
 * navigation onto each widget, the gamepad drives the EXISTING mouse pipeline:
 * whenever the local PlayerController shows the mouse cursor (boot menu, lobby, options,
 * vote, results, Tab-hold), this preprocessor turns the pad into a pointer —
 *
 *   left stick    move the cursor (FAnalogCursor accelerated movement)
 *   A             click (synthesized left-mouse down/up at the cursor)
 *   B             Escape (menu back / close options)
 *   right stick   scroll the hovered list (server browser, settings pages)
 *   D-pad down    LOBBY ONLY: toggle "play mode" — hand the pad back to the game to
 *                 walk/shoot the warm-up pen; D-pad down again returns to the cursor
 *
 * While the cursor is active every other gamepad key is consumed, so pad input can
 * never leak into gameplay actions behind an open menu (e.g. RT firing the weapon
 * while the options overlay is up). When the cursor is hidden (normal gameplay) the
 * preprocessor is inert and passes everything through untouched.
 *
 * Registered once by UCombatForgeGameInstance on clients (never on dedicated servers
 * or commandlets). Zero replication.
 */
class COMBATFORGE_API FPFGamepadCursor : public FAnalogCursor
{
public:
	explicit FPFGamepadCursor(UCombatForgeGameInstance* InGameInstance);

	virtual void Tick(const float DeltaTime, FSlateApplication& SlateApp, TSharedRef<ICursor> Cursor) override;
	virtual bool HandleKeyDownEvent(FSlateApplication& SlateApp, const FKeyEvent& InKeyEvent) override;
	virtual bool HandleKeyUpEvent(FSlateApplication& SlateApp, const FKeyEvent& InKeyEvent) override;
	virtual bool HandleAnalogInputEvent(FSlateApplication& SlateApp, const FAnalogInputEvent& InAnalogInputEvent) override;
	virtual bool HandleMouseMoveEvent(FSlateApplication& SlateApp, const FPointerEvent& MouseEvent) override;
	virtual const TCHAR* GetDebugName() const override { return TEXT("PFGamepadCursor"); }

private:
	/** True while the pad should act as a pointer (cursor shown + not lobby play mode). */
	bool IsCursorActive() const { return bEnabled && !bPlayMode; }

	/** Re-resolve enablement from the local PC's cursor state (cached once per Tick). */
	void RefreshEnabled();

	/** Lobby-only: is the D-pad-down play/cursor toggle currently allowed? */
	bool CanTogglePlayMode() const;

	void SynthesizeAcceptKey(FSlateApplication& SlateApp, const FKeyEvent& SourceEvent, bool bDown);
	void SynthesizeEscapeKey(FSlateApplication& SlateApp, bool bDown);
	void EmitScroll(FSlateApplication& SlateApp, float NotchDelta);

	TWeakObjectPtr<UCombatForgeGameInstance> GameInstance;

	bool bEnabled = false;      // cursor visible on the local PC (refreshed per Tick)
	bool bPlayMode = false;     // lobby: pad handed back to gameplay
	float ScrollAccum = 0.f;    // fractional wheel notches pending emission
};
