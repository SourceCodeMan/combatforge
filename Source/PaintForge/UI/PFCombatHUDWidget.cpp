// Copyright (c) 2026 Tom Chapman. All rights reserved.

#include "UI/PFCombatHUDWidget.h"

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
	HopperText->SetText(FText::FromString(TEXT("100 / ∞")));
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

	// ---- Top center block: pips / round number / timer / alive counts ----
	UVerticalBox* TopBox = WidgetTree->ConstructWidget<UVerticalBox>();

	UHorizontalBox* PipsRow = WidgetTree->ConstructWidget<UHorizontalBox>();
	auto AddPip = [this, PipsRow](TArray<TObjectPtr<UImage>>& OutPips)
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
	};
	for (int32 i = 0; i < MaxPips; ++i)
	{
		AddPip(PipsA);
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
		AddPip(PipsB);
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

	if (UCanvasPanelSlot* CSlot = RootCanvas->AddChildToCanvas(TopBox))
	{
		CSlot->SetAnchors(FAnchors(0.5f, 0.f));
		CSlot->SetAlignment(FVector2D(0.5f, 0.f));
		CSlot->SetPosition(FVector2D(0.f, 24.f));
		CSlot->SetAutoSize(true);
	}

	// ---- Banner (freeze / intermission / sudden death) ----
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
			HandleHopperChanged(Weapon->HopperCount);
			HandleReloadStateChanged(Weapon->bReloading);
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

void UPFCombatHUDWidget::HandleAliveCountsChanged()
{
	const APaintForgeGameState* GS = BoundGameState.Get();
	if (!GS)
	{
		return;
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
	if (HopperText)
	{
		HopperText->SetText(FText::FromString(FString::Printf(TEXT("%d / ∞"), NewCount)));
	}
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

	const bool bADS = Pawn->IsADS();
	const ESlateVisibility LinesVis = bADS ? ESlateVisibility::Hidden : ESlateVisibility::HitTestInvisible;
	if (CrossLineTop)    { CrossLineTop->SetVisibility(LinesVis); }
	if (CrossLineBottom) { CrossLineBottom->SetVisibility(LinesVis); }
	if (CrossLineLeft)   { CrossLineLeft->SetVisibility(LinesVis); }
	if (CrossLineRight)  { CrossLineRight->SetVisibility(LinesVis); }

	if (CenterDot)
	{
		if (UCanvasPanelSlot* DotSlot = Cast<UCanvasPanelSlot>(CenterDot->Slot))
		{
			// ADS shows a 2 px dot only (04 §4).
			DotSlot->SetSize(bADS ? FVector2D(2.f, 2.f) : FVector2D(3.f, 3.f));
		}
	}
	if (bADS)
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

	// CONTRACT-GAP: 04 §4 writes the gap as 40·tan(spread)/tan(FOV/2)·(ViewportW/2),
	// but the leading 40 puts a 1.5° hip cone at ~770 px on a 1080p screen (off-HUD).
	// Implemented as the true screen projection of the spread half-angle plus a fixed
	// base gap — the crosshair still "truthfully displays the live spread cone".
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
		Banner = GS->bSuddenDeath
			? FString::Printf(TEXT("SUDDEN DEATH — %d"), Secs)
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
			Banner = TEXT("SUDDEN DEATH");
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

void UPFCombatHUDWidget::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);

	TryBindGameState(); // GameState can arrive late on clients

	UpdateCrosshair();
	UpdateBanner(InDeltaTime);

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
	}

	if (bReloading && ReloadBar)
	{
		const UPFWeaponComponent* Weapon = BoundWeapon.Get();
		const float ReloadTime = (Weapon && Weapon->ReloadTime > 0.f) ? Weapon->ReloadTime : 1.f;
		ReloadElapsed += InDeltaTime;
		ReloadBar->SetPercent(FMath::Clamp(ReloadElapsed / ReloadTime, 0.f, 1.f));
	}
}
