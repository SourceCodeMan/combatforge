// Copyright (c) 2026 Tom Chapman. All rights reserved.

#include "Player/CombatForgeCharacter.h"

#include "HAL/IConsoleManager.h"   // pf.BanditChar spike toggle + pose-tune cvars
#include "DrawDebugHelpers.h"       // pf.ShowMuzzle marker
#include "Engine/Engine.h"          // on-screen pose / weapon-cycle messages

#include "CombatForge.h"
#include "Core/CombatForgeTypes.h"
#include "Core/CombatForgeGameState.h"
#include "Core/CombatForgePlayerController.h"
#include "Player/PFCharacterMovementComponent.h"
#include "Player/PFCameraShakes.h"
#include "Input/PFInputConfig.h"
#include "Combat/PFWeaponComponent.h"
#include "Combat/PFHealthComponent.h"
#include "Online/PFBackendSubsystem.h"   // fleet weapon unlock gate (ServerSetKit)
#include "Combat/PFCombatAudio.h"
#include "Core/CombatForgePlayerState.h"   // melee: victim/attacker team + kill credit
#include "Combat/PFCombatVFX.h"
#include "Combat/PFAmmoBarrel.h"
#include "Combat/PFBombActor.h"
#include "Combat/PFBombPickup.h"
#include "Building/PFBuildPieceActor.h"
#include "Building/PFBuildComponent.h"
#include "Building/PFBuildGrid.h"          // plant: aimed-piece lookup (FindPieceByHit)
#include "Core/CombatForgeGameMode.h"      // plant: server route to ServerTryPlantBomb
#include "Core/PFUserPrefs.h"

#include "Camera/CameraComponent.h"
#include "Camera/PlayerCameraManager.h"
#include "Components/CapsuleComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimSingleNodeInstance.h"
#include "Animation/AnimBlueprintGeneratedClass.h"
#include "EnhancedInputComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Engine/OverlapResult.h"   // melee fallback: OverlapMultiByObjectType
#include "EngineUtils.h"
#include "UObject/UObjectIterator.h"   // pf.ArmedAnims live-toggle sink
#include "GameFramework/CharacterMovementComponent.h"
#include "NavigationInvokerComponent.h"
#include "Net/UnrealNetwork.h"            // DOREPLIFETIME (KitRep)
#include "Perception/AISense_Hearing.h"   // running footsteps → AI can hear a sprinter 360°
#include "InputActionValue.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "TimerManager.h"
#include "UObject/ConstructorHelpers.h"

// Rifle-hold locomotion kill-switch: the armed set plays AnimStarterPack sequences on the Bandit skeleton via
// a compatible-skeletons entry — visually unverifiable headless, so keep a live revert (`pf.ArmedAnims 0` +
// respawn) in case the retarget T-poses or slides on some machine.
// Default 0 (Tom playtest 2026-07-16): the rifle-hold anim packs are on foreign mannequin skeletons
// Rifle-hold locomotion (AS_Rifle_* low-ready). ✅ IK-RETARGET BAKE DONE (Tom, 2026-07-18): these paths now
// point at animations baked onto SKM_Bandit_Skeleton, which live in the ROOT of /Game/RifleAnims/ (NOT the
// originals under Animations/BlendSpaces/Standing_IdleWalkJogRun/, which are still on UE4_Mannequin_Skeleton
// and elongate the body via the name-based compatible-skeleton remap). Same asset NAMES in both folders —
// the package path is what disambiguates, so do NOT "tidy" these paths back to the BlendSpaces folder or the
// stretched torso returns. Default ON: hands on the gun, no stretch. pf.ArmedAnims 0 = empty-handed A_MM_*.
static TAutoConsoleVariable<int32> CVarArmedAnims(
	TEXT("pf.ArmedAnims"), 1,
	TEXT("1 = rifle-hold pack anims (default; hands on gun), 0 = native unarmed Bandit anims. Applies live."));

// Tuning aid: draw the FP cosmetic muzzle (where owner tracers spawn) so pf.WeaponFP's muzzle offset can be
// aligned to the visible barrel. Off by default; local player only (drawn in Tick's IsLocallyControlled block).
static TAutoConsoleVariable<int32> CVarShowMuzzle(
	TEXT("pf.ShowMuzzle"), 0,
	TEXT("1 = draw a marker at the first-person muzzle + shot line (align pf.WeaponFP's last 3 args to the barrel)."));

// Dev pose-tuning drag: hold MIDDLE MOUSE.
//   plain MMB = translate  |  Shift = depth  |  Ctrl = pitch/yaw the MUZZLE  |  Alt = roll
// Off by default; hip pose vs ADS pose depends on whether you're aiming.
static TAutoConsoleVariable<int32> CVarWeaponDrag(
	TEXT("pf.WeaponDrag"), 0,
	TEXT("1 = MMB drag FP weapon. Plain=move, Shift=depth, Ctrl=pitch/yaw muzzle, Alt=roll. Release logs pose."));

// Catalog poses are hand-tuned (drag bake). Auto-pose overwrote those every equip and made the first
// rifles "change every cycle" — default OFF so PFWeaponCatalog is the source of truth.
static TAutoConsoleVariable<int32> CVarWeaponAutoPose(
	TEXT("pf.WeaponAutoPose"), 0,
	TEXT("1 = auto-generate FP hip+ADS from mesh bounds (ignores catalog). 0 = catalog poses only (default)."));

// Third-person grip source. 1 (default) = attach the TP gun to the ANIMATION's own weapon bone (ik_hand_gun),
// which the rifle pack animates to sit exactly in the grip and which the IK-retarget carried onto the Bandit —
// so the position is correct by construction and tracks the animation, with NO hand-tuned offset.
// 0 = legacy hand_r + the WeaponRelative* offset (still used automatically whenever the unarmed anim set is on,
// since ik_hand_gun isn't meaningfully posed there). Flip to 0 to A/B it live if a gun ever looks wrong.
// DEFAULT 0 (Tom playtest 2026-07-18): ik_hand_gun turned out NOT to be animated even after adding an IKGun
// retarget chain — it stays parked near its parent ik_hand_root down at the pelvis, which is why the rifle hung
// at the hip while the hands were correctly posed in front. Attach to hand_r instead and orient from the hands
// (see CVarWeaponAimFromHands). Kept as a switch only for A/B.
static TAutoConsoleVariable<int32> CVarWeaponBoneAttach(
	TEXT("pf.WeaponBoneAttach"), 0,
	TEXT("1 = attach TP weapon to ik_hand_gun (NOT animated on this skeleton — leaves the gun at the hip). 0 = hand_r (default)."));

// THE fix for "gun isn't where the hands are". The rifle-hold animation poses BOTH hands correctly (right hand
// on the grip, left on the foregrip) — so the weapon's line IS the vector between them. Orienting the gun along
// hand_r -> hand_l is derived entirely from the live animated pose: no hand-tuned numbers, correct for every
// frame of every clip, and self-correcting if the animation set ever changes. Falls back to the old fixed
// offset for one-handed weapons or if hand_l is missing.
static TAutoConsoleVariable<int32> CVarWeaponAimFromHands(
	TEXT("pf.WeaponAimFromHands"), 1,
	TEXT("1 = orient the TP weapon along the animated hand_r->hand_l line (default). 0 = fixed WeaponRelative* offset."));

// Live toggle: pawns are REUSED across respawns, so waiting for the next AssembleBanditCharacter meant the
// kill switch never took effect. The sink fires when any cvar changes; re-route the anim set on a real edge.
static void PFArmedAnimsSink()
{
	static int32 LastArmed = CVarArmedAnims.GetValueOnGameThread();
	static int32 LastBoneAttach = CVarWeaponBoneAttach.GetValueOnGameThread();
	const int32 Now = CVarArmedAnims.GetValueOnGameThread();
	const int32 NowBone = CVarWeaponBoneAttach.GetValueOnGameThread();
	if (Now == LastArmed && NowBone == LastBoneAttach)
	{
		return;
	}
	const bool bArmedChanged = (Now != LastArmed);
	LastArmed = Now;
	LastBoneAttach = NowBone;
	for (TObjectIterator<ACombatForgeCharacter> It; It; ++It)
	{
		if (It->GetWorld() != nullptr && It->GetWorld()->IsGameWorld())
		{
			// The attach bone is resolved ONCE and cached, but which bone is correct depends on both cvars
			// (ik_hand_gun only while the armed set plays). Clear it so the next tick re-resolves — otherwise
			// a live A/B toggle silently keeps the old bone and looks like the flag did nothing.
			It->InvalidateWeaponAttachBone();
			if (bArmedChanged)
			{
				It->RefreshBanditAnimSet();
			}
		}
	}
}
static FAutoConsoleVariableSink GPFArmedAnimsSink(FConsoleCommandDelegate::CreateStatic(&PFArmedAnimsSink));

ACombatForgeCharacter::ACombatForgeCharacter(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer.SetDefaultSubobjectClass<UPFCharacterMovementComponent>(ACharacter::CharacterMovementComponentName))
{
	PrimaryActorTick.bCanEverTick = true;

	bReplicates = true;
	SetNetUpdateFrequency(60.f);      // T28
	SetMinNetUpdateFrequency(30.f);

	bUseControllerRotationYaw = true; // first-person: capsule yaw follows camera
	bUseControllerRotationPitch = false;
	bUseControllerRotationRoll = false;

	PFMovement = Cast<UPFCharacterMovementComponent>(GetCharacterMovement());

	// Nav invoker: builds the runtime navmesh in a radius around this pawn so bots always have a mesh to
	// path on over the freshly-built fort. Radii are generous (cover most of the arena around any combatant)
	// since a bot pathing on an incomplete mesh is worse than the modest build cost. Removal radius must
	// exceed generation (hysteresis, prevents tile thrash at the edge). Auto-registers as a default subobject.
	NavInvoker = CreateDefaultSubobject<UNavigationInvokerComponent>(TEXT("NavInvoker"));
	NavInvoker->SetGenerationRadii(7000.f, 9000.f);

	UCapsuleComponent* Capsule = GetCapsuleComponent();
	Capsule->SetCollisionResponseToChannel(PF_ECC_Paintball, ECR_Block);   // §4.6
	Capsule->SetCollisionResponseToChannel(PF_ECC_BuildTrace, ECR_Ignore);

	// ---- First-person camera: capsule-top - 10 uu, 0-length boom equivalent ----
	FirstPersonCamera = CreateDefaultSubobject<UCameraComponent>(TEXT("FirstPersonCamera"));
	FirstPersonCamera->SetupAttachment(Capsule);
	FirstPersonCamera->SetRelativeLocation(
		FVector(0.f, 0.f, Capsule->GetUnscaledCapsuleHalfHeight() - CameraEyeOffsetFromCapsuleTop));
	FirstPersonCamera->bUsePawnControlRotation = true;
	FirstPersonCamera->SetFieldOfView(BaseFOV);

	// ---- Graybox body: scaled engine cubes, team MID, hidden to owner ----
	static ConstructorHelpers::FObjectFinder<UStaticMesh> CubeFinder(TEXT("/Engine/BasicShapes/Cube.Cube"));
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> MatFinder(
		TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));

	BodyMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("BodyMesh"));
	BodyMesh->SetupAttachment(Capsule);
	if (CubeFinder.Succeeded())
	{
		BodyMesh->SetStaticMesh(CubeFinder.Object);
	}
	if (MatFinder.Succeeded())
	{
		BodyMesh->SetMaterial(0, MatFinder.Object);
	}
	// Torso+legs slab: 55 x 55 x 130 uu, top just below the head cube.
	BodyMesh->SetRelativeLocation(FVector(0.f, 0.f, -18.f));
	BodyMesh->SetRelativeScale3D(FVector(0.55f, 0.55f, 1.3f));
	BodyMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	BodyMesh->SetOwnerNoSee(true);

	HeadMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("HeadMesh"));
	HeadMesh->SetupAttachment(Capsule);
	if (CubeFinder.Succeeded())
	{
		HeadMesh->SetStaticMesh(CubeFinder.Object);
	}
	if (MatFinder.Succeeded())
	{
		HeadMesh->SetMaterial(0, MatFinder.Object);
	}
	// Mask cube: 35 x 35 x 30 uu, inside the T1 mask band (capsule-top - 35).
	HeadMesh->SetRelativeLocation(FVector(0.f, 0.f, 68.f));
	HeadMesh->SetRelativeScale3D(FVector(0.35f, 0.35f, 0.3f));
	HeadMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	HeadMesh->SetOwnerNoSee(true);

	// ACharacter's skeletal mesh stays asset-less; hide it defensively.
	if (GetMesh() != nullptr)
	{
		GetMesh()->SetVisibility(false);
	}

	// ---- Art loadout components (M1): created empty; ApplyArtLoadout assigns meshes if set ----
	FirstPersonArms = CreateDefaultSubobject<USkeletalMeshComponent>(TEXT("FirstPersonArms"));
	FirstPersonArms->SetupAttachment(FirstPersonCamera);
	FirstPersonArms->SetOnlyOwnerSee(true);          // FP arms: only the owning client sees them
	FirstPersonArms->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	FirstPersonArms->SetVisibility(false);

	WeaponMeshComp = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("WeaponMeshComp"));
	WeaponMeshComp->SetupAttachment(GetMesh());      // re-attached to the hand socket in ApplyArtLoadout
	WeaponMeshComp->SetOwnerNoSee(true);             // slice: weapon rides the TP body only
	WeaponMeshComp->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	WeaponMeshComp->SetVisibility(false);

	BackWeaponMeshComp = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("BackWeaponMeshComp"));
	BackWeaponMeshComp->SetupAttachment(GetMesh());  // re-seated on spine in AttachWeaponToBack
	BackWeaponMeshComp->SetOwnerNoSee(true);
	BackWeaponMeshComp->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	BackWeaponMeshComp->SetVisibility(false);

	// ---- First-person marker viewmodel: a rifle silhouette from engine primitives, owner-only-see.
	// This is what the player stares at every second — the single biggest "it's an FPS" signal. A real
	// weapon mesh drops in later by re-pointing the parts (or via the WeaponMesh art-loadout seam).
	static ConstructorHelpers::FObjectFinder<UStaticMesh> CylFinder(TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	static ConstructorHelpers::FObjectFinder<UStaticMesh> RifleMeshFinder(TEXT("/Game/Weapons/Rifle/Mesh/SM_Rifle.SM_Rifle"));
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> RifleMatFinder(TEXT("/Game/Weapons/Rifle/M_PF_Rifle.M_PF_Rifle"));
	UStaticMesh* CubeMesh = CubeFinder.Succeeded() ? CubeFinder.Object : nullptr;
	UStaticMesh* CylMesh = CylFinder.Succeeded() ? CylFinder.Object.Get() : CubeMesh;
	UMaterialInterface* GunBaseMat = MatFinder.Succeeded() ? MatFinder.Object : nullptr;

	ViewModelRoot = CreateDefaultSubobject<USceneComponent>(TEXT("ViewModelRoot"));
	ViewModelRoot->SetupAttachment(FirstPersonCamera);
	ViewModelHomeLoc = FVector(26.f, 9.f, -12.f);   // forward-right-down of the eye, classic viewmodel pose
	ViewModelRoot->SetRelativeLocation(ViewModelHomeLoc);

	// First-person weapon: the real rifle if available (Lyra SM_Rifle + generated M_PF_Rifle, self-contained),
	// otherwise the primitive marker gun. We build ONE or the OTHER — building no primitive means the
	// eliminate/respawn visibility propagate (SetChildVisibility) can never re-show an old primitive gun.
	// Airsoft: no muzzle flash / smoke / gunfire light components.
	if (UStaticMesh* RifleMesh = RifleMeshFinder.Succeeded() ? RifleMeshFinder.Object.Get() : nullptr)
	{
		RifleMaterial = RifleMatFinder.Succeeded() ? RifleMatFinder.Object.Get() : nullptr;
		RifleFPMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("RifleFPMesh"));
		RifleFPMesh->SetupAttachment(ViewModelRoot);
		RifleFPMesh->SetStaticMesh(RifleMesh);
		// SM_Rifle may still soft-ref missing Lyra MI_Weapon_Rifle — force every slot to our material
		// so packaged clients don't chase a missing package mid-match (LoadErrors + crash risk).
		if (RifleMaterial != nullptr)
		{
			const int32 Mats = RifleFPMesh->GetNumMaterials();
			for (int32 i = 0; i < Mats; ++i)
			{
				RifleFPMesh->SetMaterial(i, RifleMaterial);
			}
		}
		RifleFPMesh->SetOnlyOwnerSee(true);
		RifleFPMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		RifleFPMesh->SetCastShadow(false);
		// Held-rifle pose in ViewModelRoot space (+X forward, +Y right, +Z up). Mesh +Y = barrel.
		// Slightly tucked for "in hands"; ADS pulls ViewModelRoot toward iron-sight center.
		RifleFPMesh->SetRelativeLocation(FVector(3.f, 5.5f, -3.5f));
		RifleFPMesh->SetRelativeRotation(FRotator(-1.5f, -90.f, 1.5f));
		RifleFPMesh->SetRelativeScale3D(FVector(0.48f));
		// Barrel tip in ViewModelRoot space — cosmetic balls spawn from here (not under the gun).
		MuzzleLocalFP = FVector(42.f, 3.5f, -3.5f);
		WeaponMesh = RifleMesh;                      // third-person seam (shoulder pose in UpdateWeaponHoldPose)
	}
	else
	{
		BuildMarker(ViewModelRoot, TEXT("VM_"), CubeMesh, CylMesh, MarkerPartsFP, MuzzleLocalFP);
		for (TObjectPtr<UStaticMeshComponent>& Part : MarkerPartsFP)
		{
			if (Part != nullptr)
			{
				if (GunBaseMat != nullptr) { Part->SetMaterial(0, GunBaseMat); }
				Part->SetOnlyOwnerSee(true);   // the marker is the owner's first-person viewmodel
			}
		}
	}

	// Team models: each team is ONE human mesh for readability.
	//   Team 0 (blue)  = Quantum operator
	//   Team 1 (orange)= Survival character
	// Mannequin pack remains the graybox-friendly fallback if a pack is missing.
	static ConstructorHelpers::FObjectFinder<USkeletalMesh> QuantumBodyFinder(
		TEXT("/Game/QuantumCharacter/Mesh/SKM_QuantumCharacter.SKM_QuantumCharacter"));
	static ConstructorHelpers::FObjectFinder<USkeletalMesh> SurvivalBodyFinder(
		TEXT("/Game/Survival_Character/Meshes/SK_Survival_Character.SK_Survival_Character"));
	static ConstructorHelpers::FObjectFinder<USkeletalMesh> MannequinBodyFinder(
		TEXT("/Game/Characters/Mannequins/Meshes/SKM_Manny_Simple.SKM_Manny_Simple"));
	static ConstructorHelpers::FObjectFinder<USkeletalMesh> QuinnBodyFinder(
		TEXT("/Game/Characters/Mannequins/Meshes/SKM_Quinn_Simple.SKM_Quinn_Simple"));
	// Quantum pack sequences (SK_Military skeleton) — used for sequence locomotion (no Quantum AnimBP).
	static ConstructorHelpers::FObjectFinder<UAnimSequence> QuantumIdleFinder(
		TEXT("/Game/QuantumCharacter/Demo/Animations/A_MM_Idle.A_MM_Idle"));
	static ConstructorHelpers::FObjectFinder<UAnimSequence> QuantumWalkFinder(
		TEXT("/Game/QuantumCharacter/Demo/Animations/A_MM_Walk_Fwd.A_MM_Walk_Fwd"));
	static ConstructorHelpers::FObjectFinder<UAnimSequence> QuantumRunFinder(
		TEXT("/Game/QuantumCharacter/Demo/Animations/A_MM_Run_Fwd.A_MM_Run_Fwd"));
	static ConstructorHelpers::FClassFinder<UAnimInstance> MannequinAnimFinder(
		TEXT("/Game/Characters/Mannequins/Anims/Unarmed/ABP_Unarmed"));
	// Survival shares the mannequin skeleton with ABP_Manny (playtest-verified walk/run).
	static ConstructorHelpers::FClassFinder<UAnimInstance> SurvivalAnimFinder(
		TEXT("/Game/Survival_Character/Demo/Characters/Mannequins/Animations/ABP_Manny"));
	static ConstructorHelpers::FObjectFinder<UAnimSequence> SurvivalIdleFinder(
		TEXT("/Game/Survival_Character/Demo/Characters/Mannequins/Animations/Manny/MM_Idle.MM_Idle"));
	static ConstructorHelpers::FObjectFinder<UStaticMesh> OliveRifleFinder(
		TEXT("/Game/QuantumCharacter/Mesh/Rifle/SM_Rifle_Olive.SM_Rifle_Olive"));

	if (QuantumBodyFinder.Succeeded())
	{
		Team0BodyMesh = QuantumBodyFinder.Object;
		ThirdPersonBodyMesh = QuantumBodyFinder.Object;
		bPreserveAuthoredMaterials = true;   // multi-slot military materials — do not wash
	}
	else if (MannequinBodyFinder.Succeeded())
	{
		Team0BodyMesh = MannequinBodyFinder.Object;
		ThirdPersonBodyMesh = MannequinBodyFinder.Object;
	}

	if (SurvivalBodyFinder.Succeeded())
	{
		Team1BodyMesh = SurvivalBodyFinder.Object;
		bPreserveAuthoredMaterials = true;
		if (ThirdPersonBodyMesh == nullptr)
		{
			ThirdPersonBodyMesh = SurvivalBodyFinder.Object;
		}
	}
	else if (QuinnBodyFinder.Succeeded())
	{
		Team1BodyMesh = QuinnBodyFinder.Object;
	}
	else
	{
		Team1BodyMesh = Team0BodyMesh;
	}

	// Quantum: NEVER force mannequin ABP — skeletons don't match (playtest: frozen/broken anims).
	// Sequence locomotion (idle/walk/run) is driven in UpdateSequenceLocomotion.
	if (QuantumIdleFinder.Succeeded()) { Team0IdleAnim = QuantumIdleFinder.Object; }
	if (QuantumWalkFinder.Succeeded()) { Team0WalkAnim = QuantumWalkFinder.Object; }
	if (QuantumRunFinder.Succeeded())  { Team0RunAnim  = QuantumRunFinder.Object; }
	Team0AnimClass = nullptr;

	// Mannequin ABP only for actual mannequin meshes (fallback path).
	if (MannequinAnimFinder.Succeeded())
	{
		ThirdPersonAnimClass = MannequinAnimFinder.Class;
	}
	// Survival (team 1): mannequin-compatible ABP_Manny (playtest-verified).
	if (SurvivalAnimFinder.Succeeded()) { Team1AnimClass = SurvivalAnimFinder.Class; }
	if (SurvivalIdleFinder.Succeeded()) { Team1IdleAnim = SurvivalIdleFinder.Object; }

	// ---- Modular Bandit character spike (Phase 1; assembled only when `pf.BanditChar 1`) ----
	static ConstructorHelpers::FObjectFinder<USkeletalMesh> BanditBodyFinder(
		TEXT("/Game/Bandits/Mesh/Body/SKM_Body.SKM_Body"));
	static ConstructorHelpers::FObjectFinder<UAnimSequence> BanditIdleFinder(
		TEXT("/Game/Bandits/Demo/Animations/A_MM_Idle.A_MM_Idle"));
	static ConstructorHelpers::FObjectFinder<UAnimSequence> BanditWalkFinder(
		TEXT("/Game/Bandits/Demo/Animations/A_MM_Walk_Fwd.A_MM_Walk_Fwd"));
	static ConstructorHelpers::FObjectFinder<UAnimSequence> BanditRunFinder(
		TEXT("/Game/Bandits/Demo/Animations/A_MM_Run_Fwd.A_MM_Run_Fwd"));
	static ConstructorHelpers::FObjectFinder<UAnimSequence> BanditJumpFinder(
		TEXT("/Game/Bandits/Demo/Animations/A_MM_Jump.A_MM_Jump"));
	// UE mannequin wall-jump — closest vault/climb clip in Content; remaps via compatible skeletons.
	static ConstructorHelpers::FObjectFinder<UAnimSequence> WallJumpFinder(
		TEXT("/Game/Characters/Mannequins/Anims/Unarmed/Jump/MM_WallJump.MM_WallJump"));
	if (BanditBodyFinder.Succeeded()) { BanditBodyMesh = BanditBodyFinder.Object; }
	if (BanditIdleFinder.Succeeded()) { BanditIdleAnim = BanditIdleFinder.Object; }
	if (BanditWalkFinder.Succeeded()) { BanditWalkAnim = BanditWalkFinder.Object; }
	if (BanditRunFinder.Succeeded())  { BanditRunAnim  = BanditRunFinder.Object; }
	if (WallJumpFinder.Succeeded())   { MantleAnim = WallJumpFinder.Object; }
	if (BanditJumpFinder.Succeeded()) { MantleAnimFallback = BanditJumpFinder.Object; }

	// Rifle-hold locomotion from the UE5 template rifle kit (/Game/Characters, SK_Mannequin — Manny-family
	// bone names, same family as the Bandit skeleton, so FSkeletonRemapping plays them cleanly; registered
	// compatible by Scripts/add_compatible_skeleton.py). 8-directional walk + jog sets: strafing characters
	// actually sidestep instead of moonwalking a forward loop. Sprint borrows AnimStarterPack's
	// Sprint_Fwd_Rifle (UE4 mannequin — shared bones remap, UE5-only twist bones hold ref pose).
	// Missing packs = null finders = automatic unarmed fallback.
	// Relaxed LOW-READY idle (Shooter Rifle Animations pack) — replaces MF_Rifle_Idle_ADS, an *aiming*
	// idle that held a stationary bot's gun up at its cheek ("gun on forehead", Adam/Tom playtest). The
	// pack is on its own UE4 mannequin skeleton, registered compatible with SKM_Bandit_Skeleton via
	// Scripts/add_compatible_skeleton.py so FSkeletonRemapping plays it on the Bandit body.
	static ConstructorHelpers::FObjectFinder<UAnimSequence> ArmedIdleFinder(
		TEXT("/Game/RifleAnims/AS_Rifle_Idle.AS_Rifle_Idle"));
	static ConstructorHelpers::FObjectFinder<UAnimSequence> ArmedRunFinder(
		TEXT("/Game/RifleAnims/AS_Rifle_RunFwd.AS_Rifle_RunFwd"));
	if (ArmedIdleFinder.Succeeded()) { ArmedIdleAnim = ArmedIdleFinder.Object; }
	if (ArmedRunFinder.Succeeded())  { ArmedRunAnim  = ArmedRunFinder.Object; }

	// 8-direction sets, index = round(atan2(right,fwd)/45°) & 7: 0=Fwd 1=FwdRight 2=Right 3=BwdRight 4=Bwd
	// 5=BwdLeft 6=Left 7=FwdLeft.
	{
		// Relaxed LOW-READY set (Shooter Rifle Animations pack) so MOVING bots are also gun-down, not
		// shouldered-at-the-face. Pack has 6 dirs (no fwd-diagonals) → map fwd-diagonals to pure L/R.
		static const TCHAR* WalkDirPaths[8] = {
			TEXT("/Game/RifleAnims/AS_Rifle_WalkFwd.AS_Rifle_WalkFwd"),
			TEXT("/Game/RifleAnims/AS_Rifle_WalkRight.AS_Rifle_WalkRight"),
			TEXT("/Game/RifleAnims/AS_Rifle_WalkRight.AS_Rifle_WalkRight"),
			TEXT("/Game/RifleAnims/AS_Rifle_WalkBwdRight.AS_Rifle_WalkBwdRight"),
			TEXT("/Game/RifleAnims/AS_Rifle_WalkBwd.AS_Rifle_WalkBwd"),
			TEXT("/Game/RifleAnims/AS_Rifle_WalkBwdLeft.AS_Rifle_WalkBwdLeft"),
			TEXT("/Game/RifleAnims/AS_Rifle_WalkLeft.AS_Rifle_WalkLeft"),
			TEXT("/Game/RifleAnims/AS_Rifle_WalkLeft.AS_Rifle_WalkLeft"),
		};
		static const TCHAR* JogDirPaths[8] = {
			TEXT("/Game/RifleAnims/AS_Rifle_JogFwd.AS_Rifle_JogFwd"),
			TEXT("/Game/RifleAnims/AS_Rifle_JogRight.AS_Rifle_JogRight"),
			TEXT("/Game/RifleAnims/AS_Rifle_JogRight.AS_Rifle_JogRight"),
			TEXT("/Game/RifleAnims/AS_Rifle_JogBwdRight.AS_Rifle_JogBwdRight"),
			TEXT("/Game/RifleAnims/AS_Rifle_JogBwd.AS_Rifle_JogBwd"),
			TEXT("/Game/RifleAnims/AS_Rifle_JogBwdLeft.AS_Rifle_JogBwdLeft"),
			TEXT("/Game/RifleAnims/AS_Rifle_JogLeft.AS_Rifle_JogLeft"),
			TEXT("/Game/RifleAnims/AS_Rifle_JogLeft.AS_Rifle_JogLeft"),
		};
		ArmedWalkDir.SetNum(8);
		ArmedJogDir.SetNum(8);
		for (int32 i = 0; i < 8; ++i)
		{
			ConstructorHelpers::FObjectFinder<UAnimSequence> W(WalkDirPaths[i]);
			ConstructorHelpers::FObjectFinder<UAnimSequence> J(JogDirPaths[i]);
			if (W.Succeeded()) { ArmedWalkDir[i] = W.Object; }
			if (J.Succeeded()) { ArmedJogDir[i] = J.Object; }
		}
		if (ArmedJogDir[0] != nullptr) { ArmedWalkAnim = ArmedJogDir[0]; }   // legacy fwd fallback
	}

	// Directional "he's out" reactions (played on elimination before the body hides — see SetEliminatedAppearance).
	{
		static const TCHAR* DeathPaths[4] = {
			TEXT("/Game/Characters/Mannequins/Anims/Death/MM_Death_Front_01.MM_Death_Front_01"),   // shot from front
			TEXT("/Game/Characters/Mannequins/Anims/Death/MM_Death_Right_01.MM_Death_Right_01"),
			TEXT("/Game/Characters/Mannequins/Anims/Death/MM_Death_Back_01.MM_Death_Back_01"),
			TEXT("/Game/Characters/Mannequins/Anims/Death/MM_Death_Left_01.MM_Death_Left_01"),
		};
		DeathDirAnims.SetNum(4);
		for (int32 i = 0; i < 4; ++i)
		{
			ConstructorHelpers::FObjectFinder<UAnimSequence> D(DeathPaths[i]);
			if (D.Succeeded()) { DeathDirAnims[i] = D.Object; }
		}
	}

	// Unarmed melee punches (UE5 Mannequin Attack set — same bone family as Bandit via compatible skeletons).
	// No dedicated "punch" montage exists in Content; these three short attack clips are the real ones.
	{
		static const TCHAR* MeleePaths[] = {
			TEXT("/Game/Characters/Mannequins/Anims/Unarmed/Attack/MM_Attack_01.MM_Attack_01"),
			TEXT("/Game/Characters/Mannequins/Anims/Unarmed/Attack/MM_Attack_02.MM_Attack_02"),
			TEXT("/Game/Characters/Mannequins/Anims/Unarmed/Attack/MM_Attack_03.MM_Attack_03"),
		};
		MeleeAnims.SetNum(UE_ARRAY_COUNT(MeleePaths));
		for (int32 i = 0; i < UE_ARRAY_COUNT(MeleePaths); ++i)
		{
			ConstructorHelpers::FObjectFinder<UAnimSequence> M(MeleePaths[i]);
			if (M.Succeeded()) { MeleeAnims[i] = M.Object; }
		}
	}

	// Config-driven modular slot components (base skin head/legs + one per PFChar customization slot). Part
	// meshes are assigned at assembly time from the active FPFCharacterConfig via the registry — no hardcoded
	// part paths, so all ~437 parts are reachable.
	auto MakePartComp = [this](const FString& CompName) -> USkeletalMeshComponent*
	{
		USkeletalMeshComponent* C = CreateDefaultSubobject<USkeletalMeshComponent>(*CompName);
		if (C != nullptr)
		{
			C->SetupAttachment(GetMesh());
			C->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			C->SetVisibility(false);
			C->SetHiddenInGame(true);
		}
		return C;
	};
	// 4 base SKIN comps (head/torso/arms/legs) — the visible skin is modular so the LEG region can be dropped
	// under trousers. SKM_Body (the leader) is a one-piece naked body and is only the skeleton/anim carrier.
	for (int32 i = 0; i < PFChar::kBasePartCount; ++i)
	{
		if (USkeletalMeshComponent* C = MakePartComp(FString::Printf(TEXT("CharBase%d"), i)))
		{
			CharBaseComps.Add(C);
		}
	}
	for (int32 i = 0; i < PFChar::SlotCount(); ++i)
	{
		if (USkeletalMeshComponent* C = MakePartComp(FString::Printf(TEXT("CharSlot%d"), i)))
		{
			CharSlotComps.Add(C);
		}
	}

	// Team-colored armband — the team tell on the shared Bandit body. Engine cylinder wrapped thin around the
	// upper arm; re-parented to the arm bone, sized, and tinted at assembly time (see AssembleBanditCharacter).
	auto MakeArmband = [this, CylMesh](const TCHAR* Name) -> UStaticMeshComponent*
	{
		UStaticMeshComponent* C = CreateDefaultSubobject<UStaticMeshComponent>(Name);
		if (C != nullptr)
		{
			C->SetupAttachment(GetMesh());
			if (CylMesh != nullptr) { C->SetStaticMesh(CylMesh); }
			C->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			C->SetCastShadow(false);
			C->SetVisibility(false);
			C->SetHiddenInGame(true);
			C->SetOwnerNoSee(true);   // hidden from the owning first-person view
		}
		return C;
	};
	ArmbandMesh  = MakeArmband(TEXT("Armband"));    // left arm
	ArmbandMeshR = MakeArmband(TEXT("ArmbandR"));   // right arm

	// Soft team-tint fallback (mannequin / graybox only).
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> TeamBodyMatFinder(
		TEXT("/Game/Materials/M_PF_TeamBody.M_PF_TeamBody"));
	if (TeamBodyMatFinder.Succeeded())
	{
		TeamBodyMaterial = TeamBodyMatFinder.Object;
	}
	// Mannequin MIs only applied when NOT preserving authored human materials.
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> MannyMatFinder(
		TEXT("/Game/Characters/Mannequins/Materials/Manny/MI_Manny_01_New.MI_Manny_01_New"));
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> QuinnMatFinder(
		TEXT("/Game/Characters/Mannequins/Materials/Quinn/MI_Quinn_01.MI_Quinn_01"));
	if (!bPreserveAuthoredMaterials)
	{
		if (MannyMatFinder.Succeeded()) { Team0BodyMaterial = MannyMatFinder.Object; }
		if (QuinnMatFinder.Succeeded()) { Team1BodyMaterial = QuinnMatFinder.Object; }
	}

	// TP weapon: Quantum olive rifle if present, else Lyra SM_Rifle.
	if (OliveRifleFinder.Succeeded())
	{
		WeaponMesh = OliveRifleFinder.Object;
	}
	else if (RifleMeshFinder.Succeeded())
	{
		WeaponMesh = RifleMeshFinder.Object;
	}

	// Set the mesh's relative transform HERE (ctor), not just in BeginPlay: the Character Movement
	// Component captures this as its network-smoothing baseline for SIMULATED PROXIES. If it's only
	// set in BeginPlay, proxies smooth around a 0 baseline and the mannequin floats on other
	// players' screens. Mannequin origin is at its feet (measured minZ = 0), so offset = -halfHeight.
	if (ThirdPersonBodyMesh != nullptr && GetMesh() != nullptr)
	{
		GetMesh()->SetRelativeLocationAndRotation(
			FVector(0.f, 0.f, -Capsule->GetUnscaledCapsuleHalfHeight()),
			FRotator(0.f, -90.f, 0.f));
	}

	// ---- Cross-package components (own their replication flags in their ctors) ----
	WeaponComponent      = CreateDefaultSubobject<UPFWeaponComponent>(TEXT("WeaponComponent"));
	BuildComponent       = CreateDefaultSubobject<UPFBuildComponent>(TEXT("BuildComponent"));
	HealthComponent      = CreateDefaultSubobject<UPFHealthComponent>(TEXT("HealthComponent"));
	CombatAudioComponent = CreateDefaultSubobject<UPFCombatAudio>(TEXT("CombatAudioComponent"));
	CombatVFXComponent   = CreateDefaultSubobject<UPFCombatVFX>(TEXT("CombatVFXComponent"));

	bSprintKeyHeld = false;
	bADSHeld = false;
	bFireHeld = false;
	bJumpKeyHeld = false;
	bADSToggleMode = false;
	bCrouchToggleMode = false;
}

