// Copyright Epic Games, Inc. All Rights Reserved.

#include "PursuitCharAgent.h"

#include "ActuatorInterface.h"
#include "Actuators/MovementInputActuator.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimSingleNodeInstance.h"
#include "Common/InteractionDefinition.h"
#include "Components/ActorComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "PursuitAI.h"
#include "PursuitJumpActuator.h"
#include "NNEModelData.h"
#include "Points/DictPoint.h"
#include "Policies/NNEPolicy.h"
#include "PursuitTargetSensor.h"
#include "SensorInterface.h"
#include "Spaces/DictSpace.h"
#include "Steppers/SimpleStepper.h"

namespace
{
	bool IsValidScholaComponent(const UActorComponent* Component, const UClass* InterfaceClass)
	{
		return Component && Component->GetClass()->ImplementsInterface(InterfaceClass);
	}
}

APursuitCharAgent::APursuitCharAgent()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = true;

	// No controller will ever possess this character in the RL path; the policy (or the
	// env's scripted driver) IS the controller. AutoPossess would fight the pipeline.
	AutoPossessAI = EAutoPossessAI::Disabled;
	AIControllerClass = nullptr;

	// Schola components. Named here once; the enumeration rule below turns these names
	// into the dict keys that both the trainer and the exported ONNX tensor names use,
	// so renaming a component in C++ is the only way the keys can change.
	MoveActuator = CreateDefaultSubobject<UMovementInputActuator>(TEXT("CharMoveInput"));
	MoveActuator->bHasXDimension = true;   // forward / back
	MoveActuator->bHasYDimension = true;   // strafe left / right
	MoveActuator->bHasZDimension = false;  // ground chase; Z belongs to gravity and jumps
	// NON-NEGATIVE action space: the policy may only push forward, never reverse.
	//
	// A symmetric [-1, +1] space was the single biggest obstacle to learning to chase
	// (measured 2026-09-22): with reverse available, "walk away from the target" is an
	// attractor a young policy falls into, and the 350k run ended with 0/38 catches and
	// d_end values of 3000+ cm against a 400-700 cm spawn gap - the dog was fleeing.
	// A chasing task never needs reverse; removing it halves the action volume and
	// deletes the flee basin outright. Turning is unaffected: the policy still steers
	// with the pair (forward, strafe) applied along forward/right vectors.
	MoveActuator->MinSpeed = 0.0f;
	MoveActuator->MaxSpeed = 1.0f;
	MoveActuator->ScaleValue = 1.0f;

	// The third action dimension: one continuous "jump intent" in [-1, 1] that fires a
	// real LaunchCharacter when it clears the threshold. The component NAME is the dict
	// key on both the trainer and the exported ONNX tensors - renaming it here breaks
	// the export tooling (tools/export_policy.bat passes the key list explicitly).
	JumpActuator = CreateDefaultSubobject<UPursuitJumpActuator>(TEXT("JumpInput"));

	TargetSensor = CreateDefaultSubobject<UPursuitTargetSensor>(TEXT("TargetSensor"));

	SetupMovementDefaults();
}

