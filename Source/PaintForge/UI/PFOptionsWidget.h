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
 * Full-screen options / pause settings: Video, Audio, Controls, How to Play.
 * Escape toggles via APaintForgePlayerController; Apply writes UGameUserSettings + ini.
 */
UCLASS()
class PAINTFORGE_API UPFOptionsWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	void Open();
	void Close();
	bool IsOpen() const { return bOpen; }

	/** Open options and jump straight to the How to Play tab. */
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
	UFUNCTION() void OnQualityClicked();
	UFUNCTION() void OnFullscreenChanged(bool bIsChecked);
	UFUNCTION() void OnVSyncChanged(bool bIsChecked);
	UFUNCTION() void OnMasterVolChanged(float Value);
	UFUNCTION() void OnSfxVolChanged(float Value);
	UFUNCTION() void OnSensChanged(float Value);
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

	static const TCHAR* QualityName(int32 Level);

	UPROPERTY() TObjectPtr<UImage> Dimmer;
	UPROPERTY() TObjectPtr<UWidgetSwitcher> PageSwitcher;
	UPROPERTY() TObjectPtr<UButton> TabVideo;
	UPROPERTY() TObjectPtr<UButton> TabAudio;
	UPROPERTY() TObjectPtr<UButton> TabControls;
	UPROPERTY() TObjectPtr<UButton> TabHowTo;
	UPROPERTY() TObjectPtr<UButton> ApplyButton;
	UPROPERTY() TObjectPtr<UButton> BackButton;
	UPROPERTY() TObjectPtr<UButton> QualityButton;
	UPROPERTY() TObjectPtr<UTextBlock> QualityValueText;
	UPROPERTY() TObjectPtr<UCheckBox> FullscreenCheck;
	UPROPERTY() TObjectPtr<UCheckBox> VSyncCheck;
	UPROPERTY() TObjectPtr<USlider> ResScaleSlider;
	UPROPERTY() TObjectPtr<UTextBlock> ResScaleValueText;
	UPROPERTY() TObjectPtr<USlider> MasterVolSlider;
	UPROPERTY() TObjectPtr<UTextBlock> MasterVolValueText;
	UPROPERTY() TObjectPtr<USlider> SfxVolSlider;
	UPROPERTY() TObjectPtr<UTextBlock> SfxVolValueText;
	UPROPERTY() TObjectPtr<USlider> SensSlider;
	UPROPERTY() TObjectPtr<UTextBlock> SensValueText;
	UPROPERTY() TObjectPtr<UTextBlock> TitleText;
	UPROPERTY() TObjectPtr<UTextBlock> HintText;

	// Working copy until Apply.
	int32  WorkingQuality = 2;       // 0..3  Low..Epic
	bool   bWorkingFullscreen = true;
	bool   bWorkingVSync = true;
	float  WorkingResScale = 100.f;  // 50..100
	float  WorkingMasterVol = 1.f;
	float  WorkingSfxVol = 1.f;
	float  WorkingSens = 1.f;

	bool bOpen = false;
	int32 ActiveTab = 0;
};
