// Copyright Epic Games, Inc. All Rights Reserved.

#include "PursuitTargetSensor.h"

#include "Engine/World.h"
#include "GameFramework/Pawn.h"

namespace
{
	/**
	 * Number of wall probes. Five bearings -90/-45/0/+45/+90 (owner-relative yaw) cover
	 * the whole forward hemisphere at 45-degree resolution, so a facade that is beside
	 * the agent (a situation the three-probe version could not see at all) is visible
	 * before the agent commits to the bearing.
	 *
	 * Raised from 3 on 2026-09-22 after the live dump showed the plaza presenting
	 * obstacles at bearings the 3-probe fan missed entirely.
	 */
	constexpr int32 NumWallProbes = 5;

	/**
	 * The observation is: local direction (2), distance (1), two speeds (2), then for
	 * each of the five probes a range reading (5) and a clearance reading (5) = 15.
	 */
	constexpr int32 NumSensorDimensions = 5 + NumWallProbes * 2;

	/** Probe directions relative to the owner's facing, in degrees of yaw. */
	constexpr float WallProbeAngleDegrees[NumWallProbes] = { -90.0f, -45.0f, 0.0f, 45.0f, 90.0f };

	/** First index of the range block; clearance block starts at RangeBase + NumWallProbes. */
	constexpr int32 RangeBase = 5;
	constexpr int32 ClearanceBase = RangeBase + NumWallProbes;

	/**
	 * Height that reads as "fully blocking" in the hop-ability channel. Matches the rig
	 * wall height (RigWallHeight = 400), i.e. the tallest obstacle either stage builds.
	 * The reading is 1 - height/this, so flat ground is 1.0 and a rig-height wall is 0.0;
	 * the ~90 cm jump apex lands at 0.775 and a 75 cm crate at 0.8125.
	 *
	 * Why this dimension exists at all (2026-09-22): the three range probes alone made
	 * a 75 cm crate and a 400 cm pillar read IDENTICALLY - the sphere sweep runs at
	 * capsule-centre height, so both produce a horizontal contact at the same distance.
	 * The greedy baseline resolves the ambiguity with a vertical line trace on the
	 * impact point and jumps anything under 100 cm; the policy had no such channel, so
	 * "steer around the crate" and "jump the crate" were indistinguishable inputs to
	 * two different optimal actions. That is a partially observable problem, and it is
	 * the most likely reason the 500k city runs could not learn to route at all.
	 */
	constexpr float HopBlockingHeightCm = 400.0f;

	/**
	 * Reading for a probe that hit nothing (clear to full range). Sits in the middle so
	 * "no obstacle" is neither "hop it" nor "go around" - the policy has the range
	 * probe to tell it the path is open.
	 */
	constexpr float ClearanceNothing = 0.5f;

	/**
	 * Speed that reads as one full unit when differenced over ONE step.
	 *
	 * The target speed is a difference of two positions divided by the step time. With a
	 * fixed frame rate that quotient is exact; with a variable one it is noisy in exactly
	 * the way the chase is not - the fraction is presentation, the direction of motion is
	 * the information. Normalising by SpeedScaleCm keeps both speeds on one scale.
	 */
	float NormaliseSpeed(float CentimetresPerSecond, float Scale)
	{
		const float SafeScale = FMath::Max(Scale, KINDA_SMALL_NUMBER);
		return FMath::Clamp(CentimetresPerSecond / SafeScale, -1.0f, 1.0f);
	}
}

UPursuitTargetSensor::UPursuitTargetSensor()
{
	// A pure data component: no transform of its own, nothing to draw, and no reason to
	// tick - it is read once per step by whoever needs the observation.
	PrimaryComponentTick.bCanEverTick = false;
}

void UPursuitTargetSensor::GetObservationSpace_Implementation(FInstancedStruct& OutObservationSpace) const
{
	FBoxSpace Space;
	for (int32 Dim = 0; Dim < NumSensorDimensions; ++Dim)
	{
		Space.Add(-1.0f, 1.0f);
	}
	OutObservationSpace.InitializeAs<FBoxSpace>(Space);
}

