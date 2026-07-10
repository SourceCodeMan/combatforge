// Copyright (c) 2026 Tom Chapman. All rights reserved.

#include "UI/PFCombatFeedbackWidget.h"

#include "Combat/PFCombatAudio.h"
#include "Combat/PFHealthComponent.h"
#include "Combat/PFWeaponComponent.h"
#include "Core/PaintForgeGameState.h"
#include "Core/PaintForgePlayerState.h"
#include "Player/PaintForgeCharacter.h"

#include "Blueprint/WidgetTree.h"
#include "Camera/PlayerCameraManager.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/Image.h"
#include "Components/TextBlock.h"
#include "Brushes/SlateColorBrush.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "Styling/CoreStyle.h"
#include "Styling/SlateBrush.h"

namespace
{
	FSlateBrush MakeCircleBrush()
	{
		// Asset-free circle: rounded-box draw with half-height radius.
		FSlateBrush Brush;
		Brush.DrawAs = ESlateBrushDrawType::RoundedBox;
		Brush.TintColor = FSlateColor(FLinearColor::White);
		Brush.OutlineSettings.RoundingType = ESlateBrushRoundingType::HalfHeightRadius;
		return Brush;
	}
}

TSharedRef<SWidget> UPFCombatFeedbackWidget::RebuildWidget()
{
	if (WidgetTree && !WidgetTree->RootWidget)
	{
		BuildTree(); // root assigned BEFORE Super::RebuildWidget (02 R3)
	}
	return Super::RebuildWidget();
}

void UPFCombatFeedbackWidget::BuildTree()
{
	UCanvasPanel* RootCanvas = WidgetTree->ConstructWidget<UCanvasPanel>();
	WidgetTree->RootWidget = RootCanvas;

	// ---- Mask splat blobs (drawn under everything else) ----
	BlobImages.Reset();
	BlobAge.Init(-1.f, BlobPoolSize);
	BlobBaseSize.Init(100.f, BlobPoolSize);
	for (int32 i = 0; i < BlobPoolSize; ++i)
	{
		UImage* Blob = WidgetTree->ConstructWidget<UImage>();
		Blob->SetBrush(MakeCircleBrush());
		Blob->SetVisibility(ESlateVisibility::Hidden);
		if (UCanvasPanelSlot* CSlot = RootCanvas->AddChildToCanvas(Blob))
		{
			CSlot->SetAnchors(FAnchors(0.5f, 0.5f));
			CSlot->SetAlignment(FVector2D(0.5f, 0.5f));
			CSlot->SetPosition(FVector2D::ZeroVector);
			CSlot->SetSize(FVector2D(100.f, 100.f));
			CSlot->SetZOrder(0);
		}
		BlobImages.Add(Blob);
	}

	// ---- Damage-direction arcs ----
	ArcImages.Reset();
	ArcRemaining.Init(0.f, ArcPoolSize);
	for (int32 i = 0; i < ArcPoolSize; ++i)
	{
		UImage* Arc = WidgetTree->ConstructWidget<UImage>();
		Arc->SetBrush(FSlateColorBrush(FLinearColor::White));
		Arc->SetVisibility(ESlateVisibility::Hidden);
		if (UCanvasPanelSlot* CSlot = RootCanvas->AddChildToCanvas(Arc))
		{
			CSlot->SetAnchors(FAnchors(0.5f, 0.5f));
			CSlot->SetAlignment(FVector2D(0.5f, 0.5f));
			CSlot->SetPosition(FVector2D::ZeroVector);
			CSlot->SetSize(FVector2D(64.f, 10.f));
			CSlot->SetZOrder(2);
		}
		ArcImages.Add(Arc);
	}

	// ---- Hitmarker: 4 diagonal ticks around the crosshair ----
	HitmarkerPanel = WidgetTree->ConstructWidget<UCanvasPanel>();
	if (UCanvasPanelSlot* CSlot = RootCanvas->AddChildToCanvas(HitmarkerPanel))
	{
		CSlot->SetAnchors(FAnchors(0.5f, 0.5f));
		CSlot->SetAlignment(FVector2D(0.5f, 0.5f));
		CSlot->SetPosition(FVector2D::ZeroVector);
		CSlot->SetSize(FVector2D(60.f, 60.f));
		CSlot->SetZOrder(6);
	}

	HitmarkerTicks.Reset();
	const float TickOffset = 11.f;
	const FVector2D TickPositions[4] =
	{
		FVector2D(TickOffset, -TickOffset),  // NE
		FVector2D(-TickOffset, TickOffset),  // SW
		FVector2D(-TickOffset, -TickOffset), // NW
		FVector2D(TickOffset, TickOffset)    // SE
	};
	const float TickAngles[4] = { -45.f, -45.f, 45.f, 45.f };
	for (int32 i = 0; i < 4; ++i)
	{
		UImage* Tick = WidgetTree->ConstructWidget<UImage>();
		Tick->SetBrush(FSlateColorBrush(FLinearColor::White));
		Tick->SetRenderTransformAngle(TickAngles[i]);
		Tick->SetVisibility(ESlateVisibility::Hidden);
		if (UCanvasPanelSlot* CSlot = HitmarkerPanel->AddChildToCanvas(Tick))
		{
			CSlot->SetAnchors(FAnchors(0.5f, 0.5f));
			CSlot->SetAlignment(FVector2D(0.5f, 0.5f));
			CSlot->SetPosition(TickPositions[i]);
			CSlot->SetSize(FVector2D(12.f, 3.f));
		}
		HitmarkerTicks.Add(Tick);
	}

	// ---- Elim confirm text ----
	ElimText = WidgetTree->ConstructWidget<UTextBlock>();
	ElimText->SetFont(FCoreStyle::GetDefaultFontStyle(FName(TEXT("Bold")), 26));
	ElimText->SetColorAndOpacity(FSlateColor(FLinearColor::White));
	ElimText->SetJustification(ETextJustify::Center);
	ElimText->SetRenderOpacity(0.f);
	if (UCanvasPanelSlot* CSlot = RootCanvas->AddChildToCanvas(ElimText))
	{
		CSlot->SetAnchors(FAnchors(0.5f, 0.5f));
		CSlot->SetAlignment(FVector2D(0.5f, 0.5f));
		CSlot->SetPosition(FVector2D(0.f, 120.f));
		CSlot->SetAutoSize(true);
		CSlot->SetZOrder(7);
	}
}