// ---------------------------------------------------------------------------
// Accessors
// ---------------------------------------------------------------------------

UPFCharacterMovementComponent* ACombatForgeCharacter::GetPFMovement() const { return PFMovement; }
UPFWeaponComponent*  ACombatForgeCharacter::GetWeapon() const      { return WeaponComponent; }
UPFBuildComponent*   ACombatForgeCharacter::GetBuild() const       { return BuildComponent; }
UPFHealthComponent*  ACombatForgeCharacter::GetHealth() const      { return HealthComponent; }
UPFCombatAudio*      ACombatForgeCharacter::GetCombatAudio() const { return CombatAudioComponent; }
UPFCombatVFX*        ACombatForgeCharacter::GetCombatVFX() const   { return CombatVFXComponent; }
UCameraComponent*    ACombatForgeCharacter::GetFirstPersonCamera() const { return FirstPersonCamera; }

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void ACombatForgeCharacter::BeginPlay()
{
	Super::BeginPlay();

	if (PFMovement != nullptr)
	{
		PFMovement->OnSlideStateChanged.AddUObject(this, &ACombatForgeCharacter::HandleSlideStateChanged);
		PFMovement->OnMantleStateChanged.AddUObject(this, &ACombatForgeCharacter::HandleMantleStateChanged);
	}

	FallStartPeakZ = GetActorLocation().Z;

	ApplyArtLoadout();       // swaps in real body/arms/weapon if assigned; no-op (graybox) otherwise
	SetupWeaponMaterials();  // dark gunmetal marker + emissive flash blobs
	ApplyWeaponLoadout();    // swap to the player's selected weapon (mesh/material/pose) — after the above

	// Soft wind/arena bed for the local player only (SFX volume scaled).
	if (IsLocallyControlled())
	{
		BaseFOV = FPFUserPrefs::GetFieldOfView();
		if (UPFCombatAudio* Audio = GetCombatAudio())
		{
			Audio->StartAmbientBed();
		}
	}
}

void ACombatForgeCharacter::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (PFMovement != nullptr)
	{
		PFMovement->OnSlideStateChanged.RemoveAll(this);
		PFMovement->OnMantleStateChanged.RemoveAll(this);
	}
	GetWorldTimerManager().ClearTimer(SprintOutTimerHandle);
	GetWorldTimerManager().ClearTimer(BufferedJumpClearHandle);
	if (UPFCombatAudio* Audio = GetCombatAudio())
	{
		Audio->StopAmbientBed();
	}
	SessionWeaponPoses.Empty();

	Super::EndPlay(EndPlayReason);
}

void ACombatForgeCharacter::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	// Track peak height while falling for the landing-dip threshold.
	if (PFMovement != nullptr && PFMovement->IsFalling())
	{
		FallStartPeakZ = FMath::Max(FallStartPeakZ, GetActorLocation().Z);
	}

	// ADS transition alpha advances on every role: the server reads
	// GetADSAlpha() for the authoritative spread cone (04 §2.3), so it must
	// track the move-stream ADS intent even for remotely controlled pawns.
	UpdateADSAlpha(DeltaSeconds);

	// AI footstep noise (server-authoritative, every pawn: host player, remote players, bots). A RUNNING
	// (sprinting) character is audible 360° — bots within range hear the footfalls and turn to investigate,
	// even from directly behind. Sneaking (walk/crouch, i.e. NOT sprinting) is silent, so it's found only by a
	// bot's actual line of sight. Runs on the server where bot perception lives; the server knows every pawn's
	// authoritative sprint state + velocity (the cosmetic footstep SOUND is separate, and client-local).
	if (HasAuthority() && PFMovement != nullptr && !PFMovement->IsFalling() && !PFMovement->IsSliding())
	{
		FootstepNoiseTimer -= DeltaSeconds;
		const bool bRunning = PFMovement->IsSprintingEffective() && GetVelocity().Size2D() > 200.f;
		if (bRunning && FootstepNoiseTimer <= 0.f)
		{
			FootstepNoiseTimer = FootstepNoiseInterval;
			UAISense_Hearing::ReportNoiseEvent(GetWorld(), GetActorLocation(), 1.f, this, FootstepHearRangeUU, TEXT("Footstep"));
		}
	}

	// Melee punch clip: hold locomotion off while the attack sequence plays, then resume.
	if (MeleeSwingAnimRemain > 0.f)
	{
		MeleeSwingAnimRemain = FMath::Max(0.f, MeleeSwingAnimRemain - DeltaSeconds);
		if (MeleeSwingAnimRemain <= 0.f)
		{
			// Restore TP gun if we hid it for the punch, and re-pick idle/walk.
			if (WeaponMeshComp != nullptr && !bEliminatedAppearanceActive)
			{
				WeaponMeshComp->SetHiddenInGame(false);
			}
			SeqLocoState = 0;
			UpdateSequenceLocomotion();
		}
	}
	else
	{
		// Quantum (and any sequence-driven body): idle ↔ walk ↔ run without an AnimBP.
		UpdateSequenceLocomotion();
	}

	// Build phase: no marker in hands (placement HUD has its own aim dot).
	UpdateBuildPhaseWeaponVisibility();

	// TP rifle: hand-carry, or eye-line raise while ADS / firing (then back to hand).
	if (WeaponRaiseHoldSec > 0.f)
	{
		WeaponRaiseHoldSec = FMath::Max(0.f, WeaponRaiseHoldSec - DeltaSeconds);
	}
	UpdateWeaponHoldPose();

	// Server-only: police completely-idle remote players (AFK kick).
	if (HasAuthority())
	{
		TickServerAfk(DeltaSeconds);
	}

	if (IsLocallyControlled())
	{
		// Slide/sprint state can change without new key events; keep the CMC
		// intents (and the queued-ADS rule) current every frame.
		UpdateMovementIntents();
		UpdateTargetFOV(DeltaSeconds);

		// Crouch camera smoothing: 88 -> 58 reads as 30 uu at 150 uu/s = 0.2 s.
		if (FirstPersonCamera != nullptr && GetCapsuleComponent() != nullptr)
		{
			const float TargetZ = GetCapsuleComponent()->GetScaledCapsuleHalfHeight() - CameraEyeOffsetFromCapsuleTop;
			FVector Rel = FirstPersonCamera->GetRelativeLocation();
			Rel.Z = FMath::FInterpConstantTo(Rel.Z, TargetZ, DeltaSeconds, CrouchCameraInterpSpeed);
			FirstPersonCamera->SetRelativeLocation(Rel);
		}

		// Viewmodel: ADS pulls iron-sights toward center; recoil springs back each shot.
		if (ViewModelRoot != nullptr)
		{
			RecoilOffset = FMath::VInterpTo(RecoilOffset, FVector::ZeroVector, DeltaSeconds, RecoilRecoverSpeed);
			RecoilPitch = FMath::FInterpTo(RecoilPitch, 0.f, DeltaSeconds, RecoilRecoverSpeed);

			// Procedural reload animation: dip the marker down + tilt the muzzle for the reload's duration so a
			// reload reads visually (there is no skeletal reload anim on the FP mesh). Start on the reload edge.
			const bool bNowReloading = (WeaponComponent != nullptr) && WeaponComponent->bReloading;
			if (bNowReloading && !bWasReloading)
			{
				bReloadDipActive = true;
				ReloadDipElapsed = 0.f;
				ReloadDipDuration = (WeaponComponent != nullptr) ? FMath::Max(0.2f, WeaponComponent->ReloadTime) : 1.f;
			}
			bWasReloading = bNowReloading;

			FVector ReloadLoc = FVector::ZeroVector;
			float ReloadPitch = 0.f;
			if (bReloadDipActive)
			{
				ReloadDipElapsed += DeltaSeconds;
				const float A = FMath::Clamp(ReloadDipElapsed / FMath::Max(ReloadDipDuration, 0.01f), 0.f, 1.f);
				const float Dip = FMath::Sin(A * PI);   // 0 -> 1 -> 0 across the reload
				ReloadLoc = FVector(-4.f * Dip, 0.f, -10.f * Dip);   // pull in + down
				ReloadPitch = 20.f * Dip;                            // muzzle tilts down
				if (A >= 1.f)
				{
					bReloadDipActive = false;
				}
			}

			// Ease-out cubic (same curve as the FOV) so the gun snaps toward the sight and settles in lockstep
			// with the zoom instead of drifting linearly.
			const float RawAds = FMath::Clamp(ADSAlpha, 0.f, 1.f);
			const float Ads = 1.f - FMath::Cube(1.f - RawAds);
			const FVector Home = FMath::Lerp(ViewModelHomeLoc, ViewModelAdsLoc, Ads);
			const FRotator HomeRot = FMath::Lerp(FRotator::ZeroRotator, ViewModelAdsRot, Ads);

			// Procedural feel: look-lag sway, movement bob, idle breath, fire-kick springs, landing dip.
			FVector FeelLoc = FVector::ZeroVector;
			FRotator FeelRot = FRotator::ZeroRotator;
			UpdateViewmodelFeel(DeltaSeconds, FeelLoc, FeelRot);

			ViewModelRoot->SetRelativeLocation(Home + RecoilOffset + ReloadLoc + FeelLoc);
			ViewModelRoot->SetRelativeRotation(FRotator(
				RecoilPitch + HomeRot.Pitch + ReloadPitch + FeelRot.Pitch,
				HomeRot.Yaw + FeelRot.Yaw,
				HomeRot.Roll + FeelRot.Roll));

			// Dev pose drag (pf.WeaponDrag): apply the middle-mouse mouse delta to the held/ADS pose.
			TickWeaponDrag();

			// pf.ShowMuzzle 1: draw the auto-resolved cosmetic muzzle (mesh socket/bounds tip) + shot line.
			// Green sphere = origin; cyan line = shot path.
			if (CVarShowMuzzle.GetValueOnGameThread() != 0)
			{
				const FVector MuzzleW = GetMuzzleLocation(true);
				const FVector AimW = GetControlRotation().Vector();
				DrawDebugSphere(GetWorld(), MuzzleW, 1.6f, 10, FColor::Green, false, -1.f, 0, 0.3f);
				DrawDebugLine(GetWorld(), MuzzleW, MuzzleW + AimW * 60.f, FColor::Cyan, false, -1.f, 0, 0.4f);
			}
		}

		// Footsteps — local only, grounded movement.
		if (PFMovement != nullptr && !PFMovement->IsFalling() && !PFMovement->IsSliding())
		{
			const float Speed2D = GetVelocity().Size2D();
			if (Speed2D > 80.f)
			{
				const bool bSprint = PFMovement->IsSprintingEffective();
				const float Stride = bSprint ? FootstepStrideSprintUU : FootstepStrideWalkUU;
				FootstepDistanceAccum += Speed2D * DeltaSeconds;
				if (FootstepDistanceAccum >= Stride)
				{
					FootstepDistanceAccum = 0.f;
					if (UPFCombatAudio* Audio = GetCombatAudio())
					{
						Audio->PlayFootstep(bSprint);
					}
				}
			}
			else
			{
				FootstepDistanceAccum = 0.f;
			}
		}
	}
}

// ---------------------------------------------------------------------------
// Input setup
// ---------------------------------------------------------------------------

void ACombatForgeCharacter::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
	Super::SetupPlayerInputComponent(PlayerInputComponent);

	UEnhancedInputComponent* EIC = Cast<UEnhancedInputComponent>(PlayerInputComponent);
	ACombatForgePlayerController* PC = Cast<ACombatForgePlayerController>(GetController());
	if (EIC == nullptr || PC == nullptr)
	{
		UE_LOG(CombatForgeLog, Warning,
			TEXT("ACombatForgeCharacter::SetupPlayerInputComponent: missing EnhancedInputComponent or CombatForge PC"));
		return;
	}

	const UPFInputConfig* Cfg = PC->GetInputConfig();
	if (Cfg == nullptr)
	{
		UE_LOG(CombatForgeLog, Error, TEXT("SetupPlayerInputComponent: PC returned null UPFInputConfig"));
		return;
	}

	EIC->BindAction(Cfg->IA_Move, ETriggerEvent::Triggered, this, &ACombatForgeCharacter::OnMoveInput);
	EIC->BindAction(Cfg->IA_Look, ETriggerEvent::Triggered, this, &ACombatForgeCharacter::OnLookInput);
	EIC->BindAction(Cfg->IA_Jump, ETriggerEvent::Started, this, &ACombatForgeCharacter::OnJumpPressed);
	EIC->BindAction(Cfg->IA_Jump, ETriggerEvent::Completed, this, &ACombatForgeCharacter::OnJumpReleased);
	EIC->BindAction(Cfg->IA_Sprint, ETriggerEvent::Started, this, &ACombatForgeCharacter::OnSprintPressed);
	EIC->BindAction(Cfg->IA_Sprint, ETriggerEvent::Completed, this, &ACombatForgeCharacter::OnSprintReleased);
	EIC->BindAction(Cfg->IA_CrouchSlide, ETriggerEvent::Started, this, &ACombatForgeCharacter::OnCrouchSlidePressed);
	EIC->BindAction(Cfg->IA_CrouchSlide, ETriggerEvent::Completed, this, &ACombatForgeCharacter::OnCrouchSlideReleased);
	EIC->BindAction(Cfg->IA_Fire, ETriggerEvent::Started, this, &ACombatForgeCharacter::OnFirePressed);
	EIC->BindAction(Cfg->IA_Fire, ETriggerEvent::Completed, this, &ACombatForgeCharacter::OnFireReleased);
	EIC->BindAction(Cfg->IA_ADS, ETriggerEvent::Started, this, &ACombatForgeCharacter::OnADSPressed);
	EIC->BindAction(Cfg->IA_ADS, ETriggerEvent::Completed, this, &ACombatForgeCharacter::OnADSReleased);
	EIC->BindAction(Cfg->IA_Reload, ETriggerEvent::Started, this, &ACombatForgeCharacter::OnReloadPressed);
	if (Cfg->IA_Interact)
	{
		EIC->BindAction(Cfg->IA_Interact, ETriggerEvent::Started, this, &ACombatForgeCharacter::OnInteractPressed);
		EIC->BindAction(Cfg->IA_Interact, ETriggerEvent::Completed, this, &ACombatForgeCharacter::OnInteractReleased);
	}
	if (Cfg->IA_PlantBomb)
	{
		EIC->BindAction(Cfg->IA_PlantBomb, ETriggerEvent::Started, this, &ACombatForgeCharacter::OnPlantPressed);
	}
	if (Cfg->IA_FireSelect)
	{
		EIC->BindAction(Cfg->IA_FireSelect, ETriggerEvent::Started, this, &ACombatForgeCharacter::OnFireSelectPressed);
	}
	if (Cfg->IA_ThrowFrag)
	{
		EIC->BindAction(Cfg->IA_ThrowFrag, ETriggerEvent::Started, this, &ACombatForgeCharacter::OnThrowFragPressed);
	}
	if (Cfg->IA_ThrowSmoke)
	{
		EIC->BindAction(Cfg->IA_ThrowSmoke, ETriggerEvent::Started, this, &ACombatForgeCharacter::OnThrowSmokePressed);
	}
	if (Cfg->IA_Melee)
	{
		EIC->BindAction(Cfg->IA_Melee, ETriggerEvent::Started, this, &ACombatForgeCharacter::OnMeleePressed);
	}
	if (Cfg->IA_WeaponDrag)
	{
		EIC->BindAction(Cfg->IA_WeaponDrag, ETriggerEvent::Started, this, &ACombatForgeCharacter::OnWeaponDragPressed);
		EIC->BindAction(Cfg->IA_WeaponDrag, ETriggerEvent::Completed, this, &ACombatForgeCharacter::OnWeaponDragReleased);
	}

	// pkg-building owns every IMC_Build action (§3.3 / §3.5).
	if (BuildComponent != nullptr)
	{
		BuildComponent->BindInput(EIC, Cfg);
	}

	// Pick up hold-vs-toggle prefs for this (locally controlled) pawn.
	RefreshADSToggleMode();
	RefreshCrouchToggleMode();
}

