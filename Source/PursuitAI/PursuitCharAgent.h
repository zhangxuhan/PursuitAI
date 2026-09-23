// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Agent/AgentInterface.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "PursuitHeroAssets.h"
#include "PursuitCharAgent.generated.h"

class UAnimSequence;
class UNNEModelData;
class UNNEPolicy;
class USimpleStepper;
class UPursuitTargetSensor;
class UPursuitJumpActuator;
class UMovementInputActuator;
struct FInteractionDefinition;

/**
 * The v2 agent: an actual ACharacter whose movement goes through
 * CharacterMovementComponent, replacing v1's pure position arithmetic.
 *
 * The action chain is the one the user asked for, verbatim:
 *
 *   PPO action {-1,1}^3 (dict: CharMoveInput = x/y stick, JumpInput = jump intent)
 *     -> UMovementInputActuator (AddMovementInput along forward/right)
 *     -> UPursuitJumpActuator (LaunchCharacter impulse when grounded and off cooldown)
 *       -> CharacterMovementComponent (acceleration, friction, max speed)
 *         -> collision sweeps (walls, steps, slopes, obstacles)
 *           -> capsule location read back as the next observation
 *
 * Structure mirrors the official ScholaExamples Tag agent (AMD, v2.1.0): Define /
 * Observe / Act are built by enumerating the pawn's IScholaSensor / IScholaActuator
 * components with one shared key-building rule, so the dict keys the trainer sees and
 * the keys an exported ONNX model's tensors expect can never drift apart - the classic
 * silent train/deploy failure that layering exists to prevent.
 *
 * Two drivers share this one class:
 *   - Training: the environment forwards the trainer's action into Act().
 *   - Inference: with a ModelData set, Tick runs Observe -> Think -> Act locally
 *     through UNNEPolicy + USimpleStepper (TagAgent::StepPolicy, adapted).
 *   - Scripted (rule evader, greedy/random baselines): the environment calls
 *     ApplyScriptedMove() directly. One class, three drivers, one movement pipeline.
 */
UCLASS(BlueprintType, Blueprintable)
class PURSUITAI_API APursuitCharAgent : public ACharacter, public IAgent
{
	GENERATED_BODY()

public:
	APursuitCharAgent();

	// ---------------------------------------------------------------------
	// Inference. Only populated when this agent runs its own ONNX policy.
	// ---------------------------------------------------------------------

	/** The exported policy. Empty means "not a policy-driven agent" (scripted or trained). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pursuit|Inference")
	TObjectPtr<UNNEModelData> ModelData = nullptr;

	/** NNE runtime to execute on. NNERuntimeORTCpu is CPU; NNERuntimeORTDml uses DirectML. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pursuit|Inference")
	FString RuntimeName = TEXT("NNERuntimeORTCpu");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pursuit|Inference", meta = (ClampMin = "1"))
	int32 MaxStateSequenceLength = 1;

	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Category = "Pursuit|Inference")
	EAgentStatus AgentStatus = EAgentStatus::Stopped;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Pursuit|Inference")
	TObjectPtr<UNNEPolicy> Policy;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Pursuit|Inference")
	TObjectPtr<USimpleStepper> Stepper;

	// ---------------------------------------------------------------------
	// Movement tuning. Everything the chase needs, nothing it does not.
	// ---------------------------------------------------------------------

	/** Walk speed ceiling in cm/s. The action only scales *how hard* to push, not this. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Pursuit|Movement", meta = (ClampMin = "10"))
	float ChaseMaxSpeed = 600.0f;

	float GetChaseMaxSpeed() const { return ChaseMaxSpeed; }
	void SetMaxWalkSpeed(float NewMaxSpeed)
	{
		ChaseMaxSpeed = NewMaxSpeed;
		if (UCharacterMovementComponent* Movement = GetCharacterMovement())
		{
			Movement->MaxWalkSpeed = NewMaxSpeed;
		}
	}

	// Schola components, built here so a plain C++ spawn needs no Blueprint.
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Pursuit|Schola")
	TObjectPtr<UPursuitTargetSensor> TargetSensor;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Pursuit|Schola")
	TObjectPtr<UMovementInputActuator> MoveActuator;

	/** Discrete-impulse actuator behind the third action dimension (jump). Key = "JumpInput". */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Pursuit|Schola")
	TObjectPtr<UPursuitJumpActuator> JumpActuator;

	// --- IAgent ---
	virtual EAgentStatus GetStatus_Implementation() override;
	virtual void SetStatus_Implementation(EAgentStatus NewStatus) override;
	virtual void Define_Implementation(FInteractionDefinition& OutInteractionDefinition) override;
	virtual void Observe_Implementation(FInstancedStruct& OutObservations) override;
	virtual void Act_Implementation(const FInstancedStruct& InAction) override;

	/**
	 * Scripted driver entry point: one tick of movement input along a world direction.
	 *
	 * Used for the rule evader and the greedy/random baselines, and deliberately routed
	 * through the same AddMovementInput the actuator uses - a scripted baseline that
	 * teleported would not be a baseline *for this task*.
	 */
	void ApplyScriptedMove(const FVector& WorldDirection, float Scale = 1.0f);

	/**
	 * Late policy binding: when a ModelData is assigned after BeginPlay already ran
	 * (the env loads -PursuitCharModel= from disk and env/agent BeginPlay order is
	 * level-dependent), build the policy runtime here. Calling it earlier is a no-op.
	 */
	void EnsureInferenceRuntime();

	// ---------------------------------------------------------------------
	// Looks. Without a mesh the agent is an invisible capsule that still
	// chases - which reads as "the watch window is frozen" from the top-down
	// camera, verified today with a two-frame screenshot diff.
	// ---------------------------------------------------------------------

	/** Which rig this agent wears. Set before LoadVisuals(); chaser and evader can differ. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pursuit|Looks")
	EPursuitHeroModel HeroModel = EPursuitHeroModel::TinyHero;

	/** Flat colour for the untextured mannequin rig only; the pack rigs keep their PBR materials. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pursuit|Looks")
	FLinearColor Tint = FLinearColor::Red;

	/**
	 * Load the rig's mesh and locomotion clips (idle + run), tint if the rig wants it.
	 * Called by the agent's BeginPlay with defaults and again by the environment after
	 * it has set HeroModel per agent. Safe to call repeatedly.
	 */
	void LoadVisuals();

protected:
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;

private:
	struct FComponentBinding
	{
		FString Key;
		TWeakObjectPtr<UObject> Object;
	};

	// --- visuals (see LoadVisuals / UpdateLocomotionVisual) ---
	bool bVisualsLoaded = false;
	float RigRunClipSpeed = 520.0f;
	TWeakObjectPtr<UAnimSequence> RunClip;
	TWeakObjectPtr<UAnimSequence> IdleClip;
	TWeakObjectPtr<UAnimSequence> JumpClip;
	TWeakObjectPtr<UAnimSequence> CurrentClip;
	void UpdateLocomotionVisual();

	void SetupMovementDefaults();
	void RefreshComponentBindings();
	void InitializeInteractors();
	void InitializeRuntime();
	void StepPolicy();

	static FString GetComponentBaseName(const UActorComponent* Component);
	static void AddComponentBindings(APawn* Pawn, bool bSensors, TArray<FComponentBinding>& OutBindings);

	TArray<FComponentBinding> Sensors;
	TArray<FComponentBinding> Actuators;
};
