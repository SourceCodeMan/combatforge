// Copyright (c) 2026 Tom Chapman. All rights reserved.

#include "UI/PFLobbyWidget.h"

#include "Core/PaintForgeGameState.h"
#include "Core/PaintForgePlayerController.h"
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
	FSlateFontInfo PFLobbyFont(int32 Size, bool bBold)
	{
		return FCoreStyle::GetDefaultFontStyle(bBold ? FName(TEXT("Bold")) : FName(TEXT("Regular")), Size);
	}

	FString BuildModeLabel(EPFBuildMode Mode)
	{
		switch (Mode)
		{
		case EPFBuildMode::Creative:    return TEXT("Creative");
		case EPFBuildMode::Improvement: return TEXT("Improvement");
		case EPFBuildMode::PlayOnly:    return TEXT("Play-Only");
		default:                        return TEXT("Creative");
		}
	}

	FString BuildModeBlurb(EPFBuildMode Mode)
	{
		switch (Mode)
		{
		case EPFBuildMode::Creative:
			return TEXT("Empty plots · build your fort from scratch");
		case EPFBuildMode::Improvement:
			return TEXT("Load a saved arena · both teams improve it");
		case EPFBuildMode::PlayOnly:
			return TEXT("Skip build · straight into combat");
		default:
			return TEXT("");
		}
	}

	FString MatchTypeLabel(EPFMatchType Type)
	{
		switch (Type)
		{
		case EPFMatchType::Elimination: return TEXT("Elimination");
		case EPFMatchType::FreeForAll:  return TEXT("Free-for-All");
		case EPFMatchType::Skirmish:    return TEXT("Skirmish");
		case EPFMatchType::CaptureFlag: return TEXT("Capture the Flag");
		case EPFMatchType::Domination:  return TEXT("Domination");
		case EPFMatchType::Hardpoint:   return TEXT("Hardpoint");
		default:                        return TEXT("Elimination");
		}
	}

	/** One-line how-to for the match TYPE row (kids-friendly). */
	FString MatchTypeBlurb(EPFMatchType Type)
	{
		switch (Type)
		{
		case EPFMatchType::Elimination:
			return TEXT("Last team standing · first to N round wins");
		case EPFMatchType::FreeForAll:
			return TEXT("Solo · most tags · no teams · play-only");
		case EPFMatchType::Skirmish:
			return TEXT("Teams · most tags · everyone respawns");
		case EPFMatchType::CaptureFlag:
			return TEXT("Grab their flag · score at your base · first to 3");
		case EPFMatchType::Domination:
			return TEXT("Hold points A / MID / B · score over time");
		case EPFMatchType::Hardpoint:
			return TEXT("One rotating point · hold it · score over time");
		default:
			return TEXT("");
		}
	}
}

// ---------------------------------------------------------------- row button

void UPFLobbyRowButton::InitRow(UPFLobbyWidget* InOwner, APaintForgePlayerState* InPlayerState)
{
	OwnerWidget = InOwner;
	RowPlayerState = InPlayerState;
	OnClicked.AddUniqueDynamic(this, &UPFLobbyRowButton::HandleRowClicked);
}

void UPFLobbyRowButton::HandleRowClicked()
{
	if (OwnerWidget.IsValid() && RowPlayerState.IsValid())
	{
		OwnerWidget->NotifyRowClicked(RowPlayerState.Get());
	}
}

// -------------------------------------------------------------- lobby widget

TSharedRef<SWidget> UPFLobbyWidget::RebuildWidget()
{
	if (WidgetTree && !WidgetTree->RootWidget)
	{
		BuildTree(); // root assigned BEFORE Super::RebuildWidget (02 R3)
	}
	return Super::RebuildWidget();
}

