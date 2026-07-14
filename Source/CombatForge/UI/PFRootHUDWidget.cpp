// Copyright (c) 2026 Tom Chapman. All rights reserved.

#include "UI/PFRootHUDWidget.h"

#include "Building/PFBuildComponent.h"
#include "Core/CombatForgeGameState.h"
#include "Core/CombatForgePlayerController.h"
#include "Player/CombatForgeCharacter.h"
#include "UI/PFBuildHUDWidget.h"
#include "UI/PFBuildWheelWidget.h"
#include "UI/PFCombatFeedbackWidget.h"
#include "UI/PFCombatHUDWidget.h"
#include "UI/PFLobbyWidget.h"
#include "UI/PFOptionsWidget.h"
#include "UI/PFResultsWidget.h"
#include "UI/PFScoreboardWidget.h"
#include "UI/PFVoteWidget.h"

#include "Blueprint/WidgetTree.h"
#include "Components/Overlay.h"
#include "Components/OverlaySlot.h"
#include "Components/WidgetSwitcher.h"
#include "GameFramework/Pawn.h"

TSharedRef<SWidget> UPFRootHUDWidget::RebuildWidget()
{
	if (WidgetTree && !WidgetTree->RootWidget)
	{
		BuildTree(); // root assigned BEFORE Super::RebuildWidget (02 R3)
	}
	return Super::RebuildWidget();
}

void UPFRootHUDWidget::BuildTree()
{
	UOverlay* RootOverlay = WidgetTree->ConstructWidget<UOverlay>();
	WidgetTree->RootWidget = RootOverlay;

	auto AddFill = [RootOverlay](UWidget* Child)
	{
		if (!Child)
		{
			return;
		}
		if (UOverlaySlot* OSlot = RootOverlay->AddChildToOverlay(Child))
		{
			OSlot->SetHorizontalAlignment(HAlign_Fill);
			OSlot->SetVerticalAlignment(VAlign_Fill);
		}
	};

	// Phase panels: switcher child index == (int32)EPFMatchPhase (Lobby..Results = 0..4).
	// Child user widgets are created against our player context (§5 R8: valid owning local PC —
	// the PC created us in BeginPlayingState, so the context is always live here).
	PhaseSwitcher = WidgetTree->ConstructWidget<UWidgetSwitcher>();
	AddFill(PhaseSwitcher);

	LobbyPanel   = CreateWidget<UPFLobbyWidget>(this);
	BuildPanel   = CreateWidget<UPFBuildHUDWidget>(this);
	CombatPanel  = CreateWidget<UPFCombatHUDWidget>(this);
	VotePanel    = CreateWidget<UPFVoteWidget>(this);
	ResultsPanel = CreateWidget<UPFResultsWidget>(this);
	if (LobbyPanel)   { PhaseSwitcher->AddChild(LobbyPanel); }
	if (BuildPanel)   { PhaseSwitcher->AddChild(BuildPanel); }
	if (CombatPanel)  { PhaseSwitcher->AddChild(CombatPanel); }
	if (VotePanel)    { PhaseSwitcher->AddChild(VotePanel); }
	if (ResultsPanel) { PhaseSwitcher->AddChild(ResultsPanel); }
	PhaseSwitcher->SetActiveWidgetIndex(static_cast<int32>(EPFMatchPhase::Lobby));

	// Persistent overlays, bottom-up: feedback under the wheel, scoreboard, then options on top.
	FeedbackWidget = CreateWidget<UPFCombatFeedbackWidget>(this);
	AddFill(FeedbackWidget);
	WheelWidget = CreateWidget<UPFBuildWheelWidget>(this);
	AddFill(WheelWidget);
	ScoreboardWidget = CreateWidget<UPFScoreboardWidget>(this);
	AddFill(ScoreboardWidget);
	OptionsWidget = CreateWidget<UPFOptionsWidget>(this);
	AddFill(OptionsWidget);
}

void UPFRootHUDWidget::OpenOptions()
{
	if (OptionsWidget)
	{
		OptionsWidget->Open();
	}
}

