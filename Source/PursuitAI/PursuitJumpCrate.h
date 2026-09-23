// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"

#include "PursuitJumpCrate.generated.h"

class UStaticMeshComponent;

/**
 * The box that -PursuitJumpTest puts between the two pawns.
 *
 * It exists because AStaticMeshActor would not do it. That class ships a StaticMeshComponent
 * that is registered before anything has been assigned to it, and assigning a mesh afterwards
 * leaves the physics body built from nothing: the actor renders correctly, while every trace
 * through it passes straight through. There is no error and no warning - "the trace missed"
 * and "there is nothing there" are the same answer - and it cost a full debugging cycle here,
 * chasing a jump feature that was being handed a crate that did not exist as far as any query
 * was concerned.
 *
 * Creating the component in the constructor and assigning its mesh in OnConstruction, before
 * registration, sidesteps the whole class of problem. The collision profile is set on the
 * component class default so it is correct at the moment of registration rather than patched
 * afterwards.
 */
UCLASS()
class PURSUITAI_API APursuitJumpCrate : public AActor
{
	GENERATED_BODY()

public:
	APursuitJumpCrate();

	/**
	 * Sets the crate's size and puts it on the ground.
	 *
	 * @param InFootprint  X and Y size, in centimetres.
	 * @param InHeight     Z size, in centimetres - this is the number the chaser's jump
	 *                     decision measures, so it is the one that matters.
	 * @param InGroundZ    World Z of the surface the crate sits on.
	 */
	void Configure(float InFootprint, float InHeight, float InGroundZ);

	/** The collision component, so a caller can read the geometry it actually produced. */
	UStaticMeshComponent* GetMeshComponent() const { return Mesh; }

private:
	UPROPERTY(VisibleAnywhere, Category = "Pursuit|JumpTest")
	TObjectPtr<UStaticMeshComponent> Mesh;
};
