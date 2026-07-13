// Copyright (c) 2026 Tom Chapman. All rights reserved.

#include "UI/PFLoadingMenuWidget.h"

#include "PaintForge.h"
#include "Core/PaintForgeGameState.h"
#include "Core/PaintForgePlayerController.h"
#include "Voting/PFRatingSubsystem.h"
#include "Engine/GameInstance.h"

#include "Blueprint/WidgetTree.h"
#include "Brushes/SlateColorBrush.h"
#include "Components/Button.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/CheckBox.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/Image.h"
#include "Components/ProgressBar.h"
#include "Components/SizeBox.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Components/WidgetSwitcher.h"
#include "Core/PFUserPrefs.h"
#include "Player/PaintForgeCharacter.h"
#include "Player/PFCharacterPreviewActor.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "ImageUtils.h"
#include "Misc/Paths.h"
#include "GameFramework/PlayerController.h"
#include "Materials/MaterialInterface.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "ShaderCompiler.h"
#include "ShaderPipelineCache.h"
#include "Styling/CoreStyle.h"
#include "UObject/SoftObjectPath.h"

namespace
{
	FSlateFontInfo PFLoadFont(int32 Size, bool bBold)
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

	FString FormatLabel(uint8 TeamSize)
	{
		return FString::Printf(TEXT("%dv%d"), TeamSize, TeamSize);
	}

	FString FormatBlurb(uint8 TeamSize, bool bBots)
	{
		if (bBots)
		{
			return FString::Printf(TEXT("%dv%d · empty slots filled with bots at match start"),
				TeamSize, TeamSize);
		}
		return FString::Printf(TEXT("%dv%d · humans only · no bot fill"), TeamSize, TeamSize);
	}
}

// ---------------------------------------------------------------- map row button

void UPFMapPickButton::InitRow(UPFLoadingMenuWidget* InOwner, int32 InSlotIndex)
{
	OwnerWidget = InOwner;
	SlotIndex = InSlotIndex;
	OnClicked.AddUniqueDynamic(this, &UPFMapPickButton::HandleClicked);
}

void UPFMapPickButton::HandleClicked()
{
	if (OwnerWidget.IsValid())
	{
		OwnerWidget->NotifyMapSlotClicked(SlotIndex);
	}
}

// -------------------------------------------------------------- loading menu

TSharedRef<SWidget> UPFLoadingMenuWidget::RebuildWidget()
{
	if (WidgetTree && !WidgetTree->RootWidget)
	{
		BuildTree();
	}
	return Super::RebuildWidget();
}

UButton* UPFLoadingMenuWidget::MakeMenuTab(const FString& Label, FName Name)
{
	UButton* Btn = WidgetTree->ConstructWidget<UButton>(UButton::StaticClass(), Name);
	Btn->SetBackgroundColor(FLinearColor(0.14f, 0.15f, 0.18f, 0.95f));
	UTextBlock* T = WidgetTree->ConstructWidget<UTextBlock>();
	T->SetText(FText::FromString(Label));
	T->SetFont(PFLoadFont(14, true));
	T->SetColorAndOpacity(FSlateColor(FLinearColor(0.12f, 0.12f, 0.14f)));
	T->SetJustification(ETextJustify::Center);
	Btn->AddChild(T);
	return Btn;
}

UButton* UPFLoadingMenuWidget::MakeSetupButton(const FString& Label, TObjectPtr<UTextBlock>& OutValueText, FName Name)
{
	UButton* Btn = WidgetTree->ConstructWidget<UButton>(UButton::StaticClass(), Name);
	Btn->SetBackgroundColor(FLinearColor(0.12f, 0.13f, 0.16f, 0.95f));

	UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>();

	UTextBlock* LabelText = WidgetTree->ConstructWidget<UTextBlock>();
	LabelText->SetText(FText::FromString(Label));
	LabelText->SetFont(PFLoadFont(13, true));
	LabelText->SetColorAndOpacity(FSlateColor(FLinearColor(0.55f, 0.72f, 0.95f)));
	USizeBox* LabelSizer = WidgetTree->ConstructWidget<USizeBox>();
	LabelSizer->SetWidthOverride(90.f);
	LabelSizer->SetContent(LabelText);
	if (UHorizontalBoxSlot* HS = Row->AddChildToHorizontalBox(LabelSizer))
	{
		HS->SetVerticalAlignment(VAlign_Center);
		HS->SetPadding(FMargin(12.f, 10.f, 8.f, 10.f));
	}

	OutValueText = WidgetTree->ConstructWidget<UTextBlock>();
	OutValueText->SetFont(PFLoadFont(16, true));
	OutValueText->SetColorAndOpacity(FSlateColor(FLinearColor::White));
	if (UHorizontalBoxSlot* HS = Row->AddChildToHorizontalBox(OutValueText))
	{
		HS->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		HS->SetVerticalAlignment(VAlign_Center);
		HS->SetPadding(FMargin(0.f, 10.f, 12.f, 10.f));
	}

	Btn->SetContent(Row);
	return Btn;
}

void UPFLoadingMenuWidget::AddHowToLine(UVerticalBox* Box, const FString& Text, int32 Size, bool bBold,
	const FLinearColor& Color)
{
	UTextBlock* T = WidgetTree->ConstructWidget<UTextBlock>();
	T->SetText(FText::FromString(Text));
	T->SetFont(PFLoadFont(Size, bBold));
	T->SetColorAndOpacity(FSlateColor(Color));
	T->SetAutoWrapText(true);
	if (UVerticalBoxSlot* V = Box->AddChildToVerticalBox(T))
	{
		V->SetPadding(FMargin(0.f, bBold ? 8.f : 2.f, 0.f, 0.f));
		V->SetHorizontalAlignment(HAlign_Fill);
	}
}

void UPFLoadingMenuWidget::BuildHowToPlayPage(UVerticalBox* Box)
{
	const FLinearColor Head(1.f, 0.92f, 0.35f);
	const FLinearColor Body(0.88f, 0.89f, 0.92f);
	const FLinearColor Key(0.65f, 0.82f, 1.f);
	const FLinearColor Dim(0.55f, 0.57f, 0.62f);

	AddHowToLine(Box, TEXT("THE MATCH"), 14, true, Head);
	AddHowToLine(Box, TEXT("Lobby → Build forts → Fight. Pick mode, type, format, and bots here, then Enter Lobby."), 12, false, Body);

	AddHowToLine(Box, TEXT("MOVE & LOOK"), 14, true, Head);
	AddHowToLine(Box, TEXT("WASD move · Mouse look · Space jump · Shift sprint · Ctrl/C crouch"), 12, false, Key);

	AddHowToLine(Box, TEXT("COMBAT"), 14, true, Head);
	AddHowToLine(Box, TEXT("LMB fire · RMB aim · R reload · Tag opponents with BBs"), 12, false, Key);

	AddHowToLine(Box, TEXT("BUILD"), 14, true, Head);
	AddHowToLine(Box, TEXT("F1–F4 structure · barrel/crate/boxes cover · LMB place · R rotate · X delete · Hold Q wheel"), 12, false, Key);

	AddHowToLine(Box, TEXT("LOBBY"), 14, true, Head);
	AddHowToLine(Box, TEXT("F ready · Enter host start · Tab scoreboard · Esc options (How to Play anytime)"), 12, false, Key);

	AddHowToLine(Box, TEXT("MODE vs TYPE"), 14, true, Head);
	AddHowToLine(Box, TEXT("Mode = build style (Creative / Improvement / Play-Only). Type = win condition (Elim, Skirmish, CTF…)."), 12, false, Dim);
	AddHowToLine(Box, TEXT("Improvement & Play-Only: pick a community map (top 100, 10 per page) on Match Setup."), 12, false, Dim);
	AddHowToLine(Box, TEXT("QUICK START: Play-Only Skirmish 4v4 with bots on a starter fort — best first session."), 12, false, Dim);
}

