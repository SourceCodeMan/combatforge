// Copyright (c) 2026 Tom Chapman. All rights reserved.

#include "UI/PFResultsWidget.h"

#include "Building/PFArenaSerialization.h"
#include "Building/PFBuildGrid.h"
#include "Core/CombatForgeGameState.h"
#include "Core/CombatForgePlayerController.h"
#include "Core/CombatForgePlayerState.h"

#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/Button.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/SizeBox.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "GameFramework/PlayerState.h"
#include "Kismet/GameplayStatics.h"
#include "Styling/CoreStyle.h"

namespace
{
	FSlateFontInfo PFResultsFont(int32 Size, bool bBold)
	{
		return FCoreStyle::GetDefaultFontStyle(bBold ? FName(TEXT("Bold")) : FName(TEXT("Regular")), Size);
	}
}

TSharedRef<SWidget> UPFResultsWidget::RebuildWidget()
{
	if (WidgetTree && !WidgetTree->RootWidget)
	{
		BuildTree(); // root assigned BEFORE Super::RebuildWidget (02 R3)
	}
	return Super::RebuildWidget();
}

void UPFResultsWidget::BuildTree()
{
	UCanvasPanel* RootCanvas = WidgetTree->ConstructWidget<UCanvasPanel>();
	WidgetTree->RootWidget = RootCanvas;

	// Full-screen dim so the results read as modal (input mode is UIOnly here — §4.5).
	UBorder* Dim = WidgetTree->ConstructWidget<UBorder>();
	Dim->SetBrushColor(FLinearColor(0.f, 0.f, 0.f, 0.6f));
	if (UCanvasPanelSlot* CSlot = RootCanvas->AddChildToCanvas(Dim))
	{
		CSlot->SetAnchors(FAnchors(0.f, 0.f, 1.f, 1.f));
		CSlot->SetOffsets(FMargin(0.f));
	}

	UVerticalBox* Body = WidgetTree->ConstructWidget<UVerticalBox>();

	auto AddCentered = [this, Body](TObjectPtr<UTextBlock>& OutText, int32 FontSize, bool bBold,
	                                const FLinearColor& Color, const FMargin& SlotPadding)
	{
		OutText = WidgetTree->ConstructWidget<UTextBlock>();
		OutText->SetFont(PFResultsFont(FontSize, bBold));
		OutText->SetColorAndOpacity(FSlateColor(Color));
		OutText->SetJustification(ETextJustify::Center);
		if (UVerticalBoxSlot* VSlot = Body->AddChildToVerticalBox(OutText))
		{
			VSlot->SetHorizontalAlignment(HAlign_Center);
			VSlot->SetPadding(SlotPadding);
		}
	};

	AddCentered(WinnerText, 38, true, FLinearColor::White, FMargin(0.f, 0.f, 0.f, 4.f));
	AddCentered(ModeText, 14, true, FLinearColor(1.f, 1.f, 1.f, 0.55f), FMargin(0.f, 0.f, 0.f, 8.f));
	AddCentered(ScoreText, 18, false, FLinearColor(1.f, 1.f, 1.f, 0.85f), FMargin(0.f, 0.f, 0.f, 20.f));
	AddCentered(MVPText, 18, true, FLinearColor(1.f, 0.85f, 0.2f), FMargin(0.f, 0.f, 0.f, 12.f));

	// --- After-action scoreboard: header + fixed pool of per-player rows (team-colored, monospace) ---
	ScoreboardHeader = WidgetTree->ConstructWidget<UTextBlock>();
	ScoreboardHeader->SetFont(FCoreStyle::GetDefaultFontStyle(FName("Mono"), 12));
	ScoreboardHeader->SetColorAndOpacity(FSlateColor(FLinearColor(1.f, 1.f, 1.f, 0.5f)));
	ScoreboardHeader->SetJustification(ETextJustify::Center);
	if (UVerticalBoxSlot* VSlot = Body->AddChildToVerticalBox(ScoreboardHeader))
	{
		VSlot->SetHorizontalAlignment(HAlign_Center);
		VSlot->SetPadding(FMargin(0.f, 0.f, 0.f, 2.f));
	}
	ScoreboardRows.Reset();
	for (int32 i = 0; i < ScoreboardRowCount; ++i)
	{
		UTextBlock* Row = WidgetTree->ConstructWidget<UTextBlock>();
		Row->SetFont(FCoreStyle::GetDefaultFontStyle(FName("Mono"), 14));
		Row->SetJustification(ETextJustify::Center);
		Row->SetVisibility(ESlateVisibility::Collapsed);
		if (UVerticalBoxSlot* VSlot = Body->AddChildToVerticalBox(Row))
		{
			VSlot->SetHorizontalAlignment(HAlign_Center);
			VSlot->SetPadding(FMargin(0.f, 1.f, 0.f, (i == ScoreboardRowCount - 1) ? 18.f : 1.f));
		}
		ScoreboardRows.Add(Row);
	}

	AddCentered(TallyText, 22, true, FLinearColor::White, FMargin(0.f, 0.f, 0.f, 4.f));
	AddCentered(CategoryText, 14, false, FLinearColor(1.f, 1.f, 1.f, 0.7f), FMargin(0.f, 0.f, 0.f, 14.f));
	AddCentered(ArenaIdText, 12, false, FLinearColor(1.f, 1.f, 1.f, 0.45f), FMargin(0.f, 0.f, 0.f, 22.f));

	// Host-only return button (server re-validates host-ness — §5 R13).
	ReturnButton = WidgetTree->ConstructWidget<UButton>();
	ReturnButton->SetBackgroundColor(FLinearColor(0.12f, 0.35f, 0.75f));
	ReturnButton->OnClicked.AddUniqueDynamic(this, &UPFResultsWidget::HandleReturnClicked);
	UTextBlock* ReturnLabel = WidgetTree->ConstructWidget<UTextBlock>();
	ReturnLabel->SetText(FText::FromString(TEXT("RETURN TO LOBBY")));
	ReturnLabel->SetFont(PFResultsFont(16, true));
	ReturnLabel->SetColorAndOpacity(FSlateColor(FLinearColor::White));
	ReturnLabel->SetJustification(ETextJustify::Center);
	ReturnButton->SetContent(ReturnLabel);
	ReturnBox = WidgetTree->ConstructWidget<USizeBox>();
	ReturnBox->SetWidthOverride(260.f);
	ReturnBox->SetHeightOverride(52.f);
	ReturnBox->SetContent(ReturnButton);
	if (UVerticalBoxSlot* VSlot = Body->AddChildToVerticalBox(ReturnBox))
	{
		VSlot->SetHorizontalAlignment(HAlign_Center);
		VSlot->SetPadding(FMargin(0.f, 4.f));
	}

	if (UCanvasPanelSlot* CSlot = RootCanvas->AddChildToCanvas(Body))
	{
		CSlot->SetAnchors(FAnchors(0.5f, 0.5f));
		CSlot->SetAlignment(FVector2D(0.5f, 0.5f));
		CSlot->SetPosition(FVector2D(0.f, -20.f));
		CSlot->SetAutoSize(true);
	}

	// Phase countdown — bottom center.
	CountdownText = WidgetTree->ConstructWidget<UTextBlock>();
	CountdownText->SetFont(PFResultsFont(14, false));
	CountdownText->SetColorAndOpacity(FSlateColor(FLinearColor(1.f, 1.f, 1.f, 0.6f)));
	CountdownText->SetJustification(ETextJustify::Center);
	if (UCanvasPanelSlot* CSlot = RootCanvas->AddChildToCanvas(CountdownText))
	{
		CSlot->SetAnchors(FAnchors(0.5f, 1.f));
		CSlot->SetAlignment(FVector2D(0.5f, 1.f));
		CSlot->SetPosition(FVector2D(0.f, -48.f));
		CSlot->SetAutoSize(true);
	}
}

