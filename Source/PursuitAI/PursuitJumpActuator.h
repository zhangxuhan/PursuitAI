// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "ActuatorInterface.h"
#include "Points/BoxPoint.h"
#include "Spaces/BoxSpace.h"
#include "Spaces/BoxSpaceDimension.h"
#include "PursuitJumpActuator.generated.h"

/**
 * The jump actuator: one continuous dimension in [-1, 1] where values at or above
 * JumpThreshold mean "jump now".
 *
 * Why a custom actuator rather than stretching the movement actuator to a Z axis:
 * AddMovementInput along Up fights the gravity integration inside
 * CharacterMovementComponent, while a real jump is LaunchCharacter with the movement
 * component's own JumpZVelocity - a discrete impulse, not a stick value. The policy
 * still sees a continuous Box space, so it can express intent (how hard it wants to
 * jump) and the actuator decides whether the world allows it (grounded + cooldown).
 *
 * Contract notes, same as every other Schola actuator:
 *   - The component's NAME is the dict key on both the trainer side and the exported
 *     ONNX tensor side (enumerated by APursuitCharAgent with one shared rule), so the
 *     component must keep the name "JumpInput" unless the export tooling changes too.
 *   - Adding this component changes the action space from {-1,1}^2 to
 *     {-1,1}^2 x {-1,1}; every checkpoint exported BEFORE it exists is incompatible
 *     with the new agent, and vice versa.
 */
UCLASS(BlueprintType, Blueprintable, ClassGroup = (Pursuit), meta = (BlueprintSpawnableComponent))
class PURSUITAI_API UPursuitJumpActuator : public UActorComponent, public IScholaActuator
{
	GENERATED_BODY()

public:
	UPursuitJumpActuator();

	/** Action values at or above this mean "jump". Below it (and below -threshold by symmetry of intent) means "stay grounded". */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Pursuit|Jump", meta = (ClampMin = "0.05", ClampMax = "1.0"))
	float JumpThreshold = 0.5f;

	/** Minimum seconds between jumps, so the policy cannot spam the impulse every step. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Pursuit|Jump", meta = (ClampMin = "0.0"))
	float JumpCooldownSeconds = 0.3f;

	/**
	 * Execution gate. FALSE = the direction dimension still exists and its value is
	 * still delivered here, but no impulse is ever produced.
	 *
	 * Why a gate rather than "delete the component" (2026-09-22, Stage 0 of the
	 * simple-to-city curriculum): the jump dimension IS part of the action contract.
	 * Removing the actuator changes the action space from 3 dims to 2, which silently
	 * invalidates every observation/action-dimension acceptance check and every
	 * exported policy. Stage 0 asks for "flat floor, static target, jump not executed"
	 * with the SAME 15D/3D interface, so the only correct implementation is to keep the
	 * space and stop the executor.
	 *
	 * Default TRUE: every existing level and training run is unchanged.
	 * Set by the environment each episode from APursuitCharEnv::bEnableAgentJump.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pursuit|Jump")
	bool bEnabled = true;

	/**
	 * Jumps that actually fired (TryJump succeeded), not jump requests. The environment
	 * differences this across an episode to report "jumps" in the validation summary -
	 * the only honest way to show the gate works: requests can stay high while this
	 * stays at zero.
	 */
	int32 GetJumpCount() const { return JumpCount; }

	// --- IScholaActuator ---
	virtual void GetActionSpace_Implementation(FInstancedStruct& OutActionSpace) const override;
	virtual void TakeAction_Implementation(const FInstancedStruct& InAction) override;

	/** Typed entry, mirroring UMovementInputActuator::TakeAction(FBoxPoint). */
	void TakeAction(const FBoxPoint& Action);

	/** Clear the cooldown clock (called by the environment on episode reset). */
	void ResetCooldown();

private:
	/** Attempt the impulse. Returns true if a jump actually fired (for logging). */
	bool TryJump();

	float LastJumpTime = -1.0e9f;

	/** Lifetime count of impulses that actually fired; read via GetJumpCount(). */
	int32 JumpCount = 0;
};