void UPFCombatFeedbackWidget::NativeConstruct()
{
	Super::NativeConstruct();
	TryBindGameState();
}

void UPFCombatFeedbackWidget::NativeDestruct()
{
	UnbindPawn();
	if (BoundGameState.IsValid())
	{
		BoundGameState->OnRoundStateChangedEvent.RemoveAll(this);
		BoundGameState->OnElimFeedChangedEvent.RemoveAll(this);
		BoundGameState.Reset();
	}
	Super::NativeDestruct();
}

void UPFCombatFeedbackWidget::TryBindGameState()
{
	if (BoundGameState.IsValid())
	{
		return;
	}
	const UWorld* World = GetWorld();
	APaintForgeGameState* GS = World ? World->GetGameState<APaintForgeGameState>() : nullptr;
	if (GS)
	{
		BoundGameState = GS;
		GS->OnRoundStateChangedEvent.AddUObject(this, &UPFCombatFeedbackWidget::HandleRoundStateChanged);
		GS->OnElimFeedChangedEvent.AddUObject(this, &UPFCombatFeedbackWidget::HandleElimFeedChanged);
	}
}

void UPFCombatFeedbackWidget::BindToPawn(APaintForgeCharacter* NewPawn)
{
	UnbindPawn();
	BoundPawn = NewPawn;
	if (NewPawn)
	{
		if (UPFWeaponComponent* Weapon = NewPawn->GetWeapon())
		{
			BoundWeapon = Weapon;
			Weapon->OnHitConfirmedEvent.AddUObject(this, &UPFCombatFeedbackWidget::HandleHitConfirmed);
		}
		if (UPFHealthComponent* Health = NewPawn->GetHealth())
		{
			BoundHealth = Health;
			Health->OnLocalPaintHitTakenEvent.AddUObject(this, &UPFCombatFeedbackWidget::HandleHitTaken);
		}
	}
}

