// Copyright (c) 2026 Tom Chapman. All rights reserved.

#include "UI/PFCombatHUDWidget.h"

#include "Core/PFUserPrefs.h"

#include "Combat/PFHealthComponent.h"
#include "Combat/PFWeaponComponent.h"
#include "Core/PaintForgeGameState.h"
#include "Core/PaintForgePlayerState.h"
#include "Player/PaintForgeCharacter.h"

#include "Blueprint/WidgetTree.h"
#include "Camera/CameraComponent.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/Image.h"
#include "Components/ProgressBar.h"
#include "Components/SizeBox.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Brushes/SlateColorBrush.h"
#include "GameFramework/PlayerController.h"
#include "Styling/CoreStyle.h"

namespace
{
	FSlateFontInfo PFCombatFont(int32 Size, bool bBold)
	{
		return FCoreStyle::GetDefaultFontStyle(bBold ? FName(TEXT("Bold")) : FName(TEXT("Regular")), Size);
	}

	UImage* MakeSolidImage(UWidgetTree* Tree, const FLinearColor& Color)
	{
		UImage* Img = Tree->ConstructWidget<UImage>();
		Img->SetBrush(FSlateColorBrush(FLinearColor::White));
		Img->SetColorAndOpacity(Color);
		return Img;
	}
}

TSharedRef<SWidget> UPFCombatHUDWidget::RebuildWidget()
{
	if (WidgetTree && !WidgetTree->RootWidget)
	{
		BuildTree(); // root assigned BEFORE Super::RebuildWidget (02 R3)
	}
	return Super::RebuildWidget();
}

