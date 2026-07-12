// Copyright (c) 2026 Tom Chapman. All rights reserved.

#include "UI/PFLoadingMenuWidget.h"

#include "PaintForge.h"
#include "Core/PaintForgePlayerController.h"

#include "Blueprint/WidgetTree.h"
#include "Brushes/SlateColorBrush.h"
#include "Components/Button.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/Image.h"
#include "Components/ProgressBar.h"
#include "Components/SizeBox.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Engine/Texture2D.h"
#include "GameFramework/PlayerController.h"
#include "Materials/MaterialInterface.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "ShaderCompiler.h"
#include "ShaderPipelineCache.h"
#include "Styling/CoreStyle.h"
#include "UObject/SoftObjectPath.h"

namespace
{
	FSlateFontInfo PFLoadFont(int32 Size, bool bBold)
	{
		return FCoreStyle::GetDefaultFontStyle(bBold ? FName(TEXT("Bold")) : FName(TEXT("Regular")), Size);
	}
}

TSharedRef<SWidget> UPFLoadingMenuWidget::RebuildWidget()
{
	if (WidgetTree && !WidgetTree->RootWidget)
	{
		BuildTree();
	}
	return Super::RebuildWidget();
}

void UPFLoadingMenuWidget::BuildTree()
{
	UCanvasPanel* Root = WidgetTree->ConstructWidget<UCanvasPanel>();
	WidgetTree->RootWidget = Root;

	// Full opaque backdrop — covers the live world completely (not a transparent HUD).
	Backdrop = WidgetTree->ConstructWidget<UImage>();
	Backdrop->SetBrush(FSlateColorBrush(FLinearColor::White));
	Backdrop->SetColorAndOpacity(FLinearColor(0.04f, 0.05f, 0.07f, 1.f));
	if (UCanvasPanelSlot* S = Root->AddChildToCanvas(Backdrop))
	{
		S->SetAnchors(FAnchors(0.f, 0.f, 1.f, 1.f));
		S->SetOffsets(FMargin(0.f));
		S->SetZOrder(0);
	}

	UVerticalBox* Col = WidgetTree->ConstructWidget<UVerticalBox>();

	TitleText = WidgetTree->ConstructWidget<UTextBlock>();
	TitleText->SetText(FText::FromString(TEXT("PAINTFORGE")));
	TitleText->SetFont(PFLoadFont(48, true));
	TitleText->SetColorAndOpacity(FSlateColor(FLinearColor(1.f, 0.92f, 0.35f)));
	TitleText->SetJustification(ETextJustify::Center);
	if (UVerticalBoxSlot* V = Col->AddChildToVerticalBox(TitleText))
	{
		V->SetHorizontalAlignment(HAlign_Center);
		V->SetPadding(FMargin(0.f, 0.f, 0.f, 8.f));
	}

	SubtitleText = WidgetTree->ConstructWidget<UTextBlock>();
	SubtitleText->SetText(FText::FromString(TEXT("Airsoft arena · multiplayer")));
	SubtitleText->SetFont(PFLoadFont(16, false));
	SubtitleText->SetColorAndOpacity(FSlateColor(FLinearColor(0.75f, 0.78f, 0.85f)));
	SubtitleText->SetJustification(ETextJustify::Center);
	if (UVerticalBoxSlot* V = Col->AddChildToVerticalBox(SubtitleText))
	{
		V->SetHorizontalAlignment(HAlign_Center);
		V->SetPadding(FMargin(0.f, 0.f, 0.f, 36.f));
	}

	StatusText = WidgetTree->ConstructWidget<UTextBlock>();
	StatusText->SetText(FText::FromString(TEXT("Starting…")));
	StatusText->SetFont(PFLoadFont(15, false));
	StatusText->SetColorAndOpacity(FSlateColor(FLinearColor(0.9f, 0.9f, 0.92f)));
	StatusText->SetJustification(ETextJustify::Center);
	if (UVerticalBoxSlot* V = Col->AddChildToVerticalBox(StatusText))
	{
		V->SetHorizontalAlignment(HAlign_Center);
		V->SetPadding(FMargin(0.f, 0.f, 0.f, 12.f));
	}

	ProgressBar = WidgetTree->ConstructWidget<UProgressBar>();
	ProgressBar->SetPercent(0.f);
	ProgressBar->SetFillColorAndOpacity(FLinearColor(1.f, 0.85f, 0.25f));
	USizeBox* ProgressSizer = WidgetTree->ConstructWidget<USizeBox>();
	ProgressSizer->SetWidthOverride(420.f);
	ProgressSizer->SetHeightOverride(14.f);
	ProgressSizer->SetContent(ProgressBar);
	if (UVerticalBoxSlot* V = Col->AddChildToVerticalBox(ProgressSizer))
	{
		V->SetHorizontalAlignment(HAlign_Center);
		V->SetPadding(FMargin(0.f, 0.f, 0.f, 28.f));
	}

	EnterButton = WidgetTree->ConstructWidget<UButton>();
	EnterButton->SetIsEnabled(false);
	EnterButton->OnClicked.AddDynamic(this, &UPFLoadingMenuWidget::OnEnterClicked);
	EnterLabel = WidgetTree->ConstructWidget<UTextBlock>();
	EnterLabel->SetText(FText::FromString(TEXT("ENTER LOBBY")));
	EnterLabel->SetFont(PFLoadFont(18, true));
	EnterLabel->SetColorAndOpacity(FSlateColor(FLinearColor(0.15f, 0.15f, 0.18f)));
	EnterLabel->SetJustification(ETextJustify::Center);
	EnterButton->AddChild(EnterLabel);
	if (UVerticalBoxSlot* V = Col->AddChildToVerticalBox(EnterButton))
	{
		V->SetHorizontalAlignment(HAlign_Center);
		V->SetPadding(FMargin(0.f, 8.f, 0.f, 0.f));
	}

	if (UCanvasPanelSlot* S = Root->AddChildToCanvas(Col))
	{
		S->SetAnchors(FAnchors(0.5f, 0.5f));
		S->SetAlignment(FVector2D(0.5f, 0.5f));
		S->SetAutoSize(true);
		S->SetZOrder(1);
	}
}