void UPFLoadingMenuWidget::BuildMapPicker(UVerticalBox* Parent)
{
	MapPickerBox = WidgetTree->ConstructWidget<UVerticalBox>();
	if (UVerticalBoxSlot* V = Parent->AddChildToVerticalBox(MapPickerBox))
	{
		V->SetPadding(FMargin(0.f, 12.f, 0.f, 0.f));
		V->SetHorizontalAlignment(HAlign_Fill);
	}

	MapPickerHeader = WidgetTree->ConstructWidget<UTextBlock>();
	MapPickerHeader->SetText(FText::FromString(TEXT("COMMUNITY MAP")));
	MapPickerHeader->SetFont(PFLoadFont(13, true));
	MapPickerHeader->SetColorAndOpacity(FSlateColor(FLinearColor(1.f, 0.92f, 0.35f)));
	MapPickerHeader->SetJustification(ETextJustify::Center);
	if (UVerticalBoxSlot* V = MapPickerBox->AddChildToVerticalBox(MapPickerHeader))
	{
		V->SetHorizontalAlignment(HAlign_Center);
		V->SetPadding(FMargin(0.f, 0.f, 0.f, 6.f));
	}

	MapSelectedLabel = WidgetTree->ConstructWidget<UTextBlock>();
	MapSelectedLabel->SetFont(PFLoadFont(12, false));
	MapSelectedLabel->SetColorAndOpacity(FSlateColor(FLinearColor(0.75f, 0.88f, 1.f)));
	MapSelectedLabel->SetJustification(ETextJustify::Center);
	MapSelectedLabel->SetAutoWrapText(true);
	if (UVerticalBoxSlot* V = MapPickerBox->AddChildToVerticalBox(MapSelectedLabel))
	{
		V->SetHorizontalAlignment(HAlign_Fill);
		V->SetPadding(FMargin(0.f, 0.f, 0.f, 6.f));
	}

	// Auto + page controls
	UHorizontalBox* PageRow = WidgetTree->ConstructWidget<UHorizontalBox>();
	MapAutoBtn = MakeMenuTab(TEXT("  AUTO (TOP)  "), TEXT("MapAuto"));
	MapAutoBtn->OnClicked.AddDynamic(this, &UPFLoadingMenuWidget::OnMapAutoClicked);
	MapPagePrevBtn = MakeMenuTab(TEXT("  <  "), TEXT("MapPrev"));
	MapPagePrevBtn->OnClicked.AddDynamic(this, &UPFLoadingMenuWidget::OnMapPagePrev);
	MapPageNextBtn = MakeMenuTab(TEXT("  >  "), TEXT("MapNext"));
	MapPageNextBtn->OnClicked.AddDynamic(this, &UPFLoadingMenuWidget::OnMapPageNext);
	MapPageLabel = WidgetTree->ConstructWidget<UTextBlock>();
	MapPageLabel->SetFont(PFLoadFont(12, true));
	MapPageLabel->SetColorAndOpacity(FSlateColor(FLinearColor(0.85f, 0.87f, 0.9f)));
	MapPageLabel->SetJustification(ETextJustify::Center);

	auto AddH = [PageRow](UWidget* W, float Pad = 3.f)
	{
		if (UHorizontalBoxSlot* H = PageRow->AddChildToHorizontalBox(W))
		{
			H->SetPadding(FMargin(Pad, 0.f));
			H->SetVerticalAlignment(VAlign_Center);
		}
	};
	AddH(MapAutoBtn);
	AddH(MapPagePrevBtn);
	if (UHorizontalBoxSlot* H = PageRow->AddChildToHorizontalBox(MapPageLabel))
	{
		H->SetPadding(FMargin(8.f, 0.f));
		H->SetVerticalAlignment(VAlign_Center);
		H->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
	}
	AddH(MapPageNextBtn);
	if (UVerticalBoxSlot* V = MapPickerBox->AddChildToVerticalBox(PageRow))
	{
		V->SetPadding(FMargin(0.f, 0.f, 0.f, 6.f));
		V->SetHorizontalAlignment(HAlign_Fill);
	}

	MapSlotButtons.Reset();
	MapSlotLabels.Reset();
	MapSlotImages.Reset();
	for (int32 i = 0; i < MapsPerPage; ++i)
	{
		UPFMapPickButton* Row = WidgetTree->ConstructWidget<UPFMapPickButton>(
			UPFMapPickButton::StaticClass(), *FString::Printf(TEXT("MapSlot%d"), i));
		Row->SetBackgroundColor(FLinearColor(0.10f, 0.11f, 0.14f, 0.95f));
		Row->InitRow(this, i);

		// A UButton holds ONE child, so pack [thumbnail | label] into a HorizontalBox.
		UHorizontalBox* RowContent = WidgetTree->ConstructWidget<UHorizontalBox>();

		UImage* Thumb = WidgetTree->ConstructWidget<UImage>();
		Thumb->SetBrush(FSlateColorBrush(FLinearColor(0.15f, 0.16f, 0.20f, 1.f)));   // fallback until a PNG loads
		USizeBox* ThumbBox = WidgetTree->ConstructWidget<USizeBox>();
		ThumbBox->SetWidthOverride(80.f);
		ThumbBox->SetHeightOverride(45.f);
		ThumbBox->SetContent(Thumb);
		if (UHorizontalBoxSlot* H = RowContent->AddChildToHorizontalBox(ThumbBox))
		{
			H->SetVerticalAlignment(VAlign_Center);
			H->SetPadding(FMargin(4.f, 2.f));
		}

		UTextBlock* Lab = WidgetTree->ConstructWidget<UTextBlock>();
		Lab->SetFont(PFLoadFont(12, false));
		Lab->SetColorAndOpacity(FSlateColor(FLinearColor(0.9f, 0.91f, 0.94f)));
		Lab->SetJustification(ETextJustify::Left);
		if (UHorizontalBoxSlot* H = RowContent->AddChildToHorizontalBox(Lab))
		{
			H->SetVerticalAlignment(VAlign_Center);
			H->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		}

		Row->AddChild(RowContent);

		MapSlotButtons.Add(Row);
		MapSlotLabels.Add(Lab);
		MapSlotImages.Add(Thumb);
		if (UVerticalBoxSlot* V = MapPickerBox->AddChildToVerticalBox(Row))
		{
			V->SetPadding(FMargin(0.f, 2.f));
			V->SetHorizontalAlignment(HAlign_Fill);
		}
	}
}

UTexture2D* UPFLoadingMenuWidget::GetMapPreview(const FString& JsonFileName)
{
	if (JsonFileName.IsEmpty())
	{
		return nullptr;
	}
	if (TObjectPtr<UTexture2D>* Cached = MapPreviewCache.Find(JsonFileName))
	{
		return Cached->Get();   // cached (possibly null = "tried, none on disk") — only load once per file
	}
	FString PngName = JsonFileName;
	PngName.RemoveFromEnd(TEXT(".json"));
	PngName += TEXT(".png");
	const FString PngPath = FPaths::ProjectSavedDir() / TEXT("Arenas") / PngName;
	UTexture2D* Tex = FPaths::FileExists(PngPath) ? FImageUtils::ImportFileAsTexture2D(PngPath) : nullptr;
	MapPreviewCache.Add(JsonFileName, Tex);
	return Tex;
}

void UPFLoadingMenuWidget::SelectMenuTab(int32 Index)
{
	ActiveMenuTab = FMath::Clamp(Index, 0, 3);
	if (MenuSwitcher)
	{
		MenuSwitcher->SetActiveWidgetIndex(ActiveMenuTab);
	}
	// Highlight active tab with a brighter plate.
	const FLinearColor Hot(1.f, 0.92f, 0.35f, 0.95f);
	const FLinearColor Cold(0.14f, 0.15f, 0.18f, 0.95f);
	if (TabSetup)     { TabSetup->SetBackgroundColor(ActiveMenuTab == 0 ? Hot : Cold); }
	if (TabHowTo)     { TabHowTo->SetBackgroundColor(ActiveMenuTab == 1 ? Hot : Cold); }
	if (TabLoadout)   { TabLoadout->SetBackgroundColor(ActiveMenuTab == 2 ? Hot : Cold); }
	if (TabCharacter) { TabCharacter->SetBackgroundColor(ActiveMenuTab == 3 ? Hot : Cold); }

	// Only run the render-target capture while the CHARACTER tab is visible.
	if (ActiveMenuTab == 3)
	{
		EnsureCharPreview();
		if (CharPreviewActor != nullptr)
		{
			CharPreviewActor->SetPreviewActive(true);
		}
	}
	else if (CharPreviewActor != nullptr)
	{
		CharPreviewActor->SetPreviewActive(false);
	}
}

void UPFLoadingMenuWidget::OnTabSetup() { SelectMenuTab(0); }
void UPFLoadingMenuWidget::OnTabHowTo() { SelectMenuTab(1); }
void UPFLoadingMenuWidget::OnTabLoadout() { SelectMenuTab(2); }
void UPFLoadingMenuWidget::OnTabCharacter() { SelectMenuTab(3); }

const TCHAR* UPFLoadingMenuWidget::MarkerPresetName(int32 Idx)
{
	switch (Idx)
	{
	case 1:  return TEXT("  Rapid — 14 bps · 30-mag / 150 total  ");
	case 2:  return TEXT("  Tournament — 10 bps · 30-mag / 150 total  ");
	default: return TEXT("  Standard — 12 bps · 30-mag / 150 total  ");
	}
}

const TCHAR* UPFLoadingMenuWidget::CrosshairStyleName(int32 Idx)
{
	switch (Idx)
	{
	case 1:  return TEXT("  Dot only  ");
	case 2:  return TEXT("  Cross only  ");
	default: return TEXT("  Cross + dot  ");
	}
}

void UPFLoadingMenuWidget::RefreshLoadoutLabels()
{
	if (LoadoutMarkerValueText)
	{
		LoadoutMarkerValueText->SetText(FText::FromString(MarkerPresetName(WorkingMarkerPreset)));
	}
	if (LoadoutCrosshairValueText)
	{
		LoadoutCrosshairValueText->SetText(FText::FromString(CrosshairStyleName(WorkingCrosshairStyle)));
	}
}

void UPFLoadingMenuWidget::OnMarkerCycle()
{
	WorkingMarkerPreset = (WorkingMarkerPreset + 1) % 3;
	FPFUserPrefs::SetMarkerPreset(WorkingMarkerPreset);
	FPFUserPrefs::Flush();
	RefreshLoadoutLabels();
}

void UPFLoadingMenuWidget::OnCrosshairCycle()
{
	WorkingCrosshairStyle = (WorkingCrosshairStyle + 1) % 3;
	FPFUserPrefs::SetCrosshairStyle(WorkingCrosshairStyle);
	FPFUserPrefs::Flush();
	RefreshLoadoutLabels();
}