void UPFResultsWidget::NativeConstruct()
{
	Super::NativeConstruct();
	TryBindGameState();
	if (ReturnBox)
	{
		// Initial paint; HandleMatchLeaderChanged re-styles if leadership migrates (dedicated).
		ReturnBox->SetVisibility(IsLocalHost() ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
	}
	PollAccum = PollInterval; // refresh on first visible tick
	RefreshAll();
}

void UPFResultsWidget::NativeDestruct()
{
	if (BoundGameState.IsValid())
	{
		BoundGameState->OnVoteTallyChangedEvent.RemoveAll(this);
		BoundGameState->OnMatchLeaderChangedEvent.RemoveAll(this);
		BoundGameState.Reset();
	}
	Super::NativeDestruct();
}

void UPFResultsWidget::TryBindGameState()
{
	if (BoundGameState.IsValid())
	{
		return;
	}
	const UWorld* World = GetWorld();
	ACombatForgeGameState* GS = World ? World->GetGameState<ACombatForgeGameState>() : nullptr;
	if (GS)
	{
		BoundGameState = GS;
		GS->OnVoteTallyChangedEvent.AddUObject(this, &UPFResultsWidget::HandleVoteTallyChanged);
		// Leadership can migrate mid-Results on a dedicated server (the leader quit) — the RETURN
		// button must land on the new leader, not vanish with the old one.
		GS->OnMatchLeaderChangedEvent.AddUObject(this, &UPFResultsWidget::HandleMatchLeaderChanged);
	}
}

void UPFResultsWidget::HandleMatchLeaderChanged()
{
	if (ReturnBox)
	{
		ReturnBox->SetVisibility(IsLocalHost() ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
	}
}

void UPFResultsWidget::HandleVoteTallyChanged()
{
	if (const ACombatForgeGameState* GS = BoundGameState.Get())
	{
		RefreshTally(*GS);
	}
}

bool UPFResultsWidget::IsLocalHost() const
{
	// Match-leader aware (dedicated servers) — see ACombatForgePlayerController::IsHostController.
	const ACombatForgePlayerController* PC = Cast<ACombatForgePlayerController>(GetOwningPlayer());
	return PC && PC->IsHostController();
}

void UPFResultsWidget::HandleReturnClicked()
{
	// UX gate only — the RPC body re-validates host identity server-side (§5 R13).
	if (ACombatForgePlayerController* PC = Cast<ACombatForgePlayerController>(GetOwningPlayer()))
	{
		PC->ServerHostReturnToLobby();
	}
}

void UPFResultsWidget::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);

	TryBindGameState(); // GameState can arrive late on clients

	const ACombatForgeGameState* GS = BoundGameState.Get();
	if (!GS)
	{
		return;
	}

	if (CountdownText)
	{
		if (GS->Phase == EPFMatchPhase::Results)
		{
			const int32 Secs = FMath::Max(0, FMath::CeilToInt(GS->GetPhaseTimeRemaining()));
			CountdownText->SetText(FText::FromString(
				FString::Printf(TEXT("Back to lobby in %d"), Secs)));
		}
		else
		{
			CountdownText->SetText(FText::GetEmpty());
		}
	}

	// This panel only ticks while it is the active phase panel (Results); the poll catches
	// straggling PlayerState/score replication after the reliable phase flip.
	PollAccum += InDeltaTime;
	if (PollAccum >= PollInterval)
	{
		PollAccum = 0.f;
		RefreshAll();
	}
}

