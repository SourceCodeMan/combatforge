// Copyright (c) 2026 Tom Chapman. All rights reserved.

#include "UI/PFOptionsWidget.h"
#include "Core/PFPaths.h"

#include "CombatForge.h"
#include "Core/CombatForgePlayerController.h"
#include "Core/PFHandheldPlatform.h"
#include "Core/PFUserPrefs.h"
#include "Input/PFInputConfig.h"
#include "EnhancedInputSubsystems.h"
#include "Engine/LocalPlayer.h"
#include "InputCoreTypes.h"
#include "Player/CombatForgeCharacter.h"
#include "Player/PFCharacterCustomization.h"   // PFChar active class-slot switch (in-game)
#include "Audio/PFMusicSubsystem.h"
#include "Combat/PFCombatAudio.h"
#include "Core/CombatForgeGameState.h"
#include "Core/PFLightingSubsystem.h"
#include "UI/PFCycleBar.h"

#include "AudioDevice.h"
#include "Blueprint/WidgetTree.h"
#include "Brushes/SlateColorBrush.h"
#include "Components/Button.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/CheckBox.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/Image.h"
#include "Components/SizeBox.h"
#include "Components/ScrollBox.h"
#include "Components/Slider.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Components/WidgetSwitcher.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "GameFramework/GameUserSettings.h"
#include "GameFramework/PlayerController.h"
#include "InputModifiers.h"
#include "Misc/ConfigCacheIni.h"
#include "Sound/SoundClass.h"
#include "Styling/CoreStyle.h"

namespace
{
	FSlateFontInfo PFOptFont(int32 Size, bool bBold)
	{
		return FCoreStyle::GetDefaultFontStyle(bBold ? FName(TEXT("Bold")) : FName(TEXT("Regular")), Size);
	}

	UTextBlock* MakeLabel(UWidgetTree* Tree, const FString& Text, int32 Size = 14, bool bBold = false)
	{
		UTextBlock* T = Tree->ConstructWidget<UTextBlock>();
		T->SetText(FText::FromString(Text));
		T->SetFont(PFOptFont(Size, bBold));
		T->SetColorAndOpacity(FSlateColor(FLinearColor(0.9f, 0.91f, 0.94f)));
		return T;
	}
}

TSharedRef<SWidget> UPFOptionsWidget::RebuildWidget()
{
	if (WidgetTree && !WidgetTree->RootWidget)
	{
		BuildTree();
	}
	return Super::RebuildWidget();
}

void UPFOptionsWidget::BuildTree()
{
	UCanvasPanel* Root = WidgetTree->ConstructWidget<UCanvasPanel>();
	WidgetTree->RootWidget = Root;

	Dimmer = WidgetTree->ConstructWidget<UImage>();
	Dimmer->SetBrush(FSlateColorBrush(FLinearColor::White));
	Dimmer->SetColorAndOpacity(FLinearColor(0.02f, 0.02f, 0.04f, 0.88f));
	if (UCanvasPanelSlot* S = Root->AddChildToCanvas(Dimmer))
	{
		S->SetAnchors(FAnchors(0.f, 0.f, 1.f, 1.f));
		S->SetOffsets(FMargin(0.f));
		S->SetZOrder(0);
	}

	// Center card.
	UVerticalBox* Card = WidgetTree->ConstructWidget<UVerticalBox>();

	TitleText = MakeLabel(WidgetTree, TEXT("OPTIONS"), 28, true);
	TitleText->SetJustification(ETextJustify::Center);
	TitleText->SetColorAndOpacity(FSlateColor(FLinearColor(1.f, 0.92f, 0.35f)));
	if (UVerticalBoxSlot* V = Card->AddChildToVerticalBox(TitleText))
	{
		V->SetHorizontalAlignment(HAlign_Center);
		V->SetPadding(FMargin(0.f, 0.f, 0.f, 6.f));
	}

	// Match-rotation strip: Esc mid-match answers "where are we in the 3-match wheel?"
	CycleBarRoot = PFCycleBar::Build(WidgetTree, CycleSegs, CycleTexts);
	if (UVerticalBoxSlot* V = Card->AddChildToVerticalBox(CycleBarRoot))
	{
		V->SetHorizontalAlignment(HAlign_Center);
		V->SetPadding(FMargin(0.f, 0.f, 0.f, 10.f));
	}

	// Tabs
	UHorizontalBox* Tabs = WidgetTree->ConstructWidget<UHorizontalBox>();
	TabVideo = MakeTabButton(TEXT("  VIDEO  "), TEXT("TabVideo"));
	TabAudio = MakeTabButton(TEXT("  AUDIO  "), TEXT("TabAudio"));
	TabControls = MakeTabButton(TEXT("  CONTROLS  "), TEXT("TabControls"));
	TabClass = MakeTabButton(TEXT("  CLASS  "), TEXT("TabClass"));
	TabHowTo = MakeTabButton(TEXT("  HOW TO PLAY  "), TEXT("TabHowTo"));
	TabVideo->OnClicked.AddDynamic(this, &UPFOptionsWidget::OnTabVideo);
	TabAudio->OnClicked.AddDynamic(this, &UPFOptionsWidget::OnTabAudio);
	TabControls->OnClicked.AddDynamic(this, &UPFOptionsWidget::OnTabControls);
	TabClass->OnClicked.AddDynamic(this, &UPFOptionsWidget::OnTabClass);
	TabHowTo->OnClicked.AddDynamic(this, &UPFOptionsWidget::OnTabHowTo);
	auto AddTab = [Tabs](UButton* B)
	{
		if (UHorizontalBoxSlot* H = Tabs->AddChildToHorizontalBox(B))
		{
			H->SetPadding(FMargin(4.f, 0.f));
		}
	};
	AddTab(TabVideo);
	AddTab(TabAudio);
	AddTab(TabControls);
	AddTab(TabClass);
	AddTab(TabHowTo);
	if (UVerticalBoxSlot* V = Card->AddChildToVerticalBox(Tabs))
	{
		V->SetHorizontalAlignment(HAlign_Center);
		V->SetPadding(FMargin(0.f, 0.f, 0.f, 18.f));
	}

	PageSwitcher = WidgetTree->ConstructWidget<UWidgetSwitcher>();
	UVerticalBox* VideoPage = WidgetTree->ConstructWidget<UVerticalBox>();
	UVerticalBox* AudioPage = WidgetTree->ConstructWidget<UVerticalBox>();
	UVerticalBox* ControlsPage = WidgetTree->ConstructWidget<UVerticalBox>();
	UVerticalBox* ClassPage = WidgetTree->ConstructWidget<UVerticalBox>();
	UVerticalBox* HowToPage = WidgetTree->ConstructWidget<UVerticalBox>();
	BuildVideoPage(VideoPage);
	BuildAudioPage(AudioPage);
	BuildControlsPage(ControlsPage);
	BuildClassPage(ClassPage);
	BuildHowToPlayPage(HowToPage);
	PageSwitcher->AddChild(VideoPage);       // 0
	PageSwitcher->AddChild(AudioPage);       // 1
	PageSwitcher->AddChild(ControlsPage);    // 2
	PageSwitcher->AddChild(ClassPage);       // 3
	PageSwitcher->AddChild(HowToPage);       // 4
	// Fixed page viewport: pin BOTH width and height so switching tabs can't grow/re-center the whole card.
	// Overflow (the tall How to Play page) scrolls inside a UScrollBox instead of resizing the menu frame.
	USizeBox* PageSizer = WidgetTree->ConstructWidget<USizeBox>();
	PageSizer->SetWidthOverride(560.f);
	PageSizer->SetHeightOverride(430.f);
	UScrollBox* PageScroll = WidgetTree->ConstructWidget<UScrollBox>();
	PageScroll->AddChild(PageSwitcher);
	PageSizer->SetContent(PageScroll);
	if (UVerticalBoxSlot* V = Card->AddChildToVerticalBox(PageSizer))
	{
		V->SetHorizontalAlignment(HAlign_Center);
		V->SetPadding(FMargin(8.f, 0.f, 8.f, 20.f));
	}

	// Footer buttons
	UHorizontalBox* Footer = WidgetTree->ConstructWidget<UHorizontalBox>();
	ApplyButton = MakeTabButton(TEXT("  APPLY  "), TEXT("ApplyBtn"));
	BackButton = MakeTabButton(TEXT("  BACK  "), TEXT("BackBtn"));
	QuitMenuButton = MakeTabButton(TEXT("  QUIT TO MENU  "), TEXT("QuitMenuBtn"));
	QuitDesktopButton = MakeTabButton(TEXT("  QUIT TO DESKTOP  "), TEXT("QuitDeskBtn"));
	ResetSpawnButton = MakeTabButton(TEXT("  RESET TO SPAWN  "), TEXT("ResetSpawnBtn"));
	ApplyButton->OnClicked.AddDynamic(this, &UPFOptionsWidget::OnApplyClicked);
	BackButton->OnClicked.AddDynamic(this, &UPFOptionsWidget::OnBackClicked);
	QuitMenuButton->OnClicked.AddDynamic(this, &UPFOptionsWidget::OnQuitToMenuClicked);
	QuitDesktopButton->OnClicked.AddDynamic(this, &UPFOptionsWidget::OnQuitToDesktopClicked);
	ResetSpawnButton->OnClicked.AddDynamic(this, &UPFOptionsWidget::OnResetToSpawnClicked);
	if (UHorizontalBoxSlot* H = Footer->AddChildToHorizontalBox(ApplyButton))
	{
		H->SetPadding(FMargin(6.f));
	}
	if (UHorizontalBoxSlot* H = Footer->AddChildToHorizontalBox(BackButton))
	{
		H->SetPadding(FMargin(6.f));
	}
	if (UHorizontalBoxSlot* H = Footer->AddChildToHorizontalBox(ResetSpawnButton))
	{
		H->SetPadding(FMargin(6.f));
	}
	if (UHorizontalBoxSlot* H = Footer->AddChildToHorizontalBox(QuitMenuButton))
	{
		H->SetPadding(FMargin(6.f));
	}
	if (UHorizontalBoxSlot* H = Footer->AddChildToHorizontalBox(QuitDesktopButton))
	{
		H->SetPadding(FMargin(6.f));
	}
	if (UVerticalBoxSlot* V = Card->AddChildToVerticalBox(Footer))
	{
		V->SetHorizontalAlignment(HAlign_Center);
	}

	HintText = MakeLabel(WidgetTree, TEXT("Esc closes · Apply saves · Quit to Menu leaves the match"), 12, false);
	HintText->SetColorAndOpacity(FSlateColor(FLinearColor(0.55f, 0.55f, 0.6f)));
	HintText->SetJustification(ETextJustify::Center);
	if (UVerticalBoxSlot* V = Card->AddChildToVerticalBox(HintText))
	{
		V->SetHorizontalAlignment(HAlign_Center);
		V->SetPadding(FMargin(0.f, 14.f, 0.f, 0.f));
	}

	if (UCanvasPanelSlot* S = Root->AddChildToCanvas(Card))
	{
		S->SetAnchors(FAnchors(0.5f, 0.5f));
		S->SetAlignment(FVector2D(0.5f, 0.5f));
		S->SetAutoSize(true);
		S->SetZOrder(1);
	}
}

