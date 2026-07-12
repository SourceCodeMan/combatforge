// Copyright (c) 2026 Tom Chapman. All rights reserved.

#include "UI/PFOptionsWidget.h"

#include "PaintForge.h"
#include "Core/PaintForgePlayerController.h"
#include "Input/PFInputConfig.h"

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
		V->SetPadding(FMargin(0.f, 0.f, 0.f, 16.f));
	}

	// Tabs
	UHorizontalBox* Tabs = WidgetTree->ConstructWidget<UHorizontalBox>();
	TabVideo = MakeTabButton(TEXT("  VIDEO  "), TEXT("TabVideo"));
	TabAudio = MakeTabButton(TEXT("  AUDIO  "), TEXT("TabAudio"));
	TabControls = MakeTabButton(TEXT("  CONTROLS  "), TEXT("TabControls"));
	TabHowTo = MakeTabButton(TEXT("  HOW TO PLAY  "), TEXT("TabHowTo"));
	TabVideo->OnClicked.AddDynamic(this, &UPFOptionsWidget::OnTabVideo);
	TabAudio->OnClicked.AddDynamic(this, &UPFOptionsWidget::OnTabAudio);
	TabControls->OnClicked.AddDynamic(this, &UPFOptionsWidget::OnTabControls);
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
	UVerticalBox* HowToPage = WidgetTree->ConstructWidget<UVerticalBox>();
	BuildVideoPage(VideoPage);
	BuildAudioPage(AudioPage);
	BuildControlsPage(ControlsPage);
	BuildHowToPlayPage(HowToPage);
	PageSwitcher->AddChild(VideoPage);
	PageSwitcher->AddChild(AudioPage);
	PageSwitcher->AddChild(ControlsPage);
	PageSwitcher->AddChild(HowToPage);
	// Constrain width so How to Play lines wrap cleanly on smaller viewports.
	USizeBox* PageSizer = WidgetTree->ConstructWidget<USizeBox>();
	PageSizer->SetWidthOverride(560.f);
	PageSizer->SetContent(PageSwitcher);
	if (UVerticalBoxSlot* V = Card->AddChildToVerticalBox(PageSizer))
	{
		V->SetHorizontalAlignment(HAlign_Center);
		V->SetPadding(FMargin(8.f, 0.f, 8.f, 20.f));
	}

	// Footer buttons
	UHorizontalBox* Footer = WidgetTree->ConstructWidget<UHorizontalBox>();
	ApplyButton = MakeTabButton(TEXT("  APPLY  "), TEXT("ApplyBtn"));
	BackButton = MakeTabButton(TEXT("  BACK  "), TEXT("BackBtn"));
	ApplyButton->OnClicked.AddDynamic(this, &UPFOptionsWidget::OnApplyClicked);
	BackButton->OnClicked.AddDynamic(this, &UPFOptionsWidget::OnBackClicked);
	if (UHorizontalBoxSlot* H = Footer->AddChildToHorizontalBox(ApplyButton))
	{
		H->SetPadding(FMargin(6.f));
	}
	if (UHorizontalBoxSlot* H = Footer->AddChildToHorizontalBox(BackButton))
	{
		H->SetPadding(FMargin(6.f));
	}
	if (UVerticalBoxSlot* V = Card->AddChildToVerticalBox(Footer))
	{
		V->SetHorizontalAlignment(HAlign_Center);
	}

	HintText = MakeLabel(WidgetTree, TEXT("Esc closes · Apply saves to disk"), 12, false);
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

	// Fullscreen
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

	// VSync
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

	// Quality preset
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

	// Resolution scale
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
}