void UPursuitTargetSensor::CollectObservations_Implementation(FInstancedStruct& OutObservations)
{
	FBoxPoint Observation;
	Observation.Values.Init(0.0f, NumSensorDimensions);

	const APawn* Owner = Cast<APawn>(GetOwner());
	const AActor* Target = TargetActor.Get();

	if (!Owner || !Target)
	{
		// A sensor with nothing to sense still has to emit a well-formed point, or the
		// observation tensor silently mismatches the space the policy was trained on.
		OutObservations.InitializeAs<FBoxPoint>(Observation);
		return;
	}

	// --- direction, in the owner's local frame, projected to the ground plane ---
	const FVector ToTarget = Target->GetActorLocation() - Owner->GetActorLocation();
	FVector Horizontal = FVector(ToTarget.X, ToTarget.Y, 0.0f);
	const float Distance = Horizontal.Size();
	if (Distance > KINDA_SMALL_NUMBER)
	{
		Horizontal /= Distance;
	}
	else
	{
		// Standing on the target: the direction is undefined, so report "straight ahead"
		// rather than a NaN that would poison the whole tensor.
		Horizontal = Owner->GetActorForwardVector().GetSafeNormal2D();
		if (Horizontal.IsNearlyZero())
		{
			Horizontal = FVector::ForwardVector;
		}
	}

	const FTransform OwnerTransform = Owner->GetActorTransform();
	const float LocalForward = static_cast<float>(OwnerTransform.InverseTransformVectorNoScale(Horizontal).X);
	const float LocalRight = static_cast<float>(OwnerTransform.InverseTransformVectorNoScale(Horizontal).Y);

	Observation.Values[0] = FMath::Clamp(LocalForward, -1.0f, 1.0f);
	Observation.Values[1] = FMath::Clamp(LocalRight, -1.0f, 1.0f);

	// --- distance ---
	const float SafeDistanceScale = FMath::Max(DistanceScaleCm, KINDA_SMALL_NUMBER);
	Observation.Values[2] = FMath::Clamp(Distance / SafeDistanceScale, 0.0f, 1.0f);

	// --- own speed ---
	Observation.Values[3] = NormaliseSpeed(Owner->GetVelocity().Size(), SpeedScaleCm);

	// --- target speed, differenced between two collections ---
	const FVector TargetLocation = Target->GetActorLocation();
	Observation.Values[4] = CollectTargetSpeed(TargetLocation);
	LastTargetLocation = TargetLocation;
	bHasVelocityHistory = true;

	// --- wall probes: for each bearing, a range reading then a clearance reading ---
	// Range answers "is something there and how far"; clearance answers "can I jump it
	// or must I go around". Keeping the two blocks contiguous per bearing makes the OBS
	// dump readable: [r0..r4 | c0..c4].
	for (int32 Probe = 0; Probe < NumWallProbes; ++Probe)
	{
		float RangeFraction = 1.0f;
		float Clearance = ClearanceNothing;
		CollectWallProbe(Owner, WallProbeAngleDegrees[Probe], RangeFraction, Clearance);
		Observation.Values[RangeBase + Probe] = RangeFraction;
		Observation.Values[ClearanceBase + Probe] = Clearance;
	}

	OutObservations.InitializeAs<FBoxPoint>(Observation);
}

void UPursuitTargetSensor::ResetVelocityHistory()
{
	// The first collection after a reset must not report the teleport as a target speed
	// of several thousand cm/s. Drop the history; the next collection starts a new pair.
	bHasVelocityHistory = false;
	LastTargetLocation = FVector::ZeroVector;
}