UButton* UPFOptionsWidget::MakeTabButton(const FString& Label, FName Name)
{
	UButton* B = WidgetTree->ConstructWidget<UButton>(UButton::StaticClass(), Name);
	UTextBlock* T = MakeLabel(WidgetTree, Label, 14, true);
	T->SetColorAndOpacity(FSlateColor(FLinearColor(0.12f, 0.12f, 0.14f)));
	B->AddChild(T);
	return B;
}

void UPFOptionsWidget::BuildVideoPage(UWidget* ParentBox)
{
	UVerticalBox* Box = CastChecked<UVerticalBox>(ParentBox);

	// Window mode
	UHorizontalBox* WmRow = WidgetTree->ConstructWidget<UHorizontalBox>();
	WmRow->AddChildToHorizontalBox(MakeLabel(WidgetTree, TEXT("Window mode"), 15, false));
	WindowModeButton = MakeTabButton(TEXT("  Fullscreen  "), TEXT("WinModeBtn"));
	WindowModeValueText = Cast<UTextBlock>(WindowModeButton->GetChildAt(0));
	WindowModeButton->OnClicked.AddDynamic(this, &UPFOptionsWidget::OnWindowModeClicked);
	if (UHorizontalBoxSlot* H = WmRow->AddChildToHorizontalBox(WindowModeButton))
	{
		H->SetPadding(FMargin(16.f, 0.f, 0.f, 0.f));
	}
	if (UVerticalBoxSlot* V = Box->AddChildToVerticalBox(WmRow))
	{
		V->SetPadding(FMargin(0.f, 6.f));
	}

	// Resolution
	UHorizontalBox* ResRow = WidgetTree->ConstructWidget<UHorizontalBox>();
	ResRow->AddChildToHorizontalBox(MakeLabel(WidgetTree, TEXT("Resolution"), 15, false));
	ResolutionButton = MakeTabButton(TEXT("  1920x1080  "), TEXT("ResBtn"));
	ResolutionValueText = Cast<UTextBlock>(ResolutionButton->GetChildAt(0));
	ResolutionButton->OnClicked.AddDynamic(this, &UPFOptionsWidget::OnResolutionClicked);
	if (UHorizontalBoxSlot* H = ResRow->AddChildToHorizontalBox(ResolutionButton))
	{
		H->SetPadding(FMargin(16.f, 0.f, 0.f, 0.f));
	}
	if (UVerticalBoxSlot* V = Box->AddChildToVerticalBox(ResRow))
	{
		V->SetPadding(FMargin(0.f, 6.f));
	}

	// FPS limit — capped by default (144): uncapped just cooks the GPU rendering frames nobody sees.
	UHorizontalBox* FpsRow = WidgetTree->ConstructWidget<UHorizontalBox>();
	FpsRow->AddChildToHorizontalBox(MakeLabel(WidgetTree, TEXT("FPS limit"), 15, false));
	FpsLimitButton = MakeTabButton(TEXT("  144  "), TEXT("FpsLimitBtn"));
	FpsLimitValueText = Cast<UTextBlock>(FpsLimitButton->GetChildAt(0));
	FpsLimitButton->OnClicked.AddDynamic(this, &UPFOptionsWidget::OnFpsLimitClicked);
	if (UHorizontalBoxSlot* H = FpsRow->AddChildToHorizontalBox(FpsLimitButton))
	{
		H->SetPadding(FMargin(16.f, 0.f, 0.f, 0.f));
	}
	if (UVerticalBoxSlot* V = Box->AddChildToVerticalBox(FpsRow))
	{
		V->SetPadding(FMargin(0.f, 6.f));
	}

	// Legacy fullscreen check still maps to window mode 0
	UHorizontalBox* FsRow = WidgetTree->ConstructWidget<UHorizontalBox>();
	FsRow->AddChildToHorizontalBox(MakeLabel(WidgetTree, TEXT("Fullscreen"), 15, false));
	FullscreenCheck = WidgetTree->ConstructWidget<UCheckBox>();
	FullscreenCheck->OnCheckStateChanged.AddDynamic(this, &UPFOptionsWidget::OnFullscreenChanged);
	if (UHorizontalBoxSlot* H = FsRow->AddChildToHorizontalBox(FullscreenCheck))
	{
		H->SetPadding(FMargin(16.f, 0.f, 0.f, 0.f));
		H->SetVerticalAlignment(VAlign_Center);
	}
	if (UVerticalBoxSlot* V = Box->AddChildToVerticalBox(FsRow))
	{
		V->SetPadding(FMargin(0.f, 6.f));
	}

	UHorizontalBox* VsRow = WidgetTree->ConstructWidget<UHorizontalBox>();
	VsRow->AddChildToHorizontalBox(MakeLabel(WidgetTree, TEXT("VSync"), 15, false));
	VSyncCheck = WidgetTree->ConstructWidget<UCheckBox>();
	VSyncCheck->OnCheckStateChanged.AddDynamic(this, &UPFOptionsWidget::OnVSyncChanged);
	if (UHorizontalBoxSlot* H = VsRow->AddChildToHorizontalBox(VSyncCheck))
	{
		H->SetPadding(FMargin(16.f, 0.f, 0.f, 0.f));
		H->SetVerticalAlignment(VAlign_Center);
	}
	if (UVerticalBoxSlot* V = Box->AddChildToVerticalBox(VsRow))
	{
		V->SetPadding(FMargin(0.f, 6.f));
	}

	UHorizontalBox* QRow = WidgetTree->ConstructWidget<UHorizontalBox>();
	QRow->AddChildToHorizontalBox(MakeLabel(WidgetTree, TEXT("Quality"), 15, false));
	QualityButton = MakeTabButton(TEXT("  Medium  "), TEXT("QualityBtn"));
	QualityValueText = Cast<UTextBlock>(QualityButton->GetChildAt(0));
	QualityButton->OnClicked.AddDynamic(this, &UPFOptionsWidget::OnQualityClicked);
	if (UHorizontalBoxSlot* H = QRow->AddChildToHorizontalBox(QualityButton))
	{
		H->SetPadding(FMargin(16.f, 0.f, 0.f, 0.f));
	}
	if (UVerticalBoxSlot* V = Box->AddChildToVerticalBox(QRow))
	{
		V->SetPadding(FMargin(0.f, 6.f));
	}

	UHorizontalBox* RsRow = WidgetTree->ConstructWidget<UHorizontalBox>();
	RsRow->AddChildToHorizontalBox(MakeLabel(WidgetTree, TEXT("Resolution scale"), 15, false));
	ResScaleSlider = WidgetTree->ConstructWidget<USlider>();
	ResScaleSlider->SetMinValue(50.f);
	ResScaleSlider->SetMaxValue(100.f);
	ResScaleSlider->SetStepSize(5.f);
	ResScaleSlider->OnValueChanged.AddDynamic(this, &UPFOptionsWidget::OnResScaleChanged);
	if (UHorizontalBoxSlot* H = RsRow->AddChildToHorizontalBox(ResScaleSlider))
	{
		H->SetPadding(FMargin(16.f, 0.f));
		H->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
	}
	ResScaleValueText = MakeLabel(WidgetTree, TEXT("100%"), 14, true);
	RsRow->AddChildToHorizontalBox(ResScaleValueText);
	if (UVerticalBoxSlot* V = Box->AddChildToVerticalBox(RsRow))
	{
		V->SetPadding(FMargin(0.f, 10.f));
	}

	// UI scale — Slate ApplicationScale. Handhelds default it up (7-8" panels); live-applies.
	UHorizontalBox* UiRow = WidgetTree->ConstructWidget<UHorizontalBox>();
	UiRow->AddChildToHorizontalBox(MakeLabel(WidgetTree, TEXT("UI scale"), 15, false));
	UIScaleSlider = WidgetTree->ConstructWidget<USlider>();
	UIScaleSlider->SetMinValue(0.85f);
	UIScaleSlider->SetMaxValue(1.3f);
	UIScaleSlider->SetStepSize(0.05f);
	UIScaleSlider->OnValueChanged.AddDynamic(this, &UPFOptionsWidget::OnUIScaleChanged);
	if (UHorizontalBoxSlot* H = UiRow->AddChildToHorizontalBox(UIScaleSlider))
	{
		H->SetPadding(FMargin(16.f, 0.f));
		H->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
	}
	UIScaleValueText = MakeLabel(WidgetTree, TEXT("100%"), 14, true);
	UiRow->AddChildToHorizontalBox(UIScaleValueText);
	if (UVerticalBoxSlot* V = Box->AddChildToVerticalBox(UiRow))
	{
		V->SetPadding(FMargin(0.f, 10.f));
	}

	// Brightness — EV offset on the match lighting rig's frozen base exposure (live-applies).
	UHorizontalBox* BrRow = WidgetTree->ConstructWidget<UHorizontalBox>();
	BrRow->AddChildToHorizontalBox(MakeLabel(WidgetTree, TEXT("Brightness"), 15, false));
	BrightnessSlider = WidgetTree->ConstructWidget<USlider>();
	BrightnessSlider->SetMinValue(-1.f);
	BrightnessSlider->SetMaxValue(1.f);
	BrightnessSlider->SetStepSize(0.05f);
	BrightnessSlider->OnValueChanged.AddDynamic(this, &UPFOptionsWidget::OnBrightnessChanged);
	if (UHorizontalBoxSlot* H = BrRow->AddChildToHorizontalBox(BrightnessSlider))
	{
		H->SetPadding(FMargin(16.f, 0.f));
		H->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
	}
	BrightnessValueText = MakeLabel(WidgetTree, TEXT("+0.00 EV"), 14, true);
	BrRow->AddChildToHorizontalBox(BrightnessValueText);
	if (UVerticalBoxSlot* V = Box->AddChildToVerticalBox(BrRow))
	{
		V->SetPadding(FMargin(0.f, 6.f));
	}

	// Contrast — scales the rig's base contrast (live-applies).
	UHorizontalBox* CtRow = WidgetTree->ConstructWidget<UHorizontalBox>();
	CtRow->AddChildToHorizontalBox(MakeLabel(WidgetTree, TEXT("Contrast"), 15, false));
	ContrastSlider = WidgetTree->ConstructWidget<USlider>();
	ContrastSlider->SetMinValue(0.85f);
	ContrastSlider->SetMaxValue(1.2f);
	ContrastSlider->SetStepSize(0.01f);
	ContrastSlider->OnValueChanged.AddDynamic(this, &UPFOptionsWidget::OnContrastChanged);
	if (UHorizontalBoxSlot* H = CtRow->AddChildToHorizontalBox(ContrastSlider))
	{
		H->SetPadding(FMargin(16.f, 0.f));
		H->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
	}
	ContrastValueText = MakeLabel(WidgetTree, TEXT("1.00"), 14, true);
	CtRow->AddChildToHorizontalBox(ContrastValueText);
	if (UVerticalBoxSlot* V = Box->AddChildToVerticalBox(CtRow))
	{
		V->SetPadding(FMargin(0.f, 6.f));
	}
}