void UPFLoadingMenuWidget::BuildLoadoutPage(UVerticalBox* Col)
{
	UTextBlock* Sub = WidgetTree->ConstructWidget<UTextBlock>();
	Sub->SetText(FText::FromString(TEXT("Local preferences — saved to this PC, applied when you spawn.")));
	Sub->SetColorAndOpacity(FSlateColor(FLinearColor(1.f, 1.f, 1.f, 0.65f)));
	Sub->SetJustification(ETextJustify::Center);
	if (UVerticalBoxSlot* V = Col->AddChildToVerticalBox(Sub))
	{
		V->SetPadding(FMargin(0.f, 4.f, 0.f, 16.f));
		V->SetHorizontalAlignment(HAlign_Center);
	}

	// Marker preset row
	{
		UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>();
		UTextBlock* Lab = WidgetTree->ConstructWidget<UTextBlock>();
		Lab->SetText(FText::FromString(TEXT("MARKER")));
		Lab->SetColorAndOpacity(FSlateColor(FLinearColor(0.55f, 0.72f, 0.95f)));
		USizeBox* Sizer = WidgetTree->ConstructWidget<USizeBox>();
		Sizer->SetWidthOverride(110.f);
		Sizer->SetContent(Lab);
		Row->AddChildToHorizontalBox(Sizer);
		LoadoutMarkerButton = WidgetTree->ConstructWidget<UButton>();
		LoadoutMarkerButton->SetBackgroundColor(FLinearColor(0.12f, 0.13f, 0.16f, 1.f));
		LoadoutMarkerButton->OnClicked.AddDynamic(this, &UPFLoadingMenuWidget::OnMarkerCycle);
		LoadoutMarkerValueText = WidgetTree->ConstructWidget<UTextBlock>();
		LoadoutMarkerValueText->SetColorAndOpacity(FSlateColor(FLinearColor::White));
		LoadoutMarkerButton->AddChild(LoadoutMarkerValueText);
		if (UHorizontalBoxSlot* H = Row->AddChildToHorizontalBox(LoadoutMarkerButton))
		{
			H->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
			H->SetPadding(FMargin(8.f, 0.f));
		}
		if (UVerticalBoxSlot* V = Col->AddChildToVerticalBox(Row))
		{
			V->SetPadding(FMargin(40.f, 6.f));
			V->SetHorizontalAlignment(HAlign_Fill);
		}
	}

	// Crosshair row
	{
		UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>();
		UTextBlock* Lab = WidgetTree->ConstructWidget<UTextBlock>();
		Lab->SetText(FText::FromString(TEXT("CROSSHAIR")));
		Lab->SetColorAndOpacity(FSlateColor(FLinearColor(0.55f, 0.72f, 0.95f)));
		USizeBox* Sizer = WidgetTree->ConstructWidget<USizeBox>();
		Sizer->SetWidthOverride(110.f);
		Sizer->SetContent(Lab);
		Row->AddChildToHorizontalBox(Sizer);
		LoadoutCrosshairButton = WidgetTree->ConstructWidget<UButton>();
		LoadoutCrosshairButton->SetBackgroundColor(FLinearColor(0.12f, 0.13f, 0.16f, 1.f));
		LoadoutCrosshairButton->OnClicked.AddDynamic(this, &UPFLoadingMenuWidget::OnCrosshairCycle);
		LoadoutCrosshairValueText = WidgetTree->ConstructWidget<UTextBlock>();
		LoadoutCrosshairValueText->SetColorAndOpacity(FSlateColor(FLinearColor::White));
		LoadoutCrosshairButton->AddChild(LoadoutCrosshairValueText);
		if (UHorizontalBoxSlot* H = Row->AddChildToHorizontalBox(LoadoutCrosshairButton))
		{
			H->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
			H->SetPadding(FMargin(8.f, 0.f));
		}
		if (UVerticalBoxSlot* V = Col->AddChildToVerticalBox(Row))
		{
			V->SetPadding(FMargin(40.f, 6.f));
			V->SetHorizontalAlignment(HAlign_Fill);
		}
	}

	UTextBlock* Hint = WidgetTree->ConstructWidget<UTextBlock>();
	Hint->SetText(FText::FromString(TEXT("Click a row to cycle · saved instantly · team paint color is fixed")));
	Hint->SetColorAndOpacity(FSlateColor(FLinearColor(0.5f, 0.52f, 0.58f)));
	Hint->SetJustification(ETextJustify::Center);
	if (UVerticalBoxSlot* V = Col->AddChildToVerticalBox(Hint))
	{
		V->SetPadding(FMargin(0.f, 16.f, 0.f, 0.f));
		V->SetHorizontalAlignment(HAlign_Center);
	}

	// Seed working values from saved prefs + paint the labels.
	WorkingMarkerPreset = FPFUserPrefs::GetMarkerPreset();
	WorkingCrosshairStyle = FPFUserPrefs::GetCrosshairStyle();
	RefreshLoadoutLabels();
}

void UPFLoadingMenuWidget::BuildCharacterPage(UVerticalBox* Col)
{
	UTextBlock* Sub = WidgetTree->ConstructWidget<UTextBlock>();
	Sub->SetText(FText::FromString(TEXT("Customize your character — saved to this PC, applied when you spawn.")));
	Sub->SetColorAndOpacity(FSlateColor(FLinearColor(1.f, 1.f, 1.f, 0.65f)));
	Sub->SetJustification(ETextJustify::Center);
	if (UVerticalBoxSlot* V = Col->AddChildToVerticalBox(Sub))
	{
		V->SetPadding(FMargin(0.f, 4.f, 0.f, 12.f));
		V->SetHorizontalAlignment(HAlign_Center);
	}

	CharConfig = PFChar::LoadConfig();
	CharSlotValueTexts.Reset();

	// Left: live rotating 3D preview (render target bound in EnsureCharPreview). Right: per-slot steppers.
	UHorizontalBox* Body = WidgetTree->ConstructWidget<UHorizontalBox>();

	CharPreviewImage = WidgetTree->ConstructWidget<UImage>();
	CharPreviewImage->SetColorAndOpacity(FLinearColor::White);
	USizeBox* PreviewSizer = WidgetTree->ConstructWidget<USizeBox>();
	PreviewSizer->SetWidthOverride(300.f);
	PreviewSizer->SetHeightOverride(400.f);
	PreviewSizer->SetContent(CharPreviewImage);
	if (UHorizontalBoxSlot* H = Body->AddChildToHorizontalBox(PreviewSizer))
	{
		H->SetPadding(FMargin(8.f, 0.f, 18.f, 0.f));
		H->SetVerticalAlignment(VAlign_Center);
	}

	UVerticalBox* Rows = WidgetTree->ConstructWidget<UVerticalBox>();
	if (UHorizontalBoxSlot* H = Body->AddChildToHorizontalBox(Rows))
	{
		H->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		H->SetVerticalAlignment(VAlign_Center);
	}

	for (int32 s = 0; s < PFChar::SlotCount(); ++s)
	{
		UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>();

		UTextBlock* Lab = WidgetTree->ConstructWidget<UTextBlock>();
		Lab->SetText(FText::FromString(PFChar::SlotLabel(s).ToUpper()));
		Lab->SetFont(PFLoadFont(13, true));
		Lab->SetColorAndOpacity(FSlateColor(FLinearColor(0.55f, 0.72f, 0.95f)));
		USizeBox* LabSizer = WidgetTree->ConstructWidget<USizeBox>();
		LabSizer->SetWidthOverride(110.f);
		LabSizer->SetContent(Lab);
		Row->AddChildToHorizontalBox(LabSizer);

		UPFCharSlotButton* Prev = WidgetTree->ConstructWidget<UPFCharSlotButton>(UPFCharSlotButton::StaticClass());
		Prev->SetBackgroundColor(FLinearColor(0.12f, 0.13f, 0.16f, 1.f));
		Prev->InitStep(this, s, -1);
		UTextBlock* PrevLab = WidgetTree->ConstructWidget<UTextBlock>();
		PrevLab->SetText(FText::FromString(TEXT(" < ")));
		PrevLab->SetFont(PFLoadFont(14, true));
		PrevLab->SetColorAndOpacity(FSlateColor(FLinearColor::White));
		Prev->SetContent(PrevLab);
		Row->AddChildToHorizontalBox(Prev);

		UTextBlock* Val = WidgetTree->ConstructWidget<UTextBlock>();
		Val->SetFont(PFLoadFont(12, false));
		Val->SetColorAndOpacity(FSlateColor(FLinearColor::White));
		Val->SetJustification(ETextJustify::Center);
		Val->SetClipping(EWidgetClipping::ClipToBounds);
		if (UHorizontalBoxSlot* H = Row->AddChildToHorizontalBox(Val))
		{
			H->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
			H->SetPadding(FMargin(6.f, 0.f));
			H->SetVerticalAlignment(VAlign_Center);
		}
		CharSlotValueTexts.Add(Val);

		UPFCharSlotButton* Next = WidgetTree->ConstructWidget<UPFCharSlotButton>(UPFCharSlotButton::StaticClass());
		Next->SetBackgroundColor(FLinearColor(0.12f, 0.13f, 0.16f, 1.f));
		Next->InitStep(this, s, +1);
		UTextBlock* NextLab = WidgetTree->ConstructWidget<UTextBlock>();
		NextLab->SetText(FText::FromString(TEXT(" > ")));
		NextLab->SetFont(PFLoadFont(14, true));
		NextLab->SetColorAndOpacity(FSlateColor(FLinearColor::White));
		Next->SetContent(NextLab);
		Row->AddChildToHorizontalBox(Next);

		if (UVerticalBoxSlot* V = Rows->AddChildToVerticalBox(Row))
		{
			V->SetPadding(FMargin(24.f, 3.f));
			V->SetHorizontalAlignment(HAlign_Fill);
		}
	}

	if (UVerticalBoxSlot* V = Col->AddChildToVerticalBox(Body))
	{
		V->SetHorizontalAlignment(HAlign_Center);
		V->SetPadding(FMargin(0.f, 6.f));
	}

	UTextBlock* Hint = WidgetTree->ConstructWidget<UTextBlock>();
	Hint->SetText(FText::FromString(TEXT("< / > to change each piece · saved instantly · team color is separate")));
	Hint->SetColorAndOpacity(FSlateColor(FLinearColor(0.5f, 0.52f, 0.58f)));
	Hint->SetJustification(ETextJustify::Center);
	if (UVerticalBoxSlot* V = Col->AddChildToVerticalBox(Hint))
	{
		V->SetPadding(FMargin(0.f, 12.f, 0.f, 0.f));
		V->SetHorizontalAlignment(HAlign_Center);
	}

	RefreshCharacterLabels();
}

