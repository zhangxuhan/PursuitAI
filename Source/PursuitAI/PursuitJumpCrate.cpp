// Copyright Epic Games, Inc. All Rights Reserved.

#include "PursuitJumpCrate.h"

#include "PursuitAI.h"

#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInterface.h"
#include "UObject/ConstructorHelpers.h"

APursuitJumpCrate::APursuitJumpCrate()
{
	PrimaryActorTick.bCanEverTick = false;

	// Movable from the start, and that is not a detail: a static-mobility component cannot be
	// repositioned at runtime, and this actor is spawned mid-round every time the fixture is
	// rebuilt. A Static component that is moved logs a warning and then ignores the move.
	Mesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Crate"));
	SetRootComponent(Mesh);
	Mesh->SetMobility(EComponentMobility::Movable);

	// Assigned in the constructor rather than at spawn time. The constructor runs before the
	// component is registered, so the body is built around the real mesh and the real profile
	// from the outset - which is exactly what AStaticMeshActor could not offer.
	//
	// Found in the constructor via a static-path lookup, so a bare project with no generated
	// content still gets a crate. The engine's unit cube is 100 cm on a side.
	static ConstructorHelpers::FObjectFinder<UStaticMesh> CubeFinder(TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (CubeFinder.Succeeded())
	{
		Mesh->SetStaticMesh(CubeFinder.Object);
	}
	else
	{
		UE_LOG(LogPursuitAI, Warning,
			TEXT("PursuitJumpCrate: could not load /Engine/BasicShapes/Cube - the jump test ")
			TEXT("will have no obstacle and proves nothing."));
	}

	// The chaser's obstacle feeler and its height probe both trace ECC_WorldStatic, so this
	// has to block that channel. BlockAll covers it and is what the fixture means.
	Mesh->SetCollisionProfileName(TEXT("BlockAll"));
	Mesh->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);

	// Never cast shadows and never contribute to lighting: this thing exists for seven seconds
	// to answer one question, and there is no reason for it to cost a shadow pass or to change
	// how the plaza reads in a capture taken from the same run.
	Mesh->SetCastShadow(false);
	Mesh->bAffectDynamicIndirectLighting = false;
	Mesh->bAffectDistanceFieldLighting = false;
}

void APursuitJumpCrate::Configure(float InFootprint, float InHeight, float InGroundZ)
{
	if (!Mesh)
	{
		return;
	}

	// The engine cube is 100 cm, so the scale is the size over 100. Every component is given
	// an explicit value: leaving Z to scale(1) put the crate's top 10 cm above the number the
	// caller asked for, which is the kind of silent 10% that turns into "why does it clear a
	// 90 cm box but not a 100 cm one".
	const float Footprint = FMath::Max(InFootprint, 10.0f);
	const float Height = FMath::Max(InHeight, 10.0f);

	SetActorScale3D(FVector(Footprint / 100.0f, Footprint / 100.0f, Height / 100.0f));
	SetActorLocation(FVector(GetActorLocation().X, GetActorLocation().Y, InGroundZ + Height * 0.5f));

	// Last, not first: the query state is rebuilt from whatever the component holds now, and
	// doing it after the scale means the bounds it rebuilds around are final. Calling it before
	// leaves the body sized for the unscaled cube for one frame - long enough for the read-back
	// and the chaser's first probe to disagree with each other.
	Mesh->RecreatePhysicsState();
}