void APursuitCharAgent::SetupMovementDefaults()
{
	if (UCharacterMovementComponent* Movement = GetCharacterMovement())
	{
		// With no controller, the movement component refuses to run unless told not to
		// care (TagAgent needs the same line - without it the character stands still
		// forever and the failure looks like a dead policy rather than a flag).
		Movement->bRunPhysicsWithNoController = true;
		Movement->DefaultLandMovementMode = MOVE_Walking;

		// The action is forward/strafe in the pawn's local frame. With nothing to yaw the
		// capsule, "face where you are going" has to come from the movement component
		// itself: the character rotates toward its velocity, which is what makes forward
		// input mean "the way I am already moving" instead of a fixed world axis.
		Movement->bOrientRotationToMovement = true;
		bUseControllerRotationYaw = false;

		Movement->MaxWalkSpeed = ChaseMaxSpeed;

		// ------------------------------------------------------------------
		// ROOT CAUSE FIX 2026-09-22 (v3.12). Until this line existed,
		// SetupMovementDefaults set MaxWalkSpeed and NOTHING ELSE. Measured
		// consequence: on the ground the chaser moved at ~30 cm/s against
		// MaxWalkSpeed 575 - a factor of 19 - while airborne it moved at 260
		// cm/s. The policy therefore learned to jump, because jumping was the
		// only way it could move fast, and no amount of jump penalty could
		// change that (the v3.10/v3.11 runs proved it: ~-73 reward per episode
		// in jump cost, still hopping).
		//
		// Why the defaults are not enough HERE, though they are fine for a
		// player pawn: the policy's action is applied ONCE PER ENV STEP
		// (MovementInputActuator -> AddMovementInput), while friction is
		// applied EVERY ENGINE FRAME. This project runs at ~545 fps
		// (Dt = 0.00183 s, derived from "3271 steps = 6.0 sim-s"), so the
		// character accumulates one frame of acceleration and then coasts
		// through ~1 frame of GroundFriction before the next action lands.
		// With UE's defaults (MaxAcceleration 2048, GroundFriction 8.0) the
		// per-frame gain is ~3.7 cm/s and friction eats most of it, so speed
		// plateaus near 30 cm/s. Higher fps makes this WORSE, not better -
		// the opposite of the usual intuition.
		//
		// The hand-driven APursuitCharacter (PursuitCharacter.cpp) sets a full
		// profile and has never shown this - which is exactly why the RL cast
		// behaved differently from every other character in the project and
		// why the bug survived so long.
		//
		// Values chosen to reach MaxWalkSpeed within a single step:
		//   MaxAcceleration 4096 - 2x the default, ~7.5 cm/s per frame at
		//     545 fps, so a handful of frames saturates to 575.
		//   BrakingDecelerationWalking 2200 - matches APursuitCharacter, and
		//     still lets the agent stop deliberately rather than skidding.
		//   GroundFriction 1.0 - down from 8.0. High friction is what makes a
		//     once-per-step input decay before the next step arrives. Lowering
		//     it is what makes the current speed actually PERSIST between
		//     steps, which is the whole point of a step-based controller.
		// ------------------------------------------------------------------
		Movement->MaxAcceleration = 4096.0f;
		Movement->BrakingDecelerationWalking = 2200.0f;
		Movement->GroundFriction = 1.0f;
	}
}

EAgentStatus APursuitCharAgent::GetStatus_Implementation()
{
	return AgentStatus;
}

void APursuitCharAgent::SetStatus_Implementation(EAgentStatus NewStatus)
{
	AgentStatus = NewStatus;
}

void APursuitCharAgent::Define_Implementation(FInteractionDefinition& OutInteractionDefinition)
{
	RefreshComponentBindings();
	InitializeInteractors();

	OutInteractionDefinition.ObsSpaceDefn.InitializeAs<FDictSpace>();
	FDictSpace& ObservationSpace = OutInteractionDefinition.ObsSpaceDefn.GetMutable<FDictSpace>();
	ObservationSpace.Spaces.Reset();

	for (const FComponentBinding& SensorBinding : Sensors)
	{
		if (const UObject* SensorObject = SensorBinding.Object.Get())
		{
			TInstancedStruct<FSpace> SensorSpace;
			IScholaSensor::Execute_GetObservationSpace(SensorObject, SensorSpace);
			ObservationSpace.Spaces.Add(SensorBinding.Key, SensorSpace);
		}
	}

	OutInteractionDefinition.ActionSpaceDefn.InitializeAs<FDictSpace>();
	FDictSpace& ActionSpace = OutInteractionDefinition.ActionSpaceDefn.GetMutable<FDictSpace>();
	ActionSpace.Spaces.Reset();

	for (const FComponentBinding& ActuatorBinding : Actuators)
	{
		if (const UObject* ActuatorObject = ActuatorBinding.Object.Get())
		{
			TInstancedStruct<FSpace> ActuatorSpace;
			IScholaActuator::Execute_GetActionSpace(ActuatorObject, ActuatorSpace);
			ActionSpace.Spaces.Add(ActuatorBinding.Key, ActuatorSpace);
		}
	}
}

void APursuitCharAgent::Observe_Implementation(FInstancedStruct& OutObservations)
{
	OutObservations.InitializeAs<FDictPoint>();
	FDictPoint& ObservationDict = OutObservations.GetMutable<FDictPoint>();
	ObservationDict.Points.Reset();

	for (const FComponentBinding& SensorBinding : Sensors)
	{
		if (UObject* SensorObject = SensorBinding.Object.Get())
		{
			TInstancedStruct<FPoint> SensorObservation;
			IScholaSensor::Execute_CollectObservations(SensorObject, SensorObservation);
			ObservationDict.Points.Add(SensorBinding.Key, SensorObservation);
		}
	}
}

