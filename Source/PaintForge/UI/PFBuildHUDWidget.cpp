// Copyright (c) 2026 Tom Chapman. All rights reserved.

#include "UI/PFBuildHUDWidget.h"

#include "Building/PFBuildComponent.h"
#include "Core/PaintForgeGameState.h"
#include "Core/PaintForgePlayerState.h"
#include "Player/PaintForgeCharacter.h"

#include "Blueprint/WidgetTree.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/TextBlock.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "Styling/CoreStyle.h"

namespace
{
	FSlateFontInfo PFBuildFont(int32 Size, bool bBold)
	{
		return FCoreStyle::GetDefaultFontStyle(bBold ? FName(TEXT("Bold")) : FName(TEXT("Regular")), Size);
	}
}

const TCHAR* UPFBuildHUDWidget::ToolDisplayName(EPFBuildTool Tool)
{
	switch (Tool)
	{
	case EPFBuildTool::Wall:       return TEXT("Wall");
	case EPFBuildTool::Floor:      return TEXT("Floor");
	case EPFBuildTool::Ramp:       return TEXT("Ramp");
	case EPFBuildTool::Roof:       return TEXT("Roof");
	case EPFBuildTool::PropCan:    return TEXT("Can");
	case EPFBuildTool::PropDorito: return TEXT("Dorito");
	case EPFBuildTool::PropSnake:  return TEXT("Snake");
	case EPFBuildTool::Delete:     return TEXT("Delete");
	default:                       return TEXT("?");
	}
}

const TCHAR* UPFBuildHUDWidget::DenyReasonText(EPFDenyReason Reason)
{
	switch (Reason)
	{
	case EPFDenyReason::WrongPhase:   return TEXT("NOT BUILD PHASE");
	case EPFDenyReason::OutOfBudget:  return TEXT("OUT OF BUDGET");
	case EPFDenyReason::SlotOccupied: return TEXT("SLOT OCCUPIED");
	case EPFDenyReason::Overlapping:  return TEXT("OVERLAPPING");
	case EPFDenyReason::OutOfPlot:    return TEXT("OUTSIDE YOUR PLOT");
	case EPFDenyReason::NoAnchor:     return TEXT("NO SUPPORT");
	case EPFDenyReason::HeightCap:    return TEXT("HEIGHT CAP");
	case EPFDenyReason::RateLimited:  return TEXT("TOO FAST");
	case EPFDenyReason::NotYourTeam:  return TEXT("NOT YOUR TEAM'S PIECE");
	case EPFDenyReason::InvalidPiece: return TEXT("INVALID PIECE");
	case EPFDenyReason::NotFound:     return TEXT("PIECE NOT FOUND");
	default:                          return TEXT("DENIED");
	}
}

TSharedRef<SWidget> UPFBuildHUDWidget::RebuildWidget()
{
	if (WidgetTree && !WidgetTree->RootWidget)
	{
		BuildTree(); // root assigned BEFORE Super::RebuildWidget (02 R3)
	}
	return Super::RebuildWidget();
}

