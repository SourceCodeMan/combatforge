// Copyright (c) 2026 Tom Chapman. All rights reserved.

#include "UI/PFVoteWidget.h"

#include "Core/PaintForgeGameState.h"
#include "Core/PaintForgePlayerController.h"

#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/SizeBox.h"
#include "Components/TextBlock.h"
#include "Components/UniformGridPanel.h"
#include "Components/UniformGridSlot.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Components/WidgetSwitcher.h"
#include "Rendering/DrawElements.h"
#include "Styling/CoreStyle.h"

namespace
{
	const FLinearColor GChipNeutral(0.14f, 0.15f, 0.17f, 1.f);
	const FLinearColor GChipLiked(0.10f, 0.55f, 0.20f, 1.f);
	const FLinearColor GChipDisliked(0.60f, 0.13f, 0.10f, 1.f);

	FSlateFontInfo PFVoteFont(int32 Size, bool bBold)
	{
		return FCoreStyle::GetDefaultFontStyle(bBold ? FName(TEXT("Bold")) : FName(TEXT("Regular")), Size);
	}
}

// ----------------------------------------------------------------- chip button

void UPFVoteChipButton::InitChip(UPFVoteWidget* InOwner, int32 InChipIndex)
{
	OwnerWidget = InOwner;
	ChipIndex = InChipIndex;
	OnClicked.AddUniqueDynamic(this, &UPFVoteChipButton::HandleChipClicked);
}

void UPFVoteChipButton::HandleChipClicked()
{
	if (OwnerWidget.IsValid())
	{
		OwnerWidget->NotifyChipClicked(ChipIndex);
	}
}

// ------------------------------------------------------------------ vote widget

TSharedRef<SWidget> UPFVoteWidget::RebuildWidget()
{
	if (WidgetTree && !WidgetTree->RootWidget)
	{
		BuildTree(); // root assigned BEFORE Super::RebuildWidget (02 R3)
	}
	return Super::RebuildWidget();
}