void UPFCombatHUDWidget::BuildTree()
{
	UCanvasPanel* RootCanvas = WidgetTree->ConstructWidget<UCanvasPanel>();
	WidgetTree->RootWidget = RootCanvas;

	// ---- Crosshair: 4 lines + center dot, all center-anchored ----
	auto AddCrossPiece = [this, RootCanvas](const FVector2D& Size) -> UImage*
	{
		UImage* Img = MakeSolidImage(WidgetTree, FLinearColor(1.f, 1.f, 1.f, 0.9f));
		if (UCanvasPanelSlot* CSlot = RootCanvas->AddChildToCanvas(Img))
		{
			CSlot->SetAnchors(FAnchors(0.5f, 0.5f));
			CSlot->SetAlignment(FVector2D(0.5f, 0.5f));
			CSlot->SetPosition(FVector2D::ZeroVector);
			CSlot->SetSize(Size);
			CSlot->SetZOrder(5);
		}
		return Img;
	};
	CrossLineTop = AddCrossPiece(FVector2D(CrossLineThicknessPx, CrossLineLengthPx));
	CrossLineBottom = AddCrossPiece(FVector2D(CrossLineThicknessPx, CrossLineLengthPx));
	CrossLineLeft = AddCrossPiece(FVector2D(CrossLineLengthPx, CrossLineThicknessPx));
	CrossLineRight = AddCrossPiece(FVector2D(CrossLineLengthPx, CrossLineThicknessPx));
	CenterDot = AddCrossPiece(FVector2D(3.f, 3.f));

	// ---- Hopper + reload — bottom right ----
	HopperText = WidgetTree->ConstructWidget<UTextBlock>();
	HopperText->SetText(FText::FromString(TEXT("30 | 120")));
	HopperText->SetFont(PFCombatFont(22, true));
	HopperText->SetColorAndOpacity(FSlateColor(FLinearColor::White));
	HopperText->SetJustification(ETextJustify::Right);
	if (UCanvasPanelSlot* CSlot = RootCanvas->AddChildToCanvas(HopperText))
	{
		CSlot->SetAnchors(FAnchors(1.f, 1.f));
		CSlot->SetAlignment(FVector2D(1.f, 1.f));
		CSlot->SetPosition(FVector2D(-32.f, -56.f));
		CSlot->SetAutoSize(true);
	}

	ReloadBar = WidgetTree->ConstructWidget<UProgressBar>();
	ReloadBar->SetPercent(0.f);
	ReloadBar->SetFillColorAndOpacity(FLinearColor(1.f, 0.85f, 0.2f));
	ReloadBar->SetVisibility(ESlateVisibility::Hidden);
	if (UCanvasPanelSlot* CSlot = RootCanvas->AddChildToCanvas(ReloadBar))
	{
		CSlot->SetAnchors(FAnchors(1.f, 1.f));
		CSlot->SetAlignment(FVector2D(1.f, 1.f));
		CSlot->SetPosition(FVector2D(-32.f, -32.f));
		CSlot->SetSize(FVector2D(160.f, 8.f));
	}

	// Fire-mode + grenade indicators, stacked above the hopper (bottom-right).
	FireModeText = WidgetTree->ConstructWidget<UTextBlock>();
	FireModeText->SetText(FText::FromString(TEXT("AUTO")));
	FireModeText->SetFont(PFCombatFont(15, true));
	FireModeText->SetColorAndOpacity(FSlateColor(FLinearColor(0.85f, 0.9f, 1.f)));
	FireModeText->SetJustification(ETextJustify::Right);
	if (UCanvasPanelSlot* CSlot = RootCanvas->AddChildToCanvas(FireModeText))
	{
		CSlot->SetAnchors(FAnchors(1.f, 1.f));
		CSlot->SetAlignment(FVector2D(1.f, 1.f));
		CSlot->SetPosition(FVector2D(-32.f, -84.f));
		CSlot->SetAutoSize(true);
	}

	GrenadeText = WidgetTree->ConstructWidget<UTextBlock>();
	GrenadeText->SetText(FText::FromString(TEXT("FRAG 2   SMOKE 2")));
	GrenadeText->SetFont(PFCombatFont(15, true));
	GrenadeText->SetColorAndOpacity(FSlateColor(FLinearColor(1.f, 0.82f, 0.35f)));
	GrenadeText->SetJustification(ETextJustify::Right);
	if (UCanvasPanelSlot* CSlot = RootCanvas->AddChildToCanvas(GrenadeText))
	{
		CSlot->SetAnchors(FAnchors(1.f, 1.f));
		CSlot->SetAlignment(FVector2D(1.f, 1.f));
		CSlot->SetPosition(FVector2D(-32.f, -106.f));
		CSlot->SetAutoSize(true);
	}

	// ---- Top center block: pips / round number / timer / alive counts ----
	UVerticalBox* TopBox = WidgetTree->ConstructWidget<UVerticalBox>();

	UHorizontalBox* PipsRow = WidgetTree->ConstructWidget<UHorizontalBox>();
	auto AddPip = [this, PipsRow](TArray<TObjectPtr<UImage>>& OutPips, TArray<TObjectPtr<USizeBox>>& OutSizers)
	{
		UImage* Pip = MakeSolidImage(WidgetTree, FLinearColor(1.f, 1.f, 1.f, 0.12f));
		USizeBox* Sizer = WidgetTree->ConstructWidget<USizeBox>();
		Sizer->SetWidthOverride(16.f);
		Sizer->SetHeightOverride(10.f);
		Sizer->SetContent(Pip);
		if (UHorizontalBoxSlot* HSlot = PipsRow->AddChildToHorizontalBox(Sizer))
		{
			HSlot->SetPadding(FMargin(3.f, 0.f));
			HSlot->SetVerticalAlignment(VAlign_Center);
		}
		OutPips.Add(Pip);
		OutSizers.Add(Sizer);
	};
	for (int32 i = 0; i < MaxPips; ++i)
	{
		AddPip(PipsA, PipSizersA);
	}
	RoundNumberText = WidgetTree->ConstructWidget<UTextBlock>();
	RoundNumberText->SetFont(PFCombatFont(14, true));
	RoundNumberText->SetColorAndOpacity(FSlateColor(FLinearColor(1.f, 1.f, 1.f, 0.8f)));
	if (UHorizontalBoxSlot* HSlot = PipsRow->AddChildToHorizontalBox(RoundNumberText))
	{
		HSlot->SetPadding(FMargin(14.f, 0.f));
		HSlot->SetVerticalAlignment(VAlign_Center);
	}
	for (int32 i = 0; i < MaxPips; ++i)
	{
		AddPip(PipsB, PipSizersB);
	}
	if (UVerticalBoxSlot* VSlot = TopBox->AddChildToVerticalBox(PipsRow))
	{
		VSlot->SetHorizontalAlignment(HAlign_Center);
	}

	RoundTimerText = WidgetTree->ConstructWidget<UTextBlock>();
	RoundTimerText->SetFont(PFCombatFont(26, true));
	RoundTimerText->SetColorAndOpacity(FSlateColor(FLinearColor::White));
	RoundTimerText->SetJustification(ETextJustify::Center);
	if (UVerticalBoxSlot* VSlot = TopBox->AddChildToVerticalBox(RoundTimerText))
	{
		VSlot->SetHorizontalAlignment(HAlign_Center);
		VSlot->SetPadding(FMargin(0.f, 2.f));
	}

	UHorizontalBox* AliveRow = WidgetTree->ConstructWidget<UHorizontalBox>();
	AliveTextA = WidgetTree->ConstructWidget<UTextBlock>();
	AliveTextA->SetFont(PFCombatFont(16, true));
	AliveTextA->SetColorAndOpacity(FSlateColor(PFColors::ForTeam(0)));
	UTextBlock* AliveDash = WidgetTree->ConstructWidget<UTextBlock>();
	AliveDash->SetText(FText::FromString(TEXT("  —  ")));
	AliveDash->SetFont(PFCombatFont(16, false));
	AliveDash->SetColorAndOpacity(FSlateColor(FLinearColor(1.f, 1.f, 1.f, 0.6f)));
	AliveTextB = WidgetTree->ConstructWidget<UTextBlock>();
	AliveTextB->SetFont(PFCombatFont(16, true));
	AliveTextB->SetColorAndOpacity(FSlateColor(PFColors::ForTeam(1)));
	AliveRow->AddChildToHorizontalBox(AliveTextA);
	AliveRow->AddChildToHorizontalBox(AliveDash);
	AliveRow->AddChildToHorizontalBox(AliveTextB);
	if (UVerticalBoxSlot* VSlot = TopBox->AddChildToVerticalBox(AliveRow))
	{
		VSlot->SetHorizontalAlignment(HAlign_Center);
	}

	// Objective status (CTF carrier / Dom·HP on-point) sits under the score strip.
	ObjectiveStatusText = WidgetTree->ConstructWidget<UTextBlock>();
	ObjectiveStatusText->SetFont(PFCombatFont(15, true));
	ObjectiveStatusText->SetColorAndOpacity(FSlateColor(FLinearColor(1.f, 0.9f, 0.35f)));
	ObjectiveStatusText->SetJustification(ETextJustify::Center);
	ObjectiveStatusText->SetText(FText::GetEmpty());
	ObjectiveStatusText->SetVisibility(ESlateVisibility::Collapsed);
	if (UVerticalBoxSlot* VSlot = TopBox->AddChildToVerticalBox(ObjectiveStatusText))
	{
		VSlot->SetHorizontalAlignment(HAlign_Center);
		VSlot->SetPadding(FMargin(0.f, 6.f, 0.f, 0.f));
	}

	if (UCanvasPanelSlot* CSlot = RootCanvas->AddChildToCanvas(TopBox))
	{
		CSlot->SetAnchors(FAnchors(0.5f, 0.f));
		CSlot->SetAlignment(FVector2D(0.5f, 0.f));
		CSlot->SetPosition(FVector2D(0.f, 24.f));
		CSlot->SetAutoSize(true);
	}

	// ---- Banner (freeze / intermission / showdown) ----
	BannerText = WidgetTree->ConstructWidget<UTextBlock>();
	BannerText->SetFont(PFCombatFont(34, true));
	BannerText->SetColorAndOpacity(FSlateColor(FLinearColor::White));
	BannerText->SetJustification(ETextJustify::Center);
	BannerText->SetText(FText::GetEmpty());
	if (UCanvasPanelSlot* CSlot = RootCanvas->AddChildToCanvas(BannerText))
	{
		CSlot->SetAnchors(FAnchors(0.5f, 0.5f));
		CSlot->SetAlignment(FVector2D(0.5f, 0.5f));
		CSlot->SetPosition(FVector2D(0.f, -140.f));
		CSlot->SetAutoSize(true);
	}

	// ---- Own HP: 3 paint dots — bottom left ----
	UHorizontalBox* HPRow = WidgetTree->ConstructWidget<UHorizontalBox>();
	for (int32 i = 0; i < MaxHP; ++i)
	{
		UImage* Dot = MakeSolidImage(WidgetTree, FLinearColor(0.95f, 0.95f, 0.95f));
		USizeBox* Sizer = WidgetTree->ConstructWidget<USizeBox>();
		Sizer->SetWidthOverride(18.f);
		Sizer->SetHeightOverride(18.f);
		Sizer->SetContent(Dot);
		if (UHorizontalBoxSlot* HSlot = HPRow->AddChildToHorizontalBox(Sizer))
		{
			HSlot->SetPadding(FMargin(4.f, 0.f));
		}
		HPDots.Add(Dot);
	}
	if (UCanvasPanelSlot* CSlot = RootCanvas->AddChildToCanvas(HPRow))
	{
		CSlot->SetAnchors(FAnchors(0.f, 1.f));
		CSlot->SetAlignment(FVector2D(0.f, 1.f));
		CSlot->SetPosition(FVector2D(32.f, -40.f));
		CSlot->SetAutoSize(true);
	}

	// ---- Elim feed — top right, newest at the bottom ----
	UVerticalBox* FeedBox = WidgetTree->ConstructWidget<UVerticalBox>();
	for (int32 i = 0; i < FeedLineCount; ++i)
	{
		UTextBlock* Line = WidgetTree->ConstructWidget<UTextBlock>();
		Line->SetFont(PFCombatFont(13, false));
		Line->SetColorAndOpacity(FSlateColor(FLinearColor::White));
		Line->SetJustification(ETextJustify::Right);
		if (UVerticalBoxSlot* VSlot = FeedBox->AddChildToVerticalBox(Line))
		{
			VSlot->SetHorizontalAlignment(HAlign_Right);
			VSlot->SetPadding(FMargin(0.f, 1.f));
		}
		FeedLines.Add(Line);
	}
	if (UCanvasPanelSlot* CSlot = RootCanvas->AddChildToCanvas(FeedBox))
	{
		CSlot->SetAnchors(FAnchors(1.f, 0.f));
		CSlot->SetAlignment(FVector2D(1.f, 0.f));
		CSlot->SetPosition(FVector2D(-32.f, 32.f));
		CSlot->SetAutoSize(true);
	}

	// ---- "YOU'RE OUT" full-screen overlay (hidden until eliminated) ----
	OutDim = MakeSolidImage(WidgetTree, FLinearColor(0.02f, 0.02f, 0.04f, 0.72f));
	OutDim->SetVisibility(ESlateVisibility::Collapsed);
	if (UCanvasPanelSlot* CSlot = RootCanvas->AddChildToCanvas(OutDim))
	{
		CSlot->SetAnchors(FAnchors(0.f, 0.f, 1.f, 1.f));
		CSlot->SetOffsets(FMargin(0.f));
		CSlot->SetZOrder(40);
	}

	OutTitleText = WidgetTree->ConstructWidget<UTextBlock>();
	OutTitleText->SetText(FText::FromString(TEXT("YOU'RE OUT")));
	OutTitleText->SetFont(PFCombatFont(56, true));
	OutTitleText->SetColorAndOpacity(FSlateColor(FLinearColor(1.f, 0.92f, 0.35f)));
	OutTitleText->SetJustification(ETextJustify::Center);
	OutTitleText->SetVisibility(ESlateVisibility::Collapsed);
	if (UCanvasPanelSlot* CSlot = RootCanvas->AddChildToCanvas(OutTitleText))
	{
		CSlot->SetAnchors(FAnchors(0.5f, 0.5f));
		CSlot->SetAlignment(FVector2D(0.5f, 0.5f));
		CSlot->SetPosition(FVector2D(0.f, -28.f));
		CSlot->SetAutoSize(true);
		CSlot->SetZOrder(41);
	}

	OutSubtitleText = WidgetTree->ConstructWidget<UTextBlock>();
	OutSubtitleText->SetText(FText::GetEmpty());
	OutSubtitleText->SetFont(PFCombatFont(28, true));
	OutSubtitleText->SetColorAndOpacity(FSlateColor(FLinearColor(1.f, 1.f, 1.f, 0.95f)));
	OutSubtitleText->SetJustification(ETextJustify::Center);
	OutSubtitleText->SetVisibility(ESlateVisibility::Collapsed);
	if (UCanvasPanelSlot* CSlot = RootCanvas->AddChildToCanvas(OutSubtitleText))
	{
		CSlot->SetAnchors(FAnchors(0.5f, 0.5f));
		CSlot->SetAlignment(FVector2D(0.5f, 0.5f));
		CSlot->SetPosition(FVector2D(0.f, 36.f));
		CSlot->SetAutoSize(true);
		CSlot->SetZOrder(41);
	}
}

