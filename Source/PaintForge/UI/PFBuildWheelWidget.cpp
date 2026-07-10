// Copyright (c) 2026 Tom Chapman. All rights reserved.

#include "UI/PFBuildWheelWidget.h"

#include "Core/PaintForgePlayerState.h"

#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/Image.h"
#include "Components/SizeBox.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Brushes/SlateColorBrush.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/PlayerController.h"
#include "InputCoreTypes.h"
#include "Styling/CoreStyle.h"

namespace
{
	const FLinearColor GSectorDimColor(0.06f, 0.07f, 0.09f, 0.85f);
	const FLinearColor GSectorHoverColor(0.22f, 0.24f, 0.30f, 0.95f);

	FSlateFontInfo PFWheelFont(int32 Size, bool bBold)
	{
		return FCoreStyle::GetDefaultFontStyle(bBold ? FName(TEXT("Bold")) : FName(TEXT("Regular")), Size);
	}
}

const TCHAR* UPFBuildWheelWidget::ToolDisplayName(EPFBuildTool Tool)
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

TSharedRef<SWidget> UPFBuildWheelWidget::RebuildWidget()
{
	if (WidgetTree && !WidgetTree->RootWidget)
	{
		BuildTree(); // root assigned BEFORE Super::RebuildWidget (02 R3)
	}
	return Super::RebuildWidget();
}

void UPFBuildWheelWidget::BuildTree()
{
	RootCanvas = WidgetTree->ConstructWidget<UCanvasPanel>();
	WidgetTree->RootWidget = RootCanvas;

	SectorSwatches.Reset();
	SectorNameTexts.Reset();

	for (int32 i = 0; i < NumSectors; ++i)
	{
		// Sector i's centroid: clockwise from top, 45 degrees each.
		const float AngleRad = FMath::DegreesToRadians(i * 45.f);
		const FVector2D Pos(FMath::Sin(AngleRad) * SectorRadiusPx, -FMath::Cos(AngleRad) * SectorRadiusPx);

		UBorder* Swatch = WidgetTree->ConstructWidget<UBorder>();
		Swatch->SetBrushColor(GSectorDimColor);
		Swatch->SetPadding(FMargin(6.f));
		Swatch->SetHorizontalAlignment(HAlign_Center);
		Swatch->SetVerticalAlignment(VAlign_Center);

		UVerticalBox* Box = WidgetTree->ConstructWidget<UVerticalBox>();

		UTextBlock* Digit = WidgetTree->ConstructWidget<UTextBlock>();
		Digit->SetText(FText::FromString(FString::Printf(TEXT("%d"), i + 1)));
		Digit->SetFont(PFWheelFont(10, false));
		Digit->SetColorAndOpacity(FSlateColor(FLinearColor(1.f, 1.f, 1.f, 0.45f)));
		Digit->SetJustification(ETextJustify::Center);
		if (UVerticalBoxSlot* DigitSlot = Box->AddChildToVerticalBox(Digit))
		{
			DigitSlot->SetHorizontalAlignment(HAlign_Center);
		}

		UTextBlock* Name = WidgetTree->ConstructWidget<UTextBlock>();
		Name->SetText(FText::FromString(ToolDisplayName(static_cast<EPFBuildTool>(i))));
		Name->SetFont(PFWheelFont(14, true));
		Name->SetColorAndOpacity(FSlateColor(FLinearColor::White));
		Name->SetJustification(ETextJustify::Center);
		if (UVerticalBoxSlot* NameSlot = Box->AddChildToVerticalBox(Name))
		{
			NameSlot->SetHorizontalAlignment(HAlign_Center);
		}

		Swatch->SetContent(Box);

		USizeBox* Sizer = WidgetTree->ConstructWidget<USizeBox>();
		Sizer->SetWidthOverride(120.f);
		Sizer->SetHeightOverride(64.f);
		Sizer->SetContent(Swatch);

		if (UCanvasPanelSlot* CSlot = RootCanvas->AddChildToCanvas(Sizer))
		{
			CSlot->SetAnchors(FAnchors(0.5f, 0.5f));
			CSlot->SetAlignment(FVector2D(0.5f, 0.5f));
			CSlot->SetPosition(Pos);
			CSlot->SetAutoSize(true);
		}

		SectorSwatches.Add(Swatch);
		SectorNameTexts.Add(Name);
	}

	// Center readout — sits inside the dead zone.
	CenterReadout = WidgetTree->ConstructWidget<UTextBlock>();
	CenterReadout->SetFont(PFWheelFont(13, true));
	CenterReadout->SetColorAndOpacity(FSlateColor(FLinearColor(1.f, 1.f, 1.f, 0.9f)));
	CenterReadout->SetJustification(ETextJustify::Center);
	if (UCanvasPanelSlot* CSlot = RootCanvas->AddChildToCanvas(CenterReadout))
	{
		CSlot->SetAnchors(FAnchors(0.5f, 0.5f));
		CSlot->SetAlignment(FVector2D(0.5f, 0.5f));
		CSlot->SetPosition(FVector2D::ZeroVector);
		CSlot->SetAutoSize(true);
	}

	// Wheel cursor dot.
	CursorDot = WidgetTree->ConstructWidget<UImage>();
	CursorDot->SetBrush(FSlateColorBrush(FLinearColor::White));
	CursorDot->SetColorAndOpacity(FLinearColor(1.f, 1.f, 1.f, 0.9f));
	if (UCanvasPanelSlot* CSlot = RootCanvas->AddChildToCanvas(CursorDot))
	{
		CSlot->SetAnchors(FAnchors(0.5f, 0.5f));
		CSlot->SetAlignment(FVector2D(0.5f, 0.5f));
		CSlot->SetPosition(FVector2D::ZeroVector);
		CSlot->SetSize(FVector2D(8.f, 8.f));
		CSlot->SetZOrder(10);
	}
}

