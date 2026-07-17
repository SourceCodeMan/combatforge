// Copyright (c) 2026 Tom Chapman. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Core/CombatForgeTypes.h"
#include "PFBuildWheelWidget.generated.h"

class UBorder;
class UCanvasPanel;
class UImage;
class USizeBox;
class UTextBlock;

/**
 * THE build wheel (03 §3): 8 x 45-degree sectors clockwise from top —
 * Wall, Floor, Ramp, Roof, Barrel, Crate, Boxes, Delete.
 * Opened on Q-hold (>= 0.18 s) via UPFRootHUDWidget from
 * UPFBuildComponent::OnBuildWheelRequestedEvent; release commits the hovered
 * sector through OnToolSelectedEvent. Client-only, zero replication.
 *
 * Dead zone r = 90 px (release inside = cancel); select band 90-280 px;
 * hovered sector 1.08x + brighten; center readout "Wall — 23 left";
 * digits 1-8 = instant select (NativeOnKeyDown, T4); selection math =
 * atan2 of accumulated mouse delta in NativeTick.
 */
UCLASS()
class COMBATFORGE_API UPFBuildWheelWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	/** Captures mouse to the wheel cursor; camera frozen while open (03 §3). */
	void Open();

	/** Release: hovered sector -> OnToolSelectedEvent; inside dead zone = cancel. */
	void CloseAndCommit();

	/** Close without committing (phase change / forced teardown). Intra-package. */
	void CloseCancel();

	bool IsWheelOpen() const { return bWheelOpen; }

	/** Contract §3.6 — UPFRootHUDWidget wires this to UPFBuildComponent::EquipTool. */
	FPFOnToolSelected OnToolSelectedEvent;

protected:
	virtual TSharedRef<SWidget> RebuildWidget() override;
	virtual void NativeOnInitialized() override;
	virtual void NativeDestruct() override;
	virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;
	virtual FReply NativeOnKeyDown(const FGeometry& InGeometry, const FKeyEvent& InKeyEvent) override;

private:
	void BuildTree();
	void CloseInternal();
	void CommitSector(int32 SectorIndex);
	void SetHoveredSector(int32 NewIndex);
	void ApplySectorVisual(int32 SectorIndex, bool bHovered);
	void UpdateCenterReadout();
	FText SectorReadout(int32 SectorIndex) const;

	static const TCHAR* ToolDisplayName(EPFBuildTool Tool);

	UPROPERTY() TObjectPtr<UCanvasPanel> RootCanvas;
	UPROPERTY() TObjectPtr<UTextBlock> CenterReadout;
	UPROPERTY() TObjectPtr<UImage> CursorDot;
	UPROPERTY() TArray<TObjectPtr<UBorder>> SectorSwatches;
	UPROPERTY() TArray<TObjectPtr<UTextBlock>> SectorNameTexts;

	/** Accumulated (screen-space) mouse delta since Open; drives hover + cursor dot. */
	FVector2D AccumDelta = FVector2D::ZeroVector;
	int32 HoveredSector = INDEX_NONE;
	bool bWheelOpen = false;
	/** Tracks our SetIgnoreLookInput(true) so we always pair the release. */
	bool bLookInputIgnored = false;

	static constexpr int32 NumSectors = 12;   // Wall..Delete including window/door/trap
	static constexpr float DeadZonePx = 90.f;
	static constexpr float SelectMaxPx = 300.f;
	static constexpr float SectorRadiusPx = 200.f;
	/** Raw mouse-delta units -> wheel-cursor pixels. */
	static constexpr float MouseToCursorScale = 1.5f;
};