void UPFCombatHUDWidget::NativeConstruct()
{
	Super::NativeConstruct();
	TryBindGameState();
	HandleScoreChanged();
	HandleAliveCountsChanged();
	HandleElimFeedChanged();
	if (BoundGameState.IsValid())
	{
		HandleRoundStateChanged(BoundGameState->RoundState);
	}
}

void UPFCombatHUDWidget::NativeDestruct()
{
	UnbindPawn();
	if (BoundGameState.IsValid())
	{
		BoundGameState->OnMatchScoreChangedEvent.RemoveAll(this);
		BoundGameState->OnAliveCountsChangedEvent.RemoveAll(this);
		BoundGameState->OnElimFeedChangedEvent.RemoveAll(this);
		BoundGameState->OnRoundStateChangedEvent.RemoveAll(this);
		BoundGameState.Reset();
	}
	Super::NativeDestruct();
}

void UPFCombatHUDWidget::TryBindGameState()
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
		GS->OnMatchScoreChangedEvent.AddUObject(this, &UPFCombatHUDWidget::HandleScoreChanged);
		GS->OnAliveCountsChangedEvent.AddUObject(this, &UPFCombatHUDWidget::HandleAliveCountsChanged);
		GS->OnElimFeedChangedEvent.AddUObject(this, &UPFCombatHUDWidget::HandleElimFeedChanged);
		GS->OnRoundStateChangedEvent.AddUObject(this, &UPFCombatHUDWidget::HandleRoundStateChanged);
		HandleScoreChanged();
		HandleAliveCountsChanged();
		HandleElimFeedChanged();
		HandleRoundStateChanged(GS->RoundState);
	}
}