void UPFBuildWheelWidget::NativeOnInitialized()
{
	Super::NativeOnInitialized();
	SetIsFocusable(true); // digits 1-8 arrive via NativeOnKeyDown while open (T4)
	SetVisibility(ESlateVisibility::Collapsed);
}

void UPFBuildWheelWidget::NativeDestruct()
{
	// Never leave the controller with a dangling look-input ignore.
	CloseCancel();
	Super::NativeDestruct();
}

void UPFBuildWheelWidget::Open()
{
	if (bWheelOpen)
	{
		return;
	}
	bWheelOpen = true;
	AccumDelta = FVector2D::ZeroVector;
	SetHoveredSector(INDEX_NONE);

	SetVisibility(ESlateVisibility::Visible);

	if (APlayerController* PC = GetOwningPlayer())
	{
		// Camera frozen while the wheel is open (03 §3); WASD keeps working.
		PC->SetIgnoreLookInput(true);
		bLookInputIgnored = true;
	}
	SetKeyboardFocus();

	if (CursorDot)
	{
		if (UCanvasPanelSlot* DotSlot = Cast<UCanvasPanelSlot>(CursorDot->Slot))
		{
			DotSlot->SetPosition(FVector2D::ZeroVector);
		}
	}
	UpdateCenterReadout();
}

void UPFBuildWheelWidget::CloseAndCommit()
{
	if (!bWheelOpen)
	{
		return;
	}
	const int32 Committed = HoveredSector;
	CloseInternal();
	if (Committed != INDEX_NONE)
	{
		OnToolSelectedEvent.Broadcast(static_cast<EPFBuildTool>(Committed));
	}
}

void UPFBuildWheelWidget::CloseCancel()
{
	if (!bWheelOpen)
	{
		return;
	}
	CloseInternal();
}

void UPFBuildWheelWidget::CommitSector(int32 SectorIndex)
{
	if (!bWheelOpen || SectorIndex < 0 || SectorIndex >= NumSectors)
	{
		return;
	}
	CloseInternal();
	OnToolSelectedEvent.Broadcast(static_cast<EPFBuildTool>(SectorIndex));
}

void UPFBuildWheelWidget::CloseInternal()
{
	bWheelOpen = false;
	SetHoveredSector(INDEX_NONE);
	SetVisibility(ESlateVisibility::Collapsed);

	if (bLookInputIgnored)
	{
		if (APlayerController* PC = GetOwningPlayer())
		{
			PC->SetIgnoreLookInput(false);
		}
		bLookInputIgnored = false;
	}
	if (FSlateApplication::IsInitialized())
	{
		FSlateApplication::Get().SetAllUserFocusToGameViewport();
	}
}