void UPFVoteWidget::BuildTree()
{
	UCanvasPanel* RootCanvas = WidgetTree->ConstructWidget<UCanvasPanel>();
	WidgetTree->RootWidget = RootCanvas;

	// Full-screen dim so the vote reads as modal.
	UBorder* Dim = WidgetTree->ConstructWidget<UBorder>();
	Dim->SetBrushColor(FLinearColor(0.f, 0.f, 0.f, 0.55f));
	if (UCanvasPanelSlot* CSlot = RootCanvas->AddChildToCanvas(Dim))
	{
		CSlot->SetAnchors(FAnchors(0.f, 0.f, 1.f, 1.f));
		CSlot->SetOffsets(FMargin(0.f));
	}

	// Countdown number — the ring is painted around it in NativePaint.
	TimerText = WidgetTree->ConstructWidget<UTextBlock>();
	TimerText->SetFont(PFVoteFont(22, true));
	TimerText->SetColorAndOpacity(FSlateColor(FLinearColor::White));
	TimerText->SetJustification(ETextJustify::Center);
	if (UCanvasPanelSlot* CSlot = RootCanvas->AddChildToCanvas(TimerText))
	{
		CSlot->SetAnchors(FAnchors(0.5f, 0.f));
		CSlot->SetAlignment(FVector2D(0.5f, 0.5f));
		CSlot->SetPosition(FVector2D(0.f, RingCenterY));
		CSlot->SetAutoSize(true);
	}

	// ---- Step switcher ----
	StepSwitcher = WidgetTree->ConstructWidget<UWidgetSwitcher>();
	if (UCanvasPanelSlot* CSlot = RootCanvas->AddChildToCanvas(StepSwitcher))
	{
		CSlot->SetAnchors(FAnchors(0.5f, 0.5f));
		CSlot->SetAlignment(FVector2D(0.5f, 0.5f));
		CSlot->SetPosition(FVector2D(0.f, 0.f));
		CSlot->SetAutoSize(true);
	}

	// ---- Step 1: the verdict ----
	UVerticalBox* Step1 = WidgetTree->ConstructWidget<UVerticalBox>();
	{
		UTextBlock* Question = WidgetTree->ConstructWidget<UTextBlock>();
		Question->SetText(FText::FromString(TEXT("Did you like this arena?")));
		Question->SetFont(PFVoteFont(26, true));
		Question->SetColorAndOpacity(FSlateColor(FLinearColor::White));
		Question->SetJustification(ETextJustify::Center);
		if (UVerticalBoxSlot* VSlot = Step1->AddChildToVerticalBox(Question))
		{
			VSlot->SetHorizontalAlignment(HAlign_Center);
			VSlot->SetPadding(FMargin(0.f, 0.f, 0.f, 28.f));
		}

		UHorizontalBox* ThumbRow = WidgetTree->ConstructWidget<UHorizontalBox>();
		auto MakeThumbButton = [this](const TCHAR* Label, const FLinearColor& Color) -> UButton*
		{
			UButton* Btn = WidgetTree->ConstructWidget<UButton>();
			Btn->SetBackgroundColor(Color);
			UTextBlock* Text = WidgetTree->ConstructWidget<UTextBlock>();
			Text->SetText(FText::FromString(Label));
			Text->SetFont(PFVoteFont(22, true));
			Text->SetColorAndOpacity(FSlateColor(FLinearColor::White));
			Text->SetJustification(ETextJustify::Center);
			Btn->SetContent(Text);
			USizeBox* Sizer = WidgetTree->ConstructWidget<USizeBox>();
			Sizer->SetWidthOverride(220.f);
			Sizer->SetHeightOverride(110.f);
			Sizer->SetContent(Btn);
			// SizeBox is added to the row by the caller via the returned button's parent.
			return Btn;
		};

		ThumbUpButton = MakeThumbButton(TEXT("▲  LIKED IT"), GChipLiked);
		ThumbDownButton = MakeThumbButton(TEXT("▼  NOT FOR ME"), GChipDisliked);
		ThumbUpButton->OnClicked.AddUniqueDynamic(this, &UPFVoteWidget::HandleThumbUpClicked);
		ThumbDownButton->OnClicked.AddUniqueDynamic(this, &UPFVoteWidget::HandleThumbDownClicked);

		if (UHorizontalBoxSlot* HSlot = ThumbRow->AddChildToHorizontalBox(ThumbUpButton->GetParent()))
		{
			HSlot->SetPadding(FMargin(14.f, 0.f));
		}
		if (UHorizontalBoxSlot* HSlot = ThumbRow->AddChildToHorizontalBox(ThumbDownButton->GetParent()))
		{
			HSlot->SetPadding(FMargin(14.f, 0.f));
		}
		if (UVerticalBoxSlot* VSlot = Step1->AddChildToVerticalBox(ThumbRow))
		{
			VSlot->SetHorizontalAlignment(HAlign_Center);
		}
	}
	StepSwitcher->AddChild(Step1);

	// ---- Step 2: category chips ----
	UVerticalBox* Step2 = WidgetTree->ConstructWidget<UVerticalBox>();
	{
		UTextBlock* Prompt = WidgetTree->ConstructWidget<UTextBlock>();
		Prompt->SetText(FText::FromString(TEXT("What stood out?  (click cycles: neutral → liked → disliked)")));
		Prompt->SetFont(PFVoteFont(18, true));
		Prompt->SetColorAndOpacity(FSlateColor(FLinearColor::White));
		Prompt->SetJustification(ETextJustify::Center);
		if (UVerticalBoxSlot* VSlot = Step2->AddChildToVerticalBox(Prompt))
		{
			VSlot->SetHorizontalAlignment(HAlign_Center);
			VSlot->SetPadding(FMargin(0.f, 0.f, 0.f, 16.f));
		}

		UUniformGridPanel* Grid = WidgetTree->ConstructWidget<UUniformGridPanel>();
		Grid->SetSlotPadding(FMargin(6.f));
		ChipButtons.Reset();
		ChipLabels.Reset();
		for (int32 i = 0; i < NumChips; ++i)
		{
			UPFVoteChipButton* Chip = WidgetTree->ConstructWidget<UPFVoteChipButton>();
			Chip->InitChip(this, i);
			Chip->SetBackgroundColor(GChipNeutral);

			UVerticalBox* ChipBox = WidgetTree->ConstructWidget<UVerticalBox>();

			UTextBlock* Label = WidgetTree->ConstructWidget<UTextBlock>();
			const FString CatName = PFVoteCategories::All.IsValidIndex(i)
				? PFVoteCategories::All[i].ToString() : FString::Printf(TEXT("cat%d"), i + 1);
			Label->SetText(FText::FromString(CatName));
			Label->SetFont(PFVoteFont(16, true));
			Label->SetColorAndOpacity(FSlateColor(FLinearColor::White));
			Label->SetJustification(ETextJustify::Center);
			if (UVerticalBoxSlot* VSlot = ChipBox->AddChildToVerticalBox(Label))
			{
				VSlot->SetHorizontalAlignment(HAlign_Center);
			}

			UTextBlock* Hint = WidgetTree->ConstructWidget<UTextBlock>();
			Hint->SetText(PFVoteCategories::HintText.IsValidIndex(i)
				? PFVoteCategories::HintText[i] : FText::GetEmpty());
			Hint->SetFont(PFVoteFont(10, false));
			Hint->SetColorAndOpacity(FSlateColor(FLinearColor(1.f, 1.f, 1.f, 0.6f)));
			Hint->SetJustification(ETextJustify::Center);
			if (UVerticalBoxSlot* VSlot = ChipBox->AddChildToVerticalBox(Hint))
			{
				VSlot->SetHorizontalAlignment(HAlign_Center);
			}

			Chip->SetContent(ChipBox);

			USizeBox* Sizer = WidgetTree->ConstructWidget<USizeBox>();
			Sizer->SetWidthOverride(190.f);
			Sizer->SetHeightOverride(64.f);
			Sizer->SetContent(Chip);

			if (UUniformGridSlot* GSlot = Grid->AddChildToUniformGrid(Sizer, i / 4, i % 4))
			{
				GSlot->SetHorizontalAlignment(HAlign_Fill);
				GSlot->SetVerticalAlignment(VAlign_Fill);
			}

			ChipButtons.Add(Chip);
			ChipLabels.Add(Label);
		}
		if (UVerticalBoxSlot* VSlot = Step2->AddChildToVerticalBox(Grid))
		{
			VSlot->SetHorizontalAlignment(HAlign_Center);
		}

		SelectionCountText = WidgetTree->ConstructWidget<UTextBlock>();
		SelectionCountText->SetFont(PFVoteFont(13, false));
		SelectionCountText->SetColorAndOpacity(FSlateColor(FLinearColor(1.f, 1.f, 1.f, 0.7f)));
		SelectionCountText->SetJustification(ETextJustify::Center);
		if (UVerticalBoxSlot* VSlot = Step2->AddChildToVerticalBox(SelectionCountText))
		{
			VSlot->SetHorizontalAlignment(HAlign_Center);
			VSlot->SetPadding(FMargin(0.f, 10.f, 0.f, 4.f));
		}

		SubmitButton = WidgetTree->ConstructWidget<UButton>();
		SubmitButton->SetBackgroundColor(FLinearColor(0.12f, 0.35f, 0.75f));
		SubmitButton->OnClicked.AddUniqueDynamic(this, &UPFVoteWidget::HandleSubmitClicked);
		UTextBlock* SubmitLabel = WidgetTree->ConstructWidget<UTextBlock>();
		SubmitLabel->SetText(FText::FromString(TEXT("SUBMIT")));
		SubmitLabel->SetFont(PFVoteFont(18, true));
		SubmitLabel->SetColorAndOpacity(FSlateColor(FLinearColor::White));
		SubmitLabel->SetJustification(ETextJustify::Center);
		SubmitButton->SetContent(SubmitLabel);
		USizeBox* SubmitSizer = WidgetTree->ConstructWidget<USizeBox>();
		SubmitSizer->SetWidthOverride(240.f);
		SubmitSizer->SetHeightOverride(52.f);
		SubmitSizer->SetContent(SubmitButton);
		if (UVerticalBoxSlot* VSlot = Step2->AddChildToVerticalBox(SubmitSizer))
		{
			VSlot->SetHorizontalAlignment(HAlign_Center);
			VSlot->SetPadding(FMargin(0.f, 8.f));
		}
	}
	StepSwitcher->AddChild(Step2);

	// Submitted / status line.
	StatusText = WidgetTree->ConstructWidget<UTextBlock>();
	StatusText->SetFont(PFVoteFont(16, true));
	StatusText->SetColorAndOpacity(FSlateColor(FLinearColor(0.2f, 0.9f, 0.3f)));
	StatusText->SetJustification(ETextJustify::Center);
	StatusText->SetText(FText::GetEmpty());
	if (UCanvasPanelSlot* CSlot = RootCanvas->AddChildToCanvas(StatusText))
	{
		CSlot->SetAnchors(FAnchors(0.5f, 1.f));
		CSlot->SetAlignment(FVector2D(0.5f, 1.f));
		CSlot->SetPosition(FVector2D(0.f, -60.f));
		CSlot->SetAutoSize(true);
	}

	ChipStates.Init(EChipState::Neutral, NumChips);
}