void UPFLoadingMenuWidget::RefreshCharacterLabels()
{
	if (CharConfig.Slots.Num() != PFChar::SlotCount())
	{
		CharConfig = PFChar::LoadConfig();
	}
	for (int32 s = 0; s < CharSlotValueTexts.Num() && s < PFChar::SlotCount(); ++s)
	{
		if (CharSlotValueTexts[s] == nullptr)
		{
			continue;
		}
		const int32 Sel = CharConfig.Slots.IsValidIndex(s) ? CharConfig.Slots[s] : -1;
		const int32 N = PFChar::SlotParts(s).Num();
		const FString Text = (Sel < 0 || N == 0)
			? FString::Printf(TEXT("None  (0/%d)"), N)
			: FString::Printf(TEXT("%s  (%d/%d)"), *PFChar::PartDisplayName(s, Sel), Sel + 1, N);
		CharSlotValueTexts[s]->SetText(FText::FromString(Text));
	}
}

void UPFLoadingMenuWidget::EnsureCharPreview()
{
	if (CharPreviewActor != nullptr)
	{
		return;
	}
	UWorld* World = GetWorld();
	if (World == nullptr)
	{
		return;
	}
	// Spawn the studio far from the arena so its lights never leak into the level.
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	CharPreviewActor = World->SpawnActor<APFCharacterPreviewActor>(
		APFCharacterPreviewActor::StaticClass(),
		FVector(50000.f, 50000.f, 5000.f), FRotator::ZeroRotator, Params);
	if (CharPreviewActor == nullptr)
	{
		return;
	}
	CharPreviewActor->ApplyConfig(CharConfig);
	if (CharPreviewImage != nullptr && CharPreviewActor->GetRenderTarget() != nullptr)
	{
		FSlateBrush Brush;
		Brush.SetResourceObject(CharPreviewActor->GetRenderTarget());
		Brush.ImageSize = FVector2D(300.f, 400.f);
		Brush.DrawAs = ESlateBrushDrawType::Image;
		CharPreviewImage->SetBrush(Brush);
	}
}

void UPFLoadingMenuWidget::NotifyCharSlotStep(int32 SlotIdx, int32 Dir)
{
	if (SlotIdx < 0 || SlotIdx >= PFChar::SlotCount())
	{
		return;
	}
	if (CharConfig.Slots.Num() != PFChar::SlotCount())
	{
		CharConfig = PFChar::LoadConfig();
	}
	const int32 N = PFChar::SlotParts(SlotIdx).Num();
	if (N <= 0)
	{
		return;
	}
	int32 Cur = CharConfig.Slots.IsValidIndex(SlotIdx) ? CharConfig.Slots[SlotIdx] : -1;
	Cur += Dir;
	if (Cur < -1)      { Cur = N - 1; }   // wrap below None -> last part
	else if (Cur >= N) { Cur = -1; }      // wrap past last -> None
	CharConfig.Slots[SlotIdx] = Cur;
	PFChar::SaveConfig(CharConfig);
	// Live-update the already-spawned local pawn (it mounted before this edit, so it won't re-load on its own).
	if (APaintForgeCharacter* Char = Cast<APaintForgeCharacter>(GetOwningPlayerPawn()))
	{
		Char->ReapplyCharacterConfig();
	}
	// Live-update the tab's 3D preview to match.
	if (CharPreviewActor != nullptr)
	{
		CharPreviewActor->ApplyConfig(CharConfig);
	}
	RefreshCharacterLabels();
}

void UPFCharSlotButton::InitStep(UPFLoadingMenuWidget* InOwner, int32 InSlot, int32 InDir)
{
	OwnerWidget = InOwner;
	SlotIndex = InSlot;
	Dir = InDir;
	OnClicked.AddUniqueDynamic(this, &UPFCharSlotButton::HandleClicked);
}

void UPFCharSlotButton::HandleClicked()
{
	if (OwnerWidget.IsValid())
	{
		OwnerWidget->NotifyCharSlotStep(SlotIndex, Dir);
	}
}

