// Copyright (c) 2026 Tom Chapman. All rights reserved.

#include "UI/PFLoadingMenuWidget.h"

#include "CombatForge.h"
#include "Core/PFPaths.h"
#include "Core/CombatForgeGameState.h"
#include "Core/CombatForgePlayerController.h"
#include "Voting/PFRatingSubsystem.h"
#include "Engine/GameInstance.h"

#include "Blueprint/WidgetTree.h"
#include "Brushes/SlateColorBrush.h"
#include "Components/Button.h"
#include "Components/EditableTextBox.h"
#include "SocketSubsystem.h"
#include "IPAddress.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/CheckBox.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/Image.h"
#include "Components/ProgressBar.h"
#include "Components/ScrollBox.h"
#include "Components/SizeBox.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Components/WidgetSwitcher.h"
#include "Core/PFUserPrefs.h"
#include "Player/CombatForgeCharacter.h"
#include "Player/PFCharacterPreviewActor.h"
#include "UI/PFOptionsWidget.h"
#include "InputCoreTypes.h"
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
	// Primary LAN IP of this machine (what a friend types into JOIN). Falls back to a hint if unresolvable.
	FString GetLocalLanIp()
	{
		bool bCanBind = false;
		if (ISocketSubsystem* Sockets = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM))
		{
			TSharedRef<FInternetAddr> Addr = Sockets->GetLocalHostAddr(*GLog, bCanBind);
			if (Addr->IsValid())
			{
				return Addr->ToString(/*bAppendPort=*/false);
			}
		}
		return TEXT("<this PC's IP>");
	}
}


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
			return TEXT("Last team standing · first to 4 round wins");
		case EPFMatchType::FreeForAll:
			return TEXT("Solo · most tags · no teams · play-only");
		case EPFMatchType::Skirmish:
			return TEXT("Teams · most tags · everyone respawns");
		case EPFMatchType::CaptureFlag:
			return TEXT("Grab their flag · score at your base · first to 3");
		case EPFMatchType::Domination:
			return TEXT("Capture & hold A / B / C · first to 200");
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

void UPFMapFavButton::InitRow(UPFLoadingMenuWidget* InOwner, int32 InSlotIndex)
{
	OwnerWidget = InOwner;
	SlotIndex = InSlotIndex;
	OnClicked.AddUniqueDynamic(this, &UPFMapFavButton::HandleClicked);
}

