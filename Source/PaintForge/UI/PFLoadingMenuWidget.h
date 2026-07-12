// Copyright (c) 2026 Tom Chapman. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "PFLoadingMenuWidget.generated.h"

class UButton;
class UImage;
class UProgressBar;
class UTextBlock;

/**
 * Full-viewport boot menu — NOT a live-game screenshot with HUD.
 * Opaque backdrop + title + status + progress. Runs asset preload and shader/PSO
 * warmup while the player stares at the menu, then enables Enter to drop into lobby.
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
	void RunWarmupStep();
	void SetStatus(const FString& Line);
	void FinishWarmup();

	UFUNCTION() void OnEnterClicked();

	UPROPERTY() TObjectPtr<UImage> Backdrop;
	UPROPERTY() TObjectPtr<UTextBlock> TitleText;
	UPROPERTY() TObjectPtr<UTextBlock> SubtitleText;
	UPROPERTY() TObjectPtr<UTextBlock> StatusText;
	UPROPERTY() TObjectPtr<UProgressBar> ProgressBar;
	UPROPERTY() TObjectPtr<UButton> EnterButton;
	UPROPERTY() TObjectPtr<UTextBlock> EnterLabel;

	int32 WarmupStep = 0;
	float Progress = 0.f;
	float ShaderWaitAccum = 0.f;
	bool bWarmupComplete = false;
	bool bDismissed = false;

	/** Soft paths to force-load so first in-game hit doesn't compile mid-fight. */
	TArray<FString> PreloadPaths;
	int32 PreloadIndex = 0;
};
