// Copyright (c) 2026 Tom Chapman. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Components/Button.h"
#include "Core/CombatForgeTypes.h"
#include "PFVoteWidget.generated.h"

class UPFVoteWidget;
class UTextBlock;
class UWidgetSwitcher;

/**
 * Internal category-chip button: carries its category index (0..7) so one
 * handler can cycle its state. Intra-package helper — not part of the
 * cross-package contract.
 */
UCLASS()
class COMBATFORGE_API UPFVoteChipButton : public UButton
{
	GENERATED_BODY()

public:
	void InitChip(UPFVoteWidget* InOwner, int32 InChipIndex);

protected:
	UFUNCTION()
	void HandleChipClicked();

private:
	TWeakObjectPtr<UPFVoteWidget> OwnerWidget;
	int32 ChipIndex = INDEX_NONE;
};

/**
 * VotePhase widget (contract §3.6, 01 §4.2):
 *  - Step 1: thumbs up/down (required; advances instantly)
 *  - Step 2: 8 chips (PFVoteCategories::All + HintText); click cycles
 *    neutral -> liked -> disliked -> neutral; <= 4 total selections (T30);
 *    SUBMIT live immediately
 *  - countdown ring from GameState::GetPhaseTimeRemaining / PhaseDuration
 *  - timeout/submit -> PC->ServerSubmitVote; no thumb at timeout = Abstained
 *  - exactly one submission per player (widget disables after send)
 */
UCLASS()
class COMBATFORGE_API UPFVoteWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	/** UPFRootHUDWidget calls on VotePhase entry: clears the previous match's vote. */
	void ResetForVote();

	/** Chip-button callback target (index 0..7). */
	void NotifyChipClicked(int32 ChipIndex);

protected:
	virtual TSharedRef<SWidget> RebuildWidget() override;
	virtual void NativeConstruct() override;
	virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;
	virtual int32 NativePaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry,
		const FSlateRect& MyCullingRect, FSlateWindowElementList& OutDrawElements, int32 LayerId,
		const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const override;

	UFUNCTION() void HandleThumbUpClicked();
	UFUNCTION() void HandleThumbDownClicked();
	UFUNCTION() void HandleSubmitClicked();

private:
	void HandleThumbClicked(EPFThumbVote Vote);
	/** Chip cycle state — mirrors the vote wire semantics. */
	enum class EChipState : uint8 { Neutral = 0, Liked = 1, Disliked = 2 };

	void BuildTree();
	void SubmitVote();
	void UpdateChipVisual(int32 ChipIndex);
	void UpdateSelectionCount();
	/** Hand Slate keyboard focus back to the viewport after a click so Tab/Esc keep working. */
	void ReturnFocusToGame();
	int32 CountSelections() const;

	UPROPERTY() TObjectPtr<UWidgetSwitcher> StepSwitcher;
	UPROPERTY() TObjectPtr<UTextBlock> TimerText;
	UPROPERTY() TObjectPtr<UTextBlock> StatusText;
	UPROPERTY() TObjectPtr<UTextBlock> SelectionCountText;
	UPROPERTY() TObjectPtr<UButton> ThumbUpButton;
	UPROPERTY() TObjectPtr<UButton> ThumbDownButton;
	UPROPERTY() TObjectPtr<UButton> SubmitButton;
	UPROPERTY() TArray<TObjectPtr<UPFVoteChipButton>> ChipButtons;
	UPROPERTY() TArray<TObjectPtr<UTextBlock>> ChipLabels;

	TArray<EChipState> ChipStates;
	EPFThumbVote Thumb = EPFThumbVote::Abstained;
	bool bSubmitted = false;

	static constexpr int32 NumChips = 8;
	static constexpr int32 MaxSelections = 4;   // liked + disliked combined (T30)
	/** Fallback only if PhaseDuration has not replicated yet (T3 default = 20 s). */
	static constexpr float VoteDurationFallback = 20.f;
	/**
	 * Timeout auto-submit fires this many seconds BEFORE the phase deadline so the
	 * ServerSubmitVote RPC lands before the server's FinalizeVotePhase timer marks
	 * non-voters Abstained (an RPC sent at exactly 0 always loses that race).
	 */
	static constexpr float AutoSubmitLeadSeconds = 0.5f;
	static constexpr float RingRadiusPx = 34.f;
	static constexpr float RingCenterY = 70.f;
};
