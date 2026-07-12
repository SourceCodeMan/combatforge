// Copyright (c) 2026 Tom Chapman. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Core/PaintForgeTypes.h"
#include "PFLoadingMenuWidget.generated.h"

class UButton;
class UCheckBox;
class UImage;
class UProgressBar;
class UTextBlock;
class UVerticalBox;
class UWidgetSwitcher;

/**
 * Full-viewport boot menu — NOT a live-game screenshot with HUD.
 * Tabs: Match Setup (mode / type / format / bots) and How to Play.
 * Host picks match config here before Enter Lobby.
 */
UCLASS()
class PAINTFORGE_API UPFLoadingMenuWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	/** True once warmup finished and the player dismissed the menu. */
	bool IsFinished() const { return bDismissed; }

	/** True once shaders/assets are ready (Enter button live). */
	bool IsWarmupComplete() const { return bWarmupComplete; }

protected:
	virtual TSharedRef<SWidget> RebuildWidget() override;
	virtual void NativeConstruct() override;
	virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;

private:
	void BuildTree();
	void BuildHowToPlayPage(UVerticalBox* Box);
	void AddHowToLine(UVerticalBox* Box, const FString& Text, int32 Size, bool bBold, const FLinearColor& Color);
	UButton* MakeSetupButton(const FString& Label, TObjectPtr<UTextBlock>& OutValueText, FName Name);
	UButton* MakeMenuTab(const FString& Label, FName Name);
	void SelectMenuTab(int32 Index);
	void RunWarmupStep();
	void SetStatus(const FString& Line);
	void FinishWarmup();
	void SeedFromGameState();
	void RefreshSetupLabels();
	void ApplySelectionsToHost();
	bool IsLocalHost() const;

	UFUNCTION() void OnEnterClicked();
	UFUNCTION() void OnModeClicked();
	UFUNCTION() void OnTypeClicked();
	UFUNCTION() void OnFormatClicked();
	UFUNCTION() void OnBotsChanged(bool bIsChecked);
	UFUNCTION() void OnTabSetup();
	UFUNCTION() void OnTabHowTo();

	UPROPERTY() TObjectPtr<UImage> Backdrop;
	UPROPERTY() TObjectPtr<UTextBlock> TitleText;
	UPROPERTY() TObjectPtr<UTextBlock> SubtitleText;
	UPROPERTY() TObjectPtr<UTextBlock> StatusText;
	UPROPERTY() TObjectPtr<UProgressBar> ProgressBar;
	UPROPERTY() TObjectPtr<UButton> EnterButton;
	UPROPERTY() TObjectPtr<UTextBlock> EnterLabel;

	// Top-level menu tabs: 0 = Match Setup, 1 = How to Play
	UPROPERTY() TObjectPtr<UButton> TabSetup;
	UPROPERTY() TObjectPtr<UButton> TabHowTo;
	UPROPERTY() TObjectPtr<UWidgetSwitcher> MenuSwitcher;

	// Pre-game match setup (host-editable).
	UPROPERTY() TObjectPtr<UButton> ModeButton;
	UPROPERTY() TObjectPtr<UTextBlock> ModeValueText;
	UPROPERTY() TObjectPtr<UTextBlock> ModeBlurbText;
	UPROPERTY() TObjectPtr<UButton> TypeButton;
	UPROPERTY() TObjectPtr<UTextBlock> TypeValueText;
	UPROPERTY() TObjectPtr<UTextBlock> TypeBlurbText;
	UPROPERTY() TObjectPtr<UButton> FormatButton;
	UPROPERTY() TObjectPtr<UTextBlock> FormatValueText;
	UPROPERTY() TObjectPtr<UTextBlock> FormatBlurbText;
	UPROPERTY() TObjectPtr<UCheckBox> BotsCheck;
	UPROPERTY() TObjectPtr<UTextBlock> BotsLabelText;
	UPROPERTY() TObjectPtr<UTextBlock> SetupHintText;

	EPFBuildMode SelectedBuildMode = EPFBuildMode::Creative;
	EPFMatchType SelectedMatchType = EPFMatchType::Skirmish;
	uint8 SelectedTeamSize = 4;   // 4 or 6
	bool bSelectedFillBots = true;
	int32 ActiveMenuTab = 0;

	int32 WarmupStep = 0;
	float Progress = 0.f;
	float ShaderWaitAccum = 0.f;
	bool bWarmupComplete = false;
	bool bDismissed = false;

	/** Soft paths to force-load so first in-game hit doesn't compile mid-fight. */
	TArray<FString> PreloadPaths;
	int32 PreloadIndex = 0;
};