void UPFLobbyWidget::BuildTree()
{
	UCanvasPanel* RootCanvas = WidgetTree->ConstructWidget<UCanvasPanel>();
	WidgetTree->RootWidget = RootCanvas;

	// Title.
	TitleText = WidgetTree->ConstructWidget<UTextBlock>();
	TitleText->SetText(FText::FromString(TEXT("PAINTFORGE — LOBBY")));
	TitleText->SetFont(PFLobbyFont(28, true));
	TitleText->SetColorAndOpacity(FSlateColor(FLinearColor::White));
	TitleText->SetJustification(ETextJustify::Center);
	if (UCanvasPanelSlot* CSlot = RootCanvas->AddChildToCanvas(TitleText))
	{
		CSlot->SetAnchors(FAnchors(0.5f, 0.f));
		CSlot->SetAlignment(FVector2D(0.5f, 0.f));
		CSlot->SetPosition(FVector2D(0.f, 48.f));
		CSlot->SetAutoSize(true);
	}

	// Start countdown (shown once the GameMode stamps a lobby end time).
	CountdownText = WidgetTree->ConstructWidget<UTextBlock>();
	CountdownText->SetText(FText::GetEmpty());
	CountdownText->SetFont(PFLobbyFont(20, true));
	CountdownText->SetColorAndOpacity(FSlateColor(FLinearColor(1.f, 0.85f, 0.2f)));
	CountdownText->SetJustification(ETextJustify::Center);
	if (UCanvasPanelSlot* CSlot = RootCanvas->AddChildToCanvas(CountdownText))
	{
		CSlot->SetAnchors(FAnchors(0.5f, 0.f));
		CSlot->SetAlignment(FVector2D(0.5f, 0.f));
		CSlot->SetPosition(FVector2D(0.f, 96.f));
		CSlot->SetAutoSize(true);
	}

	// Roster panel.
	UBorder* RosterPanel = WidgetTree->ConstructWidget<UBorder>();
	RosterPanel->SetBrushColor(FLinearColor(0.02f, 0.02f, 0.03f, 0.7f));
	RosterPanel->SetPadding(FMargin(16.f));

	RosterBox = WidgetTree->ConstructWidget<UVerticalBox>();
	RosterPanel->SetContent(RosterBox);

	USizeBox* RosterSizer = WidgetTree->ConstructWidget<USizeBox>();
	RosterSizer->SetWidthOverride(520.f);
	RosterSizer->SetContent(RosterPanel);

	if (UCanvasPanelSlot* CSlot = RootCanvas->AddChildToCanvas(RosterSizer))
	{
		CSlot->SetAnchors(FAnchors(0.5f, 0.5f));
		CSlot->SetAlignment(FVector2D(0.5f, 0.5f));
		CSlot->SetPosition(FVector2D(0.f, -20.f));
		CSlot->SetAutoSize(true);
	}

	// Footer hints.
	FooterText = WidgetTree->ConstructWidget<UTextBlock>();
	FooterText->SetFont(PFLobbyFont(14, false));
	FooterText->SetColorAndOpacity(FSlateColor(FLinearColor(1.f, 1.f, 1.f, 0.7f)));
	FooterText->SetJustification(ETextJustify::Center);
	if (UCanvasPanelSlot* CSlot = RootCanvas->AddChildToCanvas(FooterText))
	{
		CSlot->SetAnchors(FAnchors(0.5f, 1.f));
		CSlot->SetAlignment(FVector2D(0.5f, 1.f));
		CSlot->SetPosition(FVector2D(0.f, -40.f));
		CSlot->SetAutoSize(true);
	}

	BuildConfigPanel(RootCanvas);
	BuildLoadoutOverlay(RootCanvas);   // added last -> drawn on top when shown
}