void UPFOptionsWidget::BuildAudioPage(UWidget* ParentBox)
{
	UVerticalBox* Box = CastChecked<UVerticalBox>(ParentBox);

	// NOTE: AddDynamic is a macro that stringifies the member path — it MUST be a
	// literal &UClass::UFunction, never a member-function-pointer variable (that
	// asserts: "'Handler' does not look like a member function").

	// Master
	{
		UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>();
		Row->AddChildToHorizontalBox(MakeLabel(WidgetTree, TEXT("Master volume"), 15, false));
		MasterVolSlider = WidgetTree->ConstructWidget<USlider>();
		MasterVolSlider->SetMinValue(0.f);
		MasterVolSlider->SetMaxValue(1.f);
		MasterVolSlider->SetStepSize(0.05f);
		MasterVolSlider->OnValueChanged.AddDynamic(this, &UPFOptionsWidget::OnMasterVolChanged);
		if (UHorizontalBoxSlot* H = Row->AddChildToHorizontalBox(MasterVolSlider))
		{
			H->SetPadding(FMargin(16.f, 0.f));
			H->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		}
		MasterVolValueText = MakeLabel(WidgetTree, TEXT("100%"), 14, true);
		Row->AddChildToHorizontalBox(MasterVolValueText);
		if (UVerticalBoxSlot* V = Box->AddChildToVerticalBox(Row))
		{
			V->SetPadding(FMargin(0.f, 10.f));
		}
	}

	// SFX
	{
		UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>();
		Row->AddChildToHorizontalBox(MakeLabel(WidgetTree, TEXT("SFX volume"), 15, false));
		SfxVolSlider = WidgetTree->ConstructWidget<USlider>();
		SfxVolSlider->SetMinValue(0.f);
		SfxVolSlider->SetMaxValue(1.f);
		SfxVolSlider->SetStepSize(0.05f);
		SfxVolSlider->OnValueChanged.AddDynamic(this, &UPFOptionsWidget::OnSfxVolChanged);
		if (UHorizontalBoxSlot* H = Row->AddChildToHorizontalBox(SfxVolSlider))
		{
			H->SetPadding(FMargin(16.f, 0.f));
			H->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		}
		SfxVolValueText = MakeLabel(WidgetTree, TEXT("100%"), 14, true);
		Row->AddChildToHorizontalBox(SfxVolValueText);
		if (UVerticalBoxSlot* V = Box->AddChildToVerticalBox(Row))
		{
			V->SetPadding(FMargin(0.f, 10.f));
		}
	}

	// Background / phase music volume (also scales lobby wind bed).
	{
		UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>();
		Row->AddChildToHorizontalBox(MakeLabel(WidgetTree, TEXT("Music / ambience"), 15, false));
		AmbientVolSlider = WidgetTree->ConstructWidget<USlider>();
		AmbientVolSlider->SetMinValue(0.f);
		AmbientVolSlider->SetMaxValue(1.f);
		AmbientVolSlider->SetStepSize(0.05f);
		AmbientVolSlider->OnValueChanged.AddDynamic(this, &UPFOptionsWidget::OnAmbientVolChanged);
		if (UHorizontalBoxSlot* H = Row->AddChildToHorizontalBox(AmbientVolSlider))
		{
			H->SetPadding(FMargin(16.f, 0.f));
			H->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		}
		AmbientVolValueText = MakeLabel(WidgetTree, TEXT("100%"), 14, true);
		Row->AddChildToHorizontalBox(AmbientVolValueText);
		if (UVerticalBoxSlot* V = Box->AddChildToVerticalBox(Row))
		{
			V->SetPadding(FMargin(0.f, 10.f));
		}
	}

	UTextBlock* Note = MakeLabel(WidgetTree,
		TEXT("SFX = gunfire and menu sounds. Music / ambience = Build & Combat tracks + lobby wind."), 12, false);
	Note->SetColorAndOpacity(FSlateColor(FLinearColor(0.55f, 0.55f, 0.6f)));
	if (UVerticalBoxSlot* V = Box->AddChildToVerticalBox(Note))
	{
		V->SetPadding(FMargin(0.f, 8.f, 0.f, 0.f));
	}
}

void UPFOptionsWidget::BuildControlsPage(UWidget* ParentBox)
{
	UVerticalBox* Box = CastChecked<UVerticalBox>(ParentBox);

	UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>();
	Row->AddChildToHorizontalBox(MakeLabel(WidgetTree, TEXT("Mouse sensitivity"), 15, false));
	SensSlider = WidgetTree->ConstructWidget<USlider>();
	SensSlider->SetMinValue(0.2f);
	SensSlider->SetMaxValue(6.0f);   // Tom 2026-07-17: extend to 6 (same scale); the old max (3) is now the default
	SensSlider->SetStepSize(0.05f);
	SensSlider->OnValueChanged.AddDynamic(this, &UPFOptionsWidget::OnSensChanged);
	if (UHorizontalBoxSlot* H = Row->AddChildToHorizontalBox(SensSlider))
	{
		H->SetPadding(FMargin(16.f, 0.f));
		H->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
	}
	SensValueText = MakeLabel(WidgetTree, TEXT("1.00"), 14, true);
	Row->AddChildToHorizontalBox(SensValueText);
	if (UVerticalBoxSlot* V = Box->AddChildToVerticalBox(Row))
	{
		V->SetPadding(FMargin(0.f, 10.f));
	}

	// Gamepad right-stick look speed (multiplier on 220°/s yaw / 150°/s pitch).
	UHorizontalBox* PadRow = WidgetTree->ConstructWidget<UHorizontalBox>();
	PadRow->AddChildToHorizontalBox(MakeLabel(WidgetTree, TEXT("Gamepad look speed"), 15, false));
	GamepadSensSlider = WidgetTree->ConstructWidget<USlider>();
	GamepadSensSlider->SetMinValue(0.2f);
	GamepadSensSlider->SetMaxValue(3.0f);
	GamepadSensSlider->SetStepSize(0.05f);
	GamepadSensSlider->OnValueChanged.AddDynamic(this, &UPFOptionsWidget::OnGamepadSensChanged);
	if (UHorizontalBoxSlot* H = PadRow->AddChildToHorizontalBox(GamepadSensSlider))
	{
		H->SetPadding(FMargin(16.f, 0.f));
		H->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
	}
	GamepadSensValueText = MakeLabel(WidgetTree, TEXT("1.00"), 14, true);
	PadRow->AddChildToHorizontalBox(GamepadSensValueText);
	if (UVerticalBoxSlot* V = Box->AddChildToVerticalBox(PadRow))
	{
		V->SetPadding(FMargin(0.f, 10.f));
	}

	UHorizontalBox* InvRow = WidgetTree->ConstructWidget<UHorizontalBox>();
	InvRow->AddChildToHorizontalBox(MakeLabel(WidgetTree, TEXT("Invert Y"), 15, false));
	InvertYCheck = WidgetTree->ConstructWidget<UCheckBox>();
	InvertYCheck->OnCheckStateChanged.AddDynamic(this, &UPFOptionsWidget::OnInvertYChanged);
	if (UHorizontalBoxSlot* H = InvRow->AddChildToHorizontalBox(InvertYCheck))
	{
		H->SetPadding(FMargin(16.f, 0.f, 0.f, 0.f));
		H->SetVerticalAlignment(VAlign_Center);
	}
	if (UVerticalBoxSlot* V = Box->AddChildToVerticalBox(InvRow))
	{
		V->SetPadding(FMargin(0.f, 6.f));
	}

	// Aim style: checked = toggle ADS (press to enter/exit), unchecked = hold (default).
	UHorizontalBox* AdsRow = WidgetTree->ConstructWidget<UHorizontalBox>();
	AdsRow->AddChildToHorizontalBox(MakeLabel(WidgetTree, TEXT("Aim toggle (ADS)"), 15, false));
	ADSToggleCheck = WidgetTree->ConstructWidget<UCheckBox>();
	ADSToggleCheck->OnCheckStateChanged.AddDynamic(this, &UPFOptionsWidget::OnADSToggleChanged);
	if (UHorizontalBoxSlot* H = AdsRow->AddChildToHorizontalBox(ADSToggleCheck))
	{
		H->SetPadding(FMargin(16.f, 0.f, 0.f, 0.f));
		H->SetVerticalAlignment(VAlign_Center);
	}
	if (UVerticalBoxSlot* V = Box->AddChildToVerticalBox(AdsRow))
	{
		V->SetPadding(FMargin(0.f, 6.f));
	}

	// Crouch style: checked = toggle crouch (press to crouch/stand), unchecked = hold (default).
	UHorizontalBox* CrouchRow = WidgetTree->ConstructWidget<UHorizontalBox>();
	CrouchRow->AddChildToHorizontalBox(MakeLabel(WidgetTree, TEXT("Crouch toggle"), 15, false));
	CrouchToggleCheck = WidgetTree->ConstructWidget<UCheckBox>();
	CrouchToggleCheck->OnCheckStateChanged.AddDynamic(this, &UPFOptionsWidget::OnCrouchToggleChanged);
	if (UHorizontalBoxSlot* H = CrouchRow->AddChildToHorizontalBox(CrouchToggleCheck))
	{
		H->SetPadding(FMargin(16.f, 0.f, 0.f, 0.f));
		H->SetVerticalAlignment(VAlign_Center);
	}
	if (UVerticalBoxSlot* V = Box->AddChildToVerticalBox(CrouchRow))
	{
		V->SetPadding(FMargin(0.f, 6.f));
	}

	UHorizontalBox* FovRow = WidgetTree->ConstructWidget<UHorizontalBox>();
	FovRow->AddChildToHorizontalBox(MakeLabel(WidgetTree, TEXT("Field of view"), 15, false));
	FovSlider = WidgetTree->ConstructWidget<USlider>();
	FovSlider->SetMinValue(80.f);
	FovSlider->SetMaxValue(110.f);
	FovSlider->SetStepSize(1.f);
	FovSlider->OnValueChanged.AddDynamic(this, &UPFOptionsWidget::OnFovChanged);
	if (UHorizontalBoxSlot* H = FovRow->AddChildToHorizontalBox(FovSlider))
	{
		H->SetPadding(FMargin(16.f, 0.f));
		H->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
	}
	FovValueText = MakeLabel(WidgetTree, TEXT("105"), 14, true);
	FovRow->AddChildToHorizontalBox(FovValueText);
	if (UVerticalBoxSlot* V = Box->AddChildToVerticalBox(FovRow))
	{
		V->SetPadding(FMargin(0.f, 10.f));
	}

	UTextBlock* Note = MakeLabel(WidgetTree,
		TEXT("Sensitivity, Invert, Aim/Crouch toggle apply on Apply. FOV is hip FOV (ADS still zooms). Unchecked = hold."), 12, false);
	Note->SetColorAndOpacity(FSlateColor(FLinearColor(0.55f, 0.55f, 0.6f)));
	if (UVerticalBoxSlot* V = Box->AddChildToVerticalBox(Note))
	{
		V->SetPadding(FMargin(0.f, 8.f, 0.f, 0.f));
	}

	// Fixed gamepad layout (not rebindable yet) — spelled out so handheld players can
	// discover the controls without a glyph pass.
	UTextBlock* PadNote = MakeLabel(WidgetTree,
		TEXT("Gamepad: LS move · RS look · A jump · B crouch · L3 sprint · R3 punch · RT fire/place · LT aim\n")
		TEXT("X reload/use · Y fire mode/delete · RB frag/rotate · LB smoke/build wheel (hold) · D-pad: up ready,\n")
		TEXT("down plant bomb, left-right switch class/piece · View scoreboard · Menu back. In menus the pad is a\n")
		TEXT("pointer: LS moves, A clicks, B backs out, RS scrolls (lobby: D-pad down toggles walk-around)."), 12, false);
	PadNote->SetColorAndOpacity(FSlateColor(FLinearColor(0.55f, 0.55f, 0.6f)));
	if (UVerticalBoxSlot* V = Box->AddChildToVerticalBox(PadNote))
	{
		V->SetPadding(FMargin(0.f, 8.f, 0.f, 0.f));
	}

	BuildKeyBindRows(Box);
}