void UPursuitTargetSensor::CollectWallProbe(const APawn* Owner, float AngleDegrees,
	float& OutRangeFraction, float& OutClearance) const
{
	OutRangeFraction = 1.0f;
	OutClearance = ClearanceNothing;

	const UWorld* World = GetWorld();
	if (!World || !Owner)
	{
		return;
	}

	// Probe directions live in the owner's yaw frame: the same frame the direction
	// components are reported in, so "wall ahead" and "target ahead" are directly
	// comparable by the policy. Ground plane only - facades are vertical, and a
	// pitched probe would read the floor.
	const FVector Heading = Owner->GetActorForwardVector().GetSafeNormal2D();
	if (Heading.IsNearlyZero())
	{
		return;
	}
	const FVector Direction = Heading.RotateAngleAxis(AngleDegrees, FVector::UpVector).GetSafeNormal2D();

	// Start just outside the capsule: the owner must never read itself.
	const FVector Start = Owner->GetActorLocation() + Direction * (WallProbeRadiusCm + 1.0f);
	const FVector End = Start + Direction * (WallProbeDistanceCm - WallProbeRadiusCm - 1.0f);

	// Same semantics as the env's ProbePath: sphere sweep on ECC_WorldStatic at capsule
	// radius, so what the policy sees here is exactly what blocks it there. Both agents
	// are ignored - the target must not read as a wall, and self is excluded by the start.
	FHitResult Hit;
	FCollisionQueryParams Params(SCENE_QUERY_STAT(PursuitWallProbe), false);
	Params.AddIgnoredActor(Owner);
	if (const AActor* Target = TargetActor.Get())
	{
		Params.AddIgnoredActor(Target);
	}

	const float Range = FMath::Max(WallProbeDistanceCm, KINDA_SMALL_NUMBER);
	if (!World->SweepSingleByChannel(Hit, Start, End, FQuat::Identity,
		ECC_WorldStatic, FCollisionShape::MakeSphere(WallProbeRadiusCm), Params))
	{
		return; // clear to full range: 1.0 by the initialiser, hop-ability 0.5
	}

	const float Centimetres = static_cast<float>(
		FVector::Dist(Start, Hit.ImpactPoint)) - WallProbeRadiusCm - 1.0f;
	OutRangeFraction = FMath::Clamp(Centimetres / Range, 0.0f, 1.0f);

	// How tall is the thing we just hit? A downward line trace above the blocker finds
	// its top. This is the same measurement the greedy baseline makes before deciding to
	// jump (SteerScriptedAgent), lifted into the observation so the policy can make that
	// decision itself instead of guessing from an ambiguous range reading.
	//
	// CRITICAL DETAIL (debugged 2026-09-22): the trace must run above a point that is
	// INSIDE the blocker, not at Hit.ImpactPoint. On a sphere sweep the impact point is
	// where the *sphere surface* touched the geometry - for a probe aimed at a facade
	// that point can sit on the near face at capsule height, and a vertical trace there
	// drops straight to the floor and reports "0 cm tall" for a 400 cm building. The
	// first implementation did exactly that and every blocker in the city stage read as
	// hoppable, which is a worse bug than having no channel at all.
	//
	// Push a full probe radius past the impact along the probe direction: that lands the
	// vertical trace inside the volume that stopped the sweep.
	const FVector IntoBlocker = Hit.ImpactPoint + Direction * (WallProbeRadiusCm + 2.0f);
	const FVector ImpactXY(IntoBlocker.X, IntoBlocker.Y, 0.0f);
	const FVector TopStart = ImpactXY + FVector(0.0f, 0.0f, Owner->GetActorLocation().Z + 300.0f);

	FHitResult TopHit;
	FCollisionQueryParams TopParams(SCENE_QUERY_STAT(PursuitWallProbeTop), false);
	TopParams.AddIgnoredActor(Owner);
	if (const AActor* Target = TargetActor.Get())
	{
		TopParams.AddIgnoredActor(Target);
	}

	const bool bTopFound = World->LineTraceSingleByChannel(
		TopHit, TopStart, TopStart - FVector(0.0f, 0.0f, 500.0f), ECC_WorldStatic, TopParams);
	if (!bTopFound)
	{
		// A blocker with no measurable top (should not happen for static geometry, but
		// be explicit): treat it as impassable rather than silently claiming "hop it".
		OutClearance = 0.0f;
		return;
	}

	const float BlockerTopHeight = static_cast<float>(
		TopHit.ImpactPoint.Z - Owner->GetActorLocation().Z);

	// Encode the measured height as a CONTINUOUS clearance reading rather than a hard
	// boolean. 1.0 means "no climb at all / flat ground", 0.0 means "taller than anything
	// in this level". The jump apex (~90 cm) therefore sits at 1 - 90/400 = 0.775, so a
	// policy can threshold anywhere near there and still get a gradient if it is slightly
	// off - a hard 0/1 edge gives no signal about how far over the line it is.
	//
	// HeightScaleCm is the height that reads as fully blocking. 400 matches the rig wall
	// height (RigWallHeight), i.e. the tallest thing the agent can be asked about.
	const float HeightScale = FMath::Max(HopBlockingHeightCm, KINDA_SMALL_NUMBER);
	OutClearance = FMath::Clamp(1.0f - BlockerTopHeight / HeightScale, 0.0f, 1.0f);
}

float UPursuitTargetSensor::CollectTargetSpeed(const FVector& TargetLocation) const
{
	const AActor* Target = TargetActor.Get();
	if (!bHasVelocityHistory || !Target)
	{
		return 0.0f;
	}

	const UWorld* World = GetWorld();
	const float DeltaSeconds = World ? World->GetDeltaSeconds() : 0.0f;
	if (DeltaSeconds <= KINDA_SMALL_NUMBER)
	{
		return 0.0f;
	}

	const float Centimetres = static_cast<float>(FVector::Dist(TargetLocation, LastTargetLocation));
	return NormaliseSpeed(Centimetres / DeltaSeconds, SpeedScaleCm);
}