void UPFCombatFeedbackWidget::UnbindPawn()
{
	if (BoundWeapon.IsValid())
	{
		BoundWeapon->OnHitConfirmedEvent.RemoveAll(this);
	}
	if (BoundHealth.IsValid())
	{
		BoundHealth->OnLocalPaintHitTakenEvent.RemoveAll(this);
	}
	BoundWeapon.Reset();
	BoundHealth.Reset();
	BoundPawn.Reset();
}

void UPFCombatFeedbackWidget::HandleHitConfirmed(uint32 ShotIndex, bool bElimHit)
{
	HitmarkerRemaining = HitmarkerDuration;
	bHitmarkerElim = bElimHit;

	// Elim variant tints our paint color (the paint we just splatted them with).
	FLinearColor TickColor = FLinearColor::White;
	uint8 LocalTeam = 0;
	if (const APlayerController* PC = GetOwningPlayer())
	{
		if (const APaintForgePlayerState* PS = PC->GetPlayerState<APaintForgePlayerState>())
		{
			LocalTeam = PS->TeamId <= 1 ? PS->TeamId : 0;
		}
	}
	if (bElimHit)
	{
		TickColor = PFColors::ForTeam(LocalTeam);
	}
	for (UImage* Tick : HitmarkerTicks)
	{
		if (Tick)
		{
			Tick->SetColorAndOpacity(TickColor);
			Tick->SetVisibility(ESlateVisibility::HitTestInvisible);
		}
	}

	APaintForgeCharacter* Pawn = BoundPawn.Get();
	UPFCombatAudio* Audio = Pawn ? Pawn->GetCombatAudio() : nullptr;
	if (Audio)
	{
		if (bElimHit)
		{
			Audio->PlayElim();
		}
		else
		{
			Audio->PlayHitmarker();
		}
	}

	if (bElimHit && ElimText)
	{
		// CONTRACT-GAP: OnHitConfirmedEvent carries no victim name; the name is
		// recovered from the replicated elim feed (newest entry where we are the
		// shooter). Feed OnRep may land after the reliable ClientHitConfirm, so a
		// pending flag retries when the feed changes.
		const FString VictimName = LookUpRecentVictimName();
		bElimTextNamePending = VictimName.IsEmpty();
		ElimText->SetText(FText::FromString(VictimName.IsEmpty()
			? FString(TEXT("SPLATTED!"))
			: FString::Printf(TEXT("SPLATTED  %s"), *VictimName)));
		ElimTextRemaining = ElimTextDuration;
	}
}

void UPFCombatFeedbackWidget::HandleElimFeedChanged()
{
	if (bElimTextNamePending && ElimTextRemaining > 0.f && ElimText)
	{
		const FString VictimName = LookUpRecentVictimName();
		if (!VictimName.IsEmpty())
		{
			ElimText->SetText(FText::FromString(FString::Printf(TEXT("SPLATTED  %s"), *VictimName)));
			bElimTextNamePending = false;
		}
	}
}

FString UPFCombatFeedbackWidget::LookUpRecentVictimName() const
{
	const APaintForgeGameState* GS = BoundGameState.Get();
	const APlayerController* PC = GetOwningPlayer();
	const APlayerState* LocalPS = PC ? PC->PlayerState : nullptr;
	if (!GS || !LocalPS)
	{
		return FString();
	}
	const FString LocalName = LocalPS->GetPlayerName();
	const float Now = GS->GetServerWorldTimeSeconds();
	for (int32 i = GS->ElimFeed.Num() - 1; i >= 0; --i)
	{
		const FPFElimEntry& Entry = GS->ElimFeed[i];
		if (Entry.ShooterName == LocalName && (Now - Entry.ServerTime) < 3.f)
		{
			return Entry.VictimName;
		}
	}
	return FString();
}

float UPFCombatFeedbackWidget::ComputeRelativeYawDeg(const FVector& ShooterLoc) const
{
	const APlayerController* PC = GetOwningPlayer();
	if (!PC || !PC->PlayerCameraManager)
	{
		return 0.f;
	}
	const FVector CamLoc = PC->PlayerCameraManager->GetCameraLocation();
	const float CamYaw = PC->PlayerCameraManager->GetCameraRotation().Yaw;
	const FVector ToShooter = ShooterLoc - CamLoc;
	const float WorldYaw = FMath::RadiansToDegrees(FMath::Atan2(ToShooter.Y, ToShooter.X));
	return FRotator::NormalizeAxis(WorldYaw - CamYaw);
}

