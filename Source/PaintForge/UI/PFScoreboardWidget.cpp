// Copyright (c) 2026 Tom Chapman. All rights reserved.

#include "UI/PFScoreboardWidget.h"

#include "Core/PaintForgeGameState.h"
#include "Core/PaintForgePlayerState.h"

#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/Image.h"
#include "Components/SizeBox.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Brushes/SlateColorBrush.h"
#include "GameFramework/PlayerState.h"
#include "Styling/CoreStyle.h"

namespace
{
	FSlateFontInfo PFBoardFont(int32 Size, bool bBold)
	{
		return FCoreStyle::GetDefaultFontStyle(bBold ? FName(TEXT("Bold")) : FName(TEXT("Regular")), Size);
	}
}

TSharedRef<SWidget> UPFScoreboardWidget::RebuildWidget()
{
	if (WidgetTree && !WidgetTree->RootWidget)
	{
		BuildTree(); // root assigned BEFORE Super::RebuildWidget (02 R3)
	}
	return Super::RebuildWidget();
}

void UPFScoreboardWidget::BuildTree()
{
	UCanvasPanel* RootCanvas = WidgetTree->ConstructWidget<UCanvasPanel>();
	WidgetTree->RootWidget = RootCanvas;

	UBorder* Panel = WidgetTree->ConstructWidget<UBorder>();
	Panel->SetBrushColor(FLinearColor(0.02f, 0.02f, 0.03f, 0.88f));
	Panel->SetPadding(FMargin(28.f, 20.f));

	UVerticalBox* Body = WidgetTree->ConstructWidget<UVerticalBox>();

	// Round-win score line: "A 2 — 1 B" in team colors.
	UHorizontalBox* ScoreRow = WidgetTree->ConstructWidget<UHorizontalBox>();
	WinsAText = WidgetTree->ConstructWidget<UTextBlock>();
	WinsAText->SetFont(PFBoardFont(30, true));
	WinsAText->SetColorAndOpacity(FSlateColor(PFColors::ForTeam(0)));
	UTextBlock* ScoreDash = WidgetTree->ConstructWidget<UTextBlock>();
	ScoreDash->SetText(FText::FromString(TEXT("  —  ")));
	ScoreDash->SetFont(PFBoardFont(30, false));
	ScoreDash->SetColorAndOpacity(FSlateColor(FLinearColor(1.f, 1.f, 1.f, 0.6f)));
	WinsBText = WidgetTree->ConstructWidget<UTextBlock>();
	WinsBText->SetFont(PFBoardFont(30, true));
	WinsBText->SetColorAndOpacity(FSlateColor(PFColors::ForTeam(1)));
	ScoreRow->AddChildToHorizontalBox(WinsAText);
	ScoreRow->AddChildToHorizontalBox(ScoreDash);
	ScoreRow->AddChildToHorizontalBox(WinsBText);
	if (UVerticalBoxSlot* VSlot = Body->AddChildToVerticalBox(ScoreRow))
	{
		VSlot->SetHorizontalAlignment(HAlign_Center);
	}

	// Round context line ("Round 3", sudden death flag).
	RoundText = WidgetTree->ConstructWidget<UTextBlock>();
	RoundText->SetFont(PFBoardFont(13, false));
	RoundText->SetColorAndOpacity(FSlateColor(FLinearColor(1.f, 1.f, 1.f, 0.6f)));
	RoundText->SetJustification(ETextJustify::Center);
	if (UVerticalBoxSlot* VSlot = Body->AddChildToVerticalBox(RoundText))
	{
		VSlot->SetHorizontalAlignment(HAlign_Center);
		VSlot->SetPadding(FMargin(0.f, 2.f, 0.f, 12.f));
	}

	// Roster rows (header + one per PlayerState), rebuilt on poll.
	RowsBox = WidgetTree->ConstructWidget<UVerticalBox>();
	Body->AddChildToVerticalBox(RowsBox);

	Panel->SetContent(Body);

	USizeBox* Sizer = WidgetTree->ConstructWidget<USizeBox>();
	Sizer->SetWidthOverride(680.f);
	Sizer->SetContent(Panel);

	if (UCanvasPanelSlot* CSlot = RootCanvas->AddChildToCanvas(Sizer))
	{
		CSlot->SetAnchors(FAnchors(0.5f, 0.5f));
		CSlot->SetAlignment(FVector2D(0.5f, 0.5f));
		CSlot->SetPosition(FVector2D(0.f, -10.f));
		CSlot->SetAutoSize(true);
	}
}

void UPFScoreboardWidget::NativeOnInitialized()
{
	Super::NativeOnInitialized();
	// UPFRootHUDWidget drives visibility from the PC's Tab-hold state; start hidden.
	SetVisibility(ESlateVisibility::Collapsed);
}

void UPFScoreboardWidget::RefreshNow()
{
	PollAccum = 0.f;
	RefreshRows();
}

