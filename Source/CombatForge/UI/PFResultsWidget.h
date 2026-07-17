// Copyright (c) 2026 Tom Chapman. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "PFResultsWidget.generated.h"

class ACombatForgeGameState;
class UButton;
class USizeBox;
class UTextBlock;
class UVerticalBox;

/**
 * Results panel (contract §3.6; 15 s Results phase):
 *  - winner banner + final score (round wins / tags / captures / control points / FFA tags)
 *  - mode subtitle (Elimination / CTF / Dom / …) + first-to-N when applicable
 *  - MVP = highest MatchScore (FFA: TagCount), eliminations tiebreak
 *  - thumb tally + top liked / top disliked category from the replicated VoteTally
 *  - arena fingerprint line (computed client-side from the replicated grid)
 *  - host-only "Return to Lobby" button -> PC->ServerHostReturnToLobby (server re-validates)
 */
UCLASS()
class COMBATFORGE_API UPFResultsWidget : public UUserWidget
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
	void HandleMatchLeaderChanged();   // RETURN button follows a migrating leader (dedicated)

	void RefreshAll();
	void RefreshResult(const ACombatForgeGameState& GS);
	void RefreshMVP(const ACombatForgeGameState& GS);
	void RefreshScoreboard(const ACombatForgeGameState& GS);
	void RefreshTally(const ACombatForgeGameState& GS);
	void RefreshArenaId(const ACombatForgeGameState& GS);

	bool IsLocalHost() const;

	UPROPERTY() TObjectPtr<UTextBlock> WinnerText;
	/** Match type + unit (e.g. "CAPTURE THE FLAG · captures · first to 3"). */
	UPROPERTY() TObjectPtr<UTextBlock> ModeText;
	UPROPERTY() TObjectPtr<UTextBlock> ScoreText;
	UPROPERTY() TObjectPtr<UTextBlock> MVPText;
	/** After-action scoreboard: a header + a fixed pool of per-player rows (team-colored). */
	UPROPERTY() TObjectPtr<UTextBlock> ScoreboardHeader;
	UPROPERTY() TArray<TObjectPtr<UTextBlock>> ScoreboardRows;
	UPROPERTY() TObjectPtr<UTextBlock> TallyText;
	UPROPERTY() TObjectPtr<UTextBlock> CategoryText;
	UPROPERTY() TObjectPtr<UTextBlock> ArenaIdText;
	UPROPERTY() TObjectPtr<UTextBlock> CountdownText;
	UPROPERTY() TObjectPtr<UButton> ReturnButton;
	UPROPERTY() TObjectPtr<USizeBox> ReturnBox;   // visibility target (host-only button)

	TWeakObjectPtr<ACombatForgeGameState> BoundGameState;

	/** Fingerprint cache — SHA1 over the replicated grid, computed once per MatchId. */
	FString CachedArenaId;
	FString CachedArenaIdMatch;

	float PollAccum = 0.f;
	static constexpr float PollInterval = 0.5f;
	static constexpr int32 ScoreboardRowCount = 12;   // max match roster (PFGrid::MaxRosterSlots)
};