void UPFBuildHUDWidget::BuildTree()
{
	UCanvasPanel* RootCanvas = WidgetTree->ConstructWidget<UCanvasPanel>();
	WidgetTree->RootWidget = RootCanvas;

	// Phase countdown — top center.
	TimerText = WidgetTree->ConstructWidget<UTextBlock>();
	TimerText->SetFont(PFBuildFont(30, true));
	TimerText->SetColorAndOpacity(FSlateColor(FLinearColor::White));
	TimerText->SetJustification(ETextJustify::Center);
	if (UCanvasPanelSlot* CSlot = RootCanvas->AddChildToCanvas(TimerText))
	{
		CSlot->SetAnchors(FAnchors(0.5f, 0.f));
		CSlot->SetAlignment(FVector2D(0.5f, 0.f));
		CSlot->SetPosition(FVector2D(0.f, 32.f));
		CSlot->SetAutoSize(true);
	}

	// Per-team ready counts — under the timer.
	ReadyText = WidgetTree->ConstructWidget<UTextBlock>();
	ReadyText->SetFont(PFBuildFont(15, false));
	ReadyText->SetColorAndOpacity(FSlateColor(FLinearColor(1.f, 1.f, 1.f, 0.8f)));
	ReadyText->SetJustification(ETextJustify::Center);
	if (UCanvasPanelSlot* CSlot = RootCanvas->AddChildToCanvas(ReadyText))
	{
		CSlot->SetAnchors(FAnchors(0.5f, 0.f));
		CSlot->SetAlignment(FVector2D(0.5f, 0.f));
		CSlot->SetPosition(FVector2D(0.f, 74.f));
		CSlot->SetAutoSize(true);
	}

	// Deny flash — center, below crosshair line.
	DenyText = WidgetTree->ConstructWidget<UTextBlock>();
	DenyText->SetFont(PFBuildFont(18, true));
	DenyText->SetColorAndOpacity(FSlateColor(FLinearColor(0.95f, 0.1f, 0.1f)));
	DenyText->SetJustification(ETextJustify::Center);
	DenyText->SetRenderOpacity(0.f);
	if (UCanvasPanelSlot* CSlot = RootCanvas->AddChildToCanvas(DenyText))
	{
		CSlot->SetAnchors(FAnchors(0.5f, 0.5f));
		CSlot->SetAlignment(FVector2D(0.5f, 0.5f));
		CSlot->SetPosition(FVector2D(0.f, 90.f));
		CSlot->SetAutoSize(true);
	}

	// Budget + equipped piece — bottom right.
	BudgetText = WidgetTree->ConstructWidget<UTextBlock>();
	BudgetText->SetFont(PFBuildFont(20, true));
	BudgetText->SetColorAndOpacity(FSlateColor(FLinearColor::White));
	BudgetText->SetJustification(ETextJustify::Right);
	if (UCanvasPanelSlot* CSlot = RootCanvas->AddChildToCanvas(BudgetText))
	{
		CSlot->SetAnchors(FAnchors(1.f, 1.f));
		CSlot->SetAlignment(FVector2D(1.f, 1.f));
		CSlot->SetPosition(FVector2D(-32.f, -64.f));
		CSlot->SetAutoSize(true);
	}

	EquippedText = WidgetTree->ConstructWidget<UTextBlock>();
	EquippedText->SetFont(PFBuildFont(16, false));
	EquippedText->SetColorAndOpacity(FSlateColor(FLinearColor(1.f, 1.f, 1.f, 0.85f)));
	EquippedText->SetJustification(ETextJustify::Right);
	if (UCanvasPanelSlot* CSlot = RootCanvas->AddChildToCanvas(EquippedText))
	{
		CSlot->SetAnchors(FAnchors(1.f, 1.f));
		CSlot->SetAlignment(FVector2D(1.f, 1.f));
		CSlot->SetPosition(FVector2D(-32.f, -36.f));
		CSlot->SetAutoSize(true);
	}

	// Controls hint — bottom left.
	HintText = WidgetTree->ConstructWidget<UTextBlock>();
	HintText->SetText(FText::FromString(
		TEXT("F1-F4 pieces  ·  Q wheel  ·  LMB place  ·  R rotate  ·  X delete  ·  F ready")));
	HintText->SetFont(PFBuildFont(12, false));
	HintText->SetColorAndOpacity(FSlateColor(FLinearColor(1.f, 1.f, 1.f, 0.5f)));
	if (UCanvasPanelSlot* CSlot = RootCanvas->AddChildToCanvas(HintText))
	{
		CSlot->SetAnchors(FAnchors(0.f, 1.f));
		CSlot->SetAlignment(FVector2D(0.f, 1.f));
		CSlot->SetPosition(FVector2D(32.f, -36.f));
		CSlot->SetAutoSize(true);
	}
}

void UPFBuildHUDWidget::NativeConstruct()
{
	Super::NativeConstruct();
	PrevPhaseRemaining = -1.f;
	WarnPulseRemaining = 0.f;
	DenyFlashRemaining = 0.f;
	PollAccum = PollInterval;
	BindLocalPlayerState();
	UpdateBudgetText();
	if (EquippedText && BoundBuild.IsValid())
	{
		HandleEquippedToolChanged(BoundBuild->GetEquippedTool());
	}
}

void UPFBuildHUDWidget::NativeDestruct()
{
	UnbindPawn();
	if (BoundPlayerState.IsValid())
	{
		BoundPlayerState->OnFlagsChangedEvent.RemoveAll(this);
		BoundPlayerState.Reset();
	}
	Super::NativeDestruct();
}

void UPFBuildHUDWidget::BindToPawn(APaintForgeCharacter* NewPawn)
{
	UnbindPawn();
	if (NewPawn)
	{
		if (UPFBuildComponent* Build = NewPawn->GetBuild())
		{
			BoundBuild = Build;
			Build->OnEquippedToolChangedEvent.AddUObject(this, &UPFBuildHUDWidget::HandleEquippedToolChanged);
			Build->OnPlaceDeniedEvent.AddUObject(this, &UPFBuildHUDWidget::HandlePlaceDenied);
			HandleEquippedToolChanged(Build->GetEquippedTool());
		}
	}
}

void UPFBuildHUDWidget::UnbindPawn()
{
	if (BoundBuild.IsValid())
	{
		BoundBuild->OnEquippedToolChangedEvent.RemoveAll(this);
		BoundBuild->OnPlaceDeniedEvent.RemoveAll(this);
	}
	BoundBuild.Reset();
}

void UPFBuildHUDWidget::BindLocalPlayerState()
{
	if (BoundPlayerState.IsValid())
	{
		return;
	}
	const APlayerController* PC = GetOwningPlayer();
	APaintForgePlayerState* PS = PC ? PC->GetPlayerState<APaintForgePlayerState>() : nullptr;
	if (PS)
	{
		BoundPlayerState = PS;
		PS->OnFlagsChangedEvent.AddUObject(this, &UPFBuildHUDWidget::HandlePlayerStateFlagsChanged);
		UpdateBudgetText();
	}
}

