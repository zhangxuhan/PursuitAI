// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "TrainingUtils/GymConnectorManager.h"
#include "Environment/SingleAgentEnvironmentInterface.h"
#include "PursuitCharEnv.generated.h"

class APursuitCharAgent;
class UStaticMeshComponent;
class ACameraActor;
class AStaticMeshActor;
struct FBoxPoint;
struct FInteractionDefinition;
struct FHitResult;

/**
 * Who drives the chaser when no trainer is attached (watched / demo runs).
 *
 *   Train     gRPC -> Python (SB3 PPO). The connector manager owns the tick.
 *   Greedy    built-in scripted policy: straight at the target. Shows what PPO should
 *             converge to, through the same movement pipeline as the real policy.
 *   Random    uniform direction at full stick every step. The honest "no information"
 *             baseline; same argument as v1's -PursuitRandom.
 *   Inference the agent's own ONNX policy (its ModelData must be set).
 */
UENUM(BlueprintType)
enum class EPursuitCharDriveMode : uint8
{
	Train,
	Greedy,
	Random,
	Inference,
};

/**
 * The v2 training environment: one PPO-controlled chaser character, one scripted evader
 * character, real CharacterMovement under both.
 *
 * Structure follows APursuitAIEnv (v1): one actor implements both
 * ISingleAgentScholaEnvironment (training side, driven over gRPC) and the watched-run
 * lifecycle, and both drivers share one world update so a policy can never be trained
 * against one world and evaluated against another. The differences from v1 are the
 * point: the state is two capsules moving through CharacterMovementComponent, and the
 * agent class itself is the Schola agent (spaces enumerated from its components).
 */