void UPFResultsWidget::RefreshAll()
{
	const ACombatForgeGameState* GS = BoundGameState.Get();
	if (!GS)
	{
		return;
	}
	RefreshResult(*GS);
	RefreshMVP(*GS);
	RefreshScoreboard(*GS);
	RefreshTally(*GS);
	RefreshArenaId(*GS);
}

void UPFResultsWidget::RefreshScoreboard(const ACombatForgeGameState& GS)
{
	if (!ScoreboardHeader)
	{
		return;
	}
	const bool bFFA = (GS.MatchType == EPFMatchType::FreeForAll);
	ScoreboardHeader->SetText(FText::FromString(FString::Printf(
		TEXT("%-14s %3s %3s %5s"), TEXT("PLAYER"), TEXT("E"), TEXT("D"), bFFA ? TEXT("TAGS") : TEXT("PTS"))));

	// Team A block, then Team B (FFA: single group); within a team, best score first, elims tiebreak.
	auto ScoreOf = [bFFA](const ACombatForgePlayerState& P) -> int32
	{
		return bFFA ? static_cast<int32>(P.TagCount) : P.MatchScore;
	};
	TArray<const ACombatForgePlayerState*> Players;
	for (APlayerState* PSBase : GS.PlayerArray)
	{
		if (const ACombatForgePlayerState* PS = Cast<ACombatForgePlayerState>(PSBase))
		{
			Players.Add(PS);
		}
	}
	Players.Sort([bFFA, &ScoreOf](const ACombatForgePlayerState& A, const ACombatForgePlayerState& B)
	{
		if (!bFFA)
		{
			const uint8 TA = (A.TeamId <= 1) ? A.TeamId : 2;
			const uint8 TB = (B.TeamId <= 1) ? B.TeamId : 2;
			if (TA != TB) { return TA < TB; }
		}
		const int32 SA = ScoreOf(A), SB = ScoreOf(B);
		if (SA != SB) { return SA > SB; }
		if (A.Eliminations != B.Eliminations) { return A.Eliminations > B.Eliminations; }
		return A.GetPlayerName() < B.GetPlayerName();
	});

	for (int32 i = 0; i < ScoreboardRows.Num(); ++i)
	{
		UTextBlock* Row = ScoreboardRows[i].Get();
		if (!Row)
		{
			continue;
		}
		if (i < Players.Num())
		{
			const ACombatForgePlayerState* P = Players[i];
			Row->SetText(FText::FromString(FString::Printf(TEXT("%-14.14s %3d %3d %5d"),
				*P->GetPlayerName(), P->Eliminations, P->TimesEliminated, ScoreOf(*P))));
			Row->SetColorAndOpacity(FSlateColor(bFFA ? FLinearColor::White : PFColors::ForTeam(P->TeamId % 2)));
			Row->SetVisibility(ESlateVisibility::HitTestInvisible);
		}
		else
		{
			Row->SetVisibility(ESlateVisibility::Collapsed);
		}
	}
}