void UPFRootHUDWidget::CloseOptions()
{
	if (OptionsWidget && OptionsWidget->IsOpen())
	{
		OptionsWidget->Close();
	}
}

void UPFRootHUDWidget::ToggleOptions()
{
	if (!OptionsWidget)
	{
		return;
	}
	if (OptionsWidget->IsOpen())
	{
		OptionsWidget->Close();
	}
	else
	{
		// Close build wheel capture if open so Escape doesn't leave look-input ignored.
		if (WheelWidget)
		{
			WheelWidget->CloseCancel();
		}
		OptionsWidget->Open();
	}
}

bool UPFRootHUDWidget::IsOptionsOpen() const
{
	return OptionsWidget && OptionsWidget->IsOpen();
}

void UPFRootHUDWidget::NativeConstruct()
{
	Super::NativeConstruct();

	TryBindGameState();
	BindOwningPC();

	if (WheelWidget)
	{
		// Wheel -> BuildComponent::EquipTool wiring (§3.6). RemoveAll first: NativeConstruct can
		// re-run if the widget is removed from and re-added to the viewport.
		WheelWidget->OnToolSelectedEvent.RemoveAll(this);
		WheelWidget->OnToolSelectedEvent.AddUObject(this, &UPFRootHUDWidget::HandleWheelToolSelected);
	}

	WirePawn(Cast<ACombatForgeCharacter>(GetOwningPlayerPawn()));
	UpdateScoreboardVisibility();
}

void UPFRootHUDWidget::NativeDestruct()
{
	if (BoundGameState.IsValid())
	{
		BoundGameState->OnPhaseChangedEvent.RemoveAll(this);
		BoundGameState.Reset();
	}
	UnbindOwningPC();
	if (BoundBuild.IsValid())
	{
		BoundBuild->OnBuildWheelRequestedEvent.RemoveAll(this);
	}
	BoundBuild.Reset();
	WiredPawn.Reset();
	if (WheelWidget)
	{
		WheelWidget->OnToolSelectedEvent.RemoveAll(this);
	}
	Super::NativeDestruct();
}

void UPFRootHUDWidget::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);

	TryBindGameState(); // GameState can replicate in after HUD creation on clients
	BindOwningPC();

	// Belt and braces under the possession delegate: never operate on a stale pawn.
	ACombatForgeCharacter* CurrentPawn = Cast<ACombatForgeCharacter>(GetOwningPlayerPawn());
	if (CurrentPawn != WiredPawn.Get())
	{
		WirePawn(CurrentPawn);
	}
}

ACombatForgePlayerController* UPFRootHUDWidget::GetPFPlayerController() const
{
	return Cast<ACombatForgePlayerController>(GetOwningPlayer());
}

void UPFRootHUDWidget::TryBindGameState()
{
	if (BoundGameState.IsValid())
	{
		return;
	}
	const UWorld* World = GetWorld();
	ACombatForgeGameState* GS = World ? World->GetGameState<ACombatForgeGameState>() : nullptr;
	if (GS)
	{
		BoundGameState = GS;
		GS->OnPhaseChangedEvent.AddUObject(this, &UPFRootHUDWidget::HandlePhaseChanged);
		HandlePhaseChanged(GS->Phase); // join-in-progress catch-up (02 R4)
	}
}

void UPFRootHUDWidget::BindOwningPC()
{
	if (bPCBound)
	{
		return;
	}
	ACombatForgePlayerController* PC = GetPFPlayerController();
	if (!PC)
	{
		return;
	}
	PC->OnPossessedPawnChanged.AddUniqueDynamic(this, &UPFRootHUDWidget::HandlePossessedPawnChanged);
	PC->OnScoreboardHeldChanged.AddUObject(this, &UPFRootHUDWidget::HandleScoreboardHeldChanged);
	bPCBound = true;
}