void UPFVoteWidget::NativeConstruct()
{
	Super::NativeConstruct();
	if (ChipStates.Num() != NumChips)
	{
		ChipStates.Init(EChipState::Neutral, NumChips);
	}
	UpdateSelectionCount();
}

void UPFVoteWidget::ResetForVote()
{
	Thumb = EPFThumbVote::Abstained;
	bSubmitted = false;
	ChipStates.Init(EChipState::Neutral, NumChips);
	for (int32 i = 0; i < NumChips; ++i)
	{
		UpdateChipVisual(i);
	}
	if (StepSwitcher)
	{
		StepSwitcher->SetActiveWidgetIndex(0);
	}
	if (StatusText)
	{
		StatusText->SetText(FText::GetEmpty());
	}
	auto Enable = [](UButton* Btn) { if (Btn) { Btn->SetIsEnabled(true); } };
	Enable(ThumbUpButton);
	Enable(ThumbDownButton);
	Enable(SubmitButton);
	for (UPFVoteChipButton* Chip : ChipButtons)
	{
		Enable(Chip);
	}
	UpdateSelectionCount();
}

void UPFVoteWidget::HandleThumbUpClicked()
{
	if (bSubmitted)
	{
		return;
	}
	Thumb = EPFThumbVote::Up;
	if (StepSwitcher)
	{
		StepSwitcher->SetActiveWidgetIndex(1); // advances instantly (01 §4.2)
	}
}