// ---------------------------------------------------------------------------
// Movement input
// ---------------------------------------------------------------------------

void ACombatForgeCharacter::OnMoveInput(const FInputActionValue& Value)
{
	const FVector2D Axis = Value.Get<FVector2D>();
	if (Controller == nullptr)
	{
		return;
	}
	const FRotator YawRotation(0.f, Controller->GetControlRotation().Yaw, 0.f);
	AddMovementInput(FRotationMatrix(YawRotation).GetUnitAxis(EAxis::X), Axis.Y);
	AddMovementInput(FRotationMatrix(YawRotation).GetUnitAxis(EAxis::Y), Axis.X);
}

void ACombatForgeCharacter::OnLookInput(const FInputActionValue& Value)
{
	// While dragging the weapon pose, the mouse moves the GUN, not the camera (TickWeaponDrag reads the raw
	// mouse delta). Swallow the look input so the view doesn't spin under the drag.
	if (bWeaponDragging)
	{
		return;
	}
	const FVector2D Axis = Value.Get<FVector2D>();
	AddControllerYawInput(Axis.X);
	AddControllerPitchInput(Axis.Y); // Y already negated + scaled by the mapping modifiers
}

void ACombatForgeCharacter::OnWeaponDragPressed()
{
	if (CVarWeaponDrag.GetValueOnGameThread() != 0 && IsLocallyControlled())
	{
		bWeaponDragging = true;
		if (GEngine)
		{
			GEngine->AddOnScreenDebugMessage(0x57445247 /*'WDRG'*/, 3.f, FColor::Cyan,
				TEXT("Drag: MMB move · Ctrl+MMB pitch/yaw muzzle · Alt+MMB roll · Shift depth"));
		}
	}
}

void ACombatForgeCharacter::OnWeaponDragReleased()
{
	if (bWeaponDragging)
	{
		bWeaponDragging = false;
		CacheCurrentWeaponPose();  // keep live edit when cycling guns mid-session
		PrintWeaponPoseLine();     // dump the paste-ready line for catalog bake
	}
}

void ACombatForgeCharacter::TickWeaponDrag()
{
	if (!bWeaponDragging || RifleFPMesh == nullptr)
	{
		return;
	}
	APlayerController* PC = Cast<APlayerController>(GetController());
	if (PC == nullptr)
	{
		return;
	}
	float DX = 0.f, DY = 0.f;
	PC->GetInputMouseDelta(DX, DY);   // raw mouse delta this frame (independent of look sensitivity)
	if (FMath::IsNearlyZero(DX) && FMath::IsNearlyZero(DY))
	{
		return;
	}

	const bool bDepth  = PC->IsInputKeyDown(EKeys::LeftShift);     // mouse-Y → forward/back instead of up/down
	const bool bRotate = PC->IsInputKeyDown(EKeys::LeftControl);   // rotate barrel (pitch/yaw), NOT translate
	const bool bRoll   = PC->IsInputKeyDown(EKeys::LeftAlt);       // roll the gun about the barrel axis
	const bool bAds    = IsADS();                                  // ADS pose vs held pose
	constexpr float MoveScale = 0.12f;   // cm per mouse unit
	constexpr float RotScale  = 0.4f;    // deg per mouse unit

	if (bRotate || bRoll)
	{
		// Aim the MUZZLE (rotate the gun), never slide its position.
		// Viewmodel space: Pitch tips barrel up/down, Yaw turns left/right, Roll banks about the bore.
		// Mouse-down → muzzle dips (negative pitch). Mouse-right → yaw right.
		FRotator Delta = FRotator::ZeroRotator;
		if (bRoll)
		{
			// Alt: bank the weapon (barrel twist). Horizontal mouse is most natural.
			Delta.Roll = DX * RotScale;
		}
		else
		{
			// Ctrl: pitch + yaw. Flip mouse-Y so dragging down points the barrel down.
			Delta.Pitch = -DY * RotScale;
			Delta.Yaw   =  DX * RotScale;
		}
		if (bAds)
		{
			ViewModelAdsRot = (ViewModelAdsRot + Delta).GetNormalized();
		}
		else
		{
			RifleFPMesh->SetRelativeRotation(
				(RifleFPMesh->GetRelativeRotation() + Delta).GetNormalized());
		}
	}
	else
	{
		// Screen plane translate: mouse-X → gun right (+Y), mouse-Y → gun up (+Z). Shift = depth (+X).
		const float Right = DX * MoveScale;
		const float Vert  = -DY * MoveScale;   // screen-up (negative DY) → gun up
		const FVector Delta = bDepth ? FVector(Vert, Right, 0.f) : FVector(0.f, Right, Vert);
		if (bAds)
		{
			ViewModelAdsLoc += Delta;
		}
		else
		{
			RifleFPMesh->SetRelativeLocation(RifleFPMesh->GetRelativeLocation() + Delta);
		}
	}
}

void ACombatForgeCharacter::PrintWeaponPoseLine() const
{
	auto V = [](const FVector& X) { return FString::Printf(TEXT("%.2f %.2f %.2f"), X.X, X.Y, X.Z); };
	auto R = [](const FRotator& X) { return FString::Printf(TEXT("%.2f %.2f %.2f"), X.Pitch, X.Yaw, X.Roll); };
	auto VF = [](const FVector& X) {
		return FString::Printf(TEXT("FVector(%.2ff, %.2ff, %.2ff)"), X.X, X.Y, X.Z);
	};
	auto RF = [](const FRotator& X) {
		return FString::Printf(TEXT("FRotator(%.2ff, %.2ff, %.2ff)"), X.Pitch, X.Yaw, X.Roll);
	};

	const FString Id = PFWeapon::IdOf(ActiveWeaponConfig.Category, ActiveWeaponConfig.Index);
	const FString Name = PFWeapon::WeaponDisplayName(ActiveWeaponConfig.Category, ActiveWeaponConfig.Index);
	const FVector FPLoc = RifleFPMesh ? RifleFPMesh->GetRelativeLocation() : FVector::ZeroVector;
	const FRotator FPRot = RifleFPMesh ? RifleFPMesh->GetRelativeRotation() : FRotator::ZeroRotator;
	const float Scale = RifleFPMesh ? RifleFPMesh->GetRelativeScale3D().X : 1.f;

	const FString Header = FString::Printf(
		TEXT("=== WEAPON POSE [%s] %s  cat=%d idx=%d ==="),
		*Id, *Name, ActiveWeaponConfig.Category, ActiveWeaponConfig.Index);
	const FString HipCmd = FString::Printf(TEXT("pf.WeaponFP %s %s %.3f %s"),
		*V(FPLoc), *R(FPRot), Scale, *V(MuzzleLocalFP));
	const FString AdsCmd = FString::Printf(TEXT("pf.WeaponADS %s %s"),
		*V(ViewModelAdsLoc), *R(ViewModelAdsRot));
	const FString Catalog = FString::Printf(
		TEXT("  // catalog paste for %s\n  // FPLoc=%s FPRot=%s FPScale=%.3ff MuzzleFP=%s\n  // AdsLoc=%s AdsRot=%s"),
		*Id, *VF(FPLoc), *RF(FPRot), Scale, *VF(MuzzleLocalFP),
		*VF(ViewModelAdsLoc), *RF(ViewModelAdsRot));

	UE_LOG(CombatForgeLog, Log, TEXT("%s"), *Header);
	UE_LOG(CombatForgeLog, Log, TEXT("%s"), *HipCmd);
	UE_LOG(CombatForgeLog, Log, TEXT("%s"), *AdsCmd);
	UE_LOG(CombatForgeLog, Log, TEXT("%s"), *Catalog);

	if (GEngine != nullptr)
	{
		GEngine->AddOnScreenDebugMessage(-1, 14.f, FColor::Cyan, Header);
		GEngine->AddOnScreenDebugMessage(-1, 14.f, FColor::Green, HipCmd);
		GEngine->AddOnScreenDebugMessage(-1, 14.f, FColor::Yellow, AdsCmd);
	}
}

void ACombatForgeCharacter::CacheCurrentWeaponPose()
{
	if (!IsLocallyControlled() || RifleFPMesh == nullptr)
	{
		return;
	}
	const FString IdStr = PFWeapon::IdOf(ActiveWeaponConfig.Category, ActiveWeaponConfig.Index);
	if (IdStr.IsEmpty())
	{
		return;
	}
	FPFSessionWeaponPose& P = SessionWeaponPoses.FindOrAdd(FName(*IdStr));
	P.FPLoc = RifleFPMesh->GetRelativeLocation();
	P.FPRot = RifleFPMesh->GetRelativeRotation();
	P.FPScale = RifleFPMesh->GetRelativeScale3D().X;
	P.MuzzleFP = MuzzleLocalFP;
	P.AdsLoc = ViewModelAdsLoc;
	P.AdsRot = ViewModelAdsRot;
}

bool ACombatForgeCharacter::TryApplySessionWeaponPose(const FName& WeaponId, FVector& InOutFPLoc,
	FRotator& InOutFPRot, float& InOutFPScale, FVector& InOutMuzzle, FVector& InOutAdsLoc,
	FRotator& InOutAdsRot) const
{
	if (WeaponId.IsNone())
	{
		return false;
	}
	if (const FPFSessionWeaponPose* P = SessionWeaponPoses.Find(WeaponId))
	{
		InOutFPLoc = P->FPLoc;
		InOutFPRot = P->FPRot;
		InOutFPScale = P->FPScale;
		InOutMuzzle = P->MuzzleFP;
		InOutAdsLoc = P->AdsLoc;
		InOutAdsRot = P->AdsRot;
		return true;
	}
	return false;
}

void ACombatForgeCharacter::DevEquipCatalogWeapon(int32 Category, int32 Index)
{
	if (!IsLocallyControlled())
	{
		return;
	}
	// Stash the gun we're leaving so cycling back restores live drag edits (not a stale re-bake).
	CacheCurrentWeaponPose();

	Category = FMath::Clamp(Category, 0, PFWeapon::CategoryCount() - 1);
	Index = FMath::Clamp(Index, 0, FMath::Max(0, PFWeapon::WeaponCount(Category) - 1));

	// Pose tuning session: drag on, auto-pose off so you edit catalog / session values, not a regen.
	if (IConsoleVariable* Drag = IConsoleManager::Get().FindConsoleVariable(TEXT("pf.WeaponDrag")))
	{
		Drag->Set(1, ECVF_SetByConsole);   // higher priority than SetByCode so it sticks
	}
	if (IConsoleVariable* Auto = IConsoleManager::Get().FindConsoleVariable(TEXT("pf.WeaponAutoPose")))
	{
		Auto->Set(0, ECVF_SetByConsole);
	}

	FPFWeaponConfig C;
	C.Category = Category;
	C.Index = Index;
	PrimaryWeaponConfig = C;
	// Keep secondary as-is; force primary in hand for a clean tune view.
	bSecondaryActive = false;
	PFWeapon::SaveConfig(C);

	// Kit rep so listen host + any remote see the same gun stats/mesh.
	if (HasValidKit() || GetController())
	{
		KitRep.WeaponCategory = static_cast<uint8>(C.Category);
		KitRep.WeaponIndex = static_cast<uint8>(C.Index);
		if (!HasAuthority())
		{
			ServerSetKit(KitRep);
		}
	}
	ApplyWeaponLoadout();

	const FPFWeaponDef& Def = PFWeapon::Weapon(C.Category, C.Index);
	const bool bFromSession = SessionWeaponPoses.Contains(FName(Def.WeaponId ? Def.WeaponId : TEXT("")));
	const FString Msg = FString::Printf(
		TEXT("Equipped [%s] %s  (%d/%d in %s)%s  — MMB move · Ctrl+MMB pitch · release logs"),
		Def.WeaponId ? Def.WeaponId : TEXT("?"), Def.DisplayName,
		C.Index + 1, PFWeapon::WeaponCount(C.Category),
		*PFWeapon::CategoryLabel(C.Category),
		bFromSession ? TEXT(" [session edit]") : TEXT(""));
	UE_LOG(CombatForgeLog, Log, TEXT("%s"), *Msg);
	if (GEngine)
	{
		GEngine->AddOnScreenDebugMessage(-1, 8.f, FColor::Orange, Msg);
	}
}

bool ACombatForgeCharacter::DevCycleCatalogWeapon(int32 Dir)
{
	if (!IsLocallyControlled() || Dir == 0)
	{
		return false;
	}
	// Flatten catalog into a single ring: AR0, AR1, … SMG0, …
	TArray<TPair<int32, int32>> Flat;
	for (int32 Cat = 0; Cat < PFWeapon::CategoryCount(); ++Cat)
	{
		const int32 N = PFWeapon::WeaponCount(Cat);
		for (int32 Idx = 0; Idx < N; ++Idx)
		{
			Flat.Emplace(Cat, Idx);
		}
	}
	if (Flat.Num() == 0)
	{
		return false;
	}
	int32 Cur = 0;
	for (int32 i = 0; i < Flat.Num(); ++i)
	{
		if (Flat[i].Key == ActiveWeaponConfig.Category && Flat[i].Value == ActiveWeaponConfig.Index)
		{
			Cur = i;
			break;
		}
	}
	const int32 Next = (Cur + Dir % Flat.Num() + Flat.Num()) % Flat.Num();
	DevEquipCatalogWeapon(Flat[Next].Key, Flat[Next].Value);
	return true;
}

void ACombatForgeCharacter::OnJumpPressed()
{
	bJumpKeyHeld = true;
	LastJumpPressedTime = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.f;
	// 2nd SPACE press while airborne = mantle intent (double-space ledge climb, Tom 2026-07-17). The intent
	// rides FLAG_Custom_2 in the move stream; the CMC enters the climb deterministically the moment a valid
	// ≤1-level ledge is ahead (and keeps retrying while the key is held — forgiving timing).
	if (PFMovement != nullptr && PFMovement->IsFalling() && !PFMovement->IsMantling())
	{
		PFMovement->SetWantsToMantle(true);
	}
	Jump(); // no coyote time; pre-landing presses are re-armed in Landed()
}

void ACombatForgeCharacter::OnJumpReleased()
{
	bJumpKeyHeld = false;
	if (PFMovement != nullptr)
	{
		PFMovement->SetWantsToMantle(false);   // a whiffed 2nd press must not latch forever
	}
	StopJumping();
}

void ACombatForgeCharacter::OnSprintPressed()
{
	bSprintKeyHeld = true;
	UpdateMovementIntents();
}

void ACombatForgeCharacter::OnSprintReleased()
{
	bSprintKeyHeld = false;
	UpdateMovementIntents();
}

void ACombatForgeCharacter::OnCrouchSlidePressed()
{
	if (PFMovement == nullptr)
	{
		return;
	}
	// Hold mode: press enters crouch (release exits). Toggle mode: each press flips crouch intent.
	// CMC still decides slide vs crouch from (bWantsToCrouch + sprint + speed) either way.
	if (bCrouchToggleMode)
	{
		if (PFMovement->bWantsToCrouch)
		{
			PFMovement->OnCrouchSlideReleased();
		}
		else
		{
			PFMovement->OnCrouchSlidePressed();
		}
	}
	else
	{
		PFMovement->OnCrouchSlidePressed();
	}
}

void ACombatForgeCharacter::OnCrouchSlideReleased()
{
	// Toggle mode ignores the release; hold mode stands on release.
	if (!bCrouchToggleMode && PFMovement != nullptr)
	{
		PFMovement->OnCrouchSlideReleased();
	}
}

// ---------------------------------------------------------------------------
// Fire / ADS gating (04 §1.1: sprint blocks fire/ADS; sprint-out 0.18 s)
// ---------------------------------------------------------------------------

void ACombatForgeCharacter::OnFirePressed()
{
	bFireHeld = true;

	// Capture sprint state BEFORE the intent update cancels it.
	const bool bNeedsRaise = (PFMovement != nullptr) && PFMovement->IsSprintingEffective();
	UpdateMovementIntents(); // fire held -> sprint intent drops immediately

	if (bNeedsRaise)
	{
		const float SprintOutDelay = (WeaponComponent != nullptr) ? WeaponComponent->SprintOutTime : 0.18f;
		GetWorldTimerManager().SetTimer(SprintOutTimerHandle, this,
			&ACombatForgeCharacter::OnSprintOutFinished, SprintOutDelay, false);
	}
	else if (!GetWorldTimerManager().IsTimerActive(SprintOutTimerHandle))
	{
		if (WeaponComponent != nullptr)
		{
			WeaponComponent->StartFire();
		}
	}
}

void ACombatForgeCharacter::OnSprintOutFinished()
{
	// Buffered fire releases when the raise timer ends (04 §1.1).
	if (bFireHeld && WeaponComponent != nullptr)
	{
		WeaponComponent->StartFire();
	}
}

void ACombatForgeCharacter::OnFireReleased()
{
	bFireHeld = false;
	GetWorldTimerManager().ClearTimer(SprintOutTimerHandle);
	if (WeaponComponent != nullptr)
	{
		WeaponComponent->StopFire();
	}
	UpdateMovementIntents(); // shift still held -> sprint resumes
}

void ACombatForgeCharacter::OnADSPressed()
{
	// Hold mode: press enters ADS (release exits). Toggle mode: each press flips the standing intent.
	SetADS(bADSToggleMode ? !bADSHeld : true);
}

void ACombatForgeCharacter::OnADSReleased()
{
	// Toggle mode ignores the release; hold mode exits ADS on release.
	if (!bADSToggleMode)
	{
		SetADS(false);
	}
}

void ACombatForgeCharacter::OnReloadPressed()
{
	if (WeaponComponent != nullptr)
	{
		WeaponComponent->StartReload();
	}
}

FString ACombatForgeCharacter::GetInteractPromptText() const
{
	if (!IsLocallyControlled())
	{
		return FString();
	}
	if (const UPFHealthComponent* Health = GetHealth(); Health && Health->bEliminated)
	{
		return FString();
	}
	UWorld* World = GetWorld();
	if (!World)
	{
		return FString();
	}

	// Nearest usable door in range (same gate as OnInteractPressed).
	const APFBuildPieceActor* BestDoor = nullptr;
	float BestDistSq = FMath::Square(APFBuildPieceActor::DoorInteractRangeUU);
	const FVector Me = GetActorLocation();
	for (TActorIterator<APFBuildPieceActor> It(World); It; ++It)
	{
		const APFBuildPieceActor* Door = *It;
		if (!Door || !Door->CanUserToggleDoor(this))
		{
			continue;
		}
		const float D = FVector::DistSquared(Me, Door->GetDoorInteractLocation());
		if (D <= BestDistSq)
		{
			BestDistSq = D;
			BestDoor = Door;
		}
	}
	if (BestDoor)
	{
		return BestDoor->IsOpen() ? TEXT("F to close") : TEXT("F to open");
	}
	return FString();
}

void ACombatForgeCharacter::OnInteractPressed()
{
	if (!IsLocallyControlled())
	{
		return;
	}
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	// Defuse first: if an armed bomb is in reach, F becomes hold-to-defuse (8 s). The client scan is armed +
	// range ONLY (bArmed replicates; team/planter validity is decided by ServerBeginDefuse + the bomb's own
	// Tick — a client-side team gate broke FFA and, worse, read server-only state).
	for (TActorIterator<APFBombActor> It(World); It; ++It)
	{
		const APFBombActor* Bomb = *It;
		if (Bomb && Bomb->IsArmed()
			&& FVector::DistSquared(GetActorLocation(), Bomb->GetActorLocation())
				<= FMath::Square(APFBombActor::DefuseRangeUU))
		{
			ServerBeginDefuse();
			return;
		}
	}
	// Mid-field floating bomb charge (F to pick up — not granted at spawn).
	{
		APFBombPickup* BestPickup = nullptr;
		float BestDistSq = FMath::Square(APFBombPickup::InteractRangeUU);
		const FVector Me = GetActorLocation();
		for (TActorIterator<APFBombPickup> It(World); It; ++It)
		{
			APFBombPickup* P = *It;
			if (!P || !P->IsAvailable())
			{
				continue;
			}
			const float D = FVector::DistSquared(Me, P->GetActorLocation());
			if (D <= BestDistSq)
			{
				BestDistSq = D;
				BestPickup = P;
			}
		}
		if (BestPickup)
		{
			ServerClaimBombPickup(BestPickup);
			return;
		}
	}
	// Doors: F toggles open/close (one-way doors only from the front face).
	// Range must use the door leaf center — actor origin is the cell min-corner and is often >220uu away.
	{
		APFBuildPieceActor* BestDoor = nullptr;
		float BestDistSq = FMath::Square(APFBuildPieceActor::DoorInteractRangeUU);
		const FVector Me = GetActorLocation();
		for (TActorIterator<APFBuildPieceActor> It(World); It; ++It)
		{
			APFBuildPieceActor* Door = *It;
			if (!Door || !Door->CanUserToggleDoor(this))
			{
				continue;
			}
			const float D = FVector::DistSquared(Me, Door->GetDoorInteractLocation());
			if (D <= BestDistSq)
			{
				BestDistSq = D;
				BestDoor = Door;
			}
		}
		if (BestDoor)
		{
			ServerToggleBuildDoor(BestDoor);
			return;
		}
	}

	// Otherwise: nearest available ammo barrel in interact range.
	APFAmmoBarrel* Best = nullptr;
	float BestDistSq = FMath::Square(220.f);
	const FVector Me = GetActorLocation();
	for (TActorIterator<APFAmmoBarrel> It(World); It; ++It)
	{
		APFAmmoBarrel* Barrel = *It;
		if (!Barrel || !Barrel->IsAvailable())
		{
			continue;
		}
		const float D = FVector::DistSquared(Me, Barrel->GetActorLocation());
		if (D <= BestDistSq)
		{
			BestDistSq = D;
			Best = Barrel;
		}
	}
	if (Best)
	{
		// Route through the pawn's own Server RPC — a Server RPC on the barrel is dropped for
		// remote clients (barrel is owned by the server-only GameMode, no owning connection).
		ServerRefillAtBarrel(Best);
	}
}

void ACombatForgeCharacter::ServerRefillAtBarrel_Implementation(APFAmmoBarrel* Barrel)
{
	if (Barrel)
	{
		Barrel->AuthorityInteract(this);   // barrel re-validates phase/availability/range on the server
	}
}

void ACombatForgeCharacter::ServerToggleBuildDoor_Implementation(APFBuildPieceActor* Door)
{
	if (HealthComponent != nullptr && HealthComponent->bEliminated)
	{
		return;
	}
	if (Door)
	{
		Door->AuthorityTryToggleDoor(this);   // re-validates range + one-way front face
	}
}

void ACombatForgeCharacter::OnInteractReleased()
{
	if (IsLocallyControlled())
	{
		ServerEndDefuse();   // harmless no-op when not defusing
	}
}

void ACombatForgeCharacter::GrantBombCharge()
{
	if (!HasAuthority())
	{
		return;
	}
	bCarryingBomb = true;
	ForceNetUpdate();
}

bool ACombatForgeCharacter::ConsumeBombCharge()
{
	if (!HasAuthority() || !bCarryingBomb)
	{
		return false;
	}
	bCarryingBomb = false;
	ForceNetUpdate();
	return true;
}

void ACombatForgeCharacter::OnPlantPressed()
{
	if (!IsLocallyControlled())
	{
		return;
	}
	// Must have claimed a mid-field charge first (G is a no-op without one).
	if (!bCarryingBomb)
	{
		return;
	}
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	const double Now = World->GetTimeSeconds();
	if (Now - LastPlantTime < 0.5)
	{
		return;   // debounce; the server re-validates everything anyway
	}
	LastPlantTime = Now;

	// Aim at a build piece: same channel + reach as the build ghost trace.
	FVector EyeLoc; FRotator EyeRot;
	GetActorEyesViewPoint(EyeLoc, EyeRot);
	FCollisionQueryParams Params(SCENE_QUERY_STAT(PFPlantBomb), /*bTraceComplex=*/false, this);
	FHitResult Hit;
	if (!World->LineTraceSingleByChannel(Hit, EyeLoc,
		EyeLoc + EyeRot.Vector() * PFGrid::BuildReachUU, PF_ECC_BuildTrace, Params))
	{
		return;
	}
	for (TActorIterator<APFBuildGrid> It(World); It; ++It)
	{
		uint16 PieceId = 0;
		FPFBuildPieceRec Rec;
		if (*It && (*It)->FindPieceByHit(Hit, PieceId, Rec))
		{
			ServerPlantBomb(PieceId);
			return;
		}
	}
}