void UPFScoreboardWidget::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);

	// Collapsed widgets don't tick, so this only runs while the overlay is actually shown.
	PollAccum += InDeltaTime;
	if (PollAccum >= PollInterval)
	{
		PollAccum = 0.f;
		RefreshRows();
	}
}

void UPFScoreboardWidget::RefreshRows()
{
	if (!RowsBox || !WidgetTree)
	{
		return;
	}
	const UWorld* World = GetWorld();
	APaintForgeGameState* GS = World ? World->GetGameState<APaintForgeGameState>() : nullptr;
	if (!GS)
	{
		return;
	}

	if (WinsAText)
	{
		WinsAText->SetText(FText::FromString(FString::Printf(TEXT("%d"), GS->TeamRoundWins[0])));
	}
	if (WinsBText)
	{
		WinsBText->SetText(FText::FromString(FString::Printf(TEXT("%d"), GS->TeamRoundWins[1])));
	}
	if (RoundText)
	{
		FString Round;
		if (GS->RoundNumber > 0)
		{
			Round = FString::Printf(TEXT("Round %d"), GS->RoundNumber);
			if (GS->bSuddenDeath)
			{
				Round += TEXT("  ·  SUDDEN DEATH");
			}
		}
		RoundText->SetText(FText::FromString(Round));
	}

	// Stable display order: team A, team B, unassigned; score desc, elims desc, then name.
	TArray<APaintForgePlayerState*> Roster;
	for (APlayerState* PSBase : GS->PlayerArray)
	{
		if (APaintForgePlayerState* PS = Cast<APaintForgePlayerState>(PSBase))
		{
			Roster.Add(PS);
		}
	}
	Roster.Sort([](const APaintForgePlayerState& A, const APaintForgePlayerState& B)
	{
		if (A.TeamId != B.TeamId)
		{
			return A.TeamId < B.TeamId;
		}
		if (A.MatchScore != B.MatchScore)
		{
			return A.MatchScore > B.MatchScore;
		}
		if (A.Eliminations != B.Eliminations)
		{
			return A.Eliminations > B.Eliminations;
		}
		return A.GetPlayerName() < B.GetPlayerName();
	});

	// Only rebuild the row widgets when something visible actually changed.
	FString Signature;
	for (const APaintForgePlayerState* PS : Roster)
	{
		Signature += FString::Printf(TEXT("%s|%d|%d|%d|%d|%d;"), *PS->GetPlayerName(), PS->TeamId,
			PS->Eliminations, PS->TimesEliminated, PS->MatchScore, PS->bAliveInRound ? 1 : 0);
	}
	if (Signature == LastSignature)
	{
		return;
	}
	LastSignature = Signature;

	RowsBox->ClearChildren();
	AddHeaderRow();
	for (const APaintForgePlayerState* PS : Roster)
	{
		AddRow(PS);
	}
}

void UPFScoreboardWidget::AddHeaderRow()
{
	UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>();
	const FLinearColor HeaderColor(1.f, 1.f, 1.f, 0.45f);

	// Chip-column spacer keeps the header aligned with the rows below.
	USizeBox* ChipSpacer = WidgetTree->ConstructWidget<USizeBox>();
	ChipSpacer->SetWidthOverride(ChipSizePx);
	ChipSpacer->SetHeightOverride(ChipSizePx);
	if (UHorizontalBoxSlot* HSlot = Row->AddChildToHorizontalBox(ChipSpacer))
	{
		HSlot->SetVerticalAlignment(VAlign_Center);
		HSlot->SetPadding(FMargin(0.f, 2.f, 10.f, 2.f));
	}

	UTextBlock* PlayerLabel = WidgetTree->ConstructWidget<UTextBlock>();
	PlayerLabel->SetText(FText::FromString(TEXT("PLAYER")));
	PlayerLabel->SetFont(PFBoardFont(12, true));
	PlayerLabel->SetColorAndOpacity(FSlateColor(HeaderColor));
	if (UHorizontalBoxSlot* HSlot = Row->AddChildToHorizontalBox(PlayerLabel))
	{
		HSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		HSlot->SetVerticalAlignment(VAlign_Center);
	}

	auto AddColLabel = [this, Row, &HeaderColor](const TCHAR* Label, float Width)
	{
		UTextBlock* Text = WidgetTree->ConstructWidget<UTextBlock>();
		Text->SetText(FText::FromString(Label));
		Text->SetFont(PFBoardFont(12, true));
		Text->SetColorAndOpacity(FSlateColor(HeaderColor));
		Text->SetJustification(ETextJustify::Right);
		USizeBox* Sizer = WidgetTree->ConstructWidget<USizeBox>();
		Sizer->SetWidthOverride(Width);
		Sizer->SetContent(Text);
		if (UHorizontalBoxSlot* HSlot = Row->AddChildToHorizontalBox(Sizer))
		{
			HSlot->SetVerticalAlignment(VAlign_Center);
			HSlot->SetPadding(FMargin(6.f, 0.f));
		}
	};
	AddColLabel(TEXT("ELIM"), NumColWidthPx);
	AddColLabel(TEXT("OUT"), NumColWidthPx);
	AddColLabel(TEXT("SCORE"), ScoreColWidthPx);

	// Alive-dot-column spacer.
	USizeBox* DotSpacer = WidgetTree->ConstructWidget<USizeBox>();
	DotSpacer->SetWidthOverride(DotSizePx);
	DotSpacer->SetHeightOverride(DotSizePx);
	if (UHorizontalBoxSlot* HSlot = Row->AddChildToHorizontalBox(DotSpacer))
	{
		HSlot->SetVerticalAlignment(VAlign_Center);
		HSlot->SetPadding(FMargin(10.f, 0.f, 0.f, 0.f));
	}

	if (UVerticalBoxSlot* VSlot = RowsBox->AddChildToVerticalBox(Row))
	{
		VSlot->SetPadding(FMargin(0.f, 0.f, 0.f, 6.f));
	}
}

