// Copyright (c) 2026 Tom Chapman. All rights reserved.

#include "UI/PFCombatHUDWidget.h"

#include "Audio/PFMusicSubsystem.h"
#include "Core/PFUserPrefs.h"

#include "Combat/PFHealthComponent.h"
#include "Combat/PFWeaponComponent.h"
#include "Core/CombatForgeGameState.h"
#include "Core/CombatForgePlayerState.h"
#include "Objectives/PFControlPointActor.h"
#include "Player/CombatForgeCharacter.h"

#include "EngineUtils.h"

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
	// Soft ring at projected spread when bloom is near cap ("ease off the trigger").
	CrosshairHalo = MakeSolidImage(WidgetTree, FLinearColor(1.f, 1.f, 1.f, 0.22f));
	if (CrosshairHalo)
	{
		if (UCanvasPanelSlot* CSlot = RootCanvas->AddChildToCanvas(CrosshairHalo))
		{
			CSlot->SetAnchors(FAnchors(0.5f, 0.5f));
			CSlot->SetAlignment(FVector2D(0.5f, 0.5f));
			CSlot->SetPosition(FVector2D::ZeroVector);
			CSlot->SetSize(FVector2D(40.f, 40.f));
			CSlot->SetZOrder(4);
		}
		CrosshairHalo->SetVisibility(ESlateVisibility::Hidden);
	}

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
	GrenadeText->SetText(FText::FromString(TEXT("FRAG 6   SMOKE 6")));
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

	// Contextual interact prompt (center, under reticle) — doors, etc.
	InteractPromptText = WidgetTree->ConstructWidget<UTextBlock>();
	InteractPromptText->SetText(FText::GetEmpty());
	InteractPromptText->SetFont(PFCombatFont(18, true));
	InteractPromptText->SetColorAndOpacity(FSlateColor(FLinearColor(1.f, 1.f, 1.f, 0.92f)));
	InteractPromptText->SetJustification(ETextJustify::Center);
	InteractPromptText->SetVisibility(ESlateVisibility::Collapsed);
	if (UCanvasPanelSlot* CSlot = RootCanvas->AddChildToCanvas(InteractPromptText))
	{
		CSlot->SetAnchors(FAnchors(0.5f, 0.5f));
		CSlot->SetAlignment(FVector2D(0.5f, 0.f));
		CSlot->SetPosition(FVector2D(0.f, 48.f));   // just below the crosshair
		CSlot->SetAutoSize(true);
		CSlot->SetZOrder(25);
	}

	// Ammo-barrel cooldown toast (lower-right, above hopper stack) — private, ~2 s fade.
	BarrelCooldownText = WidgetTree->ConstructWidget<UTextBlock>();
	BarrelCooldownText->SetText(FText::GetEmpty());
	BarrelCooldownText->SetFont(PFCombatFont(16, true));
	BarrelCooldownText->SetColorAndOpacity(FSlateColor(FLinearColor(1.f, 0.85f, 0.4f, 0.95f)));
	BarrelCooldownText->SetJustification(ETextJustify::Right);
	BarrelCooldownText->SetVisibility(ESlateVisibility::Collapsed);
	if (UCanvasPanelSlot* CSlot = RootCanvas->AddChildToCanvas(BarrelCooldownText))
	{
		CSlot->SetAnchors(FAnchors(1.f, 1.f));
		CSlot->SetAlignment(FVector2D(1.f, 1.f));
		CSlot->SetPosition(FVector2D(-32.f, -150.f));
		CSlot->SetAutoSize(true);
		CSlot->SetZOrder(20);
	}

	// Bomb charge from mid-field pickup (hidden until claimed).
	BombCarryText = WidgetTree->ConstructWidget<UTextBlock>();
	BombCarryText->SetText(FText::FromString(TEXT("BOMB · G")));
	BombCarryText->SetFont(PFCombatFont(15, true));
	BombCarryText->SetColorAndOpacity(FSlateColor(FLinearColor(1.f, 0.35f, 0.12f)));
	BombCarryText->SetJustification(ETextJustify::Right);
	BombCarryText->SetVisibility(ESlateVisibility::Collapsed);
	if (UCanvasPanelSlot* CSlot = RootCanvas->AddChildToCanvas(BombCarryText))
	{
		CSlot->SetAnchors(FAnchors(1.f, 1.f));
		CSlot->SetAlignment(FVector2D(1.f, 1.f));
		CSlot->SetPosition(FVector2D(-32.f, -128.f));
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

	// Final-30s countdown: its own canvas slot (not TopBox) so the per-second scale pop never
	// reflows the pips/alive strip. Hidden until the live round timer crosses 30 s.
	FinalCountdownText = WidgetTree->ConstructWidget<UTextBlock>();
	FinalCountdownText->SetFont(PFCombatFont(54, true));
	FinalCountdownText->SetColorAndOpacity(FSlateColor(FLinearColor(0.98f, 0.25f, 0.12f)));
	FinalCountdownText->SetJustification(ETextJustify::Center);
	FinalCountdownText->SetRenderTransformPivot(FVector2D(0.5f, 0.5f));
	FinalCountdownText->SetVisibility(ESlateVisibility::Collapsed);
	if (UCanvasPanelSlot* CSlot = RootCanvas->AddChildToCanvas(FinalCountdownText))
	{
		CSlot->SetAnchors(FAnchors(0.5f, 0.f));
		CSlot->SetAlignment(FVector2D(0.5f, 0.f));
		CSlot->SetPosition(FVector2D(0.f, 148.f));   // clear of the pips/timer/alive strip
		CSlot->SetAutoSize(true);
		CSlot->SetZOrder(30);
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

	// Domination zone chips: A / B / C, letter tinted by owner, "▰▰▱▱" segment bar while a capture runs
	// (the CoD letter-icon-fills-with-capturing-color idea, in text form to match the HUD style).
	ZoneChipsRow = WidgetTree->ConstructWidget<UHorizontalBox>();
	ZoneChips.Reset();
	for (int32 i = 0; i < 3; ++i)
	{
		UTextBlock* Chip = WidgetTree->ConstructWidget<UTextBlock>();
		Chip->SetFont(PFCombatFont(17, true));
		Chip->SetJustification(ETextJustify::Center);
		Chip->SetText(FText::GetEmpty());
		if (UHorizontalBoxSlot* HSlot = ZoneChipsRow->AddChildToHorizontalBox(Chip))
		{
			HSlot->SetPadding(FMargin(12.f, 0.f));
		}
		ZoneChips.Add(Chip);
	}
	ZoneChipsRow->SetVisibility(ESlateVisibility::Collapsed);
	if (UVerticalBoxSlot* VSlot = TopBox->AddChildToVerticalBox(ZoneChipsRow))
	{
		VSlot->SetHorizontalAlignment(HAlign_Center);
		VSlot->SetPadding(FMargin(0.f, 4.f, 0.f, 0.f));
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

	// ---- Own hits: 10 total-hit pips + region readout — bottom left (locational model) ----
	UVerticalBox* HitsBox = WidgetTree->ConstructWidget<UVerticalBox>();
	UHorizontalBox* HPRow = WidgetTree->ConstructWidget<UHorizontalBox>();
	for (int32 i = 0; i < MaxHitPips; ++i)
	{
		UImage* Dot = MakeSolidImage(WidgetTree, FLinearColor(0.95f, 0.95f, 0.95f));
		USizeBox* Sizer = WidgetTree->ConstructWidget<USizeBox>();
		Sizer->SetWidthOverride(12.f);   // 10 pips need a tighter cell than the old 3×18 dots
		Sizer->SetHeightOverride(12.f);
		Sizer->SetContent(Dot);
		if (UHorizontalBoxSlot* HSlot = HPRow->AddChildToHorizontalBox(Sizer))
		{
			HSlot->SetPadding(FMargin(3.f, 0.f));
		}
		HPDots.Add(Dot);
	}
	HitsBox->AddChildToVerticalBox(HPRow);
	RegionHitsText = WidgetTree->ConstructWidget<UTextBlock>();
	RegionHitsText->SetFont(PFCombatFont(13, false));
	RegionHitsText->SetColorAndOpacity(FSlateColor(FLinearColor(0.85f, 0.85f, 0.85f)));
	RegionHitsText->SetText(FText::GetEmpty());
	if (UVerticalBoxSlot* VSlot = HitsBox->AddChildToVerticalBox(RegionHitsText))
	{
		VSlot->SetPadding(FMargin(3.f, 4.f, 0.f, 0.f));
	}
	if (UCanvasPanelSlot* CSlot = RootCanvas->AddChildToCanvas(HitsBox))
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

	OutClassText = WidgetTree->ConstructWidget<UTextBlock>();
	OutClassText->SetText(FText::GetEmpty());
	OutClassText->SetFont(PFCombatFont(18, true));
	OutClassText->SetColorAndOpacity(FSlateColor(FLinearColor(0.75f, 0.9f, 1.f, 0.95f)));
	OutClassText->SetJustification(ETextJustify::Center);
	OutClassText->SetVisibility(ESlateVisibility::Collapsed);
	if (UCanvasPanelSlot* CSlot = RootCanvas->AddChildToCanvas(OutClassText))
	{
		CSlot->SetAnchors(FAnchors(0.5f, 0.5f));
		CSlot->SetAlignment(FVector2D(0.5f, 0.5f));
		CSlot->SetPosition(FVector2D(0.f, 92.f));
		CSlot->SetAutoSize(true);
		CSlot->SetZOrder(41);
	}

	// ---- Domination capture meter (CoD "Capture Meter": linear bar, lower-center, above the weapon HUD;
	//      shown while the local player stands in a zone with capture activity) ----
	CaptureBarLabel = WidgetTree->ConstructWidget<UTextBlock>();
	CaptureBarLabel->SetFont(PFCombatFont(16, true));
	CaptureBarLabel->SetJustification(ETextJustify::Center);
	CaptureBarLabel->SetText(FText::GetEmpty());
	CaptureBarLabel->SetVisibility(ESlateVisibility::Collapsed);
	if (UCanvasPanelSlot* CSlot = RootCanvas->AddChildToCanvas(CaptureBarLabel))
	{
		CSlot->SetAnchors(FAnchors(0.5f, 1.f));
		CSlot->SetAlignment(FVector2D(0.5f, 1.f));
		CSlot->SetPosition(FVector2D(0.f, -196.f));
		CSlot->SetAutoSize(true);
	}
	CaptureBar = WidgetTree->ConstructWidget<UProgressBar>();
	CaptureBar->SetPercent(0.f);
	CaptureBar->SetVisibility(ESlateVisibility::Collapsed);
	if (UCanvasPanelSlot* CSlot = RootCanvas->AddChildToCanvas(CaptureBar))
	{
		CSlot->SetAnchors(FAnchors(0.5f, 1.f));
		CSlot->SetAlignment(FVector2D(0.5f, 1.f));
		CSlot->SetPosition(FVector2D(0.f, -172.f));
		CSlot->SetSize(FVector2D(340.f, 16.f));
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
	ACombatForgeGameState* GS = World ? World->GetGameState<ACombatForgeGameState>() : nullptr;
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

void UPFCombatHUDWidget::BindToPawn(ACombatForgeCharacter* NewPawn)
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
			Health->OnHitsChangedEvent.AddUObject(this, &UPFCombatHUDWidget::HandleHitsChanged);
			HandleHitsChanged(Health->HeadHits, Health->ChestHits, Health->LimbHits, Health->TotalHits);
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
		BoundHealth->OnHitsChangedEvent.RemoveAll(this);
	}
	BoundWeapon.Reset();
	BoundHealth.Reset();
	BoundPawn.Reset();
}

void UPFCombatHUDWidget::HandleScoreChanged()
{
	const ACombatForgeGameState* GS = BoundGameState.Get();
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
		if (const ACombatForgePlayerState* LocalPS =
			GetOwningPlayer() ? GetOwningPlayer()->GetPlayerState<ACombatForgePlayerState>() : nullptr)
		{
			MyTags = LocalPS->TagCount;
		}
		for (APlayerState* PSBase : GS->PlayerArray)
		{
			if (const ACombatForgePlayerState* PS = Cast<ACombatForgePlayerState>(PSBase);
				PS && !PS->IsPhantom())
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
	const ACombatForgeGameState* GS = BoundGameState.Get();
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
	const ACombatForgeGameState* GS = BoundGameState.Get();
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

void UPFCombatHUDWidget::UpdateBombCarryIndicator()
{
	if (!BombCarryText)
	{
		return;
	}
	const ACombatForgeCharacter* Char = BoundPawn.Get();
	const bool bShow = (Char != nullptr && Char->IsCarryingBomb());
	BombCarryText->SetVisibility(bShow
		? ESlateVisibility::HitTestInvisible
		: ESlateVisibility::Collapsed);
}

void UPFCombatHUDWidget::UpdateInteractPrompt()
{
	if (!InteractPromptText)
	{
		return;
	}
	// Hide while out / eliminated overlay is up.
	if (BoundHealth.IsValid() && BoundHealth->bEliminated)
	{
		InteractPromptText->SetVisibility(ESlateVisibility::Collapsed);
		return;
	}
	const ACombatForgeCharacter* Char = BoundPawn.Get();
	const FString Prompt = Char ? Char->GetInteractPromptText() : FString();
	if (Prompt.IsEmpty())
	{
		InteractPromptText->SetVisibility(ESlateVisibility::Collapsed);
		return;
	}
	InteractPromptText->SetText(FText::FromString(Prompt));
	InteractPromptText->SetVisibility(ESlateVisibility::HitTestInvisible);
}

void UPFCombatHUDWidget::UpdateBarrelCooldownNotice()
{
	if (!BarrelCooldownText)
	{
		return;
	}
	const ACombatForgeCharacter* Char = BoundPawn.Get();
	const FString Notice = Char ? Char->GetBarrelCooldownNoticeText() : FString();
	if (Notice.IsEmpty())
	{
		BarrelCooldownText->SetVisibility(ESlateVisibility::Collapsed);
		return;
	}
	const float Alpha = Char ? Char->GetBarrelCooldownNoticeAlpha() : 0.f;
	BarrelCooldownText->SetText(FText::FromString(Notice));
	BarrelCooldownText->SetColorAndOpacity(FSlateColor(FLinearColor(1.f, 0.85f, 0.4f, Alpha * 0.95f)));
	BarrelCooldownText->SetVisibility(ESlateVisibility::HitTestInvisible);
}

void UPFCombatHUDWidget::HandleHitsChanged(uint8 HeadHits, uint8 ChestHits, uint8 LimbHits, uint8 TotalHits)
{
	const UPFHealthComponent* Health = BoundHealth.Get();
	const bool bOneHit = (Health != nullptr && Health->bOneHitMode);

	// Locational model: you're OUT when any region maxes (head 3 / chest 5 / limbs 8) OR total hits 10 — not
	// only at 10. So the bar drains by the CLOSEST region (max damage fraction), and reads empty exactly when
	// you'd be eliminated (Tom 2026-07-17: the bar still showed pips after a limb-threshold kill).
	const uint8 HeadOutV  = Health ? Health->HeadOut  : 3;
	const uint8 ChestOutV = Health ? Health->ChestOut : 5;
	const uint8 LimbOutV  = Health ? Health->LimbOut  : 8;
	const uint8 TotalOutV = Health ? Health->TotalOut : 10;
	float Damage = static_cast<float>(TotalHits) / static_cast<float>(FMath::Max<uint8>(1, TotalOutV));
	Damage = FMath::Max(Damage, static_cast<float>(HeadHits)  / static_cast<float>(FMath::Max<uint8>(1, HeadOutV)));
	Damage = FMath::Max(Damage, static_cast<float>(ChestHits) / static_cast<float>(FMath::Max<uint8>(1, ChestOutV)));
	Damage = FMath::Max(Damage, static_cast<float>(LimbHits)  / static_cast<float>(FMath::Max<uint8>(1, LimbOutV)));
	if (Health && Health->bEliminated) { Damage = 1.f; }

	// Pips = health remaining by the nearest region. Showdown collapses to a single pip.
	const int32 PipCount = bOneHit ? 1 : MaxHitPips;
	const int32 Remaining = bOneHit
		? (TotalHits > 0 ? 0 : 1)
		: FMath::Clamp(FMath::CeilToInt(static_cast<float>(MaxHitPips) * (1.f - Damage)), 0, MaxHitPips);
	for (int32 i = 0; i < HPDots.Num(); ++i)
	{
		if (!HPDots[i])
		{
			continue;
		}
		HPDots[i]->SetVisibility(i < PipCount ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
		HPDots[i]->SetColorAndOpacity(i < Remaining
			? FLinearColor(0.95f, 0.95f, 0.95f)
			: FLinearColor(0.1f, 0.1f, 0.1f, 0.6f));
	}

	if (RegionHitsText)
	{
		if (bOneHit)
		{
			RegionHitsText->SetText(FText::FromString(TEXT("SHOWDOWN — 1 HIT")));
		}
		else
		{
			RegionHitsText->SetText(FText::FromString(FString::Printf(
				TEXT("H %u/%u   C %u/%u   L %u/%u"),
				static_cast<uint32>(HeadHits), static_cast<uint32>(HeadOutV),
				static_cast<uint32>(ChestHits), static_cast<uint32>(ChestOutV),
				static_cast<uint32>(LimbHits), static_cast<uint32>(LimbOutV))));
			// Warn in orange as a region nears its threshold so it's obvious WHERE you're being shot / dying.
			RegionHitsText->SetColorAndOpacity(FSlateColor(Damage >= 0.6f
				? FLinearColor(1.f, 0.55f, 0.2f) : FLinearColor(0.85f, 0.85f, 0.85f)));
		}
	}
}

void UPFCombatHUDWidget::UpdateCrosshair()
{
	const ACombatForgeCharacter* Pawn = BoundPawn.Get();
	const UPFWeaponComponent* Weapon = BoundWeapon.Get();
	const APlayerController* PC = GetOwningPlayer();
	if (!Pawn || !Weapon || !PC)
	{
		return;
	}

	// While eliminated / out UI is up, keep the reticle hidden.
	const bool bOut =
		(BoundHealth.IsValid() && BoundHealth->bEliminated)
		|| (PC->GetPlayerState<ACombatForgePlayerState>()
			&& PC->GetPlayerState<ACombatForgePlayerState>()->OutKind != 0);
	if (bOut)
	{
		const ESlateVisibility Hidden = ESlateVisibility::Hidden;
		if (CrossLineTop)    { CrossLineTop->SetVisibility(Hidden); }
		if (CrossLineBottom) { CrossLineBottom->SetVisibility(Hidden); }
		if (CrossLineLeft)   { CrossLineLeft->SetVisibility(Hidden); }
		if (CrossLineRight)  { CrossLineRight->SetVisibility(Hidden); }
		if (CenterDot)       { CenterDot->SetVisibility(Hidden); }
		if (CrosshairHalo)   { CrosshairHalo->SetVisibility(Hidden); }
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
		if (CrosshairHalo)
		{
			CrosshairHalo->SetVisibility(ESlateVisibility::Hidden);
		}
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

	// Bloom halo: when bloom is ≥ 75% of the weapon's cap, draw a faint ring at the projected
	// spread radius — the "ease off the trigger" tell (weapon-implementation-spec Stage 1).
	if (CrosshairHalo)
	{
		const float Cap = Weapon->GetBloomCapDeg();
		const float Bloom = Weapon->GetCurrentBloomDeg();
		const bool bShowHalo = Cap > KINDA_SMALL_NUMBER && Bloom >= Cap * 0.75f;
		if (bShowHalo)
		{
			const float Diam = FMath::Max(16.f, Gap * 2.f);
			if (UCanvasPanelSlot* CSlot = Cast<UCanvasPanelSlot>(CrosshairHalo->Slot))
			{
				CSlot->SetSize(FVector2D(Diam, Diam));
			}
			CrosshairHalo->SetColorAndOpacity(FLinearColor(1.f, 0.85f, 0.35f, 0.18f));
			CrosshairHalo->SetVisibility(ESlateVisibility::HitTestInvisible);
		}
		else
		{
			CrosshairHalo->SetVisibility(ESlateVisibility::Hidden);
		}
	}
}

void UPFCombatHUDWidget::UpdateBanner(float InDeltaTime)
{
	if (!BannerText)
	{
		return;
	}
	const ACombatForgeGameState* GS = BoundGameState.Get();
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
		if (OutClassText)
		{
			OutClassText->SetVisibility(ESlateVisibility::Collapsed);
			OutClassText->SetText(FText::GetEmpty());
		}
	};

	const UPFHealthComponent* Health = BoundHealth.Get();
	const ACombatForgePlayerState* LocalPS =
		GetOwningPlayer() ? GetOwningPlayer()->GetPlayerState<ACombatForgePlayerState>() : nullptr;

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

	// "Who killed who": surface the eliminator's name on our own death board (most recent feed entry that
	// names us as the victim). The corner kill-feed shows it too, but you're looking here when you die.
	if (LocalPS && BoundGameState.IsValid())
	{
		const ACombatForgeGameState* GS = BoundGameState.Get();
		const FString LocalName = LocalPS->GetPlayerName();
		const float Now = GS->GetServerWorldTimeSeconds();
		for (int32 i = GS->ElimFeed.Num() - 1; i >= 0; --i)
		{
			const FPFElimEntry& E = GS->ElimFeed[i];
			if (E.VictimName == LocalName && (Now - E.ServerTime) < 10.f)
			{
				Subtitle = Subtitle.IsEmpty()
					? FString::Printf(TEXT("Eliminated by %s"), *E.ShooterName)
					: FString::Printf(TEXT("Eliminated by %s  ·  %s"), *E.ShooterName, *Subtitle);
				break;
			}
		}
	}

	if (OutSubtitleText)
	{
		OutSubtitleText->SetText(FText::FromString(Subtitle));
		OutSubtitleText->SetVisibility(
			Subtitle.IsEmpty() ? ESlateVisibility::Collapsed : ESlateVisibility::HitTestInvisible);
	}

	// Respawn countdown doubles as the class-select screen: show the active class + its weapon, live-updated
	// as the wheel cycles slots (the fresh pawn pushes whatever is active when the timer hits zero).
	if (OutClassText)
	{
		if (OutKind == 1)
		{
			const int32 ClassSlot = PFChar::GetActiveSaveSlot();
			const FPFWeaponConfig WpnCfg = PFWeapon::LoadConfig(ClassSlot);
			const FPFWeaponDef& Def = PFWeapon::Weapon(WpnCfg.Category, WpnCfg.Index);
			OutClassText->SetText(FText::FromString(FString::Printf(
				TEXT("CLASS %d — %s     (scroll to change)"), ClassSlot + 1, Def.DisplayName)));
			OutClassText->SetVisibility(ESlateVisibility::HitTestInvisible);
		}
		else
		{
			OutClassText->SetVisibility(ESlateVisibility::Collapsed);
			OutClassText->SetText(FText::GetEmpty());
		}
	}
}

void UPFCombatHUDWidget::UpdateObjectiveStatus()
{
	if (!ObjectiveStatusText)
	{
		return;
	}
	const ACombatForgeGameState* GS = BoundGameState.Get();
	const ACombatForgePlayerState* LocalPS =
		GetOwningPlayer() ? GetOwningPlayer()->GetPlayerState<ACombatForgePlayerState>() : nullptr;
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

void UPFCombatHUDWidget::EnsureZoneCache()
{
	// Weak-cache the three zone actors, sorted A/B/C. Clients receive them by replication whenever — a
	// late joiner can tick this with only 1-2 zones replicated, so a PARTIAL set is stale (accepting it
	// aliased chips/capture bars to the wrong zones for the rest of the match). Keep rescanning until all
	// three exist; 3 actors via iterator is trivially cheap at that rate.
	bool bValid = ZoneCache.Num() >= 3;
	for (const TWeakObjectPtr<APFControlPointActor>& CP : ZoneCache)
	{
		if (!CP.IsValid())
		{
			bValid = false;
			break;
		}
	}
	if (bValid)
	{
		return;
	}
	ZoneCache.Reset();
	if (UWorld* World = GetWorld())
	{
		for (TActorIterator<APFControlPointActor> It(World); It; ++It)
		{
			ZoneCache.Add(*It);
		}
	}
	ZoneCache.Sort([](const TWeakObjectPtr<APFControlPointActor>& L, const TWeakObjectPtr<APFControlPointActor>& R)
	{
		return (L.IsValid() ? L->GetPointIndex() : 99) < (R.IsValid() ? R->GetPointIndex() : 99);
	});
}

void UPFCombatHUDWidget::UpdateDominationHUD()
{
	const ACombatForgeGameState* GS = BoundGameState.Get();
	const ACombatForgePlayerState* LocalPS =
		GetOwningPlayer() ? GetOwningPlayer()->GetPlayerState<ACombatForgePlayerState>() : nullptr;
	const bool bDom = GS && LocalPS && GS->MatchType == EPFMatchType::Domination
		&& GS->Phase == EPFMatchPhase::Combat && GS->RoundState == EPFRoundState::Live;

	auto HideAll = [this]()
	{
		if (ZoneChipsRow) { ZoneChipsRow->SetVisibility(ESlateVisibility::Collapsed); }
		if (CaptureBar) { CaptureBar->SetVisibility(ESlateVisibility::Collapsed); }
		if (CaptureBarLabel) { CaptureBarLabel->SetVisibility(ESlateVisibility::Collapsed); }
	};
	if (!bDom)
	{
		HideAll();
		return;
	}
	EnsureZoneCache();
	if (ZoneCache.Num() == 0)
	{
		HideAll();
		return;
	}

	const FLinearColor Neutral(0.65f, 0.65f, 0.7f);

	// Chips: letter colored by OWNER; while a capture chain runs, a 4-segment fill in the CAPTURING team's
	// color shows remote progress (CoD fills the letter icon the same way).
	if (ZoneChipsRow)
	{
		ZoneChipsRow->SetVisibility(ESlateVisibility::HitTestInvisible);
		for (int32 i = 0; i < ZoneChips.Num(); ++i)
		{
			UTextBlock* Chip = ZoneChips[i];
			const APFControlPointActor* CP = ZoneCache.IsValidIndex(i) ? ZoneCache[i].Get() : nullptr;
			if (!Chip)
			{
				continue;
			}
			if (!CP)
			{
				Chip->SetText(FText::GetEmpty());
				continue;
			}
			const uint8 Owner = CP->GetControllingTeam();
			const uint8 Capper = CP->GetCapturingTeam();
			FString Text = FString::Chr(static_cast<TCHAR>(TEXT('A') + i));
			FLinearColor Color = (Owner <= 1) ? PFColors::ForTeam(Owner) : Neutral;
			if (CP->IsContested())
			{
				Text += TEXT(" ×");   // contested marker (Latin-1 — the fancier glyphs render as tofu in Roboto)
				Color = FLinearColor(1.f, 0.85f, 0.3f);
			}
			else if (Capper <= 1)
			{
				const int32 Filled = FMath::Clamp(FMath::RoundToInt(CP->GetCaptureProgress01() * 4.f), 0, 4);
				Text += TEXT(" ");
				for (int32 s = 0; s < 4; ++s)
				{
					// '|' / '·' — Roboto has no ▰▱ block glyphs (they rendered as tofu boxes).
					Text += (s < Filled) ? TEXT("|") : TEXT("·");
				}
				Color = PFColors::ForTeam(Capper);
			}
			Chip->SetText(FText::FromString(Text));
			Chip->SetColorAndOpacity(FSlateColor(Color));
		}
	}

	// Capture meter: only while the LOCAL player stands in a zone with something happening.
	const APFControlPointActor* MyCP = nullptr;
	if (LocalPS->StandingOnPoint != 255)
	{
		const int32 Idx = static_cast<int32>(LocalPS->StandingOnPoint);
		MyCP = ZoneCache.IsValidIndex(Idx) ? ZoneCache[Idx].Get() : nullptr;
	}
	FString Label;
	float Fill = 0.f;
	FLinearColor FillColor = Neutral;
	if (MyCP)
	{
		const TCHAR Letter = static_cast<TCHAR>(TEXT('A') + MyCP->GetPointIndex());
		const uint8 Owner = MyCP->GetControllingTeam();
		const uint8 Capper = MyCP->GetCapturingTeam();
		const uint8 MyTeam = LocalPS->TeamId;
		if (MyCP->IsContested())
		{
			Label = TEXT("CONTESTED");
			Fill = MyCP->GetCaptureProgress01();
			FillColor = FLinearColor(1.f, 0.85f, 0.3f);
		}
		else if (Capper <= 1)
		{
			Fill = MyCP->GetCaptureProgress01();
			FillColor = PFColors::ForTeam(Capper);
			if (Capper == MyTeam)
			{
				// Stage 1 on an enemy-owned zone is the neutralize pass (CoD's two-stage flip).
				Label = (Owner != 255 && Owner != MyTeam)
					? FString::Printf(TEXT("NEUTRALIZING %c"), Letter)
					: FString::Printf(TEXT("CAPTURING %c"), Letter);
			}
			else
			{
				Label = FString::Printf(TEXT("LOSING %c"), Letter);
			}
		}
	}
	const bool bShowBar = !Label.IsEmpty();
	if (CaptureBar)
	{
		CaptureBar->SetVisibility(bShowBar ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
		if (bShowBar)
		{
			CaptureBar->SetPercent(Fill);
			CaptureBar->SetFillColorAndOpacity(FillColor);
		}
	}
	if (CaptureBarLabel)
	{
		CaptureBarLabel->SetVisibility(bShowBar ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
		if (bShowBar)
		{
			CaptureBarLabel->SetText(FText::FromString(Label));
			CaptureBarLabel->SetColorAndOpacity(FSlateColor(FillColor));
		}
	}
}

void UPFCombatHUDWidget::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);

	TryBindGameState(); // GameState can arrive late on clients

	UpdateOutOverlay();
	UpdateCrosshair();
	UpdateBanner(InDeltaTime);
	UpdateObjectiveStatus();
	UpdateDominationHUD();
	UpdateBombCarryIndicator();
	UpdateInteractPrompt();
	UpdateBarrelCooldownNotice();

	if (const ACombatForgeGameState* GS = BoundGameState.Get())
	{
		const float RoundRemain = GS->GetRoundTimeRemaining();
		const int32 Secs = FMath::Max(0, FMath::CeilToInt(RoundRemain));
		if (RoundTimerText)
		{
			RoundTimerText->SetText(FText::FromString(FString::Printf(TEXT("%d:%02d"), Secs / 60, Secs % 60)));
			RoundTimerText->SetColorAndOpacity(FSlateColor(
				(GS->RoundState == EPFRoundState::Live && Secs <= 10)
					? FLinearColor(0.95f, 0.15f, 0.1f)
					: FLinearColor::White));
		}

		// FINAL 30 SECONDS (Tom 2026-07-24): unmissable top-center countdown + the music drops
		// out. Keyed to the live round timer — the deciding clock in every timed mode; a fresh
		// round or phase flips it back off (the music un-ducks through the same call).
		const bool bFinal30 = GS->RoundState == EPFRoundState::Live && Secs > 0 && Secs <= 30;
		if (FinalCountdownText)
		{
			FinalCountdownText->SetVisibility(
				bFinal30 ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
			if (bFinal30)
			{
				FinalCountdownText->SetText(FText::FromString(FString::Printf(TEXT("0:%02d"), Secs)));
				// Per-second pop: the fractional remainder counts DOWN inside each displayed
				// second, so scale 1+0.3*frac lands big right as the digit changes and eases out.
				const float Pop = 1.f + 0.3f * FMath::Clamp(FMath::Frac(FMath::Max(RoundRemain, 0.f)), 0.f, 1.f);
				FinalCountdownText->SetRenderTransform(
					FWidgetTransform(FVector2D::ZeroVector, FVector2D(Pop, Pop), FVector2D::ZeroVector, 0.f));
			}
		}
		if (UGameInstance* GI = GetGameInstance())
		{
			if (UPFMusicSubsystem* Music = GI->GetSubsystem<UPFMusicSubsystem>())
			{
				Music->SetMatchEndDucked(bFinal30);
			}
		}
		// FreeForAll TagCount rides PlayerState OnRep (no GameState score event) — refresh the
		// YOU/LEAD strip here so clients stay live without a dedicated multicast.
		// Objective PS flags (carrier / on-point) also lack a GS multicast — keep strip live.
		if (GS->MatchType == EPFMatchType::FreeForAll
			|| GS->MatchType == EPFMatchType::CaptureFlag
			|| GS->MatchType == EPFMatchType::Domination
			|| GS->MatchType == EPFMatchType::Hardpoint)
		{
			// Only rebuild when a value the strip shows actually moved. These modes have no score
			// multicast, so the poll has to live here — but rebuilding every frame re-set the same
			// strings and re-invalidated Slate ~60x/s for the whole match. (P2-U4)
			uint16 MyTags = 0;
			uint16 LeadTags = 0;
			if (GS->MatchType == EPFMatchType::FreeForAll)
			{
				if (const ACombatForgePlayerState* LocalPS =
					GetOwningPlayer() ? GetOwningPlayer()->GetPlayerState<ACombatForgePlayerState>() : nullptr)
				{
					MyTags = LocalPS->TagCount;
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
			const FPFScoreStripKey Key{
				static_cast<uint8>(GS->MatchType), GS->RoundWinsToTake,
				GS->TeamScores[0], GS->TeamScores[1], MyTags, LeadTags };
			if (!bScoreStripKeyValid || !(Key == LastScoreStripKey))
			{
				LastScoreStripKey = Key;
				bScoreStripKeyValid = true;
				HandleScoreChanged();
			}
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