void ACombatForgeCharacter::ServerPlantBomb_Implementation(uint16 PieceId)
{
	if (HealthComponent != nullptr && HealthComponent->bEliminated)
	{
		return;
	}
	if (!bCarryingBomb)
	{
		return;   // no free plants — mid-field pickup only
	}
	if (ACombatForgeGameMode* GM = GetWorld() ? GetWorld()->GetAuthGameMode<ACombatForgeGameMode>() : nullptr)
	{
		GM->ServerTryPlantBomb(this, PieceId);   // consumes charge on successful plant
	}
}

void ACombatForgeCharacter::ServerClaimBombPickup_Implementation(APFBombPickup* Pickup)
{
	if (HealthComponent != nullptr && HealthComponent->bEliminated)
	{
		return;
	}
	if (Pickup)
	{
		Pickup->AuthorityInteract(this);
	}
}

void ACombatForgeCharacter::ServerBeginDefuse_Implementation()
{
	UWorld* World = GetWorld();
	const ACombatForgePlayerState* MyPS = GetPlayerState<ACombatForgePlayerState>();
	if (!World || MyPS == nullptr || (HealthComponent != nullptr && HealthComponent->bEliminated))
	{
		return;
	}
	// Authoritative re-find: nearest armed bomb in defuse range that ISN'T MINE (anyone but the planter may
	// defuse — works in FFA and gives a griefed team counterplay). The bomb's own Tick keeps re-validating
	// range/alive/identity while the hold accumulates.
	APFBombActor* Best = nullptr;
	float BestDistSq = FMath::Square(APFBombActor::DefuseRangeUU);
	for (TActorIterator<APFBombActor> It(World); It; ++It)
	{
		APFBombActor* Bomb = *It;
		if (!Bomb || !Bomb->IsArmed() || Bomb->GetPlanterPS() == MyPS)
		{
			continue;
		}
		const float D = FVector::DistSquared(GetActorLocation(), Bomb->GetActorLocation());
		if (D <= BestDistSq)
		{
			BestDistSq = D;
			Best = Bomb;
		}
	}
	if (Best)
	{
		DefusingBomb = Best;
		Best->ServerSetDefuser(this, true);
	}
}

void ACombatForgeCharacter::ServerEndDefuse_Implementation()
{
	if (APFBombActor* Bomb = DefusingBomb.Get())
	{
		Bomb->ServerSetDefuser(this, false);
	}
	DefusingBomb.Reset();
}

void ACombatForgeCharacter::OnFireSelectPressed()
{
	if (WeaponComponent != nullptr)
	{
		WeaponComponent->CycleFireMode();
	}
}

void ACombatForgeCharacter::OnThrowFragPressed()
{
	if (WeaponComponent != nullptr)
	{
		WeaponComponent->StartThrow(EPFGrenadeType::Frag);
	}
}

void ACombatForgeCharacter::OnThrowSmokePressed()
{
	if (WeaponComponent != nullptr)
	{
		WeaponComponent->StartThrow(EPFGrenadeType::Smoke);
	}
}

void ACombatForgeCharacter::OnMeleePressed()
{
	if (!IsLocallyControlled())
	{
		return;
	}
	if (HealthComponent != nullptr && HealthComponent->bEliminated)
	{
		return;
	}
	// Local cooldown only — do NOT write LastMeleeTimeServer here. On a listen host,
	// ServerMelee_Implementation runs on this same object; writing the shared timer first
	// made the server always see "still on cooldown" and never apply damage (Tom: punch did nothing).
	const double Now = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0;
	if (Now - LastMeleeTimeClient < MeleeCooldown)
	{
		return;
	}
	LastMeleeTimeClient = Now;
	PlayMeleeSwingLocal();   // instant local feedback; Multicast covers remotes from the server
	ServerMelee();
}

void ACombatForgeCharacter::PlayMeleeSwingLocal()
{
	// Prefer a real unarmed attack clip (MM_Attack_01/02/03) when the pack is present; fall back to a
	// short procedural timer so the cooldown/feedback still works if assets fail to load.
	UAnimSequence* Clip = nullptr;
	{
		int32 Loaded = 0;
		for (const TObjectPtr<UAnimSequence>& A : MeleeAnims)
		{
			if (A != nullptr) { ++Loaded; }
		}
		if (Loaded > 0)
		{
			// Cycle clips so repeated punches aren't identical (local counter is fine for cosmetics).
			static int32 MeleeClipCursor = 0;
			for (int32 n = 0; n < MeleeAnims.Num(); ++n)
			{
				const int32 Idx = (MeleeClipCursor + n) % MeleeAnims.Num();
				if (MeleeAnims[Idx] != nullptr)
				{
					Clip = MeleeAnims[Idx].Get();
					MeleeClipCursor = Idx + 1;
					break;
				}
			}
		}
	}

	const float ClipLen = (Clip != nullptr) ? FMath::Max(0.2f, Clip->GetPlayLength()) : MeleeSwingAnimSec;
	MeleeSwingAnimRemain = ClipLen;

	// Play the TP punch on the body mesh (sequence-loco path). Hide the hand-held gun for the swing so
	// it doesn't float mid-punch on the fist.
	if (Clip != nullptr && GetMesh() != nullptr && bUsingArtBody && !bEliminatedAppearanceActive)
	{
		if (GetMesh()->GetAnimationMode() != EAnimationMode::AnimationSingleNode)
		{
			GetMesh()->SetAnimationMode(EAnimationMode::AnimationSingleNode);
		}
		GetMesh()->PlayAnimation(Clip, /*bLooping=*/false);
		if (UAnimSingleNodeInstance* Node = GetMesh()->GetSingleNodeInstance())
		{
			Node->SetLooping(false);
			Node->SetPlaying(true);
			Node->SetPlayRate(1.f);
			Node->SetPosition(0.f, false);
		}
		SeqLocoState = 0;   // force re-pick of idle/walk when the punch ends
		if (WeaponMeshComp != nullptr)
		{
			WeaponMeshComp->SetHiddenInGame(true);
		}
	}
	else
	{
		// No clip: keep the old raise-gun jab as a last resort visual.
		WeaponRaiseHoldSec = FMath::Max(WeaponRaiseHoldSec, MeleeSwingAnimSec);
	}

	// Small FP viewmodel kick so the owner feels the punch even without FP arms anims.
	RecoilOffset += FVector(6.f, 0.f, -4.f);
	RecoilPitch  += 8.f;
	if (UPFCombatAudio* Audio = GetCombatAudio())
	{
		// Whoosh / contact-ready cue at the pawn (impact plays on hit).
		Audio->PlayImpactAt(GetActorLocation() + FVector(0.f, 0.f, 40.f));
	}
}

void ACombatForgeCharacter::MulticastMeleeSwing_Implementation()
{
	// Owning client already played in OnMeleePressed; remotes + simulated proxies need this.
	if (!IsLocallyControlled())
	{
		PlayMeleeSwingLocal();
	}
}

void ACombatForgeCharacter::ServerMelee_Implementation()
{
	UWorld* World = GetWorld();
	if (World == nullptr)
	{
		return;
	}
	// Authoritative cooldown (small RTT tolerance) + no meleeing while eliminated.
	const double Now = World->GetTimeSeconds();
	if (Now - LastMeleeTimeServer < MeleeCooldown * 0.85)
	{
		return;
	}
	if (HealthComponent != nullptr && HealthComponent->bEliminated)
	{
		return;
	}
	LastMeleeTimeServer = Now;
	MulticastMeleeSwing();   // remotes see the swing even on a miss

	ACombatForgePlayerState* MyPS = GetPlayerState<ACombatForgePlayerState>();
	const uint8 MyTeam = MyPS ? MyPS->TeamId : 255;

	// Short forward reach from the eye line. Prefer pawn channel; also try a multi-sweep over
	// overlapping capsules so a bot standing slightly off-center still gets tagged.
	FVector EyeLoc; FRotator EyeRot;
	GetActorEyesViewPoint(EyeLoc, EyeRot);
	const FVector End = EyeLoc + EyeRot.Vector() * MeleeRange;

	FCollisionQueryParams Params(SCENE_QUERY_STAT(PFMelee), /*bTraceComplex=*/false, this);
	FHitResult Hit;
	ACombatForgeCharacter* Victim = nullptr;
	FVector ImpactPoint = FVector::ZeroVector;
	FVector ImpactNormal = FVector::ForwardVector;

	if (World->SweepSingleByChannel(Hit, EyeLoc, End, FQuat::Identity, ECC_Pawn,
		FCollisionShape::MakeSphere(MeleeRadius), Params))
	{
		Victim = Cast<ACombatForgeCharacter>(Hit.GetActor());
		ImpactPoint = Hit.ImpactPoint;
		ImpactNormal = Hit.ImpactNormal;
	}

	// Fallback: sphere-overlap scan (some pawns ignore ECC_Pawn traces if capsule responses changed).
	// Cheap — only runs when the channel sweep missed a CombatForge pawn.
	if (Victim == nullptr)
	{
		TArray<FOverlapResult> Overlaps;
		FCollisionObjectQueryParams ObjParams;
		ObjParams.AddObjectTypesToQuery(ECC_Pawn);
		const FVector Mid = EyeLoc + EyeRot.Vector() * (MeleeRange * 0.5f);
		if (World->OverlapMultiByObjectType(Overlaps, Mid, FQuat::Identity, ObjParams,
			FCollisionShape::MakeSphere(MeleeRange * 0.55f), Params))
		{
			float BestDistSq = TNumericLimits<float>::Max();
			for (const FOverlapResult& O : Overlaps)
			{
				ACombatForgeCharacter* Cand = Cast<ACombatForgeCharacter>(O.GetActor());
				if (Cand == nullptr || Cand == this)
				{
					continue;
				}
				const float Dsq = FVector::DistSquared(Cand->GetActorLocation(), EyeLoc);
				if (Dsq < BestDistSq && Dsq <= FMath::Square(MeleeRange + MeleeRadius))
				{
					BestDistSq = Dsq;
					Victim = Cand;
					ImpactPoint = Cand->GetActorLocation() + FVector(0.f, 0.f, 40.f);
					ImpactNormal = (EyeLoc - ImpactPoint).GetSafeNormal();
				}
			}
		}
	}
	if (Victim == nullptr || Victim == this)
	{
		return;
	}
	UPFHealthComponent* VictimHealth = Victim->GetHealth();
	ACombatForgePlayerState* VictimPS = Victim->GetPlayerState<ACombatForgePlayerState>();
	if (VictimHealth == nullptr || VictimHealth->bEliminated)
	{
		return;
	}
	if (VictimPS != nullptr && MyTeam <= 1 && VictimPS->TeamId == MyTeam)
	{
		return;   // no friendly-fire tags
	}

	// The tag: instant elimination, credited to the attacker (feeds the kill feed + score via OnEliminatedEvent).
	FPFPaintHitInfo MeleeHit;
	MeleeHit.ShooterPS   = MyPS;
	MeleeHit.ShooterTeam = MyTeam;
	MeleeHit.ImpactPoint = ImpactPoint;
	MeleeHit.ImpactNormal= ImpactNormal;
	MeleeHit.Region      = EPFBodyRegion::Chest;
	MeleeHit.ServerTime  = static_cast<float>(Now);
	VictimHealth->ApplyPaintHit(MeleeHit, /*bForceEliminate=*/true);

	if (UPFCombatAudio* Audio = GetCombatAudio())
	{
		Audio->PlayImpactAt(ImpactPoint);   // contact "thwack" (host-side; elim feedback covers clients)
	}
}

void ACombatForgeCharacter::TickServerAfk(float DeltaSeconds)
{
	// Authority-only; called from Tick under HasAuthority().
	AController* C = GetController();
	APlayerController* PC = Cast<APlayerController>(C);
	// Only police REMOTE humans: bots are AIControllers (Cast<APlayerController> fails), and the listen-server
	// host / a standalone player are local controllers (never kick the person running the server).
	if (PC == nullptr || PC->IsLocalController())
	{
		return;
	}
	// Don't accrue idle time while eliminated / awaiting respawn — reseed the baseline on the next live poll.
	if (HealthComponent != nullptr && HealthComponent->bEliminated)
	{
		ServerLastActiveTime = -1.f;
		return;
	}

	const UWorld* World = GetWorld();
	const float Now = World ? World->GetTimeSeconds() : 0.f;

	// Poll ~1 Hz — cheap, and the 3-minute window doesn't need sub-second resolution.
	ServerAfkPollAccum += DeltaSeconds;
	if (ServerAfkPollAccum < 1.f)
	{
		return;
	}
	ServerAfkPollAccum = 0.f;

	const FVector Loc = GetActorLocation();
	const FRotator Aim = PC->GetControlRotation();   // remote client's view rotation, replicated via ServerMove

	if (ServerLastActiveTime < 0.f)
	{
		// First poll since (re)spawn — seed the baseline and treat as active.
		ServerLastActiveTime = Now;
		ServerAfkLastLoc = Loc;
		ServerAfkLastAim = Aim;
		return;
	}

	const bool bMoved = FVector::DistSquared(Loc, ServerAfkLastLoc) > (8.f * 8.f);
	const float AimDelta = FMath::Abs(FRotator::NormalizeAxis(Aim.Yaw - ServerAfkLastAim.Yaw))
	                     + FMath::Abs(FRotator::NormalizeAxis(Aim.Pitch - ServerAfkLastAim.Pitch));
	if (bMoved || AimDelta > 1.0f)
	{
		ServerLastActiveTime = Now;
		ServerAfkLastLoc = Loc;
		ServerAfkLastAim = Aim;
		return;
	}

	if (Now - ServerLastActiveTime >= AfkKickSeconds)
	{
		UE_LOG(CombatForgeLog, Log, TEXT("AFK kick: %s idle %.0fs — returned to main menu"),
			*GetNameSafe(PC->PlayerState), AfkKickSeconds);
		PC->ClientReturnToMainMenuWithTextReason(
			NSLOCTEXT("CombatForge", "AfkKick", "You were idle for 3 minutes and were returned to the menu."));
		ServerLastActiveTime = Now;   // don't re-fire while the client travel is in flight
	}
}

void ACombatForgeCharacter::SetADS(bool bWantsADS)
{
	// ADS input during a slide queues: bADSHeld stays true and IsADS() flips
	// on its own the moment the slide ends (04 §1.2).
	bADSHeld = bWantsADS;
	UpdateMovementIntents();
}

void ACombatForgeCharacter::NotifyReloadStateChanged()
{
	// Reload begin/end changes what IsADS() returns (it self-suppresses while reloading); push it into
	// the movement stream now so the server drops / restores the ADS spread cone in lockstep.
	UpdateMovementIntents();
}

void ACombatForgeCharacter::RefreshADSToggleMode()
{
	const bool bNewToggle = FPFUserPrefs::GetADSToggle();
	if (bNewToggle != (bool)bADSToggleMode)
	{
		bADSToggleMode = bNewToggle;
		// Switching modes clears any latched ADS so a toggle left "on" can't strand you scoped-in
		// after you flip back to hold (where no button is down to release).
		if (bADSHeld)
		{
			SetADS(false);
		}
	}
}

void ACombatForgeCharacter::RefreshCrouchToggleMode()
{
	const bool bNewToggle = FPFUserPrefs::GetCrouchToggle();
	if (bNewToggle != (bool)bCrouchToggleMode)
	{
		bCrouchToggleMode = bNewToggle;
		// Switching modes clears latched crouch so a toggle left "down" can't strand you crouched
		// after you flip back to hold (where no button is down to release).
		if (PFMovement != nullptr && PFMovement->bWantsToCrouch)
		{
			PFMovement->OnCrouchSlideReleased();
		}
	}
}

bool ACombatForgeCharacter::IsADS() const
{
	if (PFMovement == nullptr)
	{
		return bADSHeld;
	}
	// Reload suppresses ADS for its duration. Deriving this from the weapon's live bReloading flag (instead
	// of latching a copy) keeps bADSHeld as the player's PURE intent — a release mid-reload is honored, and
	// dying mid-reload can't strand you scoped (respawn authoritatively clears bReloading). Owner-only, so it
	// only gates the locally-controlled path; the server sees the same result via the compressed move flag.
	if (IsLocallyControlled() && WeaponComponent != nullptr && WeaponComponent->bReloading)
	{
		return false;
	}
	// Owning client: the raw held key drives the state (and feeds the
	// compressed flag via UpdateMovementIntents). Server / proxies: bADSHeld
	// never leaves the owning client, so read the intent back out of the move
	// stream (FLAG_Custom_1) — this keeps the server's spread cone identical
	// to the one the client predicted with.
	const bool bHeld = IsLocallyControlled() ? (bADSHeld != 0) : PFMovement->WantsToADS();
	return bHeld && !PFMovement->IsSliding();
}

float ACombatForgeCharacter::GetADSAlpha() const
{
	return ADSAlpha;
}

void ACombatForgeCharacter::SetPreferredBaseFOV(float Fov)
{
	BaseFOV = FMath::Clamp(Fov, 80.f, 110.f);
}

void ACombatForgeCharacter::UpdateMovementIntents()
{
	if (PFMovement == nullptr)
	{
		return;
	}
	// Sprint blocks fire and ADS; fire/ADS input cancels sprint immediately and
	// sprint resumes when both are released while shift is still held (04 §1.1).
	// CONTRACT-GAP: 04 doesn't state what a sprint press does mid-burst; we let
	// the held fire/ADS win (sprint stays suppressed until they release).
	const bool bSprintIntent = bSprintKeyHeld && !bADSHeld && !bFireHeld;
	PFMovement->SetWantsToSprint(bSprintIntent);
	PFMovement->SetWantsToADS(IsADS());
}

void ACombatForgeCharacter::HandleSlideStateChanged(bool /*bSliding*/)
{
	// Queued ADS applies on slide end; FOV kick state is polled in Tick.
	if (IsLocallyControlled())
	{
		UpdateMovementIntents();
	}
}

void ACombatForgeCharacter::HandleMantleStateChanged(bool bMantling)
{
	if (GetMesh() == nullptr || bEliminatedAppearanceActive)
	{
		return;
	}
	// AnimBP-driven bodies leave mantle cosmetics alone (no SingleNode override).
	if (GetMesh()->GetAnimationMode() == EAnimationMode::AnimationBlueprint
		&& GetMesh()->GetAnimInstance() != nullptr
		&& !GetMesh()->GetAnimInstance()->IsA<UAnimSingleNodeInstance>())
	{
		return;
	}

	if (bMantling)
	{
		UAnimSequence* Seq = MantleAnim.Get();
		if (Seq == nullptr)
		{
			Seq = MantleAnimFallback.Get();
		}
		if (Seq == nullptr)
		{
			return;
		}
		if (GetMesh()->GetAnimationMode() != EAnimationMode::AnimationSingleNode)
		{
			GetMesh()->SetAnimationMode(EAnimationMode::AnimationSingleNode);
		}
		// Stretch/compress the clip to MantleTime so the climb ends with the interp.
		float PlayRate = 1.f;
		const float ClipLen = Seq->GetPlayLength();
		const float MantleT = (PFMovement != nullptr) ? FMath::Max(0.05f, PFMovement->MantleTime) : 0.35f;
		if (ClipLen > 0.05f)
		{
			PlayRate = ClipLen / MantleT;
		}
		GetMesh()->PlayAnimation(Seq, /*bLooping=*/false);
		if (UAnimSingleNodeInstance* Node = GetMesh()->GetSingleNodeInstance())
		{
			Node->SetLooping(false);
			Node->SetPlaying(true);
			Node->SetPlayRate(PlayRate);
			Node->SetPosition(0.f, false);
		}
		bMantleAnimActive = true;
		SeqLocoState = 0;   // force locomotion re-pick when mantle ends
	}
	else
	{
		bMantleAnimActive = false;
		SeqLocoState = 0;
		// Resume idle/walk/run immediately (don't wait for the next speed bucket change).
		UpdateSequenceLocomotion();
	}
}

// ---------------------------------------------------------------------------
// FOV arbiter (04 §1.3) — the single compose point; effects never fight
// ---------------------------------------------------------------------------

void ACombatForgeCharacter::UpdateADSAlpha(float DeltaSeconds)
{
	// Linear 0.18 s in / 0.14 s out; the ease-out cubic is applied at compose
	// time (FOV) and by the spread lerp consumer. Runs on every role — see Tick.
	const float Target = IsADS() ? 1.f : 0.f;
	const float Rate = (Target > ADSAlpha)
		? (ADSInTime > 0.f ? 1.f / ADSInTime : BIG_NUMBER)
		: (ADSOutTime > 0.f ? 1.f / ADSOutTime : BIG_NUMBER);
	ADSAlpha = FMath::FInterpConstantTo(ADSAlpha, Target, DeltaSeconds, Rate);
}

void ACombatForgeCharacter::UpdateViewmodelFeel(float DeltaSeconds, FVector& OutLoc, FRotator& OutRot)
{
	// Procedural viewmodel motion (owner only): everything a static gun-glued-to-camera lacks. Constants from
	// the CoD-style procedural-viewmodel literature (spring k 200-400, zeta 0.7-0.85, figure-8 bob with the
	// vertical at 2x the lateral frequency, ADS suppressing all of it). ViewModelRoot space: +X fwd, +Y right,
	// +Z up.
	const float Dt = FMath::Clamp(DeltaSeconds, 0.0001f, 0.05f);
	const float RawAds = FMath::Clamp(ADSAlpha, 0.f, 1.f);
	const float AdsSuppress = FMath::Lerp(1.f, 0.3f, RawAds);    // sway/kick at 30% while sighted
	const float BobSuppress = FMath::Lerp(1.f, 0.12f, RawAds);   // bob nearly gone while sighted

	// ---- Look-lag sway: the gun trails the camera and settles with a slight overshoot ----
	{
		float CamYaw = PrevCamYaw, CamPitch = PrevCamPitch;
		if (const AController* C = GetController())
		{
			const FRotator CR = C->GetControlRotation();
			CamYaw = CR.Yaw;
			CamPitch = CR.Pitch;
		}
		if (!bCamRotInit)
		{
			PrevCamYaw = CamYaw;
			PrevCamPitch = CamPitch;
			bCamRotInit = true;
		}
		const float DYaw = FMath::UnwindDegrees(CamYaw - PrevCamYaw);
		const float DPitch = FMath::UnwindDegrees(CamPitch - PrevCamPitch);
		PrevCamYaw = CamYaw;
		PrevCamPitch = CamPitch;

		// Rates from the REAL frame time — dividing the accumulated delta by the clamped Dt turned every
		// 100-200ms hitch into a fake huge look-rate and a one-frame gun lurch.
		const float RateDt = FMath::Max(DeltaSeconds, 0.0001f);
		const float YawRate = DYaw / RateDt;     // deg/s
		const float PitchRate = DPitch / RateDt;
		const float MaxSway = 5.f * AdsSuppress;
		SwayYaw.Update(FMath::Clamp(-YawRate * 0.014f, -MaxSway, MaxSway) * AdsSuppress, Dt, 260.f, 0.8f);
		SwayPitch.Update(FMath::Clamp(-PitchRate * 0.012f, -MaxSway * 0.75f, MaxSway * 0.75f) * AdsSuppress, Dt, 260.f, 0.8f);
		SwayX.Update(FMath::Clamp(-YawRate * 0.0035f, -1.8f, 1.8f) * AdsSuppress, Dt, 220.f, 0.8f);
		SwayZ.Update(FMath::Clamp(-PitchRate * 0.003f, -1.4f, 1.4f) * AdsSuppress, Dt, 220.f, 0.8f);
	}

	// ---- Movement bob (grounded): figure-8, speeds up with pace; breath takes over when still ----
	float BobLat = 0.f, BobVert = 0.f, BobRoll = 0.f, BreathZ = 0.f, BreathPitch = 0.f;
	{
		const float Speed = GetVelocity().Size2D();
		const bool bGrounded = PFMovement != nullptr && !PFMovement->IsFalling();
		const float TargetAlpha = (bGrounded && Speed > 30.f) ? FMath::Min(Speed / 600.f, 1.15f) : 0.f;
		BobAlpha = FMath::FInterpTo(BobAlpha, TargetAlpha, Dt, 7.f);
		if (BobAlpha > 0.02f)
		{
			// ~2.7 Hz at jog (600 uu/s), capped so sprint doesn't blur into a vibration.
			const float Hz = FMath::Min(Speed / 220.f, 3.2f);
			BobPhase += Hz * 2.f * PI * Dt;
			const float A = BobAlpha * BobSuppress;
			BobLat  = 0.9f * A * FMath::Sin(BobPhase);
			BobVert = 0.5f * A * FMath::Sin(2.f * BobPhase);
			BobRoll = 0.9f * A * FMath::Sin(BobPhase);
		}
		else
		{
			BobPhase = 0.f;
		}
		// Idle breathing: slow rise/fall so a still gun is never a screenshot.
		const float BreathAlpha = (1.f - FMath::Min(Speed / 150.f, 1.f)) * FMath::Lerp(1.f, 0.4f, RawAds);
		BreathPhase += 0.42f * 2.f * PI * Dt;
		BreathZ = 0.14f * BreathAlpha * FMath::Sin(BreathPhase);
		BreathPitch = 0.06f * BreathAlpha * FMath::Sin(BreathPhase + 0.6f);
	}

	// ---- Fire-kick + landing springs settle toward zero ----
	KickBack.Update(0.f, Dt, 320.f, 0.72f);
	KickUp.Update(0.f, Dt, 320.f, 0.72f);
	KickPitch.Update(0.f, Dt, 340.f, 0.7f);
	KickYaw.Update(0.f, Dt, 300.f, 0.75f);
	KickRoll.Update(0.f, Dt, 300.f, 0.75f);
	LandDipSpring.Update(0.f, Dt, 180.f, 0.75f);

	// Kick accumulation clamp: full-auto stacks up to ~2x a single shot, then rides there.
	const float KickBackPos = FMath::Clamp(KickBack.Pos, -3.5f, 1.f);
	const float KickPitchPos = FMath::Clamp(KickPitch.Pos, -2.f, 4.5f);

	OutLoc = FVector(
		KickBackPos + SwayX.Pos * 0.f,                     // back/forward: kick only (sway X reads better on Y)
		SwayX.Pos + BobLat,                                // right
		SwayZ.Pos + BobVert + BreathZ + KickUp.Pos + FMath::Clamp(LandDipSpring.Pos, -6.f, 1.5f));  // up
	OutRot = FRotator(
		KickPitchPos + SwayPitch.Pos + BreathPitch + FMath::Clamp(LandDipSpring.Pos, -6.f, 1.5f) * 0.4f,
		SwayYaw.Pos + FMath::Clamp(KickYaw.Pos, -1.5f, 1.5f),
		SwayYaw.Pos * 0.5f + BobRoll + FMath::Clamp(KickRoll.Pos, -2.5f, 2.5f));
}