namespace
{
	struct FRebindDef { const TCHAR* Id; const TCHAR* Label; };
	// MUST match UPFInputConfig::BuildRebindRegistry ids/order.
	const FRebindDef GRebindDefs[] = {
		{ TEXT("Jump"),       TEXT("Jump") },
		{ TEXT("Sprint"),     TEXT("Sprint") },
		{ TEXT("Reload"),     TEXT("Reload") },
		{ TEXT("Interact"),   TEXT("Use / Refill") },
		{ TEXT("FireSelect"), TEXT("Fire Mode") },
		{ TEXT("ThrowFrag"),  TEXT("Throw Frag") },
		{ TEXT("ThrowSmoke"), TEXT("Throw Smoke") },
		{ TEXT("Punch"),      TEXT("Punch") },
	};
}

void UPFOptionsWidget::BuildKeyBindRows(UVerticalBox* Box)
{
	static_assert(UE_ARRAY_COUNT(GRebindDefs) == NumRebinds, "rebind table size must match NumRebinds");

	UTextBlock* Header = MakeLabel(WidgetTree, TEXT("KEY BINDINGS"), 15, true);
	Header->SetColorAndOpacity(FSlateColor(FLinearColor(0.8f, 0.85f, 1.f)));
	if (UVerticalBoxSlot* V = Box->AddChildToVerticalBox(Header))
	{
		V->SetPadding(FMargin(0.f, 18.f, 0.f, 4.f));
	}

	RebindButtons.Reset();
	RebindKeyLabels.Reset();
	RebindIds.Reset();

	for (int32 i = 0; i < NumRebinds; ++i)
	{
		RebindIds.Add(FName(GRebindDefs[i].Id));

		UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>();
		Row->AddChildToHorizontalBox(MakeLabel(WidgetTree, GRebindDefs[i].Label, 15, false));

		UButton* Btn = WidgetTree->ConstructWidget<UButton>();
		UTextBlock* KeyLbl = MakeLabel(WidgetTree, TEXT("--"), 14, true);
		Btn->SetContent(KeyLbl);
		// AddDynamic needs a literal &UClass::UFunction, so one explicit case per row.
		switch (i)
		{
		case 0: Btn->OnClicked.AddDynamic(this, &UPFOptionsWidget::OnRebind0); break;
		case 1: Btn->OnClicked.AddDynamic(this, &UPFOptionsWidget::OnRebind1); break;
		case 2: Btn->OnClicked.AddDynamic(this, &UPFOptionsWidget::OnRebind2); break;
		case 3: Btn->OnClicked.AddDynamic(this, &UPFOptionsWidget::OnRebind3); break;
		case 4: Btn->OnClicked.AddDynamic(this, &UPFOptionsWidget::OnRebind4); break;
		case 5: Btn->OnClicked.AddDynamic(this, &UPFOptionsWidget::OnRebind5); break;
		case 6: Btn->OnClicked.AddDynamic(this, &UPFOptionsWidget::OnRebind6); break;
		case 7: Btn->OnClicked.AddDynamic(this, &UPFOptionsWidget::OnRebind7); break;
		default: break;
		}
		if (UHorizontalBoxSlot* H = Row->AddChildToHorizontalBox(Btn))
		{
			H->SetPadding(FMargin(16.f, 0.f, 0.f, 0.f));
			H->SetHorizontalAlignment(HAlign_Right);
			H->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		}
		RebindButtons.Add(Btn);
		RebindKeyLabels.Add(KeyLbl);

		if (UVerticalBoxSlot* V = Box->AddChildToVerticalBox(Row))
		{
			V->SetPadding(FMargin(0.f, 4.f));
		}
	}

	ResetBindsButton = MakeTabButton(TEXT("  RESET KEYS  "), TEXT("ResetBindsBtn"));
	ResetBindsButton->OnClicked.AddDynamic(this, &UPFOptionsWidget::OnResetBinds);
	if (UVerticalBoxSlot* V = Box->AddChildToVerticalBox(ResetBindsButton))
	{
		V->SetPadding(FMargin(0.f, 10.f, 0.f, 0.f));
		V->SetHorizontalAlignment(HAlign_Center);
	}

	UTextBlock* Hint = MakeLabel(WidgetTree, TEXT("Click a key, then press a new key. Esc cancels."), 12, false);
	Hint->SetColorAndOpacity(FSlateColor(FLinearColor(0.55f, 0.55f, 0.6f)));
	if (UVerticalBoxSlot* V = Box->AddChildToVerticalBox(Hint))
	{
		V->SetPadding(FMargin(0.f, 6.f, 0.f, 0.f));
	}
}

void UPFOptionsWidget::OnRebind0() { BeginListen(0); }
void UPFOptionsWidget::OnRebind1() { BeginListen(1); }
void UPFOptionsWidget::OnRebind2() { BeginListen(2); }
void UPFOptionsWidget::OnRebind3() { BeginListen(3); }
void UPFOptionsWidget::OnRebind4() { BeginListen(4); }
void UPFOptionsWidget::OnRebind5() { BeginListen(5); }
void UPFOptionsWidget::OnRebind6() { BeginListen(6); }
void UPFOptionsWidget::OnRebind7() { BeginListen(7); }

void UPFOptionsWidget::BeginListen(int32 Index)
{
	if (Index < 0 || Index >= RebindKeyLabels.Num())
	{
		return;
	}
	bListeningForKey = true;
	ListeningIndex = Index;
	if (RebindKeyLabels[Index])
	{
		RebindKeyLabels[Index]->SetText(FText::FromString(TEXT("press key...")));
	}
	SetKeyboardFocus();
}

FReply UPFOptionsWidget::NativeOnKeyDown(const FGeometry& InGeometry, const FKeyEvent& InKeyEvent)
{
	if (bListeningForKey && RebindIds.IsValidIndex(ListeningIndex))
	{
		const FKey K = InKeyEvent.GetKey();
		if (K == EKeys::Escape)
		{
			bListeningForKey = false;
			ListeningIndex = -1;
			RefreshRebindLabels();
			return FReply::Handled();
		}
		if (K.IsValid() && !K.IsMouseButton() && K != EKeys::AnyKey)
		{
			WorkingBinds.Add(RebindIds[ListeningIndex], K);
			bListeningForKey = false;
			ListeningIndex = -1;
			RefreshRebindLabels();
			return FReply::Handled();
		}
		return FReply::Handled();   // swallow anything else while capturing
	}
	return Super::NativeOnKeyDown(InGeometry, InKeyEvent);
}

void UPFOptionsWidget::RefreshRebindLabels()
{
	for (int32 i = 0; i < RebindKeyLabels.Num() && i < RebindIds.Num(); ++i)
	{
		if (!RebindKeyLabels[i])
		{
			continue;
		}
		if (bListeningForKey && i == ListeningIndex)
		{
			RebindKeyLabels[i]->SetText(FText::FromString(TEXT("press key...")));
			continue;
		}
		const FKey* K = WorkingBinds.Find(RebindIds[i]);
		RebindKeyLabels[i]->SetText((K && K->IsValid()) ? K->GetDisplayName() : FText::FromString(TEXT("--")));
	}
}

