// Copyright (c) 2026 Tom Chapman. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "PFScoreboardWidget.generated.h"

class APaintForgePlayerState;
class UTextBlock;
class UVerticalBox;

/**
 * Hold-Tab scoreboard overlay (contract §3.6): one row per PlayerState — name, team color chip,
 * eliminations, times eliminated, match score, alive dot — under the team round-win score line.
 * Visibility is driven by UPFRootHUDWidget from the PC's IA_Scoreboard hold state
 * (IsScoreboardHeld / OnScoreboardHeldChanged); while visible the roster is re-polled every
 * 0.5 s (PlayerArray has no delegate). Display-only: the whole overlay is hit-test invisible.
 */
UCLASS()
class PAINTFORGE_API UPFScoreboardWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	/** Immediate refresh — UPFRootHUDWidget calls this when the overlay is shown (intra-package). */
	void RefreshNow();

protected:
	virtual TSharedRef<SWidget> RebuildWidget() override;
	virtual void NativeOnInitialized() override;
	virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;

private:
	void BuildTree();
	void RefreshRows();
	void AddHeaderRow();
	void AddRow(const APaintForgePlayerState* PS);

	UPROPERTY() TObjectPtr<UTextBlock> WinsAText;
	UPROPERTY() TObjectPtr<UTextBlock> WinsBText;
	UPROPERTY() TObjectPtr<UTextBlock> RoundText;
	UPROPERTY() TObjectPtr<UVerticalBox> RowsBox;

	float PollAccum = 0.f;
	/** Cheap change detection so rows only rebuild when something visible changed. */
	FString LastSignature;

	static constexpr float PollInterval = 0.5f;
	static constexpr float ChipSizePx = 16.f;
	static constexpr float DotSizePx = 12.f;
	static constexpr float NumColWidthPx = 70.f;
	static constexpr float ScoreColWidthPx = 90.f;
};