void UPFBuildWheelWidget::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);

	if (!bWheelOpen)
	{
		return;
	}

	APlayerController* PC = GetOwningPlayer();
	if (PC)
	{
		float DeltaX = 0.f;
		float DeltaY = 0.f;
		PC->GetInputMouseDelta(DeltaX, DeltaY);
		// Mouse up = positive DeltaY; screen space Y grows downward.
		AccumDelta += FVector2D(DeltaX, -DeltaY) * MouseToCursorScale;
	}

	if (AccumDelta.SizeSquared() > FMath::Square(SelectMaxPx))
	{
		AccumDelta = AccumDelta.GetSafeNormal() * SelectMaxPx;
	}

	int32 NewHover = INDEX_NONE;
	const float Radius = AccumDelta.Size();
	if (Radius >= DeadZonePx)
	{
		// Clockwise angle from top: up = 0, right = 90.
		const float ThetaDeg = FMath::RadiansToDegrees(FMath::Atan2(AccumDelta.X, -AccumDelta.Y));
		const float Wrapped = FMath::Fmod(ThetaDeg + 360.f + 22.5f, 360.f);
		NewHover = FMath::Clamp(FMath::FloorToInt(Wrapped / 45.f), 0, NumSectors - 1);
	}
	SetHoveredSector(NewHover);

	if (CursorDot)
	{
		if (UCanvasPanelSlot* DotSlot = Cast<UCanvasPanelSlot>(CursorDot->Slot))
		{
			DotSlot->SetPosition(AccumDelta);
		}
	}

	UpdateCenterReadout();
}

FReply UPFBuildWheelWidget::NativeOnKeyDown(const FGeometry& InGeometry, const FKeyEvent& InKeyEvent)
{
	if (bWheelOpen)
	{
		static const FKey DigitKeys[NumSectors] =
		{
			EKeys::One, EKeys::Two, EKeys::Three, EKeys::Four,
			EKeys::Five, EKeys::Six, EKeys::Seven, EKeys::Eight
		};
		const FKey Key = InKeyEvent.GetKey();
		for (int32 i = 0; i < NumSectors; ++i)
		{
			if (Key == DigitKeys[i])
			{
				CommitSector(i);
				return FReply::Handled();
			}
		}
	}
	return Super::NativeOnKeyDown(InGeometry, InKeyEvent);
}

void UPFBuildWheelWidget::SetHoveredSector(int32 NewIndex)
{
	if (NewIndex == HoveredSector)
	{
		return;
	}
	if (SectorSwatches.IsValidIndex(HoveredSector))
	{
		ApplySectorVisual(HoveredSector, false);
	}
	HoveredSector = NewIndex;
	if (SectorSwatches.IsValidIndex(HoveredSector))
	{
		ApplySectorVisual(HoveredSector, true);
	}
}

void UPFBuildWheelWidget::ApplySectorVisual(int32 SectorIndex, bool bHovered)
{
	if (UBorder* Swatch = SectorSwatches.IsValidIndex(SectorIndex) ? SectorSwatches[SectorIndex].Get() : nullptr)
	{
		Swatch->SetBrushColor(bHovered ? GSectorHoverColor : GSectorDimColor);
		Swatch->SetRenderScale(bHovered ? FVector2D(1.08f, 1.08f) : FVector2D(1.f, 1.f));
	}
}

void UPFBuildWheelWidget::UpdateCenterReadout()
{
	if (!CenterReadout)
	{
		return;
	}
	if (HoveredSector == INDEX_NONE)
	{
		CenterReadout->SetText(FText::FromString(TEXT("Release to cancel")));
		CenterReadout->SetColorAndOpacity(FSlateColor(FLinearColor(1.f, 1.f, 1.f, 0.5f)));
	}
	else
	{
		CenterReadout->SetText(SectorReadout(HoveredSector));
		CenterReadout->SetColorAndOpacity(FSlateColor(FLinearColor::White));
	}
}

FText UPFBuildWheelWidget::SectorReadout(int32 SectorIndex) const
{
	const EPFBuildTool Tool = static_cast<EPFBuildTool>(SectorIndex);
	if (Tool == EPFBuildTool::Delete)
	{
		return FText::FromString(TEXT("Delete — full refund"));
	}

	int32 Remaining = -1;
	if (const APlayerController* PC = GetOwningPlayer())
	{
		if (const APaintForgePlayerState* PS = PC->GetPlayerState<APaintForgePlayerState>())
		{
			const EPFPieceType Type = static_cast<EPFPieceType>(static_cast<uint8>(Tool));
			Remaining = PFIsProp(Type) ? PS->PropBudget : PS->StructuralBudget;
		}
	}

	if (Remaining >= 0)
	{
		return FText::FromString(FString::Printf(TEXT("%s — %d left"), ToolDisplayName(Tool), Remaining));
	}
	return FText::FromString(ToolDisplayName(Tool));
}