void UPFVoteWidget::HandleThumbDownClicked()
{
	if (bSubmitted)
	{
		return;
	}
	Thumb = EPFThumbVote::Down;
	if (StepSwitcher)
	{
		StepSwitcher->SetActiveWidgetIndex(1);
	}
}

void UPFVoteWidget::NotifyChipClicked(int32 ChipIndex)
{
	if (bSubmitted || !ChipStates.IsValidIndex(ChipIndex))
	{
		return;
	}
	const EChipState Current = ChipStates[ChipIndex];
	EChipState Next = EChipState::Neutral;
	switch (Current)
	{
	case EChipState::Neutral:  Next = EChipState::Liked;    break;
	case EChipState::Liked:    Next = EChipState::Disliked; break;
	case EChipState::Disliked: Next = EChipState::Neutral;  break;
	}
	// Only Neutral -> Liked adds a selection; deny it at the cap (T30).
	if (Current == EChipState::Neutral && Next != EChipState::Neutral && CountSelections() >= MaxSelections)
	{
		return;
	}
	ChipStates[ChipIndex] = Next;
	UpdateChipVisual(ChipIndex);
	UpdateSelectionCount();
}

int32 UPFVoteWidget::CountSelections() const
{
	int32 Count = 0;
	for (const EChipState State : ChipStates)
	{
		if (State != EChipState::Neutral)
		{
			++Count;
		}
	}
	return Count;
}

void UPFVoteWidget::UpdateChipVisual(int32 ChipIndex)
{
	UPFVoteChipButton* Chip = ChipButtons.IsValidIndex(ChipIndex) ? ChipButtons[ChipIndex].Get() : nullptr;
	UTextBlock* Label = ChipLabels.IsValidIndex(ChipIndex) ? ChipLabels[ChipIndex].Get() : nullptr;
	if (!Chip || !ChipStates.IsValidIndex(ChipIndex))
	{
		return;
	}
	const FString CatName = PFVoteCategories::All.IsValidIndex(ChipIndex)
		? PFVoteCategories::All[ChipIndex].ToString() : FString::Printf(TEXT("cat%d"), ChipIndex + 1);
	switch (ChipStates[ChipIndex])
	{
	case EChipState::Neutral:
		Chip->SetBackgroundColor(GChipNeutral);
		if (Label) { Label->SetText(FText::FromString(CatName)); }
		break;
	case EChipState::Liked:
		Chip->SetBackgroundColor(GChipLiked);
		if (Label) { Label->SetText(FText::FromString(FString::Printf(TEXT("▲ %s"), *CatName))); }
		break;
	case EChipState::Disliked:
		Chip->SetBackgroundColor(GChipDisliked);
		if (Label) { Label->SetText(FText::FromString(FString::Printf(TEXT("▼ %s"), *CatName))); }
		break;
	}
}

void UPFVoteWidget::UpdateSelectionCount()
{
	if (SelectionCountText)
	{
		SelectionCountText->SetText(FText::FromString(
			FString::Printf(TEXT("%d / %d selected"), CountSelections(), MaxSelections)));
	}
}