void APursuitCharAgent::Act_Implementation(const FInstancedStruct& InAction)
{
	const FDictPoint* ActionDict = InAction.GetPtr<FDictPoint>();
	if (!ActionDict)
	{
		// Single-actuator policies may return the actuator point directly instead of a
		// dict. Accepting both here is what lets the same actor train over gRPC and run
		// an exported ONNX without either side knowing about the other's packaging.
		if (Actuators.Num() == 1)
		{
			if (UObject* ActuatorObject = Actuators[0].Object.Get())
			{
				IScholaActuator::Execute_TakeAction(ActuatorObject, InAction);
			}
		}
		return;
	}

	for (const FComponentBinding& ActuatorBinding : Actuators)
	{
		UObject* ActuatorObject = ActuatorBinding.Object.Get();
		const TInstancedStruct<FPoint>* ActionPoint = ActionDict->Points.Find(ActuatorBinding.Key);
		if (ActuatorObject && ActionPoint)
		{
			IScholaActuator::Execute_TakeAction(ActuatorObject, *ActionPoint);
		}
	}
}

void APursuitCharAgent::ApplyScriptedMove(const FVector& WorldDirection, float Scale)
{
	const FVector Direction = WorldDirection.GetSafeNormal2D();
	if (!Direction.IsNearlyZero())
	{
		AddMovementInput(Direction, Scale);
	}
}

void APursuitCharAgent::BeginPlay()
{
	Super::BeginPlay();

	SetupMovementDefaults();

	// Hit events are bound by the environment, which owns the wall-penalty policy. The
	// agent stays a dumb body that reports its collisions through OnActorHit.

	if (ModelData)
	{
		InitializeRuntime();
	}
	else
	{
		// Scripted agents (the rule evader, the baselines) are "running" the moment they
		// exist; only policy-driven agents wait for their stepper.
		AgentStatus = EAgentStatus::Running;
	}
}

void APursuitCharAgent::EnsureInferenceRuntime()
{
	if (ModelData && !Policy)
	{
		InitializeRuntime();
	}
}

void APursuitCharAgent::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	// Inference runs itself: Observe -> Think -> Act every tick, no Python, no env. In
	// training the env drives Act() over gRPC and ModelData is null, so this is a no-op.
	if (AgentStatus == EAgentStatus::Running && Policy)
	{
		StepPolicy();
	}

	UpdateLocomotionVisual();
}

// ---------------------------------------------------------------------------
// Looks
//
// v1's rig table (PursuitHeroAssets.h) dressed the play-demo characters; this is
// the same table on the RL agent. Idle clip below a walk threshold, run clip above
// it, playback rate scaled by actual speed - no animation blueprint, no states.
// ---------------------------------------------------------------------------