void UPFResultsWidget::RefreshResult(const ACombatForgeGameState& GS)
{
	// Mode subtitle (always — kids-readable unit of score).
	if (ModeText)
	{
		FString Mode;
		switch (GS.MatchType)
		{
		case EPFMatchType::FreeForAll:   Mode = TEXT("FREE-FOR-ALL"); break;
		case EPFMatchType::Skirmish:     Mode = TEXT("SKIRMISH"); break;
		case EPFMatchType::CaptureFlag:  Mode = TEXT("CAPTURE THE FLAG"); break;
		case EPFMatchType::Domination:   Mode = TEXT("DOMINATION"); break;
		case EPFMatchType::Hardpoint:    Mode = TEXT("HARDPOINT"); break;
		default:                         Mode = TEXT("ELIMINATION"); break;
		}
		if (GS.RoundWinsToTake > 0
			&& (GS.MatchType == EPFMatchType::Skirmish
				|| GS.MatchType == EPFMatchType::CaptureFlag
				|| GS.MatchType == EPFMatchType::Domination
				|| GS.MatchType == EPFMatchType::Hardpoint
				|| GS.MatchType == EPFMatchType::FreeForAll
				|| GS.MatchType == EPFMatchType::Elimination))
		{
			Mode += FString::Printf(TEXT("  ·  first to %d"), GS.RoundWinsToTake);
		}
		ModeText->SetText(FText::FromString(Mode));
	}

	// FreeForAll: solo winner by TagCount.
	if (GS.MatchType == EPFMatchType::FreeForAll)
	{
		const ACombatForgePlayerState* Best = nullptr;
		uint16 BestTags = 0;
		int32 Tied = 0;
		for (APlayerState* PSBase : GS.PlayerArray)
		{
			const ACombatForgePlayerState* PS = Cast<ACombatForgePlayerState>(PSBase);
			if (!PS) { continue; }
			if (PS->TagCount > BestTags)
			{
				BestTags = PS->TagCount;
				Best = PS;
				Tied = 1;
			}
			else if (PS->TagCount == BestTags && BestTags > 0)
			{
				++Tied;
			}
		}
		if (WinnerText)
		{
			if (Tied != 1 || !Best)
			{
				WinnerText->SetText(FText::FromString(TEXT("DRAW")));
				WinnerText->SetColorAndOpacity(FSlateColor(FLinearColor::White));
			}
			else
			{
				WinnerText->SetText(FText::FromString(
					FString::Printf(TEXT("%s WINS"), *Best->GetPlayerName().ToUpper())));
				WinnerText->SetColorAndOpacity(FSlateColor(PFColors::ForTeam(Best->TeamId % 2)));
			}
		}
		if (ScoreText)
		{
			TArray<const ACombatForgePlayerState*> Ranked;
			for (APlayerState* PSBase : GS.PlayerArray)
			{
				if (const ACombatForgePlayerState* PS = Cast<ACombatForgePlayerState>(PSBase))
				{
					Ranked.Add(PS);
				}
			}
			Ranked.Sort([](const ACombatForgePlayerState& A, const ACombatForgePlayerState& B)
			{
				if (A.TagCount != B.TagCount) { return A.TagCount > B.TagCount; }
				return A.GetPlayerName() < B.GetPlayerName();
			});
			FString Board = TEXT("tags");
			const int32 N = FMath::Min(3, Ranked.Num());
			for (int32 i = 0; i < N; ++i)
			{
				Board += FString::Printf(TEXT("  ·  %d. %s %d"),
					i + 1, *Ranked[i]->GetPlayerName(), Ranked[i]->TagCount);
			}
			ScoreText->SetText(FText::FromString(Board));
		}
		return;
	}

	const bool bTeamScores = (GS.MatchType == EPFMatchType::Skirmish
		|| GS.MatchType == EPFMatchType::CaptureFlag
		|| GS.MatchType == EPFMatchType::Domination
		|| GS.MatchType == EPFMatchType::Hardpoint);
	const int32 WinsA = bTeamScores ? static_cast<int32>(GS.TeamScores[0]) : static_cast<int32>(GS.TeamRoundWins[0]);
	const int32 WinsB = bTeamScores ? static_cast<int32>(GS.TeamScores[1]) : static_cast<int32>(GS.TeamRoundWins[1]);

	if (WinnerText)
	{
		if (WinsA == WinsB)
		{
			WinnerText->SetText(FText::FromString(TEXT("DRAW")));
			WinnerText->SetColorAndOpacity(FSlateColor(FLinearColor::White));
		}
		else
		{
			const uint8 Winner = (WinsA > WinsB) ? 0 : 1;
			WinnerText->SetText(FText::FromString(
				FString::Printf(TEXT("TEAM %s WINS"), (Winner == 0) ? TEXT("A") : TEXT("B"))));
			WinnerText->SetColorAndOpacity(FSlateColor(PFColors::ForTeam(Winner)));
		}
	}
	if (ScoreText)
	{
		// "A 3 — 1 B  ·  captures" so team colors read without the HUD strip.
		FString Unit = TEXT("round wins");
		if (GS.MatchType == EPFMatchType::Skirmish) { Unit = TEXT("tags"); }
		else if (GS.MatchType == EPFMatchType::CaptureFlag) { Unit = TEXT("captures"); }
		else if (GS.MatchType == EPFMatchType::Domination) { Unit = TEXT("control points"); }
		else if (GS.MatchType == EPFMatchType::Hardpoint) { Unit = TEXT("hardpoint score"); }

		FString Score = FString::Printf(TEXT("A  %d  —  %d  B   ·   %s"), WinsA, WinsB, *Unit);
		if (GS.MatchType == EPFMatchType::Elimination)
		{
			if (GS.RoundNumber > 0)
			{
				Score += FString::Printf(TEXT("  ·  %d rounds played"), GS.RoundNumber);
			}
			if (GS.bSuddenDeath)
			{
				Score += TEXT("  ·  showdown");
			}
		}
		ScoreText->SetText(FText::FromString(Score));
	}
}

