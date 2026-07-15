// Copyright (c) 2026 Tom Chapman. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Components/Button.h"
#include "Core/CombatForgeTypes.h"
#include "Voting/PFRatingSubsystem.h"
#include "Player/PFCharacterCustomization.h"   // FPFCharacterConfig
#include "Combat/PFWeaponCatalog.h"            // FPFWeaponConfig
#include "PFLoadingMenuWidget.generated.h"

class UButton;
class UCheckBox;
class UImage;
class UTexture2D;
class UProgressBar;
class UTextBlock;
class UVerticalBox;
class UWidgetSwitcher;
class UPFLoadingMenuWidget;
class APFCharacterPreviewActor;
class UPFOptionsWidget;

/** Row button for one community map slot (0..9 on the current page). */
UCLASS()
class COMBATFORGE_API UPFMapPickButton : public UButton
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

/** Prev/next stepper for one character-customization slot (payload button; avoids per-row handlers). */
UCLASS()
class COMBATFORGE_API UPFCharSlotButton : public UButton
{
	GENERATED_BODY()

public:
	void InitStep(UPFLoadingMenuWidget* InOwner, int32 InSlot, int32 InDir);

protected:
	UFUNCTION() void HandleClicked();

private:
	TWeakObjectPtr<UPFLoadingMenuWidget> OwnerWidget;
	int32 SlotIndex = 0;
	int32 Dir = 1;
};

/** One of the CoD-style save slots (payload button carrying which slot it selects). */
UCLASS()
class COMBATFORGE_API UPFCharSaveSlotButton : public UButton
{
	GENERATED_BODY()

public:
	void InitSlot(UPFLoadingMenuWidget* InOwner, int32 InSaveSlot);

protected:
	UFUNCTION() void HandleClicked();

private:
	TWeakObjectPtr<UPFLoadingMenuWidget> OwnerWidget;
	int32 SaveSlot = 0;
};

/** Prev/next stepper for the weapon picker: Kind 0 = category, 1 = weapon (payload button). */
UCLASS()
class COMBATFORGE_API UPFWeaponStepButton : public UButton
{
	GENERATED_BODY()

public:
	void InitStep(UPFLoadingMenuWidget* InOwner, int32 InKind, int32 InDir);

protected:
	UFUNCTION() void HandleClicked();

private:
	TWeakObjectPtr<UPFLoadingMenuWidget> OwnerWidget;
	int32 Kind = 0;
	int32 Dir = 1;
};

/** A clickable match-setup card (CoD-style): Kind 0 = build mode, 1 = game mode, 2 = format. */
UCLASS()
class COMBATFORGE_API UPFModeCardButton : public UButton
{
	GENERATED_BODY()

public:
	void InitCard(UPFLoadingMenuWidget* InOwner, int32 InKind, int32 InValue);

protected:
	UFUNCTION() void HandleClicked();

private:
	TWeakObjectPtr<UPFLoadingMenuWidget> OwnerWidget;
	int32 Kind = 0;
	int32 Value = 0;
};

/**
 * Full-viewport boot menu — NOT a live-game screenshot with HUD.
 * Tabs: Match Setup (mode / type / format / bots / community map) and How to Play.
 * Host picks match config here before Enter Lobby.
 */
UCLASS()
class COMBATFORGE_API UPFLoadingMenuWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	/** True once warmup finished and the player dismissed the menu. */
	bool IsFinished() const { return bDismissed; }

	/** True once shaders/assets are ready (Enter button live). */
	bool IsWarmupComplete() const { return bWarmupComplete; }

	/** Map-row click from UPFMapPickButton (slot 0..9 on current page). */
	void NotifyMapSlotClicked(int32 SlotIndex);

	/** Character-customization prev/next step for a slot (Dir -1/+1), cycling through None + parts. */
	void NotifyCharSlotStep(int32 SlotIdx, int32 Dir);

	/** Select a save slot: make it active, load it into the editor + preview + pawn. */
	void NotifySaveSlotSelected(int32 SaveSlot);

	/** Weapon picker step: Kind 0 = category, 1 = weapon; Dir -1/+1. Persists + re-applies to the pawn. */
	void NotifyWeaponStep(int32 Kind, int32 Dir);

	/** Match-setup card clicked: Kind 0 = build mode, 1 = game mode, 2 = format; Value = enum/team-size. */
	void NotifyCardSelected(int32 Kind, int32 Value);