void UPFMapFavButton::HandleClicked()
{
	if (OwnerWidget.IsValid())
	{
		OwnerWidget->NotifyMapFavClicked(SlotIndex);
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
	AddHowToLine(Box, TEXT("Lobby → Build forts → Fight. Pick map, mode, type, format, and bots here, then START GAME."), 12, false, Body);

	AddHowToLine(Box, TEXT("MOVE & LOOK"), 14, true, Head);
	AddHowToLine(Box, TEXT("WASD move · Mouse look · Space jump (again mid-air at a wall = climb over) · Shift sprint · Ctrl/C crouch (hold or toggle in Options)"), 12, false, Key);

	AddHowToLine(Box, TEXT("COMBAT"), 14, true, Head);
	AddHowToLine(Box, TEXT("LMB fire · RMB aim · V fire mode · R reload · F refill at barrels · E frag · Q smoke · B melee · Scroll pistol"), 12, false, Key);
	AddHowToLine(Box, TEXT("Mid-field floating BOMB — F to grab · G plant on a build piece (15s fuse) · enemies HOLD F 8s to defuse · boom removes that piece for the match"), 12, false, Key);
	AddHowToLine(Box, TEXT("OUT at 3 head, 5 chest, 8 limb, or 10 total hits (HUD pips + H/C/L). Showdown: one hit."), 12, false, Body);

	AddHowToLine(Box, TEXT("BUILD"), 14, true, Head);
	AddHowToLine(Box, TEXT("F1–F4 structure · scroll Wall→Window→Door… · tap Q piece wheel · LMB place · F open doors · Trap = enemy footfall"), 12, false, Key);
	AddHowToLine(Box, TEXT("Your half only. Full wall-offs OK — bomb a sealed path in combat."), 12, false, Body);

	AddHowToLine(Box, TEXT("LOBBY"), 14, true, Head);
	AddHowToLine(Box, TEXT("F ready · Enter host start · Tab scoreboard · Esc options · scroll = switch class while out"), 12, false, Key);

	AddHowToLine(Box, TEXT("MAP · MODE · TYPE"), 14, true, Head);
	AddHowToLine(Box, TEXT("Map = arena (Warehouse indoor / The Yard open-air). Mode = build style (Creative / Improvement / Play-Only). Type = win condition (Elim, Skirmish, CTF…)."), 12, false, Dim);
	AddHowToLine(Box, TEXT("Improvement & Play-Only: pick a community map (top 100, 10 per page) — hosts can star up to 5 favorites."), 12, false, Dim);
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
	MapFavButtons.Reset();
	MapFavGlyphs.Reset();
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

		// Favorite star (host only; ASCII '*' — U+2605 is tofu in Roboto). Nested button
		// consumes the click, so starring never also selects the row.
		UPFMapFavButton* Fav = WidgetTree->ConstructWidget<UPFMapFavButton>(
			UPFMapFavButton::StaticClass(), *FString::Printf(TEXT("MapFav%d"), i));
		Fav->SetBackgroundColor(FLinearColor(0.10f, 0.11f, 0.14f, 0.2f));
		Fav->InitRow(this, i);
		UTextBlock* FavGlyph = WidgetTree->ConstructWidget<UTextBlock>();
		FavGlyph->SetText(FText::FromString(TEXT("*")));
		FavGlyph->SetFont(PFLoadFont(20, true));
		FavGlyph->SetColorAndOpacity(FSlateColor(FLinearColor(0.40f, 0.42f, 0.48f)));
		FavGlyph->SetJustification(ETextJustify::Center);
		Fav->AddChild(FavGlyph);
		USizeBox* FavBox = WidgetTree->ConstructWidget<USizeBox>();
		FavBox->SetWidthOverride(36.f);
		FavBox->SetHeightOverride(36.f);
		FavBox->SetContent(Fav);
		if (UHorizontalBoxSlot* H = RowContent->AddChildToHorizontalBox(FavBox))
		{
			H->SetVerticalAlignment(VAlign_Center);
			H->SetPadding(FMargin(4.f, 2.f));
		}

		Row->AddChild(RowContent);

		MapSlotButtons.Add(Row);
		MapSlotLabels.Add(Lab);
		MapSlotImages.Add(Thumb);
		MapFavButtons.Add(Fav);
		MapFavGlyphs.Add(FavGlyph);
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
	const FString PngPath = FPFPaths::ArenaDir() / PngName;
	UTexture2D* Tex = FPaths::FileExists(PngPath) ? FImageUtils::ImportFileAsTexture2D(PngPath) : nullptr;
	// Brighten ~10% in the PIXELS at load — the raw viewport grabs read dark, and a widget tint > 1.0 is
	// clamped by Slate (the earlier display-time tint was a silent no-op). Uniform for old + new shots.
	if (Tex != nullptr && Tex->GetPlatformData() != nullptr && Tex->GetPlatformData()->Mips.Num() > 0
		&& Tex->GetPixelFormat() == PF_B8G8R8A8)
	{
		FTexture2DMipMap& Mip = Tex->GetPlatformData()->Mips[0];
		if (uint8* Px = static_cast<uint8*>(Mip.BulkData.Lock(LOCK_READ_WRITE)))
		{
			const int64 Bytes = Mip.BulkData.GetBulkDataSize();
			for (int64 i = 0; i + 3 < Bytes; i += 4)
			{
				Px[i + 0] = static_cast<uint8>(FMath::Min<int32>(255, (static_cast<int32>(Px[i + 0]) * 110) / 100));
				Px[i + 1] = static_cast<uint8>(FMath::Min<int32>(255, (static_cast<int32>(Px[i + 1]) * 110) / 100));
				Px[i + 2] = static_cast<uint8>(FMath::Min<int32>(255, (static_cast<int32>(Px[i + 2]) * 110) / 100));
			}
			Mip.BulkData.Unlock();
			Tex->UpdateResource();
		}
	}
	MapPreviewCache.Add(JsonFileName, Tex);
	return Tex;
}

void UPFLoadingMenuWidget::SelectMenuTab(int32 Index)
{
	ActiveMenuTab = FMath::Clamp(Index, 0, 4);
	if (MenuSwitcher)
	{
		MenuSwitcher->SetActiveWidgetIndex(ActiveMenuTab);
	}
	// Highlight active tab with a brighter plate.
	const FLinearColor Hot(1.f, 0.92f, 0.35f, 0.95f);
	const FLinearColor Cold(0.14f, 0.15f, 0.18f, 0.95f);
	if (TabSetup)         { TabSetup->SetBackgroundColor(ActiveMenuTab == 0 ? Hot : Cold); }
	if (TabHowTo)         { TabHowTo->SetBackgroundColor(ActiveMenuTab == 1 ? Hot : Cold); }
	if (TabLoadout)       { TabLoadout->SetBackgroundColor(ActiveMenuTab == 2 ? Hot : Cold); }
	if (TabCharacter)     { TabCharacter->SetBackgroundColor(ActiveMenuTab == 3 ? Hot : Cold); }
	if (OptionsTabButton) { OptionsTabButton->SetBackgroundColor(ActiveMenuTab == 4 ? Hot : Cold); }

	// Live character+gun studio while CHARACTER (or LOADOUT) is open — same preview model both places.
	if (ActiveMenuTab == 2 || ActiveMenuTab == 3)
	{
		EnsureCharPreview();
		if (CharPreviewActor != nullptr)
		{
			CharPreviewActor->ApplyConfig(CharConfig);
			CharPreviewActor->ApplyWeapon(WeaponConfig);
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

void UPFLoadingMenuWidget::OnOptionsClicked() { SelectMenuTab(4); }

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
	if (LoadoutCrosshairValueText)
	{
		LoadoutCrosshairValueText->SetText(FText::FromString(CrosshairStyleName(WorkingCrosshairStyle)));
	}
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
	Sub->SetText(FText::FromString(TEXT("Pick a weapon — live preview on the right. Saved to this class slot.")));
	Sub->SetFont(PFLoadFont(13, false));
	Sub->SetColorAndOpacity(FSlateColor(FLinearColor(1.f, 1.f, 1.f, 0.65f)));
	Sub->SetJustification(ETextJustify::Center);
	if (UVerticalBoxSlot* V = Col->AddChildToVerticalBox(Sub))
	{
		V->SetPadding(FMargin(0.f, 4.f, 0.f, 12.f));
		V->SetHorizontalAlignment(HAlign_Center);
	}

	// Same 3D studio as CHARACTER: character + selected gun. Drag on the image to turn.
	UHorizontalBox* Body = WidgetTree->ConstructWidget<UHorizontalBox>();
	LoadoutPreviewImage = WidgetTree->ConstructWidget<UImage>();
	LoadoutPreviewImage->SetColorAndOpacity(FLinearColor::White);
	LoadoutPreviewImage->SetVisibility(ESlateVisibility::Visible);
	USizeBox* PreviewSizer = WidgetTree->ConstructWidget<USizeBox>();
	PreviewSizer->SetWidthOverride(280.f);
	PreviewSizer->SetHeightOverride(360.f);
	PreviewSizer->SetContent(LoadoutPreviewImage);
	if (UHorizontalBoxSlot* H = Body->AddChildToHorizontalBox(PreviewSizer))
	{
		H->SetPadding(FMargin(4.f, 0.f, 12.f, 0.f));
		H->SetVerticalAlignment(VAlign_Center);
	}
	UVerticalBox* Right = WidgetTree->ConstructWidget<UVerticalBox>();
	if (UHorizontalBoxSlot* H = Body->AddChildToHorizontalBox(Right))
	{
		H->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		H->SetVerticalAlignment(VAlign_Center);
	}
	BuildWeaponPicker(Right);

	// Crosshair row (local pref, not per-weapon)
	{
		UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>();
		UTextBlock* Lab = WidgetTree->ConstructWidget<UTextBlock>();
		Lab->SetText(FText::FromString(TEXT("CROSSHAIR")));
		Lab->SetFont(PFLoadFont(13, true));
		Lab->SetColorAndOpacity(FSlateColor(FLinearColor(0.55f, 0.72f, 0.95f)));
		USizeBox* Sizer = WidgetTree->ConstructWidget<USizeBox>();
		Sizer->SetWidthOverride(110.f);
		Sizer->SetContent(Lab);
		Row->AddChildToHorizontalBox(Sizer);
		LoadoutCrosshairButton = WidgetTree->ConstructWidget<UButton>();
		LoadoutCrosshairButton->SetBackgroundColor(FLinearColor(0.12f, 0.13f, 0.16f, 1.f));
		LoadoutCrosshairButton->OnClicked.AddDynamic(this, &UPFLoadingMenuWidget::OnCrosshairCycle);
		LoadoutCrosshairValueText = WidgetTree->ConstructWidget<UTextBlock>();
		LoadoutCrosshairValueText->SetFont(PFLoadFont(12, false));
		LoadoutCrosshairValueText->SetJustification(ETextJustify::Center);
		LoadoutCrosshairValueText->SetClipping(EWidgetClipping::ClipToBounds);
		LoadoutCrosshairValueText->SetColorAndOpacity(FSlateColor(FLinearColor::White));
		LoadoutCrosshairButton->AddChild(LoadoutCrosshairValueText);
		if (UHorizontalBoxSlot* H = Row->AddChildToHorizontalBox(LoadoutCrosshairButton))
		{
			H->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
			H->SetPadding(FMargin(8.f, 0.f));
		}
		if (UVerticalBoxSlot* V = Right->AddChildToVerticalBox(Row))
		{
			V->SetPadding(FMargin(24.f, 8.f, 8.f, 0.f));
			V->SetHorizontalAlignment(HAlign_Fill);
		}
	}

	if (UVerticalBoxSlot* V = Col->AddChildToVerticalBox(Body))
	{
		V->SetPadding(FMargin(0.f, 4.f));
		V->SetHorizontalAlignment(HAlign_Fill);
	}

	UTextBlock* Hint = WidgetTree->ConstructWidget<UTextBlock>();
	Hint->SetText(FText::FromString(TEXT("Drag the preview to rotate · weapon saved to this class slot")));
	Hint->SetFont(PFLoadFont(12, false));
	Hint->SetColorAndOpacity(FSlateColor(FLinearColor(0.5f, 0.52f, 0.58f)));
	Hint->SetJustification(ETextJustify::Center);
	if (UVerticalBoxSlot* V = Col->AddChildToVerticalBox(Hint))
	{
		V->SetPadding(FMargin(0.f, 12.f, 0.f, 0.f));
		V->SetHorizontalAlignment(HAlign_Center);
	}

	WorkingCrosshairStyle = FPFUserPrefs::GetCrosshairStyle();
	RefreshLoadoutLabels();
}

void UPFLoadingMenuWidget::BuildWeaponPicker(UVerticalBox* Col)
{
	WeaponConfig = PFWeapon::LoadConfig();

	UTextBlock* Head = WidgetTree->ConstructWidget<UTextBlock>();
	Head->SetText(FText::FromString(TEXT("WEAPON")));
	Head->SetFont(PFLoadFont(15, true));
	Head->SetColorAndOpacity(FSlateColor(FLinearColor(1.f, 0.92f, 0.35f)));
	Head->SetJustification(ETextJustify::Center);
	if (UVerticalBoxSlot* V = Col->AddChildToVerticalBox(Head))
	{
		V->SetHorizontalAlignment(HAlign_Center);
		V->SetPadding(FMargin(0.f, 2.f, 0.f, 6.f));
	}

	auto MakeStepRow = [this, Col](const FString& Label, int32 Kind, TObjectPtr<UTextBlock>& OutVal)
	{
		UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>();
		UTextBlock* Lab = WidgetTree->ConstructWidget<UTextBlock>();
		Lab->SetText(FText::FromString(Label));
		Lab->SetFont(PFLoadFont(13, true));
		Lab->SetColorAndOpacity(FSlateColor(FLinearColor(0.55f, 0.72f, 0.95f)));
		USizeBox* LabSizer = WidgetTree->ConstructWidget<USizeBox>();
		LabSizer->SetWidthOverride(110.f);
		LabSizer->SetContent(Lab);
		Row->AddChildToHorizontalBox(LabSizer);

		UPFWeaponStepButton* Prev = WidgetTree->ConstructWidget<UPFWeaponStepButton>(UPFWeaponStepButton::StaticClass());
		Prev->SetBackgroundColor(FLinearColor(0.12f, 0.13f, 0.16f, 1.f));
		Prev->InitStep(this, Kind, -1);
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
		OutVal = Val;

		UPFWeaponStepButton* Next = WidgetTree->ConstructWidget<UPFWeaponStepButton>(UPFWeaponStepButton::StaticClass());
		Next->SetBackgroundColor(FLinearColor(0.12f, 0.13f, 0.16f, 1.f));
		Next->InitStep(this, Kind, +1);
		UTextBlock* NextLab = WidgetTree->ConstructWidget<UTextBlock>();
		NextLab->SetText(FText::FromString(TEXT(" > ")));
		NextLab->SetFont(PFLoadFont(14, true));
		NextLab->SetColorAndOpacity(FSlateColor(FLinearColor::White));
		Next->SetContent(NextLab);
		Row->AddChildToHorizontalBox(Next);

		if (UVerticalBoxSlot* V = Col->AddChildToVerticalBox(Row))
		{
			V->SetPadding(FMargin(24.f, 3.f));
			V->SetHorizontalAlignment(HAlign_Fill);
		}
	};

	MakeStepRow(TEXT("CATEGORY"), 0, WeaponCatValueText);
	MakeStepRow(TEXT("WEAPON"), 1, WeaponValueText);
	RefreshWeaponLabels();
}

void UPFLoadingMenuWidget::RefreshWeaponLabels()
{
	const int32 CatCount = PFWeapon::CategoryCount();
	WeaponConfig.Category = FMath::Clamp(WeaponConfig.Category, 0, CatCount - 1);
	const int32 WpnCount = PFWeapon::WeaponCount(WeaponConfig.Category);
	WeaponConfig.Index = FMath::Clamp(WeaponConfig.Index, 0, WpnCount - 1);
	if (WeaponCatValueText != nullptr)
	{
		WeaponCatValueText->SetText(FText::FromString(FString::Printf(TEXT("%s  (%d/%d)"),
			*PFWeapon::CategoryLabel(WeaponConfig.Category), WeaponConfig.Category + 1, CatCount)));
	}
	if (WeaponValueText != nullptr)
	{
		FString Label = FString::Printf(TEXT("%s  (%d/%d)"),
			*PFWeapon::WeaponDisplayName(WeaponConfig.Category, WeaponConfig.Index),
			WeaponConfig.Index + 1, WpnCount);
		// Rank lock badge when logged in with unlocks (offline = ungated).
		if (UPFBackendSubsystem* Backend = GetBackend())
		{
			const FString Id = PFWeapon::IdOf(WeaponConfig.Category, WeaponConfig.Index);
			if (!Backend->IsWeaponUnlocked(Id))
			{
				Label += FString::Printf(TEXT("  🔒 Rank %u"),
					PFWeapon::UnlockRankOf(WeaponConfig.Category, WeaponConfig.Index));
			}
		}
		WeaponValueText->SetText(FText::FromString(Label));
	}
}

void UPFLoadingMenuWidget::NotifyWeaponStep(int32 Kind, int32 Dir)
{
	const int32 CatCount = PFWeapon::CategoryCount();
	const FPFWeaponConfig Prev = WeaponConfig;
	if (Kind == 0)
	{
		WeaponConfig.Category = (WeaponConfig.Category + Dir + CatCount) % CatCount;
		WeaponConfig.Index = 0;   // first weapon in the new category
	}
	else
	{
		const int32 WpnCount = FMath::Max(1, PFWeapon::WeaponCount(WeaponConfig.Category));
		WeaponConfig.Index = (WeaponConfig.Index + Dir + WpnCount) % WpnCount;
	}

	// Offline / empty unlocks = fully ungated. When locked: allow browsing the name+rank, but refuse
	// to save/equip and snap the equipped kit back (weapon-implementation-spec Stage 5).
	if (UPFBackendSubsystem* Backend = GetBackend())
	{
		const FString Id = PFWeapon::IdOf(WeaponConfig.Category, WeaponConfig.Index);
		if (!Backend->IsWeaponUnlocked(Id))
		{
			const FString LockedName = PFWeapon::WeaponDisplayName(WeaponConfig.Category, WeaponConfig.Index);
			const uint8 Rank = PFWeapon::UnlockRankOf(WeaponConfig.Category, WeaponConfig.Index);
			WeaponConfig = Prev;
			RefreshWeaponLabels();
			if (WeaponValueText != nullptr)
			{
				WeaponValueText->SetText(FText::FromString(FString::Printf(
					TEXT("%s  🔒 Rank %u — not equipped"), *LockedName, Rank)));
			}
			return;
		}
	}

	PFWeapon::SaveConfig(WeaponConfig);
	if (ACombatForgeCharacter* Char = Cast<ACombatForgeCharacter>(GetOwningPlayerPawn()))
	{
		Char->ReapplyWeaponLoadout();
	}
	// Menu studio: show the selected gun in the character's hand (same model as the CHARACTER tab).
	EnsureCharPreview();
	if (CharPreviewActor != nullptr)
	{
		CharPreviewActor->ApplyWeapon(WeaponConfig);
		if (ActiveMenuTab == 2 || ActiveMenuTab == 3)
		{
			CharPreviewActor->SetPreviewActive(true);
		}
	}
	RefreshWeaponLabels();
}

void UPFLoadingMenuWidget::BuildCharacterPage(UVerticalBox* Col)
{
	UTextBlock* Sub = WidgetTree->ConstructWidget<UTextBlock>();
	// 13px + wrap + Fill slot: the old default-size, centered, non-wrapping line overran the
	// HOST LAN GAME column on the right (Tom 2026-07-15).
	Sub->SetText(FText::FromString(TEXT("Your five classes — clothing and weapon per slot. Saved to this PC, applied on spawn.")));
	Sub->SetFont(PFLoadFont(13, false));
	Sub->SetAutoWrapText(true);
	Sub->SetColorAndOpacity(FSlateColor(FLinearColor(1.f, 1.f, 1.f, 0.65f)));
	Sub->SetJustification(ETextJustify::Center);
	if (UVerticalBoxSlot* V = Col->AddChildToVerticalBox(Sub))
	{
		V->SetPadding(FMargin(0.f, 4.f, 0.f, 12.f));
		V->SetHorizontalAlignment(HAlign_Fill);   // wrap width bounded by the 600px page, not the text
	}

	ActiveSaveSlot = PFChar::GetActiveSaveSlot();
	CharConfig = PFChar::LoadConfig(ActiveSaveSlot);
	CharSlotValueTexts.Reset();
	SaveSlotButtons.Reset();

	// Save-slot row (CoD create-a-class): pick a slot to load/edit; part edits auto-save to the active slot,
	// and the pawn spawns with whichever slot is active.
	UHorizontalBox* SlotRow = WidgetTree->ConstructWidget<UHorizontalBox>();
	for (int32 ss = 0; ss < PFChar::SaveSlotCount(); ++ss)
	{
		UPFCharSaveSlotButton* SlotBtn = WidgetTree->ConstructWidget<UPFCharSaveSlotButton>(UPFCharSaveSlotButton::StaticClass());
		SlotBtn->InitSlot(this, ss);
		UTextBlock* SlotLab = WidgetTree->ConstructWidget<UTextBlock>();
		SlotLab->SetText(FText::FromString(FString::Printf(TEXT("  %d  "), ss + 1)));
		SlotLab->SetFont(PFLoadFont(16, true));
		SlotLab->SetColorAndOpacity(FSlateColor(FLinearColor::White));
		SlotLab->SetJustification(ETextJustify::Center);
		SlotBtn->SetContent(SlotLab);
		SaveSlotButtons.Add(SlotBtn);
		if (UHorizontalBoxSlot* H = SlotRow->AddChildToHorizontalBox(SlotBtn))
		{
			H->SetPadding(FMargin(5.f, 0.f));
			H->SetVerticalAlignment(VAlign_Center);
		}
	}
	if (UVerticalBoxSlot* V = Col->AddChildToVerticalBox(SlotRow))
	{
		V->SetHorizontalAlignment(HAlign_Center);
		V->SetPadding(FMargin(0.f, 0.f, 0.f, 10.f));
	}

	// The class's WEAPON — merged from the old LOADOUT tab; per-slot persistence (WeaponCat_<slot>/WeaponIdx_<slot>).
	BuildWeaponPicker(Col);

	// Crosshair preference (moved from the old LOADOUT tab; local, not per-class).
	{
		UHorizontalBox* XRow = WidgetTree->ConstructWidget<UHorizontalBox>();
		UTextBlock* XLab = WidgetTree->ConstructWidget<UTextBlock>();
		XLab->SetText(FText::FromString(TEXT("CROSSHAIR")));
		XLab->SetFont(PFLoadFont(13, true));
		XLab->SetColorAndOpacity(FSlateColor(FLinearColor(0.55f, 0.72f, 0.95f)));
		USizeBox* XSizer = WidgetTree->ConstructWidget<USizeBox>();
		XSizer->SetWidthOverride(110.f);
		XSizer->SetContent(XLab);
		XRow->AddChildToHorizontalBox(XSizer);
		LoadoutCrosshairButton = WidgetTree->ConstructWidget<UButton>();
		LoadoutCrosshairButton->SetBackgroundColor(FLinearColor(0.12f, 0.13f, 0.16f, 1.f));
		LoadoutCrosshairButton->OnClicked.AddDynamic(this, &UPFLoadingMenuWidget::OnCrosshairCycle);
		LoadoutCrosshairValueText = WidgetTree->ConstructWidget<UTextBlock>();
		LoadoutCrosshairValueText->SetFont(PFLoadFont(12, false));
		LoadoutCrosshairValueText->SetJustification(ETextJustify::Center);
		LoadoutCrosshairValueText->SetClipping(EWidgetClipping::ClipToBounds);
		LoadoutCrosshairValueText->SetColorAndOpacity(FSlateColor(FLinearColor::White));
		LoadoutCrosshairButton->AddChild(LoadoutCrosshairValueText);
		if (UHorizontalBoxSlot* H = XRow->AddChildToHorizontalBox(LoadoutCrosshairButton))
		{
			H->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
			H->SetPadding(FMargin(8.f, 0.f));
		}
		if (UVerticalBoxSlot* V = Col->AddChildToVerticalBox(XRow))
		{
			V->SetPadding(FMargin(40.f, 4.f));
			V->SetHorizontalAlignment(HAlign_Fill);
		}
	}
	WorkingCrosshairStyle = FPFUserPrefs::GetCrosshairStyle();
	RefreshLoadoutLabels();

	// Left: live rotating 3D preview (render target bound in EnsureCharPreview). Right: per-slot steppers.
	UHorizontalBox* Body = WidgetTree->ConstructWidget<UHorizontalBox>();

	CharPreviewImage = WidgetTree->ConstructWidget<UImage>();
	CharPreviewImage->SetColorAndOpacity(FLinearColor::White);
	CharPreviewImage->SetVisibility(ESlateVisibility::Visible);   // hit-testable so drag reaches the menu handler
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
	Hint->SetText(FText::FromString(TEXT("Pick a slot 1-5 · < / > to change each piece · saved instantly · drag to rotate")));
	Hint->SetColorAndOpacity(FSlateColor(FLinearColor(0.5f, 0.52f, 0.58f)));
	Hint->SetJustification(ETextJustify::Center);
	if (UVerticalBoxSlot* V = Col->AddChildToVerticalBox(Hint))
	{
		V->SetPadding(FMargin(0.f, 12.f, 0.f, 0.f));
		V->SetHorizontalAlignment(HAlign_Center);
	}

	RefreshCharacterLabels();
	RefreshSaveSlotHighlight();
}

void UPFLoadingMenuWidget::RefreshSaveSlotHighlight()
{
	const FLinearColor Hot(1.f, 0.92f, 0.35f, 0.95f);
	const FLinearColor Cold(0.12f, 0.13f, 0.16f, 1.f);
	for (int32 i = 0; i < SaveSlotButtons.Num(); ++i)
	{
		if (SaveSlotButtons[i] != nullptr)
		{
			SaveSlotButtons[i]->SetBackgroundColor(i == ActiveSaveSlot ? Hot : Cold);
		}
	}
}

void UPFLoadingMenuWidget::NotifySaveSlotSelected(int32 SaveSlot)
{
	if (SaveSlot < 0 || SaveSlot >= PFChar::SaveSlotCount())
	{
		return;
	}
	ActiveSaveSlot = SaveSlot;
	PFChar::SetActiveSaveSlot(SaveSlot);
	CharConfig = PFChar::LoadConfig(SaveSlot);
	WeaponConfig = PFWeapon::LoadConfig(SaveSlot);   // a class = clothing AND weapon — load both together
	// Push the newly-selected class to the spawned pawn + the tab preview.
	if (ACombatForgeCharacter* Char = Cast<ACombatForgeCharacter>(GetOwningPlayerPawn()))
	{
		Char->ReapplyCharacterConfig();
		Char->ReapplyWeaponLoadout();
	}
	if (CharPreviewActor != nullptr)
	{
		CharPreviewActor->ApplyConfig(CharConfig);
		CharPreviewActor->ApplyWeapon(WeaponConfig);
	}
	RefreshCharacterLabels();
	RefreshWeaponLabels();
	RefreshSaveSlotHighlight();
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
	CharPreviewActor->ApplyWeapon(WeaponConfig);
	auto BindPreview = [this](UImage* Img, float W, float H)
	{
		if (Img == nullptr || CharPreviewActor == nullptr || CharPreviewActor->GetRenderTarget() == nullptr)
		{
			return;
		}
		FSlateBrush Brush;
		Brush.SetResourceObject(CharPreviewActor->GetRenderTarget());
		Brush.ImageSize = FVector2D(W, H);
		Brush.DrawAs = ESlateBrushDrawType::Image;
		Img->SetBrush(Brush);
	};
	BindPreview(CharPreviewImage, 300.f, 400.f);
	BindPreview(LoadoutPreviewImage, 280.f, 360.f);
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
	if (ACombatForgeCharacter* Char = Cast<ACombatForgeCharacter>(GetOwningPlayerPawn()))
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

void UPFCharSaveSlotButton::InitSlot(UPFLoadingMenuWidget* InOwner, int32 InSaveSlot)
{
	OwnerWidget = InOwner;
	SaveSlot = InSaveSlot;
	OnClicked.AddUniqueDynamic(this, &UPFCharSaveSlotButton::HandleClicked);
}

void UPFCharSaveSlotButton::HandleClicked()
{
	if (OwnerWidget.IsValid())
	{
		OwnerWidget->NotifySaveSlotSelected(SaveSlot);
	}
}

void UPFWeaponStepButton::InitStep(UPFLoadingMenuWidget* InOwner, int32 InKind, int32 InDir)
{
	OwnerWidget = InOwner;
	Kind = InKind;
	Dir = InDir;
	OnClicked.AddUniqueDynamic(this, &UPFWeaponStepButton::HandleClicked);
}

void UPFWeaponStepButton::HandleClicked()
{
	if (OwnerWidget.IsValid())
	{
		OwnerWidget->NotifyWeaponStep(Kind, Dir);
	}
}

void UPFModeCardButton::InitCard(UPFLoadingMenuWidget* InOwner, int32 InKind, int32 InValue)
{
	OwnerWidget = InOwner;
	Kind = InKind;
	Value = InValue;
	OnClicked.AddUniqueDynamic(this, &UPFModeCardButton::HandleClicked);
}

void UPFModeCardButton::HandleClicked()
{
	if (OwnerWidget.IsValid())
	{
		OwnerWidget->NotifyCardSelected(Kind, Value);
	}
}

void UPFLoadingMenuWidget::BuildSetupCards(UVerticalBox* Col)
{
	// Description panel — the selected game mode's name + what it is (CoD-style header above the cards).
	SetupDescTitle = WidgetTree->ConstructWidget<UTextBlock>();
	SetupDescTitle->SetFont(PFLoadFont(20, true));
	SetupDescTitle->SetColorAndOpacity(FSlateColor(FLinearColor(1.f, 0.92f, 0.35f)));
	SetupDescTitle->SetJustification(ETextJustify::Center);
	if (UVerticalBoxSlot* V = Col->AddChildToVerticalBox(SetupDescTitle))
	{
		V->SetHorizontalAlignment(HAlign_Center);
		V->SetPadding(FMargin(0.f, 0.f, 0.f, 2.f));
	}
	SetupDescText = WidgetTree->ConstructWidget<UTextBlock>();
	SetupDescText->SetFont(PFLoadFont(13, false));
	SetupDescText->SetColorAndOpacity(FSlateColor(FLinearColor(0.78f, 0.81f, 0.88f)));
	SetupDescText->SetJustification(ETextJustify::Center);
	SetupDescText->SetAutoWrapText(true);
	if (UVerticalBoxSlot* V = Col->AddChildToVerticalBox(SetupDescText))
	{
		V->SetHorizontalAlignment(HAlign_Center);
		V->SetPadding(FMargin(0.f, 0.f, 0.f, 12.f));
	}

	auto MakeCardRow = [this, Col](const FString& Header, int32 Kind, int32 Count,
		TFunctionRef<FString(int32)> LabelFn, TArray<TObjectPtr<UPFModeCardButton>>& OutCards)
	{
		UTextBlock* Head = WidgetTree->ConstructWidget<UTextBlock>();
		Head->SetText(FText::FromString(Header));
		Head->SetFont(PFLoadFont(11, true));
		Head->SetColorAndOpacity(FSlateColor(FLinearColor(0.55f, 0.72f, 0.95f)));
		if (UVerticalBoxSlot* V = Col->AddChildToVerticalBox(Head))
		{
			V->SetPadding(FMargin(4.f, 4.f, 0.f, 3.f));
		}
		UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>();
		OutCards.Reset();
		for (int32 i = 0; i < Count; ++i)
		{
			UPFModeCardButton* Card = WidgetTree->ConstructWidget<UPFModeCardButton>(UPFModeCardButton::StaticClass());
			Card->InitCard(this, Kind, i);
			UTextBlock* Lab = WidgetTree->ConstructWidget<UTextBlock>();
			Lab->SetText(FText::FromString(LabelFn(i)));
			Lab->SetFont(PFLoadFont(11, true));
			Lab->SetColorAndOpacity(FSlateColor(FLinearColor::White));
			Lab->SetJustification(ETextJustify::Center);
			Lab->SetAutoWrapText(true);   // long names (e.g. "Capture the Flag") wrap inside the card
			Card->SetContent(Lab);
			OutCards.Add(Card);
			if (UHorizontalBoxSlot* H = Row->AddChildToHorizontalBox(Card))
			{
				H->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
				H->SetPadding(FMargin(3.f, 0.f));
			}
		}
		if (UVerticalBoxSlot* V = Col->AddChildToVerticalBox(Row))
		{
			V->SetPadding(FMargin(0.f, 0.f, 0.f, 8.f));
			V->SetHorizontalAlignment(HAlign_Fill);
		}
	};

	MakeCardRow(TEXT("BUILD MODE"), 0, static_cast<int32>(EPFBuildMode::MAX_Count),
		[](int32 i) { return BuildModeLabel(static_cast<EPFBuildMode>(i)); }, ModeCards);
	MakeCardRow(TEXT("GAME MODE"), 1, static_cast<int32>(EPFMatchType::MAX_Count),
		[](int32 i) { return MatchTypeLabel(static_cast<EPFMatchType>(i)); }, TypeCards);
	MakeCardRow(TEXT("FORMAT"), 2, 2,
		[](int32 i) { return FString(i == 0 ? TEXT("4 v 4") : TEXT("6 v 6")); }, FormatCards);
	MakeCardRow(TEXT("MAP"), 3, static_cast<int32>(EPFArenaMap::MAX_Count),
		[](int32 i) { return PFGetArenaMapDef(static_cast<EPFArenaMap>(i)).Label; }, MapArenaCards);

	RefreshSetupCards();
}

void UPFLoadingMenuWidget::RefreshSetupCards()
{
	const FLinearColor Hot(1.f, 0.85f, 0.25f, 0.95f);
	const FLinearColor Cold(0.14f, 0.15f, 0.18f, 0.95f);
	for (int32 i = 0; i < ModeCards.Num(); ++i)
	{
		if (ModeCards[i] != nullptr)
		{
			ModeCards[i]->SetBackgroundColor(i == static_cast<int32>(SelectedBuildMode) ? Hot : Cold);
		}
	}
	for (int32 i = 0; i < TypeCards.Num(); ++i)
	{
		if (TypeCards[i] != nullptr)
		{
			TypeCards[i]->SetBackgroundColor(i == static_cast<int32>(SelectedMatchType) ? Hot : Cold);
		}
	}
	for (int32 i = 0; i < MapArenaCards.Num(); ++i)
	{
		if (MapArenaCards[i] != nullptr)
		{
			MapArenaCards[i]->SetBackgroundColor(i == static_cast<int32>(SelectedArenaMap) ? Hot : Cold);
		}
	}
	const int32 FmtSel = (SelectedTeamSize == 6) ? 1 : 0;
	// Free-for-All has no teams, so "4 v 4" is a lie there — the same format value means 8 / 12 total players
	// (FillBotsToFormat: FFA fills TeamSize*2 combatants). Re-text the cards to match the selected game mode.
	const bool bFFA = (SelectedMatchType == EPFMatchType::FreeForAll);
	for (int32 i = 0; i < FormatCards.Num(); ++i)
	{
		if (FormatCards[i] != nullptr)
		{
			FormatCards[i]->SetBackgroundColor(i == FmtSel ? Hot : Cold);
			if (UTextBlock* Lab = Cast<UTextBlock>(FormatCards[i]->GetContent()))
			{
				Lab->SetText(FText::FromString(bFFA
					? (i == 0 ? TEXT("8 PLAYERS") : TEXT("12 PLAYERS"))
					: (i == 0 ? TEXT("4 v 4") : TEXT("6 v 6"))));
			}
		}
	}
	// Description = the selected GAME MODE (what Tom asked to explain at the top).
	if (SetupDescTitle != nullptr)
	{
		SetupDescTitle->SetText(FText::FromString(MatchTypeLabel(SelectedMatchType)));
	}
	if (SetupDescText != nullptr)
	{
		SetupDescText->SetText(FText::FromString(MatchTypeBlurb(SelectedMatchType)));
	}
}

void UPFLoadingMenuWidget::NotifyCardSelected(int32 Kind, int32 Value)
{
	if (!IsLocalHost() || bDismissed)
	{
		return;
	}
	if (Kind == 0)
	{
		SelectedBuildMode = static_cast<EPFBuildMode>(
			FMath::Clamp(Value, 0, static_cast<int32>(EPFBuildMode::MAX_Count) - 1));
	}
	else if (Kind == 1)
	{
		SelectedMatchType = static_cast<EPFMatchType>(
			FMath::Clamp(Value, 0, static_cast<int32>(EPFMatchType::MAX_Count) - 1));
		if (SelectedMatchType == EPFMatchType::FreeForAll)
		{
			SelectedBuildMode = EPFBuildMode::PlayOnly;   // FFA is play-only by design
		}
	}
	else if (Kind == 2)
	{
		SelectedTeamSize = (Value == 1) ? 6 : 4;
	}
	else if (Kind == 3)
	{
		const EPFArenaMap NewMap = static_cast<EPFArenaMap>(
			FMath::Clamp(Value, 0, static_cast<int32>(EPFArenaMap::MAX_Count) - 1));
		const bool bArenaChanged = (NewMap != SelectedArenaMap);
		SelectedArenaMap = NewMap;
		// Warehouse ↔ Yard have independent community catalogs — drop the old pick first so we
		// never push a Warehouse file while switching to The Yard (or the reverse), then re-list.
		if (bArenaChanged)
		{
			SelectedMapCatalogIndex = INDEX_NONE;
			ApplySelectionsToHost();   // shell swap + community → auto; GS ArenaMap updates for ListTop
			ReloadMapCatalog();        // only maps for the new shell
			RefreshSetupLabels();
			return;
		}
	}
	RefreshSetupLabels();   // refreshes the cards AND the map-picker visibility (shows it for Improvement/Play-Only)
	ApplySelectionsToHost();
	if (NeedsCommunityMap())
	{
		ApplyMapSelectionToHost();
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
	TitleText->SetText(FText::FromString(TEXT("COMBAT FORGE")));
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

	// ---- Two-column body (playtest: one tall column scrolled forever) ----
	// LEFT: the tab row + all setup content (build mode, game mode, format, community maps, class, options).
	// RIGHT: every "do something" control — QUICK START, HOST/JOIN, START GAME, DONATE, QUIT — always on screen.
	UVerticalBox* LeftCol = WidgetTree->ConstructWidget<UVerticalBox>();
	UVerticalBox* RightCol = WidgetTree->ConstructWidget<UVerticalBox>();

	// First-session one-click preset (host): Play-Only Skirmish 4v4 with bots + community map.
	QuickStartButton = WidgetTree->ConstructWidget<UButton>(UButton::StaticClass(), TEXT("QuickStartBtn"));
	QuickStartButton->SetBackgroundColor(FLinearColor(1.f, 0.85f, 0.2f, 0.95f));
	QuickStartButton->OnClicked.AddDynamic(this, &UPFLoadingMenuWidget::OnQuickStartClicked);
	QuickStartLabel = WidgetTree->ConstructWidget<UTextBlock>();
	QuickStartLabel->SetText(FText::FromString(TEXT("  AUTO MATCH SETUP  ")));
	QuickStartLabel->SetFont(PFLoadFont(16, true));
	QuickStartLabel->SetColorAndOpacity(FSlateColor(FLinearColor(0.12f, 0.12f, 0.14f)));
	QuickStartLabel->SetJustification(ETextJustify::Center);
	QuickStartButton->AddChild(QuickStartLabel);
	if (UVerticalBoxSlot* V = RightCol->AddChildToVerticalBox(QuickStartButton))
	{
		V->SetHorizontalAlignment(HAlign_Fill);
		V->SetPadding(FMargin(0.f, 0.f, 0.f, 8.f));
	}
	UTextBlock* QuickHint = WidgetTree->ConstructWidget<UTextBlock>();
	QuickHint->SetText(FText::FromString(TEXT("Play-Only · Skirmish · 4v4 · bots · community map")));
	QuickHint->SetFont(PFLoadFont(12, false));
	QuickHint->SetColorAndOpacity(FSlateColor(FLinearColor(0.6f, 0.62f, 0.68f)));
	QuickHint->SetJustification(ETextJustify::Center);
	if (UVerticalBoxSlot* V = RightCol->AddChildToVerticalBox(QuickHint))
	{
		V->SetHorizontalAlignment(HAlign_Center);
		V->SetPadding(FMargin(0.f, 0.f, 0.f, 16.f));
	}

	// ---- LAN multiplayer (no matchmaking yet): HOST turns this PC into a listen server; JOIN connects to a
	//      host's IP — same network or VPN. State-aware: a fresh boot shows the controls; a hosting session
	//      shows "friends join <ip>"; a joined client shows the connection.
	{
		const ENetMode Net = GetWorld() ? GetWorld()->GetNetMode() : NM_Standalone;
		// Stacked for the narrow right column: HOST on its own line, then IP + JOIN as a row.
		UVerticalBox* MpBox = WidgetTree->ConstructWidget<UVerticalBox>();
		if (Net == NM_Standalone)
		{
			// ---- ONLINE (hosted servers via api.playcombatforge.com; account required) ----
			AccountStatusText = WidgetTree->ConstructWidget<UTextBlock>();
			AccountStatusText->SetFont(PFLoadFont(12, false));
			AccountStatusText->SetJustification(ETextJustify::Center);
			AccountStatusText->SetAutoWrapText(true);
			if (UVerticalBoxSlot* V = MpBox->AddChildToVerticalBox(AccountStatusText))
			{
				V->SetHorizontalAlignment(HAlign_Fill);
				V->SetPadding(FMargin(0.f, 0.f, 0.f, 6.f));
			}

			UHorizontalBox* OnlineRow = WidgetTree->ConstructWidget<UHorizontalBox>();
			LoginButton = WidgetTree->ConstructWidget<UButton>(UButton::StaticClass(), TEXT("LoginBtn"));
			LoginButton->SetBackgroundColor(FLinearColor(0.5f, 0.36f, 0.14f, 1.f));
			LoginButton->OnClicked.AddDynamic(this, &UPFLoadingMenuWidget::OnLoginClicked);
			LoginLabel = WidgetTree->ConstructWidget<UTextBlock>();
			LoginLabel->SetText(FText::FromString(TEXT("  LOG IN  ")));
			LoginLabel->SetFont(PFLoadFont(13, true));
			LoginLabel->SetColorAndOpacity(FSlateColor(FLinearColor::White));
			LoginButton->AddChild(LoginLabel);
			if (UHorizontalBoxSlot* H = OnlineRow->AddChildToHorizontalBox(LoginButton))
			{
				H->SetPadding(FMargin(0.f, 0.f, 8.f, 0.f));
				H->SetVerticalAlignment(VAlign_Center);
			}

			QuickPlayButton = WidgetTree->ConstructWidget<UButton>(UButton::StaticClass(), TEXT("QuickPlayBtn"));
			QuickPlayButton->SetBackgroundColor(FLinearColor(0.2f, 0.42f, 0.62f, 1.f));
			QuickPlayButton->OnClicked.AddDynamic(this, &UPFLoadingMenuWidget::OnQuickPlayClicked);
			UTextBlock* QpLab = WidgetTree->ConstructWidget<UTextBlock>();
			QpLab->SetText(FText::FromString(TEXT("  QUICK PLAY  ")));
			QpLab->SetFont(PFLoadFont(13, true));
			QpLab->SetColorAndOpacity(FSlateColor(FLinearColor::White));
			QuickPlayButton->AddChild(QpLab);
			if (UHorizontalBoxSlot* H = OnlineRow->AddChildToHorizontalBox(QuickPlayButton))
			{
				H->SetPadding(FMargin(0.f, 0.f, 8.f, 0.f));
				H->SetVerticalAlignment(VAlign_Center);
			}

			ServerListButton = WidgetTree->ConstructWidget<UButton>(UButton::StaticClass(), TEXT("ServerListBtn"));
			ServerListButton->SetBackgroundColor(FLinearColor(0.26f, 0.28f, 0.34f, 1.f));
			ServerListButton->OnClicked.AddDynamic(this, &UPFLoadingMenuWidget::OnServerListClicked);
			UTextBlock* SlLab = WidgetTree->ConstructWidget<UTextBlock>();
			SlLab->SetText(FText::FromString(TEXT("  SERVERS  ")));
			SlLab->SetFont(PFLoadFont(13, true));
			SlLab->SetColorAndOpacity(FSlateColor(FLinearColor::White));
			ServerListButton->AddChild(SlLab);
			if (UHorizontalBoxSlot* H = OnlineRow->AddChildToHorizontalBox(ServerListButton))
			{
				H->SetVerticalAlignment(VAlign_Center);
			}
			if (UVerticalBoxSlot* V = MpBox->AddChildToVerticalBox(OnlineRow))
			{
				V->SetHorizontalAlignment(HAlign_Center);
				V->SetPadding(FMargin(0.f, 0.f, 0.f, 6.f));
			}

			// Match code row: friends' private matches (6 characters, from the match host).
			UHorizontalBox* CodeRow = WidgetTree->ConstructWidget<UHorizontalBox>();
			JoinCodeBox = WidgetTree->ConstructWidget<UEditableTextBox>();
			JoinCodeBox->SetHintText(FText::FromString(TEXT("match code")));
			JoinCodeBox->WidgetStyle.SetFont(PFLoadFont(13, false));
			JoinCodeBox->WidgetStyle.SetForegroundColor(FSlateColor(FLinearColor(0.06f, 0.06f, 0.08f)));
			JoinCodeBox->SetForegroundColor(FLinearColor(0.06f, 0.06f, 0.08f));
			USizeBox* CodeSizer = WidgetTree->ConstructWidget<USizeBox>();
			CodeSizer->SetWidthOverride(210.f);
			CodeSizer->SetContent(JoinCodeBox);
			if (UHorizontalBoxSlot* H = CodeRow->AddChildToHorizontalBox(CodeSizer))
			{
				H->SetPadding(FMargin(0.f, 0.f, 8.f, 0.f));
				H->SetVerticalAlignment(VAlign_Center);
				H->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
			}
			JoinCodeButton = WidgetTree->ConstructWidget<UButton>(UButton::StaticClass(), TEXT("JoinCodeBtn"));
			JoinCodeButton->SetBackgroundColor(FLinearColor(0.16f, 0.24f, 0.4f, 1.f));
			JoinCodeButton->OnClicked.AddDynamic(this, &UPFLoadingMenuWidget::OnJoinCodeClicked);
			UTextBlock* CodeLab = WidgetTree->ConstructWidget<UTextBlock>();
			CodeLab->SetText(FText::FromString(TEXT("  GO  ")));
			CodeLab->SetFont(PFLoadFont(13, true));
			CodeLab->SetColorAndOpacity(FSlateColor(FLinearColor::White));
			JoinCodeButton->AddChild(CodeLab);
			if (UHorizontalBoxSlot* H = CodeRow->AddChildToHorizontalBox(JoinCodeButton))
			{
				H->SetVerticalAlignment(VAlign_Center);
			}
			if (UVerticalBoxSlot* V = MpBox->AddChildToVerticalBox(CodeRow))
			{
				V->SetHorizontalAlignment(HAlign_Fill);
				V->SetPadding(FMargin(0.f, 0.f, 0.f, 6.f));
			}

			// Collapsible server-browser rows (SERVERS toggles; rows fill from GET /v1/servers).
			ServerRowsBox = WidgetTree->ConstructWidget<UVerticalBox>();
			ServerRowButtons.Reset();
			ServerRowLabels.Reset();
			for (int32 RowIdx = 0; RowIdx < BrowserRowCount; ++RowIdx)
			{
				UButton* Row = WidgetTree->ConstructWidget<UButton>(UButton::StaticClass(),
					*FString::Printf(TEXT("ServerRow%dBtn"), RowIdx));
				Row->SetBackgroundColor(FLinearColor(0.14f, 0.16f, 0.2f, 1.f));
				switch (RowIdx)
				{
				case 0: Row->OnClicked.AddDynamic(this, &UPFLoadingMenuWidget::OnServerRow0Clicked); break;
				case 1: Row->OnClicked.AddDynamic(this, &UPFLoadingMenuWidget::OnServerRow1Clicked); break;
				case 2: Row->OnClicked.AddDynamic(this, &UPFLoadingMenuWidget::OnServerRow2Clicked); break;
				case 3: Row->OnClicked.AddDynamic(this, &UPFLoadingMenuWidget::OnServerRow3Clicked); break;
				case 4: Row->OnClicked.AddDynamic(this, &UPFLoadingMenuWidget::OnServerRow4Clicked); break;
				default: Row->OnClicked.AddDynamic(this, &UPFLoadingMenuWidget::OnServerRow5Clicked); break;
				}
				UTextBlock* RowLab = WidgetTree->ConstructWidget<UTextBlock>();
				RowLab->SetFont(PFLoadFont(12, false));
				RowLab->SetColorAndOpacity(FSlateColor(FLinearColor(0.85f, 0.87f, 0.92f)));
				Row->AddChild(RowLab);
				Row->SetVisibility(ESlateVisibility::Collapsed);
				if (UVerticalBoxSlot* V = ServerRowsBox->AddChildToVerticalBox(Row))
				{
					V->SetHorizontalAlignment(HAlign_Fill);
					V->SetPadding(FMargin(0.f, 0.f, 0.f, 3.f));
				}
				ServerRowButtons.Add(Row);
				ServerRowLabels.Add(RowLab);
			}
			if (UVerticalBoxSlot* V = MpBox->AddChildToVerticalBox(ServerRowsBox))
			{
				V->SetHorizontalAlignment(HAlign_Fill);
				V->SetPadding(FMargin(0.f, 0.f, 0.f, 10.f));
			}
			RefreshOnlinePanel();

			// ---- LAN (no account needed — offline path stays free) ----
			HostLanButton = WidgetTree->ConstructWidget<UButton>(UButton::StaticClass(), TEXT("HostLanBtn"));
			HostLanButton->SetBackgroundColor(FLinearColor(0.16f, 0.34f, 0.2f, 1.f));
			HostLanButton->OnClicked.AddDynamic(this, &UPFLoadingMenuWidget::OnHostLanClicked);
			UTextBlock* HostLab = WidgetTree->ConstructWidget<UTextBlock>();
			HostLab->SetText(FText::FromString(TEXT("HOST LAN GAME")));
			HostLab->SetFont(PFLoadFont(14, true));
			HostLab->SetColorAndOpacity(FSlateColor(FLinearColor::White));
			HostLab->SetJustification(ETextJustify::Center);
			HostLanButton->AddChild(HostLab);
			if (UVerticalBoxSlot* V = MpBox->AddChildToVerticalBox(HostLanButton))
			{
				V->SetHorizontalAlignment(HAlign_Fill);
				V->SetPadding(FMargin(0.f, 0.f, 0.f, 8.f));
			}

			UHorizontalBox* JoinRow = WidgetTree->ConstructWidget<UHorizontalBox>();
			JoinIpBox = WidgetTree->ConstructWidget<UEditableTextBox>();
			JoinIpBox->SetHintText(FText::FromString(TEXT("host IP")));
			JoinIpBox->SetText(FText::FromString(FPFUserPrefs::GetLastJoinIp()));
			JoinIpBox->WidgetStyle.SetFont(PFLoadFont(13, false));   // default editable-text font is comically large
			// The light default box washed out the "host IP" hint + typed IP to near-invisible (Tom). Dark text
			// on the light box; the hint reads as a lighter shade of the same dark so it's legible but muted.
			JoinIpBox->WidgetStyle.SetForegroundColor(FSlateColor(FLinearColor(0.06f, 0.06f, 0.08f)));
			JoinIpBox->SetForegroundColor(FLinearColor(0.06f, 0.06f, 0.08f));
			USizeBox* IpSizer = WidgetTree->ConstructWidget<USizeBox>();
			IpSizer->SetWidthOverride(210.f);
			IpSizer->SetContent(JoinIpBox);
			if (UHorizontalBoxSlot* H = JoinRow->AddChildToHorizontalBox(IpSizer))
			{
				H->SetPadding(FMargin(0.f, 0.f, 8.f, 0.f));
				H->SetVerticalAlignment(VAlign_Center);
				H->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
			}

			JoinLanButton = WidgetTree->ConstructWidget<UButton>(UButton::StaticClass(), TEXT("JoinLanBtn"));
			JoinLanButton->SetBackgroundColor(FLinearColor(0.16f, 0.24f, 0.4f, 1.f));
			JoinLanButton->OnClicked.AddDynamic(this, &UPFLoadingMenuWidget::OnJoinLanClicked);
			UTextBlock* JoinLab = WidgetTree->ConstructWidget<UTextBlock>();
			JoinLab->SetText(FText::FromString(TEXT("  JOIN  ")));
			JoinLab->SetFont(PFLoadFont(13, true));
			JoinLab->SetColorAndOpacity(FSlateColor(FLinearColor::White));
			JoinLanButton->AddChild(JoinLab);
			if (UHorizontalBoxSlot* H = JoinRow->AddChildToHorizontalBox(JoinLanButton))
			{
				H->SetVerticalAlignment(VAlign_Center);
			}
			if (UVerticalBoxSlot* V = MpBox->AddChildToVerticalBox(JoinRow))
			{
				V->SetHorizontalAlignment(HAlign_Fill);
			}
		}
		else
		{
			UTextBlock* NetLab = WidgetTree->ConstructWidget<UTextBlock>();
			NetLab->SetFont(PFLoadFont(13, true));
			NetLab->SetJustification(ETextJustify::Center);
			if (Net == NM_ListenServer)
			{
				NetLab->SetText(FText::FromString(FString::Printf(TEXT("HOSTING — friends join:  %s  "), *GetLocalLanIp())));
				NetLab->SetColorAndOpacity(FSlateColor(FLinearColor(0.45f, 0.9f, 0.5f)));
			}
			else
			{
				NetLab->SetText(FText::FromString(TEXT("CONNECTED to host")));
				NetLab->SetColorAndOpacity(FSlateColor(FLinearColor(0.5f, 0.75f, 1.f)));
			}
			if (UVerticalBoxSlot* V = MpBox->AddChildToVerticalBox(NetLab))
			{
				V->SetHorizontalAlignment(HAlign_Center);
				V->SetPadding(FMargin(0.f, 0.f, 0.f, 8.f));
			}
			if (Net == NM_ListenServer)
			{
				// Way back out: return to a private standalone session (disconnects any joined friends, who
				// can then host their own game).
				UButton* StopBtn = WidgetTree->ConstructWidget<UButton>(UButton::StaticClass(), TEXT("StopHostBtn"));
				StopBtn->SetBackgroundColor(FLinearColor(0.42f, 0.18f, 0.16f, 1.f));
				StopBtn->OnClicked.AddDynamic(this, &UPFLoadingMenuWidget::OnStopHostingClicked);
				UTextBlock* StopLab = WidgetTree->ConstructWidget<UTextBlock>();
				StopLab->SetText(FText::FromString(TEXT("STOP HOSTING")));
				StopLab->SetFont(PFLoadFont(12, true));
				StopLab->SetColorAndOpacity(FSlateColor(FLinearColor::White));
				StopLab->SetJustification(ETextJustify::Center);
				StopBtn->AddChild(StopLab);
				if (UVerticalBoxSlot* V = MpBox->AddChildToVerticalBox(StopBtn))
				{
					V->SetHorizontalAlignment(HAlign_Fill);
				}
			}
		}
		if (UVerticalBoxSlot* V = RightCol->AddChildToVerticalBox(MpBox))
		{
			V->SetHorizontalAlignment(HAlign_Fill);
			V->SetPadding(FMargin(0.f, 0.f, 0.f, 20.f));
		}
	}

	// ---- Menu tabs: Match Setup | How to Play ----
	UHorizontalBox* MenuTabs = WidgetTree->ConstructWidget<UHorizontalBox>();
	TabSetup = MakeMenuTab(TEXT("  MATCH SETUP  "), TEXT("TabSetup"));
	TabHowTo = MakeMenuTab(TEXT("  HOW TO PLAY  "), TEXT("TabHowTo"));
	TabCharacter = MakeMenuTab(TEXT("  CLASS  "), TEXT("TabCharacter"));   // merged Loadout+Character: 5 classes = clothing + weapon
	OptionsTabButton = MakeMenuTab(TEXT("  OPTIONS  "), TEXT("TabOptions"));   // opens the options overlay
	TabSetup->OnClicked.AddDynamic(this, &UPFLoadingMenuWidget::OnTabSetup);
	TabHowTo->OnClicked.AddDynamic(this, &UPFLoadingMenuWidget::OnTabHowTo);
	TabCharacter->OnClicked.AddDynamic(this, &UPFLoadingMenuWidget::OnTabCharacter);
	OptionsTabButton->OnClicked.AddDynamic(this, &UPFLoadingMenuWidget::OnOptionsClicked);
	// Tab display order: MATCH SETUP · LOADOUT · CHARACTER · OPTIONS · HOW TO PLAY (reference last). The
	// switcher page indices are decoupled from this order, so only the add order changes.
	if (UHorizontalBoxSlot* H = MenuTabs->AddChildToHorizontalBox(TabSetup))
	{
		H->SetPadding(FMargin(4.f, 0.f));
	}
	if (UHorizontalBoxSlot* H = MenuTabs->AddChildToHorizontalBox(TabCharacter))
	{
		H->SetPadding(FMargin(4.f, 0.f));
	}
	if (UHorizontalBoxSlot* H = MenuTabs->AddChildToHorizontalBox(OptionsTabButton))
	{
		H->SetPadding(FMargin(4.f, 0.f));
	}
	if (UHorizontalBoxSlot* H = MenuTabs->AddChildToHorizontalBox(TabHowTo))
	{
		H->SetPadding(FMargin(4.f, 0.f));
	}
	if (UVerticalBoxSlot* V = LeftCol->AddChildToVerticalBox(MenuTabs))
	{
		V->SetHorizontalAlignment(HAlign_Center);
		V->SetPadding(FMargin(0.f, 0.f, 0.f, 14.f));
	}

	USizeBox* MenuSizer = WidgetTree->ConstructWidget<USizeBox>();
	MenuSizer->SetWidthOverride(600.f);   // wide enough to host the embedded OPTIONS card
	MenuSwitcher = WidgetTree->ConstructWidget<UWidgetSwitcher>();
	MenuSizer->SetContent(MenuSwitcher);

	// Page 0 — match setup
	UVerticalBox* SetupCol = WidgetTree->ConstructWidget<UVerticalBox>();

	BuildSetupCards(SetupCol);   // CoD-style: description panel + mode/type/format card rows

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
	// (Loadout page merged into the CLASS tab — page 2 kept as an empty switcher placeholder.)

	UVerticalBox* CharacterCol = WidgetTree->ConstructWidget<UVerticalBox>();
	BuildCharacterPage(CharacterCol);

	// Page 4 — full options embedded inline as a tab (not a pop-up overlay).
	UVerticalBox* OptionsCol = WidgetTree->ConstructWidget<UVerticalBox>();
	if (APlayerController* PC = GetOwningPlayer())
	{
		BootOptions = CreateWidget<UPFOptionsWidget>(PC, UPFOptionsWidget::StaticClass());
		if (BootOptions != nullptr)
		{
			BootOptions->EnterEmbeddedMode();
			USizeBox* OptSizer = WidgetTree->ConstructWidget<USizeBox>();
			OptSizer->SetWidthOverride(580.f);
			OptSizer->SetHeightOverride(560.f);
			OptSizer->SetContent(BootOptions);
			if (UVerticalBoxSlot* V = OptionsCol->AddChildToVerticalBox(OptSizer))
			{
				V->SetHorizontalAlignment(HAlign_Center);
			}
		}
	}

	MenuSwitcher->AddChild(SetupCol);
	MenuSwitcher->AddChild(HowToCol);
	MenuSwitcher->AddChild(LoadoutCol);      // index 2
	MenuSwitcher->AddChild(CharacterCol);    // index 3
	MenuSwitcher->AddChild(OptionsCol);      // index 4

	if (UVerticalBoxSlot* V = LeftCol->AddChildToVerticalBox(MenuSizer))
	{
		V->SetHorizontalAlignment(HAlign_Center);
		V->SetPadding(FMargin(0.f, 0.f, 0.f, 28.f));
	}

	// ---- Warmup status (right column, above START GAME) ----
	StatusText = WidgetTree->ConstructWidget<UTextBlock>();
	StatusText->SetText(FText::FromString(TEXT("Starting…")));
	StatusText->SetFont(PFLoadFont(14, false));
	StatusText->SetColorAndOpacity(FSlateColor(FLinearColor(0.9f, 0.9f, 0.92f)));
	StatusText->SetJustification(ETextJustify::Center);
	StatusText->SetAutoWrapText(true);
	if (UVerticalBoxSlot* V = RightCol->AddChildToVerticalBox(StatusText))
	{
		V->SetHorizontalAlignment(HAlign_Fill);
		V->SetPadding(FMargin(0.f, 4.f, 0.f, 10.f));
	}

	ProgressBar = WidgetTree->ConstructWidget<UProgressBar>();
	ProgressBar->SetPercent(0.f);
	ProgressBar->SetFillColorAndOpacity(FLinearColor(1.f, 0.85f, 0.25f));
	USizeBox* ProgressSizer = WidgetTree->ConstructWidget<USizeBox>();
	ProgressSizer->SetHeightOverride(14.f);
	ProgressSizer->SetContent(ProgressBar);
	if (UVerticalBoxSlot* V = RightCol->AddChildToVerticalBox(ProgressSizer))
	{
		V->SetHorizontalAlignment(HAlign_Fill);
		V->SetPadding(FMargin(0.f, 0.f, 0.f, 20.f));
	}

	EnterButton = WidgetTree->ConstructWidget<UButton>();
	EnterButton->SetIsEnabled(false);
	EnterButton->OnClicked.AddDynamic(this, &UPFLoadingMenuWidget::OnEnterClicked);
	EnterLabel = WidgetTree->ConstructWidget<UTextBlock>();
	EnterLabel->SetText(FText::FromString(TEXT("START GAME")));
	EnterLabel->SetFont(PFLoadFont(18, true));
	EnterLabel->SetColorAndOpacity(FSlateColor(FLinearColor(0.15f, 0.15f, 0.18f)));
	EnterLabel->SetJustification(ETextJustify::Center);
	EnterButton->AddChild(EnterLabel);
	if (UVerticalBoxSlot* V = RightCol->AddChildToVerticalBox(EnterButton))
	{
		V->SetHorizontalAlignment(HAlign_Fill);
		V->SetPadding(FMargin(0.f, 8.f, 0.f, 0.f));
	}

	// Community links bar — Website · Discord · Buy Me a Coffee — floats bottom-center of the menu.
	// Each opens in the system default browser (FPlatformProcess::LaunchURL) — no in-game web view,
	// no network call from the game itself.
	UHorizontalBox* LinksRow = WidgetTree->ConstructWidget<UHorizontalBox>();
	auto AddLinkButton = [&](UButton*& OutBtn, TObjectPtr<UTextBlock>& OutLabel, const FString& Text,
		const FLinearColor& Bg, const FLinearColor& Fg, FName BtnName)
	{
		OutBtn = WidgetTree->ConstructWidget<UButton>(UButton::StaticClass(), BtnName);
		OutBtn->SetBackgroundColor(Bg);
		OutLabel = WidgetTree->ConstructWidget<UTextBlock>();
		OutLabel->SetText(FText::FromString(Text));
		OutLabel->SetFont(PFLoadFont(14, true));
		OutLabel->SetColorAndOpacity(FSlateColor(Fg));
		OutLabel->SetJustification(ETextJustify::Center);
		OutBtn->AddChild(OutLabel);
		if (UHorizontalBoxSlot* H = LinksRow->AddChildToHorizontalBox(OutBtn))
		{
			H->SetPadding(FMargin(6.f, 0.f));
			H->SetVerticalAlignment(VAlign_Center);
		}
	};
	UButton* WebBtn = nullptr;
	UButton* DiscBtn = nullptr;
	UButton* DonBtn = nullptr;
	AddLinkButton(WebBtn, WebsiteLabel, TEXT("  WEBSITE  "),
		FLinearColor(0.16f, 0.20f, 0.28f, 0.95f), FLinearColor(0.85f, 0.90f, 1.f), TEXT("WebsiteButton"));
	AddLinkButton(DiscBtn, DiscordLabel, TEXT("  DISCORD  "),
		FLinearColor(0.35f, 0.40f, 0.95f, 0.95f), FLinearColor(0.95f, 0.96f, 1.f), TEXT("DiscordButton"));   // Discord blurple
	AddLinkButton(DonBtn, DonateLabel, TEXT("  BUY ME A COFFEE  "),
		FLinearColor(1.f, 0.72f, 0.12f, 0.95f), FLinearColor(0.12f, 0.10f, 0.06f), TEXT("DonateButton"));   // BMC warm yellow
	WebsiteButton = WebBtn;
	DiscordButton = DiscBtn;
	DonateButton = DonBtn;
	WebsiteButton->OnClicked.AddDynamic(this, &UPFLoadingMenuWidget::OnWebsiteClicked);
	DiscordButton->OnClicked.AddDynamic(this, &UPFLoadingMenuWidget::OnDiscordClicked);
	DonateButton->OnClicked.AddDynamic(this, &UPFLoadingMenuWidget::OnDonateClicked);
	if (UCanvasPanelSlot* S = Root->AddChildToCanvas(LinksRow))
	{
		S->SetAnchors(FAnchors(0.5f, 1.f, 0.5f, 1.f));
		S->SetAlignment(FVector2D(0.5f, 1.f));
		S->SetPosition(FVector2D(0.f, -18.f));
		S->SetAutoSize(true);
		S->SetZOrder(2);
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
	if (UVerticalBoxSlot* V = RightCol->AddChildToVerticalBox(QuitDesktopButton))
	{
		V->SetHorizontalAlignment(HAlign_Fill);
		V->SetPadding(FMargin(0.f, 10.f, 0.f, 0.f));
	}

	// ---- Assemble the two columns ----
	UHorizontalBox* BodyRow = WidgetTree->ConstructWidget<UHorizontalBox>();
	if (UHorizontalBoxSlot* H = BodyRow->AddChildToHorizontalBox(LeftCol))
	{
		H->SetPadding(FMargin(0.f, 0.f, 28.f, 0.f));
		H->SetVerticalAlignment(VAlign_Top);
	}
	USizeBox* RightSizer = WidgetTree->ConstructWidget<USizeBox>();
	RightSizer->SetWidthOverride(320.f);
	RightSizer->SetContent(RightCol);
	if (UHorizontalBoxSlot* H = BodyRow->AddChildToHorizontalBox(RightSizer))
	{
		H->SetVerticalAlignment(VAlign_Top);
	}
	if (UVerticalBoxSlot* V = Col->AddChildToVerticalBox(BodyRow))
	{
		V->SetHorizontalAlignment(HAlign_Center);
	}

	// Wrap the whole menu in a full-viewport scroll box: content stays top-anchored (so clicking tabs never
	// shifts the layout — the old "jumping" bug) and long pages (e.g. Play-Only + community map picker) scroll
	// so the START GAME button is always reachable instead of falling off the bottom.
	UScrollBox* MenuScroll = WidgetTree->ConstructWidget<UScrollBox>();
	MenuScroll->AddChild(Col);
	if (UCanvasPanelSlot* S = Root->AddChildToCanvas(MenuScroll))
	{
		S->SetAnchors(FAnchors(0.f, 0.f, 1.f, 1.f));
		// Reserve a bottom band (90px) so scrolled content — the last item is START GAME / QUIT — can never
		// slide UNDER the floating LinksRow (WEBSITE / DISCORD / COFFEE, ZOrder 2, pinned bottom-center). Before,
		// at the bottom of a long page START GAME rendered in the same band and the higher-ZOrder link buttons
		// stole its clicks (offset/dead clickbox). The links stay pinned + clickable; content now stops above them.
		S->SetOffsets(FMargin(0.f, 24.f, 0.f, 90.f));
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

	// Backend account events → the ONLINE panel (status line + button states).
	if (UPFBackendSubsystem* Backend = GetBackend())
	{
		Backend->OnAuthChanged.AddUObject(this, &UPFLoadingMenuWidget::RefreshOnlinePanel);
		Backend->OnStatus.AddWeakLambda(this, [this](const FString& Line)
		{
			SetStatus(Line);
			RefreshOnlinePanel();
		});
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
	UE_LOG(CombatForgeLog, Log, TEXT("LoadingMenu: boot menu up — warmup starting"));
}

void UPFLoadingMenuWidget::NativeDestruct()
{
	// Tear down the off-screen preview studio with the menu.
	if (CharPreviewActor != nullptr)
	{
		CharPreviewActor->Destroy();
		CharPreviewActor = nullptr;
	}
	if (UPFBackendSubsystem* Backend = GetBackend())
	{
		Backend->OnAuthChanged.RemoveAll(this);
		Backend->OnStatus.RemoveAll(this);
	}
	Super::NativeDestruct();
}

FReply UPFLoadingMenuWidget::NativeOnMouseButtonDown(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent)
{
	// Start a rotate-drag only if the press lands on the character preview.
	if ((ActiveMenuTab == 2 || ActiveMenuTab == 3) && CharPreviewActor != nullptr
		&& InMouseEvent.GetEffectingButton() == EKeys::LeftMouseButton)
	{
		UImage* Img = (ActiveMenuTab == 2) ? LoadoutPreviewImage.Get() : CharPreviewImage.Get();
		if (Img != nullptr)
		{
			const FGeometry& ImgGeo = Img->GetCachedGeometry();
			if (ImgGeo.IsUnderLocation(InMouseEvent.GetScreenSpacePosition()))
			{
				bPreviewDragging = true;
				PreviewDragLastX = InMouseEvent.GetScreenSpacePosition().X;
				return FReply::Handled().CaptureMouse(TakeWidget());
			}
		}
	}
	return Super::NativeOnMouseButtonDown(InGeometry, InMouseEvent);
}

FReply UPFLoadingMenuWidget::NativeOnMouseMove(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent)
{
	if (bPreviewDragging && CharPreviewActor != nullptr)
	{
		const float X = InMouseEvent.GetScreenSpacePosition().X;
		const float Dx = X - PreviewDragLastX;
		PreviewDragLastX = X;
		CharPreviewActor->AddYaw(-Dx * 0.5f);   // drag right spins the character right-to-left (natural grab)
		return FReply::Handled();
	}
	return Super::NativeOnMouseMove(InGeometry, InMouseEvent);
}

FReply UPFLoadingMenuWidget::NativeOnMouseButtonUp(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent)
{
	if (bPreviewDragging && InMouseEvent.GetEffectingButton() == EKeys::LeftMouseButton)
	{
		bPreviewDragging = false;
		return FReply::Handled().ReleaseMouseCapture();
	}
	return Super::NativeOnMouseButtonUp(InGeometry, InMouseEvent);
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
	const bool bHostNow = IsLocalHost();
	if (!bHostNow)
	{
		SeedFromGameState();
		RefreshSetupLabels();
		RefreshMapPicker();
	}
	// Leadership can arrive AFTER construction (dedicated: MatchLeader replicates in, or the old
	// leader left and the crown migrated here). Re-style the leader-only rows exactly once per flip.
	if (bHostNow != bWasHostLastTick)
	{
		bWasHostLastTick = bHostNow;
		RefreshSetupLabels();
		RefreshMapPicker();
		if (QuickStartButton)
		{
			QuickStartButton->SetIsEnabled(bHostNow);
		}
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
	// Match-leader aware (dedicated servers): the PC predicate covers listen host AND the
	// replicated MatchLeader, so leader clients get the full match-config surface.
	const ACombatForgePlayerController* PC = Cast<ACombatForgePlayerController>(GetOwningPlayer());
	return PC && PC->IsHostController();
}

void UPFLoadingMenuWidget::SeedFromGameState()
{
	const UWorld* World = GetWorld();
	const ACombatForgeGameState* GS = World ? World->GetGameState<ACombatForgeGameState>() : nullptr;
	if (GS)
	{
		SelectedBuildMode = GS->BuildMode;
		SelectedMatchType = GS->MatchType;
		SelectedArenaMap = GS->ArenaMap;
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
	// Only surface maps that have a screenshot — hides picture-less arenas (incl. auto-regenerated seeds) so the
	// picker never shows a blank tile.
	MapCatalog.RemoveAll([](const FPFCommunityMapInfo& M)
	{
		FString PngName = M.FileName;
		PngName.RemoveFromEnd(TEXT(".json"));
		PngName += TEXT(".png");
		return !FPaths::FileExists(FPFPaths::ArenaDir() / PngName);
	});

	// ---- Favorites (host-side only; max 5, persisted in GameUserSettings.ini) ----
	// Favorites are global prefs but each shell only surfaces favorites that exist IN THIS catalog
	// (Warehouse vs Yard catalogs are independent). Never prune a favorite just because it belongs
	// to the other map — that would wipe Yard favorites while browsing Warehouse and vice versa.
	FavoriteIds = FPFUserPrefs::GetFavoriteMapIds();
	if (IsLocalHost() && FavoriteIds.Num() > 0)
	{
		// Favorites-first stable partition for maps that are valid on THIS shell. Preserve selection.
		FString SelectedFile;
		if (MapCatalog.IsValidIndex(SelectedMapCatalogIndex))
		{
			SelectedFile = MapCatalog[SelectedMapCatalogIndex].FileName;
		}
		TArray<FPFCommunityMapInfo> Partitioned;
		Partitioned.Reserve(MapCatalog.Num());
		for (const FPFCommunityMapInfo& M : MapCatalog) { if (IsFavorite(M))  { Partitioned.Add(M); } }
		for (const FPFCommunityMapInfo& M : MapCatalog) { if (!IsFavorite(M)) { Partitioned.Add(M); } }
		MapCatalog = MoveTemp(Partitioned);
		if (!SelectedFile.IsEmpty())
		{
			SelectedMapCatalogIndex = MapCatalog.IndexOfByPredicate(
				[&SelectedFile](const FPFCommunityMapInfo& M) { return M.FileName == SelectedFile; });
		}
	}

	const int32 PageCount = FMath::Max(1, FMath::DivideAndRoundUp(FMath::Max(MapCatalog.Num(), 1), MapsPerPage));
	MapPageIndex = FMath::Clamp(MapPageIndex, 0, PageCount - 1);
	if (SelectedMapCatalogIndex != INDEX_NONE && !MapCatalog.IsValidIndex(SelectedMapCatalogIndex))
	{
		SelectedMapCatalogIndex = INDEX_NONE;
	}
}

FString UPFLoadingMenuWidget::FavKeyFor(const FPFCommunityMapInfo& M)
{
	// Same identity rule the catalog dedupes by: content hash when present, else the file name.
	return M.ArenaId.IsEmpty() ? M.FileName : M.ArenaId;
}

bool UPFLoadingMenuWidget::IsFavorite(const FPFCommunityMapInfo& M) const
{
	return FavoriteIds.Contains(FavKeyFor(M));
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

	if (MapPickerHeader)
	{
		// Tag the shell so it's obvious Warehouse picks stay off The Yard and vice versa.
		const FString ShellTag = PFGetArenaMapDef(SelectedArenaMap).Label;
		int32 FavOnThisShell = 0;
		for (const FPFCommunityMapInfo& M : MapCatalog)
		{
			if (IsFavorite(M)) { ++FavOnThisShell; }
		}
		MapPickerHeader->SetText(FText::FromString((IsLocalHost() && FavOnThisShell > 0)
			? FString::Printf(TEXT("%s MAPS  ·  FAVORITES %d/%d"),
				*ShellTag, FavOnThisShell, FPFUserPrefs::MaxFavoriteMaps)
			: FString::Printf(TEXT("%s MAPS"), *ShellTag)));
	}

	if (MapPageLabel)
	{
		if (Total == 0)
		{
			MapPageLabel->SetText(FText::FromString(
				FString::Printf(TEXT("No %s maps yet"), *PFGetArenaMapDef(SelectedArenaMap).Label)));
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
		// Favorite star: hosts only (favorites never surface for joining clients).
		if (MapFavButtons.IsValidIndex(SlotIdx) && MapFavButtons[SlotIdx])
		{
			MapFavButtons[SlotIdx]->SetVisibility((bHost && bValid)
				? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
			MapFavButtons[SlotIdx]->SetIsEnabled(bHost);
		}
		if (bValid)
		{
			const FPFCommunityMapInfo& M = MapCatalog[CatalogIdx];
			if (MapFavGlyphs.IsValidIndex(SlotIdx) && MapFavGlyphs[SlotIdx])
			{
				MapFavGlyphs[SlotIdx]->SetColorAndOpacity(FSlateColor(IsFavorite(M)
					? FLinearColor(1.0f, 0.85f, 0.2f)      // gold = favorited
					: FLinearColor(0.40f, 0.42f, 0.48f))); // dim = not
			}
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
					// Brightening happens in the pixel data at load (GetMapPreview) — a >1.0 widget tint
					// is clamped by Slate and did nothing.
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
					// Tom: drop the "Click MODE/TYPE/FORMAT to cycle" helper — empty collapses the row.
					: TEXT(""))
				: TEXT("Host chooses match setup · waiting for Enter")));
	}
	// Non-host: still clickable visually but handlers no-op; dim slightly via background.
	const FLinearColor ActiveBg(0.12f, 0.13f, 0.16f, 0.95f);
	const FLinearColor DimBg(0.08f, 0.08f, 0.10f, 0.7f);
	const FLinearColor BtnBg = IsLocalHost() ? ActiveBg : DimBg;
	if (ModeButton)   { ModeButton->SetBackgroundColor(BtnBg); }
	if (TypeButton)   { TypeButton->SetBackgroundColor(BtnBg); }
	if (FormatButton) { FormatButton->SetBackgroundColor(BtnBg); }

	RefreshSetupCards();   // CoD card selectors replaced the steppers; keep them + the description in sync
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

void UPFLoadingMenuWidget::NotifyMapFavClicked(int32 SlotIndex)
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
	const FString Key = FavKeyFor(MapCatalog[CatalogIdx]);
	if (FavoriteIds.Remove(Key) == 0)
	{
		// Not a favorite yet — add, unless the 5-cap is hit (message auto-clears on next repaint).
		if (FavoriteIds.Num() >= FPFUserPrefs::MaxFavoriteMaps)
		{
			if (MapSelectedLabel)
			{
				MapSelectedLabel->SetText(FText::FromString(
					TEXT("Favorites full (5) — unstar one first")));
			}
			return;
		}
		FavoriteIds.Add(Key);
	}
	FPFUserPrefs::SetFavoriteMapIds(FavoriteIds);
	FPFUserPrefs::Flush();
	ReloadMapCatalog();    // re-partition favorites-first; the toggled map jumps pages immediately
	RefreshMapPicker();
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
	if (ACombatForgePlayerController* PC = Cast<ACombatForgePlayerController>(GetOwningPlayer()))
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
	SetStatus(TEXT("Quick Start ready — press START GAME when warm-up finishes."));
	if (QuickStartLabel)
	{
		QuickStartLabel->SetText(FText::FromString(TEXT("  AUTO MATCH SETUP — applied ✓  ")));
	}
	UE_LOG(CombatForgeLog, Log, TEXT("LoadingMenu: Quick Start preset applied"));
}

void UPFLoadingMenuWidget::OnHostLanClicked()
{
	// Relaunch the map as a LISTEN server: same solo flow, but LAN/VPN friends can now join this PC by IP.
	// (First host triggers the Windows Firewall prompt — allow it or nobody can connect.)
	if (APlayerController* PC = GetOwningPlayer())
	{
		UE_LOG(CombatForgeLog, Log, TEXT("Menu: hosting LAN listen server (join at %s)"), *GetLocalLanIp());
		PC->ConsoleCommand(TEXT("open L_Graybox?listen"));
	}
}

void UPFLoadingMenuWidget::OnStopHostingClicked()
{
	// Travel back to a plain standalone session: the net driver shuts down, joined clients are returned to
	// their own menus, and the multiplayer row shows HOST/JOIN again — anyone can host next.
	// Must NOT use ?listen — that was the bug that left the menu stuck "HOSTING" after a stop.
	if (APlayerController* PC = GetOwningPlayer())
	{
		UE_LOG(CombatForgeLog, Log, TEXT("Menu: stop hosting — returning to standalone"));
		PC->ConsoleCommand(TEXT("open L_Graybox"));
	}
}

void UPFLoadingMenuWidget::OnJoinLanClicked()
{
	FString Ip = JoinIpBox ? JoinIpBox->GetText().ToString().TrimStartAndEnd() : FString();
	if (Ip.IsEmpty())
	{
		SetStatus(TEXT("Type the host's IP first (they see it after clicking HOST)."));
		return;
	}
	FPFUserPrefs::SetLastJoinIp(Ip);
	FPFUserPrefs::Flush();
	if (APlayerController* PC = GetOwningPlayer())
	{
		UE_LOG(CombatForgeLog, Log, TEXT("Menu: joining LAN host %s"), *Ip);
		SetStatus(FString::Printf(TEXT("Connecting to %s…"), *Ip));
		PC->ConsoleCommand(FString::Printf(TEXT("open %s"), *Ip));
	}
}

// ---------------------------------------------------------------------------
// ONLINE panel (combatforge-api: account, quick play, browser, match codes)
// ---------------------------------------------------------------------------

UPFBackendSubsystem* UPFLoadingMenuWidget::GetBackend() const
{
	const UGameInstance* GI = GetGameInstance();
	return GI ? GI->GetSubsystem<UPFBackendSubsystem>() : nullptr;
}

void UPFLoadingMenuWidget::RefreshOnlinePanel()
{
	UPFBackendSubsystem* Backend = GetBackend();
	const bool bLoggedIn = Backend && Backend->IsLoggedIn();
	const bool bLinking = Backend && Backend->IsDeviceLoginActive();
	if (AccountStatusText)
	{
		FString Line;
		if (bLinking)
		{
			const FString& Uri = Backend->GetVerificationUri();
			Line = Backend->GetPendingUserCode().IsEmpty()
				? TEXT("Contacting the account server…")
				// New players: the destination page's "New here?" button makes an account in
				// seconds — say so up front so a first-timer doesn't wonder how to sign up.
				: FString::Printf(TEXT("Code %s — go to %s · new? sign up free when you get there"),
					*Backend->GetPendingUserCode(), Uri.IsEmpty() ? TEXT("the CombatForge website") : *Uri);
		}
		else if (bLoggedIn)
		{
			const FPFBackendProfile& P = Backend->GetProfile();
			// Between token grant and the profile fetch landing there is no name yet — say so
			// instead of flashing "Signed in as  · Level 1".
			Line = P.DisplayName.IsEmpty()
				? TEXT("Signed in — loading profile…")
				: FString::Printf(TEXT("Signed in as %s · Level %d"), *P.DisplayName, P.Level);
		}
		else
		{
			Line = TEXT("ONLINE — log in to browse hosted servers");
		}
		AccountStatusText->SetText(FText::FromString(Line));
		AccountStatusText->SetColorAndOpacity(FSlateColor(bLoggedIn
			? FLinearColor(0.45f, 0.9f, 0.5f) : FLinearColor(0.6f, 0.62f, 0.68f)));
	}
	if (LoginLabel)
	{
		// While the device flow runs the button is the CANCEL path — a second click must never
		// read as "log in again".
		LoginLabel->SetText(FText::FromString(
			bLinking ? TEXT("  CANCEL  ") : bLoggedIn ? TEXT("  LOG OUT  ") : TEXT("  LOG IN  ")));
	}
	if (QuickPlayButton)
	{
		QuickPlayButton->SetIsEnabled(bLoggedIn);
	}
	if (ServerListButton)
	{
		ServerListButton->SetIsEnabled(bLoggedIn);
	}
	if (JoinCodeButton)
	{
		JoinCodeButton->SetIsEnabled(bLoggedIn);
	}
	// Logout / session expiry with the browser open: stale rows must not stay joinable.
	if (!bLoggedIn && bServerListOpen)
	{
		bServerListOpen = false;
		BrowserRows.Reset();
		RebuildServerRows();
	}
}

void UPFLoadingMenuWidget::OnLoginClicked()
{
	UPFBackendSubsystem* Backend = GetBackend();
	if (!Backend)
	{
		return;
	}
	if (Backend->IsLoggedIn())
	{
		Backend->Logout();
	}
	else if (Backend->IsDeviceLoginActive())
	{
		Backend->CancelDeviceLogin();
	}
	else
	{
		Backend->BeginDeviceLogin();
	}
	RefreshOnlinePanel();
}

void UPFLoadingMenuWidget::OnQuickPlayClicked()
{
	UPFBackendSubsystem* Backend = GetBackend();
	if (!Backend || !Backend->IsLoggedIn())
	{
		return;
	}
	SetStatus(TEXT("Finding a match…"));
	TWeakObjectPtr<UPFLoadingMenuWidget> WeakThis(this);
	Backend->RequestQuickPlay([WeakThis](bool bOk, const FPFBackendServerInfo& Info)
	{
		if (UPFLoadingMenuWidget* Self = WeakThis.Get())
		{
			if (bOk)
			{
				Self->JoinBackendServer(Info);
			}
			else
			{
				Self->SetStatus(TEXT("No open servers right now — try SERVERS or host a LAN game."));
			}
		}
	});
}

void UPFLoadingMenuWidget::OnServerListClicked()
{
	UPFBackendSubsystem* Backend = GetBackend();
	if (!Backend || !Backend->IsLoggedIn())
	{
		return;
	}
	// Toggle closed without a refetch; opening always refetches (10 s of staleness max matters
	// little at our scale, but a click should always show live data).
	if (bServerListOpen)
	{
		bServerListOpen = false;
		BrowserRows.Reset();
		RebuildServerRows();
		return;
	}
	SetStatus(TEXT("Fetching servers…"));
	TWeakObjectPtr<UPFLoadingMenuWidget> WeakThis(this);
	Backend->FetchServers([WeakThis](bool bOk, const TArray<FPFBackendServerInfo>& Servers)
	{
		UPFLoadingMenuWidget* Self = WeakThis.Get();
		if (!Self)
		{
			return;
		}
		if (!bOk)
		{
			Self->SetStatus(TEXT("Couldn't fetch the server list — try again."));
			return;
		}
		Self->bServerListOpen = true;
		Self->BrowserRows = Servers;
		Self->RebuildServerRows();
		Self->SetStatus(Servers.Num() > 0
			? FString::Printf(TEXT("%d server%s online — click one to join."),
				Servers.Num(), Servers.Num() == 1 ? TEXT("") : TEXT("s"))
			: FString(TEXT("No servers online right now.")));
	});
}

void UPFLoadingMenuWidget::RebuildServerRows()
{
	for (int32 RowIdx = 0; RowIdx < ServerRowButtons.Num(); ++RowIdx)
	{
		UButton* Row = ServerRowButtons[RowIdx];
		UTextBlock* Lab = ServerRowLabels.IsValidIndex(RowIdx) ? ServerRowLabels[RowIdx].Get() : nullptr;
		if (!Row)
		{
			continue;
		}
		if (bServerListOpen && BrowserRows.IsValidIndex(RowIdx))
		{
			const FPFBackendServerInfo& Info = BrowserRows[RowIdx];
			const bool bCompatible = Info.NetProtocol == PFBuild::NetProtocol;
			const bool bFull = Info.Players >= Info.MaxPlayers;
			if (Lab)
			{
				Lab->SetText(FText::FromString(FString::Printf(TEXT(" %s   %s · %s   %d/%d%s"),
					*Info.Name, *Info.Map, *Info.Mode, Info.Players, Info.MaxPlayers,
					!bCompatible ? TEXT("   (update needed)") : bFull ? TEXT("   (full)") : TEXT(""))));
			}
			Row->SetVisibility(ESlateVisibility::Visible);
			Row->SetIsEnabled(bCompatible && !bFull);
		}
		else
		{
			Row->SetVisibility(ESlateVisibility::Collapsed);
		}
	}
}

void UPFLoadingMenuWidget::OnServerRow0Clicked() { JoinBrowserRow(0); }
void UPFLoadingMenuWidget::OnServerRow1Clicked() { JoinBrowserRow(1); }
void UPFLoadingMenuWidget::OnServerRow2Clicked() { JoinBrowserRow(2); }
void UPFLoadingMenuWidget::OnServerRow3Clicked() { JoinBrowserRow(3); }
void UPFLoadingMenuWidget::OnServerRow4Clicked() { JoinBrowserRow(4); }
void UPFLoadingMenuWidget::OnServerRow5Clicked() { JoinBrowserRow(5); }

void UPFLoadingMenuWidget::JoinBrowserRow(int32 Index)
{
	if (BrowserRows.IsValidIndex(Index))
	{
		JoinBackendServer(BrowserRows[Index]);
	}
}

void UPFLoadingMenuWidget::JoinBackendServer(const FPFBackendServerInfo& Info)
{
	if (APlayerController* PC = GetOwningPlayer())
	{
		const FString Address = Info.JoinAddress();
		UE_LOG(CombatForgeLog, Log, TEXT("Menu: joining online server %s (%s)"), *Info.Name, *Address);
		SetStatus(FString::Printf(TEXT("Connecting to %s…"), *Info.Name));
		PC->ConsoleCommand(FString::Printf(TEXT("open %s"), *Address));
	}
}

void UPFLoadingMenuWidget::OnJoinCodeClicked()
{
	UPFBackendSubsystem* Backend = GetBackend();
	if (!Backend || !Backend->IsLoggedIn())
	{
		return;
	}
	const FString Code = JoinCodeBox ? JoinCodeBox->GetText().ToString().TrimStartAndEnd() : FString();
	if (Code.Len() != 6)
	{
		SetStatus(TEXT("Match codes are 6 characters (ask the match host)."));
		return;
	}
	SetStatus(TEXT("Looking up the match…"));
	TWeakObjectPtr<UPFLoadingMenuWidget> WeakThis(this);
	Backend->RequestJoinByCode(Code, [WeakThis](bool bOk, const FPFBackendServerInfo& Info)
	{
		if (UPFLoadingMenuWidget* Self = WeakThis.Get())
		{
			if (bOk)
			{
				Self->JoinBackendServer(Info);
			}
			else
			{
				Self->SetStatus(TEXT("No match with that code — check it with the host."));
			}
		}
	});
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
	if (ACombatForgePlayerController* PC = Cast<ACombatForgePlayerController>(GetOwningPlayer()))
	{
		PC->ServerHostSetBuildMode(static_cast<uint8>(SelectedBuildMode));
		PC->ServerHostSetMatchType(static_cast<uint8>(SelectedMatchType));
		PC->ServerHostSetFormat(SelectedTeamSize);
		PC->ServerHostSetFillWithBots(bSelectedFillBots);
		// Re-sent on every apply like the rest; the GameMode no-ops unless the map actually changed
		// (a shell respawn re-seats everyone — don't churn it from unrelated card clicks).
		PC->ServerHostSetArenaMap(static_cast<uint8>(SelectedArenaMap));
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
	if (ProgressBar)
	{
		ProgressBar->SetPercent(1.f);
		ProgressBar->SetFillColorAndOpacity(FLinearColor(0.35f, 0.85f, 0.4f));   // green = done warming up
	}
	SetStatus(TEXT("Ready — press START GAME to start."));
	if (EnterButton)
	{
		EnterButton->SetIsEnabled(true);
	}
	if (EnterLabel)
	{
		EnterLabel->SetText(FText::FromString(TEXT("START GAME")));
	}
	// Ensure host selection is on the server before anyone enters.
	ApplySelectionsToHost();
	UE_LOG(CombatForgeLog, Log,
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
	if (ACombatForgePlayerController* PC = Cast<ACombatForgePlayerController>(GetOwningPlayer()))
	{
		PC->NotifyLoadingMenuFinished();
	}

	UE_LOG(CombatForgeLog, Log,
		TEXT("LoadingMenu: dismissed — entering lobby (Mode=%s Type=%s Format=%s Bots=%s)"),
		*BuildModeLabel(SelectedBuildMode), *MatchTypeLabel(SelectedMatchType),
		*FormatLabel(SelectedTeamSize), bSelectedFillBots ? TEXT("on") : TEXT("off"));
}

void UPFLoadingMenuWidget::OnDonateClicked()
{
	// Opens the system default browser — no in-game web view, no network call from the game.
	FPlatformProcess::LaunchURL(TEXT("https://buymeacoffee.com/tomchapman"), nullptr, nullptr);
	UE_LOG(CombatForgeLog, Log, TEXT("LoadingMenu: opened donate page (buymeacoffee.com/tomchapman)"));
}

void UPFLoadingMenuWidget::OnWebsiteClicked()
{
	FPlatformProcess::LaunchURL(TEXT("https://playcombatforge.com/"), nullptr, nullptr);
	UE_LOG(CombatForgeLog, Log, TEXT("LoadingMenu: opened website (playcombatforge.com)"));
}

void UPFLoadingMenuWidget::OnDiscordClicked()
{
	FPlatformProcess::LaunchURL(TEXT("https://discord.gg/f7U2xXxAxc"), nullptr, nullptr);
	UE_LOG(CombatForgeLog, Log, TEXT("LoadingMenu: opened Discord invite"));
}

void UPFLoadingMenuWidget::OnQuitDesktopClicked()
{
	if (ACombatForgePlayerController* PC = Cast<ACombatForgePlayerController>(GetOwningPlayer()))
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
