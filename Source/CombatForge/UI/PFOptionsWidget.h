// Copyright (c) 2026 Tom Chapman. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "InputCoreTypes.h"   // FKey
#include "PFOptionsWidget.generated.h"

class UButton;
class UCheckBox;
class UImage;
class USlider;
class UTextBlock;
class UVerticalBox;
class UWidgetSwitcher;

/**
 * Full-screen options: Video, Audio, Controls, How to Play.
 * Escape toggles via PC; Apply writes UGameUserSettings + CombatForge ini.
 */
UCLASS()
class COMBATFORGE_API UPFOptionsWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	void Open();
	void Close();
	bool IsOpen() const { return bOpen; }
	void OpenHowToPlay();
	/** Render inline as a menu tab instead of a full-screen overlay: drop the dimmer, title, and
	 *  Back/Quit chrome, seed the settings, and stay visible without touching input mode. */
	void EnterEmbeddedMode();

protected:
	virtual TSharedRef<SWidget> RebuildWidget() override;
	virtual void NativeConstruct() override;
	virtual void NativeDestruct() override;   // embedded instance saves working settings on teardown
	virtual FReply NativeOnKeyDown(const FGeometry& InGeometry, const FKeyEvent& InKeyEvent) override;

	// One click handler per rebind row (UButton::OnClicked takes no payload). Each begins key capture.
	UFUNCTION() void OnRebind0();
	UFUNCTION() void OnRebind1();
	UFUNCTION() void OnRebind2();
	UFUNCTION() void OnRebind3();
	UFUNCTION() void OnRebind4();
	UFUNCTION() void OnRebind5();
	UFUNCTION() void OnRebind6();
	UFUNCTION() void OnResetBinds();

	UFUNCTION() void OnTabVideo();
	UFUNCTION() void OnTabAudio();
	UFUNCTION() void OnTabControls();
	UFUNCTION() void OnTabHowTo();
	UFUNCTION() void OnApplyClicked();
	UFUNCTION() void OnBackClicked();
	UFUNCTION() void OnQuitToMenuClicked();
	UFUNCTION() void OnQuitToDesktopClicked();
	UFUNCTION() void OnQualityClicked();
	UFUNCTION() void OnWindowModeClicked();
	UFUNCTION() void OnResolutionClicked();
	UFUNCTION() void OnFpsLimitClicked();
	UFUNCTION() void OnFullscreenChanged(bool bIsChecked);
	UFUNCTION() void OnVSyncChanged(bool bIsChecked);
	UFUNCTION() void OnInvertYChanged(bool bIsChecked);
	UFUNCTION() void OnADSToggleChanged(bool bIsChecked);
	UFUNCTION() void OnClassClicked();   // cycle the active class slot + apply to the local pawn (in-game)
	UFUNCTION() void OnMasterVolChanged(float Value);
	UFUNCTION() void OnSfxVolChanged(float Value);
	UFUNCTION() void OnAmbientVolChanged(float Value);
	UFUNCTION() void OnSensChanged(float Value);
	UFUNCTION() void OnFovChanged(float Value);
	UFUNCTION() void OnResScaleChanged(float Value);
	UFUNCTION() void OnBrightnessChanged(float Value);
	UFUNCTION() void OnContrastChanged(float Value);

