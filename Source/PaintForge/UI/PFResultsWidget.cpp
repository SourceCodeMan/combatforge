// Copyright (c) 2026 Tom Chapman. All rights reserved.

#include "UI/PFResultsWidget.h"

#include "Building/PFArenaSerialization.h"
#include "Building/PFBuildGrid.h"
#include "Core/PaintForgeGameState.h"
#include "Core/PaintForgePlayerController.h"
#include "Core/PaintForgePlayerState.h"

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
	AddCentered(ScoreText, 18, false, FLinearColor(1.f, 1.f, 1.f, 0.85f), FMargin(0.f, 0.f, 0.f, 20.f));
	AddCentered(MVPText, 18, true, FLinearColor(1.f, 0.85f, 0.2f), FMargin(0.f, 0.f, 0.f, 20.f));
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
		// Host-ness is fixed for the session (listen host, 02 D12).
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
	APaintForgeGameState* GS = World ? World->GetGameState<APaintForgeGameState>() : nullptr;
	if (GS)
	{
		BoundGameState = GS;
		GS->OnVoteTallyChangedEvent.AddUObject(this, &UPFResultsWidget::HandleVoteTallyChanged);
	}
}

void UPFResultsWidget::HandleVoteTallyChanged()
{
	if (const APaintForgeGameState* GS = BoundGameState.Get())
	{
		RefreshTally(*GS);
	}
}

bool UPFResultsWidget::IsLocalHost() const
{
	const APlayerController* PC = GetOwningPlayer();
	return PC && PC->IsLocalController() && PC->HasAuthority();
}

void UPFResultsWidget::HandleReturnClicked()
{
	// UX gate only — the RPC body re-validates host identity server-side (§5 R13).
	if (APaintForgePlayerController* PC = Cast<APaintForgePlayerController>(GetOwningPlayer()))
	{
		PC->ServerHostReturnToLobby();
	}
}

void UPFResultsWidget::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);

	TryBindGameState(); // GameState can arrive late on clients

	const APaintForgeGameState* GS = BoundGameState.Get();
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
	const APaintForgeGameState* GS = BoundGameState.Get();
	if (!GS)
	{
		return;
	}
	RefreshResult(*GS);
	RefreshMVP(*GS);
	RefreshTally(*GS);
	RefreshArenaId(*GS);
}

void UPFResultsWidget::RefreshResult(const APaintForgeGameState& GS)
{
	// Skirmish reports team TAG counts; Elimination reports round wins.
	const bool bSkirmish = (GS.MatchType == EPFMatchType::Skirmish);
	const int32 WinsA = bSkirmish ? static_cast<int32>(GS.TeamScores[0]) : static_cast<int32>(GS.TeamRoundWins[0]);
	const int32 WinsB = bSkirmish ? static_cast<int32>(GS.TeamScores[1]) : static_cast<int32>(GS.TeamRoundWins[1]);

	if (WinnerText)
	{
		if (WinsA == WinsB)
		{
			WinnerText->SetText(FText::FromString(TEXT("MATCH DRAW")));
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
		FString Score = FString::Printf(TEXT("%d — %d"), WinsA, WinsB);
		if (bSkirmish)
		{
			Score += TEXT("  ·  tags");
		}
		else
		{
			if (GS.RoundNumber > 0)
			{
				Score += FString::Printf(TEXT("  ·  %d rounds"), GS.RoundNumber);
			}
			if (GS.bSuddenDeath)
			{
				Score += TEXT("  ·  sudden death");
			}
		}
		ScoreText->SetText(FText::FromString(Score));
	}
}

void UPFResultsWidget::RefreshMVP(const APaintForgeGameState& GS)
{
	if (!MVPText)
	{
		return;
	}
	// MVP = highest MatchScore, eliminations tiebreak (T18/§3.6); name breaks exact ties so
	// every client shows the same MVP.
	const APaintForgePlayerState* Best = nullptr;
	for (APlayerState* PSBase : GS.PlayerArray)
	{
		const APaintForgePlayerState* PS = Cast<APaintForgePlayerState>(PSBase);
		if (!PS)
		{
			continue;
		}
		if (!Best
			|| PS->MatchScore > Best->MatchScore
			|| (PS->MatchScore == Best->MatchScore && PS->Eliminations > Best->Eliminations)
			|| (PS->MatchScore == Best->MatchScore && PS->Eliminations == Best->Eliminations
				&& PS->GetPlayerName() < Best->GetPlayerName()))
		{
			Best = PS;
		}
	}
	MVPText->SetText(Best
		? FText::FromString(FString::Printf(TEXT("MVP  %s — %d pts  ·  %d elims"),
			*Best->GetPlayerName(), Best->MatchScore, Best->Eliminations))
		: FText::GetEmpty());
}

void UPFResultsWidget::RefreshTally(const APaintForgeGameState& GS)
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

void UPFResultsWidget::RefreshArenaId(const APaintForgeGameState& GS)
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
