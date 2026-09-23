// Copyright Epic Games, Inc. All Rights Reserved.

#include "PursuitJumpActuator.h"

#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "PursuitAI.h"
#include "Points/DictPoint.h"

UPursuitJumpActuator::UPursuitJumpActuator()
{
	// Actions arrive only while the owner ticks; the actuator itself does nothing per-frame.
	PrimaryComponentTick.bCanEverTick = false;
}

void UPursuitJumpActuator::GetActionSpace_Implementation(FInstancedStruct& OutActionSpace) const
{
	// One continuous dimension, the same [-1, 1] band the movement stick uses - the
	// trainer's policy head is built from this definition, so the bounds here are the
	// bounds the exported model was trained against.
	TArray<FBoxSpaceDimension> Dimensions;
	Dimensions.Add(FBoxSpaceDimension(-1.0f, 1.0f));

	FBoxSpace ActionSpace(Dimensions);
	OutActionSpace.InitializeAs<FBoxSpace>(ActionSpace);
}

void UPursuitJumpActuator::TakeAction_Implementation(const FInstancedStruct& InAction)
{
	if (const FBoxPoint* BoxAction = InAction.GetPtr<FBoxPoint>())
	{
		TakeAction(*BoxAction);
		return;
	}

	// Same tolerance as the movement actuator: a single-actuator policy may hand over
	// the point directly; anything else is a contract break worth a warning line.
	const FString ReceivedType = InAction.GetScriptStruct()
		? InAction.GetScriptStruct()->GetName()
		: TEXT("null");
	UE_LOG(LogTemp, Warning,
		TEXT("UPursuitJumpActuator::TakeAction: action is %s, not a BoxPoint - ignored - %s"),
		*ReceivedType, *GetName());
}

void UPursuitJumpActuator::TakeAction(const FBoxPoint& Action)
{
	// The gate. Checked BEFORE the value is read, so a disabled actuator is not merely
	// "ignoring the jump key" - it cannot reach TryJump at all, and therefore cannot
	// reach LaunchCharacter from any caller (trained policy or scripted reflex alike).
	// The action SPACE is untouched: GetActionSpace still reports one dimension, which
	// is what keeps a Stage 0 checkpoint shape-compatible with a Stage 1 one.
	if (!bEnabled)
	{
		return;
	}

	if (Action.Values.Num() < 1)
	{
		return;
	}

	if (Action.Values[0] >= JumpThreshold)
	{
		TryJump();
	}
}

bool UPursuitJumpActuator::TryJump()
{
	ACharacter* OwnerCharacter = Cast<ACharacter>(GetOwner());
	UCharacterMovementComponent* Movement = OwnerCharacter ? OwnerCharacter->GetCharacterMovement() : nullptr;
	if (!OwnerCharacter || !Movement)
	{
		return false;
	}

	const UWorld* World = GetWorld();
	const float Now = World ? World->GetTimeSeconds() : 0.0f;

	if (Now - LastJumpTime < JumpCooldownSeconds)
	{
		return false;
	}

	// Grounded only: jumping mid-air is a teleport-shaped exploit for a chase task
	// (double-jump across a wall), and CharacterMovement reports falling for anything
	// not standing on a surface, which is exactly the gate wanted here.
	if (!Movement->IsMovingOnGround())
	{
		return false;
	}

	LastJumpTime = Now;
	++JumpCount;
	// The Z impulse comes from the movement component's own JumpZVelocity, so tuning
	// jump height is one property on the agent, not a second number here.
	OwnerCharacter->LaunchCharacter(FVector(0.0f, 0.0f, Movement->JumpZVelocity), false, false);
	// Actual jumps are rare by construction (grounded + cooldown), so a per-jump log
	// is cheap and is the ground truth for "did the jump ever fire" questions.
	UE_LOG(LogPursuitAI, Log, TEXT("PursuitJumpActuator: %s jumped"), *OwnerCharacter->GetName());
	return true;
}

void UPursuitJumpActuator::ResetCooldown()
{
	LastJumpTime = -1.0e9f;
}