void APursuitCharAgent::LoadVisuals()
{
	USkeletalMeshComponent* MeshComponent = GetMesh();
	if (!MeshComponent)
	{
		return;
	}

	const PursuitCharacterPaths::FSkeletalHeroAssets Assets = PursuitCharacterPaths::HeroAssets(HeroModel);

	USkeletalMesh* Loaded = Cast<USkeletalMesh>(Assets.Mesh.TryLoad());
	if (!Loaded)
	{
		// The one failure that must never pass silently: an invisible capsule in the
		// watch window reads as a frozen sim, not as missing art.
		UE_LOG(LogPursuitAI, Error,
			TEXT("PursuitCharAgent: nothing at %s - %s stays an INVISIBLE capsule. "
			     "Check the rig paths in PursuitHeroAssets.h."),
			*Assets.Mesh.ToString(), *GetName());
		return;
	}

	// Mesh first, animation mode second (v1's LoadVisuals order): SetAnimationMode
	// builds the single node instance against the mesh. Reversed, the instance is
	// never created and PlayAnimation silently does nothing.
	MeshComponent->SetSkeletalMesh(Loaded);
	MeshComponent->SetAnimationMode(EAnimationMode::AnimationSingleNode);

	IdleClip = Cast<UAnimSequence>(Assets.Idle.TryLoad());
	RunClip = Cast<UAnimSequence>(Assets.Run.TryLoad());
	// Rigs without a jump clip (the dog) simply leave this null - UpdateLocomotionVisual
	// falls back to the run clip while airborne, which is the honest degradation.
	JumpClip = Cast<UAnimSequence>(Assets.Jump.TryLoad());
	CurrentClip = nullptr;

	// All pack rigs are authored facing +Y; the capsule's forward is +X (v1 measured
	// the quarter turn in tools/inspect_hero_facing.py).
	MeshComponent->SetRelativeRotation(FRotator(0.0f, -90.0f, 0.0f));

	RigRunClipSpeed = Assets.RunClipSpeed;
	bVisualsLoaded = true;

	// Untextured rig only (the mannequin): flat tint. Pack rigs keep their PBR materials.
	if (Assets.bTint)
	{
		if (UMaterialInterface* BaseMaterial = LoadObject<UMaterialInterface>(nullptr, PursuitCharacterPaths::TintMaterialPath))
		{
			if (UMaterialInstanceDynamic* Dynamic = UMaterialInstanceDynamic::Create(BaseMaterial, this))
			{
				Dynamic->SetVectorParameterValue(TEXT("Color"), Tint);
				const int32 Slots = FMath::Max(MeshComponent->GetNumMaterials(), 1);
				for (int32 Slot = 0; Slot < Slots; ++Slot)
				{
					MeshComponent->SetMaterial(Slot, Dynamic);
				}
			}
		}
		else
		{
			UE_LOG(LogPursuitAI, Warning,
				TEXT("PursuitCharAgent: tint material missing (%s) - %s renders default grey."),
				PursuitCharacterPaths::TintMaterialPath, *GetName());
		}
	}

	UE_LOG(LogPursuitAI, Log,
		TEXT("PursuitCharAgent: %s wears %s - mesh %s, run %.0f cm/s reference"),
		*GetName(), Assets.Label, *Loaded->GetName(), RigRunClipSpeed);
}

void APursuitCharAgent::UpdateLocomotionVisual()
{
	if (!bVisualsLoaded)
	{
		return;
	}

	const USkeletalMeshComponent* MeshComponent = GetMesh();
	UAnimSingleNodeInstance* Node = MeshComponent ? Cast<UAnimSingleNodeInstance>(MeshComponent->GetAnimInstance()) : nullptr;
	if (!Node)
	{
		return;
	}

	const UCharacterMovementComponent* Movement = GetCharacterMovement();
	const float Speed = Movement ? Movement->Velocity.Size2D() : 0.0f;
	const bool bAirborne = Movement && !Movement->IsMovingOnGround();

	// Below a crawl the idle reads better than a slow-motion run; above it, the run at
	// a playback rate that tracks real speed so the feet do not ice-skate. Airborne
	// prefers the rig's jump clip when it has one, run clip otherwise.
	UAnimSequence* Target = bAirborne && JumpClip.IsValid() ? JumpClip.Get()
		: (Speed > 30.0f && RunClip.IsValid()) ? RunClip.Get()
		: IdleClip.Get();
	if (!Target)
	{
		return;
	}

	if (CurrentClip.Get() != Target)
	{
		Node->SetAnimationAsset(Target, /*bIsPlaying*/ true);
		Node->SetLooping(true);
		CurrentClip = Target;
	}

	Node->SetPlayRate(Target == RunClip.Get()
		? FMath::Clamp(Speed / FMath::Max(RigRunClipSpeed, 1.0f), 0.15f, 1.8f)
		: 1.0f);
}

void APursuitCharAgent::RefreshComponentBindings()
{
	AddComponentBindings(this, true, Sensors);
	AddComponentBindings(this, false, Actuators);
}

void APursuitCharAgent::InitializeInteractors()
{
	for (const FComponentBinding& SensorBinding : Sensors)
	{
		if (UObject* SensorObject = SensorBinding.Object.Get())
		{
			IScholaSensor::Execute_InitSensor(SensorObject);
		}
	}

	for (const FComponentBinding& ActuatorBinding : Actuators)
	{
		if (UObject* ActuatorObject = ActuatorBinding.Object.Get())
		{
			IScholaActuator::Execute_InitActuator(ActuatorObject);
		}
	}
}