void ACombatForgeCharacter::UpdateTargetFOV(float DeltaSeconds)
{
	if (FirstPersonCamera == nullptr)
	{
		return;
	}

	// Sprint kick: +6 over 0.15 s.
	{
		const float Target = (PFMovement != nullptr && PFMovement->IsSprintingEffective()) ? 1.f : 0.f;
		const float Rate = SprintKickTime > 0.f ? 1.f / SprintKickTime : BIG_NUMBER;
		SprintKickAlpha = FMath::FInterpConstantTo(SprintKickAlpha, Target, DeltaSeconds, Rate);
	}

	// Slide kick: +9 during the glide phase, lerped out with the friction ramp.
	{
		const float Target = (PFMovement != nullptr && PFMovement->IsSlideGlidePhase()) ? 1.f : 0.f;
		const float Rate = (Target > SlideKickAlpha)
			? 10.f                                                    // snap in with the boost
			: (PFMovement != nullptr && PFMovement->SlideFrictionRampTime > 0.f
				? 1.f / PFMovement->SlideFrictionRampTime : BIG_NUMBER); // out over the ramp (0.5 s)
		SlideKickAlpha = FMath::FInterpConstantTo(SlideKickAlpha, Target, DeltaSeconds, Rate);
	}

	// Ease-out cubic on the ADS transition (04 §1.4).
	const float Eased = 1.f - FMath::Cube(1.f - ADSAlpha);

	float FOV = FMath::Lerp(BaseFOV, ADSFOV, Eased);
	FOV += (1.f - Eased) * (SprintFOVKick * SprintKickAlpha + SlideFOVKick * SlideKickAlpha);

	FirstPersonCamera->SetFieldOfView(FOV);
}

// ---------------------------------------------------------------------------
// Landing: dip shake + jump buffer
// ---------------------------------------------------------------------------

void ACombatForgeCharacter::OnMovementModeChanged(EMovementMode PrevMovementMode, uint8 PreviousCustomMode)
{
	Super::OnMovementModeChanged(PrevMovementMode, PreviousCustomMode);

	if (PFMovement != nullptr && PFMovement->IsFalling())
	{
		FallStartPeakZ = GetActorLocation().Z;
	}
}

void ACombatForgeCharacter::Landed(const FHitResult& Hit)
{
	Super::Landed(Hit);

	// A held-but-whiffed mantle press must not stay latched past the landing (a later walk-off-a-ledge
	// would silently auto-mantle). Runs symmetrically in both sides' sims, like the rest of Landed().
	if (PFMovement != nullptr)
	{
		PFMovement->SetWantsToMantle(false);
	}

	// Landing dip on falls > 300 uu (04 §1.3), local camera only.
	const float FallDistance = FallStartPeakZ - GetActorLocation().Z;
	if (FallDistance > LandingDipMinFallUU && IsLocallyControlled())
	{
		// Viewmodel absorbs the landing too (springs back over ~0.3s) — the camera shake alone left the gun
		// rigidly glued to the view, which reads weightless. Impulse sized so the spring PEAK lands in the
		// visible -1..-5 uu range (the original /55 clamp 5..24 peaked at ~0.5 uu — invisible).
		LandDipSpring.Vel -= FMath::Clamp(FallDistance / 6.f, 45.f, 220.f);
		if (APlayerController* PC = Cast<APlayerController>(GetController()))
		{
			if (PC->PlayerCameraManager != nullptr)
			{
				PC->PlayerCameraManager->StartCameraShake(UPFLandShake::StaticClass());
			}
		}
	}

	// Lethal fall from 3+ build levels (900 uu): authority only → elim + instant respawn path.
	if (HasAuthority() && FallDistance >= FallLethalHeightUU)
	{
		if (UPFHealthComponent* HPComp = GetHealth())
		{
			if (!HPComp->bEliminated)
			{
				// Only during live combat so lobby/build falls are free.
				if (const UWorld* World = GetWorld())
				{
					if (const ACombatForgeGameState* GS = World->GetGameState<ACombatForgeGameState>())
					{
						if (GS->Phase == EPFMatchPhase::Combat && GS->RoundState == EPFRoundState::Live)
						{
							HPComp->ApplyFallDeath();
						}
					}
				}
			}
		}
	}

	FallStartPeakZ = GetActorLocation().Z;

	// 0.1 s pre-landing jump buffer, no coyote time (04 §1.1).
	if (GetWorld() != nullptr
		&& (GetWorld()->GetTimeSeconds() - LastJumpPressedTime) <= JumpBufferTime
		&& !bPressedJump)
	{
		Jump();
		if (!bJumpKeyHeld)
		{
			// Re-armed from a released tap: clear the pressed state right after
			// the jump consumes it so we don't hold a phantom jump.
			GetWorldTimerManager().SetTimer(BufferedJumpClearHandle, this,
				&ACombatForgeCharacter::ClearBufferedJump, 0.15f, false);
		}
	}
}

void ACombatForgeCharacter::ClearBufferedJump()
{
	if (!bJumpKeyHeld)
	{
		StopJumping();
	}
}

// ---------------------------------------------------------------------------
// Team + elimination cosmetics
// ---------------------------------------------------------------------------

void ACombatForgeCharacter::AssembleBanditCharacter()
{
	USkeletalMeshComponent* Base = GetMesh();
	if (Base == nullptr || BanditBodyMesh == nullptr)
	{
		return;
	}

	// Route the Bandit sequences through the existing sequence-loco path (both teams — global test toggle).
	// Selection lives in RefreshBanditAnimSet so the pf.ArmedAnims kill switch applies LIVE (pawns are
	// reused across respawns — waiting for a fresh Assemble meant the switch never actually took effect).
	UAnimSequence* IdleSeq = RefreshBanditAnimSet();

	// Base body carries the skeleton + animation (single-node + UpdateSequenceLocomotion, like Quantum).
	Base->SetSkeletalMeshAsset(BanditBodyMesh);
	Base->Stop();
	Base->SetAnimInstanceClass(nullptr);
	Base->SetAnimationMode(EAnimationMode::AnimationSingleNode);
	CachedWeaponAttachBone = NAME_None;
	SeqLocoState = 0;
	bSequenceLocoActive = true;
	bUsingArtBody = true;
	if (IdleSeq != nullptr)
	{
		Base->PlayAnimation(IdleSeq, /*bLooping=*/true);
		SeqLocoState = 1;
	}

	// Modular parts assembled from the player's saved character (SKM_Bandit_Skeleton -> Leader Pose).
	// Kit-first: the replicated kit (pushed by the owning client at PawnClientRestart) dresses this pawn on
	// EVERY machine. Until a kit arrives: the local human wears its saved config; bots and not-yet-replicated
	// remotes get the default look (OnRep_Kit re-dresses them the moment the kit lands).
	if (HasValidKit())
	{
		ActiveCharConfig.Slots.Reset(KitRep.CharParts.Num());
		for (const int16 Part : KitRep.CharParts) { ActiveCharConfig.Slots.Add(Part); }
	}
	else if (ActiveCharConfig.Slots.Num() == 0)
	{
		const bool bLocalHuman = (GetController() != nullptr) && GetController()->IsLocalPlayerController();
		ActiveCharConfig = bLocalHuman ? PFChar::LoadConfig() : PFChar::DefaultConfig();
	}
	ApplyCharacterConfig();

	// Align: face +X, feet at capsule bottom (handles pelvis-origin packs).
	Base->SetRelativeRotation(FRotator(0.f, -90.f, 0.f));
	if (const UCapsuleComponent* Cap = GetCapsuleComponent())
	{
		const float MeshMinZ = BanditBodyMesh->GetBounds().GetBox().Min.Z;
		const float FeetZ = -Cap->GetUnscaledCapsuleHalfHeight() - MeshMinZ;
		Base->SetRelativeLocation(FVector(0.f, 0.f, FeetZ));
	}
	Base->SetVisibility(true);
	Base->SetHiddenInGame(false);
	Base->SetOwnerNoSee(true);
	Base->SetCastShadow(true);
	if (BodyMesh != nullptr) { BodyMesh->SetVisibility(false); }
	if (HeadMesh != nullptr) { HeadMesh->SetVisibility(false); }

	// Re-seat the rifle on the new (Bandit) skeleton's hand bone — otherwise GetMuzzleLocation falls back to
	// the eye line and BBs spawn from the head. Bandit skeleton is Mannequin-compatible, so hand_r resolves.
	AttachWeaponToHand();

	// Team-colored armbands on BOTH upper arms. Bandit skeleton is Mannequin-compatible, so upperarm_l/_r resolve;
	// if a pack lacks a bone the band simply rides the mesh root (still visible, just not on the arm).
	auto MountArmband = [this, Base](UStaticMeshComponent* Band, TObjectPtr<UMaterialInstanceDynamic>& MID,
		const TCHAR* Bone, float BicepDir)
	{
		if (Band == nullptr)
		{
			return;
		}
		Band->AttachToComponent(Base, FAttachmentTransformRules::KeepRelativeTransform, Bone);
		// The right arm bone mirrors the left, so its +X runs toward the neck — flip the offset per side so the
		// band sits on the bicep on BOTH arms (left = +X down the arm, right = -X down the arm).
		Band->SetRelativeLocation(FVector(16.f * BicepDir, 0.f, 0.f));
		Band->SetRelativeRotation(FRotator(90.f, 0.f, 0.f));     // cylinder axis -> along the arm bone
		Band->SetRelativeScale3D(FVector(0.16f, 0.16f, 0.045f)); // thin band, ~8 cm radius
		if (MID == nullptr && TeamBodyMaterial != nullptr)
		{
			MID = Band->CreateDynamicMaterialInstance(0, TeamBodyMaterial);
		}
		if (MID != nullptr)
		{
			MID->SetVectorParameterValue(TEXT("Color"), PFColors::ForTeam(CachedBodyTeamId));
		}
		Band->SetVisibility(true);
		Band->SetHiddenInGame(false);
		Band->SetOwnerNoSee(true);
	};
	MountArmband(ArmbandMesh, ArmbandMID, TEXT("upperarm_l"), 1.f);
	MountArmband(ArmbandMeshR, ArmbandMIDR, TEXT("upperarm_r"), -1.f);

	// Free-for-All has no teams, so the team-colored armband (a friend/foe tell) is meaningless — hide it in FFA.
	if (const ACombatForgeGameState* GSFFA = GetWorld() ? GetWorld()->GetGameState<ACombatForgeGameState>() : nullptr)
	{
		if (GSFFA->MatchType == EPFMatchType::FreeForAll)
		{
			if (ArmbandMesh != nullptr)  { ArmbandMesh->SetVisibility(false); }
			if (ArmbandMeshR != nullptr) { ArmbandMeshR->SetVisibility(false); }
		}
	}

	bBanditAssembled = true;
	UE_LOG(CombatForgeLog, Log, TEXT("AssembleBanditCharacter: mounted Bandit body + %d slot comps (%d parts in registry)."),
		CharSlotComps.Num(), PFChar::TotalPartCount());
}

void ACombatForgeCharacter::ApplyCharacterConfig()
{
	USkeletalMeshComponent* Base = GetMesh();
	if (Base == nullptr)
	{
		return;
	}
	auto Mount = [Base](USkeletalMeshComponent* C, USkeletalMesh* M)
	{
		if (C == nullptr)
		{
			return;
		}
		if (M != nullptr)
		{
			C->SetSkeletalMeshAsset(M);
			C->SetLeaderPoseComponent(Base);
			C->SetVisibility(true);
			C->SetHiddenInGame(false);
			C->SetOwnerNoSee(true);
			C->SetCastShadow(true);
		}
		else
		{
			C->SetSkeletalMeshAsset(nullptr);
			C->SetVisibility(false);
			C->SetHiddenInGame(true);
		}
	};

	const TArray<FSoftObjectPath>& BaseP = PFChar::BaseParts();
	for (int32 i = 0; i < CharBaseComps.Num(); ++i)
	{
		USkeletalMesh* M = BaseP.IsValidIndex(i) ? Cast<USkeletalMesh>(BaseP[i].TryLoad()) : nullptr;
		Mount(CharBaseComps[i], M);
	}
	for (int32 s = 0; s < CharSlotComps.Num(); ++s)
	{
		const int32 Sel = ActiveCharConfig.Slots.IsValidIndex(s) ? ActiveCharConfig.Slots[s] : -1;
		USkeletalMesh* M = (Sel >= 0) ? PFChar::LoadPart(s, Sel) : nullptr;
		// Belt-and-braces: never mount weapon cosmetics even if an old save slot still points at them.
		if (M != nullptr)
		{
			const FString N = M->GetName();
			if (N.Contains(TEXT("AK_Drops")) || N.Contains(TEXT("AK_Drop"))
				|| N.Contains(TEXT("SM_AK")) || N.Contains(TEXT("SM_Pistol")) || N.Contains(TEXT("SM_Rifle")))
			{
				M = nullptr;
			}
		}
		Mount(CharSlotComps[s], M);
	}

	// ---- Modular skin: stop the one-piece body's legs rendering under the trousers ----
	// SKM_Body (the leader) is a ONE-PIECE naked body (torso+arms+LEGS). Hiding only SKM_Legs left the
	// body's own legs poking through the inner thigh. Visible skin = modular set; leader = skeleton/anim only.
	// Require torso + arms + head so we never hide the leader into an invisible character.
	const bool bModularSkinReady =
		CharBaseComps.IsValidIndex(PFChar::kBaseHead) && CharBaseComps[PFChar::kBaseHead] != nullptr
		&& CharBaseComps[PFChar::kBaseHead]->GetSkeletalMeshAsset() != nullptr
		&& CharBaseComps.IsValidIndex(PFChar::kBaseTorso) && CharBaseComps[PFChar::kBaseTorso] != nullptr
		&& CharBaseComps[PFChar::kBaseTorso]->GetSkeletalMeshAsset() != nullptr
		&& CharBaseComps.IsValidIndex(PFChar::kBaseArms) && CharBaseComps[PFChar::kBaseArms] != nullptr
		&& CharBaseComps[PFChar::kBaseArms]->GetSkeletalMeshAsset() != nullptr;
	if (USkeletalMeshComponent* Leader = GetMesh())
	{
		// A hidden leader would normally stop evaluating its pose (OnlyTickPoseWhenRendered), which would FREEZE
		// every follower. Force it to keep ticking bones while invisible.
		Leader->VisibilityBasedAnimTickOption = bModularSkinReady
			? EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones
			: EVisibilityBasedAnimTickOption::OnlyTickPoseWhenRendered;
		// Both visibility flags — SetVisibility alone was not enough on some paths (shadows / ISM).
		Leader->SetVisibility(!bModularSkinReady, /*bPropagateToChildren=*/false);
		Leader->SetHiddenInGame(bModularSkinReady, /*bPropagateToChildren=*/false);
		if (bModularSkinReady)
		{
			Leader->SetCastShadow(false);   // no naked-body shadow under the clothes
		}
	}

	// Drop bare LEG skin whenever a Pants garment is worn (privates / thighs printing through jeans).
	// Pants = GSlots index 6. If modular legs failed to load we still hide nothing extra on the leader
	// (leader is already fully hidden when modular skin is ready).
	const bool bPantsWorn = ActiveCharConfig.Slots.IsValidIndex(PFChar::kSlotPants)
		&& ActiveCharConfig.Slots[PFChar::kSlotPants] >= 0;
	if (CharBaseComps.IsValidIndex(PFChar::kBaseLegs) && CharBaseComps[PFChar::kBaseLegs] != nullptr)
	{
		const bool bShowLegs = bModularSkinReady && !bPantsWorn
			&& CharBaseComps[PFChar::kBaseLegs]->GetSkeletalMeshAsset() != nullptr;
		CharBaseComps[PFChar::kBaseLegs]->SetVisibility(bShowLegs);
		CharBaseComps[PFChar::kBaseLegs]->SetHiddenInGame(!bShowLegs);
		CharBaseComps[PFChar::kBaseLegs]->SetCastShadow(bShowLegs);
	}
}

void ACombatForgeCharacter::ReapplyCharacterConfig()
{
	// Menu edits on a locally owned pawn: rebuild the kit from prefs and push it, so the change shows up on
	// the server and every other machine too — not just this screen.
	if (GetController() != nullptr && GetController()->IsLocalPlayerController())
	{
		PushLocalKit();
		return;
	}
	if (!bBanditAssembled)
	{
		return;   // only meaningful once the modular character is mounted (pf.BanditChar)
	}
	ActiveCharConfig = PFChar::LoadConfig();
	ApplyCharacterConfig();
}

void ACombatForgeCharacter::ApplyWeaponLoadout()
{
	// BOTS ROLL THEIR OWN GUN (Tom 2026-07-18: "bots always have whatever weapon the player has — they ought to
	// be chosen at random"). Without this a bot has no kit, so it fell through to PFWeapon::LoadConfig() below,
	// which is the HOST'S saved loadout — hence every bot mirroring the player's current weapon.
	// Server-authoritative and written into the REPLICATED kit, so every client sees the same gun on that bot.
	// Rolled exactly ONCE per pawn: populating CharParts makes HasValidKit() true, so this block can't re-roll
	// on later calls (respawn reuses the pawn, and a gun that changed every tick would be worse than the bug).
	if (HasAuthority() && !HasValidKit() && IsBotControlled())
	{
		const FPFCharacterConfig BotLook = PFChar::DefaultConfig();
		KitRep.CharParts.Reset(BotLook.Slots.Num());
		for (const int32 Slot : BotLook.Slots)
		{
			KitRep.CharParts.Add(static_cast<int16>(Slot));
		}
		const int32 Cat = FMath::RandRange(0, PFWeapon::CategoryCount() - 1);
		const int32 Idx = FMath::RandRange(0, FMath::Max(0, PFWeapon::WeaponCount(Cat) - 1));
		KitRep.WeaponCategory = static_cast<uint8>(Cat);
		KitRep.WeaponIndex    = static_cast<uint8>(Idx);
		KitRep.SecondaryCategory = 2;   // pistol sidearm, same as a player's default secondary
		KitRep.SecondaryIndex    = 0;
		UE_LOG(CombatForgeLog, Verbose, TEXT("Bot loadout rolled: %s"),
			*PFWeapon::WeaponDisplayName(Cat, Idx));
	}

	// Resolve primary + secondary from kit (or local prefs / defaults for bots).
	if (HasValidKit())
	{
		PrimaryWeaponConfig.Category   = KitRep.WeaponCategory;
		PrimaryWeaponConfig.Index      = KitRep.WeaponIndex;
		SecondaryWeaponConfig.Category = KitRep.SecondaryCategory;
		SecondaryWeaponConfig.Index    = KitRep.SecondaryIndex;
	}
	else
	{
		PrimaryWeaponConfig   = PFWeapon::LoadConfig();
		SecondaryWeaponConfig = PFWeapon::LoadSecondaryConfig();
	}
	// Clamp in case a save points past a category that shrank.
	PrimaryWeaponConfig.Category   = FMath::Clamp(PrimaryWeaponConfig.Category, 0, PFWeapon::CategoryCount() - 1);
	PrimaryWeaponConfig.Index      = FMath::Clamp(PrimaryWeaponConfig.Index, 0,
		FMath::Max(0, PFWeapon::WeaponCount(PrimaryWeaponConfig.Category) - 1));
	SecondaryWeaponConfig.Category = FMath::Clamp(SecondaryWeaponConfig.Category, 0, PFWeapon::CategoryCount() - 1);
	SecondaryWeaponConfig.Index    = FMath::Clamp(SecondaryWeaponConfig.Index, 0,
		FMath::Max(0, PFWeapon::WeaponCount(SecondaryWeaponConfig.Category) - 1));

	// Hand = active slot; back = the other.
	ActiveWeaponConfig = bSecondaryActive ? SecondaryWeaponConfig : PrimaryWeaponConfig;
	const FPFWeaponConfig StowedConfig = bSecondaryActive ? PrimaryWeaponConfig : SecondaryWeaponConfig;

	const FPFWeaponDef& Def = PFWeapon::Weapon(ActiveWeaponConfig.Category, ActiveWeaponConfig.Index);
	UStaticMesh* WpnMesh = PFWeapon::LoadMesh(Def);
#if !UE_BUILD_SHIPPING
	if (IsLocallyControlled() && GEngine != nullptr)
	{
		const FPFWeaponDef& StowedDef = PFWeapon::Weapon(StowedConfig.Category, StowedConfig.Index);
		GEngine->AddOnScreenDebugMessage(-1, 4.f, FColor::Yellow,
			FString::Printf(TEXT("Hand: %s | Back: %s"), Def.DisplayName, StowedDef.DisplayName));
	}
#endif
	if (WpnMesh == nullptr)
	{
		return;   // asset missing — keep the current weapon
	}
	UMaterialInterface* Mat = PFWeapon::LoadMaterial(Def);
	RifleMaterial = Mat;

	// First-person viewmodel: catalog pose (default), optional auto-bounds, then session drag cache wins.
	FVector  UseFPLoc   = Def.FPLoc;
	FRotator UseFPRot   = Def.FPRot;
	float    UseFPScale = Def.FPScale;
	FVector  UseMuzzle  = Def.MuzzleFP;
	FVector  UseAdsLoc  = Def.AdsLoc;
	FRotator UseAdsRot  = Def.AdsRot;
	if (CVarWeaponAutoPose.GetValueOnGameThread() != 0)
	{
		FPFWeaponAutoPose Auto;
		if (PFWeapon::ComputeAutoPose(WpnMesh, ActiveWeaponConfig.Category, Auto))
		{
			UseFPLoc   = Auto.FPLoc;
			UseFPRot   = Auto.FPRot;
			UseFPScale = Auto.FPScale;
			UseAdsLoc  = Auto.AdsLoc;
			UseAdsRot  = Auto.AdsRot;
#if !UE_BUILD_SHIPPING
			if (IsLocallyControlled())
			{
				UE_LOG(CombatForgeLog, Log,
					TEXT("WeaponAutoPose [%s]: FPLoc=(%.1f,%.1f,%.1f) Rot=(%.1f,%.1f,%.1f) Sc=%.2f | AdsLoc=(%.1f,%.1f,%.1f)"),
					Def.DisplayName, UseFPLoc.X, UseFPLoc.Y, UseFPLoc.Z,
					UseFPRot.Pitch, UseFPRot.Yaw, UseFPRot.Roll, UseFPScale,
					UseAdsLoc.X, UseAdsLoc.Y, UseAdsLoc.Z);
			}
#endif
		}
	}
	// Session drag edits (pose-tune cycle) override catalog/auto so guns don't "change every time".
	if (IsLocallyControlled() && Def.WeaponId)
	{
		TryApplySessionWeaponPose(FName(Def.WeaponId), UseFPLoc, UseFPRot, UseFPScale, UseMuzzle,
			UseAdsLoc, UseAdsRot);
	}

	if (RifleFPMesh != nullptr)
	{
		RifleFPMesh->SetStaticMesh(WpnMesh);
		const int32 Mats = RifleFPMesh->GetNumMaterials();
		for (int32 i = 0; i < Mats; ++i)
		{
			RifleFPMesh->SetMaterial(i, Mat);
		}
		RifleFPMesh->SetRelativeLocation(UseFPLoc);
		RifleFPMesh->SetRelativeRotation(UseFPRot);
		RifleFPMesh->SetRelativeScale3D(FVector(UseFPScale));
	}
	MuzzleLocalFP = UseMuzzle;
	ViewModelAdsLoc = UseAdsLoc;
	ViewModelAdsRot = UseAdsRot;

	if (WeaponComponent != nullptr)
	{
		WeaponComponent->SpreadHip     = Def.SpreadHipDeg;
		WeaponComponent->SpreadADS     = Def.SpreadADSDeg;
		WeaponComponent->SpreadHipMoving = Def.SpreadHipDeg * Def.MoveSpreadMult;
		WeaponComponent->MuzzleSpeedUU = Def.MuzzleSpeedUU;
		WeaponComponent->ProjLifetime  = Def.ProjLifetimeSec;
		WeaponComponent->BurstCount    = Def.ClassBurstCount;
		WeaponComponent->HopperCapacity = Def.MagSize;
		WeaponComponent->HopperCount    = Def.MagSize;
		WeaponComponent->FireRateBps    = Def.FireRateBps;
		WeaponComponent->HitValue       = Def.HitValue;
		WeaponComponent->Pellets        = Def.Pellets;
		WeaponComponent->PelletSpreadDeg = Def.PelletSpreadDeg;
		WeaponComponent->BloomPerShot   = Def.BloomPerShotDeg;
		WeaponComponent->BloomCap       = Def.BloomCapDeg;
		WeaponComponent->BloomFreeShots = Def.BloomFreeShots;
		WeaponComponent->BloomDecayDegPerSec = Def.BloomDecayDegPerSec;
		WeaponComponent->ClimbPitchPerShotDeg = Def.ClimbPitchPerShotDeg;
		WeaponComponent->ClimbYawPerShotDeg   = Def.ClimbYawPerShotDeg;
		WeaponComponent->ClimbRecoverDegPerSec = Def.ClimbRecoverDegPerSec;
		WeaponComponent->SprintOutTime  = Def.SprintOutTime;
		WeaponComponent->ReloadTime     = Def.ReloadTime;
		WeaponComponent->SpinupSec      = Def.SpinupSec;
		WeaponComponent->ReburstDelaySec = Def.ReburstDelaySec;
		WeaponComponent->SetAllowedFireModes(Def.AllowedFireModes, Def.DefaultFireMode);
	}
	// Per-weapon ADS-in (out stays the character's ADSOutTime). Both sides read kit-derived Def.
	ADSInTime = Def.ADSTimeSec;
	// CoD-style WHOLE-SCREEN zoom while ADS, only for weapons that have a scope/sight on the gun.
	// Category 4 = Sniper (every catalog sniper ships with an optic mesh). Explicit Def.ScopedADSFOV
	// wins when set; otherwise snipers get a hard ~5x FOV pull (BaseFOV 105 → ~18). Other categories
	// keep a mild iron-sight zoom. Scope MASK (black ring + reticle) is a separate HUD piece.
	if (Def.ScopedADSFOV > 0.f)
	{
		ADSFOV = Def.ScopedADSFOV;
	}
	else if (ActiveWeaponConfig.Category == 4)
	{
		ADSFOV = 18.f;   // CoD-style sniper magnification (whole-screen zoom)
	}
	else
	{
		ADSFOV = 58.f;   // standard iron-sight magnification
	}
	// Prediction-safe move mult — only from replicated kit + which slot is drawn.
	if (UPFCharacterMovementComponent* CMC = GetPFMovement())
	{
		CMC->CachedWeaponMoveSpeedMult = Def.MoveSpeedMult;
	}

	// TP RAISED (fire/ADS) rotation is per-weapon (Tom 2026-07-18): the default (-90 / 0) is SM_Rifle's
	// +Y-barrel axis; pistols/revolvers render UPSIDE DOWN with it (different mesh barrel axis), so category 2
	// flips 180° about the barrel unless the catalog row explicitly overrides TPRaisedRoll. Best-guess — the
	// pistol mesh axis is unverified in-editor; if a pistol still looks off, adjust TPRaisedYawOffset/Roll.
	CachedTPRaisedYaw  = Def.TPRaisedYawOffset;
	CachedTPRaisedRoll = Def.TPRaisedRoll;
	if (ActiveWeaponConfig.Category == 2 && FMath::IsNearlyZero(Def.TPRaisedRoll))
	{
		CachedTPRaisedRoll = 180.f;
	}

	// TP hand gun + back-slung stowed gun.
	WeaponMesh = WpnMesh;
	AttachWeaponToHand();

	const FPFWeaponDef& StowedDef = PFWeapon::Weapon(StowedConfig.Category, StowedConfig.Index);
	UStaticMesh* StowedMesh = PFWeapon::LoadMesh(StowedDef);
	UMaterialInterface* StowedMat = PFWeapon::LoadMaterial(StowedDef);
	AttachWeaponToBack(StowedMesh, StowedMat);
}