void UPFLoadingMenuWidget::BuildTree()
{
	UCanvasPanel* Root = WidgetTree->ConstructWidget<UCanvasPanel>();
	WidgetTree->RootWidget = Root;

	// Full opaque backdrop — covers the live world completely (not a transparent HUD).
	Backdrop = WidgetTree->ConstructWidget<UImage>();
	Backdrop->SetBrush(FSlateColorBrush(FLinearColor::White));
	Backdrop->SetColorAndOpacity(FLinearColor(0.04f, 0.05f, 0.07f, 1.f));
	if (UCanvasPanelSlot* S = Root->AddChildToCanvas(Backdrop))
	{
		S->SetAnchors(FAnchors(0.f, 0.f, 1.f, 1.f));
		S->SetOffsets(FMargin(0.f));
		S->SetZOrder(0);
	}

	UVerticalBox* Col = WidgetTree->ConstructWidget<UVerticalBox>();

	TitleText = WidgetTree->ConstructWidget<UTextBlock>();
	TitleText->SetText(FText::FromString(TEXT("PAINTFORGE")));
	TitleText->SetFont(PFLoadFont(48, true));
	TitleText->SetColorAndOpacity(FSlateColor(FLinearColor(1.f, 0.92f, 0.35f)));
	TitleText->SetJustification(ETextJustify::Center);
	if (UVerticalBoxSlot* V = Col->AddChildToVerticalBox(TitleText))
	{
		V->SetHorizontalAlignment(HAlign_Center);
		V->SetPadding(FMargin(0.f, 0.f, 0.f, 8.f));
	}

	SubtitleText = WidgetTree->ConstructWidget<UTextBlock>();
	SubtitleText->SetText(FText::FromString(TEXT("Airsoft arena · multiplayer")));
	SubtitleText->SetFont(PFLoadFont(16, false));
	SubtitleText->SetColorAndOpacity(FSlateColor(FLinearColor(0.75f, 0.78f, 0.85f)));
	SubtitleText->SetJustification(ETextJustify::Center);
	if (UVerticalBoxSlot* V = Col->AddChildToVerticalBox(SubtitleText))
	{
		V->SetHorizontalAlignment(HAlign_Center);
		V->SetPadding(FMargin(0.f, 0.f, 0.f, 16.f));
	}

	// First-session one-click preset (host): Play-Only Skirmish 4v4 with bots + community map.
	QuickStartButton = WidgetTree->ConstructWidget<UButton>(UButton::StaticClass(), TEXT("QuickStartBtn"));
	QuickStartButton->SetBackgroundColor(FLinearColor(1.f, 0.85f, 0.2f, 0.95f));
	QuickStartButton->OnClicked.AddDynamic(this, &UPFLoadingMenuWidget::OnQuickStartClicked);
	QuickStartLabel = WidgetTree->ConstructWidget<UTextBlock>();
	QuickStartLabel->SetText(FText::FromString(TEXT("  QUICK START — first game  ")));
	QuickStartLabel->SetFont(PFLoadFont(16, true));
	QuickStartLabel->SetColorAndOpacity(FSlateColor(FLinearColor(0.12f, 0.12f, 0.14f)));
	QuickStartLabel->SetJustification(ETextJustify::Center);
	QuickStartButton->AddChild(QuickStartLabel);
	if (UVerticalBoxSlot* V = Col->AddChildToVerticalBox(QuickStartButton))
	{
		V->SetHorizontalAlignment(HAlign_Center);
		V->SetPadding(FMargin(0.f, 0.f, 0.f, 8.f));
	}
	UTextBlock* QuickHint = WidgetTree->ConstructWidget<UTextBlock>();
	QuickHint->SetText(FText::FromString(TEXT("Play-Only · Skirmish · 4v4 · bots · community map")));
	QuickHint->SetFont(PFLoadFont(12, false));
	QuickHint->SetColorAndOpacity(FSlateColor(FLinearColor(0.6f, 0.62f, 0.68f)));
	QuickHint->SetJustification(ETextJustify::Center);
	if (UVerticalBoxSlot* V = Col->AddChildToVerticalBox(QuickHint))
	{
		V->SetHorizontalAlignment(HAlign_Center);
		V->SetPadding(FMargin(0.f, 0.f, 0.f, 20.f));
	}

	// ---- Menu tabs: Match Setup | How to Play ----
	UHorizontalBox* MenuTabs = WidgetTree->ConstructWidget<UHorizontalBox>();
	TabSetup = MakeMenuTab(TEXT("  MATCH SETUP  "), TEXT("TabSetup"));
	TabHowTo = MakeMenuTab(TEXT("  HOW TO PLAY  "), TEXT("TabHowTo"));
	TabLoadout = MakeMenuTab(TEXT("  LOADOUT  "), TEXT("TabLoadout"));
	TabCharacter = MakeMenuTab(TEXT("  CHARACTER  "), TEXT("TabCharacter"));
	TabSetup->OnClicked.AddDynamic(this, &UPFLoadingMenuWidget::OnTabSetup);
	TabHowTo->OnClicked.AddDynamic(this, &UPFLoadingMenuWidget::OnTabHowTo);
	TabLoadout->OnClicked.AddDynamic(this, &UPFLoadingMenuWidget::OnTabLoadout);
	TabCharacter->OnClicked.AddDynamic(this, &UPFLoadingMenuWidget::OnTabCharacter);
	if (UHorizontalBoxSlot* H = MenuTabs->AddChildToHorizontalBox(TabSetup))
	{
		H->SetPadding(FMargin(4.f, 0.f));
	}
	if (UHorizontalBoxSlot* H = MenuTabs->AddChildToHorizontalBox(TabHowTo))
	{
		H->SetPadding(FMargin(4.f, 0.f));
	}
	if (UHorizontalBoxSlot* H = MenuTabs->AddChildToHorizontalBox(TabLoadout))
	{
		H->SetPadding(FMargin(4.f, 0.f));
	}
	if (UHorizontalBoxSlot* H = MenuTabs->AddChildToHorizontalBox(TabCharacter))
	{
		H->SetPadding(FMargin(4.f, 0.f));
	}
	if (UVerticalBoxSlot* V = Col->AddChildToVerticalBox(MenuTabs))
	{
		V->SetHorizontalAlignment(HAlign_Center);
		V->SetPadding(FMargin(0.f, 0.f, 0.f, 14.f));
	}

	USizeBox* MenuSizer = WidgetTree->ConstructWidget<USizeBox>();
	MenuSizer->SetWidthOverride(520.f);
	MenuSwitcher = WidgetTree->ConstructWidget<UWidgetSwitcher>();
	MenuSizer->SetContent(MenuSwitcher);

	// Page 0 — match setup
	UVerticalBox* SetupCol = WidgetTree->ConstructWidget<UVerticalBox>();

	ModeButton = MakeSetupButton(TEXT("MODE"), ModeValueText, TEXT("ModeBtn"));
	ModeButton->OnClicked.AddDynamic(this, &UPFLoadingMenuWidget::OnModeClicked);
	if (UVerticalBoxSlot* V = SetupCol->AddChildToVerticalBox(ModeButton))
	{
		V->SetPadding(FMargin(0.f, 2.f));
		V->SetHorizontalAlignment(HAlign_Fill);
	}
	ModeBlurbText = WidgetTree->ConstructWidget<UTextBlock>();
	ModeBlurbText->SetFont(PFLoadFont(12, false));
	ModeBlurbText->SetColorAndOpacity(FSlateColor(FLinearColor(0.55f, 0.58f, 0.65f)));
	ModeBlurbText->SetJustification(ETextJustify::Left);
	if (UVerticalBoxSlot* V = SetupCol->AddChildToVerticalBox(ModeBlurbText))
	{
		V->SetPadding(FMargin(4.f, 2.f, 4.f, 10.f));
	}

	TypeButton = MakeSetupButton(TEXT("TYPE"), TypeValueText, TEXT("TypeBtn"));
	TypeButton->OnClicked.AddDynamic(this, &UPFLoadingMenuWidget::OnTypeClicked);
	if (UVerticalBoxSlot* V = SetupCol->AddChildToVerticalBox(TypeButton))
	{
		V->SetPadding(FMargin(0.f, 2.f));
		V->SetHorizontalAlignment(HAlign_Fill);
	}
	TypeBlurbText = WidgetTree->ConstructWidget<UTextBlock>();
	TypeBlurbText->SetFont(PFLoadFont(12, false));
	TypeBlurbText->SetColorAndOpacity(FSlateColor(FLinearColor(0.55f, 0.58f, 0.65f)));
	TypeBlurbText->SetJustification(ETextJustify::Left);
	if (UVerticalBoxSlot* V = SetupCol->AddChildToVerticalBox(TypeBlurbText))
	{
		V->SetPadding(FMargin(4.f, 2.f, 4.f, 10.f));
	}

	FormatButton = MakeSetupButton(TEXT("FORMAT"), FormatValueText, TEXT("FormatBtn"));
	FormatButton->OnClicked.AddDynamic(this, &UPFLoadingMenuWidget::OnFormatClicked);
	if (UVerticalBoxSlot* V = SetupCol->AddChildToVerticalBox(FormatButton))
	{
		V->SetPadding(FMargin(0.f, 2.f));
		V->SetHorizontalAlignment(HAlign_Fill);
	}
	FormatBlurbText = WidgetTree->ConstructWidget<UTextBlock>();
	FormatBlurbText->SetFont(PFLoadFont(12, false));
	FormatBlurbText->SetColorAndOpacity(FSlateColor(FLinearColor(0.55f, 0.58f, 0.65f)));
	FormatBlurbText->SetJustification(ETextJustify::Left);
	if (UVerticalBoxSlot* V = SetupCol->AddChildToVerticalBox(FormatBlurbText))
	{
		V->SetPadding(FMargin(4.f, 2.f, 4.f, 10.f));
	}

	// Bots checkbox row
	UHorizontalBox* BotsRow = WidgetTree->ConstructWidget<UHorizontalBox>();
	BotsCheck = WidgetTree->ConstructWidget<UCheckBox>();
	BotsCheck->SetIsChecked(true);
	BotsCheck->OnCheckStateChanged.AddDynamic(this, &UPFLoadingMenuWidget::OnBotsChanged);
	if (UHorizontalBoxSlot* H = BotsRow->AddChildToHorizontalBox(BotsCheck))
	{
		H->SetVerticalAlignment(VAlign_Center);
		H->SetPadding(FMargin(8.f, 6.f, 10.f, 6.f));
	}
	BotsLabelText = WidgetTree->ConstructWidget<UTextBlock>();
	BotsLabelText->SetText(FText::FromString(TEXT("Fill empty slots with bots")));
	BotsLabelText->SetFont(PFLoadFont(15, false));
	BotsLabelText->SetColorAndOpacity(FSlateColor(FLinearColor(0.9f, 0.91f, 0.94f)));
	if (UHorizontalBoxSlot* H = BotsRow->AddChildToHorizontalBox(BotsLabelText))
	{
		H->SetVerticalAlignment(VAlign_Center);
		H->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
	}
	if (UVerticalBoxSlot* V = SetupCol->AddChildToVerticalBox(BotsRow))
	{
		V->SetPadding(FMargin(0.f, 4.f, 0.f, 2.f));
		V->SetHorizontalAlignment(HAlign_Fill);
	}

	BuildMapPicker(SetupCol);

	SetupHintText = WidgetTree->ConstructWidget<UTextBlock>();
	SetupHintText->SetFont(PFLoadFont(12, false));
	SetupHintText->SetColorAndOpacity(FSlateColor(FLinearColor(0.5f, 0.52f, 0.58f)));
	SetupHintText->SetJustification(ETextJustify::Center);
	if (UVerticalBoxSlot* V = SetupCol->AddChildToVerticalBox(SetupHintText))
	{
		V->SetPadding(FMargin(0.f, 8.f, 0.f, 0.f));
		V->SetHorizontalAlignment(HAlign_Center);
	}

	// Page 1 — how to play
	UVerticalBox* HowToCol = WidgetTree->ConstructWidget<UVerticalBox>();
	BuildHowToPlayPage(HowToCol);

	UVerticalBox* LoadoutCol = WidgetTree->ConstructWidget<UVerticalBox>();
	BuildLoadoutPage(LoadoutCol);

	UVerticalBox* CharacterCol = WidgetTree->ConstructWidget<UVerticalBox>();
	BuildCharacterPage(CharacterCol);

	MenuSwitcher->AddChild(SetupCol);
	MenuSwitcher->AddChild(HowToCol);
	MenuSwitcher->AddChild(LoadoutCol);      // index 2
	MenuSwitcher->AddChild(CharacterCol);    // index 3

	if (UVerticalBoxSlot* V = Col->AddChildToVerticalBox(MenuSizer))
	{
		V->SetHorizontalAlignment(HAlign_Center);
		V->SetPadding(FMargin(0.f, 0.f, 0.f, 28.f));
	}

	// ---- Warmup status ----
	StatusText = WidgetTree->ConstructWidget<UTextBlock>();
	StatusText->SetText(FText::FromString(TEXT("Starting…")));
	StatusText->SetFont(PFLoadFont(15, false));
	StatusText->SetColorAndOpacity(FSlateColor(FLinearColor(0.9f, 0.9f, 0.92f)));
	StatusText->SetJustification(ETextJustify::Center);
	if (UVerticalBoxSlot* V = Col->AddChildToVerticalBox(StatusText))
	{
		V->SetHorizontalAlignment(HAlign_Center);
		V->SetPadding(FMargin(0.f, 0.f, 0.f, 12.f));
	}

	ProgressBar = WidgetTree->ConstructWidget<UProgressBar>();
	ProgressBar->SetPercent(0.f);
	ProgressBar->SetFillColorAndOpacity(FLinearColor(1.f, 0.85f, 0.25f));
	USizeBox* ProgressSizer = WidgetTree->ConstructWidget<USizeBox>();
	ProgressSizer->SetWidthOverride(420.f);
	ProgressSizer->SetHeightOverride(14.f);
	ProgressSizer->SetContent(ProgressBar);
	if (UVerticalBoxSlot* V = Col->AddChildToVerticalBox(ProgressSizer))
	{
		V->SetHorizontalAlignment(HAlign_Center);
		V->SetPadding(FMargin(0.f, 0.f, 0.f, 28.f));
	}

	EnterButton = WidgetTree->ConstructWidget<UButton>();
	EnterButton->SetIsEnabled(false);
	EnterButton->OnClicked.AddDynamic(this, &UPFLoadingMenuWidget::OnEnterClicked);
	EnterLabel = WidgetTree->ConstructWidget<UTextBlock>();
	EnterLabel->SetText(FText::FromString(TEXT("ENTER LOBBY")));
	EnterLabel->SetFont(PFLoadFont(18, true));
	EnterLabel->SetColorAndOpacity(FSlateColor(FLinearColor(0.15f, 0.15f, 0.18f)));
	EnterLabel->SetJustification(ETextJustify::Center);
	EnterButton->AddChild(EnterLabel);
	if (UVerticalBoxSlot* V = Col->AddChildToVerticalBox(EnterButton))
	{
		V->SetHorizontalAlignment(HAlign_Center);
		V->SetPadding(FMargin(0.f, 8.f, 0.f, 0.f));
	}

	QuitDesktopButton = WidgetTree->ConstructWidget<UButton>();
	QuitDesktopButton->SetBackgroundColor(FLinearColor(0.25f, 0.08f, 0.08f, 0.95f));
	QuitDesktopButton->OnClicked.AddDynamic(this, &UPFLoadingMenuWidget::OnQuitDesktopClicked);
	QuitDesktopLabel = WidgetTree->ConstructWidget<UTextBlock>();
	QuitDesktopLabel->SetText(FText::FromString(TEXT("QUIT TO DESKTOP")));
	QuitDesktopLabel->SetFont(PFLoadFont(14, true));
	QuitDesktopLabel->SetColorAndOpacity(FSlateColor(FLinearColor(1.f, 0.85f, 0.85f)));
	QuitDesktopLabel->SetJustification(ETextJustify::Center);
	QuitDesktopButton->AddChild(QuitDesktopLabel);
	if (UVerticalBoxSlot* V = Col->AddChildToVerticalBox(QuitDesktopButton))
	{
		V->SetHorizontalAlignment(HAlign_Center);
		V->SetPadding(FMargin(0.f, 14.f, 0.f, 0.f));
	}

	if (UCanvasPanelSlot* S = Root->AddChildToCanvas(Col))
	{
		S->SetAnchors(FAnchors(0.5f, 0.5f));
		S->SetAlignment(FVector2D(0.5f, 0.5f));
		S->SetAutoSize(true);
		S->SetZOrder(1);
	}
}