void UPFCombatHUDWidget::BindToPawn(APaintForgeCharacter* NewPawn)
{
	UnbindPawn();
	BoundPawn = NewPawn;
	if (NewPawn)
	{
		if (UPFWeaponComponent* Weapon = NewPawn->GetWeapon())
		{
			BoundWeapon = Weapon;
			Weapon->OnHopperChangedEvent.AddUObject(this, &UPFCombatHUDWidget::HandleHopperChanged);
			Weapon->OnReloadStateChangedEvent.AddUObject(this, &UPFCombatHUDWidget::HandleReloadStateChanged);
			Weapon->OnFireModeChangedEvent.AddUObject(this, &UPFCombatHUDWidget::HandleFireModeChanged);
			Weapon->OnGrenadeCountChangedEvent.AddUObject(this, &UPFCombatHUDWidget::HandleGrenadeCountChanged);
			HandleHopperChanged(Weapon->HopperCount);
			HandleReloadStateChanged(Weapon->bReloading);
			HandleFireModeChanged(Weapon->GetFireMode());
			HandleGrenadeCountChanged(Weapon->GetFragCount(), Weapon->GetSmokeCount());
		}
		if (UPFHealthComponent* Health = NewPawn->GetHealth())
		{
			BoundHealth = Health;
			Health->OnHPChangedEvent.AddUObject(this, &UPFCombatHUDWidget::HandleHPChanged);
			HandleHPChanged(Health->HP);
		}
	}
}

void UPFCombatHUDWidget::UnbindPawn()
{
	if (BoundWeapon.IsValid())
	{
		BoundWeapon->OnHopperChangedEvent.RemoveAll(this);
		BoundWeapon->OnReloadStateChangedEvent.RemoveAll(this);
		BoundWeapon->OnFireModeChangedEvent.RemoveAll(this);
		BoundWeapon->OnGrenadeCountChangedEvent.RemoveAll(this);
	}
	if (BoundHealth.IsValid())
	{
		BoundHealth->OnHPChangedEvent.RemoveAll(this);
	}
	BoundWeapon.Reset();
	BoundHealth.Reset();
	BoundPawn.Reset();
}