UButton* UPFLobbyWidget::MakeConfigButton(const FString& Label, TObjectPtr<UTextBlock>& OutValueText)
{
	UButton* Btn = WidgetTree->ConstructWidget<UButton>();
	Btn->SetBackgroundColor(FLinearColor(0.10f, 0.11f, 0.13f, 0.9f));

	UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>();

	UTextBlock* LabelText = WidgetTree->ConstructWidget<UTextBlock>();
	LabelText->SetText(FText::FromString(Label));
	LabelText->SetFont(PFLobbyFont(12, true));
	LabelText->SetColorAndOpacity(FSlateColor(FLinearColor(0.55f, 0.68f, 0.85f)));
	USizeBox* LabelSizer = WidgetTree->ConstructWidget<USizeBox>();
	LabelSizer->SetWidthOverride(74.f);
	LabelSizer->SetContent(LabelText);
	if (UHorizontalBoxSlot* HS = Row->AddChildToHorizontalBox(LabelSizer))
	{
		HS->SetVerticalAlignment(VAlign_Center);
		HS->SetPadding(FMargin(6.f, 6.f, 8.f, 6.f));
	}

	OutValueText = WidgetTree->ConstructWidget<UTextBlock>();
	OutValueText->SetFont(PFLobbyFont(15, false));
	OutValueText->SetColorAndOpacity(FSlateColor(FLinearColor::White));
	if (UHorizontalBoxSlot* HS = Row->AddChildToHorizontalBox(OutValueText))
	{
		HS->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		HS->SetVerticalAlignment(VAlign_Center);
		HS->SetPadding(FMargin(0.f, 6.f, 6.f, 6.f));
	}

	Btn->SetContent(Row);
	return Btn;
}

void UPFLobbyWidget::AddReadOnlyRow(UVerticalBox* Box, const FString& Label, TObjectPtr<UTextBlock>& OutValueText)
{
	UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>();

	UTextBlock* LabelText = WidgetTree->ConstructWidget<UTextBlock>();
	LabelText->SetText(FText::FromString(Label));
	LabelText->SetFont(PFLobbyFont(12, true));
	LabelText->SetColorAndOpacity(FSlateColor(FLinearColor(0.45f, 0.55f, 0.7f)));
	USizeBox* LabelSizer = WidgetTree->ConstructWidget<USizeBox>();
	LabelSizer->SetWidthOverride(74.f);
	LabelSizer->SetContent(LabelText);
	if (UHorizontalBoxSlot* HS = Row->AddChildToHorizontalBox(LabelSizer))
	{
		HS->SetVerticalAlignment(VAlign_Center);
		HS->SetPadding(FMargin(6.f, 4.f, 8.f, 4.f));
	}

	OutValueText = WidgetTree->ConstructWidget<UTextBlock>();
	OutValueText->SetFont(PFLobbyFont(14, false));
	OutValueText->SetColorAndOpacity(FSlateColor(FLinearColor(0.85f, 0.87f, 0.9f)));
	if (UHorizontalBoxSlot* HS = Row->AddChildToHorizontalBox(OutValueText))
	{
		HS->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		HS->SetVerticalAlignment(VAlign_Center);
		HS->SetPadding(FMargin(0.f, 4.f, 6.f, 4.f));
	}

	if (UVerticalBoxSlot* VS = Box->AddChildToVerticalBox(Row))
	{
		VS->SetPadding(FMargin(0.f, 2.f));
		VS->SetHorizontalAlignment(HAlign_Fill);
	}
}