void UPFCombatFeedbackWidget::HandleHitTaken(FVector ShooterLoc, uint8 ShooterTeam, uint8 NewHP)
{
	const float RelYawDeg = ComputeRelativeYawDeg(ShooterLoc);

	// ---- Damage-direction arc: on the 140 px circle at the shooter's bearing ----
	int32 ArcIdx = 0;
	for (int32 i = 1; i < ArcPoolSize; ++i)
	{
		if (ArcRemaining[i] < ArcRemaining[ArcIdx])
		{
			ArcIdx = i;
		}
	}
	if (UImage* Arc = ArcImages.IsValidIndex(ArcIdx) ? ArcImages[ArcIdx].Get() : nullptr)
	{
		const float Rad = FMath::DegreesToRadians(RelYawDeg);
		const FVector2D Pos(FMath::Sin(Rad) * ArcRadiusPx, -FMath::Cos(Rad) * ArcRadiusPx);
		if (UCanvasPanelSlot* CSlot = Cast<UCanvasPanelSlot>(Arc->Slot))
		{
			CSlot->SetPosition(Pos);
		}
		Arc->SetRenderTransformAngle(RelYawDeg); // tangent to the circle
		Arc->SetColorAndOpacity(PFColors::ForTeam(ShooterTeam));
		Arc->SetRenderOpacity(0.8f);
		Arc->SetVisibility(ESlateVisibility::HitTestInvisible);
		ArcRemaining[ArcIdx] = ArcDuration;
	}

	if (APaintForgeCharacter* Pawn = BoundPawn.Get())
	{
		if (UPFCombatAudio* Audio = Pawn->GetCombatAudio())
		{
			Audio->PlaySplatIncoming();
		}
	}

	// ---- Mask splats: wiped fully on elimination (04 §4), else 2-3 new blobs ----
	if (NewHP == 0)
	{
		WipeMaskSplats();
		return;
	}

	FVector2D LocalSize = GetCachedGeometry().GetLocalSize();
	if (LocalSize.X < 64.f || LocalSize.Y < 64.f)
	{
		LocalSize = FVector2D(1280.f, 720.f);
	}
	const float MaxReach = 0.5f * FMath::Min(LocalSize.X, LocalSize.Y);

	const int32 BlobCount = FMath::RandRange(2, 3);
	for (int32 n = 0; n < BlobCount; ++n)
	{
		// Recycle: prefer inactive, else the oldest (hard cap of 6 concurrent).
		int32 BlobIdx = 0;
		for (int32 i = 0; i < BlobPoolSize; ++i)
		{
			if (BlobAge[i] < 0.f)
			{
				BlobIdx = i;
				break;
			}
			if (BlobAge[i] > BlobAge[BlobIdx])
			{
				BlobIdx = i;
			}
		}
		UImage* Blob = BlobImages.IsValidIndex(BlobIdx) ? BlobImages[BlobIdx].Get() : nullptr;
		if (!Blob)
		{
			continue;
		}

		// Edge-biased toward the shooter's bearing.
		const float JitterDeg = RelYawDeg + FMath::FRandRange(-50.f, 50.f);
		const float Rad = FMath::DegreesToRadians(JitterDeg);
		const FVector2D Dir(FMath::Sin(Rad), -FMath::Cos(Rad));
		const FVector2D Pos = Dir * (MaxReach * FMath::FRandRange(0.55f, 0.95f));

		const float Size = FMath::FRandRange(60.f, 140.f);
		BlobBaseSize[BlobIdx] = Size;
		if (UCanvasPanelSlot* CSlot = Cast<UCanvasPanelSlot>(Blob->Slot))
		{
			CSlot->SetPosition(Pos);
			CSlot->SetSize(FVector2D(Size, Size * FMath::FRandRange(0.7f, 1.05f)));
		}
		Blob->SetRenderTransformAngle(FMath::FRandRange(0.f, 360.f));
		Blob->SetColorAndOpacity(PFColors::ForTeam(ShooterTeam));
		Blob->SetRenderOpacity(0.85f);
		Blob->SetVisibility(ESlateVisibility::HitTestInvisible);
		BlobAge[BlobIdx] = 0.f;
	}
}