void UPFCombatHUDWidget::HandleScoreChanged()
{
	const APaintForgeGameState* GS = BoundGameState.Get();
	if (!GS)
	{
		return;
	}

	if (GS->MatchType == EPFMatchType::Skirmish
		|| GS->MatchType == EPFMatchType::CaptureFlag
		|| GS->MatchType == EPFMatchType::Domination
		|| GS->MatchType == EPFMatchType::Hardpoint)
	{
		// Continuous team-score modes: collapse round pips, show "first to N" + TeamScores in the
		// alive row. Timer is the match countdown via GetRoundTimeRemaining().
		UpdatePipVisibility(0);
		for (int32 i = 0; i < MaxPips; ++i)
		{
			if (PipsA.IsValidIndex(i) && PipsA[i]) { PipsA[i]->SetColorAndOpacity(FLinearColor(1.f, 1.f, 1.f, 0.12f)); }
			if (PipsB.IsValidIndex(i) && PipsB[i]) { PipsB[i]->SetColorAndOpacity(FLinearColor(1.f, 1.f, 1.f, 0.12f)); }
		}
		if (RoundNumberText)
		{
			const TCHAR* Prefix = TEXT("first to");
			if (GS->MatchType == EPFMatchType::CaptureFlag) { Prefix = TEXT("CTF · first to"); }
			else if (GS->MatchType == EPFMatchType::Domination) { Prefix = TEXT("DOM · first to"); }
			else if (GS->MatchType == EPFMatchType::Hardpoint) { Prefix = TEXT("HP · first to"); }
			RoundNumberText->SetText(FText::FromString(
				FString::Printf(TEXT("%s %d"), Prefix, GS->RoundWinsToTake)));
		}
		if (AliveTextA) { AliveTextA->SetText(FText::FromString(FString::Printf(TEXT("%d"), GS->TeamScores[0]))); }
		if (AliveTextB) { AliveTextB->SetText(FText::FromString(FString::Printf(TEXT("%d"), GS->TeamScores[1]))); }
		return;
	}

	if (GS->MatchType == EPFMatchType::FreeForAll)
	{
		// Solo leaderboard strip: "YOU N" / "LEAD M" + first-to-N. Pips collapsed.
		UpdatePipVisibility(0);
		for (int32 i = 0; i < MaxPips; ++i)
		{
			if (PipsA.IsValidIndex(i) && PipsA[i]) { PipsA[i]->SetColorAndOpacity(FLinearColor(1.f, 1.f, 1.f, 0.12f)); }
			if (PipsB.IsValidIndex(i) && PipsB[i]) { PipsB[i]->SetColorAndOpacity(FLinearColor(1.f, 1.f, 1.f, 0.12f)); }
		}
		if (RoundNumberText)
		{
			RoundNumberText->SetText(FText::FromString(FString::Printf(TEXT("FFA · first to %d"), GS->RoundWinsToTake)));
		}
		uint16 MyTags = 0;
		uint16 LeadTags = 0;
		if (const APaintForgePlayerState* LocalPS =
			GetOwningPlayer() ? GetOwningPlayer()->GetPlayerState<APaintForgePlayerState>() : nullptr)
		{
			MyTags = LocalPS->TagCount;
		}
		for (APlayerState* PSBase : GS->PlayerArray)
		{
			if (const APaintForgePlayerState* PS = Cast<APaintForgePlayerState>(PSBase))
			{
				LeadTags = FMath::Max(LeadTags, PS->TagCount);
			}
		}
		if (AliveTextA) { AliveTextA->SetText(FText::FromString(FString::Printf(TEXT("YOU %d"), MyTags))); }
		if (AliveTextB) { AliveTextB->SetText(FText::FromString(FString::Printf(TEXT("LEAD %d"), LeadTags))); }
		return;
	}

	// Effective wins-to-take is resolved server-side from the format and replicated on GameState
	// (RoundWinsToTake) — read it directly instead of inferring from the live (bot-padded /
	// leaver-shrunk) roster size. Never show fewer pips than a team already has wins.
	int32 EffectivePips = (GS->RoundWinsToTake > 0) ? GS->RoundWinsToTake : MaxPips;
	EffectivePips = FMath::Max3(EffectivePips,
		static_cast<int32>(GS->TeamRoundWins[0]), static_cast<int32>(GS->TeamRoundWins[1]));
	UpdatePipVisibility(FMath::Min(EffectivePips, MaxPips));

	for (int32 i = 0; i < MaxPips; ++i)
	{
		if (PipsA.IsValidIndex(i) && PipsA[i])
		{
			PipsA[i]->SetColorAndOpacity(i < GS->TeamRoundWins[0]
				? PFColors::ForTeam(0) : FLinearColor(1.f, 1.f, 1.f, 0.12f));
		}
		if (PipsB.IsValidIndex(i) && PipsB[i])
		{
			// Team B pips fill right-to-left so the fills grow toward the center.
			const int32 Wins = GS->TeamRoundWins[1];
			PipsB[i]->SetColorAndOpacity((MaxPips - 1 - i) < Wins
				? PFColors::ForTeam(1) : FLinearColor(1.f, 1.f, 1.f, 0.12f));
		}
	}
	if (RoundNumberText)
	{
		RoundNumberText->SetText(FText::FromString(FString::Printf(TEXT("R%d"), GS->RoundNumber)));
	}
}

void UPFCombatHUDWidget::UpdatePipVisibility(int32 EffectiveCount)
{
	// Both rows collapse their center-nearest pips, keeping the layout symmetric.
	// Team A fills outer->center from index 0, so its extras are the HIGH indices;
	// team B fills outer->center from index MaxPips-1, so its extras are the LOW
	// indices — collapsing them leaves both fill formulas untouched.
	for (int32 i = 0; i < MaxPips; ++i)
	{
		if (PipSizersA.IsValidIndex(i) && PipSizersA[i])
		{
			PipSizersA[i]->SetVisibility(i < EffectiveCount
				? ESlateVisibility::SelfHitTestInvisible : ESlateVisibility::Collapsed);
		}
		if (PipSizersB.IsValidIndex(i) && PipSizersB[i])
		{
			PipSizersB[i]->SetVisibility(i >= MaxPips - EffectiveCount
				? ESlateVisibility::SelfHitTestInvisible : ESlateVisibility::Collapsed);
		}
	}
}

void UPFCombatHUDWidget::HandleAliveCountsChanged()
{
	const APaintForgeGameState* GS = BoundGameState.Get();
	if (!GS)
	{
		return;
	}
	if (GS->MatchType == EPFMatchType::Skirmish
		|| GS->MatchType == EPFMatchType::FreeForAll
		|| GS->MatchType == EPFMatchType::CaptureFlag
		|| GS->MatchType == EPFMatchType::Domination
		|| GS->MatchType == EPFMatchType::Hardpoint)
	{
		return;   // tag / FFA / objective score row is owned by HandleScoreChanged
	}
	if (AliveTextA)
	{
		AliveTextA->SetText(FText::FromString(FString::Printf(TEXT("%d"), GS->AliveCounts[0])));
	}
	if (AliveTextB)
	{
		AliveTextB->SetText(FText::FromString(FString::Printf(TEXT("%d"), GS->AliveCounts[1])));
	}
}

