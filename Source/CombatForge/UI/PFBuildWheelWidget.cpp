// Copyright (c) 2026 Tom Chapman. All rights reserved.

#include "UI/PFBuildWheelWidget.h"

#include "Building/PFBuildPieceVisuals.h"

#include "Core/CombatForgePlayerState.h"

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
#include "Input/PFGamepadCursor.h"   // FPFInputDevice — gamepad-aware hint text
#include "InputCoreTypes.h"
#include "Styling/CoreStyle.h"

namespace
{
	const FLinearColor GSectorDimColor(0.06f, 0.07f, 0.09f, 0.85f);
	const FLinearColor GSectorHoverColor(0.22f, 0.24f, 0.30f, 0.95f);

	// Matches UPFBuildComponent GCycleOrder — wall types grouped for easy pick.
	constexpr EPFBuildTool GSectorTools[12] =
	{
		EPFBuildTool::Wall, EPFBuildTool::WallWindow, EPFBuildTool::WallDoor, EPFBuildTool::WallDoorOneWay,
		EPFBuildTool::Floor, EPFBuildTool::FloorTrap, EPFBuildTool::Ramp, EPFBuildTool::Roof,
		EPFBuildTool::PropCan, EPFBuildTool::PropDorito, EPFBuildTool::PropSnake, EPFBuildTool::Delete
	};

	FSlateFontInfo PFWheelFont(int32 Size, bool bBold)
	{
		return FCoreStyle::GetDefaultFontStyle(bBold ? FName(TEXT("Bold")) : FName(TEXT("Regular")), Size);
	}
}

const TCHAR* UPFBuildWheelWidget::ToolDisplayName(EPFBuildTool Tool)
{
	return PFBuildPieceVisuals::DisplayName(Tool);
}

FString UPFBuildWheelWidget::SectorHotkeyLabel(int32 SectorIndex)
{
	// UPFBuildComponent::HandleWheelDigit maps digit 1..9 -> sector 0..8 and digit 0 -> sector 9.
	// Anything past that is reachable by mouse only, so it gets no digit at all.
	if (SectorIndex >= 0 && SectorIndex <= 8) { return FString::Printf(TEXT("%d"), SectorIndex + 1); }
	if (SectorIndex == 9)                     { return TEXT("0"); }
	return FString();
}

EPFBuildTool UPFBuildWheelWidget::SectorTool(int32 SectorIndex)
{
	if (SectorIndex < 0 || SectorIndex >= UE_ARRAY_COUNT(GSectorTools))
	{
		return EPFBuildTool::Wall;
	}
	return GSectorTools[SectorIndex];
}