void ACombatForgeCharacter::ReapplyWeaponLoadout()
{
	if (GetController() != nullptr && GetController()->IsLocalPlayerController())
	{
		PushLocalKit();   // menu edit: replicate the new weapon choice, don't just re-read local prefs
		return;
	}
	ApplyWeaponLoadout();
}

void ACombatForgeCharacter::OnWeaponSwapInput()
{
	if (!IsLocallyControlled() || (HealthComponent != nullptr && HealthComponent->bEliminated))
	{
		return;
	}
	const double Now = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0;
	if (Now - LastSwapTime < SwapCooldown)
	{
		return;   // debounce a multi-notch scroll into a single swap
	}
	LastSwapTime = Now;
	ServerSwapWeapon();
}

void ACombatForgeCharacter::ServerSwapWeapon_Implementation()
{
	if (WeaponComponent == nullptr || (HealthComponent != nullptr && HealthComponent->bEliminated))
	{
		return;
	}
	// Stash the outgoing weapon's ammo so a swap never refills.
	const int32 Cur = bSecondaryActive ? 1 : 0;
	StashHopper[Cur]  = WeaponComponent->HopperCount;
	StashReserve[Cur] = WeaponComponent->ReserveAmmo;
	bStashValid[Cur]  = true;

	bSecondaryActive = !bSecondaryActive;   // replicated → OnRep re-equips the gun mesh on every other machine
	ApplyWeaponLoadout();                    // incoming weapon's mesh/pose/stats (+ sets its mag full)

	const int32 Next = bSecondaryActive ? 1 : 0;
	if (bStashValid[Next])
	{
		WeaponComponent->HopperCount = static_cast<uint8>(
			FMath::Clamp<int32>(StashHopper[Next], 0, WeaponComponent->HopperCapacity));
		WeaponComponent->ReserveAmmo = StashReserve[Next];
	}
	WeaponComponent->NotifyAmmoChanged();   // host HUD refresh (owning client refreshes via ammo OnReps)
}

void ACombatForgeCharacter::OnRep_SecondaryActive()
{
	ApplyWeaponLoadout();   // cosmetic re-equip of the correct weapon mesh/pose on remotes + owning client
}

void ACombatForgeCharacter::ResetToPrimaryWeapon()
{
	bStashValid[0] = bStashValid[1] = false;
	if (bSecondaryActive)
	{
		bSecondaryActive = false;   // replicated → remotes re-equip the primary
		ApplyWeaponLoadout();
		if (WeaponComponent != nullptr)
		{
			WeaponComponent->NotifyAmmoChanged();
		}
	}
}

// ---------------------------------------------------------------------------
// Replicated kit — clothing + weapon travel with the pawn, not the machine.
// The class configs live in each player's LOCAL GameUserSettings, so the server can't read them: the owning
// client pushes its active class up (ServerSetKit) and the server replicates it to everyone. Before this,
// LAN clients ran with the HOST's weapon stats and showed default outfits on remote screens.
// ---------------------------------------------------------------------------

void ACombatForgeCharacter::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(ACombatForgeCharacter, KitRep);
	DOREPLIFETIME(ACombatForgeCharacter, bSecondaryActive);   // pistol-secondary: every machine re-equips the gun mesh
	DOREPLIFETIME(ACombatForgeCharacter, bCarryingBomb);      // mid-field pickup charge (not spawn-default)
}

void ACombatForgeCharacter::PawnClientRestart()
{
	Super::PawnClientRestart();
	// Fires on the OWNING machine when possession lands (initial spawn AND every respawn) — the moment this
	// pawn knows whose it is. Push that player's ACTIVE class so everyone dresses it; this is also what makes
	// the dead-time class switch stick: whatever slot the wheel left active is what the fresh pawn pushes.
	PushLocalKit();
}

void ACombatForgeCharacter::PushLocalKit()
{
	if (GetController() == nullptr || !GetController()->IsLocalPlayerController())
	{
		return;   // owning human machines only — bots keep the host-side fallback config
	}
	FPFKitRep Kit;
	const FPFCharacterConfig CharCfg = PFChar::LoadConfig();
	Kit.CharParts.Reserve(CharCfg.Slots.Num());
	for (const int32 Part : CharCfg.Slots) { Kit.CharParts.Add((int16)Part); }
	const FPFWeaponConfig WpnCfg = PFWeapon::LoadConfig();
	Kit.WeaponCategory = (uint8)WpnCfg.Category;
	Kit.WeaponIndex    = (uint8)WpnCfg.Index;
	const FPFWeaponConfig SecCfg = PFWeapon::LoadSecondaryConfig();
	Kit.SecondaryCategory = (uint8)SecCfg.Category;
	Kit.SecondaryIndex    = (uint8)SecCfg.Index;

	KitRep = Kit;   // listen host: this IS the replicated copy; pure client: local preview until the RPC lands
	ApplyKit();

	// Also push the player's ACCOUNT display name so scoreboards/nameplates/after-action show their real name
	// instead of an engine default (which read as the host's name). The name lives only on the owning client
	// (from its login profile) — the server can't read it any other way.
	FString MyName;
	if (UWorld* World = GetWorld())
	{
		if (UGameInstance* GI = World->GetGameInstance())
		{
			if (UPFBackendSubsystem* Backend = GI->GetSubsystem<UPFBackendSubsystem>())
			{
				MyName = Backend->GetProfile().DisplayName;
			}
		}
	}

	if (!HasAuthority())
	{
		ServerSetKit(Kit);
		if (!MyName.IsEmpty()) { ServerSetPlayerName(MyName); }
	}
	else if (!MyName.IsEmpty())
	{
		if (APlayerState* PS = GetPlayerState()) { PS->SetPlayerName(MyName); }   // listen host: set directly
	}
}

void ACombatForgeCharacter::RequestResetToSpawn()
{
	// Owning-client entry (Options menu button). The server performs the authoritative teleport + heal.
	ServerRequestResetToSpawn();
}

void ACombatForgeCharacter::ServerRequestResetToSpawn_Implementation()
{
	if (ACombatForgeGameMode* GM = GetWorld() ? GetWorld()->GetAuthGameMode<ACombatForgeGameMode>() : nullptr)
	{
		GM->RequestResetToSpawn(this);
	}
}

void ACombatForgeCharacter::ServerSetPlayerName_Implementation(const FString& Name)
{
	if (APlayerState* PS = GetPlayerState())
	{
		const FString Clean = Name.TrimStartAndEnd().Left(24);   // backend already validated the name; clamp length
		if (!Clean.IsEmpty())
		{
			PS->SetPlayerName(Clean);
		}
	}
}

void ACombatForgeCharacter::ServerSetKit_Implementation(const FPFKitRep& NewKit)
{
	FPFKitRep Kit = NewKit;
	// Fleet rank gate (weapon-implementation-spec Stage 5): when unlocks are loaded, force locked
	// claims down to the category's rank-1 starter. Listen/LAN / empty unlocks = skip (advisory client-side only).
	if (UWorld* World = GetWorld())
	{
		if (UGameInstance* GI = World->GetGameInstance())
		{
			if (UPFBackendSubsystem* Backend = GI->GetSubsystem<UPFBackendSubsystem>())
			{
				if (Backend->IsFleetActive() && Backend->HasUnlocksLoaded())
				{
					auto ClampSlot = [Backend](uint8& Cat, uint8& Idx)
					{
						const FString Id = PFWeapon::IdOf(Cat, Idx);
						if (!Backend->IsWeaponUnlocked(Id))
						{
							// Category starters: ar_m4 / smg_aksu_black / pis_std / sg_01 / snp_01 / lmg_01
							static const TCHAR* Starters[] = {
								TEXT("ar_m4"), TEXT("smg_aksu_black"), TEXT("pis_std"),
								TEXT("sg_01"), TEXT("snp_01"), TEXT("lmg_01")
							};
							const int32 C = FMath::Clamp<int32>(Cat, 0, UE_ARRAY_COUNT(Starters) - 1);
							const FPFWeaponConfig Safe = PFWeapon::FindById(Starters[C]);
							Cat = static_cast<uint8>(Safe.Category);
							Idx = static_cast<uint8>(Safe.Index);
						}
					};
					ClampSlot(Kit.WeaponCategory, Kit.WeaponIndex);
					ClampSlot(Kit.SecondaryCategory, Kit.SecondaryIndex);
				}
			}
		}
	}
	KitRep = Kit;
	ApplyKit();   // server runs the owner's weapon stats; other clients re-dress via OnRep_Kit
}

void ACombatForgeCharacter::OnRep_Kit()
{
	ApplyKit();
}

void ACombatForgeCharacter::ApplyKit()
{
	if (!HasValidKit())
	{
		return;
	}
	ActiveCharConfig.Slots.Reset(KitRep.CharParts.Num());
	for (const int16 Part : KitRep.CharParts) { ActiveCharConfig.Slots.Add(Part); }
	if (bBanditAssembled)
	{
		ApplyCharacterConfig();   // re-dress the modular character (no-op pre-assembly; Assemble reads the kit)
	}
	ApplyWeaponLoadout();         // kit-first read inside picks up KitRep's weapon on every machine
}

void ACombatForgeCharacter::SetCharSlot(int32 Slot, int32 Index)
{
	if (Slot < 0 || Slot >= PFChar::SlotCount())
	{
		return;
	}
	if (ActiveCharConfig.Slots.Num() != PFChar::SlotCount())
	{
		ActiveCharConfig = PFChar::DefaultConfig();
	}
	ActiveCharConfig.Slots[Slot] = Index;
	ApplyCharacterConfig();
}

// Browse parts without the UI (Phase 2): set a slot's part index on every character in the world.
static void PFCharSlotCmd(const TArray<FString>& Args, UWorld* World)
{
	if (World == nullptr || Args.Num() < 2)
	{
		return;
	}
	const int32 Slot = FCString::Atoi(*Args[0]);
	const int32 Index = FCString::Atoi(*Args[1]);
	int32 Applied = 0;
	for (TActorIterator<ACombatForgeCharacter> It(World); It; ++It)
	{
		It->SetCharSlot(Slot, Index);
		++Applied;
	}
	UE_LOG(CombatForgeLog, Log, TEXT("pf.CharSlot: slot %d ('%s') -> part %d on %d character(s); %d parts in that slot."),
		Slot, *PFChar::SlotLabel(Slot), Index, Applied, PFChar::SlotParts(Slot).Num());
}
static FAutoConsoleCommandWithWorldAndArgs GPFCharSlotCmd(
	TEXT("pf.CharSlot"),
	TEXT("Modular character: <slotIndex> <partIndex> on all pawns. Slots: 0 Head 1 Face 2 Helmet 3 Chest 4 Arms 5 Hips 6 Pants 7 Cloth 8 Backpack (-1 part = none)."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&PFCharSlotCmd));

void ACombatForgeCharacter::TuneWeaponFP(const FVector& Loc, const FRotator& Rot, float Scale, const FVector& Muzzle)
{
	if (RifleFPMesh != nullptr)
	{
		RifleFPMesh->SetRelativeLocation(Loc);
		RifleFPMesh->SetRelativeRotation(Rot);
		RifleFPMesh->SetRelativeScale3D(FVector(Scale));
	}
	MuzzleLocalFP = Muzzle;
}

// Live-tune the equipped weapon's FP pose without a rebuild. Paste the printed values into PFWeaponCatalog.cpp.
static void PFWeaponFPCmd(const TArray<FString>& Args, UWorld* World)
{
	if (World == nullptr || Args.Num() < 7)
	{
		UE_LOG(CombatForgeLog, Log, TEXT("usage: pf.WeaponFP x y z pitch yaw roll scale [muzX muzY muzZ]"));
		return;
	}
	const FVector Loc(FCString::Atof(*Args[0]), FCString::Atof(*Args[1]), FCString::Atof(*Args[2]));
	const FRotator Rot(FCString::Atof(*Args[3]), FCString::Atof(*Args[4]), FCString::Atof(*Args[5]));
	const float Scale = FCString::Atof(*Args[6]);
	const FVector Muzzle = (Args.Num() >= 10)
		? FVector(FCString::Atof(*Args[7]), FCString::Atof(*Args[8]), FCString::Atof(*Args[9]))
		: FVector(42.f, 3.5f, -3.5f);
	for (TActorIterator<ACombatForgeCharacter> It(World); It; ++It)
	{
		if (It->IsLocallyControlled())
		{
			It->TuneWeaponFP(Loc, Rot, Scale, Muzzle);
		}
	}
	UE_LOG(CombatForgeLog, Log,
		TEXT("pf.WeaponFP: FPLoc=FVector(%.2ff,%.2ff,%.2ff), FPRot=FRotator(%.2ff,%.2ff,%.2ff), FPScale=%.3ff, MuzzleFP=FVector(%.2ff,%.2ff,%.2ff)"),
		Loc.X, Loc.Y, Loc.Z, Rot.Pitch, Rot.Yaw, Rot.Roll, Scale, Muzzle.X, Muzzle.Y, Muzzle.Z);
}
static FAutoConsoleCommandWithWorldAndArgs GPFWeaponFPCmd(
	TEXT("pf.WeaponFP"),
	TEXT("Tune the equipped weapon's first-person pose: x y z pitch yaw roll scale [muzX muzY muzZ]. Prints values to paste into PFWeaponCatalog."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&PFWeaponFPCmd));

void ACombatForgeCharacter::TuneWeaponTP(const FVector& Loc, const FRotator& Rot, float Scale)
{
	// Third-person grip: where the gun sits in hand_r. Applied by ApplyHandWeaponPose every tick, so simply
	// writing the members takes effect on the next frame — no re-attach needed.
	WeaponRelativeLocation = Loc;
	WeaponRelativeRotation = Rot;
	WeaponRelativeScale = FVector(Scale);
	ApplyHandWeaponPose();   // immediate feedback while dragging numbers in the console
}

// Live-tune the THIRD-PERSON grip (what everyone else sees). The FP tools above only move the viewmodel; this
// is the one that fixes "the rifle is canted across the chest". Needed after the IK-retarget bake because the
// Bandit's hand bone orientation changed, so the old rifle-tuned offset no longer lands in the grip.
// Applies to EVERY character in the world (including bots) so you can eyeball a bot while tuning.
static void PFWeaponTPCmd(const TArray<FString>& Args, UWorld* World)
{
	if (World == nullptr || Args.Num() < 6)
	{
		UE_LOG(CombatForgeLog, Log, TEXT("usage: pf.WeaponTP x y z pitch yaw roll [scale]   (third-person grip in hand_r)"));
		return;
	}
	const FVector Loc(FCString::Atof(*Args[0]), FCString::Atof(*Args[1]), FCString::Atof(*Args[2]));
	const FRotator Rot(FCString::Atof(*Args[3]), FCString::Atof(*Args[4]), FCString::Atof(*Args[5]));
	const float Scale = (Args.Num() >= 7) ? FCString::Atof(*Args[6]) : 0.85f;
	int32 Applied = 0;
	for (TActorIterator<ACombatForgeCharacter> It(World); It; ++It)
	{
		It->TuneWeaponTP(Loc, Rot, Scale);
		++Applied;
	}
	UE_LOG(CombatForgeLog, Log,
		TEXT("pf.WeaponTP (%d pawns): WeaponRelativeLocation=FVector(%.2ff,%.2ff,%.2ff), WeaponRelativeRotation=FRotator(%.2ff,%.2ff,%.2ff), WeaponRelativeScale=FVector(%.3ff)"),
		Applied, Loc.X, Loc.Y, Loc.Z, Rot.Pitch, Rot.Yaw, Rot.Roll, Scale);
}
static FAutoConsoleCommandWithWorldAndArgs GPFWeaponTPCmd(
	TEXT("pf.WeaponTP"),
	TEXT("Tune the THIRD-PERSON weapon grip (hand_r): x y z pitch yaw roll [scale]. Applies to all pawns incl. bots; prints paste-ready values."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&PFWeaponTPCmd));

void ACombatForgeCharacter::TuneWeaponADS(const FVector& Loc, const FRotator& Rot)
{
	ViewModelAdsLoc = Loc;
	ViewModelAdsRot = Rot;
}

// Live-tune the equipped weapon's aim-down-sight pose. Hold right-click to see it; paste into PFWeaponCatalog.
static void PFWeaponADSCmd(const TArray<FString>& Args, UWorld* World)
{
	if (World == nullptr || Args.Num() < 6)
	{
		UE_LOG(CombatForgeLog, Log, TEXT("usage: pf.WeaponADS x y z pitch yaw roll  (hold right-click to preview)"));
		return;
	}
	const FVector Loc(FCString::Atof(*Args[0]), FCString::Atof(*Args[1]), FCString::Atof(*Args[2]));
	const FRotator Rot(FCString::Atof(*Args[3]), FCString::Atof(*Args[4]), FCString::Atof(*Args[5]));
	for (TActorIterator<ACombatForgeCharacter> It(World); It; ++It)
	{
		if (It->IsLocallyControlled())
		{
			It->TuneWeaponADS(Loc, Rot);
		}
	}
	UE_LOG(CombatForgeLog, Log,
		TEXT("pf.WeaponADS: AdsLoc=FVector(%.2ff,%.2ff,%.2ff), AdsRot=FRotator(%.2ff,%.2ff,%.2ff)"),
		Loc.X, Loc.Y, Loc.Z, Rot.Pitch, Rot.Yaw, Rot.Roll);
}
static FAutoConsoleCommandWithWorldAndArgs GPFWeaponADSCmd(
	TEXT("pf.WeaponADS"),
	TEXT("Tune the equipped weapon's aim-down-sight pose: x y z pitch yaw roll. Hold right-click to preview; prints values for PFWeaponCatalog."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&PFWeaponADSCmd));

// Secondary / back-sling weapon (any catalog gun). Scroll wheel swaps hand ↔ back.
static void PFWeapon2Cmd(const TArray<FString>& Args, UWorld* World)
{
	if (World == nullptr || Args.Num() < 2)
	{
		UE_LOG(CombatForgeLog, Log,
			TEXT("usage: pf.Weapon2 <category> <index>   (0=AR 1=SMG 2=Pistol 3=Shotgun 4=Sniper 5=LMG)"));
		return;
	}
	FPFWeaponConfig C;
	C.Category = FCString::Atoi(*Args[0]);
	C.Index = FCString::Atoi(*Args[1]);
	PFWeapon::SaveSecondaryConfig(C);
	for (TActorIterator<ACombatForgeCharacter> It(World); It; ++It)
	{
		if (It->IsLocallyControlled())
		{
			It->ReapplyWeaponLoadout();
		}
	}
	const FPFWeaponDef& D = PFWeapon::Weapon(C.Category, C.Index);
	UE_LOG(CombatForgeLog, Log, TEXT("pf.Weapon2: secondary = %s / %s (on back when primary is drawn)"),
		*PFWeapon::CategoryLabel(C.Category), D.DisplayName);
}
static FAutoConsoleCommandWithWorldAndArgs GPFWeapon2Cmd(
	TEXT("pf.Weapon2"),
	TEXT("Set secondary (back-sling) weapon: <category> <index>. Scroll swaps with primary."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&PFWeapon2Cmd));

// Cycle / pick guns for hip+ADS drag tuning without leaving the match.
static void PFWeaponCmd(const TArray<FString>& Args, UWorld* World)
{
	if (World == nullptr)
	{
		return;
	}
	ACombatForgeCharacter* Local = nullptr;
	for (TActorIterator<ACombatForgeCharacter> It(World); It; ++It)
	{
		if (It->IsLocallyControlled() && It->IsPlayerControlled())
		{
			Local = *It;
			break;
		}
	}
	if (Local == nullptr)
	{
		UE_LOG(CombatForgeLog, Warning, TEXT("pf.Weapon: no local player pawn"));
		return;
	}

	if (Args.Num() == 0)
	{
		UE_LOG(CombatForgeLog, Log, TEXT(
			"pf.Weapon usage:\n"
			"  pf.Weapon next | prev | n | p     cycle every gun in the catalog\n"
			"  pf.Weapon <cat> <idx>             equip by category/index (0=AR..5=LMG)\n"
			"  pf.Weapon <weaponId>              equip by slug (e.g. ar_ak_black)\n"
			"  pf.Weapon dump                    log hip+ADS pose for the held gun\n"
			"  pf.Weapon list                    print the full catalog\n"
			"Workflow: pf.Weapon next → hold MMB drag hip → hold ADS+MMB drag sights → release logs paste lines."));
		Local->PrintWeaponPoseLine();
		return;
	}

	const FString A0 = Args[0].ToLower();
	if (A0 == TEXT("next") || A0 == TEXT("n") || A0 == TEXT("+"))
	{
		Local->DevCycleCatalogWeapon(+1);
		return;
	}
	if (A0 == TEXT("prev") || A0 == TEXT("p") || A0 == TEXT("-"))
	{
		Local->DevCycleCatalogWeapon(-1);
		return;
	}
	if (A0 == TEXT("dump") || A0 == TEXT("pose") || A0 == TEXT("print"))
	{
		Local->PrintWeaponPoseLine();
		return;
	}
	if (A0 == TEXT("list"))
	{
		int32 Flat = 0;
		for (int32 Cat = 0; Cat < PFWeapon::CategoryCount(); ++Cat)
		{
			for (int32 Idx = 0; Idx < PFWeapon::WeaponCount(Cat); ++Idx)
			{
				const FPFWeaponDef& D = PFWeapon::Weapon(Cat, Idx);
				UE_LOG(CombatForgeLog, Log, TEXT("  [%2d] cat=%d idx=%d  %-16s  %s"),
					Flat++, Cat, Idx, D.WeaponId ? D.WeaponId : TEXT("?"), D.DisplayName);
			}
		}
		return;
	}

	// pf.Weapon <cat> <idx>
	if (Args.Num() >= 2 && A0.IsNumeric())
	{
		Local->DevEquipCatalogWeapon(FCString::Atoi(*Args[0]), FCString::Atoi(*Args[1]));
		return;
	}

	// pf.Weapon <weaponId>
	const FPFWeaponConfig Found = PFWeapon::FindById(Args[0]);
	if (!PFWeapon::IdOf(Found.Category, Found.Index).IsEmpty()
		&& PFWeapon::IdOf(Found.Category, Found.Index).Equals(Args[0], ESearchCase::IgnoreCase))
	{
		Local->DevEquipCatalogWeapon(Found.Category, Found.Index);
		return;
	}

	UE_LOG(CombatForgeLog, Warning, TEXT("pf.Weapon: unknown arg '%s' — try next / prev / list / dump / <id>"), *Args[0]);
}
static FAutoConsoleCommandWithWorldAndArgs GPFWeaponCmd(
	TEXT("pf.Weapon"),
	TEXT("Pose-tune helper: next|prev|list|dump|<cat> <idx>|<weaponId>. Enables drag, disables auto-pose."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&PFWeaponCmd));

