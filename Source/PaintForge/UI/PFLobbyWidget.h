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
 * Lobby panel (contract §3.6): roster rows from GameState->PlayerArray
 * (polled at 0.5 s — PlayerArray has no delegate); per row: name, team color
 * chip, ready check. Host rows clickable -> PC->ServerHostCycleTeam (T22).
 * Footer hints: "F = Ready", host: "Enter = Start". Tab-hold cursor is the
 * PlayerController's job (input mode); this widget just keeps rows clickable.
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

	// Host match-setup controls (server re-validates host-ness; these are UX gating only).
	UFUNCTION() void OnModeClicked();
	UFUNCTION() void OnTypeClicked();
	UFUNCTION() void OnFormatClicked();
	UFUNCTION() void OnLoadoutClicked();
	UFUNCTION() void OnLoadoutClose();

private:
	void BuildTree();
	void BuildConfigPanel(UCanvasPanel* RootCanvas);
	void BuildLoadoutOverlay(UCanvasPanel* RootCanvas);
	UButton* MakeConfigButton(const FString& Label, TObjectPtr<UTextBlock>& OutValueText);
	void RefreshRoster();
	void RefreshConfig();
	bool IsLocalHost() const;

	UPROPERTY() TObjectPtr<UTextBlock> TitleText;
	UPROPERTY() TObjectPtr<UTextBlock> CountdownText;
	UPROPERTY() TObjectPtr<UTextBlock> FooterText;
	UPROPERTY() TObjectPtr<UVerticalBox> RosterBox;

	// Match-setup panel (host-editable, replicated on GameState so everyone sees the selection).
	UPROPERTY() TObjectPtr<UTextBlock> ModeValueText;
	UPROPERTY() TObjectPtr<UTextBlock> TypeValueText;
	UPROPERTY() TObjectPtr<UTextBlock> FormatValueText;
	UPROPERTY() TObjectPtr<UTextBlock> ConfigHintText;
	UPROPERTY() TObjectPtr<UBorder> LoadoutOverlay;   // stub screen (nothing to equip yet)

	float PollAccum = 0.f;
	/** Cheap change detection so rows are only rebuilt when the roster actually changed. */
	FString LastRosterSignature;

	static constexpr float PollInterval = 0.5f;
};