void UPFVoteWidget::HandleSubmitClicked()
{
	SubmitVote();
}

void UPFVoteWidget::SubmitVote()
{
	if (bSubmitted)
	{
		return;
	}
	bSubmitted = true;

	TArray<uint8> LikedIds;
	TArray<uint8> DislikedIds;
	for (int32 i = 0; i < ChipStates.Num(); ++i)
	{
		if (ChipStates[i] == EChipState::Liked)
		{
			LikedIds.Add(static_cast<uint8>(i + 1)); // category IDs are 1-based (T13/T30)
		}
		else if (ChipStates[i] == EChipState::Disliked)
		{
			DislikedIds.Add(static_cast<uint8>(i + 1));
		}
	}

	if (APaintForgePlayerController* PC = Cast<APaintForgePlayerController>(GetOwningPlayer()))
	{
		PC->ServerSubmitVote(Thumb, LikedIds, DislikedIds);
	}

	auto Disable = [](UButton* Btn) { if (Btn) { Btn->SetIsEnabled(false); } };
	Disable(ThumbUpButton);
	Disable(ThumbDownButton);
	Disable(SubmitButton);
	for (UPFVoteChipButton* Chip : ChipButtons)
	{
		Disable(Chip);
	}
	if (StatusText)
	{
		StatusText->SetText(FText::FromString(TEXT("Vote submitted")));
	}
}

void UPFVoteWidget::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);

	const UWorld* World = GetWorld();
	const APaintForgeGameState* GS = World ? World->GetGameState<APaintForgeGameState>() : nullptr;
	if (!GS || GS->Phase != EPFMatchPhase::Vote)
	{
		return;
	}

	const float Remaining = GS->GetPhaseTimeRemaining();
	if (TimerText)
	{
		TimerText->SetText(FText::FromString(
			FString::Printf(TEXT("%d"), FMath::Max(0, FMath::CeilToInt(Remaining)))));
	}

	// Timeout auto-submits whatever is selected; no thumb = Abstained (01 §4.2).
	if (Remaining <= 0.f && !bSubmitted)
	{
		SubmitVote();
	}
}

int32 UPFVoteWidget::NativePaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry,
	const FSlateRect& MyCullingRect, FSlateWindowElementList& OutDrawElements, int32 LayerId,
	const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const
{
	const int32 MaxLayer = Super::NativePaint(
		Args, AllottedGeometry, MyCullingRect, OutDrawElements, LayerId, InWidgetStyle, bParentEnabled);

	// 20 s countdown ring around the timer number.
	const UWorld* World = GetWorld();
	const APaintForgeGameState* GS = World ? World->GetGameState<APaintForgeGameState>() : nullptr;
	if (!GS || GS->Phase != EPFMatchPhase::Vote)
	{
		return MaxLayer;
	}
	const float Fraction = FMath::Clamp(GS->GetPhaseTimeRemaining() / VoteDuration, 0.f, 1.f);
	if (Fraction <= 0.f)
	{
		return MaxLayer;
	}

	const FVector2D Center(AllottedGeometry.GetLocalSize().X * 0.5f, RingCenterY);
	const int32 MaxSegments = 48;
	const int32 Segments = FMath::Max(2, FMath::CeilToInt(Fraction * MaxSegments));

	// FVector2f points: Slate's single-precision overload of MakeLines is the canonical one
	// in UE 5.x (§5.17 keeps warnings-as-errors on — avoid the legacy FVector2D path).
	TArray<FVector2f> Points;
	Points.Reserve(Segments + 1);
	for (int32 i = 0; i <= Segments; ++i)
	{
		// Clockwise from 12 o'clock, sweeping the remaining fraction.
		const float Angle = (static_cast<float>(i) / MaxSegments) * 2.f * PI;
		Points.Add(FVector2f(Center + FVector2D(FMath::Sin(Angle), -FMath::Cos(Angle)) * RingRadiusPx));
	}

	const FLinearColor RingColor = Fraction < 0.25f
		? FLinearColor(0.95f, 0.15f, 0.1f)
		: FLinearColor(1.f, 1.f, 1.f, 0.9f);
	FSlateDrawElement::MakeLines(OutDrawElements, MaxLayer + 1, AllottedGeometry.ToPaintGeometry(),
		Points, ESlateDrawEffect::None, RingColor, true, 3.f);

	return MaxLayer + 1;
}