void UPFResultsWidget::RefreshMVP(const ACombatForgeGameState& GS)
{
	if (!MVPText)
	{
		return;
	}
	// FFA MVP = most tags (matches win condition). Other modes = MatchScore + elims (T18/§3.6).
	// Name breaks exact ties so every client shows the same MVP.
	const bool bFFA = (GS.MatchType == EPFMatchType::FreeForAll);
	const ACombatForgePlayerState* Best = nullptr;
	for (APlayerState* PSBase : GS.PlayerArray)
	{
		const ACombatForgePlayerState* PS = Cast<ACombatForgePlayerState>(PSBase);
		if (!PS)
		{
			continue;
		}
		if (!Best)
		{
			Best = PS;
			continue;
		}
		if (bFFA)
		{
			if (PS->TagCount > Best->TagCount
				|| (PS->TagCount == Best->TagCount && PS->Eliminations > Best->Eliminations)
				|| (PS->TagCount == Best->TagCount && PS->Eliminations == Best->Eliminations
					&& PS->GetPlayerName() < Best->GetPlayerName()))
			{
				Best = PS;
			}
		}
		else if (PS->MatchScore > Best->MatchScore
			|| (PS->MatchScore == Best->MatchScore && PS->Eliminations > Best->Eliminations)
			|| (PS->MatchScore == Best->MatchScore && PS->Eliminations == Best->Eliminations
				&& PS->GetPlayerName() < Best->GetPlayerName()))
		{
			Best = PS;
		}
	}
	if (!Best)
	{
		MVPText->SetText(FText::GetEmpty());
		return;
	}
	if (bFFA)
	{
		MVPText->SetText(FText::FromString(FString::Printf(
			TEXT("MVP  %s — %d tags  ·  %d elims"),
			*Best->GetPlayerName(), Best->TagCount, Best->Eliminations)));
	}
	else
	{
		MVPText->SetText(FText::FromString(FString::Printf(
			TEXT("MVP  %s — %d pts  ·  %d elims"),
			*Best->GetPlayerName(), Best->MatchScore, Best->Eliminations)));
	}
}