void UPFLobbyWidget::BuildConfigPanel(UCanvasPanel* RootCanvas)
{
	UBorder* Panel = WidgetTree->ConstructWidget<UBorder>();
	Panel->SetBrushColor(FLinearColor(0.02f, 0.02f, 0.03f, 0.7f));
	Panel->SetPadding(FMargin(14.f));

	UVerticalBox* Box = WidgetTree->ConstructWidget<UVerticalBox>();
	Panel->SetContent(Box);

	UTextBlock* Header = WidgetTree->ConstructWidget<UTextBlock>();
	Header->SetText(FText::FromString(TEXT("MATCH SETUP")));
	Header->SetFont(PFLobbyFont(16, true));
	Header->SetColorAndOpacity(FSlateColor(FLinearColor::White));
	if (UVerticalBoxSlot* VS = Box->AddChildToVerticalBox(Header)) { VS->SetPadding(FMargin(0.f, 0.f, 0.f, 8.f)); }

	// Mode / type / format / bots are chosen on the pre-game loading menu; lobby only displays them.
	AddReadOnlyRow(Box, TEXT("MODE"), ModeValueText);
	AddReadOnlyRow(Box, TEXT("TYPE"), TypeValueText);
	AddReadOnlyRow(Box, TEXT("FORMAT"), FormatValueText);
	AddReadOnlyRow(Box, TEXT("BOTS"), BotsValueText);

	UButton* LoadoutBtn = WidgetTree->ConstructWidget<UButton>();
	LoadoutBtn->SetBackgroundColor(FLinearColor(0.14f, 0.12f, 0.05f, 0.9f));
	LoadoutBtn->OnClicked.AddUniqueDynamic(this, &UPFLobbyWidget::OnLoadoutClicked);
	UTextBlock* LoadoutLabel = WidgetTree->ConstructWidget<UTextBlock>();
	LoadoutLabel->SetText(FText::FromString(TEXT("LOADOUT")));
	LoadoutLabel->SetFont(PFLobbyFont(14, true));
	LoadoutLabel->SetJustification(ETextJustify::Center);
	LoadoutLabel->SetColorAndOpacity(FSlateColor(FLinearColor(1.f, 0.9f, 0.5f)));
	LoadoutBtn->SetContent(LoadoutLabel);
	if (UVerticalBoxSlot* VS = Box->AddChildToVerticalBox(LoadoutBtn)) { VS->SetPadding(FMargin(0.f, 10.f, 0.f, 2.f)); VS->SetHorizontalAlignment(HAlign_Fill); }

	UButton* OptionsBtn = WidgetTree->ConstructWidget<UButton>();
	OptionsBtn->SetBackgroundColor(FLinearColor(0.08f, 0.12f, 0.16f, 0.9f));
	OptionsBtn->OnClicked.AddUniqueDynamic(this, &UPFLobbyWidget::OnOptionsClicked);
	UTextBlock* OptionsLabel = WidgetTree->ConstructWidget<UTextBlock>();
	OptionsLabel->SetText(FText::FromString(TEXT("OPTIONS")));
	OptionsLabel->SetFont(PFLobbyFont(14, true));
	OptionsLabel->SetJustification(ETextJustify::Center);
	OptionsLabel->SetColorAndOpacity(FSlateColor(FLinearColor(0.75f, 0.88f, 1.f)));
	OptionsBtn->SetContent(OptionsLabel);
	if (UVerticalBoxSlot* VS = Box->AddChildToVerticalBox(OptionsBtn)) { VS->SetPadding(FMargin(0.f, 6.f, 0.f, 2.f)); VS->SetHorizontalAlignment(HAlign_Fill); }

	ConfigHintText = WidgetTree->ConstructWidget<UTextBlock>();
	ConfigHintText->SetFont(PFLobbyFont(11, false));
	ConfigHintText->SetColorAndOpacity(FSlateColor(FLinearColor(1.f, 1.f, 1.f, 0.5f)));
	if (UVerticalBoxSlot* VS = Box->AddChildToVerticalBox(ConfigHintText)) { VS->SetPadding(FMargin(0.f, 6.f, 0.f, 0.f)); }

	USizeBox* Sizer = WidgetTree->ConstructWidget<USizeBox>();
	Sizer->SetWidthOverride(250.f);
	Sizer->SetContent(Panel);
	if (UCanvasPanelSlot* CSlot = RootCanvas->AddChildToCanvas(Sizer))
	{
		CSlot->SetAnchors(FAnchors(0.f, 0.5f));
		CSlot->SetAlignment(FVector2D(0.f, 0.5f));
		CSlot->SetPosition(FVector2D(40.f, -20.f));
		CSlot->SetAutoSize(true);
	}
}