void UPFScoreboardWidget::AddRow(const APaintForgePlayerState* PS)
{
	if (!PS)
	{
		return;
	}
	UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>();

	// Team color chip.
	UImage* Chip = WidgetTree->ConstructWidget<UImage>();
	Chip->SetBrush(FSlateColorBrush(FLinearColor::White));
	Chip->SetColorAndOpacity(PS->TeamId <= 1 ? PFColors::ForTeam(PS->TeamId) : FLinearColor(0.4f, 0.4f, 0.4f));
	USizeBox* ChipSizer = WidgetTree->ConstructWidget<USizeBox>();
	ChipSizer->SetWidthOverride(ChipSizePx);
	ChipSizer->SetHeightOverride(ChipSizePx);
	ChipSizer->SetContent(Chip);
	if (UHorizontalBoxSlot* HSlot = Row->AddChildToHorizontalBox(ChipSizer))
	{
		HSlot->SetVerticalAlignment(VAlign_Center);
		HSlot->SetPadding(FMargin(0.f, 3.f, 10.f, 3.f));
	}

	// Player name.
	UTextBlock* NameText = WidgetTree->ConstructWidget<UTextBlock>();
	NameText->SetText(FText::FromString(PS->GetPlayerName()));
	NameText->SetFont(PFBoardFont(15, false));
	NameText->SetColorAndOpacity(FSlateColor(FLinearColor::White));
	if (UHorizontalBoxSlot* HSlot = Row->AddChildToHorizontalBox(NameText))
	{
		HSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		HSlot->SetVerticalAlignment(VAlign_Center);
	}

	// Numeric columns: eliminations, times eliminated, match score.
	auto AddNumCell = [this, Row](int32 Value, float Width, bool bBold)
	{
		UTextBlock* Text = WidgetTree->ConstructWidget<UTextBlock>();
		Text->SetText(FText::FromString(FString::Printf(TEXT("%d"), Value)));
		Text->SetFont(PFBoardFont(15, bBold));
		Text->SetColorAndOpacity(FSlateColor(FLinearColor(1.f, 1.f, 1.f, 0.9f)));
		Text->SetJustification(ETextJustify::Right);
		USizeBox* Sizer = WidgetTree->ConstructWidget<USizeBox>();
		Sizer->SetWidthOverride(Width);
		Sizer->SetContent(Text);
		if (UHorizontalBoxSlot* HSlot = Row->AddChildToHorizontalBox(Sizer))
		{
			HSlot->SetVerticalAlignment(VAlign_Center);
			HSlot->SetPadding(FMargin(6.f, 0.f));
		}
	};
	AddNumCell(PS->Eliminations, NumColWidthPx, false);
	AddNumCell(PS->TimesEliminated, NumColWidthPx, false);
	AddNumCell(PS->MatchScore, ScoreColWidthPx, true);

	// Alive dot (round HP state at PlayerState granularity: in or out this round).
	UImage* Dot = WidgetTree->ConstructWidget<UImage>();
	Dot->SetBrush(FSlateColorBrush(FLinearColor::White));
	Dot->SetColorAndOpacity(PS->bAliveInRound
		? FLinearColor(0.2f, 0.9f, 0.3f)
		: FLinearColor(1.f, 1.f, 1.f, 0.15f));
	USizeBox* DotSizer = WidgetTree->ConstructWidget<USizeBox>();
	DotSizer->SetWidthOverride(DotSizePx);
	DotSizer->SetHeightOverride(DotSizePx);
	DotSizer->SetContent(Dot);
	if (UHorizontalBoxSlot* HSlot = Row->AddChildToHorizontalBox(DotSizer))
	{
		HSlot->SetVerticalAlignment(VAlign_Center);
		HSlot->SetPadding(FMargin(10.f, 0.f, 0.f, 0.f));
	}

	if (UVerticalBoxSlot* VSlot = RowsBox->AddChildToVerticalBox(Row))
	{
		VSlot->SetPadding(FMargin(0.f, 2.f));
	}
}
