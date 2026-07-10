// Copyright (c) 2026 Tom Chapman. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "PFResultsWidget.generated.h"

class APaintForgeGameState;
class UButton;
class USizeBox;
class UTextBlock;

/**
 * Results panel (contract §3.6; 15 s Results phase):
 *  - winner banner + final score from GameState TeamRoundWins
 *  - MVP = highest PlayerState MatchScore, eliminations tiebreak
 *  - thumb tally + top liked / top disliked category from the replicated VoteTally
 *    (OnVoteTallyChangedEvent)
 *  - arena fingerprint line (computed client-side from the replicated grid)
 *  - host-only "Return to Lobby" button -> PC->ServerHostReturnToLobby (server re-validates)
 */
UCLASS()
class PAINTFORGE_API UPFResultsWidget : public UUserWidget
{
	GENERATED_BODY()

protected:
	virtual TSharedRef<SWidget> RebuildWidget() override;
	virtual void NativeConstruct() override;
	virtual void NativeDestruct() override;
	virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;

	UFUNCTION() void HandleReturnClicked();

private:
	void BuildTree();
	void TryBindGameState();
	void HandleVoteTallyChanged();

	void RefreshAll();
	void RefreshResult(const APaintForgeGameState& GS);
	void RefreshMVP(const APaintForgeGameState& GS);
	void RefreshTally(const APaintForgeGameState& GS);
	void RefreshArenaId(const APaintForgeGameState& GS);

	bool IsLocalHost() const;

	UPROPERTY() TObjectPtr<UTextBlock> WinnerText;
	UPROPERTY() TObjectPtr<UTextBlock> ScoreText;
	UPROPERTY() TObjectPtr<UTextBlock> MVPText;
	UPROPERTY() TObjectPtr<UTextBlock> TallyText;
	UPROPERTY() TObjectPtr<UTextBlock> CategoryText;
	UPROPERTY() TObjectPtr<UTextBlock> ArenaIdText;
	UPROPERTY() TObjectPtr<UTextBlock> CountdownText;
	UPROPERTY() TObjectPtr<UButton> ReturnButton;
	UPROPERTY() TObjectPtr<USizeBox> ReturnBox;   // visibility target (host-only button)

	TWeakObjectPtr<APaintForgeGameState> BoundGameState;

	/** Fingerprint cache — SHA1 over the replicated grid, computed once per MatchId. */
	FString CachedArenaId;
	FString CachedArenaIdMatch;

	float PollAccum = 0.f;
	static constexpr float PollInterval = 0.5f;
};