void UPFLobbyWidget::BuildLoadoutOverlay(UCanvasPanel* RootCanvas)
{
	LoadoutOverlay = WidgetTree->ConstructWidget<UBorder>();
	LoadoutOverlay->SetBrushColor(FLinearColor(0.01f, 0.01f, 0.02f, 0.96f));
	LoadoutOverlay->SetPadding(FMargin(24.f));
	LoadoutOverlay->SetHorizontalAlignment(HAlign_Center);
	LoadoutOverlay->SetVerticalAlignment(VAlign_Center);
	LoadoutOverlay->SetVisibility(ESlateVisibility::Collapsed);

	UVerticalBox* Box = WidgetTree->ConstructWidget<UVerticalBox>();
	LoadoutOverlay->SetContent(Box);

	UTextBlock* Title = WidgetTree->ConstructWidget<UTextBlock>();
	Title->SetText(FText::FromString(TEXT("LOADOUT")));
	Title->SetFont(PFLobbyFont(34, true));
	Title->SetJustification(ETextJustify::Center);
	Title->SetColorAndOpacity(FSlateColor(FLinearColor::White));
	if (UVerticalBoxSlot* VS = Box->AddChildToVerticalBox(Title)) { VS->SetHorizontalAlignment(HAlign_Center); }

	UTextBlock* Sub = WidgetTree->ConstructWidget<UTextBlock>();
	Sub->SetText(FText::FromString(TEXT("Weapons, markers and gear get equipped here.\nComing soon — nothing to customize yet.")));
	Sub->SetFont(PFLobbyFont(16, false));
	Sub->SetJustification(ETextJustify::Center);
	Sub->SetColorAndOpacity(FSlateColor(FLinearColor(1.f, 1.f, 1.f, 0.7f)));
	if (UVerticalBoxSlot* VS = Box->AddChildToVerticalBox(Sub)) { VS->SetPadding(FMargin(0.f, 16.f, 0.f, 24.f)); VS->SetHorizontalAlignment(HAlign_Center); }

	UButton* CloseBtn = WidgetTree->ConstructWidget<UButton>();
	CloseBtn->SetBackgroundColor(FLinearColor(0.12f, 0.13f, 0.16f, 1.f));
	CloseBtn->OnClicked.AddUniqueDynamic(this, &UPFLobbyWidget::OnLoadoutClose);
	UTextBlock* CloseLabel = WidgetTree->ConstructWidget<UTextBlock>();
	CloseLabel->SetText(FText::FromString(TEXT("BACK")));
	CloseLabel->SetFont(PFLobbyFont(16, true));
	CloseLabel->SetJustification(ETextJustify::Center);
	CloseLabel->SetColorAndOpacity(FSlateColor(FLinearColor::White));
	CloseBtn->SetContent(CloseLabel);
	USizeBox* CloseSizer = WidgetTree->ConstructWidget<USizeBox>();
	CloseSizer->SetWidthOverride(160.f);
	CloseSizer->SetContent(CloseBtn);
	if (UVerticalBoxSlot* VS = Box->AddChildToVerticalBox(CloseSizer)) { VS->SetHorizontalAlignment(HAlign_Center); }

	if (UCanvasPanelSlot* CSlot = RootCanvas->AddChildToCanvas(LoadoutOverlay))
	{
		CSlot->SetAnchors(FAnchors(0.f, 0.f, 1.f, 1.f));   // fill the viewport
		CSlot->SetOffsets(FMargin(0.f));
	}
}

void UPFLobbyWidget::OnLoadoutClicked()
{
	if (LoadoutOverlay) { LoadoutOverlay->SetVisibility(ESlateVisibility::Visible); }
}

void UPFLobbyWidget::OnLoadoutClose()
{
	if (LoadoutOverlay) { LoadoutOverlay->SetVisibility(ESlateVisibility::Collapsed); }
}

void UPFLobbyWidget::OnOptionsClicked()
{
	if (APaintForgePlayerController* PC = Cast<APaintForgePlayerController>(GetOwningPlayer()))
	{
		PC->ToggleOptionsMenu();
	}
}

void UPFLobbyWidget::RefreshConfig()
{
	const UWorld* World = GetWorld();
	const APaintForgeGameState* GS = World ? World->GetGameState<APaintForgeGameState>() : nullptr;
	if (!GS) { return; }
	if (ModeValueText)   { ModeValueText->SetText(FText::FromString(BuildModeLabel(GS->BuildMode))); }
	if (TypeValueText)   { TypeValueText->SetText(FText::FromString(MatchTypeLabel(GS->MatchType))); }
	if (FormatValueText)
	{
		FormatValueText->SetText(FText::FromString(
			FString::Printf(TEXT("%dv%d"), GS->TargetTeamSize, GS->TargetTeamSize)));
	}
	if (BotsValueText)
	{
		BotsValueText->SetText(FText::FromString(GS->bFillWithBots ? TEXT("On") : TEXT("Off")));
	}
	if (ConfigHintText)
	{
		const FString ModeBlurb = BuildModeBlurb(GS->BuildMode);
		const FString TypeBlurb = MatchTypeBlurb(GS->MatchType);
		FString Hint;
		if (!ModeBlurb.IsEmpty()) { Hint += ModeBlurb; }
		if (!TypeBlurb.IsEmpty())
		{
			if (!Hint.IsEmpty()) { Hint += TEXT("\n"); }
			Hint += TypeBlurb;
		}
		if (!Hint.IsEmpty()) { Hint += TEXT("\n"); }
		Hint += TEXT("Match setup was chosen on the pre-game menu");
		ConfigHintText->SetText(FText::FromString(Hint));
	}
}

