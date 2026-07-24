// Copyright (c) 2026 Tom Chapman. All rights reserved.

#include "UI/PFCycleBar.h"

#include "Core/CombatForgeGameState.h"

#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/TextBlock.h"
#include "Styling/CoreStyle.h"

namespace
{
	const FLinearColor GSegCurrent(1.f, 0.55f, 0.10f, 0.95f);    // the stage being played (accent)
	const FLinearColor GSegDone(0.50f, 0.28f, 0.06f, 0.85f);     // stages already played this wheel
	const FLinearColor GSegUpcoming(0.10f, 0.11f, 0.14f, 0.85f); // still ahead
	const TCHAR* GSegLabels[3] = { TEXT("1 · CREATIVE"), TEXT("2 · REMIX"), TEXT("3 · REMIX SWAP") };
}

UWidget* PFCycleBar::Build(UWidgetTree* Tree,
	TArray<TObjectPtr<UBorder>>& OutSegs, TArray<TObjectPtr<UTextBlock>>& OutTexts)
{
	UHorizontalBox* Row = Tree->ConstructWidget<UHorizontalBox>();
	for (int32 i = 0; i < 3; ++i)
	{
		UBorder* Seg = Tree->ConstructWidget<UBorder>();
		Seg->SetBrushColor(GSegUpcoming);
		Seg->SetPadding(FMargin(10.f, 3.f));
		Seg->SetHorizontalAlignment(HAlign_Center);
		Seg->SetVerticalAlignment(VAlign_Center);

		UTextBlock* Label = Tree->ConstructWidget<UTextBlock>();
		Label->SetText(FText::FromString(GSegLabels[i]));
		Label->SetFont(FCoreStyle::GetDefaultFontStyle(FName(TEXT("Bold")), 11));
		Label->SetColorAndOpacity(FSlateColor(FLinearColor(1.f, 1.f, 1.f, 0.45f)));
		Label->SetJustification(ETextJustify::Center);
		Seg->SetContent(Label);

		if (UHorizontalBoxSlot* HSlot = Row->AddChildToHorizontalBox(Seg))
		{
			HSlot->SetPadding(FMargin(i == 0 ? 0.f : 2.f, 0.f, 0.f, 0.f));
			HSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));   // equal thirds
		}
		OutSegs.Add(Seg);
		OutTexts.Add(Label);
	}
	return Row;
}

void PFCycleBar::Update(const ACombatForgeGameState* GS, UWidget* BarRoot,
	const TArray<TObjectPtr<UBorder>>& Segs, const TArray<TObjectPtr<UTextBlock>>& Texts)
{
	if (!BarRoot)
	{
		return;
	}
	const bool bActive = GS && GS->BuildMode != EPFBuildMode::PlayOnly
		&& GS->MatchType != EPFMatchType::FreeForAll;
	BarRoot->SetVisibility(bActive ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
	if (!bActive)
	{
		return;
	}
	const int32 Stage = FMath::Clamp(static_cast<int32>(GS->CycleStage), 0, 2);
	for (int32 i = 0; i < Segs.Num() && i < Texts.Num(); ++i)
	{
		if (Segs[i])
		{
			Segs[i]->SetBrushColor(i == Stage ? GSegCurrent : (i < Stage ? GSegDone : GSegUpcoming));
		}
		if (Texts[i])
		{
			Texts[i]->SetColorAndOpacity(FSlateColor(
				i <= Stage ? FLinearColor::White : FLinearColor(1.f, 1.f, 1.f, 0.45f)));
		}
	}
}