UCLASS(BlueprintType, Blueprintable)
class PURSUITAI_API APursuitCharEnv : public AGymConnectorManager,
                                      public ISingleAgentScholaEnvironment
{
	GENERATED_BODY()

public:
	APursuitCharEnv();

	// --- the cast. Wired up by the generated training level (or a city demo level). ---
	UPROPERTY(EditInstanceOnly, BlueprintReadWrite, Category = "Pursuit|Agents")
	TObjectPtr<APursuitCharAgent> ChaserAgent;

	/** May be null: a null evader is the Static-target task (same switch as v1's -PursuitStatic). */
	UPROPERTY(EditInstanceOnly, BlueprintReadWrite, Category = "Pursuit|Agents")
	TObjectPtr<APursuitCharAgent> EvaderAgent;

	/**
	 * Optional second chaser for watched runs (user request 2026-09-21: "再加一个追逐者，
	 * 用项目内的士兵"). Spawned and wired by the -PursuitCharEnvBox game-mode host only.
	 * Strictly cosmetic: never scored, never observed, catch stays dog-vs-hero - so the
	 * training task, reward and observation contract are untouched. Wears RPGHero and
	 * chases the evader with the same greedy steering + obstacle reflex as the dog.
	 */
	UPROPERTY(EditInstanceOnly, BlueprintReadWrite, Category = "Pursuit|Agents")
	TObjectPtr<APursuitCharAgent> SupportAgent;

	/**
	 * Set by the -PursuitCharEnvBox game-mode host right before FinishSpawning: this
	 * env was spawned by a WATCHED run, not by the Schola training connector. A hosted
	 * run that forgot its drive-mode flag used to stay at the Train default, whose Tick
	 * is a no-op outside training - no camera, no episodes, the user keeps the raw
	 * pawn view and reads that as "nothing spawned" (2026-09-21 watch report). Hosted
	 * runs fall back to the greedy baseline instead.
	 */
	bool bHostedWatchRun = false;

	// --- task geometry. The env actor's location IS the arena centre, so moving the
	// arena means moving one actor - same centre-relative argument as v1. ---
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pursuit|Arena")
	float ArenaRadius = 1200.0f;

	/** How far above the env actor's Z the capsules spawn (half capsule height + margin). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pursuit|Arena")
	float SpawnHeight = 95.0f;

	/**
	 * Build the training rig (floor + boundary ring) in C++ at BeginPlay.
	 *
	 * The rig lives here rather than in the generated level for a measured reason: the
	 * editor-python commandlet crashes on spawn_actor_from_object for static meshes in
	 * this 5.7 install, so the level script cannot place meshes itself. The env already
	 * owned its stage visuals in v1; v2 keeps that responsibility and gains a boundary
	 * the wall-hit reward can rely on. Meshes are engine basic shapes - nothing to import.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pursuit|Arena")
	bool bBuildArenaRig = false;

	/**
	 * On the city stage, keep the rig FLOOR (and its lights) but none of the greybox
	 * walls/obstacles.
	 *
	 * Why this exists (2026-09-22, the fix for the "learning signal is wrong" report):
	 * bCityStage used to hard-force bBuildArenaRig = false on the theory that "the city
	 * ground is the floor". tools/inspect_plaza_flatness.py measured that disc and the
	 * theory is false - around the (-250, 0) plaza, ground=10 holds only out to a 200 cm
	 * radius; at 300 cm it is 18% other heights, at 400 cm 30%, and the spawn disc was
	 * scattering capsules over steps of 10 / 100 / 120 / 148 cm. Every scored episode
	 * ran with the chaser in `Falling`, walk speed never engaging, Z sawtoothing 78..281.
	 *
	 * With this on, the stage gets ONE flat collidable plane at the env's own Z, so every
	 * spawn lands on the same surface, while the city still supplies the backdrop and the
	 * buildings as hit-able obstacles. It is deliberately separate from bBuildArenaRig so
	 * the city keeps NO boundary ring and NO greybox obstacle table - those would sit on
	 * top of real streets.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pursuit|Arena")
	bool bCityRigFloor = false;

	/** One side of the square floor the rig spawns, in cm (BasicShapes/Plane is 100 cm). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pursuit|Arena")
	float RigFloorScale = 40.0f;

	/** Boundary ring height in cm. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pursuit|Arena")
	float RigWallHeight = 400.0f;

	// --- task rules ---
	/**
	 * Horizontal catch radius. Raised 85 -> 120 on 2026-09-22 (v3.1_reachable).
	 *
	 * 85 cm is a fair contact radius for a 34 cm capsule, but it is a keyhole for a
	 * policy that has never once reached the target: 0 catches in 47 episodes across
	 * 400k steps. The point of this run is to prove the task is REACHABLE at all, so
	 * the window is widened to 120 cm - still a genuine contact (3.5x the capsule
	 * radius, not a proximity trophy) but wide enough that a clumsy first approach
	 * scores. Tighten back to ~85 once CAUGHT is reliably nonzero, which turns this
	 * number into a difficulty dial rather than a reachability blocker.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pursuit|Rules", meta = (ClampMin = "10"))
	float CatchRadius = 120.0f;

	/** Vertical tolerance for the catch: |chaser Z - evader Z| must also pass this. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pursuit|Rules")
	float CatchHeightTolerance = 70.0f;

	/**
	 * Display/compat only - NOT a timeout anymore. At the trainer's frame rate a
	 * 2000-step cap fired at 4.6 sim-seconds and defeated the EpisodeSeconds wall
	 * (the v2 retrain still read mean_steps = 2000). Do not reintroduce it into the
	 * timeout condition without solving the frame-rate dependence first.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pursuit|Rules", meta = (ClampMin = "1"))
	int32 MaxSteps = 2000;

	/**
	 * Episode wall, in SIMULATED seconds, and the ONLY timeout (see MaxSteps comment).
	 * Movement is dt-driven (CharacterMovement), so a step-count wall makes episode
	 * duration depend on the frame rate: at the trainer's ~430 fps, 2000 steps was
	 * only 4.6 sim-seconds - a gap the 90 cm/s closing speed cannot cover, so
	 * CatchReward was physically unreachable and every training episode timed out
	 * (measured: mean_steps = 2000 over 500k, twice).
	 *
	 * LOWERED 15 -> 6 on 2026-09-22 (v3.2_density) to buy SAMPLE COUNT, not difficulty.
	 *
	 * Measured problem: at ~8200 steps/episode a 50k run produced only SIX training
	 * episodes. `0/6` cannot distinguish "unreachable" from "not learned yet", and
	 * per-episode d_end scattered 756-1575 cm. Every extra 50k steps bought six more
	 * samples, so the run budget was being spent on wall-clock rather than information.
	 *
	 * Why 6 is enough to be reachable: with EvaderSpeedRatio at 0.70 the net closing
	 * rate is 172.5 cm/s (575 - 402.5), and MaxSpawnSeparation below caps the opening
	 * gap at 250 cm - a straight chase covers that in 1.45 s. 6 s leaves ~4x headroom
	 * for the policy's own exploration and for the evader's dodging, while cutting
	 * steps/episode to roughly 3300 so the same 50k run yields ~3x more episodes.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pursuit|Rules", meta = (ClampMin = "1.0"))
	float EpisodeSeconds = 6.0f;

	/**
	 * Floor on the spawn separation. The rig's 800 cm floor exists to stop a one-step
	 * catch lookalike; on the city plaza it is clamped down to 0.8 * CityStageRadius
	 * (400 cm) inside BeginEpisode.
	 *
	 * LOWERED 800 -> 150 on 2026-09-22 (v3.2_density). This is NOT cosmetic: the clamp in
	 * BeginEpisode computes
	 *     RequiredSeparation = min(MinSpawnSeparation, CityStageRadius * 0.8)   // 400
	 *     SeparationCap      = max(MaxSpawnSeparation, RequiredSeparation + 1)  // 401
	 * so with MinSpawnSeparation at 800 the cap floor was 401 and any MaxSpawnSeparation
	 * below that was silently raised back to 401 - the 250 cm cap would have done nothing
	 * at all. Exactly the same trap the 700 cm cap fell into earlier. At 150:
	 *     RequiredSeparation = min(150, 400) = 150
	 *     SeparationCap      = max(250, 151) = 250   <- the cap now actually applies
	 * 150 cm is still far outside the 120 cm catch radius, so the one-step-catch
	 * lookalike this floor guards against cannot happen.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pursuit|Rules")
	float MinSpawnSeparation = 150.0f;

	/**
	 * Upper bound on the spawn separation, 0 = no cap. Added 2026-09-22: with a 500 cm
	 * city plaza the draw could place the pair up to ~1000 cm apart, and at the 90 cm/s
	 * closing speed that needs 10+ seconds of flawless pursuit inside a 15 s episode -
	 * reachable for the greedy baseline, never reachable for a policy that has to
	 * discover pursuit by trial and error. Capping the draw keeps the chase honest while
	 * making the catch reward actually attainable, which is the entire point of a
	 * sparse terminal reward.
	 *
	 * Lowered 700 -> 450 on 2026-09-22 during the third fix. The 700 cap had TWO
	 * problems: (a) it sat BELOW MinSpawnSeparation, so the clamp in BeginEpisode
	 * silently widened it back to 801 every time and the cap never actually applied -
	 * so the intended short chase never happened; and (b) even at 700 the closing cost
	 * is real: at a measured 587 steps/sim-s the chaser's 575 cm/s is only 0.98 cm per
	 * step, and with StopDistanceResolution (below) the net closing rate against an
	 * 0.85x evader is ~57 cm/s, so 700 cm needs 12.3 of the 15 s against an evader that
	 * actively dodges. 450 cm needs ~7.9 s, which leaves room to recover from a bad
	 * opening. Keep this comfortably ABOVE MinSpawnSeparation - the clamp handles it,
	 * but a cap under the floor means neither number is doing what it says.
	 */
	/**
	 * Lowered 450 -> 250 on 2026-09-22 (v3.2_density), tracking the shorter 6 s wall.
	 *
	 * The cap exists so the opening gap cannot exceed what the episode can close. With
	 * the net closing rate at 172.5 cm/s, 250 cm needs 1.45 s of clean pursuit inside a
	 * 6 s episode - comfortable even for a policy that wastes most of its steps.
	 *
	 * This cap only takes effect once MinSpawnSeparation is ALSO below it: BeginEpisode
	 * raises the cap to RequiredSeparation + 1, and RequiredSeparation is
	 * min(MinSpawnSeparation, 0.8 * CityStageRadius) = min(150, 400) = 150 here, so the
	 * effective ceiling is max(250, 151) = 250. See the MinSpawnSeparation comment.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pursuit|Rules", meta = (ClampMin = "0.0"))
	float MaxSpawnSeparation = 250.0f;

	/**
	 * Evader top speed as a fraction of the chaser's. Lowered 0.85 -> 0.70 on
	 * 2026-09-22 (v3.1_reachable) to buy the net closing rate the task needs.
	 *
	 * Equal speeds mean a straight-line runner is literally uncatchable in the open
	 * field (verified: flee runs time out at 1.0), so the ratio has to hand the chaser
	 * a learnable edge. At 0.85 the edge was 0.15 * 575 = 86 cm/s, and measured over
	 * 47 episodes that was never enough: the pair spawns 400-450 cm apart and the
	 * chase must also absorb the policy's own wander, so the catch stayed unreachable
	 * (0/47). At 0.70 the edge doubles to 172 cm/s, which closes the 450 cm opening in
	 * 2.6 s of clean pursuit - fast enough for the reward to be discovered by
	 * exploration, while the evader is still 30% slower rather than trivial.
	 *
	 * Tighten back toward 0.85 (or beyond) only once CAUGHT is reliably nonzero.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pursuit|Rules", meta = (ClampMin = "0.1", ClampMax = "1.0"))
	float EvaderSpeedRatio = 0.70f;

	// --- reward. Both per-step penalties are SCALED BY dt (normalized to 60 fps) in
	// ScoreStep: movement per step is proportional to dt, so a fixed per-step penalty
	// makes the reward rate depend on the frame rate (at the trainer's 430 fps the
	// shaping signal was ~1 cm/step and drowned next to the fixed penalty; measured:
	// explained_variance ~ 0, clip_fraction 0 - the advantage signal was noise).
	// Scaling keeps the 60 fps semantics below at ANY frame rate. The dense shaping
	// is already per-cm, hence frame-rate-invariant. ---
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pursuit|Reward")
	float CatchReward = 10.0f;

	/**
	 * Reward per cm of distance closed this step. Dense shaping, v1-style.
	 *
	 * Raised 4x from 0.005 on 2026-09-22 after measuring why two 500k runs never
	 * learned to chase: at the trainer's ~533 fps the closing rate is ~90 cm/s (chaser
	 * 600, evader 0.85*600), i.e. 0.17 cm per step, so 0.005/cm paid +0.00085 per step
	 * against a -0.00056 per-step existence penalty - a full episode of flawless
	 * pursuit netted about +4 against a flat -4.5, which is not a gradient, it is noise.
	 * At 0.02/cm the same pursuit pays ~+16 and dominates the penalty by 3-4x.
	 *
	 * LOWERED 0.02 -> 0.005 on 2026-09-22 (v3.5_signal) AFTER DECOMPOSING THE ACTUAL
	 * EPISODE TOTALS. This term was never the "3-4x nudge" the note above assumed - it
	 * was the ENTIRE episode penalty, and its sign was pointing the wrong way.
	 *
	 * Measured decomposition of the v3.4 median episode (total -16.3):
	 *
	 *     shaping (d 212 -> 937) : -14.5    <- 89% of the whole penalty
	 *     StepPenalty (6 s)      :  -1.8
	 *     wall hits (median 8)   :  -0.2
	 *
	 * The earlier budget sketch wrongly used the STEP COUNT (~3400) where the code
	 * actually accumulates sum(dt*60) = 360 over a 6 s episode, so StepPenalty is a
	 * minor -1.8, not -17.0. That mis-read hid the real culprit for a whole round.
	 *
	 * Why 0.02 is too strong here: because the shaping telescopes to (start - end)*scale,
	 * its magnitude is set by the DISTANCE SCALE (up to 2000 cm), not by the episode
	 * length. At 0.02 a full-length flee pays -36 while a catch pays only +1.8 + 10.
	 * So the policy maximises return by WIDENING the gap - which is exactly the measured
	 * behaviour (d_end median 937 cm, second half REGRESSING 847 -> 1117).
	 *
	 * At 0.005 the same flee costs about -3.6 and the catch/flee spread becomes
	 * dominated by CatchReward, which is where it belongs. The term still supplies a
	 * dense gradient (it is now within an order of magnitude of StepPenalty instead of
	 * 8x above it), so it does its job without becoming the objective.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pursuit|Reward")
	float DistanceShapingScale = 0.005f;

	/**
	 * Proximity shaping: inside ProximityRadiusCm the chaser is paid a bonus that grows
	 * linearly as it closes, on top of the distance shaping above.
	 *
	 * Why it is needed: the distance shaping is potential-based, so it only pays for
	 * NET closure and says nothing about how close the pair actually is - the last few
	 * metres, where the evader's dodging matters most, had no gradient at all. This term
	 * is what turns "wander roughly towards the target" into "press the final approach".
	 * Scaled by dt*60 like the other per-step terms, so it is frame-rate-invariant.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pursuit|Reward", meta = (ClampMin = "0.0"))
	float ProximityBonus = 0.06f;

	/**
	 * Radius (cm) inside which ProximityBonus ramps from 0 (at the rim) to full (at
	 * contact). Widened 300 -> 450 on 2026-09-22 (v3.5_signal).
	 *
	 * Why: the spawn band is 154-248 cm (mean 212), so at a 300 cm radius the pair is
	 * ALREADY inside the bonus zone at step 1 and the ramp is nearly maxed - the term
	 * gave almost no gradient for the approach that matters, and measured v3.4 runs had
	 * the pair OUTSIDE 300 cm for essentially the whole episode (d_end median 937), so
	 * the bonus was paying nothing at all.
	 *
	 * At 450 cm the ramp spans the whole reachable chase: it is ~0.53 at the mean spawn
	 * distance and rises to 1.0 at contact, so every step of closing the gap is paid.
	 * This is the term that should carry the "press the final approach" gradient now
	 * that DistanceShapingScale has been reduced.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pursuit|Reward", meta = (ClampMin = "1.0"))
	float ProximityRadiusCm = 450.0f;

	/** Per-step penalty at 60 fps; scaled by dt*60 in ScoreStep (see reward block note). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pursuit|Reward")
	float StepPenalty = -0.005f;

	/** Per-hit penalty at 60 fps; scaled by dt*60 in ScoreStep (see reward block note). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pursuit|Reward")
	float WallHitPenalty = -0.02f;

	/**
	 * Per-step penalty (at 60 fps) while the chaser is NOT on the ground.
	 *
	 * Added 2026-09-22 (v3.7_airborne) after the jump log exposed what 44/44
	 * "ended farther than spawn" episodes were actually doing:
	 *
	 *     PursuitJumpActuator: PursuitCharAgent_0 jumped    <- 319 times, 47 episodes
	 *     PursuitJumpActuator: PursuitCharAgent_1 jumped    <- 256 times
	 *
	 * MEASURED INEFFECTIVE ON ITS OWN (v3.7, 150k steps): jumps stayed at 6.7 per
	 * episode and reward dropped from ~-6 to ~-13, i.e. the penalty WAS charged but the
	 * behaviour did not move. The reason is a design error in this term USED ALONE: a
	 * penalty applied to EVERY step the agent is airborne is charged identically for
	 * "jump" and "don't jump" as soon as the agent has become a perpetual hopper, so the
	 * advantage baseline absorbs it and no gradient survives. It punished the symptom,
	 * not the decision.
	 *
	 * RE-ENABLED 2026-09-22 (v3.11) as the CONSEQUENCE half of a two-part jump cost, not
	 * as a replacement for the action charge. Why both are needed: JumpActionPenalty
	 * taxes the DECISION and is what actually changes behaviour, but a policy that
	 * requests a single step per flight only pays for that one step. This term taxes the
	 * OUTCOME, so every step spent falling is charged no matter how briefly the key was
	 * pressed. Together they close the pulse loophole from both ends; either one alone
	 * leaves a cheap way to hop.
	 *
	 * This is NOT the v3.7 mistake repeated. v3.7 ran this ALONE against a policy that was
	 * airborne essentially 100% of the time (grounded fraction 0.00), where the term was a
	 * constant and the baseline absorbed it. Now the agent CAN stand - the rig floor added
	 * in v3.11 fixed that, chaser Z no longer crosses the ground plane - so "airborne" and
	 * "grounded" are genuinely different states and the term carries real information.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pursuit|Reward")
	float AirbornePenalty = -0.05f;

	/**
	 * Penalty charged per step the policy ASKS to jump (JumpInput >= the actuator's
	 * threshold), at 60 fps.
	 *
	 * This is the term that works, and the difference from AirbornePenalty is the
	 * whole point. Measured facts that forced it (2026-09-22, v3.8/v3.9 traces):
	 *
	 *   grounded fraction       : 0.00 - 0.01 across whole runs
	 *   chaser Z                : oscillates 87..264 cm over ground Z = 70
	 *   mean speed              : 58-87 cm/s, against a 575 cm/s walk speed
	 *   mean dot(vel, target)   : -0.11, i.e. drifting AWAY when moving at all
	 *
	 * The chaser is not walking badly: it never walks. It holds JumpInput high, and
	 * because TryJump only needs one grounded frame to fire, every landing is
	 * immediately followed by another launch - a sawtooth between 87 and 264 cm in
	 * which MaxWalkSpeed never applies and only the default AirControl (0.05) shapes
	 * the path. That is why the gap opens in every episode regardless of policy.
	 *
	 * Charging the ACTION gives the two choices different returns, which is exactly
	 * what the baseline can no longer cancel out.
	 *
	 * REVISED 2026-09-22 (v3.11, from the ACTION PROBE added this round). The v3.10 run
	 * proved the plumbing works - the probe printed
	 *     ACTION PROBE step 1 shape=DictPoint [2 keys: JumpInput CharMoveInput] resolved=1
	 *     jump_requested=1
	 * so bJumpRequestedThisStep WAS being set and the penalty WAS being charged. The
	 * behaviour did not move because the ORIGINAL SIZE assumed the policy would hold the
	 * key down: -0.05/step only bites if the request spans the whole ~33-step flight. The
	 * policy learned to send a ONE OR TWO STEP pulse instead, which the actuator honours
	 * (TryJump needs a single grounded frame) while costing only -0.1 per jump. Measured
	 * v3.10: 11.7 jumps per episode at that price, i.e. roughly -1.2 per episode against
	 * a -2.0 timeout - cheaper than giving up jumping, so nothing changed.
	 *
	 * -0.5/step*scale makes the pulse strategy pay too: 11 pulses x 2 steps x -0.5 = -11
	 * per episode, five times the timeout penalty. Holding the key costs ~-180, so
	 * neither shape is survivable. This is the SIZE the design needed all along; the
	 * shape (-0.05) was right, the assumption about the policy was wrong.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pursuit|Reward")
	float JumpActionPenalty = -0.5f;

	/**
	 * Terminal penalty for running the clock out without a catch. Set to -2.0 on
	 * 2026-09-22 (v3.5_signal); history: -8.0 originally, 0.0 during v3.1-v3.4.
	 *
	 * Why it was added: with NO timeout cost, "escape and survive" scored the same as
	 * "catch", and the 400k run converged on exactly that - d_end grew to a 1300 cm
	 * median, 0 catches in 24 episodes.
	 *
	 * Why it was turned back off (v3.1_reachable): the penalty is a CONSOLIDATION tool,
	 * not a LAUNCH tool. Over the 400k v3_reward run - 47 episodes, 47 TIMEOUTS, 0
	 * catches - every episode ended in the -8 terminal, so the advantage estimate saw
	 * only a constant negative terminal signal and never a positive one, flattening the
	 * within-episode "closing vs fleeing" difference the shaping is meant to teach.
	 *
	 * Why a SMALL one comes back now: v3.3 proved CAUGHT is physically reachable
	 * (2/45 = 4.4%), so the failure mode has changed from "impossible" to "not worth
	 * attempting" - and leaving the terminal at exactly 0.0 makes "never engage" a
	 * stable optimum, because a policy that avoids the catch also avoids the -2. At
	 * -2.0 the term is big enough to price "ran the clock out" as a loss while staying
	 * small enough that it cannot dominate the +10 catch (the 400k failure mode).
	 *
	 * Deliberately NOT -8.0: that scale swamped the shaping signal while the policy was
	 * still discovering pursuit. If CAUGHT becomes reliably nonzero and the remaining
	 * problem is "closes the gap then dawdles", raise this toward -4.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pursuit|Reward")
	float TimeoutPenalty = -2.0f;

	// ---------------------------------------------------------------------
	// Stage 0 curriculum switches (2026-09-22)
	//
	// Why these are switches rather than "just generate a simpler level":
	// bBuildArenaRig was ONE flag covering two different difficulty layers - the flat
	// floor + boundary ring, and the 8-obstacle table - so the only way to get an
	// obstacle-free arena was to drop the boundary ring with it, which leaves the agent
	// nothing to be stopped by and makes "did it close the gap" unmeasurable. Splitting
	// the layers is what makes a curriculum possible at all: Stage 0 keeps the floor and
	// the ring and turns off only what Stage 0 is not testing yet.
	// ---------------------------------------------------------------------

	/**
	 * Greybox obstacle table (4 pillars, 2 low boxes, 2 low chokes) switch, INDEPENDENT
	 * of bBuildArenaRig.
	 *
	 * Default TRUE, so every existing rig level keeps its obstacles unchanged.
	 * L_PursuitCharCurriculum bakes FALSE: flat floor, boundary ring, nothing inside.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pursuit|Arena")
	bool bSpawnArenaObstacles = true;

	/**
	 * Jump execution switch: FALSE keeps the third action dimension in the space (see
	 * UPursuitJumpActuator::bEnabled) and simply never executes it - the value is still
	 * read, the reward still never charges it, and no impulse is ever produced.
	 *
	 * Default TRUE, so existing levels behave exactly as before.
	 * L_PursuitCharCurriculum bakes FALSE - Stage 0 is "flat, static, no internal
	 * obstacles", and a jump is exactly the behaviour that stage is meant to isolate.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pursuit|Rules")
	bool bEnableAgentJump = true;

	// ---------------------------------------------------------------------
	// Stage 3 curriculum: ONE tall pillar, and the only new skill is "route around it"
	// (2026-09-23, the environment-acceptance round that precedes any Stage 3 training).
	//
	// Why this is a switch plus a CONSTRUCTED spawn layout, rather than a mesh hand-placed
	// in a new level:
	//
	//   * A level-placed mesh is invisible to RigObstacleDiscs, so a capsule could
	//     materialise inside the pillar - exactly the "fake problem" this round has to
	//     exclude. Built here, the pillar is registered with the same avoidance disc the
	//     greybox obstacle table uses.
	//   * The two layouts this stage is built on - "the straight line to the target is
	//     blocked" and "the straight line is clear" - CANNOT be produced by the existing
	//     area-uniform spawn draw at all. With the 1200 cm spawn disc and a 250 cm
	//     separation cap, a drawn pair straddles a 300 cm central pillar in a few percent
	//     of episodes: that is a coincidence, not a controlled experiment, and it would
	//     leave the blocked group too small to say anything about.
	//   * Both layouts are therefore constructed, symmetric about the same separation and
	//     the same facing, and alternate by episode parity. The ONLY difference between
	//     them is whether the pillar sits on the chase line - so a difference in outcome
	//     is attributable to routing, not to two different tasks.
	//
	// Everything else this stage inherits is UNCHANGED on purpose: the 15D observation
	// (5 scalars + 5 probe ranges + 5 probe clearances), the 3D action space, jump
	// disabled, Moving050's EvaderSpeedRatio, every reward term, EpisodeSeconds, the catch
	// rules and the physics. A Stage 3 that is easier to pass because a reward term moved
	// would prove nothing about routing.
	// ---------------------------------------------------------------------

	/** Stage 3 layout switch. FALSE by default: every existing level keeps its exact geometry. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pursuit|Arena")
	bool bStage3PillarLayout = false;

	/** Pillar footprint side, cm (Cube basic shape is 100 cm, so scale = footprint / 100). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pursuit|Arena")
	float Stage3PillarFootprintCm = 300.0f;

	/** Pillar height, cm. 400 = the rig wall height = the height the clearance channel reads as fully blocking. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pursuit|Arena")
	float Stage3PillarHeightCm = 400.0f;

	/** Centre-to-centre separation of the constructed Stage 3 pair, cm. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pursuit|Arena")
	float Stage3SpawnSeparationCm = 600.0f;

	/**
	 * Lateral offset, cm, of the CLEAR layout's chase line from the pillar's axis. Has to
	 * clear the pillar's half-footprint plus a capsule radius with room to spare, and stay
	 * far enough inside the 1200 cm arena that the boundary ring is never the thing being
	 * tested: 600 puts the corridor 450 cm from the pillar face and ~600 cm from the wall.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pursuit|Arena")
	float Stage3ClearLateralOffsetCm = 600.0f;

	/**
	 * Spawn facing (yaw, degrees) of the chaser in BOTH Stage 3 layouts; 0 = facing +X,
	 * i.e. straight down the chase line. Deliberately fixed and logged rather than drawn:
	 * the whole point of the stage is that the pillar is (or is not) between the agent and
	 * the target it can see, so "where is the target" must not be the variable under test.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pursuit|Arena")
	float Stage3SpawnYawDegrees = 0.0f;

	/**
	 * -PursuitStage3ProbeSelfTest: run one deterministic probe sweep and write it to the
	 * log, then never again. CLI-only rather than baked, so the evidence run cannot be
	 * confused with the measurement runs: the eval and the oracle run this stage without
	 * it, and a level can be re-verified at any time without regenerating anything.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pursuit|Arena")
	bool bStage3ProbeSelfTest = false;

	/** The one Stage 3 pillar, kept so the wall-hit classifier can attribute hits to it. */
	UPROPERTY()
	TObjectPtr<UStaticMeshComponent> Stage3Pillar;

	/** True when the CURRENT episode uses the blocked layout (episode parity 0). Logged per episode. */
	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Category = "Pursuit|Arena")
	bool bStage3BlockedLayout = false;

	/** Chaser wall contacts with the Stage 3 pillar this episode - the stage's key metric. */
	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Category = "Pursuit|Arena")
	int32 Stage3PillarHitsThisEpisode = 0;

	/** Chaser wall contacts this episode with anything else, boundary ring included. */
	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Category = "Pursuit|Arena")
	int32 Stage3OtherWallHitsThisEpisode = 0;

	// =====================================================================
	// Stage 4A - single LOW WALL + jump (environment acceptance, 2026-09-23).
	//
	// Stage 3 asked "can the agent route AROUND a blocker?" with jump held OFF, so the
	// only solution was a detour, and the answer was yes (rep3 zero-shot, archived as a
	// fixed-layout PASS). Stage 4A asks the next question on the same arena: "can the
	// agent go OVER a blocker?" - is the jump action usable at all, and is the 15D
	// observation rich enough to tell a hop-able wall from an unhop-able one?
	//
	// Why the wall is NOT the 400 cm pillar. The RL chaser is an APursuitCharAgent, an
	// ACharacter with the ENGINE DEFAULT JumpZVelocity (420) and GravityScale (1.0), so
	// its jump apex is 420^2 / (2 * 980) = 90 cm. A 400 cm obstacle is outside the
	// action's reach by a factor of ~4.4, which would make "the policy never jumps the
	// pillar" a statement about the physics rather than about the policy - unfalsifiable
	// and useless. The wall therefore has to sit strictly between the STEP envelope
	// (CharacterMovement MaxStepHeight, 45 cm) and the JUMP envelope (~90 cm): low enough
	// to be inside the action's reach, high enough that walking up it is impossible.
	//
	// Two groups, differ by ONE translation - exactly the Stage 3 pattern:
	//
	//   must-jump - chaser and evader on OPPOSITE sides of the wall, both on the chase
	//               axis. The wall spans the FULL arena, so there is no lateral detour
	//               and jumping is the only way across. This is the group that tests the
	//               jump action.
	//   no-jump   - the same pair translated along X so BOTH agents are on the same side.
	//               Same wall, same separation, same facing, same target bearing, but the
	//               chase never has to cross it. This is the control that separates "the
	//               wall is the problem" from "the task just got harder".
	//
	// Everything else is inherited UNCHANGED: the 15D observation, the 3D action space,
	// Moving050's EvaderSpeedRatio, every reward term, EpisodeSeconds, the catch rules and
	// the physics. Jump is the ONE variable this stage turns on, and it is overridable
	// from the command line (-PursuitEnableJump=0) so the jump-disabled control runs the
	// very same level instead of a differently-baked one.
	// =====================================================================

	/** Stage 4 layout switch. FALSE by default: every existing level keeps its exact geometry. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pursuit|Arena")
	bool bStage4WallLayout = false;

	/**
	 * Wall height, cm. NOT a free knob: it must sit strictly between the step envelope and
	 * the jump envelope or the stage stops testing what it claims to. BuildStage4Wall reads
	 * MaxStepHeight / JumpZVelocity / GravityScale off the LIVE chaser and logs whether this
	 * height is inside that window, so "55 cm is jumpable and not walkable" is a measurement
	 * in the log rather than a claim in a comment.
	 *
	 * 55 rather than the obvious 70, and the difference is not cosmetic. The scripted driver's
	 * launch cadence is fixed by the actuator's own 0.3 s cooldown, so at ~575 cm/s it can only
	 * start a hop every ~172 cm; a height whose clearable launch window is NARROWER than that can
	 * be missed purely because of where the first launch happened to fall. At 70 cm the window is
	 * ~125 cm wide and the driver can miss it; at 55 it is ~199 cm and it cannot. The band is
	 * therefore swept empirically on the same level with -PursuitStage4WallHeight= and reported,
	 * instead of being baked once and believed.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pursuit|Arena")
	float Stage4WallHeightCm = 55.0f;

	/** Wall thickness along the chase axis, cm (the Cube basic shape is 100 cm). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pursuit|Arena")
	float Stage4WallThicknessCm = 40.0f;

	/**
	 * Wall length across the arena, cm. 0 = span the arena (2 * ArenaRadius + 200).
	 *
	 * That default is the only value that makes the must-jump layout actually must-jump: at
	 * any shorter length the ~575 cm/s chaser can walk around the wall's end well inside the
	 * 6 s episode, and the stage would silently degrade into a second Stage 3 (route around a
	 * blocker) while still being labelled "jump". A non-zero value is accepted for future
	 * experiments but logs the detour budget so the degradation is visible, not assumed.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pursuit|Arena")
	float Stage4WallLengthCm = 0.0f;

	/** Centre-to-centre separation of the constructed Stage 4 pair, cm. Kept at Stage 3's 600. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pursuit|Arena")
	float Stage4SpawnSeparationCm = 600.0f;

	/**
	 * X translation, cm, applied to BOTH agents for the no-jump control. -600 turns the
	 * must-jump pair (-300, +300) into (-900, -300): the separation, the facing and the
	 * target bearing are all unchanged, but the whole chase now lives on one side of the
	 * wall. Chosen so the translated chaser keeps a >100 cm margin from the boundary ring
	 * (the Stage 3 legality rule) while the evader still has room to flee.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pursuit|Arena")
	float Stage4ClearShiftCm = -600.0f;

	/**
	 * Spawn facing (yaw, degrees) of the chaser in BOTH Stage 4 layouts. Reuses the Stage 3
	 * field and its reasoning verbatim: the subject of this stage is the WALL, so "where is
	 * the target" must not be a variable under test, and a drawn facing would make it one.
	 */
	// (see Stage3SpawnYawDegrees above - intentionally shared)

	/**
	 * -PursuitStage4WallProbeSelfTest: one deterministic probe sweep against the wall,
	 * logged once per process. CLI-only for the same reason as the Stage 3 sweep: the
	 * evidence run has to be distinguishable from the runs it is evidence for, and a level
	 * must stay re-verifiable without regenerating it.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pursuit|Arena")
	bool bStage4ProbeSelfTest = false;

	/** The one Stage 4 wall, kept so contacts can be attributed and the geometry re-read. */
	UPROPERTY()
	TObjectPtr<UStaticMeshComponent> Stage4Wall;

	/** True when the CURRENT episode uses the must-jump layout (episode parity 0). Logged per episode. */
	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Category = "Pursuit|Arena")
	bool bStage4BlockedLayout = false;

	// --- Stage 4A per-episode jump / crossing diagnostics -------------------------------
	//
	// ALL of these are LOGGING ONLY. None is read by ScoreStep: JumpActionPenalty and
	// AirbornePenalty keep consuming the reward's own latches, untouched. They are reset on
	// the same episode boundary as the Stage 3B contact counters, because every one of them
	// is compared per episode between the two groups.

	/** Steps this episode whose action ASKED for a jump (intent - execution is `jumps`). */
	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Category = "Pursuit|Arena")
	int32 JumpRequestStepsThisEpisode = 0;

	/** Steps this episode the chaser was actually off the ground. */
	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Category = "Pursuit|Arena")
	int32 AirborneStepsThisEpisode = 0;

	/**
	 * Highest point the chaser's capsule BOTTOM reached above the floor this episode, cm.
	 *
	 * Read live off the capsule component's scaled half-height, never off SpawnHeight, so it
	 * is the height the jump ACTUALLY produced rather than the height the spawn code
	 * intended. This is the number that answers "what can this character really jump?" with
	 * evidence from the running game instead of an arithmetic derivation.
	 */
	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Category = "Pursuit|Arena")
	float MaxJumpApexCmThisEpisode = 0.0f;

	/** Times the chaser crossed the wall plane with its feet ABOVE the wall top (a real hop). */
	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Category = "Pursuit|Arena")
	int32 WallCrossOverCount = 0;

	/**
	 * Times the chaser crossed the wall plane with its feet BELOW the wall top.
	 *
	 * With a full-span wall this is geometrically impossible, so a non-zero value is evidence
	 * of a GAP in the level rather than of a clever policy - which is exactly why it is
	 * counted separately instead of being folded into one "crossings" number that would hide
	 * a broken level behind a healthy-looking total.
	 */
	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Category = "Pursuit|Arena")
	int32 WallCrossAroundCount = 0;

	/**
	 * Jump LAUNCHES fired while nothing was within the 300 cm forward path probe.
	 *
	 * Definition is deliberately mechanical so it can be recomputed from the log rather than
	 * argued about: a launch is "meaningless" when the driver jumped in open ground. A jump
	 * that was merely mistimed (wall ahead but too far away to clear) is NOT counted here and
	 * shows up as "jumps > 0, wall_cross_over = 0" instead.
	 */
	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Category = "Pursuit|Arena")
	int32 MeaninglessJumpCount = 0;

	/** Airborne -> grounded transitions this episode. */
	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Category = "Pursuit|Arena")
	int32 LandingCountThisEpisode = 0;

	/** Landings after which the distance to the target shrank within the next 30 steps. */
	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Category = "Pursuit|Arena")
	int32 ChaseResumedAfterLandingCount = 0;

	/** Times the EVADER crossed the wall plane at all (either height). Must stay 0. */
	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Category = "Pursuit|Arena")
	int32 EvaderWallCrossCount = 0;

	/** Evader jump count at episode start, so the summary can report its delta. */
	UPROPERTY()
	int32 EvaderJumpCountAtEpisodeStart = 0;

	// Stage 4A per-step scratch. Plain members, not UPROPERTY: they are frame-local state for
	// the classifier below and nothing outside this class may read them.
	bool bStage4WasAirborneLastStep = false;
	float Stage4PrevChaserX = 0.0f;
	float Stage4PrevEvaderX = 0.0f;
	float Stage4DistanceAtLastLandingCm = 0.0f;
	int32 Stage4LandingWindowRemaining = 0;
	bool Stage4LandingChaseResumed = false;
	int32 Stage4PrevJumpCount = 0;

	/**
	 * Settle gate. The pair is placed at SpawnHeight above the floor and free-falls the last
	 * few cm before its first grounded frame; without this gate that drop would be booked as
	 * air time, as the episode's jump apex AND as a landing - three false readings inside the
	 * first fraction of a second, all of which this stage reports. Every Stage 4A measurement
	 * therefore starts at the first grounded sample, and the skipped steps are logged rather
	 * than silently discarded, so the gate itself stays auditable.
	 */
	bool bStage4HasBeenGrounded = false;
	int32 Stage4SettleStepsSkipped = 0;

	/**
	 * Build the Stage 4 low wall (bStage4WallLayout only): one BlockAll cube, thickness along
	 * X, full arena length along Y, base on the floor, plus its spawn-avoidance discs.
	 *
	 * Called from BeginPlay after BuildArenaRig so the level stays a pure property bake - the
	 * same division of labour BuildStage3Pillar already uses. Unlike the pillar it logs the
	 * chaser's LIVE step and jump envelopes beside the wall height, which is what turns
	 * "70 cm" into an accepted measurement.
	 */
	void BuildStage4Wall();

	/**
	 * Construct the two controlled Stage 4 spawn pairs, in env-relative cm.
	 *
	 *   must-jump - chaser (-S/2, 0), evader (+S/2, 0): opposite sides of the wall, which
	 *               spans the arena, so the straight chase line is blocked and there is no
	 *               lateral detour. Jump is the only solution.
	 *   no-jump   - the same pair translated by Stage4ClearShiftCm on X: same separation,
	 *               same facing, same target bearing, but both agents are on the same side
	 *               and the wall is never crossed.
	 *
	 * Episode parity picks the group (even = must-jump), so a run of N episodes contains both
	 * groups by construction rather than by luck - identical to PlaceStage3SpawnPair.
	 */
	void PlaceStage4SpawnPair(FVector2D& OutChaserPoint, FVector2D& OutEvaderPoint);

	/**
	 * Classify one step of Stage 4A evidence: jump intent, actual airborne time, the peak the
	 * jump reached, whether the wall plane was crossed and at what height, meaningless
	 * launches, landings and whether pursuit resumed after each one.
	 *
	 * Called from AccumulateValidationSample, NOT from Step_Implementation: that is the one
	 * per-step hook BOTH the training path and the watched `-game` path hit, and putting a
	 * counter on the training-only side is the exact mistake that made the Stage 3B
	 * contact-duration counter read a hard zero in every watched run.
	 */
	void AccumulateStage4Sample(float DistanceCm);

	/** One-shot probe sweep proving the wall is OBSERVABLE and its TOP HEIGHT is readable. */
	void RunStage4WallProbeSelfTest();

	/** One-shot latch for the Stage 4 probe sweep, so a long run is not buried in sweeps. */
	bool bStage4ProbeSelfTestDone = false;

	// --- watched-run options ---
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Pursuit|Watch")
	EPursuitCharDriveMode DriveMode = EPursuitCharDriveMode::Train;

	/**
	 * ONNX file Inference mode loads into the chaser at BeginPlay (v1's -PursuitModel
	 * mechanism, carried over). Relative paths resolve against the project directory,
	 * so 0k/100k/500k models swap by command line, never by re-generating the level.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pursuit|Inference")
	FString InferenceModelPath = TEXT("checkpoints/policy.onnx");

	/**
	 * Straights-only scripted chase, selected by -PursuitCharStraight. It exists for ONE
	 * reason: Stage 3 needs the layout's blockage demonstrated, not asserted. The default
	 * Greedy driver already carries the avoidance reflection (ProbeOffsets in
	 * SteerScriptedAgent), which makes it the solvability ORACLE - it is the thing that
	 * has to succeed for "the layout is solvable" to hold. A layout every driver solves
	 * says nothing about whether the pillar is really in the way, so this flag strips the
	 * probe and the detour out and drives the raw bearing to the target: the naive
	 * straight chase that a policy without obstacle awareness degenerates to.
	 *
	 * Default false, and only read by the scripted driver - it changes no reward, no
	 * observation, no action dimension and no physics. A run with it set is a diagnostic,
	 * never a training configuration.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pursuit|Rules")
	bool bScriptedStraightChase = false;

	/**
	 * Watched-run presentation: a top-down camera over the arena, plus a label burned
	 * into each pane's own picture (two game windows share one title, so post-production
	 * cannot tell them apart). Only the watched modes set any of this up.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pursuit|Watch", meta = (ClampMin = "500"))
	float WatchCamHeight = 4500.0f;

	/**
	 * Watched camera tilt, degrees below the horizon. -90 is the old straight-down
	 * view, which flattens Z: a 90 cm jump reads as a two-pixel silhouette shrink.
	 * The default keeps WatchCamHeight of altitude but walks the camera back along
	 * -WatchCamYaw, so the view axis still passes through the arena centre at any
	 * tilt while jumps and obstacle heights gain visible depth. Set by
	 * -PursuitWatchPitch=.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pursuit|Watch", meta = (ClampMin = "-89.5", ClampMax = "-10.0"))
	float WatchCamPitch = -60.0f;

	/** Watched camera compass direction; 45 puts a rig arena's far corner upper-middle. Set by -PursuitWatchYaw=. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pursuit|Watch", meta = (ClampMin = "-180.0", ClampMax = "180.0"))
	float WatchCamYaw = 50.0f;

	/** Watched camera FOV; narrower than the engine's 90 so the tilted view reads less fisheye. Set by -PursuitWatchFOV=. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pursuit|Watch", meta = (ClampMin = "20.0", ClampMax = "120.0"))
	float WatchCamFOV = 65.0f;

	/**
	 * -PursuitWatchAt=X,Y,Z: absolute camera position override. When set, the back-off
	 * math above is skipped entirely - the camera parks at these world coordinates and
	 * rotates to look at the arena centre. Zero vector + bHasWatchCamOverride=false is
	 * the default derived placement.
	 */
	FVector WatchCamAtOverride = FVector::ZeroVector;
	bool bHasWatchCamOverride = false;

	/**
	 * -PursuitWatchForce: set up and hold the watched camera even under the Schola
	 * connector (DriveMode == Train). Purely presentational - no observation, reward,
	 * action or episode-lifecycle code path reads this - it exists because the trainer
	 * owns the view by default, which leaves a connector-driven eval recording with the
	 * host pawn's static view (2026-09-23 portfolio capture report: camera parked on
	 * map decorations, zero characters in frame while episodes ran).
	 */
	bool bForceWatchView = false;

	/**
	 * Watched-camera follow state (presentation only): when the watch camera is live,
	 * MaintainWatchedView eases the camera focus toward the chaser/evader midpoint so
	 * the chase stays in frame at close watch heights, where a camera parked on the
	 * arena centre would film empty floor for most episodes (2026-09-23 v2 capture:
	 * static plaza, zero agents in view while 18 episodes ran outside the frame).
	 */
	FVector WatchFocusSmoothed = FVector::ZeroVector;
	bool bWatchFocusInit = false;

	/** Set from -PursuitPaneLabel=<text>; underscores become spaces. Empty = no label. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Pursuit|Watch")
	FString PaneLabel;

	/** Watched-mode camera over the arena centre (tilted by default); the local player's view target. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Pursuit|Watch")
	TObjectPtr<ACameraActor> WatchCamera;

	/**
	 * Baked equivalent of -PursuitStage=city, for TRAINING levels: the city training
	 * map (Demonstration_Train, a duplicate of the demo map) carries the env with this
	 * set, so the Schola connector needs no command-line stage flags at all. Sets
	 * bCityStage and drops the arena rig exactly like the CLI switch; the env actor's
	 * own transform supplies the plaza centre (-250, 0, 70). Watched runs keep using
	 * the original Demonstration map via the EnvBox host - two maps, two purposes.
	 */
	UPROPERTY(EditInstanceOnly, Category = "Pursuit|Arena")
	bool bStartOnCityStage = false;

	/** City stage only: fixed jump platforms (75 cm, above the 45 cm step height, below
	 * the ~90 cm jump apex) scattered on the plaza. The evader's reflex hops them, so
	 * the chasing policy must learn JumpInput to keep the pursuit line - the reason
	 * the third action dimension exists. Catch from the ground onto a platform top is
	 * outside CatchHeightTolerance, so the capture itself may require the jump. */
	UPROPERTY(EditInstanceOnly, Category = "Pursuit|Arena")
	bool bSpawnCityJumpPlatforms = true;

	/** The spawned city jump platforms, kept for the HUD/tests and future removal. */
	UPROPERTY(VisibleInstanceOnly, Category = "Pursuit|Arena")
	TArray<TObjectPtr<AStaticMeshActor>> CityPlatforms;

	/** Spawns the city jump platforms (bCityStage + bSpawnCityJumpPlatforms only). */
	void SpawnCityJumpPlatforms();

	/**
	 * City stage only: 300 cm pillars on the plaza - taller than the ~90 cm jump apex, so
	 * the only way past is to steer around. Added 2026-09-22 after measuring that the
	 * plaza presented NO full-height obstacle at all (13 live episodes: 5 read 3/3 wall
	 * probes clear, every other reading was 0-31 cm of ground detail), which made
	 * straight-line pursuit optimal during training and useless off the plaza.
	 */
	UPROPERTY(EditInstanceOnly, Category = "Pursuit|Arena")
	bool bSpawnCityPillars = true;

	UPROPERTY(VisibleInstanceOnly, Category = "Pursuit|Arena")
	TArray<TObjectPtr<AStaticMeshActor>> CityPillars;

	/** Spawns the city pillars (bCityStage + bSpawnCityPillars only). */
	void SpawnCityPillars();

	// ---------------------------------------------------------------------
	// Read-only accessors, for HUD/debug draw/tests
	// ---------------------------------------------------------------------

	int32 GetCurrentStep() const { return CurrentStep; }
	int32 GetEpisodeCount() const { return EpisodeIndex; }
	float GetTotalReward() const { return TotalReward; }
	float GetCurrentReward() const { return CurrentReward; }
	bool WasCaughtThisEpisode() const { return bCaught; }