void UPFOptionsWidget::ApplyKeyBinds()
{
	ACombatForgePlayerController* PC = Cast<ACombatForgePlayerController>(GetOwningPlayer());
	UPFInputConfig* Cfg = PC ? PC->GetInputConfig() : nullptr;
	if (!Cfg)
	{
		return;
	}
	bool bAnyChanged = false;
	for (const FName& Id : RebindIds)
	{
		const FKey* Want = WorkingBinds.Find(Id);
		if (!Want || !Want->IsValid() || Cfg->GetActionKey(Id) == *Want)
		{
			continue;
		}
		if (Cfg->SetActionKey(Id, *Want))
		{
			bAnyChanged = true;
			const FKey* Def = DefaultBinds.Find(Id);
			if (Def && *Def == *Want)
			{
				FPFUserPrefs::ClearKeyOverride(Id);   // back to default -> no override entry
			}
			else
			{
				FPFUserPrefs::SetKeyOverride(Id, *Want);
			}
		}
	}
	if (bAnyChanged)
	{
		if (ULocalPlayer* LP = GetOwningLocalPlayer())
		{
			if (UEnhancedInputLocalPlayerSubsystem* Sub = LP->GetSubsystem<UEnhancedInputLocalPlayerSubsystem>())
			{
				Sub->RequestRebuildControlMappings();
			}
		}
	}
}

void UPFOptionsWidget::OnResetBinds()
{
	bListeningForKey = false;
	ListeningIndex = -1;
	for (const FName& Id : RebindIds)
	{
		if (const FKey* Def = DefaultBinds.Find(Id))
		{
			WorkingBinds.Add(Id, *Def);
		}
	}
	RefreshRebindLabels();
}

void UPFOptionsWidget::AddHowToLine(UVerticalBox* Box, const FString& Text, int32 Size, bool bBold,
	const FLinearColor& Color)
{
	UTextBlock* T = MakeLabel(WidgetTree, Text, Size, bBold);
	T->SetColorAndOpacity(FSlateColor(Color));
	T->SetAutoWrapText(true);
	if (UVerticalBoxSlot* V = Box->AddChildToVerticalBox(T))
	{
		V->SetPadding(FMargin(0.f, bBold ? 10.f : 2.f, 0.f, 0.f));
		V->SetHorizontalAlignment(HAlign_Fill);
	}
}

void UPFOptionsWidget::BuildClassPage(UWidget* ParentBox)
{
	// In-game class switch (pause menu). The boot menu has its own full CHARACTER tab, so this whole tab is
	// hidden there (ApplyEmbeddedChrome collapses TabClass). Click the button to cycle your 5 saved classes;
	// it applies to your pawn's loadout immediately.
	UVerticalBox* Box = CastChecked<UVerticalBox>(ParentBox);

	if (UVerticalBoxSlot* V = Box->AddChildToVerticalBox(
		MakeLabel(WidgetTree, TEXT("Switch your class without leaving the match."), 14, false)))
	{
		V->SetHorizontalAlignment(HAlign_Center);
		V->SetPadding(FMargin(0.f, 0.f, 0.f, 18.f));
	}

	UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>();
	Row->AddChildToHorizontalBox(MakeLabel(WidgetTree, TEXT("Active class"), 16, true));
	ClassButton = WidgetTree->ConstructWidget<UButton>();
	ClassButton->SetBackgroundColor(FLinearColor(0.12f, 0.35f, 0.75f));
	ClassButton->OnClicked.AddDynamic(this, &UPFOptionsWidget::OnClassClicked);
	ClassValueText = MakeLabel(WidgetTree, TEXT("  Class 1  "), 18, true);
	ClassButton->AddChild(ClassValueText);
	if (UHorizontalBoxSlot* H = Row->AddChildToHorizontalBox(ClassButton))
	{
		H->SetPadding(FMargin(20.f, 0.f, 0.f, 0.f));
		H->SetVerticalAlignment(VAlign_Center);
	}
	if (UVerticalBoxSlot* V = Box->AddChildToVerticalBox(Row))
	{
		V->SetHorizontalAlignment(HAlign_Center);
		V->SetPadding(FMargin(0.f, 6.f));
	}

	if (UVerticalBoxSlot* V = Box->AddChildToVerticalBox(MakeLabel(WidgetTree,
		TEXT("Click to cycle your 5 saved classes. Edit each class's weapon + outfit from the boot menu's CHARACTER tab."),
		12, false)))
	{
		V->SetHorizontalAlignment(HAlign_Center);
		V->SetPadding(FMargin(0.f, 18.f, 0.f, 0.f));
	}
}

void UPFOptionsWidget::BuildHowToPlayPage(UWidget* ParentBox)
{
	UVerticalBox* Box = CastChecked<UVerticalBox>(ParentBox);

	const FLinearColor Head(1.f, 0.92f, 0.35f);
	const FLinearColor Body(0.88f, 0.89f, 0.92f);
	const FLinearColor Key(0.65f, 0.82f, 1.f);
	const FLinearColor Dim(0.55f, 0.57f, 0.62f);

	AddHowToLine(Box, TEXT("THE MATCH"), 15, true, Head);
	AddHowToLine(Box, TEXT("Lobby → Build forts → Fight. Host sets map, mode, type, format, and bots on the pre-game screen, then START GAME."), 13, false, Body);

	AddHowToLine(Box, TEXT("MOVE & LOOK"), 15, true, Head);
	AddHowToLine(Box, TEXT("WASD  move     ·     Mouse  look     ·     Space  jump (press Space again mid-air at a wall to climb over)"), 13, false, Key);
	AddHowToLine(Box, TEXT("Shift  sprint     ·     Ctrl / C  crouch (slide while sprinting; hold or toggle in Options)"), 13, false, Key);

	AddHowToLine(Box, TEXT("COMBAT"), 15, true, Head);
	AddHowToLine(Box, TEXT("LMB  fire  ·  RMB  aim  ·  V  fire mode  ·  R  reload  ·  F  refill at ammo barrels"), 13, false, Key);
	AddHowToLine(Box, TEXT("E  frag  ·  Q  smoke  ·  B  melee tag  ·  Scroll  swap pistol  ·  Rifle 30rd / SMG 25rd / Pistol 18rd"), 13, false, Key);
	AddHowToLine(Box, TEXT("Mid-field floating BOMB — F grab · G plant on a piece (15s fuse) — enemies HOLD F 8s to defuse. Boom removes that piece for the match."), 13, false, Key);
	AddHowToLine(Box, TEXT("You're OUT at 3 head, 5 chest, 8 limb, or 10 total hits — whichever comes first (HUD pips + H/C/L readout). Showdown rounds: one hit."), 13, false, Body);

	AddHowToLine(Box, TEXT("BUILD PHASE"), 15, true, Head);
	AddHowToLine(Box, TEXT("F1 Wall  ·  F2 Floor  ·  F3 Ramp  ·  F4 Ceiling  ·  scroll cycles pieces (Wall→Window→Door…)  ·  tap Q for piece wheel  ·  X delete"), 13, false, Key);
	AddHowToLine(Box, TEXT("LMB place  ·  R rotate ramp/prop  ·  Scroll cycle  ·  Tap Q piece wheel  ·  F open/close doors  ·  Trap drops under enemies only"), 13, false, Key);
	AddHowToLine(Box, TEXT("Build on your half only. Full wall-offs are allowed — breach sealed paths with the mid-field bomb. Barrier down when combat starts."), 13, false, Body);

	AddHowToLine(Box, TEXT("LOBBY / MATCH"), 15, true, Head);
	AddHowToLine(Box, TEXT("F  ready     ·     Enter  host start     ·     Tab  scoreboard     ·     Esc  options"), 13, false, Key);
	AddHowToLine(Box, TEXT("Host can click a player in lobby to swap their team. While you're out, scroll to switch class for your next spawn."), 13, false, Body);

	AddHowToLine(Box, TEXT("MAPS & MODES"), 15, true, Head);
	AddHowToLine(Box, TEXT("MAP: Warehouse (indoor) or The Yard (open-air, double width)."), 13, false, Body);
	AddHowToLine(Box, TEXT("Creative / Remix / Play-Only = what happens in Build. Remix & Play-Only load a community map — hosts can star up to 5 favorites."), 13, false, Body);
	AddHowToLine(Box, TEXT("Elimination · Skirmish · FFA · CTF · Domination · Hardpoint = how you win combat."), 13, false, Body);
	AddHowToLine(Box, TEXT("4v4 / 6v6 + bots checkbox fill empty slots. Rate the arena after the match — top maps float up the community list."), 13, false, Dim);
}

void UPFOptionsWidget::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);
	// Collapsed widgets don't tick, so this only polls while the menu is actually on screen.
	// Embedded (boot-menu tab) instances never show the rotation strip — ApplyEmbeddedChrome
	// collapsed it, and updating here would flip it visible again.
	if (bEmbedded)
	{
		return;
	}
	CycleBarPollAccum += InDeltaTime;
	if (CycleBarPollAccum >= 0.5f)
	{
		CycleBarPollAccum = 0.f;
		PFCycleBar::Update(
			GetWorld() ? GetWorld()->GetGameState<ACombatForgeGameState>() : nullptr,
			CycleBarRoot, CycleSegs, CycleTexts);
	}
}

void UPFOptionsWidget::NativeConstruct()
{
	Super::NativeConstruct();
	SetIsFocusable(true);   // required so key-rebind capture (NativeOnKeyDown) receives input
	if (bEmbedded)
	{
		// Hosted as a menu tab: stay visible + keep the chrome hidden (don't collapse like the overlay does).
		ApplyEmbeddedChrome();
		SetVisibility(ESlateVisibility::Visible);
		bOpen = true;
	}
	else
	{
		SetVisibility(ESlateVisibility::Collapsed);
		bOpen = false;
	}
	PullFromSettings();
	RefreshLabels();
	SelectTab(bEmbedded ? ActiveTab : 0);
}