void UPFLoadingMenuWidget::NativeConstruct()
{
	Super::NativeConstruct();

	// Preload list: combat/build art hit early in a match. Soft — missing packs are fine.
	PreloadPaths = {
		TEXT("/Game/Materials/M_PF_ArenaFloor.M_PF_ArenaFloor"),
		TEXT("/Game/Materials/M_PF_ArenaWall.M_PF_ArenaWall"),
		TEXT("/Game/Materials/M_PF_BuildPiece.M_PF_BuildPiece"),
		TEXT("/Game/Materials/M_PF_TeamBody.M_PF_TeamBody"),
		TEXT("/Game/Weapons/Rifle/Mesh/SM_Rifle.SM_Rifle"),
		TEXT("/Game/Weapons/Rifle/M_PF_Rifle.M_PF_Rifle"),
		TEXT("/Game/QuantumCharacter/Mesh/SKM_QuantumCharacter.SKM_QuantumCharacter"),
		TEXT("/Game/Survival_Character/Meshes/SK_Survival_Character.SK_Survival_Character"),
		TEXT("/Game/Characters/Mannequins/Meshes/SKM_Manny_Simple.SKM_Manny_Simple"),
		TEXT("/Game/Characters/Mannequins/Anims/Unarmed/ABP_Unarmed.ABP_Unarmed_C"),
		TEXT("/Game/Materials/M_PF_Flash.M_PF_Flash"),
		TEXT("/Game/Materials/M_PF_ImpactMark.M_PF_ImpactMark"),
	};
	PreloadIndex = 0;
	WarmupStep = 0;
	Progress = 0.f;
	bWarmupComplete = false;
	bDismissed = false;

	// UI-only while this menu is up — don't let look/move leak into the pen behind the curtain.
	if (APlayerController* PC = GetOwningPlayer())
	{
		FInputModeUIOnly Mode;
		Mode.SetWidgetToFocus(TakeWidget());
		Mode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
		PC->SetInputMode(Mode);
		PC->bShowMouseCursor = true;
	}

	SetStatus(TEXT("Preparing…"));
	UE_LOG(PaintForgeLog, Log, TEXT("LoadingMenu: boot menu up — warmup starting"));
}

void UPFLoadingMenuWidget::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);
	if (bDismissed || bWarmupComplete)
	{
		return;
	}
	RunWarmupStep();
}

void UPFLoadingMenuWidget::SetStatus(const FString& Line)
{
	if (StatusText)
	{
		StatusText->SetText(FText::FromString(Line));
	}
}

