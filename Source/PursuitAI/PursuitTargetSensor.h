// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "SensorInterface.h"
#include "PursuitTargetSensor.generated.h"

class APawn;

/**
 * Observation sensor for the v2 character environment: where is the target, in MY frame.
 *
 * Produces a Box observation of fifteen dimensions, each in [-1, 1] by construction:
 *
 *   0  target direction, forward component   (owner-local, normalized)
 *   1  target direction, right component     (owner-local, normalized)
 *   2  horizontal distance to target / DistanceScaleCm
 *   3  own speed / SpeedScaleCm
 *   4  target speed / SpeedScaleCm           (differenced between two collections)
 *   5..9   wall probe RANGE for bearings -90/-45/0/+45/+90 deg (owner-relative yaw):
 *          1.0 = clear to WallProbeDistanceCm, smaller = an obstacle closer than that
 *  10..14  wall probe CLEARANCE for the same five bearings, in matching order:
 *          1 - blockerHeight/400 cm, clamped to [0,1]. Flat ground = 1.0, a 400 cm
 *          facade = 0.0. The ~90 cm jump apex sits at 0.775, so "jump it" vs
 *          "go around it" is a threshold the policy can place itself.
 *
 * Local-frame direction+distance is the official ScholaExamples Tag layout
 * (DirectionDistanceObserver) rather than v1's world-arena relative XYZ, and the reason
 * is portability: a policy that sees "target is 30 degrees to my right, 8 m ahead"
 * does not care which map it spawned on, while a policy that sees absolute arena
 * coordinates has the arena geometry baked into its weights. Porting the chase from
 * the training rig to the city stage must not change what the policy sees.
 *
 * WALL PROBES - the history, because each step was a measured correction:
 *
 * 1. 2026-09-22: five dimensions were not enough. The open arena chase needed only
 *    "where is the target", but on the city stage the direction vector says "straight
 *    ahead" and the first information about a facade was the wall-hit penalty after
 *    the collision. Three forward probes were added (same sphere-sweep semantics as
 *    the env's ProbePath, so what the policy sees is exactly what blocks it).
 *
 * 2. Same day: three RANGE probes are ambiguous in exactly the case the city stage
 *    presents - the sweep runs at capsule-centre height, so a 75 cm crate and a 400 cm
 *    facade return the SAME reading while requiring opposite actions (jump through vs
 *    steer around). Partial observability, and the likely reason 500k city steps
 *    produced no routing behaviour. A downward trace recovers the blocker's top - the
 *    same measurement the greedy baseline makes before jumping - exposed as the
 *    clearance block.
 *
 * 3. Same day, debugging the clearance channel: the trace was first run at
 *    Hit.ImpactPoint. A sphere sweep reports where the SPHERE SURFACE touched, which
 *    for a facade is the near face at capsule height - a vertical trace from there
 *    drops to the floor and reports "0 cm tall". Every blocker therefore read as
 *    hoppable, which is worse than no channel because it teaches the wrong action.
 *    Fixed by pushing a probe radius past the impact, landing the trace inside the
 *    blocker.
 *
 * 4. Same day, live dump of 13 greedy episodes: 5 read 3/3 probes clear and every
 *    non-clear reading was 0-31 cm of ground detail (kerbs, planters). The 600 cm
 *    probes never saw a facade because a spawn 170-430 cm from the plaza centre leaves
 *    the nearest building outside the cone. The stage was an empty room, straight-line
 *    pursuit was optimal, and that is exactly what the policy learned. Fixed on two
 *    fronts: the probe fan went to five bearings (a facade beside the agent was
 *    invisible at three) with the range extended to 800 cm, and the stage now spawns
 *    full-height pillars so routing is actually required (see SpawnCityPillars).
 *
 * The target's Z is deliberately dropped from the direction components (projected to
 * the horizontal plane). The chase is a ground pursuit; feeding pitch would ask the
 * policy to learn that one dimension never pays.
 */