void UPFResultsWidget::RefreshTally(const ACombatForgeGameState& GS)
{
	const FPFVoteTally& Tally = GS.VoteTally;

	if (TallyText)
	{
		TallyText->SetText(FText::FromString(
			FString::Printf(TEXT("▲ %d    ▼ %d"), Tally.Up, Tally.Down)));
	}

	if (CategoryText)
	{
		int32 TopLikedIdx = INDEX_NONE;
		int32 TopDislikedIdx = INDEX_NONE;
		for (int32 i = 0; i < 8; ++i)
		{
			if (Tally.LikedCounts[i] > 0
				&& (TopLikedIdx == INDEX_NONE || Tally.LikedCounts[i] > Tally.LikedCounts[TopLikedIdx]))
			{
				TopLikedIdx = i;
			}
			if (Tally.DislikedCounts[i] > 0
				&& (TopDislikedIdx == INDEX_NONE || Tally.DislikedCounts[i] > Tally.DislikedCounts[TopDislikedIdx]))
			{
				TopDislikedIdx = i;
			}
		}

		FString Line;
		if (TopLikedIdx != INDEX_NONE)
		{
			Line += FString::Printf(TEXT("Most liked: %s"),
				*PFVoteCategories::FromId(static_cast<uint8>(TopLikedIdx + 1)).ToString());
		}
		if (TopDislikedIdx != INDEX_NONE)
		{
			if (!Line.IsEmpty())
			{
				Line += TEXT("   ·   ");
			}
			Line += FString::Printf(TEXT("Most disliked: %s"),
				*PFVoteCategories::FromId(static_cast<uint8>(TopDislikedIdx + 1)).ToString());
		}
		CategoryText->SetText(FText::FromString(Line));
	}
}

void UPFResultsWidget::RefreshArenaId(const ACombatForgeGameState& GS)
{
	if (!ArenaIdText)
	{
		return;
	}
	// CONTRACT-GAP: §3.6 doesn't spec a fingerprint readout on the results panel, but the pkg-ui
	// brief asks for the arena fingerprint here. Computed client-side through §3 APIs only
	// (APFBuildGrid::GetPieces + FPFArenaSerialization::ComputeArenaId) — the grid still holds
	// this match's frozen pieces until the next Lobby→Build ClearAll.
	if (GS.MatchId.IsEmpty())
	{
		ArenaIdText->SetText(FText::GetEmpty());
		return;
	}
	if (CachedArenaIdMatch != GS.MatchId || CachedArenaId.IsEmpty())
	{
		const APFBuildGrid* Grid = Cast<APFBuildGrid>(
			UGameplayStatics::GetActorOfClass(GetWorld(), APFBuildGrid::StaticClass()));
		if (Grid)
		{
			CachedArenaId = FPFArenaSerialization::ComputeArenaId(Grid->GetPieces());
			CachedArenaIdMatch = GS.MatchId;
		}
	}
	ArenaIdText->SetText(CachedArenaId.IsEmpty()
		? FText::GetEmpty()
		: FText::FromString(FString::Printf(TEXT("Arena %s"), *CachedArenaId.Left(8))));
}