void UPFOptionsWidget::Open()
{
	// P2-U1: an in-flight rebind capture must never survive a close/reopen — a stale
	// bListeningForKey swallows the next keypress as a bind and leaves the row on "press key…".
	if (bListeningForKey)
	{
		bListeningForKey = false;
		ListeningIndex = -1;
		RefreshRebindLabels();
	}
	PullFromSettings();
	RefreshLabels();
	SelectTab(ActiveTab);
	SetVisibility(ESlateVisibility::Visible);
	bOpen = true;

	if (APlayerController* PC = GetOwningPlayer())
	{
		FInputModeGameAndUI Mode;
		Mode.SetWidgetToFocus(TakeWidget());
		Mode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
		Mode.SetHideCursorDuringCapture(false);
		PC->SetInputMode(Mode);
		PC->bShowMouseCursor = true;
	}
	UE_LOG(CombatForgeLog, Log, TEXT("Options: opened"));
}

void UPFOptionsWidget::OpenHowToPlay()
{
	ActiveTab = 4;   // How to Play moved to index 4 when the CLASS tab was inserted at 3
	Open();
}

void UPFOptionsWidget::ApplyEmbeddedChrome()
{
	if (Dimmer)            { Dimmer->SetVisibility(ESlateVisibility::Collapsed); }
	if (TitleText)         { TitleText->SetVisibility(ESlateVisibility::Collapsed); }
	if (BackButton)        { BackButton->SetVisibility(ESlateVisibility::Collapsed); }
	if (QuitMenuButton)    { QuitMenuButton->SetVisibility(ESlateVisibility::Collapsed); }
	if (QuitDesktopButton) { QuitDesktopButton->SetVisibility(ESlateVisibility::Collapsed); }
	if (ResetSpawnButton)  { ResetSpawnButton->SetVisibility(ESlateVisibility::Collapsed); }
	// The boot menu has its own top-level HOW TO PLAY tab, so the embedded options card showing
	// a second one read as a duplicate (Tom 2026-07-15). The in-game pause overlay never routes
	// through here (bEmbedded=false), so it keeps its How to Play tab.
	if (TabHowTo)          { TabHowTo->SetVisibility(ESlateVisibility::Collapsed); }
	// The boot menu has its own CHARACTER tab for class selection, so hide the in-game CLASS tab here.
	if (TabClass)          { TabClass->SetVisibility(ESlateVisibility::Collapsed); }
	// The match-rotation strip is an IN-MATCH readout ("where is the wheel right now") — on the
	// boot menu's embedded options tab it floated over the tab content (Tom, alpha-16 playtest).
	// The in-game pause overlay (bEmbedded=false) keeps it; NativeTick's poll is gated the same
	// way so it can't re-show itself here.
	if (CycleBarRoot)      { CycleBarRoot->SetVisibility(ESlateVisibility::Collapsed); }
}

void UPFOptionsWidget::EnterEmbeddedMode()
{
	// Mark embedded FIRST so NativeConstruct (which fires later, when added to the parent tree) keeps us visible
	// instead of collapsing. Force the tree to build, then strip the overlay chrome so only the card shows.
	bEmbedded = true;
	TakeWidget();
	ApplyEmbeddedChrome();
	PullFromSettings();
	RefreshLabels();
	SelectTab(0);
	SetVisibility(ESlateVisibility::Visible);
	bOpen = true;   // treat as active so its own logic (rebind capture, etc.) runs, but DON'T grab input mode
}

void UPFOptionsWidget::Close()
{
	// P2-U1: cancel any half-finished rebind capture with the menu (see Open()).
	if (bListeningForKey)
	{
		bListeningForKey = false;
		ListeningIndex = -1;
		RefreshRebindLabels();
	}
	// Changes save on CLOSE, not only on Apply — "set quality, Esc out, next match it's back to Low" was a
	// real playtest loss: the widget silently dropped unsaved working values.
	PushToSettings(/*bSave=*/true);
	SetVisibility(ESlateVisibility::Collapsed);
	bOpen = false;
	if (ACombatForgePlayerController* PC = Cast<ACombatForgePlayerController>(GetOwningPlayer()))
	{
		PC->NotifyOptionsMenuClosed();
	}
	UE_LOG(CombatForgeLog, Log, TEXT("Options: closed (settings saved)"));
}

void UPFOptionsWidget::NativeDestruct()
{
	bListeningForKey = false;   // P2-U1: no capture may outlive the widget
	ListeningIndex = -1;
	// The EMBEDDED boot-menu instance is never Close()d — it dies with the menu when the match starts. Save
	// its working values on the way out so options chosen in the boot menu stick too.
	if (bEmbedded)
	{
		PushToSettings(/*bSave=*/true);
	}
	Super::NativeDestruct();
}

void UPFOptionsWidget::SelectTab(int32 Index)
{
	ActiveTab = FMath::Clamp(Index, 0, 4);
	if (PageSwitcher)
	{
		PageSwitcher->SetActiveWidgetIndex(ActiveTab);
	}
}

void UPFOptionsWidget::OnTabVideo() { SelectTab(0); }
void UPFOptionsWidget::OnTabAudio() { SelectTab(1); }
void UPFOptionsWidget::OnTabControls() { SelectTab(2); }
void UPFOptionsWidget::OnTabClass() { SelectTab(3); }
void UPFOptionsWidget::OnTabHowTo() { SelectTab(4); }

void UPFOptionsWidget::OnBackClicked()
{
	Close();
}

void UPFOptionsWidget::OnQuitToMenuClicked()
{
	if (ACombatForgePlayerController* PC = Cast<ACombatForgePlayerController>(GetOwningPlayer()))
	{
		PC->QuitToMenu();
	}
}

void UPFOptionsWidget::OnQuitToDesktopClicked()
{
	if (ACombatForgePlayerController* PC = Cast<ACombatForgePlayerController>(GetOwningPlayer()))
	{
		PC->QuitToDesktop();
	}
}

void UPFOptionsWidget::OnApplyClicked()
{
	PushToSettings(true);
	RefreshLabels();
}

void UPFOptionsWidget::OnFullscreenChanged(bool bIsChecked)
{
	// P2-U2: the checkbox and the WINDOW mode button are ONE control now — checking it selects
	// Fullscreen (borderless at apply time, see PushToSettings), unchecking selects Windowed.
	// Before this it only flipped bWorkingFullscreen, so the applied mode and the SAVED
	// WorkingWindowMode disagreed and "fullscreen" reverted on the next launch.
	bWorkingFullscreen = bIsChecked;
	WorkingWindowMode = bIsChecked ? 0 : 2;
	RefreshLabels();
}

void UPFOptionsWidget::OnVSyncChanged(bool bIsChecked)
{
	bWorkingVSync = bIsChecked;
}

void UPFOptionsWidget::OnQualityClicked()
{
	WorkingQuality = (WorkingQuality + 1) % 4;
	RefreshLabels();
}

void UPFOptionsWidget::OnWindowModeClicked()
{
	WorkingWindowMode = (WorkingWindowMode + 1) % 3;
	bWorkingFullscreen = (WorkingWindowMode == 0);
	RefreshLabels();
}

void UPFOptionsWidget::OnResolutionClicked()
{
	WorkingResIndex = (WorkingResIndex + 1) % NumResolutions;
	RefreshLabels();
}

void UPFOptionsWidget::OnFpsLimitClicked()
{
	WorkingFpsIndex = (WorkingFpsIndex + 1) % 5;
	RefreshLabels();
}

void UPFOptionsWidget::OnResScaleChanged(float Value)
{
	WorkingResScale = FMath::Clamp(Value, 50.f, 100.f);
	RefreshLabels();
}

void UPFOptionsWidget::OnBrightnessChanged(float Value)
{
	WorkingBrightness = FMath::Clamp(Value, -1.f, 1.f);
	ApplyBrightnessContrast(WorkingBrightness, WorkingContrast);   // live while dragging
	RefreshLabels();
}

void UPFOptionsWidget::OnContrastChanged(float Value)
{
	WorkingContrast = FMath::Clamp(Value, 0.85f, 1.2f);
	ApplyBrightnessContrast(WorkingBrightness, WorkingContrast);
	RefreshLabels();
}

void UPFOptionsWidget::ApplyBrightnessContrast(float EV, float Contrast)
{
	if (UWorld* World = GetWorld())
	{
		if (UPFLightingSubsystem* Lighting = World->GetSubsystem<UPFLightingSubsystem>())
		{
			Lighting->SetUserGrade(EV, Contrast);   // no-op in worlds without a spawned rig (boot menu)
		}
	}
}

void UPFOptionsWidget::OnMasterVolChanged(float Value)
{
	WorkingMasterVol = FMath::Clamp(Value, 0.f, 1.f);
	ApplyMasterVolume(WorkingMasterVol);
	RefreshLabels();
}

void UPFOptionsWidget::OnSfxVolChanged(float Value)
{
	WorkingSfxVol = FMath::Clamp(Value, 0.f, 1.f);
	ApplySfxVolume(WorkingSfxVol);
	RefreshLabels();
}

void UPFOptionsWidget::OnAmbientVolChanged(float Value)
{
	WorkingAmbientVol = FMath::Clamp(Value, 0.f, 1.f);
	// Live preview: phase music uses AmbientVolume — push immediately while dragging.
	FPFUserPrefs::SetAmbientVolume(WorkingAmbientVol);
	if (APlayerController* PC = GetOwningPlayer())
	{
		if (UGameInstance* GI = PC->GetGameInstance())
		{
			if (UPFMusicSubsystem* Music = GI->GetSubsystem<UPFMusicSubsystem>())
			{
				Music->ApplyVolumeFromPrefs();
			}
		}
	}
	RefreshLabels();
}

void UPFOptionsWidget::OnSensChanged(float Value)
{
	WorkingSens = FMath::Clamp(Value, 0.2f, 6.f);
	ApplyLookSensitivity(WorkingSens);   // live while dragging (Tom 2026-07-17: was apply-on-Apply only)
	RefreshLabels();
}

void UPFOptionsWidget::OnGamepadSensChanged(float Value)
{
	// Pref-backed and read live by OnLookStickInput, so writing it IS the live apply.
	WorkingGamepadSens = FMath::Clamp(Value, 0.2f, 3.f);
	FPFUserPrefs::SetGamepadLookScale(WorkingGamepadSens);
	RefreshLabels();
}

void UPFOptionsWidget::OnUIScaleChanged(float Value)
{
	WorkingUIScale = FMath::Clamp(Value, 0.85f, 1.3f);
	// Live-apply while dragging so the effect is visible immediately (like brightness).
	FPFUserPrefs::SetUIScale(WorkingUIScale);
	FPFHandheldPlatform::ApplyUIScaleFromPrefs();
	RefreshLabels();
}