UCLASS(ClassGroup = (Pursuit), meta = (BlueprintSpawnableComponent))
class PURSUITAI_API UPursuitTargetSensor : public UActorComponent, public IScholaSensor
{
	GENERATED_BODY()

public:
	UPursuitTargetSensor();

	/**
	 * Who we are chasing. Set by the environment after BeginPlay (the env knows the
	 * pairing; the sensor only knows "that actor over there"), so an agent BP stays
	 * reusable as chaser or evader.
	 */
	TWeakObjectPtr<AActor> TargetActor;

	/**
	 * Distance in cm that maps to a sensor reading of exactly 1.0.
	 *
	 * Lowered 2400 -> 600 on 2026-09-22 (v3.6_facefix), after measuring the dynamic
	 * range this channel actually gets in a real episode.
	 *
	 * 2400 was chosen for v1's 1200 cm arena where the pair could start a full diameter
	 * apart. The city-stage chase never does: spawn gap is 154-250 cm and the catch
	 * fires at 120 cm, so the ENTIRE pursuit moves this reading from 0.090 to 0.050 -
	 * a 0.04 swing in a channel the policy shares with four wall probes that span the
	 * full 0..1. The distance channel was carrying ~4% of the information it could.
	 *
	 * At 600 cm: spawn reads 0.36, catch reads 0.20, so the same pursuit swings 0.16 -
	 * four times the signal for no contract change. 600 also stays clear of saturation:
	 * a 994 cm spawn (the old pre-v3.3 fallback pathology) clamps at 1.0, but anything
	 * past 600 cm deserves "close the gap as hard as possible" anyway, so the flat top
	 * is harmless in the direction that matters.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Pursuit|Sensor")
	float DistanceScaleCm = 600.0f;

	/** Speed in cm/s that maps to a reading of exactly 1.0. Defaults to the chase speed. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Pursuit|Sensor")
	float SpeedScaleCm = 600.0f;

	/**
	 * Length of each wall probe, in cm. A reading of 1.0 means "clear to this range";
	 * anything smaller is the fraction of the range the obstacle sits at.
	 *
	 * Raised 600 -> 800 on 2026-09-22: a spawn lands 170-430 cm from the plaza centre,
	 * and at 600 cm the nearest facade still fell outside the forward cone often enough
	 * that the stage read as an empty room. 800 cm is ~1.9 chase-seconds at top speed -
	 * the lead time needed to commit to a detour rather than a collision.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Pursuit|Sensor")
	float WallProbeDistanceCm = 800.0f;

	/** Sphere radius swept along each wall probe; mirrors the env's ProbePath radius. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Pursuit|Sensor")
	float WallProbeRadiusCm = 34.0f;

	virtual void GetObservationSpace_Implementation(FInstancedStruct& OutObservationSpace) const override;
	virtual void CollectObservations_Implementation(FInstancedStruct& OutObservations) override;

	/** Clears the differenced-velocity history. The env calls this on every episode reset. */
	void ResetVelocityHistory();

private:
	/** Target location at the previous collection, for the differenced target speed. */
	FVector LastTargetLocation = FVector::ZeroVector;

	bool bHasVelocityHistory = false;

	/** One speed reading: cm/s of the target between this collection and the last one. */
	float CollectTargetSpeed(const FVector& TargetLocation) const;

	/**
	 * One wall probe: sphere-swept ray from the owner along AngleDegrees (owner-relative
	 * yaw). Writes two readings:
	 *   OutRangeFraction - clear-fraction of the range in [0, 1]; 1.0 = nothing found.
	 *   OutClearance    - 0.5 when nothing was hit, otherwise 1 - height/400 of the
	 *                     blocker (1.0 = flat, 0.0 = 400 cm or taller). The caller
	 *                     applies whatever jump threshold it wants.
	 * Stateless by design; both outputs are always written.
	 */
	void CollectWallProbe(const APawn* Owner, float AngleDegrees,
		float& OutRangeFraction, float& OutClearance) const;
};
