// Copyright (c) 2026 Tom Chapman. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/ObjectPtr.h"

class ACombatForgeGameState;
class UBorder;
class UTextBlock;
class UWidget;
class UWidgetTree;

/**
 * Shared 3-segment match-rotation bar: 1 CREATIVE | 2 REMIX | 3 REMIX SWAP (Tom 2026-07-24 —
 * "a box divided into thirds ... highlighted and filled depending on where they are").
 * Segments up to the current stage are filled, the current one brightest; hidden entirely while
 * the wheel is inactive (PlayOnly / FreeForAll). Pure C++ UMG helper: the HOST widget owns the
 * created children in UPROPERTY arrays (GC ownership rides its widget tree) and calls Update
 * from its own refresh/tick. Embedded in the Tab scoreboard, the Esc options menu, and the lobby.
 */
namespace PFCycleBar
{
	/** Build the bar row into Tree; returns the root to slot anywhere. Segment widgets are
	 *  appended to the caller's UPROPERTY arrays. */
	UWidget* Build(UWidgetTree* Tree,
		TArray<TObjectPtr<UBorder>>& OutSegs, TArray<TObjectPtr<UTextBlock>>& OutTexts);

	/** Re-tint from GameState; collapses BarRoot when the wheel is inactive. Cheap — safe per-tick. */
	void Update(const ACombatForgeGameState* GS, UWidget* BarRoot,
		const TArray<TObjectPtr<UBorder>>& Segs, const TArray<TObjectPtr<UTextBlock>>& Texts);
}