void UPFOptionsWidget::OnFovChanged(float Value)
{
	WorkingFov = FMath::Clamp(Value, 80.f, 110.f);
	RefreshLabels();
}

void UPFOptionsWidget::OnInvertYChanged(bool bIsChecked)
{
	bWorkingInvertY = bIsChecked;
}

void UPFOptionsWidget::OnADSToggleChanged(bool bIsChecked)
{
	bWorkingADSToggle = bIsChecked;
}

void UPFOptionsWidget::OnCrouchToggleChanged(bool bIsChecked)
{
	bWorkingCrouchToggle = bIsChecked;
}

void UPFOptionsWidget::OnClassClicked()
{
	const int32 N = PFChar::SaveSlotCount();
	if (N <= 0)
	{
		return;
	}
	PFChar::SetActiveSaveSlot((PFChar::GetActiveSaveSlot() + 1) % N);
	// Apply live to the local pawn (new outfit + weapon). In combat this swaps your loadout immediately;
	// dead/lobby it's picked up on the next spawn.
	if (APlayerController* PC = GetOwningPlayer())
	{
		if (ACombatForgeCharacter* Char = Cast<ACombatForgeCharacter>(PC->GetPawn()))
		{
			Char->ReapplyCharacterConfig();
			Char->ReapplyWeaponLoadout();
		}
	}
	if (ClassValueText)
	{
		ClassValueText->SetText(FText::FromString(
			FString::Printf(TEXT("  Class %d  "), PFChar::GetActiveSaveSlot() + 1)));
	}
}

void UPFOptionsWidget::OnResetToSpawnClicked()
{
	if (APlayerController* PC = GetOwningPlayer())
	{
		if (ACombatForgeCharacter* Char = Cast<ACombatForgeCharacter>(PC->GetPawn()))
		{
			Char->RequestResetToSpawn();
		}
	}
	Close();   // dismiss options so the player sees the teleport
}

const TCHAR* UPFOptionsWidget::QualityName(int32 Level)
{
	switch (Level)
	{
	case 0: return TEXT("Low");
	case 1: return TEXT("Medium");
	case 2: return TEXT("High");
	case 3: return TEXT("Epic");
	default: return TEXT("Medium");
	}
}

const TCHAR* UPFOptionsWidget::WindowModeName(int32 Idx)
{
	switch (Idx)
	{
	case 1: return TEXT("Borderless");
	case 2: return TEXT("Windowed");
	default: return TEXT("Fullscreen");
	}
}

FString UPFOptionsWidget::ResolutionLabel() const
{
	static const int32 W[] = { 1280, 1366, 1600, 1920, 2560 };
	static const int32 H[] = { 720,  768,  900,  1080, 1440 };
	const int32 i = FMath::Clamp(WorkingResIndex, 0, NumResolutions - 1);
	return FString::Printf(TEXT("%dx%d"), W[i], H[i]);
}

void UPFOptionsWidget::RefreshLabels()
{
	if (ClassValueText)
	{
		ClassValueText->SetText(FText::FromString(
			FString::Printf(TEXT("  Class %d  "), PFChar::GetActiveSaveSlot() + 1)));
	}
	if (QualityValueText)
	{
		QualityValueText->SetText(FText::FromString(FString::Printf(TEXT("  %s  "), QualityName(WorkingQuality))));
	}
	if (WindowModeValueText)
	{
		WindowModeValueText->SetText(FText::FromString(FString::Printf(TEXT("  %s  "), WindowModeName(WorkingWindowMode))));
	}
	if (ResolutionValueText)
	{
		ResolutionValueText->SetText(FText::FromString(FString::Printf(TEXT("  %s  "), *ResolutionLabel())));
	}
	if (FpsLimitValueText)
	{
		const float Cap = FPFUserPrefs::FrameRateLimitForIndex(WorkingFpsIndex);
		FpsLimitValueText->SetText(FText::FromString(Cap > 0.f
			? FString::Printf(TEXT("  %d  "), FMath::RoundToInt(Cap))
			: FString(TEXT("  Uncapped  "))));
	}
	if (ResScaleValueText)
	{
		ResScaleValueText->SetText(FText::FromString(FString::Printf(TEXT("%d%%"), FMath::RoundToInt(WorkingResScale))));
	}
	if (GamepadSensValueText)
	{
		GamepadSensValueText->SetText(FText::FromString(FString::Printf(TEXT("%.2f"), WorkingGamepadSens)));
	}
	if (UIScaleValueText)
	{
		UIScaleValueText->SetText(FText::FromString(FString::Printf(TEXT("%d%%"), FMath::RoundToInt(WorkingUIScale * 100.f))));
	}
	if (BrightnessValueText)
	{
		BrightnessValueText->SetText(FText::FromString(FString::Printf(TEXT("%+.2f EV"), WorkingBrightness)));
	}
	if (ContrastValueText)
	{
		ContrastValueText->SetText(FText::FromString(FString::Printf(TEXT("%.2f"), WorkingContrast)));
	}
	if (MasterVolValueText)
	{
		MasterVolValueText->SetText(FText::FromString(FString::Printf(TEXT("%d%%"), FMath::RoundToInt(WorkingMasterVol * 100.f))));
	}
	if (SfxVolValueText)
	{
		SfxVolValueText->SetText(FText::FromString(FString::Printf(TEXT("%d%%"), FMath::RoundToInt(WorkingSfxVol * 100.f))));
	}
	if (AmbientVolValueText)
	{
		AmbientVolValueText->SetText(FText::FromString(FString::Printf(TEXT("%d%%"), FMath::RoundToInt(WorkingAmbientVol * 100.f))));
	}
	if (SensValueText)
	{
		SensValueText->SetText(FText::FromString(FString::Printf(TEXT("%.2f"), WorkingSens)));
	}
	if (FovValueText)
	{
		FovValueText->SetText(FText::FromString(FString::Printf(TEXT("%d"), FMath::RoundToInt(WorkingFov))));
	}
	if (FullscreenCheck)
	{
		FullscreenCheck->SetIsChecked(bWorkingFullscreen || WorkingWindowMode == 0);
	}
	if (VSyncCheck)
	{
		VSyncCheck->SetIsChecked(bWorkingVSync);
	}
	if (InvertYCheck)
	{
		InvertYCheck->SetIsChecked(bWorkingInvertY);
	}
	if (ADSToggleCheck)
	{
		ADSToggleCheck->SetIsChecked(bWorkingADSToggle);
	}
	if (CrouchToggleCheck)
	{
		CrouchToggleCheck->SetIsChecked(bWorkingCrouchToggle);
	}
	if (ResScaleSlider) { ResScaleSlider->SetValue(WorkingResScale); }
	if (BrightnessSlider) { BrightnessSlider->SetValue(WorkingBrightness); }
	if (ContrastSlider) { ContrastSlider->SetValue(WorkingContrast); }
	if (MasterVolSlider) { MasterVolSlider->SetValue(WorkingMasterVol); }
	if (SfxVolSlider) { SfxVolSlider->SetValue(WorkingSfxVol); }
	if (AmbientVolSlider) { AmbientVolSlider->SetValue(WorkingAmbientVol); }
	if (SensSlider) { SensSlider->SetValue(WorkingSens); }
	if (GamepadSensSlider) { GamepadSensSlider->SetValue(WorkingGamepadSens); }
	if (UIScaleSlider) { UIScaleSlider->SetValue(WorkingUIScale); }
	if (FovSlider) { FovSlider->SetValue(WorkingFov); }
}

void UPFOptionsWidget::PullFromSettings()
{
	if (UGameUserSettings* S = GEngine ? GEngine->GetGameUserSettings() : nullptr)
	{
		const EWindowMode::Type Mode = S->GetFullscreenMode();
		if (Mode == EWindowMode::Fullscreen) { WorkingWindowMode = 0; }
		else if (Mode == EWindowMode::WindowedFullscreen) { WorkingWindowMode = 1; }
		else { WorkingWindowMode = 2; }
		bWorkingFullscreen = (WorkingWindowMode == 0);
		bWorkingVSync = S->IsVSyncEnabled();
	}

	// Seed quality/resolution/scale from OUR prefs, never from the engine readbacks — those are lossy:
	// GetOverallScalabilityLevel() returns -1 whenever a custom resolution scale is applied (which we always
	// apply), the old clamp turned that into 0 = Low, and the next Apply then SAVED Low for real. That was
	// the "my graphics keep resetting to Low every match" bug.
	WorkingQuality = FPFUserPrefs::GetQualityLevel();
	WorkingResScale = FPFUserPrefs::GetResolutionScalePct();
	WorkingResIndex = FPFUserPrefs::GetResolutionIndex();
	WorkingFpsIndex = FPFUserPrefs::GetFrameRateLimitIndex();

	if (GConfig)
	{
		GConfig->GetFloat(TEXT("CombatForge"), TEXT("MasterVolume"), WorkingMasterVol, FPFPaths::UserPrefsIni());
		GConfig->GetFloat(TEXT("CombatForge"), TEXT("SfxVolume"), WorkingSfxVol, FPFPaths::UserPrefsIni());
		GConfig->GetFloat(TEXT("CombatForge"), TEXT("PFSensitivity"), WorkingSens, FPFPaths::UserPrefsIni());
	}
	WorkingMasterVol = FMath::Clamp(WorkingMasterVol, 0.f, 1.f);
	WorkingSfxVol = FMath::Clamp(WorkingSfxVol, 0.f, 1.f);
	WorkingSens = FMath::Clamp(WorkingSens, 0.2f, 6.f);
	WorkingAmbientVol = FPFUserPrefs::GetAmbientVolume();
	WorkingBrightness = FPFUserPrefs::GetBrightnessEV();
	WorkingContrast = FPFUserPrefs::GetContrastScale();
	WorkingGamepadSens = FPFUserPrefs::GetGamepadLookScale();
	WorkingUIScale = FPFUserPrefs::GetUIScale();
	bWorkingInvertY = FPFUserPrefs::GetInvertY();
	bWorkingADSToggle = FPFUserPrefs::GetADSToggle();
	bWorkingCrouchToggle = FPFUserPrefs::GetCrouchToggle();
	WorkingFov = FPFUserPrefs::GetFieldOfView();
	WorkingWindowMode = FPFUserPrefs::GetWindowModeIndex();
	// P2-U2: prefs are the mode's source of truth (they overwrite the UGameUserSettings seed
	// above) — re-derive the fullscreen flag from the SAME source or the checkbox desyncs.
	bWorkingFullscreen = (WorkingWindowMode == 0);

	// Key binds (current live keys + shipped defaults) from the input config.
	WorkingBinds.Empty();
	DefaultBinds.Empty();
	if (ACombatForgePlayerController* PC = Cast<ACombatForgePlayerController>(GetOwningPlayer()))
	{
		if (UPFInputConfig* Cfg = PC->GetInputConfig())
		{
			for (const FName& Id : RebindIds)
			{
				WorkingBinds.Add(Id, Cfg->GetActionKey(Id));
				DefaultBinds.Add(Id, Cfg->GetActionDefaultKey(Id));
			}
		}
	}
	RefreshRebindLabels();

	ApplyMasterVolume(WorkingMasterVol);
	ApplySfxVolume(WorkingSfxVol);
}

