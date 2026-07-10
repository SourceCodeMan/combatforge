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
}

void UPFLobbyWidget::NativeConstruct()
{
	Super::NativeConstruct();

	if (FooterText)
	{
		FString Hints = TEXT("F — Ready    ·    Hold Tab — Cursor");
		if (IsLocalHost())
		{
			Hints += TEXT("    ·    Enter — Start Match    ·    Click a player to swap team");
		}
		FooterText->SetText(FText::FromString(Hints));
	}

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
	TArray<APaintForgePlayerState*> Roster;
	for (APlayerState* PS : GS->PlayerArray)
	{
		if (APaintForgePlayerState* PFPS = Cast<APaintForgePlayerState>(PS))
		{
			Roster.Add(PFPS);
		}
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
