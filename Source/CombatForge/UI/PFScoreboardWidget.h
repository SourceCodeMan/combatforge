// Copyright (c) 2026 Tom Chapman. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "PFScoreboardWidget.generated.h"

class ACombatForgePlayerState;
class UTextBlock;
class UVerticalBox;

/**
 * Hold-Tab scoreboard overlay (contract §3.6): one row per PlayerState — name, team color chip,
 * eliminations, times eliminated, match score (or FFA tags), alive dot — under the mode score line.
 * Headers adapt to Elimination / Skirmish / CTF / Dom / HP / FFA. FLAG / on-point markers for
 * objectives. Visibility from UPFRootHUDWidget (Tab hold); roster polled every 0.5 s.
 */
UCLASS()
class COMBATFORGE_API UPFScoreboardWidget : public UUserWidget
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
	void AddRow(const ACombatForgePlayerState* PS);

	UPROPERTY() TObjectPtr<UTextBlock> WinsAText;
	UPROPERTY() TObjectPtr<UTextBlock> WinsBText;
	UPROPERTY() TObjectPtr<UTextBlock> RoundText;
	/** "TAGS" / "CAPTURES" / "POINTS" / "ROUND WINS" under the big score numbers. */
	UPROPERTY() TObjectPtr<UTextBlock> ScoreUnitText;
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