void UPFLoadingMenuWidget::RunWarmupStep()
{
	// Spread work across frames so the progress bar animates and the process stays responsive.
	switch (WarmupStep)
	{
	case 0:
		SetStatus(TEXT("Loading materials & meshes…"));
		WarmupStep = 1;
		Progress = 0.05f;
		break;

	case 1:
	{
		// Load a few soft objects per tick.
		const int32 Batch = 2;
		int32 Loaded = 0;
		while (PreloadIndex < PreloadPaths.Num() && Loaded < Batch)
		{
			const FSoftObjectPath Path(PreloadPaths[PreloadIndex]);
			if (UObject* Obj = Path.TryLoad())
			{
				// Touch render proxies for materials so shaders queue.
				if (UMaterialInterface* Mat = Cast<UMaterialInterface>(Obj))
				{
					Mat->GetRenderProxy();
				}
			}
			++PreloadIndex;
			++Loaded;
		}
		const float Frac = PreloadPaths.Num() > 0
			? static_cast<float>(PreloadIndex) / static_cast<float>(PreloadPaths.Num())
			: 1.f;
		Progress = 0.05f + Frac * 0.45f;
		if (ProgressBar) { ProgressBar->SetPercent(Progress); }
		if (PreloadIndex >= PreloadPaths.Num())
		{
			WarmupStep = 2;
			SetStatus(TEXT("Compiling shaders…"));
		}
		break;
	}

	case 2:
	{
		// Drain the async shader compile queue (editor + packaged first runs).
		int32 RemainingJobs = 0;
		if (GShaderCompilingManager)
		{
			// Process a slice; FinishAllCompilation blocks the whole process if we call it
			// every frame with a huge queue — prefer ProcessAsyncResults then finish once thin.
			GShaderCompilingManager->ProcessAsyncResults(false, false);
			RemainingJobs = GShaderCompilingManager->GetNumRemainingJobs();
			if (RemainingJobs <= 2)
			{
				GShaderCompilingManager->FinishAllCompilation();
				RemainingJobs = GShaderCompilingManager->GetNumRemainingJobs();
			}
		}

		// PSO / pipeline cache precompiles (packaged builds).
		const uint32 PsoLeft = FShaderPipelineCache::NumPrecompilesRemaining();

		const float ShaderFrac = RemainingJobs > 0
			? FMath::Clamp(1.f - static_cast<float>(RemainingJobs) / 64.f, 0.f, 0.95f)
			: 1.f;
		Progress = 0.50f + ShaderFrac * 0.40f;
		if (ProgressBar) { ProgressBar->SetPercent(Progress); }

		if (RemainingJobs > 0 || PsoLeft > 0)
		{
			SetStatus(FString::Printf(TEXT("Compiling shaders… (%d jobs, %u PSO)"),
				RemainingJobs, PsoLeft));
			ShaderWaitAccum = 0.f;
		}
		else
		{
			// Hold a beat so the bar doesn't flash 100% and vanish.
			ShaderWaitAccum += GetWorld() ? GetWorld()->GetDeltaSeconds() : 0.016f;
			if (ShaderWaitAccum > 0.15f)
			{
				WarmupStep = 3;
			}
		}
		break;
	}

	case 3:
		FinishWarmup();
		break;

	default:
		break;
	}
}

void UPFLoadingMenuWidget::FinishWarmup()
{
	if (bWarmupComplete)
	{
		return;
	}
	bWarmupComplete = true;
	Progress = 1.f;
	if (ProgressBar) { ProgressBar->SetPercent(1.f); }
	SetStatus(TEXT("Ready."));
	if (EnterButton)
	{
		EnterButton->SetIsEnabled(true);
	}
	if (EnterLabel)
	{
		EnterLabel->SetText(FText::FromString(TEXT("ENTER LOBBY")));
	}
	UE_LOG(PaintForgeLog, Log, TEXT("LoadingMenu: warmup complete — waiting for Enter"));
}

void UPFLoadingMenuWidget::OnEnterClicked()
{
	if (!bWarmupComplete || bDismissed)
	{
		return;
	}
	bDismissed = true;
	RemoveFromParent();

	// Re-apply lobby IMCs + GameAndUI now that the curtain is gone.
	if (APaintForgePlayerController* PC = Cast<APaintForgePlayerController>(GetOwningPlayer()))
	{
		PC->NotifyLoadingMenuFinished();
	}

	UE_LOG(PaintForgeLog, Log, TEXT("LoadingMenu: dismissed — entering lobby view"));
}