void UPFLobbyWidget::NativeConstruct()
{
	Super::NativeConstruct();

	if (FooterText)
	{
		FString Hints = TEXT("F — Ready    ·    Esc — Options / How to Play    ·    Hold Tab — Cursor");
		if (IsLocalHost())
		{
			Hints += TEXT("    ·    Enter — Start Match    ·    Click a player to swap team");
		}
		FooterText->SetText(FText::FromString(Hints));
	}

	RefreshConfig(); // mode blurb + host line (GS may already be valid)

	PollAccum = PollInterval; // refresh on first tick
	LastRosterSignature.Reset();
}

void UPFLobbyWidget::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);

	PollAccum += InDeltaTime;
	if (PollAccum >= PollInterval)
	{
		PollAccum = 0.f;
		RefreshRoster();
		RefreshConfig();
	}

	// The loadout overlay only makes sense while the Tab-hold cursor exists. Collapse it the moment the
	// cursor goes away — releasing Tab, or a match cycle returning to the lobby — so a near-opaque
	// full-screen panel can never strand the view with no cursor left to press BACK.
	if (LoadoutOverlay && LoadoutOverlay->GetVisibility() != ESlateVisibility::Collapsed)
	{
		const APaintForgePlayerController* PC = Cast<APaintForgePlayerController>(GetOwningPlayer());
		if (!PC || !PC->IsScoreboardHeld())
		{
			LoadoutOverlay->SetVisibility(ESlateVisibility::Collapsed);
		}
	}

	// Lobby start countdown: PhaseEndServerTime == 0 means untimed lobby.
	if (CountdownText)
	{
		const UWorld* World = GetWorld();
		const APaintForgeGameState* GS = World ? World->GetGameState<APaintForgeGameState>() : nullptr;
		if (GS && GS->Phase == EPFMatchPhase::Lobby && GS->PhaseEndServerTime > 0.f)
		{
			const int32 Secs = FMath::Max(0, FMath::CeilToInt(GS->GetPhaseTimeRemaining()));
			CountdownText->SetText(FText::FromString(FString::Printf(TEXT("Match starting in %d..."), Secs)));
		}
		else
		{
			CountdownText->SetText(FText::GetEmpty());
		}
	}
}

bool UPFLobbyWidget::IsLocalHost() const
{
	const APlayerController* PC = GetOwningPlayer();
	return PC && PC->IsLocalController() && PC->HasAuthority();
}

void UPFLobbyWidget::NotifyRowClicked(APaintForgePlayerState* ClickedPlayerState)
{
	if (!ClickedPlayerState || !IsLocalHost())
	{
		return; // UX gate only — server re-validates host-ness (T22)
	}
	if (APaintForgePlayerController* PC = Cast<APaintForgePlayerController>(GetOwningPlayer()))
	{
		PC->ServerHostCycleTeam(ClickedPlayerState);
	}
}