EPFBuildTool UPFBuildWheelWidget::ToolAtSector(int32 SectorIndex) const
{
	return SectorTool(SectorIndex);
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

	const float SectorDeg = 360.f / static_cast<float>(NumSectors);
	for (int32 i = 0; i < NumSectors; ++i)
	{
		// Sector i's centroid: clockwise from top.
		const float AngleRad = FMath::DegreesToRadians(i * SectorDeg);
		const FVector2D Pos(FMath::Sin(AngleRad) * SectorRadiusPx, -FMath::Cos(AngleRad) * SectorRadiusPx);

		UBorder* Swatch = WidgetTree->ConstructWidget<UBorder>();
		Swatch->SetBrushColor(GSectorDimColor);
		Swatch->SetPadding(FMargin(6.f));
		Swatch->SetHorizontalAlignment(HAlign_Center);
		Swatch->SetVerticalAlignment(VAlign_Center);

		UVerticalBox* Box = WidgetTree->ConstructWidget<UVerticalBox>();

		// Digit label must match the key that actually commits this sector: 1-9 -> sectors 0-8,
		// 0 -> sector 9, and sectors 10+ have no digit binding at all (mouse-select only). (P2-U3)
		UTextBlock* Digit = WidgetTree->ConstructWidget<UTextBlock>();
		Digit->SetText(FText::FromString(SectorHotkeyLabel(i)));
		Digit->SetFont(PFWheelFont(10, false));
		Digit->SetColorAndOpacity(FSlateColor(FLinearColor(1.f, 1.f, 1.f, 0.45f)));
		Digit->SetJustification(ETextJustify::Center);
		if (UVerticalBoxSlot* DigitSlot = Box->AddChildToVerticalBox(Digit))
		{
			DigitSlot->SetHorizontalAlignment(HAlign_Center);
		}

		UTextBlock* Name = WidgetTree->ConstructWidget<UTextBlock>();
		Name->SetText(FText::FromString(ToolDisplayName(SectorTool(i))));
		Name->SetFont(PFWheelFont(14, true));
		Name->SetColorAndOpacity(FSlateColor(FLinearColor::White));
		Name->SetJustification(ETextJustify::Center);
		if (UVerticalBoxSlot* NameSlot = Box->AddChildToVerticalBox(Name))
		{
			NameSlot->SetHorizontalAlignment(HAlign_Center);
		}

		Swatch->SetContent(Box);

		USizeBox* Sizer = WidgetTree->ConstructWidget<USizeBox>();
		Sizer->SetWidthOverride(108.f);
		Sizer->SetHeightOverride(56.f);
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
	// NEVER focusable (2026-07-24): taking keyboard focus flushed every pressed key (viewport
	// LostFocus) → phantom Q-release → the held wheel flashed open/closed at key-repeat rate.
	// Digits now arrive via Enhanced Input (WheelDigitActions → RootHUD → CommitSector).
	SetIsFocusable(false);
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

	// HitTestInvisible + NO SetKeyboardFocus (2026-07-24): the focus steal flushed pressed keys
	// and broke hold-Q entirely (see class comment). Nothing here needs focus or hit-testing —
	// hover is mouse-delta math in NativeTick, digits ride Enhanced Input.
	SetVisibility(ESlateVisibility::HitTestInvisible);

	if (APlayerController* PC = GetOwningPlayer())
	{
		// Camera frozen while the wheel is open (03 §3); WASD keeps working.
		PC->SetIgnoreLookInput(true);
		bLookInputIgnored = true;
	}

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
		OnToolSelectedEvent.Broadcast(SectorTool(Committed));
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
	OnToolSelectedEvent.Broadcast(SectorTool(SectorIndex));
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
	// (No focus restore needed — the wheel never takes focus; see Open().)
	// Let RootHUD clear BuildComponent's open flag (digit / Esc / phase paths).
	OnWheelClosedEvent.Broadcast(false);
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

		// Gamepad: right stick steers the wheel cursor too (camera is already frozen —
		// Open()'s SetIgnoreLookInput makes the stick's look handler a no-op). Raw key
		// state, not an action: IA_LookStick stays camera-only.
		float StickX = 0.f;
		float StickY = 0.f;
		PC->GetInputAnalogStickState(EControllerAnalogStick::CAS_RightStick, StickX, StickY);
		const FVector2D Stick(StickX, StickY);
		if (Stick.Size() > StickDeadZone)
		{
			// Stick up = +Y; screen Y grows downward.
			AccumDelta += FVector2D(Stick.X, -Stick.Y) * StickCursorPxPerSec * InDeltaTime;
		}
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
		const float SectorDeg = 360.f / static_cast<float>(NumSectors);
		const float ThetaDeg = FMath::RadiansToDegrees(FMath::Atan2(AccumDelta.X, -AccumDelta.Y));
		const float Wrapped = FMath::Fmod(ThetaDeg + 360.f + SectorDeg * 0.5f, 360.f);
		NewHover = FMath::Clamp(FMath::FloorToInt(Wrapped / SectorDeg), 0, NumSectors - 1);
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

int32 UPFBuildWheelWidget::NativePaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry,
	const FSlateRect& MyCullingRect, FSlateWindowElementList& OutDrawElements, int32 LayerId,
	const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const
{
	// Full pie-slice highlight under the labels (Tom 2026-07-24: "highlights like a pie chart,
	// highlighting that option entirely"). A translucent wedge fan spanning the hovered sector's
	// whole 30°, from just outside the dead zone to past the swatches.
	if (bWheelOpen && HoveredSector != INDEX_NONE && FSlateApplication::IsInitialized())
	{
		const float SectorDeg = 360.f / static_cast<float>(NumSectors);
		const float StartDeg = HoveredSector * SectorDeg - SectorDeg * 0.5f;
		constexpr int32 Segs = 8;
		constexpr float InnerR = DeadZonePx * 0.55f;
		const float OuterR = SectorRadiusPx + 60.f;
		const FVector2f Center(AllottedGeometry.GetLocalSize() * 0.5f);
		const FSlateRenderTransform& RT = AllottedGeometry.ToPaintGeometry().GetAccumulatedRenderTransform();
		const FColor Fill = FLinearColor(1.f, 0.55f, 0.10f, 0.30f).ToFColor(true);   // translucent accent glass

		TArray<FSlateVertex> Verts;
		TArray<SlateIndex> Indices;
		Verts.Reserve((Segs + 1) * 2);
		Indices.Reserve(Segs * 6);
		for (int32 SegIdx = 0; SegIdx <= Segs; ++SegIdx)
		{
			const float Deg = StartDeg + SectorDeg * static_cast<float>(SegIdx) / static_cast<float>(Segs);
			const float Rad = FMath::DegreesToRadians(Deg);
			const FVector2f Dir(FMath::Sin(Rad), -FMath::Cos(Rad));   // clockwise from top
			Verts.Add(FSlateVertex::Make<ESlateVertexRounding::Disabled>(
				RT, Center + Dir * InnerR, FVector2f::ZeroVector, Fill));
			Verts.Add(FSlateVertex::Make<ESlateVertexRounding::Disabled>(
				RT, Center + Dir * OuterR, FVector2f::ZeroVector, Fill));
		}
		for (int32 SegIdx = 0; SegIdx < Segs; ++SegIdx)
		{
			const SlateIndex I0 = static_cast<SlateIndex>(SegIdx * 2);         // inner this spoke
			const SlateIndex O0 = static_cast<SlateIndex>(SegIdx * 2 + 1);     // outer this spoke
			const SlateIndex I1 = static_cast<SlateIndex>(SegIdx * 2 + 2);     // inner next spoke
			const SlateIndex O1 = static_cast<SlateIndex>(SegIdx * 2 + 3);     // outer next spoke
			Indices.Append({ I0, O0, O1,  I0, O1, I1 });
		}

		const FSlateBrush* WhiteBrush = FCoreStyle::Get().GetBrush("WhiteBrush");
		const FSlateResourceHandle Handle =
			FSlateApplication::Get().GetRenderer()->GetResourceHandle(*WhiteBrush);
		FSlateDrawElement::MakeCustomVerts(OutDrawElements, LayerId, Handle, Verts, Indices,
			nullptr, 0, 0);
	}

	// Children (swatches, labels, cursor dot) paint ABOVE the wedge.
	return Super::NativePaint(Args, AllottedGeometry, MyCullingRect, OutDrawElements, LayerId + 1,
		InWidgetStyle, bParentEnabled);
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
		CenterReadout->SetText(FText::FromString(FPFInputDevice::IsGamepadPrimary()
			? TEXT("hold LB · aim with stick · release picks")
			: TEXT("hold Q · aim a slice · release picks")));
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
	const EPFBuildTool Tool = SectorTool(SectorIndex);
	if (Tool == EPFBuildTool::Delete)
	{
		return FText::FromString(TEXT("Delete — full refund · release Q"));
	}

	int32 Remaining = -1;
	if (const APlayerController* PC = GetOwningPlayer())
	{
		if (const ACombatForgePlayerState* PS = PC->GetPlayerState<ACombatForgePlayerState>())
		{
			const EPFPieceType Type = static_cast<EPFPieceType>(static_cast<uint8>(Tool));
			Remaining = PFIsProp(Type) ? PS->PropBudget : PS->StructuralBudget;
		}
	}

	if (Remaining >= 0)
	{
		return FText::FromString(FString::Printf(TEXT("%s — %d left · release Q"), ToolDisplayName(Tool), Remaining));
	}
	return FText::FromString(FString::Printf(TEXT("%s · release Q"), ToolDisplayName(Tool)));
}