// Convenience aliases so muscle memory can hammer one word.
static void PFWeaponNextCmd(UWorld* World)
{
	PFWeaponCmd(TArray<FString>{ TEXT("next") }, World);
}
static void PFWeaponPrevCmd(UWorld* World)
{
	PFWeaponCmd(TArray<FString>{ TEXT("prev") }, World);
}
static FAutoConsoleCommandWithWorld GPFWeaponNextCmd(
	TEXT("pf.WeaponNext"),
	TEXT("Equip the next catalog gun (pose-tune workflow). Same as `pf.Weapon next`."),
	FConsoleCommandWithWorldDelegate::CreateStatic(&PFWeaponNextCmd));
static FAutoConsoleCommandWithWorld GPFWeaponPrevCmd(
	TEXT("pf.WeaponPrev"),
	TEXT("Equip the previous catalog gun (pose-tune workflow). Same as `pf.Weapon prev`."),
	FConsoleCommandWithWorldDelegate::CreateStatic(&PFWeaponPrevCmd));

void ACombatForgeCharacter::ApplyTeamBody(uint8 Team)
{
	// Mounts the per-team skeletal body. Gated so the frequent SetTeamColor calls (ready/budget/roster
	// flag replications all route here) don't re-mount the mesh every time.
	if (GetMesh() == nullptr)
	{
		return;
	}

	// The modular Bandit character is the body for every player (replaces the old Quantum/Survival team bodies).
	// Assemble once; the frequent SetTeamColor re-calls just refresh the cached team. Falls back to the legacy
	// per-team body below only if the Bandit pack is absent.
	if (!bBanditAssembled && BanditBodyMesh != nullptr)
	{
		AssembleBanditCharacter();
	}
	if (bBanditAssembled)
	{
		CachedBodyTeamId = Team;
		bUsingArtBody = true;
		return;
	}

	USkeletalMesh* Chosen = (Team == 1)
		? (Team1BodyMesh ? Team1BodyMesh : ThirdPersonBodyMesh)
		: (Team0BodyMesh ? Team0BodyMesh : ThirdPersonBodyMesh);
	if (Chosen == nullptr)
	{
		return;   // graybox fallback: keep the cubes (unconfigured character stays the validated graybox)
	}
	if (bUsingArtBody && CachedBodyTeamId == Team)
	{
		return;   // already the right body
	}

	GetMesh()->SetSkeletalMeshAsset(Chosen);

	// Anim: prefer per-team AnimBP (skeleton-guarded). Quantum has no matching ABP — use sequence
	// locomotion (idle/walk/run) instead of forcing mannequin ABP (playtest: broken pose).
	UAnimSequence* IdleAnim = (Team == 1) ? Team1IdleAnim.Get() : Team0IdleAnim.Get();
	TSubclassOf<UAnimInstance> AnimClass = (Team == 1) ? Team1AnimClass : Team0AnimClass;
	const bool bLooksLikeMannequin = Chosen->GetName().Contains(TEXT("Manny"))
		|| Chosen->GetName().Contains(TEXT("Quinn"))
		|| Chosen->GetName().Contains(TEXT("Mannequin"));
	if (AnimClass == nullptr && bLooksLikeMannequin)
	{
		AnimClass = ThirdPersonAnimClass;
	}

	// Safety: only drive with an AnimBP whose target skeleton matches this mesh.
	if (AnimClass != nullptr)
	{
		const UAnimBlueprintGeneratedClass* GenClass = Cast<UAnimBlueprintGeneratedClass>(AnimClass.Get());
		const USkeleton* AnimSkel = GenClass ? GenClass->GetTargetSkeleton() : nullptr;
		if (AnimSkel != nullptr && AnimSkel != Chosen->GetSkeleton())
		{
			UE_LOG(CombatForgeLog, Log,
				TEXT("ApplyTeamBody: AnimBP skeleton mismatch for %s — using sequence locomotion."),
				*Chosen->GetName());
			AnimClass = nullptr;
		}
	}

	GetMesh()->Stop();
	SeqLocoState = 0;
	bSequenceLocoActive = false;
	CachedWeaponAttachBone = NAME_None;
	if (AnimClass != nullptr)
	{
		GetMesh()->SetAnimationMode(EAnimationMode::AnimationBlueprint);
		GetMesh()->SetAnimInstanceClass(AnimClass);
	}
	else
	{
		// Sequence path (Quantum): SingleNode instance + UpdateSequenceLocomotion each tick.
		GetMesh()->SetAnimInstanceClass(nullptr);
		GetMesh()->SetAnimationMode(EAnimationMode::AnimationSingleNode);
		bSequenceLocoActive = true;
		if (IdleAnim != nullptr)
		{
			GetMesh()->PlayAnimation(IdleAnim, /*bLooping=*/true);
			SeqLocoState = 1;
			UE_LOG(CombatForgeLog, Log, TEXT("ApplyTeamBody: Quantum sequence loco ready (idle=%s)"),
				*IdleAnim->GetName());
		}
		else
		{
			UE_LOG(CombatForgeLog, Warning,
				TEXT("ApplyTeamBody: sequence loco active but no idle for %s"), *Chosen->GetName());
		}
	}

	// Align: face +X (standard ACharacter -90 yaw). Feet at capsule bottom:
	// relative Z = -halfHeight - meshMinZ (handles pelvis-origin human packs).
	GetMesh()->SetRelativeRotation(FRotator(0.f, -90.f, 0.f));
	if (const UCapsuleComponent* Cap = GetCapsuleComponent())
	{
		const float MeshMinZ = Chosen->GetBounds().GetBox().Min.Z;
		const float FeetZ = -Cap->GetUnscaledCapsuleHalfHeight() - MeshMinZ;
		GetMesh()->SetRelativeLocation(FVector(0.f, 0.f, FeetZ));
	}
	GetMesh()->SetVisibility(true);
	GetMesh()->SetHiddenInGame(false);
	GetMesh()->SetOwnerNoSee(true);           // first-person: owner doesn't see own TP body
	GetMesh()->SetCastShadow(true);
	GetMesh()->bCastDynamicShadow = true;
	if (BodyMesh != nullptr) { BodyMesh->SetVisibility(false); }
	if (HeadMesh != nullptr) { HeadMesh->SetVisibility(false); }

	CachedBodyTeamId = Team;
	bUsingArtBody = true;

	// Team body swap rebuilds the skeletal mesh — re-seat the rifle pose.
	AttachWeaponToHand();
}

void ACombatForgeCharacter::AttachWeaponToHand()
{
	if (WeaponMeshComp == nullptr || WeaponMesh == nullptr)
	{
		return;
	}

	WeaponMeshComp->SetStaticMesh(WeaponMesh);
	WeaponMeshComp->SetVisibility(true);
	WeaponMeshComp->SetHiddenInGame(false);
	WeaponMeshComp->SetOwnerNoSee(true);   // owner uses the FP viewmodel; remotes see the TP gun
	WeaponMeshComp->SetCastShadow(true);
	{
		const int32 Mats = WeaponMeshComp->GetNumMaterials();
		for (int32 i = 0; i < Mats; ++i)
		{
			// Force our material (SM_Rifle: no missing Lyra MI refs) OR clear any stale override so a
			// preserve-authored weapon (AK/pistol) shows its own materials after a swap.
			WeaponMeshComp->SetMaterial(i, RifleMaterial);
		}
	}

	ApplyHandWeaponPose();
}

void ACombatForgeCharacter::AttachWeaponToBack(UStaticMesh* StowedMesh, UMaterialInterface* StowedMat)
{
	if (BackWeaponMeshComp == nullptr)
	{
		return;
	}
	if (StowedMesh == nullptr || !bUsingArtBody || GetMesh() == nullptr)
	{
		BackWeaponMeshComp->SetStaticMesh(nullptr);
		BackWeaponMeshComp->SetVisibility(false);
		BackWeaponMeshComp->SetHiddenInGame(true);
		return;
	}

	BackWeaponMeshComp->SetStaticMesh(StowedMesh);
	const int32 Mats = BackWeaponMeshComp->GetNumMaterials();
	for (int32 i = 0; i < Mats; ++i)
	{
		BackWeaponMeshComp->SetMaterial(i, StowedMat);
	}

	USkeletalMeshComponent* Body = GetMesh();
	// Mid/lower spine for a backpack sling — NEVER neck/head, and never ik_hand_gun (chest/hip holster).
	static const FName BackBones[] = {
		TEXT("spine_02"), TEXT("Spine_02"), TEXT("spine_01"), TEXT("Spine_01"),
		TEXT("spine_03"), TEXT("Spine_03"), TEXT("backpack"), TEXT("Backpack"),
	};
	FName Bone = BackWeaponAttachBone;
	auto Exists = [Body](FName N) -> bool
	{
		return Body->DoesSocketExist(N) || Body->GetBoneIndex(N) != INDEX_NONE;
	};
	if (Bone.IsNone() || !Exists(Bone))
	{
		Bone = NAME_None;
		for (const FName& N : BackBones)
		{
			if (Exists(N))
			{
				Bone = N;
				break;
			}
		}
		BackWeaponAttachBone = Bone;
	}

	if (!Bone.IsNone())
	{
		BackWeaponMeshComp->AttachToComponent(Body,
			FAttachmentTransformRules::SnapToTargetNotIncludingScale, Bone);
	}
	else
	{
		// Mesh-root fallback: upper back-ish relative to pelvis.
		BackWeaponMeshComp->AttachToComponent(Body,
			FAttachmentTransformRules::SnapToTargetNotIncludingScale);
	}
	BackWeaponMeshComp->SetRelativeLocation(BackWeaponRelativeLocation);
	BackWeaponMeshComp->SetRelativeRotation(BackWeaponRelativeRotation);
	BackWeaponMeshComp->SetRelativeScale3D(BackWeaponRelativeScale);
	BackWeaponMeshComp->SetOwnerNoSee(true);
	BackWeaponMeshComp->SetCastShadow(true);
	BackWeaponMeshComp->SetVisibility(true);
	BackWeaponMeshComp->SetHiddenInGame(false);
}

FName ACombatForgeCharacter::ResolveWeaponAttachBone(const USkeletalMeshComponent* Body) const
{
	if (Body == nullptr)
	{
		return NAME_None;
	}
	// ik_hand_gun FIRST when the rifle-hold animation set is driving the arms (Tom 2026-07-18: "just use whatever
	// the animation says is correct — don't hand-tune it"). On mannequin-family skeletons ik_hand_gun IS the
	// animator's weapon marker: the rifle pack animates it to sit exactly in the grip, and the IK-retarget carried
	// it onto SKM_Bandit_Skeleton. Attaching there needs NO offset — position/orientation come from the animation
	// itself and track it for free, which is why this beats a hand-tuned hand_r offset that only matches one pose.
	// Only preferred while the armed set is active; with the unarmed set the arms hang and ik_hand_gun isn't
	// meaningfully posed, so hand_r + the tuned offset remains correct there.
	const bool bArmedIdleActive = CVarArmedAnims.GetValueOnGameThread() != 0 && ArmedIdleAnim != nullptr;
	static const FName WeaponBoneFirst[] = { TEXT("ik_hand_gun"), TEXT("IK_hand_gun") };
	// Do NOT prefer "weapon_r" / holster sockets first — many packs put weapon_r on the hip; attaching there left
	// a fixed hip gun while the FP viewmodel (or armed-hand pose) still looked "in hands" (double-gun bug).
	static const FName HandFirst[] = {
		TEXT("hand_r"),
		TEXT("Hand_R"),
		TEXT("hand_rSocket"),
		TEXT("RightHand"),
		TEXT("ik_hand_gun"),
		TEXT("ik_hand_r"),
		TEXT("HandR"),
		TEXT("LowerArm_R"),
	};
	static const FName HolsterLast[] = {
		TEXT("weapon_r"),
		TEXT("WeaponPoint"),
		TEXT("weapon_l"),
	};
	auto Exists = [Body](FName N) -> bool
	{
		return Body->DoesSocketExist(N) || Body->GetBoneIndex(N) != INDEX_NONE;
	};
	// Animation-authored weapon bone wins while the rifle-hold set is playing (see note above).
	if (bArmedIdleActive && CVarWeaponBoneAttach.GetValueOnGameThread() != 0)
	{
		for (const FName& N : WeaponBoneFirst)
		{
			if (Exists(N))
			{
				return N;
			}
		}
	}
	// Explicit default only if it's a hand (not a holster name).
	if (!WeaponAttachSocket.IsNone() && Exists(WeaponAttachSocket))
	{
		const FString Want = WeaponAttachSocket.ToString().ToLower();
		const bool bHolsterName = Want.Contains(TEXT("weapon")) && !Want.Contains(TEXT("hand"));
		if (!bHolsterName)
		{
			return WeaponAttachSocket;
		}
	}
	for (const FName& N : HandFirst)
	{
		if (Exists(N))
		{
			return N;
		}
	}
	// Scan for any bone with "hand" + "r" in the name (Quantum / Mixamo variants).
	const int32 NumBones = Body->GetNumBones();
	for (int32 i = 0; i < NumBones; ++i)
	{
		const FString Bone = Body->GetBoneName(i).ToString();
		const FString Lower = Bone.ToLower();
		// Never attach to head / neck / spine — looks like "gun on head".
		if (Lower.Contains(TEXT("head")) || Lower.Contains(TEXT("neck"))
			|| Lower.Contains(TEXT("spine")) || Lower.Contains(TEXT("clavicle")))
		{
			continue;
		}
		if (Lower.Contains(TEXT("hand")) && (Lower.Contains(TEXT("_r")) || Lower.EndsWith(TEXT("r"))
				|| Lower.Contains(TEXT("right"))))
		{
			// Skip left hand.
			if (Lower.Contains(TEXT("_l")) || Lower.Contains(TEXT("left")))
			{
				continue;
			}
			return Body->GetBoneName(i);
		}
	}
	// Holster sockets only if no hand bone exists at all.
	for (const FName& N : HolsterLast)
	{
		if (Exists(N))
		{
			return N;
		}
	}
	return NAME_None;
}

void ACombatForgeCharacter::ApplyHandWeaponPose()
{
	if (WeaponMeshComp == nullptr || WeaponMesh == nullptr)
	{
		return;
	}

	USkeletalMeshComponent* Body = GetMesh();
	if (!bUsingArtBody || Body == nullptr)
	{
		return;
	}

	if (CachedWeaponAttachBone.IsNone())
	{
		CachedWeaponAttachBone = ResolveWeaponAttachBone(Body);
		if (!CachedWeaponAttachBone.IsNone())
		{
			UE_LOG(CombatForgeLog, Log, TEXT("Weapon attach bone: %s on %s"),
				*CachedWeaponAttachBone.ToString(),
				Body->GetSkeletalMeshAsset() ? *Body->GetSkeletalMeshAsset()->GetName() : TEXT("?"));
		}
		else
		{
			static bool bLogged = false;
			if (!bLogged)
			{
				bLogged = true;
				UE_LOG(CombatForgeLog, Warning,
					TEXT("ApplyHandWeaponPose: no hand bone on %s — hip-carry fallback. Bone dump:"),
					Body->GetSkeletalMeshAsset() ? *Body->GetSkeletalMeshAsset()->GetName() : TEXT("?"));
				const int32 NumBones = Body->GetNumBones();
				for (int32 i = 0; i < NumBones && i < 60; ++i)
				{
					UE_LOG(CombatForgeLog, Warning, TEXT("  bone[%d]=%s"), i, *Body->GetBoneName(i).ToString());
				}
			}
		}
	}

	if (!CachedWeaponAttachBone.IsNone())
	{
		if (WeaponMeshComp->GetAttachParent() != Body
			|| WeaponMeshComp->GetAttachSocketName() != CachedWeaponAttachBone)
		{
			WeaponMeshComp->AttachToComponent(Body,
				FAttachmentTransformRules::SnapToTargetNotIncludingScale, CachedWeaponAttachBone);
		}
		// On the ANIMATION's weapon bone (ik_hand_gun) the grip transform is already baked into the bone by the
		// animator — applying the hand_r-tuned offset on top would re-introduce exactly the cant we're removing.
		// So: identity offset there, tuned offset on a plain hand bone. Scale is a mesh-size choice either way.
		const bool bOnWeaponBone = CachedWeaponAttachBone.ToString().StartsWith(TEXT("ik_hand_gun"), ESearchCase::IgnoreCase);
		WeaponMeshComp->SetRelativeLocation(bOnWeaponBone ? FVector::ZeroVector : WeaponRelativeLocation);
		WeaponMeshComp->SetRelativeRotation(bOnWeaponBone ? FRotator::ZeroRotator : WeaponRelativeRotation);
		WeaponMeshComp->SetRelativeScale3D(WeaponRelativeScale);

		// ---- Orient the gun from the ANIMATED HANDS (the fix for "gun at the hip / bicep") ----
		// Every fixed offset we tried was wrong for at least one pose, and ik_hand_gun turned out not to be
		// animated on this skeleton (it sits at ik_hand_root near the pelvis — the hip gun). But the rifle-hold
		// animation DOES pose both hands correctly, so the weapon's own line is simply hand_r -> hand_l:
		// right hand on the grip, left on the foregrip. Deriving the barrel from that needs no tuned numbers and
		// stays correct for every frame of every clip. Two-handed only; a one-handed pose keeps the fixed offset.
		if (CVarWeaponAimFromHands.GetValueOnGameThread() != 0 && WeaponMesh != nullptr)
		{
			static const FName LeftHandNames[] = { TEXT("hand_l"), TEXT("Hand_L"), TEXT("LeftHand"), TEXT("HandL") };
			FName LeftHand = NAME_None;
			for (const FName& N : LeftHandNames)
			{
				if (Body->DoesSocketExist(N) || Body->GetBoneIndex(N) != INDEX_NONE) { LeftHand = N; break; }
			}
			if (!LeftHand.IsNone())
			{
				const FVector GripLoc  = Body->GetSocketLocation(CachedWeaponAttachBone);
				const FVector FrontLoc = Body->GetSocketLocation(LeftHand);
				const FVector Barrel   = (FrontLoc - GripLoc).GetSafeNormal();
				// Guard: if the hands are together (holstered/unarmed poses) the direction is meaningless.
				if (!Barrel.IsNearlyZero() && FVector::Dist(FrontLoc, GripLoc) > 10.f)
				{
					// Which local axis is the barrel? Same bounds test the FP auto-pose uses: the longest
					// horizontal extent of the mesh IS the barrel (SM_Rifle family is +Y; others are +X).
					const FBoxSphereBounds B = WeaponMesh->GetBounds();
					const bool bBarrelAlongY = (B.BoxExtent.Y >= B.BoxExtent.X);
					// Roll reference: the hand's up keeps the gun from spinning about its own barrel.
					const FVector HandUp = Body->GetSocketQuaternion(CachedWeaponAttachBone).GetUpVector();
					const FRotator WorldRot = bBarrelAlongY
						? FRotationMatrix::MakeFromYZ(Barrel, HandUp).Rotator()
						: FRotationMatrix::MakeFromXZ(Barrel, HandUp).Rotator();
					WeaponMeshComp->SetWorldRotation(WorldRot);
					// Seat the GRIP (rear of the gun) in the right hand rather than the mesh centre, so the
					// receiver doesn't float forward of the fist.
					const float BarrelHalf = (bBarrelAlongY ? B.BoxExtent.Y : B.BoxExtent.X) * WeaponRelativeScale.X;
					WeaponMeshComp->SetWorldLocation(GripLoc + Barrel * (BarrelHalf * 0.35f));
				}
			}
		}
		return;
	}

	// No hand bone: hip-carry in mesh space (never origin = shoulder/chest).
	WeaponMeshComp->AttachToComponent(Body,
		FAttachmentTransformRules::SnapToTargetNotIncludingScale);
	WeaponMeshComp->SetRelativeLocation(WeaponMeshFallbackLocation);
	WeaponMeshComp->SetRelativeRotation(WeaponMeshFallbackRotation);
	WeaponMeshComp->SetRelativeScale3D(WeaponRelativeScale);
}

UAnimSequence* ACombatForgeCharacter::RefreshBanditAnimSet()
{
	// Prefer the RIFLE-HOLD set (soldiers should look like they're holding the gun, not walking
	// empty-handed); pf.ArmedAnims 0 reverts to the unarmed A_MM_* set if cross-skeleton playback
	// misbehaves. Called from Assemble AND from the cvar sink (live toggle).
	UAnimSequence* IdleSeq = BanditIdleAnim;
	UAnimSequence* WalkSeq = BanditWalkAnim;
	UAnimSequence* RunSeq  = BanditRunAnim;
	if (CVarArmedAnims.GetValueOnGameThread() != 0
		&& ArmedIdleAnim != nullptr && ArmedWalkAnim != nullptr && ArmedRunAnim != nullptr)
	{
		IdleSeq = ArmedIdleAnim;
		WalkSeq = ArmedWalkAnim;
		RunSeq  = ArmedRunAnim;
	}
	Team0IdleAnim = IdleSeq; Team0WalkAnim = WalkSeq; Team0RunAnim = RunSeq;
	Team1IdleAnim = IdleSeq; Team1WalkAnim = WalkSeq; Team1RunAnim = RunSeq;

	// Live re-arm on an already-assembled pawn (skip corpses — the death fall owns the mesh).
	if (bBanditAssembled && bSequenceLocoActive && !bEliminatedAppearanceActive
		&& GetMesh() != nullptr && IdleSeq != nullptr)
	{
		GetMesh()->PlayAnimation(IdleSeq, /*bLooping=*/true);
		SeqLocoState = 1;   // next UpdateSequenceLocomotion re-picks walk/jog from the new set
	}
	return IdleSeq;
}

void ACombatForgeCharacter::UpdateSequenceLocomotion()
{
	if (!bSequenceLocoActive || !bUsingArtBody || GetMesh() == nullptr)
	{
		return;
	}
	// Eliminated: the directional death fall owns the mesh until DeathHideTimer hides it. SeqLocoState=0
	// (stamped at death AND on revive) re-arms the right locomotion anim at respawn.
	if (bEliminatedAppearanceActive)
	{
		return;
	}
	// Mantle owns the mesh for the climb (MM_WallJump) — don't overwrite with walk/jog mid-pull-up.
	if (bMantleAnimActive || (PFMovement != nullptr && PFMovement->IsMantling()))
	{
		return;
	}
	// Melee punch clip owns the mesh for its duration (MM_Attack_*) — don't stomp with idle/walk.
	if (MeleeSwingAnimRemain > 0.f)
	{
		return;
	}

	// Stay on SingleNode — if a full AnimBP was installed, leave it alone.
	if (GetMesh()->GetAnimationMode() == EAnimationMode::AnimationBlueprint
		&& GetMesh()->GetAnimInstance() != nullptr
		&& !GetMesh()->GetAnimInstance()->IsA<UAnimSingleNodeInstance>())
	{
		bSequenceLocoActive = false;
		return;
	}

	if (GetMesh()->GetAnimationMode() != EAnimationMode::AnimationSingleNode)
	{
		GetMesh()->SetAnimationMode(EAnimationMode::AnimationSingleNode);
	}

	const bool bTeam1 = (CachedBodyTeamId == 1);
	UAnimSequence* Idle = bTeam1 ? Team1IdleAnim.Get() : Team0IdleAnim.Get();
	UAnimSequence* Walk = bTeam1 ? Team1WalkAnim.Get() : Team0WalkAnim.Get();
	UAnimSequence* Run  = bTeam1 ? Team1RunAnim.Get()  : Team0RunAnim.Get();
	if (Idle == nullptr && Walk == nullptr && Run == nullptr)
	{
		return;
	}

	// Prefer CMC velocity (replicates on proxies); actor velocity can lag for bots.
	FVector Vel = FVector::ZeroVector;
	if (const UCharacterMovementComponent* Move = GetCharacterMovement())
	{
		Vel = Move->Velocity;
	}
	else
	{
		Vel = GetVelocity();
	}
	const float Speed = Vel.Size2D();

	// 8-direction armed locomotion when the rifle sets loaded (strafing plays real sidestep/backpedal anims —
	// the single biggest "mannequins sliding around" tell). Falls back to the fwd-only 3-state set otherwise.
	const bool bDirectional = ArmedWalkDir.Num() == 8 && ArmedJogDir.Num() == 8
		&& ArmedWalkDir[0] != nullptr && ArmedJogDir[0] != nullptr
		&& Idle == ArmedIdleAnim;   // only when the armed set is what's routed (pf.ArmedAnims on)

	uint8 Want = 1;
	int32 Dir = 0;
	if (Speed > 30.f)
	{
		if (bDirectional)
		{
			// Velocity direction relative to facing, 45° buckets, clockwise from forward.
			const float LocalYawDeg = FMath::UnwindDegrees(
				FMath::RadiansToDegrees(FMath::Atan2(Vel.Y, Vel.X)) - GetActorRotation().Yaw);
			Dir = FMath::RoundToInt(LocalYawDeg / 45.f) & 7;
			// Sprint (>700) only has a forward anim; sideways sprinting reads better as fast jog.
			if (Speed > 700.f && Dir == 0 && Run != nullptr) { Want = 3; }
			else if (Speed > 300.f)                          { Want = static_cast<uint8>(20 + Dir); }
			else                                             { Want = static_cast<uint8>(10 + Dir); }
		}
		else
		{
			if (Speed > 500.f && Run != nullptr)      { Want = 3; }
			else if (Walk != nullptr)                 { Want = 2; }
			else if (Run != nullptr)                  { Want = 3; }
		}
	}

	if (Want == SeqLocoState)
	{
		return;
	}
	SeqLocoState = Want;

	UAnimSequence* Seq = Idle;
	if (Want == 2) { Seq = Walk ? Walk : (Run ? Run : Idle); }
	else if (Want == 3) { Seq = Run ? Run : (Walk ? Walk : Idle); }
	else if (Want >= 20) { Seq = ArmedJogDir[Want - 20] ? ArmedJogDir[Want - 20].Get() : Walk; }
	else if (Want >= 10) { Seq = ArmedWalkDir[Want - 10] ? ArmedWalkDir[Want - 10].Get() : Walk; }
	if (Seq == nullptr)
	{
		return;
	}

	// Use SingleNode API — more reliable than PlayAnimation when mode was already set.
	GetMesh()->PlayAnimation(Seq, /*bLooping=*/true);
	if (UAnimSingleNodeInstance* Node = GetMesh()->GetSingleNodeInstance())
	{
		Node->SetLooping(true);
		Node->SetPlaying(true);
		Node->SetPlayRate(1.f);
	}
}