protected:
	// --- ISingleAgentScholaEnvironment ---
	virtual void InitializeEnvironment_Implementation(FInteractionDefinition& OutAgentDefinition) override;
	virtual void SeedEnvironment_Implementation(int InSeed) override;
	virtual void SetEnvironmentOptions_Implementation(const TMap<FString, FString>& InOptions) override;
	virtual void Reset_Implementation(FInitialAgentState& OutAgentState) override;
	virtual void Step_Implementation(const FInstancedStruct& InAction, FAgentState& OutAgentState) override;

	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;

private:
	UFUNCTION()
	void HandleChaserHit(AActor* HitActor, AActor* OtherActor, FVector NormalImpulse, const FHitResult& Hit);

	/** One rig obstacle, kept for spawn avoidance: a disc the capsule must not spawn inside. */
	struct FPursuitObstacle
	{
		FVector2D Centre;
		float ClearRadius = 0.0f;
	};

	void BeginEpisode();
	void SpawnAgents();

	/**
	 * Teleport an agent to Location with an explicit yaw.
	 *
	 * The yaw is a parameter rather than "look at the target" because facing the target
	 * on spawn DESTROYS THE DIRECTION OBSERVATION. Measured 2026-09-22: PlaceAgent used
	 * to derive its rotation from `(FaceToward - Location)`, i.e. the chaser spawned
	 * pointing exactly at the evader, so `InverseTransformVectorNoScale(ToTarget)` read
	 * (1.00, 0.00) in 45 of 45 episodes while the true bearing spanned -164..+175 deg
	 * (see tools/diag_obs_direction.py). The policy was told "target dead ahead" on the
	 * first step of every episode and never saw a reason to learn steering - which is
	 * exactly the behaviour 44/44 "ended farther than spawn" episodes showed.
	 */
	void PlaceAgent(APursuitCharAgent* Agent, const FVector& Location, float YawDegrees);
	void StopAgent(APursuitCharAgent* Agent);
	void BindHitEvents();
	void BuildArenaRig();
	FVector2D RandomSpawnPoint();

	/**
	 * True when a capsule-sized sphere swept from From along Dir (2D-normalised) hits
	 * world static geometry - the obstacle rig or the boundary ring. The scripted
	 * drivers use it to steer around obstacles; the trained policy gets no such help,
	 * which is the point of putting obstacles in.
	 */
	bool IsPathBlocked(const FVector& From, const FVector& Dir) const;

	/** IsPathBlocked with the hit exposed, so callers can tell a low box (hoppable) from a pillar (go around). */
	bool ProbePath(const FVector& From, const FVector& Dir, FHitResult& OutHit) const;

	/**
	 * City-stage spawn gate: one straight WorldStatic trace at torso height between
	 * the two candidate spawn columns. A clear column pair with a blocked line between
	 * them reads downstream as "the greedy chaser grinds against a facade at speed 0
	 * for the whole episode" (2026-09-21 city report), so a blocked pair is redrawn.
	 */
	bool SpawnPairPathClear(const FVector2D& A, const FVector2D& B) const;

	/**
	 * City-stage single-column spawn gate: a downward WorldStatic trace from just
	 * above the arena plane, so a lamp post, bench or planter rejects the draw while
	 * the asphalt itself never does. Extracted 2026-09-22 from RandomSpawnPoint so the
	 * constructed-pair path in SpawnAgents can validate its evader column through the
	 * exact same probe instead of duplicating the trace.
	 */
	bool SpawnPointColumnClear(const FVector2D& Point) const;

	/**
	 * One scripted-driver step toward Desired: straight if clear, hop over the blocker
	 * if it is low enough for the 90 cm jump apex, otherwise rotate through the probe
	 * bearings until a clear one is found. Shared by the rule evader and the greedy
	 * baseline - same movement pipeline, no teleporting, and it puts the jump on
	 * display before any policy exists.
	 */
	void SteerScriptedAgent(APursuitCharAgent* Mover, const FVector& Desired);

	/**
	 * Drives the support soldier (second chaser) at the evader: straight-line greedy
	 * steering, shared obstacle reflex, and the city-stage inward bend. One body of
	 * code because two watched modes want it - Greedy (both chasers script-driven)
	 * and Inference (the dog runs the policy; the soldier stays its scripted wingman).
	 * No-op without a SupportAgent, which is every non-hosted run.
	 */
	void SteerSupportChaser();


	void LoadChaserPolicy();
	void SetupWatchedView();

	/** Per-frame view-target guard + chase-follow camera + spectator hiding (watched Tick and -PursuitWatchForce). */
	void MaintainWatchedView(float DeltaSeconds);
	void UpdateWatchedHud();
	void AdvanceScriptedDrivers(float DeltaSeconds);
	void RunWatchedEpisode(float DeltaSeconds);
	void FinishEpisodeStats(bool bWasCaught);
	float ScoreStep(float NewDistance, bool bNewCatch, bool bOutOfTime, bool bWallHit, float Dt);

	/** Parsed from -PursuitCharSeed=<n>; 0 means "trainer decides via SeedEnvironment". */
	int32 Seed = 0;

	/** Seed + EpisodeIndex keys every episode's spawn draw, v1's anti-divergence rule. */
	FRandomStream Rng;
	FRandomStream DriveRng;

	/**
	 * Random-baseline heading hold. Re-rolling the chase direction every frame flips
	 * the movement input faster than the CharacterMovementComponent can accelerate,
	 * which reads on screen as the dog twitching in place (2026-09-21 watch report).
	 * One heading lasts RandomHeadingHoldSeconds of SIMULATED time, so the wander
	 * cadence stays frame-rate independent - the same argument as the sim-s wall.
	 */
	float RandomHeadingAngle = 0.0f;
	float RandomHeadingHold = 0.0f;
	static constexpr float RandomHeadingHoldSeconds = 0.8f;

	int32 CurrentStep = 0;
	int32 EpisodeIndex = 0;
	float TotalReward = 0.0f;
	float CurrentReward = 0.0f;
	float PrevDistance = 0.0f;
	/** Simulated seconds accumulated this episode; the EpisodeSeconds wall reads this. */
	float EpisodeSimTime = 0.0f;
	/** Chaser wall hits this episode; sanity metric for the episode-end log line. */
	int32 WallHitCount = 0;
	/**
	 * Wall-probe liveness counters (2026-09-22).
	 *
	 * Why they exist: the whole reason the 8-dim observation gained three wall probes
	 * was the hypothesis "the city chase fails because the dog cannot see facades".
	 * That hypothesis was never measured - only asserted - so a run could not tell
	 * "the probes work and it still cannot learn" apart from "the probes return 1.0
	 * forever and it is still blind". WallProbeHitsThisEpisode counts every probe,
	 * every step, that read closer than full range; ClosestWallProbeThisEpisode keeps
	 * the minimum reading seen. Both are printed with each episode summary.
	 */
	int32 WallProbeHitsThisEpisode = 0;
	float ClosestWallProbeThisEpisode = 1.0f;
	/**
	 * How many of those readings were tall obstacles ("block", hop-ability 0) rather
	 * than hoppable ones. A run with many wall hits but ~no block readings means the
	 * stage only ever presents crates, and routing never gets exercised; many block
	 * readings is the regime where the policy must learn to steer around.
	 */
	int32 HopBlockedReadingsThisEpisode = 0;
	bool bCaught = false;
	bool bWallHitThisStep = false;

	/**
	 * Contact-DURATION counters (Stage 3B, 2026-09-23) - the correction to a metric that
	 * was being read as if it measured duration.
	 *
	 * `WallHitCount` is a contact-EVENT count: it is incremented from OnActorHit, which a
	 * sustained contact fires on every physics tick (see HandleChaserHit's own note). An
	 * episode that leans on the pillar for its whole length therefore reports thousands of
	 * "hits" for ONE continuous touch, so `wall_hits_counted / steps` is not a duration at
	 * all - events are not steps.
	 *
	 * These two count what the metric should have been: the number of CONTROL STEPS in
	 * which at least one counted wall contact occurred, and how many of those steps
	 * involved the Stage 3 pillar specifically. `wall_hit_steps / samples` is a true
	 * contact-duration fraction bounded by 1.0, which the event count never was.
	 *
	 * The latches are CONSUMED BY THE PER-STEP SAMPLE HOOK, not by Step_Implementation. That
	 * placement is not cosmetic: Step_Implementation is the training/gRPC path only, so a
	 * metric advanced there reads an unconditional zero in every watched run (`-game`) -
	 * measured, on a straight-chase control whose chaser spent 11,550 steps pressed into the
	 * pillar and still reported 0 contact steps. AccumulateValidationSample is called exactly
	 * once per step on BOTH paths, so counting there puts the step count on the very same
	 * boundary as the `samples` it will be divided by.
	 *
	 * `bWallHitThisStep` is deliberately NOT reused: it is the reward's latch, and consuming
	 * it outside Step_Implementation would silently stop the wall-hit penalty being charged.
	 * Purely diagnostic: neither value below is read by the reward.
	 */
	int32 WallHitStepsThisEpisode = 0;
	int32 Stage3PillarHitStepsThisEpisode = 0;
	bool bStage3PillarHitThisStep = false;
	bool bWallContactStepLatch = false;

	/**
	 * True when the action being scored asked for a jump (JumpInput at or above the
	 * actuator's threshold). Recorded in Step_Implementation BEFORE Execute_Act, so
	 * ScoreStep can charge JumpActionPenalty against the DECISION rather than the
	 * resulting airborne state. See JumpActionPenalty's declaration for the measured
	 * reason the state-based version could not work.
	 */
	bool bJumpRequestedThisStep = false;

	/** True once the jump element of the incoming action has been located in either of
	 * the two shapes Step_Implementation accepts (dict key or flat 3-vector). The ACTION
	 * PROBE logs this on the first steps of a run: if it stays false, no penalty that
	 * keys off bJumpRequestedThisStep can ever fire, and the actuator numbers in the log
	 * would be measuring a term that is silently dead. */
	bool bJumpIntentResolved = false;

	/** One-shot latch for the ACTION PROBE (first two steps of a run only). */
	bool bActionProbeDone = false;
	bool bEpisodeRunning = false;

	/**
	 * Static-target task: the evader exists but never flees (see AdvanceScriptedDrivers,
	 * which early-outs on it).
	 *
	 * THREE writers, in this precedence order (2026-09-22, curriculum work):
	 *   1. the LEVEL bakes it - L_PursuitCharCurriculum ships with it ON, so a Stage 0
	 *      run over that map needs no command-line switch at all;
	 *   2. -PursuitCharStatic forces it ON from the command line. The switch can only
	 *      turn it on, never off: it is a "make this static" request, and a run that
	 *      wants a fleeing target simply does not pass it;
	 *   3. SetEnvironmentOptions("bStaticTarget") lets the trainer set it either way.
	 *
	 * Deliberately ONE field for all three. An extra "bStaticTargetOnStart" would give
	 * the env two sources of truth for one task semantic and a precedence question at
	 * every read site - the same class of bug as the TargetActor that was never
	 * assigned (see SpawnAgents): the wrong value is not wrong anywhere in particular,
	 * it is wrong in the gap between two fields that are each individually correct.
	 *
	 * EditAnywhere and NOT BlueprintReadWrite: this member lives in the private section
	 * (UHT rejects BlueprintReadWrite there), and EditAnywhere is all the level-bake path
	 * needs - tools/gen_char_curriculum_level.py sets it through set_editor_property,
	 * which only requires CPF_Edit.
	 */
	UPROPERTY(EditAnywhere, Category = "Pursuit|Rules")
	bool bStaticTarget = false;

	/**
	 * -PursuitStage=city: the rig is skipped entirely and the city map is the stage -
	 * its ground is the floor, its lights light the shot. The arena defaults to the
	 * measured clean plaza (tools/inspect_city_stage.py: centre -250,0, ground z=10,
	 * hence arena centre z=70), overridable with -PursuitArenaAt. Spawns gain a
	 * vertical clearance probe (city geometry is not in RigObstacleDiscs) plus a
	 * straight-path gate between the pair, and the evader a soft leash back toward
	 * the plaza, which is where the camera is.
	 */
	bool bCityStage = false;

	/**
	 * City-stage spawn/leash radius in cm. The rig's ArenaRadius (1200) assumes a
	 * walled arena; on the city stage the same draw radius scatters agents into the
	 * streets where facades block a straight-line chase. 500 keeps both agents on the
	 * measured clean plaza. -PursuitCitySpawnRadius overrides.
	 */
	UPROPERTY(EditAnywhere, Category = "Pursuit|Arena", meta = (ClampMin = "200"))
	float CityStageRadius = 500.0f;

	bool bLogEpisodes = true;

	// ---------------------------------------------------------------------
	// Stage 0 validation state (2026-09-22). Inert unless -PursuitCharMaxEpisodes is
	// given or a Greedy/Random watched run is being measured; the training path only
	// pays a handful of float adds per step for it.
	// ---------------------------------------------------------------------

	/**
	 * -PursuitCharMaxEpisodes=<n>: exit the process after exactly n COMPLETED episodes
	 * (catch or timeout). 0 = no episode-count exit.
	 *
	 * Why it exists: a headless run does NOT tick in real time - under -nullrhi it runs
	 * far faster than realtime - so "run for 100 x EpisodeSeconds of wall clock" cannot
	 * produce exactly 100 episodes. Deriving the episode count from a wall-clock budget
	 * is what made the previous validation numbers unreproducible. QuitAfterSeconds
	 * stays in place as the anti-hang net; this is the mechanism that makes "exactly
	 * 100 completed episodes" true by construction.
	 *
	 * Deliberately NOT a UPROPERTY: this is a validation-only exit, and a value baked
	 * into a level would silently cap a real training run that loaded that level.
	 */
	int32 MaxEpisodes = 0;

	/** Completed episodes (catch + timeout); the counter MaxEpisodes is tested against. */
	int32 CompletedEpisodeCount = 0;

	/** One-shot latch: once set, no further episode is started and the process exits. */
	bool bValidationExitRequested = false;

	/** Chaser-target distance at the first frame of the current episode ("start_d"). */
	float EpisodeStartDistance = 0.0f;

	/** Closest chaser-target distance seen this episode (starts at the spawn gap). */
	float ClosestDistanceThisEpisode = 0.0f;

	/** Samples taken this episode, and how many of them had the chaser grounded. */
	int32 ValidationSampleCount = 0;
	int32 GroundedSampleCount = 0;

	/** Sums for the episode means: speed (cm/s) and cos(velocity, bearing-to-target). */
	double SpeedSumCmPerSec = 0.0;
	double DotVelocityToTargetSum = 0.0;

	/**
	 * EVADER motion census - the moving-target stages (Stage 2A onward).
	 *
	 * The chaser's own numbers cannot answer "is the target actually moving?": a static
	 * evader and a fleeing one produce the same start_d/end_d whenever the chaser closes
	 * either way. So the target's motion is measured on the target, directly:
	 * path length (sum of per-sample displacements, so a circling evader that returns to
	 * its spawn still reads as moving) beside the end-to-start chord, the mean speed over
	 * the same samples as the chaser's, and the peak.
	 *
	 * All five stay at exactly 0 through a static-target episode, which is the converse
	 * evidence for the Stage 0/1 runs; they only advance when bStaticTarget is false.
	 * Snapshot at placement in SpawnAgents, accumulated in AccumulateValidationSample.
	 */
	FVector EvaderEpisodeStartLocation = FVector::ZeroVector;
	FVector EvaderPrevLocation = FVector::ZeroVector;
	float EvaderPathLengthCm = 0.0f;
	double EvaderSpeedSumCmPerSec = 0.0;
	float EvaderMaxSpeedCmPerSec = 0.0f;

	/** Actuator jump counter at episode start; the summary reports the difference. */
	int32 JumpCountAtEpisodeStart = 0;

	/**
	 * Wall-hit classification census - RUN LEVEL, never reset (see HandleChaserHit for
	 * why a per-episode window had to be abandoned: it loses the only evader contact that
	 * actually occurs, leaving `target=0` beside a logged CLASS=ignored_target line).
	 *
	 * Printed with every validation summary as `ignored_total[...]`, which is how
	 * "standing on flat ground accumulates nothing / hitting the boundary wall does /
	 * contacting the target does not" is proven from the log rather than asserted.
	 */
	int32 WallHitIgnoredSelfCount = 0;
	int32 WallHitIgnoredTargetCount = 0;
	int32 WallHitIgnoredFloorCount = 0;
	int32 WallHitIgnoredSupportCount = 0;

	/** Caps the CLASS=... log lines for a whole run, so the log stays readable. */
	int32 WallHitClassLogCount = 0;

	/**
	 * Separate cap for the evader-contact class. It needs its own because it is the RARE
	 * one - the catch fires at CatchRadius (120 cm), well outside the ~68 cm capsule
	 * contact distance, so in a Greedy run the chaser can win without ever touching the
	 * target. Sharing one cap with the floor/support lines would let the common classes
	 * crowd the rare one out of the log, and the rare one is exactly the class the
	 * accept/reject list has to demonstrate ("contacting the target is not a wall hit").
	 */
	int32 WallHitTargetClassLogCount = 0;

	/** Push bEnableAgentJump down onto every jump actuator the env owns. */
	void SyncJumpGate();

	/**
	 * Make the game mode's unused spectator pawn non-physical.
	 *
	 * Why (2026-09-22, Stage 0 measurement): a generated training level has no PlayerStart,
	 * so the local player's DefaultPawn spawns at the WORLD ORIGIN - which is the arena
	 * centre - and its collision sphere becomes a solid body sitting in the middle of the
	 * chase. Measured on the flat curriculum arena over 100 greedy episodes: the chaser
	 * collided with DefaultPawn_0/MeshComponent0 and the wall-hit classifier, correctly
	 * following its own rules, counted it as a vertical obstacle (latent `wall_hits=5` in a
	 * level that contains no internal obstacle at all, plus 9 contacts it classified as
	 * support). This is a REWARD defect, not just a metric one: WallHitPenalty was being
	 * charged for bumping scenery during training too.
	 *
	 * Deliberately narrow: only the local player's pawn, and never one of the three task
	 * agents. "Ignore all pawns" would erase the agents as well.
	 */
	void NeutralizeStraySpectator();

	/**
	 * Build the single Stage 3 pillar (bStage3PillarLayout only): one 300x300x400 cm
	 * BlockAll cube centred on the arena, base on the floor, plus its spawn-avoidance disc.
	 * Called from BeginPlay after BuildArenaRig so the level stays a pure property bake -
	 * the same division of labour the greybox obstacle table already uses.
	 */
	void BuildStage3Pillar();

	/**
	 * Construct the two controlled Stage 3 spawn pairs, in env-relative cm.
	 *
	 *   blocked - chaser (-S/2, 0), evader (+S/2, 0): the pillar is exactly on the
	 *             straight line between them, so the direct chase is impossible.
	 *   clear   - the same pair translated +L on Y: the same separation, the same facing,
	 *             the same target bearing, but nothing on the line.
	 *
	 * Which one this episode gets is episode parity (even = blocked), so a run of N
	 * episodes contains both groups by construction instead of by luck.
	 */
	void PlaceStage3SpawnPair(FVector2D& OutChaserPoint, FVector2D& OutEvaderPoint);

	/**
	 * One-shot probe sweep proving the pillar is OBSERVABLE: the chaser is teleported to a
	 * fixed list of positions and facings around it, the 15D sensor is read once per
	 * position, and every reading is logged against the geometry that produced it. The
	 * comparison (which probe should have fired, on which side, and what "out of range"
	 * must read) is done off-line by the analyzer from the logged position/facing - this
	 * function only produces evidence, it does not grade itself.
	 */
	void RunStage3ProbeSelfTest();

	/** One-shot latch for the probe self-test, so a long run is not buried in sweeps. */
	bool bStage3ProbeSelfTestDone = false;

	/**
	 * Tally one observation's five wall-probe range readings into the per-episode liveness
	 * counters. Shared VERBATIM by both paths, because Stage 0's acceptance runs are
	 * watched runs: a counter that only existed on the training path would leave the one
	 * metric this fix is about unmeasured in every run that decides whether to go on to
	 * training at all.
	 *
	 * Takes the already-unwrapped FBoxPoint on purpose - it must be fed by
	 * ResolveTargetSensorPoint, never by a hand-written cast. The hand-written cast is
	 * what made this counter read 0 forever.
	 */
	void TallyWallProbes(const FBoxPoint* SensorBox);

	/** Zero the per-episode validation accumulators (called at the end of BeginEpisode). */
	void ResetEpisodeValidationState();

	/** One per-step validation sample: closest distance, grounded, speed, direction. */
	void AccumulateValidationSample(float DistanceCm);

	/** Print the per-episode validation line (result, start/end/closest, grounded, speed, direction). */
	void LogEpisodeValidationSummary(const TCHAR* ResultKind, float EndDistance);

	/** Count a finished episode and, at MaxEpisodes, request the process exit. */
	void NoteEpisodeCompleted(bool bWasCaught);

	/** Parsed from -PursuitStartAt=<unix epoch>: both panes hold until the same second. */
	double StartAtEpoch = 0.0;
	bool bWaitingForGo = false;

	/** Parsed from -PursuitCharQuitAfter=<seconds>: watched runs exit themselves, headless-friendly. */
	double QuitAfterSeconds = 0.0;
	double WatchedStartTime = 0.0;

	/** Rig components, created at BeginPlay when bBuildArenaRig is set. */
	UPROPERTY()
	TObjectPtr<UStaticMeshComponent> RigFloor;

	UPROPERTY()
	TArray<TObjectPtr<UStaticMeshComponent>> RigWalls;

	UPROPERTY()
	TArray<TObjectPtr<UStaticMeshComponent>> RigObstacles;

	/** Obstacle discs for spawn avoidance, filled by BuildArenaRig. */
	TArray<FPursuitObstacle> RigObstacleDiscs;
};