void UPFCombatHUDWidget::HandleElimFeedChanged()
{
	const APaintForgeGameState* GS = BoundGameState.Get();
	if (!GS)
	{
		return;
	}
	const int32 Num = GS->ElimFeed.Num();
	for (int32 i = 0; i < FeedLineCount; ++i)
	{
		UTextBlock* Line = FeedLines.IsValidIndex(i) ? FeedLines[i].Get() : nullptr;
		if (!Line)
		{
			continue;
		}
		// Oldest of the visible window on top, newest at the bottom.
		const int32 FeedIdx = Num - FeedLineCount + i;
		if (FeedIdx >= 0 && FeedIdx < Num)
		{
			const FPFElimEntry& E = GS->ElimFeed[FeedIdx];
			Line->SetText(FText::FromString(FString::Printf(TEXT("%s splatted %s"), *E.ShooterName, *E.VictimName)));
			Line->SetColorAndOpacity(FSlateColor(PFColors::ForTeam(E.ShooterTeam)));
		}
		else
		{
			Line->SetText(FText::GetEmpty());
		}
	}
}

void UPFCombatHUDWidget::HandleRoundStateChanged(EPFRoundState NewState)
{
	if (NewState == EPFRoundState::Live && LastRoundState == EPFRoundState::Freeze)
	{
		BannerHoldRemaining = 1.f; // "GO!" flash at breakout
	}
	LastRoundState = NewState;
}

void UPFCombatHUDWidget::HandleHopperChanged(int32 NewCount)
{
	if (!HopperText)
	{
		return;
	}
	int32 Reserve = 0;
	if (const UPFWeaponComponent* W = BoundWeapon.Get())
	{
		Reserve = W->ReserveAmmo;
		NewCount = W->HopperCount;
	}
	// Mag | reserve (e.g. "30 | 120")
	HopperText->SetText(FText::FromString(FString::Printf(TEXT("%d | %d"), NewCount, Reserve)));
}

void UPFCombatHUDWidget::HandleReloadStateChanged(bool bNowReloading)
{
	bReloading = bNowReloading;
	ReloadElapsed = 0.f;
	if (ReloadBar)
	{
		ReloadBar->SetPercent(0.f);
		ReloadBar->SetVisibility(bNowReloading ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Hidden);
	}
}

void UPFCombatHUDWidget::HandleFireModeChanged(EPFFireMode NewMode)
{
	if (!FireModeText)
	{
		return;
	}
	const TCHAR* Name =
		(NewMode == EPFFireMode::Single) ? TEXT("SINGLE") :
		(NewMode == EPFFireMode::Burst)  ? TEXT("BURST")  : TEXT("AUTO");
	FireModeText->SetText(FText::FromString(Name));
}

void UPFCombatHUDWidget::HandleGrenadeCountChanged(uint8 Frag, uint8 Smoke)
{
	if (!GrenadeText)
	{
		return;
	}
	GrenadeText->SetText(FText::FromString(FString::Printf(TEXT("FRAG %d   SMOKE %d"), Frag, Smoke)));
}

void UPFCombatHUDWidget::HandleHPChanged(uint8 NewHP)
{
	for (int32 i = 0; i < MaxHP; ++i)
	{
		if (HPDots.IsValidIndex(i) && HPDots[i])
		{
			HPDots[i]->SetColorAndOpacity(i < NewHP
				? FLinearColor(0.95f, 0.95f, 0.95f)
				: FLinearColor(0.1f, 0.1f, 0.1f, 0.6f));
		}
	}
}