void UPFRootHUDWidget::UnbindOwningPC()
{
	if (ACombatForgePlayerController* PC = GetPFPlayerController())
	{
		PC->OnPossessedPawnChanged.RemoveDynamic(this, &UPFRootHUDWidget::HandlePossessedPawnChanged);
		PC->OnScoreboardHeldChanged.RemoveAll(this);
	}
	bPCBound = false;
}

void UPFRootHUDWidget::HandlePhaseChanged(EPFMatchPhase NewPhase)
{
	if (PhaseSwitcher)
	{
		PhaseSwitcher->SetActiveWidgetIndex(static_cast<int32>(NewPhase));
	}
	if (NewPhase == EPFMatchPhase::Vote && VotePanel)
	{
		VotePanel->ResetForVote(); // fresh ballot each VotePhase (§3.6)
	}
	if (NewPhase != EPFMatchPhase::Build && WheelWidget)
	{
		// Never leave Build with a captured wheel / a dangling look-input ignore.
		WheelWidget->CloseCancel();
	}
	UpdateScoreboardVisibility();
}

void UPFRootHUDWidget::HandlePossessedPawnChanged(APawn* OldPawn, APawn* NewPawn)
{
	WirePawn(Cast<ACombatForgeCharacter>(NewPawn));
}

void UPFRootHUDWidget::WirePawn(ACombatForgeCharacter* NewPawn)
{
	if (BoundBuild.IsValid())
	{
		BoundBuild->OnBuildWheelRequestedEvent.RemoveAll(this);
	}
	BoundBuild.Reset();

	// Push the pawn down into every pawn-bound child; each unbinds its previous pawn itself.
	if (BuildPanel)     { BuildPanel->BindToPawn(NewPawn); }
	if (CombatPanel)    { CombatPanel->BindToPawn(NewPawn); }
	if (FeedbackWidget) { FeedbackWidget->BindToPawn(NewPawn); }

	if (NewPawn)
	{
		if (UPFBuildComponent* Build = NewPawn->GetBuild())
		{
			BoundBuild = Build;
			Build->OnBuildWheelRequestedEvent.AddUObject(this, &UPFRootHUDWidget::HandleBuildWheelRequested);
		}
	}
	else if (WheelWidget)
	{
		WheelWidget->CloseCancel(); // pawn gone mid-hold: drop the capture cleanly
	}
	WiredPawn = NewPawn;
}

void UPFRootHUDWidget::HandleBuildWheelRequested(bool bOpen)
{
	if (!WheelWidget)
	{
		return;
	}
	if (bOpen)
	{
		// Belt and braces with the PC's IMC swap (§4.5): the wheel only opens in BuildPhase.
		const ACombatForgeGameState* GS = BoundGameState.Get();
		if (!GS || !GS->IsBuildAllowed())
		{
			return;
		}
		WheelWidget->Open();
	}
	else
	{
		WheelWidget->CloseAndCommit();
	}
}

void UPFRootHUDWidget::HandleWheelToolSelected(EPFBuildTool Tool)
{
	if (UPFBuildComponent* Build = BoundBuild.Get())
	{
		Build->EquipTool(Tool);
	}
}

void UPFRootHUDWidget::HandleScoreboardHeldChanged(bool /*bHeld*/)
{
	UpdateScoreboardVisibility();
}

void UPFRootHUDWidget::UpdateScoreboardVisibility()
{
	if (!ScoreboardWidget)
	{
		return;
	}
	const ACombatForgePlayerController* PC = GetPFPlayerController();
	const ACombatForgeGameState* GS = BoundGameState.Get();
	// T22: in Lobby, Tab-hold is the roster-cursor toggle and the lobby panel already shows the
	// full roster — the match scoreboard overlay answers Tab from Build onward.
	const bool bShow = PC && PC->IsScoreboardHeld()
		&& GS && GS->Phase != EPFMatchPhase::Lobby;
	ScoreboardWidget->SetVisibility(bShow
		? ESlateVisibility::HitTestInvisible
		: ESlateVisibility::Collapsed);
	if (bShow)
	{
		ScoreboardWidget->RefreshNow();
	}
}
