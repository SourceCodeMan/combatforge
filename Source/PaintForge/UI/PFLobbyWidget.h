// Copyright (c) 2026 Tom Chapman. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Components/Button.h"
#include "PFLobbyWidget.generated.h"

class APaintForgePlayerState;
class UPFLobbyWidget;
class UBorder;
class UButton;
class UCanvasPanel;
class UTextBlock;
class UVerticalBox;

/**
 * Internal roster-row button: carries the PlayerState it represents so a single
 * dynamic OnClicked handler can route back to the lobby widget (host click-cycles
 * the row's team, T22). Intra-package helper — not part of the cross-package contract.
 */
UCLASS()
class PAINTFORGE_API UPFLobbyRowButton : public UButton
{
	GENERATED_BODY()

public:
	void InitRow(UPFLobbyWidget* InOwner, APaintForgePlayerState* InPlayerState);

protected:
	UFUNCTION()
	void HandleRowClicked();

private:
	TWeakObjectPtr<UPFLobbyWidget> OwnerWidget;
	TWeakObjectPtr<APaintForgePlayerState> RowPlayerState;
};

/**
 * Lobby panel (contract §3.6): roster + match setup display + loadout / options.
 */
UCLASS()
class PAINTFORGE_API UPFLobbyWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	/** Row-button callback target. Server re-validates host-ness; this is UX gating only. */
	void NotifyRowClicked(APaintForgePlayerState* ClickedPlayerState);

protected:
	virtual TSharedRef<SWidget> RebuildWidget() override;
	virtual void NativeConstruct() override;
	virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;

	UFUNCTION() void OnLoadoutClicked();
	UFUNCTION() void OnLoadoutClose();
	UFUNCTION() void OnLoadoutApply();
	UFUNCTION() void OnCrosshairCycle();
	UFUNCTION() void OnOptionsClicked();

private:
	void BuildTree();
	void BuildConfigPanel(UCanvasPanel* RootCanvas);
	void BuildLoadoutOverlay(UCanvasPanel* RootCanvas);
	UButton* MakeConfigButton(const FString& Label, TObjectPtr<UTextBlock>& OutValueText);
	void AddReadOnlyRow(UVerticalBox* Box, const FString& Label, TObjectPtr<UTextBlock>& OutValueText);
	void RefreshRoster();
	void RefreshConfig();
	void RefreshLoadoutLabels();
	void ApplyLoadoutPrefs();
	bool IsLocalHost() const;

	static const TCHAR* CrosshairStyleName(int32 Idx);

	UPROPERTY() TObjectPtr<UTextBlock> TitleText;
	UPROPERTY() TObjectPtr<UTextBlock> CountdownText;
	UPROPERTY() TObjectPtr<UTextBlock> FooterText;
	UPROPERTY() TObjectPtr<UVerticalBox> RosterBox;

	UPROPERTY() TObjectPtr<UTextBlock> ModeValueText;
	UPROPERTY() TObjectPtr<UTextBlock> TypeValueText;
	UPROPERTY() TObjectPtr<UTextBlock> FormatValueText;
	UPROPERTY() TObjectPtr<UTextBlock> BotsValueText;
	UPROPERTY() TObjectPtr<UTextBlock> MapValueText;
	UPROPERTY() TObjectPtr<UTextBlock> ConfigHintText;
	UPROPERTY() TObjectPtr<UBorder> LoadoutOverlay;

	UPROPERTY() TObjectPtr<UButton> CrosshairButton;
	UPROPERTY() TObjectPtr<UTextBlock> CrosshairValueText;
	UPROPERTY() TObjectPtr<UTextBlock> LoadoutHintText;
	UPROPERTY() TObjectPtr<UTextBlock> LoadoutSummaryText;

	int32 WorkingCrosshairStyle = 0;

	float PollAccum = 0.f;
	FString LastRosterSignature;

	static constexpr float PollInterval = 0.5f;
};