void UPFCombatHUDWidget::UpdateCrosshair()
{
	const APaintForgeCharacter* Pawn = BoundPawn.Get();
	const UPFWeaponComponent* Weapon = BoundWeapon.Get();
	const APlayerController* PC = GetOwningPlayer();
	if (!Pawn || !Weapon || !PC)
	{
		return;
	}

	// While eliminated / out UI is up, keep the reticle hidden.
	const bool bOut =
		(BoundHealth.IsValid() && BoundHealth->bEliminated)
		|| (PC->GetPlayerState<APaintForgePlayerState>()
			&& PC->GetPlayerState<APaintForgePlayerState>()->OutKind != 0);
	if (bOut)
	{
		const ESlateVisibility Hidden = ESlateVisibility::Hidden;
		if (CrossLineTop)    { CrossLineTop->SetVisibility(Hidden); }
		if (CrossLineBottom) { CrossLineBottom->SetVisibility(Hidden); }
		if (CrossLineLeft)   { CrossLineLeft->SetVisibility(Hidden); }
		if (CrossLineRight)  { CrossLineRight->SetVisibility(Hidden); }
		if (CenterDot)       { CenterDot->SetVisibility(Hidden); }
		return;
	}
	// Loadout crosshair: 0=cross+dot, 1=dot only, 2=cross only.
	const int32 CrossStyle = FPFUserPrefs::GetCrosshairStyle();
	const bool bShowDot = (CrossStyle != 2);
	const bool bShowLines = (CrossStyle != 1);

	if (CenterDot)
	{
		CenterDot->SetVisibility(bShowDot ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Hidden);
	}

	const bool bADS = Pawn->IsADS();
	const ESlateVisibility LinesVis = (bADS || !bShowLines)
		? ESlateVisibility::Hidden
		: ESlateVisibility::HitTestInvisible;
	if (CrossLineTop)    { CrossLineTop->SetVisibility(LinesVis); }
	if (CrossLineBottom) { CrossLineBottom->SetVisibility(LinesVis); }
	if (CrossLineLeft)   { CrossLineLeft->SetVisibility(LinesVis); }
	if (CrossLineRight)  { CrossLineRight->SetVisibility(LinesVis); }

	if (CenterDot && bShowDot)
	{
		if (UCanvasPanelSlot* DotSlot = Cast<UCanvasPanelSlot>(CenterDot->Slot))
		{
			// ADS shows a 2 px dot only (04 §4).
			DotSlot->SetSize(bADS ? FVector2D(2.f, 2.f) : FVector2D(3.f, 3.f));
		}
	}
	if (bADS || !bShowLines)
	{
		return;
	}

	const float SpreadDeg = Weapon->GetCurrentSpreadHalfAngleDeg();
	const UCameraComponent* Camera = Pawn->GetFirstPersonCamera();
	const float FOV = Camera ? Camera->FieldOfView : 105.f;

	int32 ViewX = 0;
	int32 ViewY = 0;
	PC->GetViewportSize(ViewX, ViewY);
	if (ViewX <= 0)
	{
		ViewX = 1920;
	}

	// Contract §3.6 (ERRATUM 2026-07-10): gap px = 6 + tan(spreadHalfAngle)/tan(FOV/2)
	// · (ViewportW/2). The original formula's stray leading 40· factor was struck by the
	// erratum; the ratified form is the true screen projection of the spread half-angle
	// plus the 6 px base gap (CrosshairBaseGapPx).
	const float Projected = FMath::Tan(FMath::DegreesToRadians(SpreadDeg))
		/ FMath::Tan(FMath::DegreesToRadians(FOV * 0.5f))
		* (static_cast<float>(ViewX) * 0.5f);
	const float Gap = CrosshairBaseGapPx + Projected;
	const float LineCenterOffset = Gap + CrossLineLengthPx * 0.5f;

	auto SetLinePos = [](UImage* Img, const FVector2D& Pos)
	{
		if (Img)
		{
			if (UCanvasPanelSlot* CSlot = Cast<UCanvasPanelSlot>(Img->Slot))
			{
				CSlot->SetPosition(Pos);
			}
		}
	};
	SetLinePos(CrossLineTop, FVector2D(0.f, -LineCenterOffset));
	SetLinePos(CrossLineBottom, FVector2D(0.f, LineCenterOffset));
	SetLinePos(CrossLineLeft, FVector2D(-LineCenterOffset, 0.f));
	SetLinePos(CrossLineRight, FVector2D(LineCenterOffset, 0.f));
}

void UPFCombatHUDWidget::UpdateBanner(float InDeltaTime)
{
	if (!BannerText)
	{
		return;
	}
	const APaintForgeGameState* GS = BoundGameState.Get();
	if (!GS)
	{
		BannerText->SetText(FText::GetEmpty());
		return;
	}

	FString Banner;
	switch (GS->RoundState)
	{
	case EPFRoundState::Freeze:
	{
		const int32 Secs = FMath::Max(0, FMath::CeilToInt(GS->GetRoundTimeRemaining()));
		// Non-lethal copy: SHOWDOWN (not "sudden death") — kids/airsoft tone (roadmap M2).
		Banner = GS->bSuddenDeath
			? FString::Printf(TEXT("SHOWDOWN — %d"), Secs)
			: FString::Printf(TEXT("GET READY — %d"), Secs);
		break;
	}
	case EPFRoundState::Live:
		if (BannerHoldRemaining > 0.f)
		{
			BannerHoldRemaining = FMath::Max(0.f, BannerHoldRemaining - InDeltaTime);
			Banner = TEXT("GO!");
		}
		else if (GS->bSuddenDeath)
		{
			Banner = TEXT("SHOWDOWN");
		}
		break;
	case EPFRoundState::Intermission:
		Banner = TEXT("ROUND OVER");
		break;
	default:
		break;
	}
	BannerText->SetText(FText::FromString(Banner));
}

void UPFCombatHUDWidget::UpdateOutOverlay()
{
	auto HideOut = [this]()
	{
		if (OutDim) { OutDim->SetVisibility(ESlateVisibility::Collapsed); }
		if (OutTitleText) { OutTitleText->SetVisibility(ESlateVisibility::Collapsed); }
		if (OutSubtitleText)
		{
			OutSubtitleText->SetVisibility(ESlateVisibility::Collapsed);
			OutSubtitleText->SetText(FText::GetEmpty());
		}
	};

	const UPFHealthComponent* Health = BoundHealth.Get();
	const APaintForgePlayerState* LocalPS =
		GetOwningPlayer() ? GetOwningPlayer()->GetPlayerState<APaintForgePlayerState>() : nullptr;

	// Show only for the local human while their pawn is eliminated or PS says they're out.
	const bool bHealthOut = Health != nullptr && Health->bEliminated;
	const uint8 OutKind = LocalPS ? LocalPS->OutKind : 0;
	const bool bPSOut = OutKind == 1 || OutKind == 2;
	if (!bHealthOut && !bPSOut)
	{
		HideOut();
		return;
	}

	if (OutDim) { OutDim->SetVisibility(ESlateVisibility::HitTestInvisible); }
	if (OutTitleText)
	{
		OutTitleText->SetText(FText::FromString(TEXT("YOU'RE OUT")));
		OutTitleText->SetVisibility(ESlateVisibility::HitTestInvisible);
	}

	FString Subtitle;
	if (OutKind == 2 || (LocalPS && !LocalPS->bAliveInRound && OutKind != 1))
	{
		Subtitle = TEXT("Out for this round — spectating");
	}
	else if (OutKind == 1 && LocalPS && BoundGameState.IsValid())
	{
		const float Remaining = LocalPS->RespawnAtServerTime
			- BoundGameState->GetServerWorldTimeSeconds();
		const int32 Secs = FMath::Max(0, FMath::CeilToInt(Remaining));
		if (Secs > 0)
		{
			Subtitle = FString::Printf(TEXT("Respawning in %d…"), Secs);
		}
		else
		{
			Subtitle = TEXT("Respawning…");
		}
	}
	else if (bHealthOut)
	{
		// Eliminated but PS stamp not in yet — keep the board up so kids still know.
		Subtitle = TEXT("Waiting to respawn…");
	}

	if (OutSubtitleText)
	{
		OutSubtitleText->SetText(FText::FromString(Subtitle));
		OutSubtitleText->SetVisibility(
			Subtitle.IsEmpty() ? ESlateVisibility::Collapsed : ESlateVisibility::HitTestInvisible);
	}

}