private:
	void BuildTree();
	void ApplyEmbeddedChrome();   // collapse the dimmer/title/Back/Quit so only the settings card shows
	void BuildVideoPage(UWidget* ParentBox);
	void BuildAudioPage(UWidget* ParentBox);
	void BuildControlsPage(UWidget* ParentBox);
	void BuildHowToPlayPage(UWidget* ParentBox);
	void AddHowToLine(UVerticalBox* Box, const FString& Text, int32 Size, bool bBold, const FLinearColor& Color);
	UButton* MakeTabButton(const FString& Label, FName Name);
	void SelectTab(int32 Index);
	void PullFromSettings();
	void PushToSettings(bool bSave);
	void RefreshLabels();
	void BuildKeyBindRows(UVerticalBox* Box);
	void BeginListen(int32 Index);
	void RefreshRebindLabels();
	void ApplyKeyBinds();
	static constexpr int32 NumRebinds = 7;
	void ApplyMasterVolume(float Linear01);
	void ApplySfxVolume(float Linear01);
	void ApplyBrightnessContrast(float EV, float Contrast);   // → PFLightingSubsystem::SetUserGrade
	void ApplyLookSensitivity(float Sens);
	void ApplyInvertY(bool bInvert);
	void ApplyADSToggle(bool bToggle);
	void ApplyFieldOfView(float Fov);
	void ApplyWindowAndResolution();

	static const TCHAR* QualityName(int32 Level);
	static const TCHAR* WindowModeName(int32 Idx);
	FString ResolutionLabel() const;

	UPROPERTY() TObjectPtr<UImage> Dimmer;
	UPROPERTY() TObjectPtr<UWidgetSwitcher> PageSwitcher;
	UPROPERTY() TObjectPtr<UButton> TabVideo;
	UPROPERTY() TObjectPtr<UButton> TabAudio;
	UPROPERTY() TObjectPtr<UButton> TabControls;
	UPROPERTY() TObjectPtr<UButton> TabHowTo;
	UPROPERTY() TObjectPtr<UButton> ApplyButton;
	UPROPERTY() TObjectPtr<UButton> BackButton;
	UPROPERTY() TObjectPtr<UButton> QuitMenuButton;
	UPROPERTY() TObjectPtr<UButton> QuitDesktopButton;
	UPROPERTY() TObjectPtr<UButton> QualityButton;
	UPROPERTY() TObjectPtr<UTextBlock> QualityValueText;
	UPROPERTY() TObjectPtr<UButton> WindowModeButton;
	UPROPERTY() TObjectPtr<UTextBlock> WindowModeValueText;
	// In-game class quick-switch (Controls page). Hidden in the boot-menu embedded options (ApplyEmbeddedChrome).
	UPROPERTY() TObjectPtr<class UHorizontalBox> ClassRow;
	UPROPERTY() TObjectPtr<UButton> ClassButton;
	UPROPERTY() TObjectPtr<UTextBlock> ClassValueText;
	UPROPERTY() TObjectPtr<UButton> ResolutionButton;
	UPROPERTY() TObjectPtr<UTextBlock> ResolutionValueText;
	UPROPERTY() TObjectPtr<UButton> FpsLimitButton;
	UPROPERTY() TObjectPtr<UTextBlock> FpsLimitValueText;
	UPROPERTY() TObjectPtr<UCheckBox> FullscreenCheck;
	UPROPERTY() TObjectPtr<UCheckBox> VSyncCheck;
	UPROPERTY() TObjectPtr<UCheckBox> InvertYCheck;
	UPROPERTY() TObjectPtr<UCheckBox> ADSToggleCheck;
	UPROPERTY() TObjectPtr<USlider> ResScaleSlider;
	UPROPERTY() TObjectPtr<UTextBlock> ResScaleValueText;
	UPROPERTY() TObjectPtr<USlider> BrightnessSlider;
	UPROPERTY() TObjectPtr<UTextBlock> BrightnessValueText;
	UPROPERTY() TObjectPtr<USlider> ContrastSlider;
	UPROPERTY() TObjectPtr<UTextBlock> ContrastValueText;
	UPROPERTY() TObjectPtr<USlider> MasterVolSlider;
	UPROPERTY() TObjectPtr<UTextBlock> MasterVolValueText;
	UPROPERTY() TObjectPtr<USlider> SfxVolSlider;
	UPROPERTY() TObjectPtr<UTextBlock> SfxVolValueText;
	UPROPERTY() TObjectPtr<USlider> AmbientVolSlider;
	UPROPERTY() TObjectPtr<UTextBlock> AmbientVolValueText;
	UPROPERTY() TObjectPtr<USlider> SensSlider;
	UPROPERTY() TObjectPtr<UTextBlock> SensValueText;
	UPROPERTY() TObjectPtr<USlider> FovSlider;
	UPROPERTY() TObjectPtr<UTextBlock> FovValueText;
	UPROPERTY() TObjectPtr<UTextBlock> TitleText;
	UPROPERTY() TObjectPtr<UTextBlock> HintText;

	int32  WorkingQuality = 2;
	int32  WorkingFpsIndex = 2;   // {60,120,144,240,Uncapped}, default 144
	int32  WorkingWindowMode = 0;   // 0 FS 1 Borderless 2 Windowed
	int32  WorkingResIndex = 0;
	bool   bWorkingFullscreen = true;
	bool   bWorkingVSync = true;
	bool   bWorkingInvertY = false;
	bool   bWorkingADSToggle = false;
	float  WorkingResScale = 100.f;
	float  WorkingBrightness = 0.f;    // EV offset, -1..+1
	float  WorkingContrast = 1.f;      // scale, 0.85..1.20
	float  WorkingMasterVol = 1.f;
	float  WorkingSfxVol = 1.f;
	float  WorkingAmbientVol = 0.22f;
	float  WorkingSens = 1.f;
	float  WorkingFov = 105.f;

	bool bOpen = false;
	bool bEmbedded = false;   // hosted inline as a menu tab (not a full-screen overlay)
	int32 ActiveTab = 0;

	// ---- Key rebinding ----
	UPROPERTY() TArray<TObjectPtr<UButton>> RebindButtons;
	UPROPERTY() TObjectPtr<UButton> ResetBindsButton;
	UPROPERTY() TArray<TObjectPtr<UTextBlock>> RebindKeyLabels;
	TArray<FName> RebindIds;                 // parallel to the rows, filled in BuildKeyBindRows
	TMap<FName, FKey> WorkingBinds;           // pending (unapplied) key per action
	TMap<FName, FKey> DefaultBinds;           // shipped defaults, for reset + clearing overrides
	bool bListeningForKey = false;
	int32 ListeningIndex = -1;

	static constexpr int32 NumResolutions = 5;   // capped at 1440p — 4K murders framerate on this content for no gameplay gain
};