void UPFLobbyWidget::RefreshRoster()
{
	if (!RosterBox || !WidgetTree)
	{
		return;
	}
	const UWorld* World = GetWorld();
	APaintForgeGameState* GS = World ? World->GetGameState<APaintForgeGameState>() : nullptr;
	if (!GS)
	{
		return;
	}

	// Stable display order: team A, team B, unassigned; then by name.
	// Skip ghost PlayerStates (no Controller) so disconnects don't leave a second un-ready row.
	TArray<APaintForgePlayerState*> Roster;
	for (APlayerState* PS : GS->PlayerArray)
	{
		APaintForgePlayerState* PFPS = Cast<APaintForgePlayerState>(PS);
		if (!PFPS)
		{
			continue;
		}
		const bool bLive = PFPS->IsABot()
			? (PFPS->GetOwningController() != nullptr)
			: (PFPS->GetPlayerController() != nullptr || PFPS->GetOwningController() != nullptr);
		if (!bLive)
		{
			continue;
		}
		Roster.Add(PFPS);
	}
	Roster.Sort([](const APaintForgePlayerState& A, const APaintForgePlayerState& B)
	{
		if (A.TeamId != B.TeamId)
		{
			return A.TeamId < B.TeamId;
		}
		return A.GetPlayerName() < B.GetPlayerName();
	});

	// Only rebuild the rows when something visible changed (protects in-flight clicks).
	FString Signature;
	for (const APaintForgePlayerState* PS : Roster)
	{
		Signature += FString::Printf(TEXT("%s|%d|%d;"), *PS->GetPlayerName(), PS->TeamId, PS->bReady ? 1 : 0);
	}
	if (Signature == LastRosterSignature)
	{
		return;
	}
	LastRosterSignature = Signature;

	RosterBox->ClearChildren();
	for (APaintForgePlayerState* PS : Roster)
	{
		UPFLobbyRowButton* Row = WidgetTree->ConstructWidget<UPFLobbyRowButton>();
		Row->InitRow(this, PS);
		Row->SetBackgroundColor(FLinearColor(0.10f, 0.11f, 0.13f, 0.9f));

		UHorizontalBox* RowBox = WidgetTree->ConstructWidget<UHorizontalBox>();

		// Team color chip.
		UImage* Chip = WidgetTree->ConstructWidget<UImage>();
		Chip->SetBrush(FSlateColorBrush(FLinearColor::White));
		Chip->SetColorAndOpacity(PS->TeamId <= 1 ? PFColors::ForTeam(PS->TeamId) : FLinearColor(0.4f, 0.4f, 0.4f));
		USizeBox* ChipSizer = WidgetTree->ConstructWidget<USizeBox>();
		ChipSizer->SetWidthOverride(18.f);
		ChipSizer->SetHeightOverride(18.f);
		ChipSizer->SetContent(Chip);
		if (UHorizontalBoxSlot* HSlot = RowBox->AddChildToHorizontalBox(ChipSizer))
		{
			HSlot->SetVerticalAlignment(VAlign_Center);
			HSlot->SetPadding(FMargin(4.f, 6.f, 12.f, 6.f));
		}

		// Player name.
		UTextBlock* NameText = WidgetTree->ConstructWidget<UTextBlock>();
		NameText->SetText(FText::FromString(PS->GetPlayerName()));
		NameText->SetFont(PFLobbyFont(16, false));
		NameText->SetColorAndOpacity(FSlateColor(FLinearColor::White));
		if (UHorizontalBoxSlot* HSlot = RowBox->AddChildToHorizontalBox(NameText))
		{
			HSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
			HSlot->SetVerticalAlignment(VAlign_Center);
		}

		// Ready check.
		UTextBlock* ReadyText = WidgetTree->ConstructWidget<UTextBlock>();
		ReadyText->SetText(FText::FromString(PS->bReady ? TEXT("READY") : TEXT("—")));
		ReadyText->SetFont(PFLobbyFont(14, true));
		ReadyText->SetColorAndOpacity(FSlateColor(PS->bReady
			? FLinearColor(0.2f, 0.9f, 0.3f)
			: FLinearColor(1.f, 1.f, 1.f, 0.35f)));
		if (UHorizontalBoxSlot* HSlot = RowBox->AddChildToHorizontalBox(ReadyText))
		{
			HSlot->SetVerticalAlignment(VAlign_Center);
			HSlot->SetPadding(FMargin(12.f, 6.f, 4.f, 6.f));
		}

		Row->SetContent(RowBox);

		if (UVerticalBoxSlot* VSlot = RosterBox->AddChildToVerticalBox(Row))
		{
			VSlot->SetPadding(FMargin(0.f, 2.f));
			VSlot->SetHorizontalAlignment(HAlign_Fill);
		}
	}
}
