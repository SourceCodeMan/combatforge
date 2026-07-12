// Copyright (c) 2026 Tom Chapman. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Components/Button.h"
#include "Core/PaintForgeTypes.h"
#include "Voting/PFRatingSubsystem.h"
#include "PFLoadingMenuWidget.generated.h"

class UButton;
class UCheckBox;
class UImage;
class UProgressBar;
class UTextBlock;
class UVerticalBox;
class UWidgetSwitcher;
class UPFLoadingMenuWidget;

/** Row button for one community map slot (0..9 on the current page). */
UCLASS()
class PAINTFORGE_API UPFMapPickButton : public UButton
{
	GENERATED_BODY()

public:
	void InitRow(UPFLoadingMenuWidget* InOwner, int32 InSlotIndex);

protected:
	UFUNCTION() void HandleClicked();

private:
	TWeakObjectPtr<UPFLoadingMenuWidget> OwnerWidget;
	int32 SlotIndex = 0;
};

/**
 * Full-viewport boot menu — NOT a live-game screenshot with HUD.
 * Tabs: Match Setup (mode / type / format / bots / community map) and How to Play.
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

	/** Map-row click from UPFMapPickButton (slot 0..9 on current page). */
	void NotifyMapSlotClicked(int32 SlotIndex);

protected:
	virtual TSharedRef<SWidget> RebuildWidget() override;
	virtual void NativeConstruct() override;
	virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;

private:
	void BuildTree();
	void BuildHowToPlayPage(UVerticalBox* Box);
	void BuildMapPicker(UVerticalBox* Parent);
	void AddHowToLine(UVerticalBox* Box, const FString& Text, int32 Size, bool bBold, const FLinearColor& Color);
	UButton* MakeSetupButton(const FString& Label, TObjectPtr<UTextBlock>& OutValueText, FName Name);
	UButton* MakeMenuTab(const FString& Label, FName Name);
	void SelectMenuTab(int32 Index);
	void RunWarmupStep();
	void SetStatus(const FString& Line);
	void FinishWarmup();
	void SeedFromGameState();
	void RefreshSetupLabels();
	void RefreshMapPicker();
	void ReloadMapCatalog();
	void ApplySelectionsToHost();
	void ApplyMapSelectionToHost();
	bool IsLocalHost() const;
	bool NeedsCommunityMap() const;

	UFUNCTION() void OnEnterClicked();
	UFUNCTION() void OnModeClicked();
	UFUNCTION() void OnTypeClicked();
	UFUNCTION() void OnFormatClicked();
	UFUNCTION() void OnBotsChanged(bool bIsChecked);
	UFUNCTION() void OnTabSetup();
	UFUNCTION() void OnTabHowTo();
	UFUNCTION() void OnMapPagePrev();
	UFUNCTION() void OnMapPageNext();
	UFUNCTION() void OnMapAutoClicked();
	UFUNCTION() void OnQuickStartClicked();

	/** Apply Play-Only + Skirmish + 4v4 + bots + auto map (first-session default). */
	void ApplyQuickStartPreset();

	UPROPERTY() TObjectPtr<UImage> Backdrop;
	UPROPERTY() TObjectPtr<UTextBlock> TitleText;
	UPROPERTY() TObjectPtr<UTextBlock> SubtitleText;
	UPROPERTY() TObjectPtr<UButton> QuickStartButton;
	UPROPERTY() TObjectPtr<UTextBlock> QuickStartLabel;
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

	// Community map picker (Improvement / Play-only).
	UPROPERTY() TObjectPtr<UVerticalBox> MapPickerBox;
	UPROPERTY() TObjectPtr<UTextBlock> MapPickerHeader;
	UPROPERTY() TObjectPtr<UTextBlock> MapPageLabel;
	UPROPERTY() TObjectPtr<UButton> MapPagePrevBtn;
	UPROPERTY() TObjectPtr<UButton> MapPageNextBtn;
	UPROPERTY() TObjectPtr<UButton> MapAutoBtn;
	UPROPERTY() TObjectPtr<UTextBlock> MapSelectedLabel;
	UPROPERTY() TArray<TObjectPtr<UPFMapPickButton>> MapSlotButtons;
	UPROPERTY() TArray<TObjectPtr<UTextBlock>> MapSlotLabels;

	EPFBuildMode SelectedBuildMode = EPFBuildMode::Creative;
	EPFMatchType SelectedMatchType = EPFMatchType::Skirmish;
	uint8 SelectedTeamSize = 4;   // 4 or 6
	bool bSelectedFillBots = true;
	int32 ActiveMenuTab = 0;

	TArray<FPFCommunityMapInfo> MapCatalog;
	int32 MapPageIndex = 0;
	/** Index into MapCatalog, or INDEX_NONE for auto top-ranked. */
	int32 SelectedMapCatalogIndex = INDEX_NONE;
	static constexpr int32 MapsPerPage = 10;
	static constexpr int32 MaxMaps = 100;

	int32 WarmupStep = 0;
	float Progress = 0.f;
	float ShaderWaitAccum = 0.f;
	bool bWarmupComplete = false;
	bool bDismissed = false;

	/** Soft paths to force-load so first in-game hit doesn't compile mid-fight. */
	TArray<FString> PreloadPaths;
	int32 PreloadIndex = 0;
};