void UPFLoadingMenuWidget::NativeConstruct()
{
	Super::NativeConstruct();

	// Preload list: combat/build art hit early in a match. Soft — missing packs are fine.
	PreloadPaths = {
		TEXT("/Game/Materials/M_PF_ArenaFloor.M_PF_ArenaFloor"),
		TEXT("/Game/Materials/M_PF_ArenaWall.M_PF_ArenaWall"),
		TEXT("/Game/Materials/M_PF_BuildPiece.M_PF_BuildPiece"),
		TEXT("/Game/Materials/M_PF_TeamBody.M_PF_TeamBody"),
		TEXT("/Game/Weapons/Rifle/Mesh/SM_Rifle.SM_Rifle"),
		TEXT("/Game/Weapons/Rifle/M_PF_Rifle.M_PF_Rifle"),
		TEXT("/Game/QuantumCharacter/Mesh/SKM_QuantumCharacter.SKM_QuantumCharacter"),
		TEXT("/Game/Survival_Character/Meshes/SK_Survival_Character.SK_Survival_Character"),
		TEXT("/Game/Characters/Mannequins/Meshes/SKM_Manny_Simple.SKM_Manny_Simple"),
		TEXT("/Game/Characters/Mannequins/Anims/Unarmed/ABP_Unarmed.ABP_Unarmed_C"),
		TEXT("/Game/Materials/M_PF_Flash.M_PF_Flash"),
		TEXT("/Game/Materials/M_PF_ImpactMark.M_PF_ImpactMark"),
		// Build props (warehouse) — soft, so first place doesn't hitch on mesh load.
		TEXT("/Game/Scene_Warehouse/Assets/MS/3D/Ind_Aba_Storage_Barrel_Metal_Blue_01/SM_Ind_Aba_Storage_Barrel_Metal_Blue_01.SM_Ind_Aba_Storage_Barrel_Metal_Blue_01"),
		TEXT("/Game/Scene_Warehouse/Assets/MS/3D/Ind_War_Storage_Crate_Plastic_Blue_01/SM_Ind_War_Storage_Crate_Plastic_Blue_01.SM_Ind_War_Storage_Crate_Plastic_Blue_01"),
		TEXT("/Game/Scene_Warehouse/Assets/MS/3D/Ind_War_Storage_Box_Cardboard_Set_01/SM_Ind_War_Storage_Box_Cardboard_Set_01_A.SM_Ind_War_Storage_Box_Cardboard_Set_01_A"),
	};
	PreloadIndex = 0;
	WarmupStep = 0;
	Progress = 0.f;
	bWarmupComplete = false;
	bDismissed = false;

	// UI-only while this menu is up — don't let look/move leak into the pen behind the curtain.
	if (APlayerController* PC = GetOwningPlayer())
	{
		FInputModeUIOnly Mode;
		Mode.SetWidgetToFocus(TakeWidget());
		Mode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
		PC->SetInputMode(Mode);
		PC->bShowMouseCursor = true;
	}

	SeedFromGameState();
	ReloadMapCatalog();
	// Host first paint: default UI toward a friendly first session if still Creative default.
	if (IsLocalHost() && SelectedBuildMode == EPFBuildMode::Creative
		&& SelectedMatchType == EPFMatchType::Skirmish)
	{
		// Soft-default (not force): leave Creative if host already changed via GS.
		// Only nudge status; Quick Start remains the explicit first-game path.
	}
	RefreshSetupLabels();
	RefreshMapPicker();
	SelectMenuTab(0);
	if (QuickStartButton)
	{
		QuickStartButton->SetIsEnabled(IsLocalHost());
	}
	SetStatus(TEXT("Preparing… · first time? try QUICK START"));
	UE_LOG(PaintForgeLog, Log, TEXT("LoadingMenu: boot menu up — warmup starting"));
}

void UPFLoadingMenuWidget::NativeDestruct()
{
	// Tear down the off-screen preview studio with the menu.
	if (CharPreviewActor != nullptr)
	{
		CharPreviewActor->Destroy();
		CharPreviewActor = nullptr;
	}
	Super::NativeDestruct();
}

void UPFLoadingMenuWidget::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);
	if (bDismissed)
	{
		return;
	}

	// Keep labels honest if GS replicates in late (client join).
	if (!bWarmupComplete)
	{
		RunWarmupStep();
	}
	// Host can still change while waiting on Enter; clients just mirror GS.
	if (!IsLocalHost())
	{
		SeedFromGameState();
		RefreshSetupLabels();
		RefreshMapPicker();
	}
}

void UPFLoadingMenuWidget::SetStatus(const FString& Line)
{
	if (StatusText)
	{
		StatusText->SetText(FText::FromString(Line));
	}
}

bool UPFLoadingMenuWidget::IsLocalHost() const
{
	const APlayerController* PC = GetOwningPlayer();
	return PC && PC->HasAuthority() && PC->IsLocalPlayerController();
}

void UPFLoadingMenuWidget::SeedFromGameState()
{
	const UWorld* World = GetWorld();
	const APaintForgeGameState* GS = World ? World->GetGameState<APaintForgeGameState>() : nullptr;
	if (GS)
	{
		SelectedBuildMode = GS->BuildMode;
		SelectedMatchType = GS->MatchType;
		SelectedTeamSize = (GS->TargetTeamSize >= 6) ? 6 : 4;
		bSelectedFillBots = GS->bFillWithBots;
		// Mirror host map pick when we have a catalog match.
		if (!GS->SelectedCommunityMapFile.IsEmpty())
		{
			SelectedMapCatalogIndex = INDEX_NONE;
			for (int32 i = 0; i < MapCatalog.Num(); ++i)
			{
				if (MapCatalog[i].FileName == GS->SelectedCommunityMapFile)
				{
					SelectedMapCatalogIndex = i;
					break;
				}
			}
		}
		else if (!IsLocalHost())
		{
			SelectedMapCatalogIndex = INDEX_NONE;
		}
	}
}

bool UPFLoadingMenuWidget::NeedsCommunityMap() const
{
	return SelectedBuildMode == EPFBuildMode::Improvement
		|| SelectedBuildMode == EPFBuildMode::PlayOnly;
}