void UPFOptionsWidget::BuildAudioPage(UWidget* ParentBox)
{
	UVerticalBox* Box = CastChecked<UVerticalBox>(ParentBox);

	auto MakeVolRow = [this, Box](const FString& Name, TObjectPtr<USlider>& OutSlider,
		TObjectPtr<UTextBlock>& OutVal, void (UPFOptionsWidget::*Handler)(float))
	{
		UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>();
		Row->AddChildToHorizontalBox(MakeLabel(WidgetTree, Name, 15, false));
		OutSlider = WidgetTree->ConstructWidget<USlider>();
		OutSlider->SetMinValue(0.f);
		OutSlider->SetMaxValue(1.f);
		OutSlider->SetStepSize(0.05f);
		OutSlider->OnValueChanged.AddDynamic(this, Handler);
		if (UHorizontalBoxSlot* H = Row->AddChildToHorizontalBox(OutSlider))
		{
			H->SetPadding(FMargin(16.f, 0.f));
			H->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		}
		OutVal = MakeLabel(WidgetTree, TEXT("100%"), 14, true);
		Row->AddChildToHorizontalBox(OutVal);
		if (UVerticalBoxSlot* V = Box->AddChildToVerticalBox(Row))
		{
			V->SetPadding(FMargin(0.f, 10.f));
		}
	};

	MakeVolRow(TEXT("Master volume"), MasterVolSlider, MasterVolValueText, &UPFOptionsWidget::OnMasterVolChanged);
	MakeVolRow(TEXT("SFX volume"), SfxVolSlider, SfxVolValueText, &UPFOptionsWidget::OnSfxVolChanged);

	UTextBlock* Note = MakeLabel(WidgetTree,
		TEXT("SFX scales combat / UI one-shots relative to master."), 12, false);
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
	SensSlider->SetMaxValue(3.0f);
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

	UTextBlock* Note = MakeLabel(WidgetTree,
		TEXT("1.0 is default. Higher = faster look. Applied immediately on Apply."), 12, false);
	Note->SetColorAndOpacity(FSlateColor(FLinearColor(0.55f, 0.55f, 0.6f)));
	if (UVerticalBoxSlot* V = Box->AddChildToVerticalBox(Note))
	{
		V->SetPadding(FMargin(0.f, 8.f, 0.f, 0.f));
	}
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

void UPFOptionsWidget::BuildHowToPlayPage(UWidget* ParentBox)
{
	UVerticalBox* Box = CastChecked<UVerticalBox>(ParentBox);

	const FLinearColor Head(1.f, 0.92f, 0.35f);
	const FLinearColor Body(0.88f, 0.89f, 0.92f);
	const FLinearColor Key(0.65f, 0.82f, 1.f);
	const FLinearColor Dim(0.55f, 0.57f, 0.62f);

	AddHowToLine(Box, TEXT("THE MATCH"), 15, true, Head);
	AddHowToLine(Box, TEXT("Lobby → Build forts → Fight. Host sets mode, type, format, and bots on the pre-game screen."), 13, false, Body);

	AddHowToLine(Box, TEXT("MOVE & LOOK"), 15, true, Head);
	AddHowToLine(Box, TEXT("WASD  move     ·     Mouse  look     ·     Space  jump"), 13, false, Key);
	AddHowToLine(Box, TEXT("Shift  sprint     ·     Ctrl / C  crouch (slide while sprinting)"), 13, false, Key);

	AddHowToLine(Box, TEXT("COMBAT"), 15, true, Head);
	AddHowToLine(Box, TEXT("LMB  fire     ·     RMB  aim down sights     ·     R  reload"), 13, false, Key);
	AddHowToLine(Box, TEXT("Tag opponents with paint. Hits on body / mask take them out of the round (mode-dependent)."), 13, false, Body);

	AddHowToLine(Box, TEXT("BUILD PHASE"), 15, true, Head);
	AddHowToLine(Box, TEXT("F1 Wall  ·  F2 Floor  ·  F3 Ramp  ·  F4 Roof  ·  cover: Barrel / Crate / Boxes  ·  X / F5 delete"), 13, false, Key);
	AddHowToLine(Box, TEXT("LMB place  ·  R rotate  ·  Scroll cycle piece  ·  Hold Q build wheel  ·  Tap Q last piece"), 13, false, Key);
	AddHowToLine(Box, TEXT("Build on your half only. Barrier down when combat starts."), 13, false, Body);

	AddHowToLine(Box, TEXT("LOBBY / MATCH"), 15, true, Head);
	AddHowToLine(Box, TEXT("F  ready     ·     Enter  host start     ·     Tab  scoreboard     ·     Esc  options"), 13, false, Key);
	AddHowToLine(Box, TEXT("Host can click a player in lobby to swap their team."), 13, false, Body);

	AddHowToLine(Box, TEXT("MODES (quick)"), 15, true, Head);
	AddHowToLine(Box, TEXT("Creative / Improvement / Play-Only = what happens in Build."), 13, false, Body);
	AddHowToLine(Box, TEXT("Elimination · Skirmish · FFA · CTF · Domination · Hardpoint = how you win combat."), 13, false, Body);
	AddHowToLine(Box, TEXT("4v4 / 6v6 + bots checkbox fill empty slots when the match starts."), 13, false, Dim);
}

void UPFOptionsWidget::NativeConstruct()
{
	Super::NativeConstruct();
	SetVisibility(ESlateVisibility::Collapsed);
	bOpen = false;
	PullFromSettings();
	RefreshLabels();
	SelectTab(0);
}

void UPFOptionsWidget::Open()
{
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
	UE_LOG(PaintForgeLog, Log, TEXT("Options: opened"));
}

void UPFOptionsWidget::OpenHowToPlay()
{
	ActiveTab = 3;
	Open();
}

void UPFOptionsWidget::Close()
{
	SetVisibility(ESlateVisibility::Collapsed);
	bOpen = false;
	if (APaintForgePlayerController* PC = Cast<APaintForgePlayerController>(GetOwningPlayer()))
	{
		PC->NotifyOptionsMenuClosed();
	}
	UE_LOG(PaintForgeLog, Log, TEXT("Options: closed"));
}

void UPFOptionsWidget::SelectTab(int32 Index)
{
	ActiveTab = FMath::Clamp(Index, 0, 3);
	if (PageSwitcher)
	{
		PageSwitcher->SetActiveWidgetIndex(ActiveTab);
	}
}

void UPFOptionsWidget::OnTabVideo() { SelectTab(0); }
void UPFOptionsWidget::OnTabAudio() { SelectTab(1); }
void UPFOptionsWidget::OnTabControls() { SelectTab(2); }
void UPFOptionsWidget::OnTabHowTo() { SelectTab(3); }

void UPFOptionsWidget::OnBackClicked()
{
	Close();
}

void UPFOptionsWidget::OnApplyClicked()
{
	PushToSettings(true);
	RefreshLabels();
}

void UPFOptionsWidget::OnFullscreenChanged(bool bIsChecked)
{
	bWorkingFullscreen = bIsChecked;
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

void UPFOptionsWidget::OnResScaleChanged(float Value)
{
	WorkingResScale = FMath::Clamp(Value, 50.f, 100.f);
	RefreshLabels();
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

void UPFOptionsWidget::OnSensChanged(float Value)
{
	WorkingSens = FMath::Clamp(Value, 0.2f, 3.f);
	RefreshLabels();
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

void UPFOptionsWidget::RefreshLabels()
{
	if (QualityValueText)
	{
		QualityValueText->SetText(FText::FromString(FString::Printf(TEXT("  %s  "), QualityName(WorkingQuality))));
	}
	if (ResScaleValueText)
	{
		ResScaleValueText->SetText(FText::FromString(FString::Printf(TEXT("%d%%"), FMath::RoundToInt(WorkingResScale))));
	}
	if (MasterVolValueText)
	{
		MasterVolValueText->SetText(FText::FromString(FString::Printf(TEXT("%d%%"), FMath::RoundToInt(WorkingMasterVol * 100.f))));
	}
	if (SfxVolValueText)
	{
		SfxVolValueText->SetText(FText::FromString(FString::Printf(TEXT("%d%%"), FMath::RoundToInt(WorkingSfxVol * 100.f))));
	}
	if (SensValueText)
	{
		SensValueText->SetText(FText::FromString(FString::Printf(TEXT("%.2f"), WorkingSens)));
	}
	if (FullscreenCheck)
	{
		FullscreenCheck->SetIsChecked(bWorkingFullscreen);
	}
	if (VSyncCheck)
	{
		VSyncCheck->SetIsChecked(bWorkingVSync);
	}
	if (ResScaleSlider)
	{
		ResScaleSlider->SetValue(WorkingResScale);
	}
	if (MasterVolSlider)
	{
		MasterVolSlider->SetValue(WorkingMasterVol);
	}
	if (SfxVolSlider)
	{
		SfxVolSlider->SetValue(WorkingSfxVol);
	}
	if (SensSlider)
	{
		SensSlider->SetValue(WorkingSens);
	}
}

void UPFOptionsWidget::PullFromSettings()
{
	if (UGameUserSettings* S = GEngine ? GEngine->GetGameUserSettings() : nullptr)
	{
		const EWindowMode::Type Mode = S->GetFullscreenMode();
		bWorkingFullscreen = (Mode == EWindowMode::Fullscreen || Mode == EWindowMode::WindowedFullscreen);
		bWorkingVSync = S->IsVSyncEnabled();
		WorkingQuality = FMath::Clamp(S->GetOverallScalabilityLevel(), 0, 3);
		// Resolution scale is 0..100 in some APIs, 0..1 in others — normalize to 50..100 display.
		float ScaleNorm = S->GetResolutionScaleNormalized(); // 0..1
		WorkingResScale = FMath::Clamp(ScaleNorm * 100.f, 50.f, 100.f);
	}

	if (GConfig)
	{
		GConfig->GetFloat(TEXT("PaintForge"), TEXT("MasterVolume"), WorkingMasterVol, GGameUserSettingsIni);
		GConfig->GetFloat(TEXT("PaintForge"), TEXT("SfxVolume"), WorkingSfxVol, GGameUserSettingsIni);
		GConfig->GetFloat(TEXT("PaintForge"), TEXT("PFSensitivity"), WorkingSens, GGameUserSettingsIni);
	}
	WorkingMasterVol = FMath::Clamp(WorkingMasterVol, 0.f, 1.f);
	WorkingSfxVol = FMath::Clamp(WorkingSfxVol, 0.f, 1.f);
	WorkingSens = FMath::Clamp(WorkingSens, 0.2f, 3.f);

	// Live-apply audio so opening options doesn't leave silence from a previous session.
	ApplyMasterVolume(WorkingMasterVol);
	ApplySfxVolume(WorkingSfxVol);
}

void UPFOptionsWidget::PushToSettings(bool bSave)
{
	if (UGameUserSettings* S = GEngine ? GEngine->GetGameUserSettings() : nullptr)
	{
		S->SetFullscreenMode(bWorkingFullscreen ? EWindowMode::Fullscreen : EWindowMode::Windowed);
		S->SetVSyncEnabled(bWorkingVSync);
		S->SetOverallScalabilityLevel(WorkingQuality);
		S->SetResolutionScaleNormalized(FMath::Clamp(WorkingResScale / 100.f, 0.5f, 1.f));
		S->ApplySettings(false);
		if (bSave)
		{
			S->SaveSettings();
		}
	}

	ApplyMasterVolume(WorkingMasterVol);
	ApplySfxVolume(WorkingSfxVol);
	ApplyLookSensitivity(WorkingSens);

	if (bSave && GConfig)
	{
		GConfig->SetFloat(TEXT("PaintForge"), TEXT("MasterVolume"), WorkingMasterVol, GGameUserSettingsIni);
		GConfig->SetFloat(TEXT("PaintForge"), TEXT("SfxVolume"), WorkingSfxVol, GGameUserSettingsIni);
		GConfig->SetFloat(TEXT("PaintForge"), TEXT("PFSensitivity"), WorkingSens, GGameUserSettingsIni);
		GConfig->Flush(false, GGameUserSettingsIni);
	}

	if (HintText)
	{
		HintText->SetText(FText::FromString(bSave ? TEXT("Settings applied & saved.") : TEXT("Settings applied.")));
	}
	UE_LOG(PaintForgeLog, Log,
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
		GConfig->SetFloat(TEXT("PaintForge"), TEXT("SfxVolume"), FMath::Clamp(Linear01, 0.f, 1.f), GGameUserSettingsIni);
	}
}

void UPFOptionsWidget::ApplyLookSensitivity(float Sens)
{
	if (APaintForgePlayerController* PC = Cast<APaintForgePlayerController>(GetOwningPlayer()))
	{
		if (UPFInputConfig* Cfg = PC->GetInputConfig())
		{
			Cfg->SetLookSensitivity(Sens);
		}
	}
}
