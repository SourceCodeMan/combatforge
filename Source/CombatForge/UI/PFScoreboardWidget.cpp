// Copyright (c) 2026 Tom Chapman. All rights reserved.

#include "UI/PFScoreboardWidget.h"

#include "Core/CombatForgeGameState.h"
#include "Core/CombatForgePlayerState.h"
#include "UI/PFCycleBar.h"

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

	// Match-rotation strip (Tom 2026-07-24): where the Creative → Remix → Remix Swap wheel is.
	CycleBarRoot = PFCycleBar::Build(WidgetTree, CycleSegs, CycleTexts);
	if (UVerticalBoxSlot* VSlot = Body->AddChildToVerticalBox(CycleBarRoot))
	{
		VSlot->SetHorizontalAlignment(HAlign_Center);
		VSlot->SetPadding(FMargin(0.f, 0.f, 0.f, 10.f));
	}

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

	// Mode + unit line ("CAPTURE THE FLAG · first to 3 · CAPTURES").
	RoundText = WidgetTree->ConstructWidget<UTextBlock>();
	RoundText->SetFont(PFBoardFont(13, true));
	RoundText->SetColorAndOpacity(FSlateColor(FLinearColor(1.f, 1.f, 1.f, 0.75f)));
	RoundText->SetJustification(ETextJustify::Center);
	if (UVerticalBoxSlot* VSlot = Body->AddChildToVerticalBox(RoundText))
	{
		VSlot->SetHorizontalAlignment(HAlign_Center);
		VSlot->SetPadding(FMargin(0.f, 4.f, 0.f, 2.f));
	}

	ScoreUnitText = WidgetTree->ConstructWidget<UTextBlock>();
	ScoreUnitText->SetFont(PFBoardFont(11, false));
	ScoreUnitText->SetColorAndOpacity(FSlateColor(FLinearColor(1.f, 1.f, 1.f, 0.45f)));
	ScoreUnitText->SetJustification(ETextJustify::Center);
	if (UVerticalBoxSlot* VSlot = Body->AddChildToVerticalBox(ScoreUnitText))
	{
		VSlot->SetHorizontalAlignment(HAlign_Center);
		VSlot->SetPadding(FMargin(0.f, 0.f, 0.f, 12.f));
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
	ACombatForgeGameState* GS = World ? World->GetGameState<ACombatForgeGameState>() : nullptr;
	if (!GS)
	{
		return;
	}

	PFCycleBar::Update(GS, CycleBarRoot, CycleSegs, CycleTexts);

	// TeamScores modes: Skirmish tags + CTF/Dom/HP objective points. FreeForAll: YOU / LEAD. Else: round wins.
	const bool bTeamScores = (GS->MatchType == EPFMatchType::Skirmish
		|| GS->MatchType == EPFMatchType::CaptureFlag
		|| GS->MatchType == EPFMatchType::Domination
		|| GS->MatchType == EPFMatchType::Hardpoint);
	const bool bFFA = (GS->MatchType == EPFMatchType::FreeForAll);

	uint16 MyTags = 0;
	uint16 LeadTags = 0;
	if (bFFA)
	{
		if (const APlayerController* PC = GetOwningPlayer())
		{
			if (const ACombatForgePlayerState* LocalPS = PC->GetPlayerState<ACombatForgePlayerState>())
			{
				MyTags = LocalPS->TagCount;
			}
		}
		for (APlayerState* PSBase : GS->PlayerArray)
		{
			if (const ACombatForgePlayerState* PS = Cast<ACombatForgePlayerState>(PSBase);
				PS && !PS->IsPhantom())
			{
				LeadTags = FMath::Max(LeadTags, PS->TagCount);
			}
		}
	}

	if (WinsAText)
	{
		if (bFFA)
		{
			WinsAText->SetText(FText::FromString(FString::Printf(TEXT("%d"), MyTags)));
			WinsAText->SetColorAndOpacity(FSlateColor(FLinearColor(1.f, 0.9f, 0.35f))); // YOU
		}
		else
		{
			WinsAText->SetText(FText::FromString(FString::Printf(TEXT("%d"),
				bTeamScores ? static_cast<int32>(GS->TeamScores[0]) : static_cast<int32>(GS->TeamRoundWins[0]))));
			WinsAText->SetColorAndOpacity(FSlateColor(PFColors::ForTeam(0)));
		}
	}
	if (WinsBText)
	{
		if (bFFA)
		{
			WinsBText->SetText(FText::FromString(FString::Printf(TEXT("%d"), LeadTags)));
			WinsBText->SetColorAndOpacity(FSlateColor(FLinearColor(1.f, 1.f, 1.f, 0.85f))); // LEAD
		}
		else
		{
			WinsBText->SetText(FText::FromString(FString::Printf(TEXT("%d"),
				bTeamScores ? static_cast<int32>(GS->TeamScores[1]) : static_cast<int32>(GS->TeamRoundWins[1]))));
			WinsBText->SetColorAndOpacity(FSlateColor(PFColors::ForTeam(1)));
		}
	}

	// Mode title + score-unit subtitle.
	FString ModeTitle;
	FString ScoreUnit;
	switch (GS->MatchType)
	{
	case EPFMatchType::FreeForAll:
		ModeTitle = TEXT("FREE-FOR-ALL");
		ScoreUnit = TEXT("YOU  —  LEAD   ·   tags");
		break;
	case EPFMatchType::Skirmish:
		ModeTitle = TEXT("SKIRMISH");
		ScoreUnit = TEXT("TEAM TAGS");
		break;
	case EPFMatchType::CaptureFlag:
		ModeTitle = TEXT("CAPTURE THE FLAG");
		ScoreUnit = TEXT("CAPTURES");
		break;
	case EPFMatchType::Domination:
		ModeTitle = TEXT("DOMINATION");
		ScoreUnit = TEXT("CONTROL POINTS");
		break;
	case EPFMatchType::Hardpoint:
		ModeTitle = TEXT("HARDPOINT");
		ScoreUnit = TEXT("HARDPOINT SCORE");
		break;
	default:
		ModeTitle = (GS->RoundNumber > 0)
			? FString::Printf(TEXT("ELIMINATION  ·  ROUND %d"), GS->RoundNumber)
			: TEXT("ELIMINATION");
		if (GS->bSuddenDeath) { ModeTitle += TEXT("  ·  SHOWDOWN"); }
		ScoreUnit = TEXT("ROUND WINS");
		break;
	}
	if (!bFFA && GS->MatchType != EPFMatchType::Elimination && GS->RoundWinsToTake > 0)
	{
		ModeTitle += FString::Printf(TEXT("  ·  first to %d"), GS->RoundWinsToTake);
	}
	else if (bFFA && GS->RoundWinsToTake > 0)
	{
		ModeTitle += FString::Printf(TEXT("  ·  first to %d"), GS->RoundWinsToTake);
	}
	if (RoundText)
	{
		RoundText->SetText(FText::FromString(ModeTitle));
	}
	if (ScoreUnitText)
	{
		ScoreUnitText->SetText(FText::FromString(ScoreUnit));
	}

	// Stable display order: FFA = tags desc; else team A/B then score/elims.
	// The pilot box's phantom PlayerState is not a player — never give it a row.
	TArray<ACombatForgePlayerState*> Roster;
	for (APlayerState* PSBase : GS->PlayerArray)
	{
		if (ACombatForgePlayerState* PS = Cast<ACombatForgePlayerState>(PSBase);
			PS && !PS->IsPhantom())
		{
			Roster.Add(PS);
		}
	}
	if (bFFA)
	{
		Roster.Sort([](const ACombatForgePlayerState& A, const ACombatForgePlayerState& B)
		{
			if (A.TagCount != B.TagCount)
			{
				return A.TagCount > B.TagCount;
			}
			if (A.Eliminations != B.Eliminations)
			{
				return A.Eliminations > B.Eliminations;
			}
			return A.GetPlayerName() < B.GetPlayerName();
		});
	}
	else
	{
		Roster.Sort([](const ACombatForgePlayerState& A, const ACombatForgePlayerState& B)
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
	}

	// Rebuild rows when roster, stats, mode, or team scores change (header is mode-sensitive).
	FString Signature = FString::Printf(TEXT("m%d|s%d|s%d|t%d|"),
		static_cast<int32>(GS->MatchType), GS->TeamScores[0], GS->TeamScores[1], GS->RoundWinsToTake);
	for (const ACombatForgePlayerState* PS : Roster)
	{
		Signature += FString::Printf(TEXT("%s|%d|%d|%d|%d|%d|%d|%d|%d;"), *PS->GetPlayerName(), PS->TeamId,
			PS->Eliminations, PS->TimesEliminated, PS->MatchScore, PS->TagCount, PS->bAliveInRound ? 1 : 0,
			PS->bCarryingFlag ? 1 : 0, static_cast<int32>(PS->StandingOnPoint));
	}
	if (Signature == LastSignature)
	{
		return;
	}
	LastSignature = Signature;

	RowsBox->ClearChildren();
	AddHeaderRow();
	for (const ACombatForgePlayerState* PS : Roster)
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
	{
		const ACombatForgeGameState* GS = GetWorld() ? GetWorld()->GetGameState<ACombatForgeGameState>() : nullptr;
		const bool bFFA = GS && GS->MatchType == EPFMatchType::FreeForAll;
		AddColLabel(bFFA ? TEXT("TAGS") : TEXT("ELIM"), NumColWidthPx);
		AddColLabel(TEXT("OUT"), NumColWidthPx);
		AddColLabel(bFFA ? TEXT("ELIM") : TEXT("SCORE"), ScoreColWidthPx);
	}

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

void UPFScoreboardWidget::AddRow(const ACombatForgePlayerState* PS)
{
	if (!PS)
	{
		return;
	}
	UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>();

	const ACombatForgeGameState* GS = GetWorld() ? GetWorld()->GetGameState<ACombatForgeGameState>() : nullptr;
	const bool bFFA = GS && GS->MatchType == EPFMatchType::FreeForAll;

	// Team color chip (FFA: palette by combat-id % 2).
	UImage* Chip = WidgetTree->ConstructWidget<UImage>();
	Chip->SetBrush(FSlateColorBrush(FLinearColor::White));
	{
		const uint8 TintTeam = (PS->TeamId == 255) ? 255 : static_cast<uint8>(PS->TeamId % 2);
		Chip->SetColorAndOpacity(TintTeam <= 1 ? PFColors::ForTeam(TintTeam) : FLinearColor(0.4f, 0.4f, 0.4f));
	}
	USizeBox* ChipSizer = WidgetTree->ConstructWidget<USizeBox>();
	ChipSizer->SetWidthOverride(ChipSizePx);
	ChipSizer->SetHeightOverride(ChipSizePx);
	ChipSizer->SetContent(Chip);
	if (UHorizontalBoxSlot* HSlot = Row->AddChildToHorizontalBox(ChipSizer))
	{
		HSlot->SetVerticalAlignment(VAlign_Center);
		HSlot->SetPadding(FMargin(0.f, 3.f, 10.f, 3.f));
	}

	// Player name (+ YOU / FLAG / ON POINT markers).
	const ACombatForgePlayerState* LocalPS =
		GetOwningPlayer() ? GetOwningPlayer()->GetPlayerState<ACombatForgePlayerState>() : nullptr;
	const bool bIsLocal = (LocalPS == PS);

	UTextBlock* NameText = WidgetTree->ConstructWidget<UTextBlock>();
	FString DisplayName = PS->GetPlayerName();
	if (bIsLocal)
	{
		DisplayName += TEXT("  (you)");
	}
	if (PS->bCarryingFlag)
	{
		DisplayName += TEXT("  ⚑ FLAG");
	}
	else if (PS->StandingOnPoint != 255)
	{
		static const TCHAR* PointNames[] = { TEXT("A"), TEXT("B"), TEXT("C") };
		const int32 Idx = FMath::Clamp(static_cast<int32>(PS->StandingOnPoint), 0, 2);
		DisplayName += FString::Printf(TEXT("  ·  %s"), PointNames[Idx]);
	}
	NameText->SetText(FText::FromString(DisplayName));
	NameText->SetFont(PFBoardFont(15, bIsLocal));
	FLinearColor NameColor = FLinearColor::White;
	if (PS->bCarryingFlag && PS->CarriedFlagTeam <= 1)
	{
		NameColor = PFColors::ForTeam(PS->CarriedFlagTeam);
	}
	else if (bIsLocal)
	{
		NameColor = FLinearColor(1.f, 0.92f, 0.45f);
	}
	NameText->SetColorAndOpacity(FSlateColor(NameColor));
	if (UHorizontalBoxSlot* HSlot = Row->AddChildToHorizontalBox(NameText))
	{
		HSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		HSlot->SetVerticalAlignment(VAlign_Center);
	}

	// Numeric columns: FFA = tags / out / elims; else elim / out / score.
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
	if (bFFA)
	{
		AddNumCell(PS->TagCount, NumColWidthPx, true);
		AddNumCell(PS->TimesEliminated, NumColWidthPx, false);
		AddNumCell(PS->Eliminations, ScoreColWidthPx, false);
	}
	else
	{
		AddNumCell(PS->Eliminations, NumColWidthPx, false);
		AddNumCell(PS->TimesEliminated, NumColWidthPx, false);
		AddNumCell(PS->MatchScore, ScoreColWidthPx, true);
	}

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