void UPFLoadingMenuWidget::ReloadMapCatalog()
{
	MapCatalog.Reset();
	if (const UGameInstance* GI = GetGameInstance())
	{
		if (UPFRatingSubsystem* Rating = GI->GetSubsystem<UPFRatingSubsystem>())
		{
			Rating->EnsureSeedArenas();
			Rating->ListTopCommunityMaps(MapCatalog, MaxMaps);
		}
	}
	const int32 PageCount = FMath::Max(1, FMath::DivideAndRoundUp(FMath::Max(MapCatalog.Num(), 1), MapsPerPage));
	MapPageIndex = FMath::Clamp(MapPageIndex, 0, PageCount - 1);
	if (SelectedMapCatalogIndex != INDEX_NONE && !MapCatalog.IsValidIndex(SelectedMapCatalogIndex))
	{
		SelectedMapCatalogIndex = INDEX_NONE;
	}
}

void UPFLoadingMenuWidget::RefreshMapPicker()
{
	const bool bShow = NeedsCommunityMap();
	if (MapPickerBox)
	{
		MapPickerBox->SetVisibility(bShow ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
	}
	if (!bShow)
	{
		return;
	}

	const int32 Total = MapCatalog.Num();
	const int32 PageCount = FMath::Max(1, FMath::DivideAndRoundUp(FMath::Max(Total, 1), MapsPerPage));
	MapPageIndex = FMath::Clamp(MapPageIndex, 0, PageCount - 1);
	const int32 Start = MapPageIndex * MapsPerPage;
	const int32 End = FMath::Min(Start + MapsPerPage, Total);

	if (MapPageLabel)
	{
		if (Total == 0)
		{
			MapPageLabel->SetText(FText::FromString(TEXT("No maps saved yet")));
		}
		else
		{
			MapPageLabel->SetText(FText::FromString(FString::Printf(
				TEXT("%d–%d of %d"), Start + 1, End, Total)));
		}
	}
	if (MapSelectedLabel)
	{
		if (SelectedMapCatalogIndex == INDEX_NONE)
		{
			MapSelectedLabel->SetText(FText::FromString(
				Total > 0 ? TEXT("Selected: Auto (highest ranked)")
				          : TEXT("Selected: empty field (no Saved/Arenas)")));
		}
		else if (MapCatalog.IsValidIndex(SelectedMapCatalogIndex))
		{
			MapSelectedLabel->SetText(FText::FromString(FString::Printf(
				TEXT("Selected: #%d  %s"),
				SelectedMapCatalogIndex + 1, *MapCatalog[SelectedMapCatalogIndex].DisplayName)));
		}
	}

	const bool bHost = IsLocalHost();
	for (int32 SlotIdx = 0; SlotIdx < MapsPerPage; ++SlotIdx)
	{
		const int32 CatalogIdx = Start + SlotIdx;
		const bool bValid = MapCatalog.IsValidIndex(CatalogIdx);
		if (MapSlotButtons.IsValidIndex(SlotIdx) && MapSlotButtons[SlotIdx])
		{
			MapSlotButtons[SlotIdx]->SetVisibility(bValid ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
			MapSlotButtons[SlotIdx]->SetIsEnabled(bHost && bValid);
			const bool bSelected = bValid && CatalogIdx == SelectedMapCatalogIndex;
			MapSlotButtons[SlotIdx]->SetBackgroundColor(bSelected
				? FLinearColor(0.25f, 0.22f, 0.08f, 0.98f)
				: FLinearColor(0.10f, 0.11f, 0.14f, 0.95f));
		}
		if (bValid)
		{
			const FPFCommunityMapInfo& M = MapCatalog[CatalogIdx];
			if (MapSlotLabels.IsValidIndex(SlotIdx) && MapSlotLabels[SlotIdx])
			{
				MapSlotLabels[SlotIdx]->SetText(FText::FromString(FString::Printf(
					TEXT("  #%d  %s  (+%d / -%d)"),
					CatalogIdx + 1, *M.DisplayName, M.ThumbUp, M.ThumbDown)));
			}
			// Screenshot-on-publish preview (#6): show the map's PNG if it was captured, else a neutral tile.
			if (MapSlotImages.IsValidIndex(SlotIdx) && MapSlotImages[SlotIdx])
			{
				if (UTexture2D* Preview = GetMapPreview(M.FileName))
				{
					MapSlotImages[SlotIdx]->SetBrushFromTexture(Preview, /*bMatchSize=*/false);
				}
				else
				{
					MapSlotImages[SlotIdx]->SetBrush(FSlateColorBrush(FLinearColor(0.15f, 0.16f, 0.20f, 1.f)));
				}
			}
		}
	}
	if (MapPagePrevBtn) { MapPagePrevBtn->SetIsEnabled(bHost && MapPageIndex > 0); }
	if (MapPageNextBtn) { MapPageNextBtn->SetIsEnabled(bHost && MapPageIndex < PageCount - 1); }
	if (MapAutoBtn) { MapAutoBtn->SetIsEnabled(bHost); }
}

void UPFLoadingMenuWidget::RefreshSetupLabels()
{
	if (ModeValueText)
	{
		ModeValueText->SetText(FText::FromString(BuildModeLabel(SelectedBuildMode)));
	}
	if (ModeBlurbText)
	{
		ModeBlurbText->SetText(FText::FromString(BuildModeBlurb(SelectedBuildMode)));
	}
	if (TypeValueText)
	{
		TypeValueText->SetText(FText::FromString(MatchTypeLabel(SelectedMatchType)));
	}
	if (TypeBlurbText)
	{
		TypeBlurbText->SetText(FText::FromString(MatchTypeBlurb(SelectedMatchType)));
	}
	if (FormatValueText)
	{
		FormatValueText->SetText(FText::FromString(FormatLabel(SelectedTeamSize)));
	}
	if (FormatBlurbText)
	{
		FormatBlurbText->SetText(FText::FromString(FormatBlurb(SelectedTeamSize, bSelectedFillBots)));
	}
	if (BotsCheck)
	{
		BotsCheck->SetIsChecked(bSelectedFillBots);
		BotsCheck->SetIsEnabled(IsLocalHost());
	}
	if (BotsLabelText)
	{
		BotsLabelText->SetText(FText::FromString(
			bSelectedFillBots ? TEXT("Fill empty slots with bots") : TEXT("No bots — humans only")));
		BotsLabelText->SetColorAndOpacity(FSlateColor(
			IsLocalHost() ? FLinearColor(0.9f, 0.91f, 0.94f) : FLinearColor(0.55f, 0.55f, 0.6f)));
	}
	if (SetupHintText)
	{
		SetupHintText->SetText(FText::FromString(
			IsLocalHost()
				? (NeedsCommunityMap()
					? TEXT("Improvement/Play-Only: pick a community map below (10 per page)")
					: TEXT("Click MODE / TYPE / FORMAT to cycle · bots checkbox · applied live"))
				: TEXT("Host chooses match setup · waiting for Enter")));
	}
	// Non-host: still clickable visually but handlers no-op; dim slightly via background.
	const FLinearColor ActiveBg(0.12f, 0.13f, 0.16f, 0.95f);
	const FLinearColor DimBg(0.08f, 0.08f, 0.10f, 0.7f);
	const FLinearColor BtnBg = IsLocalHost() ? ActiveBg : DimBg;
	if (ModeButton)   { ModeButton->SetBackgroundColor(BtnBg); }
	if (TypeButton)   { TypeButton->SetBackgroundColor(BtnBg); }
	if (FormatButton) { FormatButton->SetBackgroundColor(BtnBg); }

	RefreshMapPicker();
}

void UPFLoadingMenuWidget::NotifyMapSlotClicked(int32 SlotIndex)
{
	if (!IsLocalHost() || bDismissed || !NeedsCommunityMap())
	{
		return;
	}
	const int32 CatalogIdx = MapPageIndex * MapsPerPage + SlotIndex;
	if (!MapCatalog.IsValidIndex(CatalogIdx))
	{
		return;
	}
	SelectedMapCatalogIndex = CatalogIdx;
	RefreshMapPicker();
	ApplyMapSelectionToHost();
}

void UPFLoadingMenuWidget::OnMapPagePrev()
{
	if (!IsLocalHost() || MapPageIndex <= 0) { return; }
	--MapPageIndex;
	RefreshMapPicker();
}

void UPFLoadingMenuWidget::OnMapPageNext()
{
	if (!IsLocalHost()) { return; }
	const int32 PageCount = FMath::Max(1, FMath::DivideAndRoundUp(FMath::Max(MapCatalog.Num(), 1), MapsPerPage));
	if (MapPageIndex >= PageCount - 1) { return; }
	++MapPageIndex;
	RefreshMapPicker();
}

void UPFLoadingMenuWidget::OnMapAutoClicked()
{
	if (!IsLocalHost() || bDismissed) { return; }
	SelectedMapCatalogIndex = INDEX_NONE;
	RefreshMapPicker();
	ApplyMapSelectionToHost();
}

void UPFLoadingMenuWidget::ApplyMapSelectionToHost()
{
	if (!IsLocalHost()) { return; }
	FString File;
	FString Label = TEXT("Auto (top ranked)");
	if (MapCatalog.IsValidIndex(SelectedMapCatalogIndex))
	{
		File = MapCatalog[SelectedMapCatalogIndex].FileName;
		Label = FString::Printf(TEXT("#%d %s"), SelectedMapCatalogIndex + 1,
			*MapCatalog[SelectedMapCatalogIndex].DisplayName);
	}
	if (APaintForgePlayerController* PC = Cast<APaintForgePlayerController>(GetOwningPlayer()))
	{
		PC->ServerHostSetCommunityMap(File, Label);
	}
}

void UPFLoadingMenuWidget::ApplyQuickStartPreset()
{
	// First-session default: straight into combat with bots on a community fort.
	SelectedBuildMode = EPFBuildMode::PlayOnly;
	SelectedMatchType = EPFMatchType::Skirmish;
	SelectedTeamSize = 4;
	bSelectedFillBots = true;
	SelectedMapCatalogIndex = INDEX_NONE;   // auto top-ranked (seeds always available after Ensure)
	ReloadMapCatalog();
	// Prefer named starter seed if present so first game is predictable.
	for (int32 i = 0; i < MapCatalog.Num(); ++i)
	{
		if (MapCatalog[i].FileName.Contains(TEXT("seed_starter")))
		{
			SelectedMapCatalogIndex = i;
			break;
		}
	}
	RefreshSetupLabels();
	ApplySelectionsToHost();
	ApplyMapSelectionToHost();
	SetStatus(TEXT("Quick Start ready — press ENTER LOBBY when warm-up finishes."));
	if (QuickStartLabel)
	{
		QuickStartLabel->SetText(FText::FromString(TEXT("  QUICK START — applied ✓  ")));
	}
	UE_LOG(PaintForgeLog, Log, TEXT("LoadingMenu: Quick Start preset applied"));
}

void UPFLoadingMenuWidget::OnQuickStartClicked()
{
	if (!IsLocalHost() || bDismissed)
	{
		return;
	}
	ApplyQuickStartPreset();
}

void UPFLoadingMenuWidget::OnModeClicked()
{
	if (!IsLocalHost() || bDismissed)
	{
		return;
	}
	SelectedBuildMode = static_cast<EPFBuildMode>(
		(static_cast<uint8>(SelectedBuildMode) + 1) % static_cast<uint8>(EPFBuildMode::MAX_Count));
	// FFA is play-only by nature — nudge build mode if needed is still host's call.
	RefreshSetupLabels();
	ApplySelectionsToHost();
	if (NeedsCommunityMap())
	{
		ApplyMapSelectionToHost();
	}
}

void UPFLoadingMenuWidget::OnTypeClicked()
{
	if (!IsLocalHost() || bDismissed)
	{
		return;
	}
	SelectedMatchType = static_cast<EPFMatchType>(
		(static_cast<uint8>(SelectedMatchType) + 1) % static_cast<uint8>(EPFMatchType::MAX_Count));
	// FreeForAll is play-only by design (GameMode enforces the same).
	if (SelectedMatchType == EPFMatchType::FreeForAll)
	{
		SelectedBuildMode = EPFBuildMode::PlayOnly;
	}
	RefreshSetupLabels();
	ApplySelectionsToHost();
}

void UPFLoadingMenuWidget::OnFormatClicked()
{
	if (!IsLocalHost() || bDismissed)
	{
		return;
	}
	SelectedTeamSize = (SelectedTeamSize >= 6) ? 4 : 6;
	RefreshSetupLabels();
	ApplySelectionsToHost();
}

void UPFLoadingMenuWidget::OnBotsChanged(bool bIsChecked)
{
	if (!IsLocalHost() || bDismissed)
	{
		// Revert UI if a non-host somehow toggled.
		if (BotsCheck)
		{
			BotsCheck->SetIsChecked(bSelectedFillBots);
		}
		return;
	}
	bSelectedFillBots = bIsChecked;
	RefreshSetupLabels();
	ApplySelectionsToHost();
}

void UPFLoadingMenuWidget::ApplySelectionsToHost()
{
	if (!IsLocalHost())
	{
		return;
	}
	if (APaintForgePlayerController* PC = Cast<APaintForgePlayerController>(GetOwningPlayer()))
	{
		PC->ServerHostSetBuildMode(static_cast<uint8>(SelectedBuildMode));
		PC->ServerHostSetMatchType(static_cast<uint8>(SelectedMatchType));
		PC->ServerHostSetFormat(SelectedTeamSize);
		PC->ServerHostSetFillWithBots(bSelectedFillBots);
	}
	if (NeedsCommunityMap())
	{
		ApplyMapSelectionToHost();
	}
}

void UPFLoadingMenuWidget::RunWarmupStep()
{
	// Spread work across frames so the progress bar animates and the process stays responsive.
	switch (WarmupStep)
	{
	case 0:
		SetStatus(TEXT("Loading materials & meshes…"));
		WarmupStep = 1;
		Progress = 0.05f;
		break;

	case 1:
	{
		// Load a few soft objects per tick.
		const int32 Batch = 2;
		int32 Loaded = 0;
		while (PreloadIndex < PreloadPaths.Num() && Loaded < Batch)
		{
			const FSoftObjectPath Path(PreloadPaths[PreloadIndex]);
			if (UObject* Obj = Path.TryLoad())
			{
				// Touch render proxies for materials so shaders queue.
				if (UMaterialInterface* Mat = Cast<UMaterialInterface>(Obj))
				{
					Mat->GetRenderProxy();
				}
			}
			++PreloadIndex;
			++Loaded;
		}
		const float Frac = PreloadPaths.Num() > 0
			? static_cast<float>(PreloadIndex) / static_cast<float>(PreloadPaths.Num())
			: 1.f;
		Progress = 0.05f + Frac * 0.45f;
		if (ProgressBar) { ProgressBar->SetPercent(Progress); }
		if (PreloadIndex >= PreloadPaths.Num())
		{
			WarmupStep = 2;
			SetStatus(TEXT("Compiling shaders…"));
		}
		break;
	}

	case 2:
	{
		// Drain the async shader compile queue (editor + packaged first runs).
		int32 RemainingJobs = 0;
		if (GShaderCompilingManager)
		{
			// Process a slice; FinishAllCompilation blocks the whole process if we call it
			// every frame with a huge queue — prefer ProcessAsyncResults then finish once thin.
			GShaderCompilingManager->ProcessAsyncResults(false, false);
			RemainingJobs = GShaderCompilingManager->GetNumRemainingJobs();
			if (RemainingJobs <= 2)
			{
				GShaderCompilingManager->FinishAllCompilation();
				RemainingJobs = GShaderCompilingManager->GetNumRemainingJobs();
			}
		}

		// PSO / pipeline cache precompiles (packaged builds).
		const uint32 PsoLeft = FShaderPipelineCache::NumPrecompilesRemaining();

		const float ShaderFrac = RemainingJobs > 0
			? FMath::Clamp(1.f - static_cast<float>(RemainingJobs) / 64.f, 0.f, 0.95f)
			: 1.f;
		Progress = 0.50f + ShaderFrac * 0.40f;
		if (ProgressBar) { ProgressBar->SetPercent(Progress); }

		if (RemainingJobs > 0 || PsoLeft > 0)
		{
			SetStatus(FString::Printf(TEXT("Compiling shaders… (%d jobs, %u PSO)"),
				RemainingJobs, PsoLeft));
			ShaderWaitAccum = 0.f;
		}
		else
		{
			// Hold a beat so the bar doesn't flash 100% and vanish.
			ShaderWaitAccum += GetWorld() ? GetWorld()->GetDeltaSeconds() : 0.016f;
			if (ShaderWaitAccum > 0.15f)
			{
				WarmupStep = 3;
			}
		}
		break;
	}

	case 3:
		FinishWarmup();
		break;

	default:
		break;
	}
}

void UPFLoadingMenuWidget::FinishWarmup()
{
	if (bWarmupComplete)
	{
		return;
	}
	bWarmupComplete = true;
	Progress = 1.f;
	if (ProgressBar) { ProgressBar->SetPercent(1.f); }
	SetStatus(TEXT("Ready."));
	if (EnterButton)
	{
		EnterButton->SetIsEnabled(true);
	}
	if (EnterLabel)
	{
		EnterLabel->SetText(FText::FromString(TEXT("ENTER LOBBY")));
	}
	// Ensure host selection is on the server before anyone enters.
	ApplySelectionsToHost();
	UE_LOG(PaintForgeLog, Log,
		TEXT("LoadingMenu: warmup complete — waiting for Enter (Mode=%d Type=%d Format=%dv%d Bots=%d)"),
		static_cast<int32>(SelectedBuildMode), static_cast<int32>(SelectedMatchType),
		SelectedTeamSize, SelectedTeamSize, bSelectedFillBots ? 1 : 0);
}

void UPFLoadingMenuWidget::OnEnterClicked()
{
	if (!bWarmupComplete || bDismissed)
	{
		return;
	}
	// Final push so late cycles stick.
	ApplySelectionsToHost();

	bDismissed = true;
	RemoveFromParent();

	// Re-apply lobby IMCs + GameAndUI now that the curtain is gone.
	if (APaintForgePlayerController* PC = Cast<APaintForgePlayerController>(GetOwningPlayer()))
	{
		PC->NotifyLoadingMenuFinished();
	}

	UE_LOG(PaintForgeLog, Log,
		TEXT("LoadingMenu: dismissed — entering lobby (Mode=%s Type=%s Format=%s Bots=%s)"),
		*BuildModeLabel(SelectedBuildMode), *MatchTypeLabel(SelectedMatchType),
		*FormatLabel(SelectedTeamSize), bSelectedFillBots ? TEXT("on") : TEXT("off"));
}

void UPFLoadingMenuWidget::OnQuitDesktopClicked()
{
	if (APaintForgePlayerController* PC = Cast<APaintForgePlayerController>(GetOwningPlayer()))
	{
		PC->QuitToDesktop();
		return;
	}
	// Fallback if no PC yet (very early boot).
	if (APlayerController* PC = GetOwningPlayer())
	{
		PC->ConsoleCommand(TEXT("quit"));
	}
}