void ACombatForgeCharacter::UpdateBuildPhaseWeaponVisibility()
{
	bool bHideForBuild = false;
	if (const UWorld* World = GetWorld())
	{
		if (const ACombatForgeGameState* GS = World->GetGameState<ACombatForgeGameState>())
		{
			bHideForBuild = (GS->Phase == EPFMatchPhase::Build);
		}
	}

	// FP viewmodel (owner): fully off during build so it doesn't block placement, AND off while
	// eliminated — without the elim gate this per-tick re-show defeated the death hide and left the
	// owner's own rifle floating at the death spot until respawn (Tom's "floating gun").
	if (ViewModelRoot != nullptr)
	{
		const bool bElim = (GetHealth() != nullptr && GetHealth()->bEliminated);
		ViewModelRoot->SetVisibility(!bHideForBuild && !bElim, /*bPropagateToChildren=*/true);
	}
	// TP rifles (hand + back sling): hide in build / elim so builders don't look armed.
	if (BackWeaponMeshComp != nullptr)
	{
		const bool bElim = (GetHealth() != nullptr && GetHealth()->bEliminated);
		BackWeaponMeshComp->SetHiddenInGame(bHideForBuild || bElim);
	}
	if (WeaponMeshComp != nullptr)
	{
		// Don't fight elimination hide — elim appearance owns that path.
		if (bHideForBuild)
		{
			WeaponMeshComp->SetHiddenInGame(true);
		}
		// When leaving build, re-show unless eliminated (SetEliminatedAppearance may re-hide).
		else if (const UPFHealthComponent* Health = GetHealth())
		{
			WeaponMeshComp->SetHiddenInGame(Health->bEliminated);
		}
		else
		{
			WeaponMeshComp->SetHiddenInGame(false);
		}
	}
}

bool ACombatForgeCharacter::ShouldRaiseWeapon() const
{
	// Raise whenever we're aiming or shooting so the muzzle leaves hip height.
	if (IsADS())
	{
		return true;
	}
	if (bFireHeld)
	{
		return true;
	}
	if (WeaponComponent != nullptr && WeaponComponent->WantsFire())
	{
		return true;
	}
	if (WeaponRaiseHoldSec > 0.f)
	{
		return true;
	}
	return false;
}

FVector ACombatForgeCharacter::GetEyeWorldLocation() const
{
	if (const UCapsuleComponent* Cap = GetCapsuleComponent())
	{
		const float EyeUp = Cap->GetScaledCapsuleHalfHeight() - CameraEyeOffsetFromCapsuleTop;
		return GetActorLocation() + Cap->GetUpVector() * EyeUp;
	}
	return GetActorLocation() + FVector(0.f, 0.f, 60.f);
}

// NOTE: the old ApplyRaisedWeaponPose() lived here and was DEAD CODE — nothing ever called it (it re-parented
// the TP gun to the capsule at eye height, which is the bug that put guns on heads and broke muzzle sampling).
// It was deleted 2026-07-18 because it silently absorbed a pistol-rotation fix that therefore never ran. The
// ONE live TP weapon-pose path is UpdateWeaponHoldPose() below — edit that.

void ACombatForgeCharacter::UpdateWeaponHoldPose()
{
	// Force ALL weapon visuals hidden every frame while eliminated. SetEliminatedAppearance hides them
	// once, but UpdateBuildPhaseWeaponVisibility re-shows the FP viewmodel every tick (no elim gate) —
	// so the OWNER saw their own first-person rifle floating at the death spot for the 5 s corpse window
	// (Tom: "gun stays in the air"). Backstop the TP gun AND the FP viewmodel/arms every tick.
	if (const UPFHealthComponent* H = GetHealth())
	{
		if (H->bEliminated)
		{
			if (WeaponMeshComp != nullptr) { WeaponMeshComp->SetHiddenInGame(true); }
			if (BackWeaponMeshComp != nullptr) { BackWeaponMeshComp->SetHiddenInGame(true); }
			if (ViewModelRoot != nullptr)  { ViewModelRoot->SetVisibility(false, /*bPropagateToChildren=*/true); }
			if (FirstPersonArms != nullptr) { FirstPersonArms->SetVisibility(false); }
			return;
		}
	}

	if (WeaponMeshComp == nullptr || WeaponMesh == nullptr || !bUsingArtBody)
	{
		return;
	}
	if (WeaponMeshComp->bHiddenInGame)
	{
		return;
	}
	// Always keep the TP gun on the hand bone. The old "raise to eye" path parented the
	// mesh to the capsule root at eye height — remotes saw every gun sitting on the head
	// and auth balls spawned from that tip. FP viewmodel (camera) stays separate for the owner.
	ApplyHandWeaponPose();

	// While aiming/shooting, POINT the hand-held gun along the aim. Gun STAYS attached to hand_r (muzzle
	// sampling depends on that). We re-orient in place and apply a SMALL lift so the barrel clears the
	// thigh — never a large world-Z teleport (that was the "gun on neck/armpit" bug with the dual sling).
	if (ShouldRaiseWeapon())
	{
		// WHO OWNS THE WEAPON POSE?
		// With the IK-retargeted rifle-hold set active, the ANIMATION already puts both hands on the gun and
		// points it down the aim — the mesh is parented to hand_r, so it follows that pose for free. Overriding
		// the rotation here FOUGHT the animation: ShouldRaiseWeapon() flips on/off constantly (bots re-arm fire
		// every few frames), so the gun snapped between the animated pose and this aim-derived one every tick.
		// That was the "rifle glitching between correct and angled down" flicker (Tom 2026-07-18).
		// So: only pose the gun in code when the UNARMED fallback set is driving the arms (they hang, and the
		// gun genuinely has to be pointed + lifted by hand). Otherwise leave it to the animation.
		const bool bArmedIdleActive = CVarArmedAnims.GetValueOnGameThread() != 0 && ArmedIdleAnim != nullptr;
		if (!bArmedIdleActive)
		{
			const FRotator Aim = GetBaseAimRotation();
			// PER-WEAPON barrel-axis correction (default -90 / 0 = SM_Rifle +Y barrel; pistols flip roll).
			WeaponMeshComp->SetWorldRotation(FRotator(Aim.Pitch, Aim.Yaw + CachedTPRaisedYaw, CachedTPRaisedRoll));

			// Modest lift so the hanging-arm pose doesn't leave the barrel in the thigh. Hard-clamped —
			// cannot reach the head (that was the forehead-gun bug).
			if (WeaponMaxShoulderLiftUU > 0.f)
			{
				const float TargetZ = GetEyeWorldLocation().Z - WeaponShoulderDropFromEyeUU;
				const float CurrentZ = WeaponMeshComp->GetComponentLocation().Z;
				const float Lift = FMath::Clamp(TargetZ - CurrentZ, 0.f, WeaponMaxShoulderLiftUU);
				if (Lift > 0.f)
				{
					WeaponMeshComp->AddWorldOffset(FVector(0.f, 0.f, Lift));
				}
			}
		}
	}
}

void ACombatForgeCharacter::ApplyArtLoadout()
{
	// Optional M1 art. Every branch is a no-op when its property is unset, so an
	// unconfigured character stays byte-identical to the validated graybox.
	bUsingArtBody = false;
	CachedBodyTeamId = 255;

	// Provisional body from the (possibly not-yet-replicated) team; SetTeamColor re-drives it
	// authoritatively when the real TeamId lands.
	ApplyTeamBody(CachedTeamId);

	if (FirstPersonArmsMesh != nullptr && FirstPersonArms != nullptr)
	{
		FirstPersonArms->SetSkeletalMeshAsset(FirstPersonArmsMesh);
		FirstPersonArms->SetVisibility(true);
	}

	AttachWeaponToHand();

	SetTeamColor(CachedTeamId);   // re-tint whatever body is now active (skeletal or cubes)
}

void ACombatForgeCharacter::SetTeamColor(uint8 TeamId)
{
	// SetTeamColor is the one convergence point that runs on server, host, and pure clients once the real
	// TeamId is known (in either replication order) and again on host team-cycle — so the per-team BODY
	// swap lives here, not in BeginPlay (where a client often doesn't know its team yet).
	const bool bTeamChanged = (TeamId != CachedTeamId) || !bTeamAppearanceApplied;
	CachedTeamId = TeamId;

	if (TeamId <= 1)
	{
		ApplyTeamBody(TeamId);   // gated: re-mounts Manny/Quinn only on an actual team change
	}

	if (!bTeamChanged)
	{
		return;   // appearance already applied for this team; don't rebuild MIDs on every flag replication
	}
	bTeamAppearanceApplied = true;

	const FLinearColor TeamColor = PFColors::ForTeam(TeamId);
	// Team armband takes the full vivid team color (max readability — it's the primary friend/foe tell).
	if (ArmbandMID != nullptr)
	{
		ArmbandMID->SetVectorParameterValue(TEXT("Color"), TeamColor);
	}
	if (ArmbandMIDR != nullptr)
	{
		ArmbandMIDR->SetVectorParameterValue(TEXT("Color"), TeamColor);
	}
	// FFA has no teams → hide the armband tell (belt-and-suspenders with the mount-time gate, in case team info
	// converged after the body was assembled). Team modes keep it visible.
	{
		const ACombatForgeGameState* GSFFA = GetWorld() ? GetWorld()->GetGameState<ACombatForgeGameState>() : nullptr;
		const bool bFFA = (GSFFA != nullptr) && (GSFFA->MatchType == EPFMatchType::FreeForAll);
		if (ArmbandMesh != nullptr)  { ArmbandMesh->SetVisibility(!bFFA); }
		if (ArmbandMeshR != nullptr) { ArmbandMeshR->SetVisibility(!bFFA); }
	}
	// Muted team tint for the soldier body — clearly team-colored but not neon speedball (splats/tracers
	// keep the full vivid ForTeam color). Tune the 0.45 lerp toward gray to taste.
	const FLinearColor BodyTint = FMath::Lerp(TeamColor, FLinearColor(0.22f, 0.22f, 0.24f, 1.f), 0.45f);

	if (BodyMesh != nullptr && BodyMID == nullptr)
	{
		BodyMID = BodyMesh->CreateAndSetMaterialInstanceDynamic(0);
	}
	if (HeadMesh != nullptr && HeadMID == nullptr)
	{
		HeadMID = HeadMesh->CreateAndSetMaterialInstanceDynamic(0);
	}
	if (BodyMID != nullptr)
	{
		BodyMID->SetVectorParameterValue(TEXT("Color"), TeamColor);
	}
	if (HeadMID != nullptr)
	{
		HeadMID->SetVectorParameterValue(TEXT("Color"), TeamColor);
	}

	// Human packs (Quantum / Survival) keep their multi-slot authored materials — team identity
	// is the model itself. Mannequin/graybox may still take a single-slot tint override.
	if (bUsingArtBody && GetMesh() != nullptr && !bPreserveAuthoredMaterials)
	{
		UMaterialInterface* NativeMat = (TeamId == 1)
			? (Team1BodyMaterial ? Team1BodyMaterial.Get() : Team0BodyMaterial.Get())
			: Team0BodyMaterial.Get();

		if (NativeMat != nullptr)
		{
			const int32 NumMats = GetMesh()->GetNumMaterials();
			for (int32 Slot = 0; Slot < NumMats; ++Slot)
			{
				GetMesh()->SetMaterial(Slot, NativeMat);
			}
		}
		else if (TeamBodyMaterial != nullptr)
		{
			const int32 NumMats = GetMesh()->GetNumMaterials();
			for (int32 Slot = 0; Slot < NumMats; ++Slot)
			{
				if (UMaterialInstanceDynamic* BodyArtMID =
						GetMesh()->CreateAndSetMaterialInstanceDynamicFromMaterial(Slot, TeamBodyMaterial))
				{
					BodyArtMID->SetVectorParameterValue(TEXT("Color"), BodyTint);
				}
			}
		}
	}
}

void ACombatForgeCharacter::SetEliminatedAppearance(bool bEliminated)
{
	// Mesh only — collision timing (0.5 s corpse block) is the health
	// component's job (04 §2.4).
	// Freeze the locomotion driver while out: it runs every Tick and would stomp the death fall back to
	// looping idle ONE FRAME after it starts (bug hunt 2026-07-15 — the fall never rendered).
	bEliminatedAppearanceActive = bEliminated;
	if (BodyMesh != nullptr)
	{
		BodyMesh->SetHiddenInGame(bEliminated);
	}
	if (HeadMesh != nullptr)
	{
		HeadMesh->SetHiddenInGame(bEliminated);
	}
	if (bUsingArtBody && GetMesh() != nullptr)
	{
		if (bEliminated && bSequenceLocoActive && DeathDirAnims.Num() == 4 && DeathDirAnims[0] != nullptr)
		{
			// "He's out": play a directional fall (random pick — the shot direction isn't plumbed here and the
			// read at gameplay distance is 'body drops', not which way), THEN hide when the anim lands. Beats
			// the old vanish-in-place. Weapon/viewmodel still hide instantly below.
			UAnimSequence* Death = DeathDirAnims[FMath::RandRange(0, 3)].Get();
			if (Death == nullptr) { Death = DeathDirAnims[0].Get(); }
			GetMesh()->PlayAnimation(Death, /*bLooping=*/false);
			SeqLocoState = 0;   // force locomotion re-arm on respawn
			GetWorldTimerManager().SetTimer(DeathHideTimer, FTimerDelegate::CreateWeakLambda(this, [this]()
			{
				if (GetMesh() != nullptr)
				{
					GetMesh()->SetHiddenInGame(true, /*bPropagateToChildren=*/true);
				}
			}), FMath::Max(0.2f, Death->GetPlayLength() - 0.05f), false);
		}
		else
		{
			// Propagate to children so the modular character parts (CharBaseComps/CharSlotComps) + armband hide with
			// the base body — otherwise an eliminated pawn leaves its clothing/head/legs frozen in the idle pose.
			GetWorldTimerManager().ClearTimer(DeathHideTimer);
			GetMesh()->SetHiddenInGame(bEliminated, /*bPropagateToChildren=*/true);
			if (!bEliminated)
			{
				SeqLocoState = 0;   // back alive: next UpdateSequenceLocomotion re-picks the right anim
			}
		}
	}
	if (WeaponMeshComp != nullptr)
	{
		WeaponMeshComp->SetHiddenInGame(bEliminated);
	}
	if (BackWeaponMeshComp != nullptr)
	{
		BackWeaponMeshComp->SetHiddenInGame(bEliminated);
	}
	if (ViewModelRoot != nullptr)
	{
		ViewModelRoot->SetVisibility(!bEliminated, /*bPropagateToChildren=*/true);
	}

	if (!bEliminated)
	{
		// Respawns REUSE this pawn (reset-in-place + teleport, no repossession), so PawnClientRestart never
		// re-fires — without this push, a class switched on the death screen showed on the countdown UI but
		// you respawned with the OLD kit. Revive runs on every machine; PushLocalKit no-ops on bots/remotes.
		PushLocalKit();
	}
}

FVector ACombatForgeCharacter::ResolveGunMuzzleWorld(const UStaticMeshComponent* Gun, const FVector& LocalFallback)
{
	if (Gun == nullptr || Gun->GetStaticMesh() == nullptr)
	{
		return FVector::ZeroVector;
	}

	// 1) Authored sockets (Marketplace / Bandits often ship these) — true barrel tip when present.
	static const FName SocketNames[] = {
		TEXT("Muzzle"), TEXT("muzzle"), TEXT("MuzzleFlash"), TEXT("muzzle_flash"),
		TEXT("Muzzle_Flash"), TEXT("barrel"), TEXT("Barrel"), TEXT("barrel_end"),
		TEXT("WP_Muzzle"), TEXT("socket_muzzle"), TEXT("Fire"), TEXT("fire")
	};
	for (const FName& Sock : SocketNames)
	{
		if (Gun->DoesSocketExist(Sock))
		{
			return Gun->GetSocketLocation(Sock);
		}
	}

	// 2) Auto tip from mesh bounds: barrel runs along the longest horizontal axis; tip at the +end,
	// slightly above the box center so grip/stock mass below the bore doesn't pull the point into the handguard.
	const FBoxSphereBounds B = Gun->GetStaticMesh()->GetBounds();
	const FVector O = B.Origin;
	const FVector E = B.BoxExtent;
	FVector TipLocal = O;
	if (E.Y >= E.X)
	{
		// SM_Rifle / many Bandits: local +Y is barrel-forward.
		TipLocal = FVector(O.X, O.Y + E.Y, O.Z + E.Z * 0.12f);
	}
	else
	{
		// Most MarketplaceBlockout statics: local +X is barrel-forward.
		TipLocal = FVector(O.X + E.X, O.Y, O.Z + E.Z * 0.12f);
	}
	// Nudge past the tip so the BB doesn't spawn inside the solid.
	const FVector Along = (TipLocal - O).GetSafeNormal();
	if (!Along.IsNearlyZero())
	{
		TipLocal += Along * 3.f;
	}
	else if (!LocalFallback.IsNearlyZero())
	{
		TipLocal = LocalFallback;
	}
	return Gun->GetComponentTransform().TransformPosition(TipLocal);
}

FVector ACombatForgeCharacter::GetMuzzleLocation(bool bCosmetic) const
{
	// Owning-client cosmetic tracers: FP viewmodel gun mesh barrel (any catalog weapon).
	if (bCosmetic)
	{
		if (RifleFPMesh != nullptr && RifleFPMesh->GetStaticMesh() != nullptr && RifleFPMesh->IsVisible())
		{
			// Mesh-local auto tip → world via the FP component (pose/scale already applied).
			const FVector W = ResolveGunMuzzleWorld(RifleFPMesh, FVector::ZeroVector);
			if (!W.IsNearlyZero())
			{
				return W;
			}
		}
		// Graybox marker parts: fall back to the composed ViewModelRoot offset.
		if (ViewModelRoot != nullptr && !MuzzleLocalFP.IsNearlyZero())
		{
			return ViewModelRoot->GetComponentTransform().TransformPosition(MuzzleLocalFP);
		}
		if (FirstPersonCamera != nullptr)
		{
			return FirstPersonCamera->GetComponentLocation() + FirstPersonCamera->GetForwardVector() * 55.f;
		}
	}

	// Authoritative / remote: TP gun on the body — same auto barrel tip, no per-weapon hand offset.
	// IsAttachedTo (not direct parent) so intermediate pose parents can't drop us to the eye fallback.
	if (WeaponMeshComp != nullptr && WeaponMeshComp->GetStaticMesh() != nullptr && bUsingArtBody
		&& !WeaponMeshComp->bHiddenInGame
		&& GetMesh() != nullptr && WeaponMeshComp->IsAttachedTo(GetMesh()))
	{
		const FVector W = ResolveGunMuzzleWorld(WeaponMeshComp, RifleMuzzleLocalTP);
		if (!W.IsNearlyZero())
		{
			return W;
		}
	}

	// Fallback: slightly forward of the eye (no art body / hidden gun).
	const FRotationMatrix AimBasis(GetBaseAimRotation());
	return GetEyeWorldLocation()
		+ AimBasis.GetUnitAxis(EAxis::X) * 55.f
		+ AimBasis.GetUnitAxis(EAxis::Y) * 12.f
		+ AimBasis.GetUnitAxis(EAxis::Z) * (-8.f);
}

// ---------------------------------------------------------------------------
// Weapon viewmodel + muzzle-flash cosmetics (M1 art pass)
// ---------------------------------------------------------------------------

UStaticMeshComponent* ACombatForgeCharacter::MakeGunPart(USceneComponent* Parent, const FString& CompName,
	UStaticMesh* PartMesh, UMaterialInterface* Mat, const FVector& RelLoc, const FVector& RelScale,
	const FRotator& RelRot)
{
	UStaticMeshComponent* Part = CreateDefaultSubobject<UStaticMeshComponent>(FName(*CompName));
	if (Part != nullptr)
	{
		Part->SetupAttachment(Parent);
		if (PartMesh != nullptr) { Part->SetStaticMesh(PartMesh); }
		if (Mat != nullptr) { Part->SetMaterial(0, Mat); }
		Part->SetRelativeLocation(RelLoc);
		Part->SetRelativeScale3D(RelScale);
		Part->SetRelativeRotation(RelRot);
		Part->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Part->SetCastShadow(false);
	}
	return Part;
}

void ACombatForgeCharacter::BuildMarker(USceneComponent* Parent, const FString& Prefix, UStaticMesh* Cube,
	UStaticMesh* Cylinder, TArray<TObjectPtr<UStaticMeshComponent>>& OutParts, FVector& OutMuzzleLocal)
{
	// Silhouette in uu (barrel points +X). Engine Cube/Cylinder are 100 uu, so scale = size/100.
	// Base material is assigned by the caller (owner-only-see for the FP viewmodel).
	OutParts.Add(MakeGunPart(Parent, Prefix + TEXT("Recv"), Cube, nullptr,      // receiver / upper
		FVector(0.f, 0.f, 0.f), FVector(0.34f, 0.06f, 0.10f), FRotator::ZeroRotator));
	OutParts.Add(MakeGunPart(Parent, Prefix + TEXT("Guard"), Cube, nullptr,     // handguard
		FVector(24.f, 0.f, -1.f), FVector(0.22f, 0.05f, 0.055f), FRotator::ZeroRotator));
	OutParts.Add(MakeGunPart(Parent, Prefix + TEXT("Barrel"), Cylinder, nullptr, // barrel (cyl +Z -> +X)
		FVector(40.f, 0.f, 1.5f), FVector(0.024f, 0.024f, 0.28f), FRotator(90.f, 0.f, 0.f)));
	OutParts.Add(MakeGunPart(Parent, Prefix + TEXT("Stock"), Cube, nullptr,     // stock
		FVector(-20.f, 0.f, -1.5f), FVector(0.20f, 0.05f, 0.075f), FRotator::ZeroRotator));
	OutParts.Add(MakeGunPart(Parent, Prefix + TEXT("Mag"), Cube, nullptr,       // magazine (angled)
		FVector(6.f, 0.f, -11.f), FVector(0.05f, 0.035f, 0.16f), FRotator(0.f, 0.f, 18.f)));
	OutParts.Add(MakeGunPart(Parent, Prefix + TEXT("Grip"), Cube, nullptr,      // pistol grip
		FVector(-6.f, 0.f, -9.f), FVector(0.045f, 0.045f, 0.12f), FRotator(0.f, 0.f, -12.f)));
	OutMuzzleLocal = FVector(54.f, 0.f, 1.5f);   // barrel tip
}

void ACombatForgeCharacter::SetupWeaponMaterials()
{
	// Dark gunmetal on the primitive marker fallback (real SM_Rifle uses M_PF_Rifle).
	if (MarkerPartsFP.Num() > 0 && MarkerPartsFP[0] != nullptr)
	{
		MarkerMID = MarkerPartsFP[0]->CreateAndSetMaterialInstanceDynamic(0);
		if (MarkerMID != nullptr)
		{
			MarkerMID->SetVectorParameterValue(TEXT("Color"), FLinearColor(0.035f, 0.038f, 0.045f));
			for (int32 Index = 1; Index < MarkerPartsFP.Num(); ++Index)
			{
				if (MarkerPartsFP[Index] != nullptr)
				{
					MarkerPartsFP[Index]->SetMaterial(0, MarkerMID);
				}
			}
		}
	}
}

void ACombatForgeCharacter::OnFireCosmetic(float RecoilScale)
{
	// Marker: viewmodel recoil + CO₂ muzzle wisp (PlayMuzzle FX is driven from the weapon fire path).
	RecoilOffset += FVector(-RecoilKickUU, 0.f, RecoilKickUU * 0.35f) * RecoilScale;
	RecoilPitch += RecoilKickPitchDeg * RecoilScale;
	// Spring VELOCITY impulses (not position sets): the fast-out + one-bounce settle is what sells the shot.
	// Random yaw/roll per shot keeps auto fire alive instead of metronomic.
	KickBack.Vel  -= 55.f * RecoilScale;                                   // cm/s straight back
	KickUp.Vel    += 14.f * RecoilScale;
	KickPitch.Vel += 140.f * RecoilScale;                                  // deg/s muzzle-up
	KickYaw.Vel   += FMath::FRandRange(-45.f, 45.f) * RecoilScale;
	KickRoll.Vel  += FMath::FRandRange(-80.f, 80.f) * RecoilScale;
	WeaponRaiseHoldSec = FMath::Max(WeaponRaiseHoldSec, WeaponRaiseHoldOnShot);
	// Do NOT ApplyRaisedWeaponPose() here. It re-parented the TP rifle to the capsule AT EYE HEIGHT for exactly
	// the one frame in which FireOneShot samples GetMuzzleLocation(false) → the hand-rifle gate failed and every
	// third-person shot (bots + remote players) spawned its BB at the shooter's EYES. The raise was already
	// visually dead (UpdateWeaponHoldPose re-seats the gun to the hand every tick); killing it fixes the origin.
}

void ACombatForgeCharacter::OnRemoteFireCosmetic()
{
	// No raised-pose swap (see OnFireCosmetic — it poisoned same-frame GetMuzzleLocation).
	// Muzzle VFX/audio are spawned by MulticastShotFX / FireOneShot, not here.
	WeaponRaiseHoldSec = FMath::Max(WeaponRaiseHoldSec, WeaponRaiseHoldOnShot);
}