void APursuitCharAgent::InitializeRuntime()
{
	if (!ModelData)
	{
		UE_LOG(LogTemp, Error, TEXT("PursuitCharAgent %s: inference requested but no ModelData"), *GetName());
		AgentStatus = EAgentStatus::Error;
		return;
	}

	Policy = NewObject<UNNEPolicy>(this, TEXT("PursuitCharPolicy"));
	Stepper = NewObject<USimpleStepper>(this, TEXT("PursuitCharStepper"));
	if (!Policy || !Stepper)
	{
		AgentStatus = EAgentStatus::Error;
		return;
	}

	Policy->ModelData = ModelData;
	Policy->RuntimeName = RuntimeName;
	Policy->MaxStateSequenceLength = MaxStateSequenceLength;

	FInteractionDefinition InteractionDefinition;
	Define_Implementation(InteractionDefinition);

	if (Sensors.Num() == 0 || Actuators.Num() == 0)
	{
		UE_LOG(LogTemp, Error,
			TEXT("PursuitCharAgent %s: sensors=%d actuators=%d - the exported policy expects at least one of each; the component set must mirror training"),
			*GetName(), Sensors.Num(), Actuators.Num());
		AgentStatus = EAgentStatus::Error;
		return;
	}

	if (!Policy->Init(InteractionDefinition))
	{
		UE_LOG(LogTemp, Error, TEXT("PursuitCharAgent %s: policy init failed"), *GetName());
		AgentStatus = EAgentStatus::Error;
		return;
	}

	TScriptInterface<IAgent> AgentInterface;
	AgentInterface.SetObject(this);
	AgentInterface.SetInterface(Cast<IAgent>(this));

	TScriptInterface<IPolicy> PolicyInterface;
	PolicyInterface.SetObject(Policy);
	PolicyInterface.SetInterface(Cast<IPolicy>(Policy));

	if (!Stepper->Init({ AgentInterface }, PolicyInterface))
	{
		UE_LOG(LogTemp, Error, TEXT("PursuitCharAgent %s: stepper init failed"), *GetName());
		AgentStatus = EAgentStatus::Error;
		return;
	}

	AgentStatus = EAgentStatus::Running;
}

void APursuitCharAgent::StepPolicy()
{
	FInstancedStruct Observation;
	Observe_Implementation(Observation);

	FInstancedStruct Action;
	if (!Policy->Think(Observation, Action))
	{
		UE_LOG(LogTemp, Error, TEXT("PursuitCharAgent %s: policy Think failed"), *GetName());
		return;
	}

	Act_Implementation(Action);
}

FString APursuitCharAgent::GetComponentBaseName(const UActorComponent* Component)
{
	// The same rule TagAgent uses, for the same reason: the dict keys here become the
	// tensor names the trainer bakes into the exported ONNX model, so the rule cannot
	// differ by a character between this and the training-side enumeration.
	if (!Component)
	{
		return TEXT("Component");
	}

	FString ComponentName = Component->GetName();
	ComponentName.RemoveFromEnd(TEXT("_GEN_VARIABLE"));
	ComponentName.RemoveFromEnd(TEXT("_C"));
	return ComponentName;
}

void APursuitCharAgent::AddComponentBindings(APawn* Pawn, bool bSensors, TArray<FComponentBinding>& OutBindings)
{
	OutBindings.Reset();
	if (!Pawn)
	{
		return;
	}

	TArray<TPair<UActorComponent*, FString>> Components;
	TMap<FString, int32> ComponentCounts;

	TInlineComponentArray<UActorComponent*> ActorComponents(Pawn);
	for (UActorComponent* Component : ActorComponents)
	{
		const UClass* InterfaceClass = bSensors ? UScholaSensor::StaticClass() : UScholaActuator::StaticClass();
		if (!IsValidScholaComponent(Component, InterfaceClass))
		{
			continue;
		}

		const FString BaseName = GetComponentBaseName(Component);
		Components.Emplace(Component, BaseName);
		++ComponentCounts.FindOrAdd(BaseName);
	}

	// Same name seen twice? "TargetSensor", "TargetSensor1", ... - deterministic, order
	// stable, and identical on the training and inference sides because both enumerate
	// the same actor's components in the same order.
	TMap<FString, int32> SeenCounts;
	for (const TPair<UActorComponent*, FString>& ComponentPair : Components)
	{
		const FString& BaseName = ComponentPair.Value;
		const int32 CurrentIndex = SeenCounts.FindOrAdd(BaseName)++;
		const int32 TotalCount = ComponentCounts.FindRef(BaseName);
		const FString Key = TotalCount == 1 ? BaseName : FString::Printf(TEXT("%s%d"), *BaseName, CurrentIndex);

		FComponentBinding Binding;
		Binding.Key = Key;
		Binding.Object = ComponentPair.Key;
		OutBindings.Add(Binding);
	}
}