void UPFCombatHUDWidget::UpdateObjectiveStatus()
{
	if (!ObjectiveStatusText)
	{
		return;
	}
	const APaintForgeGameState* GS = BoundGameState.Get();
	const APaintForgePlayerState* LocalPS =
		GetOwningPlayer() ? GetOwningPlayer()->GetPlayerState<APaintForgePlayerState>() : nullptr;
	if (!GS || !LocalPS || GS->Phase != EPFMatchPhase::Combat || GS->RoundState != EPFRoundState::Live)
	{
		ObjectiveStatusText->SetVisibility(ESlateVisibility::Collapsed);
		ObjectiveStatusText->SetText(FText::GetEmpty());
		return;
	}

	FString Status;
	FLinearColor Color(1.f, 0.9f, 0.35f);

	if (GS->MatchType == EPFMatchType::CaptureFlag)
	{
		if (LocalPS->bCarryingFlag && LocalPS->CarriedFlagTeam <= 1)
		{
			Status = TEXT("⚑ FLAG — return to your base");
			Color = PFColors::ForTeam(LocalPS->CarriedFlagTeam);
		}
	}
	else if (GS->MatchType == EPFMatchType::Domination || GS->MatchType == EPFMatchType::Hardpoint)
	{
		if (LocalPS->StandingOnPoint != 255)
		{
			static const TCHAR* PointNames[] = { TEXT("A"), TEXT("B"), TEXT("C") };
			const int32 Idx = FMath::Clamp(static_cast<int32>(LocalPS->StandingOnPoint), 0, 2);
			if (GS->MatchType == EPFMatchType::Hardpoint)
			{
				Status = FString::Printf(TEXT("● HARDPOINT %s"), PointNames[Idx]);
			}
			else
			{
				Status = FString::Printf(TEXT("● POINT %s"), PointNames[Idx]);
			}
			Color = (LocalPS->TeamId <= 1)
				? PFColors::ForTeam(LocalPS->TeamId)
				: FLinearColor(1.f, 0.9f, 0.35f);
		}
	}

	if (Status.IsEmpty())
	{
		ObjectiveStatusText->SetVisibility(ESlateVisibility::Collapsed);
		ObjectiveStatusText->SetText(FText::GetEmpty());
		return;
	}
	ObjectiveStatusText->SetVisibility(ESlateVisibility::HitTestInvisible);
	ObjectiveStatusText->SetText(FText::FromString(Status));
	ObjectiveStatusText->SetColorAndOpacity(FSlateColor(Color));
}

void UPFCombatHUDWidget::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);

	TryBindGameState(); // GameState can arrive late on clients

	UpdateOutOverlay();
	UpdateCrosshair();
	UpdateBanner(InDeltaTime);
	UpdateObjectiveStatus();

	if (const APaintForgeGameState* GS = BoundGameState.Get())
	{
		if (RoundTimerText)
		{
			const int32 Secs = FMath::Max(0, FMath::CeilToInt(GS->GetRoundTimeRemaining()));
			RoundTimerText->SetText(FText::FromString(FString::Printf(TEXT("%d:%02d"), Secs / 60, Secs % 60)));
			RoundTimerText->SetColorAndOpacity(FSlateColor(
				(GS->RoundState == EPFRoundState::Live && Secs <= 10)
					? FLinearColor(0.95f, 0.15f, 0.1f)
					: FLinearColor::White));
		}
		// FreeForAll TagCount rides PlayerState OnRep (no GameState score event) — refresh the
		// YOU/LEAD strip here so clients stay live without a dedicated multicast.
		// Objective PS flags (carrier / on-point) also lack a GS multicast — keep strip live.
		if (GS->MatchType == EPFMatchType::FreeForAll
			|| GS->MatchType == EPFMatchType::CaptureFlag
			|| GS->MatchType == EPFMatchType::Domination
			|| GS->MatchType == EPFMatchType::Hardpoint)
		{
			HandleScoreChanged();
		}
	}

	if (bReloading && ReloadBar)
	{
		const UPFWeaponComponent* Weapon = BoundWeapon.Get();
		const float ReloadTime = (Weapon && Weapon->ReloadTime > 0.f) ? Weapon->ReloadTime : 1.f;
		ReloadElapsed += InDeltaTime;
		ReloadBar->SetPercent(FMath::Clamp(ReloadElapsed / ReloadTime, 0.f, 1.f));
	}
}