void UPFOptionsWidget::PushToSettings(bool bSave)
{
	FPFUserPrefs::SetInvertY(bWorkingInvertY);
	FPFUserPrefs::SetADSToggle(bWorkingADSToggle);
	FPFUserPrefs::SetCrouchToggle(bWorkingCrouchToggle);
	FPFUserPrefs::SetFieldOfView(WorkingFov);
	FPFUserPrefs::SetGamepadLookScale(WorkingGamepadSens);
	FPFUserPrefs::SetUIScale(WorkingUIScale);
	FPFHandheldPlatform::ApplyUIScaleFromPrefs();
	FPFUserPrefs::SetAmbientVolume(WorkingAmbientVol);
	FPFUserPrefs::SetBrightnessEV(WorkingBrightness);
	FPFUserPrefs::SetContrastScale(WorkingContrast);
	FPFUserPrefs::SetWindowModeIndex(WorkingWindowMode);
	// Mirror the user's TRUE video choices (the engine's own readbacks are lossy — see PullFromSettings).
	FPFUserPrefs::SetQualityLevel(WorkingQuality);
	FPFUserPrefs::SetResolutionIndex(WorkingResIndex);
	FPFUserPrefs::SetResolutionScalePct(WorkingResScale);
	FPFUserPrefs::SetFrameRateLimitIndex(WorkingFpsIndex);

	if (UGameUserSettings* S = GEngine ? GEngine->GetGameUserSettings() : nullptr)
	{
		// NEVER exclusive fullscreen. At a non-native resolution it switches the monitor's DISPLAY MODE, so
		// alt-tabbing re-tears the whole desktop (playtest at 1440p on a 4K monitor: every window resized/
		// zoomed). "Fullscreen" now means BORDERLESS (WindowedFullscreen) — the desktop is untouched and
		// alt-tab is instant, which is the modern-shooter standard.
		const EWindowMode::Type Mode = (WorkingWindowMode == 2 && !bWorkingFullscreen)
			? EWindowMode::Windowed
			: EWindowMode::WindowedFullscreen;
		S->SetFullscreenMode(Mode);
		S->SetVSyncEnabled(bWorkingVSync);
		S->SetOverallScalabilityLevel(WorkingQuality);
		// Method switches ride along with the sg level (not scalability-flagged -> must be set from code).
		FPFUserPrefs::ApplyQualityMethodCVars(WorkingQuality);
		S->SetFrameRateLimit(FPFUserPrefs::FrameRateLimitForIndex(WorkingFpsIndex));

		static const int32 W[] = { 1280, 1366, 1600, 1920, 2560 };
		static const int32 H[] = { 720,  768,  900,  1080, 1440 };
		const int32 i = FMath::Clamp(WorkingResIndex, 0, NumResolutions - 1);
		float Scale = FMath::Clamp(WorkingResScale / 100.f, 0.5f, 1.f);
		if (Mode == EWindowMode::WindowedFullscreen)
		{
			// Borderless always fills the desktop, so make the RESOLUTION pick still mean something: fold it
			// into the render scale (1440p on a 4K desktop = ~67% render cost, 1080p = 50%). The window never
			// changes size; only the internal render resolution does.
			const FIntPoint Desktop = S->GetDesktopResolution();
			if (Desktop.Y > 0)
			{
				Scale = FMath::Clamp(Scale * (static_cast<float>(H[i]) / static_cast<float>(Desktop.Y)), 0.5f, 1.f);
			}
		}
		S->SetResolutionScaleNormalized(Scale);
		S->SetScreenResolution(FIntPoint(W[i], H[i]));   // honored in Windowed; borderless sizes to the desktop
		S->ApplySettings(false);
		if (bSave)
		{
			S->SaveSettings();
		}
	}

	ApplyMasterVolume(WorkingMasterVol);
	ApplySfxVolume(WorkingSfxVol);
	ApplyBrightnessContrast(WorkingBrightness, WorkingContrast);
	ApplyLookSensitivity(WorkingSens);
	ApplyInvertY(bWorkingInvertY);
	ApplyADSToggle(bWorkingADSToggle);
	ApplyCrouchToggle(bWorkingCrouchToggle);
	ApplyFieldOfView(WorkingFov);
	ApplyKeyBinds();

	// Restart ambient at new volume if playing; live-update phase music volume.
	if (ACombatForgePlayerController* PC = Cast<ACombatForgePlayerController>(GetOwningPlayer()))
	{
		if (ACombatForgeCharacter* Char = Cast<ACombatForgeCharacter>(PC->GetPawn()))
		{
			if (UPFCombatAudio* Audio = Char->GetCombatAudio())
			{
				Audio->StopAmbientBed();
				Audio->StartAmbientBed();
			}
		}
		if (UGameInstance* GI = PC->GetGameInstance())
		{
			if (UPFMusicSubsystem* Music = GI->GetSubsystem<UPFMusicSubsystem>())
			{
				Music->ApplyVolumeFromPrefs();
			}
		}
	}

	if (bSave && GConfig)
	{
		GConfig->SetFloat(TEXT("CombatForge"), TEXT("MasterVolume"), WorkingMasterVol, FPFPaths::UserPrefsIni());
		GConfig->SetFloat(TEXT("CombatForge"), TEXT("SfxVolume"), WorkingSfxVol, FPFPaths::UserPrefsIni());
		GConfig->SetFloat(TEXT("CombatForge"), TEXT("PFSensitivity"), WorkingSens, FPFPaths::UserPrefsIni());
		FPFUserPrefs::Flush();
		GConfig->Flush(false, FPFPaths::UserPrefsIni());
	}

	if (HintText)
	{
		HintText->SetText(FText::FromString(bSave ? TEXT("Settings applied & saved.") : TEXT("Settings applied.")));
	}
	UE_LOG(CombatForgeLog, Log,
		TEXT("Options applied: FS=%d VSync=%d Qual=%d ResScale=%.0f Master=%.2f Sfx=%.2f Sens=%.2f"),
		bWorkingFullscreen ? 1 : 0, bWorkingVSync ? 1 : 0, WorkingQuality, WorkingResScale,
		WorkingMasterVol, WorkingSfxVol, WorkingSens);
}

void UPFOptionsWidget::ApplyMasterVolume(float Linear01)
{
	if (GEngine)
	{
		if (FAudioDeviceHandle Dev = GEngine->GetMainAudioDevice())
		{
			if (FAudioDevice* Audio = Dev.GetAudioDevice())
			{
				Audio->SetTransientPrimaryVolume(FMath::Clamp(Linear01, 0.f, 1.f));
			}
		}
	}
}

void UPFOptionsWidget::ApplySfxVolume(float Linear01)
{
	// Stored for combat audio to read; also nudge master-relative transient if only SFX changes.
	// Combat code multiplies by this via a shared config read on play.
	if (GConfig)
	{
		GConfig->SetFloat(TEXT("CombatForge"), TEXT("SfxVolume"), FMath::Clamp(Linear01, 0.f, 1.f), FPFPaths::UserPrefsIni());
	}
}

void UPFOptionsWidget::ApplyLookSensitivity(float Sens)
{
	if (ACombatForgePlayerController* PC = Cast<ACombatForgePlayerController>(GetOwningPlayer()))
	{
		if (UPFInputConfig* Cfg = PC->GetInputConfig())
		{
			Cfg->SetLookSensitivity(Sens);
		}
	}
}

void UPFOptionsWidget::ApplyInvertY(bool bInvert)
{
	if (ACombatForgePlayerController* PC = Cast<ACombatForgePlayerController>(GetOwningPlayer()))
	{
		if (UPFInputConfig* Cfg = PC->GetInputConfig())
		{
			Cfg->SetLookInvertY(bInvert);
		}
	}
}

void UPFOptionsWidget::ApplyADSToggle(bool bToggle)
{
	// Write first so the pawn's re-read sees the new value even if Apply runs before PushToSettings.
	FPFUserPrefs::SetADSToggle(bToggle);
	if (APlayerController* PC = GetOwningPlayer())
	{
		if (ACombatForgeCharacter* Char = Cast<ACombatForgeCharacter>(PC->GetPawn()))
		{
			Char->RefreshADSToggleMode();   // live-apply mid-match, no re-possess needed
		}
	}
}

void UPFOptionsWidget::ApplyCrouchToggle(bool bToggle)
{
	// Write first so the pawn's re-read sees the new value even if Apply runs before PushToSettings.
	FPFUserPrefs::SetCrouchToggle(bToggle);
	if (APlayerController* PC = GetOwningPlayer())
	{
		if (ACombatForgeCharacter* Char = Cast<ACombatForgeCharacter>(PC->GetPawn()))
		{
			Char->RefreshCrouchToggleMode();   // live-apply mid-match, no re-possess needed
		}
	}
}

void UPFOptionsWidget::ApplyFieldOfView(float Fov)
{
	if (ACombatForgePlayerController* PC = Cast<ACombatForgePlayerController>(GetOwningPlayer()))
	{
		if (ACombatForgeCharacter* Char = Cast<ACombatForgeCharacter>(PC->GetPawn()))
		{
			Char->SetPreferredBaseFOV(Fov);
		}
	}
}

void UPFOptionsWidget::ApplyWindowAndResolution()
{
	// Handled inside PushToSettings via UGameUserSettings.
}