void UPFBuildHUDWidget::HandleEquippedToolChanged(EPFBuildTool NewTool)
{
	if (EquippedText)
	{
		EquippedText->SetText(FText::FromString(ToolDisplayName(NewTool)));
		EquippedText->SetColorAndOpacity(FSlateColor(NewTool == EPFBuildTool::Delete
			? PFColors::DeleteHighlight
			: FLinearColor(1.f, 1.f, 1.f, 0.85f)));
	}
}

void UPFBuildHUDWidget::HandlePlaceDenied(EPFDenyReason Reason)
{
	if (DenyText)
	{
		DenyText->SetText(FText::FromString(DenyReasonText(Reason)));
	}
	DenyFlashRemaining = DenyFlashDuration;
	// CONTRACT-GAP: 01 asks for a warning "tone" alongside HUD flashes; UPFCombatAudio
	// contracts no timer/deny-tone hook usable from UI (PlayDenied is fired by
	// UPFBuildComponent per §3.5), so the HUD side is visual-only.
}

void UPFBuildHUDWidget::HandlePlayerStateFlagsChanged()
{
	UpdateBudgetText();
}

void UPFBuildHUDWidget::UpdateBudgetText()
{
	if (!BudgetText)
	{
		return;
	}
	int32 Structural = 0;
	int32 Props = 0;
	if (const APaintForgePlayerState* PS = BoundPlayerState.Get())
	{
		Structural = PS->StructuralBudget;
		Props = PS->PropBudget;
	}
	// Caps are contract constants (B6): 30 structural + 6 props per player.
	BudgetText->SetText(FText::FromString(
		FString::Printf(TEXT("▦ %d/30   ◆ %d/6"), Structural, Props)));
}

void UPFBuildHUDWidget::UpdateReadyCounts()
{
	if (!ReadyText)
	{
		return;
	}
	const UWorld* World = GetWorld();
	const APaintForgeGameState* GS = World ? World->GetGameState<APaintForgeGameState>() : nullptr;
	if (!GS)
	{
		return;
	}
	int32 Total[2] = { 0, 0 };
	int32 Ready[2] = { 0, 0 };
	for (const APlayerState* PS : GS->PlayerArray)
	{
		const APaintForgePlayerState* PFPS = Cast<APaintForgePlayerState>(PS);
		if (PFPS && PFPS->TeamId <= 1)
		{
			++Total[PFPS->TeamId];
			if (PFPS->bReady)
			{
				++Ready[PFPS->TeamId];
			}
		}
	}
	ReadyText->SetText(FText::FromString(
		FString::Printf(TEXT("Ready %d/%d — %d/%d"), Ready[0], Total[0], Ready[1], Total[1])));
}

void UPFBuildHUDWidget::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);

	BindLocalPlayerState(); // PlayerState can arrive late on clients

	const UWorld* World = GetWorld();
	const APaintForgeGameState* GS = World ? World->GetGameState<APaintForgeGameState>() : nullptr;

	// Phase countdown + 30 s / 10 s warning flashes (T2).
	if (TimerText && GS)
	{
		const float Remaining = GS->GetPhaseTimeRemaining();
		const int32 Secs = FMath::Max(0, FMath::CeilToInt(Remaining));
		TimerText->SetText(FText::FromString(FString::Printf(TEXT("%d:%02d"), Secs / 60, Secs % 60)));

		if (PrevPhaseRemaining > 30.f && Remaining <= 30.f)
		{
			WarnPulseRemaining = WarnPulseDuration;
		}
		if (PrevPhaseRemaining > 10.f && Remaining <= 10.f)
		{
			WarnPulseRemaining = WarnPulseDuration;
		}
		PrevPhaseRemaining = Remaining;

		FLinearColor TimerColor = FLinearColor::White;
		if (Remaining <= 10.f)
		{
			TimerColor = FLinearColor(0.95f, 0.15f, 0.1f);
		}
		else if (Remaining <= 30.f)
		{
			TimerColor = FLinearColor(1.f, 0.75f, 0.15f);
		}
		TimerText->SetColorAndOpacity(FSlateColor(TimerColor));

		if (WarnPulseRemaining > 0.f)
		{
			WarnPulseRemaining = FMath::Max(0.f, WarnPulseRemaining - InDeltaTime);
			const float Pulse = 0.35f + 0.65f * FMath::Abs(FMath::Sin(WarnPulseRemaining * 12.f));
			TimerText->SetRenderOpacity(Pulse);
		}
		else
		{
			TimerText->SetRenderOpacity(1.f);
		}
	}

	// Deny flash fade.
	if (DenyText)
	{
		if (DenyFlashRemaining > 0.f)
		{
			DenyFlashRemaining = FMath::Max(0.f, DenyFlashRemaining - InDeltaTime);
			DenyText->SetRenderOpacity(FMath::Clamp(DenyFlashRemaining / DenyFlashDuration, 0.f, 1.f));
		}
		else
		{
			DenyText->SetRenderOpacity(0.f);
		}
	}

	PollAccum += InDeltaTime;
	if (PollAccum >= PollInterval)
	{
		PollAccum = 0.f;
		UpdateReadyCounts();
		UpdateBudgetText(); // delegate is primary; poll is join-in-progress safety
	}
}