void UPFCombatFeedbackWidget::HandleRoundStateChanged(EPFRoundState NewState)
{
	if (NewState == EPFRoundState::Freeze)
	{
		// Round reset: fresh mask (spawn wipes splats — 04 §6).
		WipeMaskSplats();
	}
}

void UPFCombatFeedbackWidget::WipeMaskSplats()
{
	for (int32 i = 0; i < BlobPoolSize; ++i)
	{
		BlobAge[i] = -1.f;
		if (BlobImages.IsValidIndex(i) && BlobImages[i])
		{
			BlobImages[i]->SetVisibility(ESlateVisibility::Hidden);
		}
	}
}

void UPFCombatFeedbackWidget::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);

	TryBindGameState(); // GameState can arrive late on clients

	// ---- Hitmarker: scale 1.2 -> 1.0, opacity 1 -> 0 over 0.15 s ----
	if (HitmarkerRemaining > 0.f)
	{
		HitmarkerRemaining = FMath::Max(0.f, HitmarkerRemaining - InDeltaTime);
		const float Alpha = HitmarkerRemaining / HitmarkerDuration; // 1 -> 0
		const float BaseScale = bHitmarkerElim ? 1.4f : 1.f;
		const float Scale = BaseScale * FMath::Lerp(1.f, 1.2f, Alpha);
		if (HitmarkerPanel)
		{
			HitmarkerPanel->SetRenderScale(FVector2D(Scale, Scale));
		}
		for (UImage* Tick : HitmarkerTicks)
		{
			if (Tick)
			{
				Tick->SetRenderOpacity(Alpha);
				if (HitmarkerRemaining <= 0.f)
				{
					Tick->SetVisibility(ESlateVisibility::Hidden);
				}
			}
		}
	}

	// ---- Damage arcs: opacity 0.8 -> 0 over 0.75 s ----
	for (int32 i = 0; i < ArcPoolSize; ++i)
	{
		if (ArcRemaining[i] > 0.f)
		{
			ArcRemaining[i] = FMath::Max(0.f, ArcRemaining[i] - InDeltaTime);
			if (UImage* Arc = ArcImages.IsValidIndex(i) ? ArcImages[i].Get() : nullptr)
			{
				Arc->SetRenderOpacity(0.8f * (ArcRemaining[i] / ArcDuration));
				if (ArcRemaining[i] <= 0.f)
				{
					Arc->SetVisibility(ESlateVisibility::Hidden);
				}
			}
		}
	}

	// ---- Mask splats: 0.85 -> 0.35 over 1 s, hold, wipe out by 6 s ----
	for (int32 i = 0; i < BlobPoolSize; ++i)
	{
		if (BlobAge[i] < 0.f)
		{
			continue;
		}
		BlobAge[i] += InDeltaTime;
		UImage* Blob = BlobImages.IsValidIndex(i) ? BlobImages[i].Get() : nullptr;
		if (!Blob)
		{
			continue;
		}
		const float Age = BlobAge[i];
		if (Age >= BlobLifetime)
		{
			BlobAge[i] = -1.f;
			Blob->SetVisibility(ESlateVisibility::Hidden);
		}
		else if (Age < BlobFadePhase)
		{
			Blob->SetRenderOpacity(FMath::Lerp(0.85f, 0.35f, Age / BlobFadePhase));
		}
		else if (Age > BlobLifetime - 1.f)
		{
			Blob->SetRenderOpacity(0.35f * (BlobLifetime - Age)); // final 1 s wipe
		}
		else
		{
			Blob->SetRenderOpacity(0.35f);
		}
	}

	// ---- Elim confirm text ----
	if (ElimTextRemaining > 0.f && ElimText)
	{
		ElimTextRemaining = FMath::Max(0.f, ElimTextRemaining - InDeltaTime);
		// Full opacity, quick fade in the final 0.25 s.
		ElimText->SetRenderOpacity(FMath::Clamp(ElimTextRemaining / 0.25f, 0.f, 1.f));
		if (ElimTextRemaining <= 0.f)
		{
			bElimTextNamePending = false;
		}
	}
}
