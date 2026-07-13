// Copyright (c) 2026 Tom Chapman. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
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
 * Escape toggles via PC; Apply writes UGameUserSettings + PaintForge ini.
 */
UCLASS()
class PAINTFORGE_API UPFOptionsWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	void Open();
	void Close();
	bool IsOpen() const { return bOpen; }
	void OpenHowToPlay();

protected:
	virtual TSharedRef<SWidget> RebuildWidget() override;
	virtual void NativeConstruct() override;

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
	UFUNCTION() void OnFullscreenChanged(bool bIsChecked);
	UFUNCTION() void OnVSyncChanged(bool bIsChecked);
	UFUNCTION() void OnInvertYChanged(bool bIsChecked);
	UFUNCTION() void OnMasterVolChanged(float Value);
	UFUNCTION() void OnSfxVolChanged(float Value);
	UFUNCTION() void OnAmbientVolChanged(float Value);
	UFUNCTION() void OnSensChanged(float Value);
	UFUNCTION() void OnFovChanged(float Value);
	UFUNCTION() void OnResScaleChanged(float Value);

private:
	void BuildTree();
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
	void ApplyMasterVolume(float Linear01);
	void ApplySfxVolume(float Linear01);
	void ApplyLookSensitivity(float Sens);
	void ApplyInvertY(bool bInvert);
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
	UPROPERTY() TObjectPtr<UButton> ResolutionButton;
	UPROPERTY() TObjectPtr<UTextBlock> ResolutionValueText;
	UPROPERTY() TObjectPtr<UCheckBox> FullscreenCheck;
	UPROPERTY() TObjectPtr<UCheckBox> VSyncCheck;
	UPROPERTY() TObjectPtr<UCheckBox> InvertYCheck;
	UPROPERTY() TObjectPtr<USlider> ResScaleSlider;
	UPROPERTY() TObjectPtr<UTextBlock> ResScaleValueText;
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
	int32  WorkingWindowMode = 0;   // 0 FS 1 Borderless 2 Windowed
	int32  WorkingResIndex = 0;
	bool   bWorkingFullscreen = true;
	bool   bWorkingVSync = true;
	bool   bWorkingInvertY = false;
	float  WorkingResScale = 100.f;
	float  WorkingMasterVol = 1.f;
	float  WorkingSfxVol = 1.f;
	float  WorkingAmbientVol = 0.22f;
	float  WorkingSens = 1.f;
	float  WorkingFov = 105.f;

	bool bOpen = false;
	int32 ActiveTab = 0;

	static constexpr int32 NumResolutions = 6;
};