protected:
	virtual TSharedRef<SWidget> RebuildWidget() override;
	virtual void NativeConstruct() override;
	virtual void NativeDestruct() override;
	virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;
	virtual FReply NativeOnMouseButtonDown(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent) override;
	virtual FReply NativeOnMouseMove(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent) override;
	virtual FReply NativeOnMouseButtonUp(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent) override;

private:
	void BuildTree();
	void BuildHowToPlayPage(UVerticalBox* Box);
	void BuildLoadoutPage(UVerticalBox* Col);
	void RefreshLoadoutLabels();
	void BuildCharacterPage(UVerticalBox* Col);
	void RefreshCharacterLabels();
	/** Highlight the active save-slot button. */
	void RefreshSaveSlotHighlight();
	/** Lazily spawn the off-screen preview studio and bind its render target to the tab image. */
	void EnsureCharPreview();
	static const TCHAR* CrosshairStyleName(int32 Idx);
	void BuildMapPicker(UVerticalBox* Parent);
	/** Load (and cache) the screenshot-on-publish preview PNG for a saved arena, or null if none exists. */
	UTexture2D* GetMapPreview(const FString& JsonFileName);
	void AddHowToLine(UVerticalBox* Box, const FString& Text, int32 Size, bool bBold, const FLinearColor& Color);
	UButton* MakeSetupButton(const FString& Label, TObjectPtr<UTextBlock>& OutValueText, FName Name);
	UButton* MakeMenuTab(const FString& Label, FName Name);
	void SelectMenuTab(int32 Index);
	void RunWarmupStep();
	void SetStatus(const FString& Line);
	void FinishWarmup();
	void SeedFromGameState();
	void RefreshSetupLabels();
	void BuildSetupCards(UVerticalBox* Col);   // CoD-style description panel + mode/type/format card rows
	void RefreshSetupCards();                  // highlight the selected cards + update the description
	void RefreshMapPicker();
	void ReloadMapCatalog();
	void ApplySelectionsToHost();
	void ApplyMapSelectionToHost();
	bool IsLocalHost() const;
	bool NeedsCommunityMap() const;

	UFUNCTION() void OnEnterClicked();
	UFUNCTION() void OnQuitDesktopClicked();
	UFUNCTION() void OnModeClicked();
	UFUNCTION() void OnTypeClicked();
	UFUNCTION() void OnFormatClicked();
	UFUNCTION() void OnBotsChanged(bool bIsChecked);
	UFUNCTION() void OnTabSetup();
	UFUNCTION() void OnTabHowTo();
	UFUNCTION() void OnTabLoadout();
	UFUNCTION() void OnTabCharacter();
	UFUNCTION() void OnOptionsClicked();   // opens the options overlay above the boot menu
	UFUNCTION() void OnCrosshairCycle();
	UFUNCTION() void OnMapPagePrev();
	UFUNCTION() void OnMapPageNext();
	UFUNCTION() void OnMapAutoClicked();
	UFUNCTION() void OnQuickStartClicked();
	UFUNCTION() void OnHostLanClicked();   // relaunch this map as a LISTEN server (LAN/VPN friends can join)
	UFUNCTION() void OnJoinLanClicked();   // connect to the host IP typed in the join box

	/** Apply Play-Only + Skirmish + 4v4 + bots + auto map (first-session default). */
	void ApplyQuickStartPreset();

	UPROPERTY() TObjectPtr<UImage> Backdrop;
	UPROPERTY() TObjectPtr<UTextBlock> TitleText;
	UPROPERTY() TObjectPtr<UTextBlock> SubtitleText;
	UPROPERTY() TObjectPtr<UButton> QuickStartButton;
	UPROPERTY() TObjectPtr<UButton> HostLanButton;
	UPROPERTY() TObjectPtr<class UEditableTextBox> JoinIpBox;
	UPROPERTY() TObjectPtr<UButton> JoinLanButton;
	UPROPERTY() TObjectPtr<UTextBlock> QuickStartLabel;
	UPROPERTY() TObjectPtr<UTextBlock> StatusText;
	UPROPERTY() TObjectPtr<UProgressBar> ProgressBar;
	UPROPERTY() TObjectPtr<UButton> EnterButton;
	UPROPERTY() TObjectPtr<UTextBlock> EnterLabel;
	UPROPERTY() TObjectPtr<UButton> QuitDesktopButton;
	UPROPERTY() TObjectPtr<UTextBlock> QuitDesktopLabel;

	// Top-level menu tabs: 0 = Match Setup, 1 = How to Play, 2 = Loadout
	UPROPERTY() TObjectPtr<UButton> TabSetup;
	UPROPERTY() TObjectPtr<UButton> TabHowTo;
	UPROPERTY() TObjectPtr<UButton> TabLoadout;
	UPROPERTY() TObjectPtr<UButton> TabCharacter;
	UPROPERTY() TObjectPtr<UButton> OptionsTabButton;
	UPROPERTY() TObjectPtr<UPFOptionsWidget> BootOptions;   // options overlay opened from the boot menu
	UPROPERTY() TObjectPtr<UWidgetSwitcher> MenuSwitcher;

	// Character customization tab (per-slot part selection; saved to config, applied on spawn).
	UPROPERTY() TArray<TObjectPtr<UTextBlock>> CharSlotValueTexts;
	UPROPERTY() TArray<TObjectPtr<UPFCharSaveSlotButton>> SaveSlotButtons;   // the 5 create-a-class slots
	int32 ActiveSaveSlot = 0;
	UPROPERTY() TObjectPtr<UImage> CharPreviewImage;                       // shows the live 3D render target
	UPROPERTY() TObjectPtr<APFCharacterPreviewActor> CharPreviewActor;     // off-screen studio (lazy-spawned)
	FPFCharacterConfig CharConfig;
	bool bPreviewDragging = false;   // left-drag on the preview rotates the character
	float PreviewDragLastX = 0.f;

	// Loadout tab — weapon picker (category + weapon steppers) + local prefs.
	UPROPERTY() TObjectPtr<UTextBlock> WeaponCatValueText;
	UPROPERTY() TObjectPtr<UTextBlock> WeaponValueText;
	FPFWeaponConfig WeaponConfig;
	void BuildWeaponPicker(UVerticalBox* Col);
	void RefreshWeaponLabels();

	// Loadout tab (local prefs: marker fire-rate preset + crosshair style).
	UPROPERTY() TObjectPtr<UButton> LoadoutCrosshairButton;
	UPROPERTY() TObjectPtr<UTextBlock> LoadoutCrosshairValueText;
	int32 WorkingCrosshairStyle = 0;

	// Pre-game match setup (host-editable).
	// CoD-style card selectors (replace the < > steppers): a description panel + one card row per selector.
	UPROPERTY() TObjectPtr<UTextBlock> SetupDescTitle;
	UPROPERTY() TObjectPtr<UTextBlock> SetupDescText;
	UPROPERTY() TArray<TObjectPtr<UPFModeCardButton>> ModeCards;    // index = EPFBuildMode
	UPROPERTY() TArray<TObjectPtr<UPFModeCardButton>> TypeCards;    // index = EPFMatchType
	UPROPERTY() TArray<TObjectPtr<UPFModeCardButton>> FormatCards;  // 0 = 4v4, 1 = 6v6

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
	UPROPERTY() TArray<TObjectPtr<UImage>> MapSlotImages;
	/** filename(.json) -> loaded PNG preview (cached; a null value means "tried, none on disk"). */
	UPROPERTY() TMap<FString, TObjectPtr<UTexture2D>> MapPreviewCache;

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
