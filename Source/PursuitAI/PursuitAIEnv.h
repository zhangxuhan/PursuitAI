// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "TrainingUtils/GymConnectorManager.h"
#include "Environment/SingleAgentEnvironmentInterface.h"
#include "Agent/AgentInterface.h"
#include "Points/Point.h"
#include "PursuitAIEnv.generated.h"

class UCameraComponent;
class UStaticMeshComponent;

// Forward-declared on purpose: including NNEPolicy.h and SimpleStepper.h here would drag
// the whole NNE header tree into every translation unit that touches this actor.
class UNNEModelData;
class UNNEPolicy;
class USimpleStepper;

/**
 * Who drives the chase. One level, three drivers, one implementation of the world:
 *
 *   Train      gRPC -> Python (SB3 PPO). Headless, no scenery, hundreds of steps/second.
 *   Inference  a trained ONNX model run in-engine by UNNEPolicy + USimpleStepper. No Python.
 *   Demo       the built-in scripted policy. Needs no model at all.
 *
 * This matters because Schola's training and inference sides are two *separate*
 * abstractions - IBaseScholaEnvironment (InitializeEnvironment/Reset/Step, driven by
 * AGymConnectorManager over gRPC) and IAgent (Define/Observe/Act, driven by a stepper
 * over a local policy) - which share nothing on their own. Implementing both on this one
 * actor and routing them through shared helpers is what stops the two from drifting:
 * Define() is the only place the spaces are described, BuildObservation() the only
 * observation, ApplyActionVector() the only movement. A policy therefore cannot be
 * trained against one layout and then fed a different one at inference time, which is
 * the classic silent failure of a train/deploy split.
 */
UENUM(BlueprintType)
enum class EPursuitDriveMode : uint8
{
	/** gRPC to a Python trainer. Invisible; the bare map is what a training run wants. */
	Train,

	/** A trained ONNX model executed locally through UNNEPolicy. No Python involved. */
	Inference,

	/** Built-in scripted policy. No model, no Python - just something to look at. */
	Demo,

	/**
	 * A uniform random direction at full stick, every step.
	 *
	 * This is the "before training" baseline, and the reason it is random rather than an
	 * untrained network is measured rather than aesthetic: SB3 initialises its action layer
	 * with a gain of 0.01, so an untouched policy outputs |logits| around 0.002 and moves
	 * about 0.11 cm per step on a 50 cm step budget. Filmed, that is a frozen dot, and a
	 * viewer reads a frozen dot as a broken pipeline rather than as an untrained policy.
	 * A random walk is what "no information about the task" actually looks like, and it is
	 * honest: it is a baseline, not a claim about any network's output.
	 *
	 * The step draws from its own FRandomStream, seeded from the same -PursuitSeed as the
	 * spawns but advanced separately, so pinning the seed still gives two panes identical
	 * starting positions and a reproducible walk.
	 */
	Random,
};

/**
 * What the target does when the agent is not on top of it.
 *
 *   Static  the original behaviour: a fixed point. Kept as the default so every result
 *           recorded before this existed stays reproducible.
 *   Flee    a scripted evader. Phase 1 of the project trains the *chaser* only, and a
 *           chaser trained against a stationary target learns nothing that survives a
 *           target that moves - so the evader has to exist before training means anything.
 */
UENUM(BlueprintType)
enum class EPursuitTargetPolicy : uint8
{
	Static,
	Flee,
};

/**
 * A flying chaser hunting a target inside a box-shaped arena.
 *
 * Observation: Box(9) - enough to hunt something that runs away.
 *   [0..2] relative position, (target - agent) / full arena extent
 *   [3..5] own position,      agent / half arena extent
 *   [6..8] relative velocity, (target velocity - agent velocity) / (MoveStep * 2 * rate)
 * Both position halves are inside [-1, 1] by construction, so no clamping or saturation is
 * needed. The relative position alone is enough to hunt a *stationary* target; a target that
 * moves cannot be intercepted from position alone, because the policy has no way to tell a
 * target standing still from one that is about to leave. The velocity half is what closes
 * that gap, and it is normalised by the fastest relative motion the step allows so it lands
 * in the same range.
 *
 * Action: Box(3) - a direction in [-1, 1]^3 at full stick. Magnitude is clamped to 1
 * before scaling by MoveStep, so travelling diagonally is no faster per step than
 * travelling along an axis and the reward shaping keeps its scale.
 *
 * Reward (training only): closing the gap is worth up to +1 per step, backing off costs
 * the same; +GoalReward on capture, +TimeoutPenalty when the step budget runs out.
 *
 * The target's behaviour is TargetPolicy. It defaults to Static, which is what every
 * result recorded before the evader existed was measured against; Flee turns it into a
 * scripted runner so the chaser has something that actually has to be run down.
 */
UCLASS()
class PURSUITAI_API APursuitAIEnv : public AGymConnectorManager,
                                    public ISingleAgentScholaEnvironment,
                                    public IAgent
{
	GENERATED_BODY()

public:
	APursuitAIEnv();

	// ---------------------------------------------------------------------
	// World. The arena and its rules, in cm. Nothing here knows about RL.
	// ---------------------------------------------------------------------

	/** Half extent along X and Y. The agent stays inside [-ArenaHalfSize, +ArenaHalfSize]. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "PursuitAI|World")
	float ArenaHalfSize = 500.0f;

	/** Half extent along Z. Smaller than the XY extent, so the arena is a slab, not a cube. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "PursuitAI|World")
	float ArenaHalfHeight = 250.0f;

	/**
	 * World-space centre of the arena. Default (0,0,0) - the grey training rig is built
	 * around the origin, so nothing there needs to move. On the Demonstration city map the
	 * arena is parked in a street instead, and every world placement (spawn, clamp, floor,
	 * camera) is expressed relative to this point. Set by -PursuitArenaAt=X,Y,Z.
	 *
	 * The simulation itself stays centre-relative by construction: RandomizePositions and
	 * ClampToArena add the centre, BuildObservation divides by it, and nothing else in the
	 * state ever sees a world coordinate. That keeps an arena moved 250 cm east identical,
	 * observation for observation, to one left at the origin - which is what lets a policy
	 * train on the rig and demo in the city without the stage being part of the task.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "PursuitAI|World")
	FVector ArenaCenter = FVector::ZeroVector;

	/**
	 * City stage, turned on by -PursuitStage=city. Three things change, all presentation:
	 * the env does not build its own floor (the city road is the floor), does not spawn its
	 * two directional lights (the city has its own sun), and the demo camera goes vertical -
	 * straight down from above the arena centre, the one angle that a canyon of buildings
	 * cannot occlude. Everything else, the slab, the stepping, the draw overlay, is the
	 * same code that runs on the training rig.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "PursuitAI|World")
	bool bCityStage = false;

	/**
	 * Height of the city-stage camera above the arena floor, in cm. Same number as the god
	 * camera on the playable chase, for the same measured reason: below the 5739 cm towers,
	 * above the 2441 cm mid-rises, framing a 1000 cm arena through a 34 degree lens.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "PursuitAI|World")
	float CityCameraHeight = 2900.0f;

	/** Distance travelled per step at full deflection, in cm. Diagonals are not faster. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "PursuitAI|World")
	float MoveStep = 50.0f;

	/** Episode ends with success when the agent gets within this distance of the target. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "PursuitAI|World")
	float CatchRadius = 50.0f;

	/**
	 * Whether the target runs away. See EPursuitTargetPolicy.
	 *
	 * Overridable from the command line with -PursuitFlee / -PursuitStatic, because the two
	 * behaviours answer different questions and a comparison between them should not need a
	 * code change or a re-saved level.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "PursuitAI|Target")
	EPursuitTargetPolicy TargetPolicy = EPursuitTargetPolicy::Static;

	/**
	 * How much of a full chaser step the target moves per step while fleeing.
	 *
	 * Below 1.0 on purpose, and that inequality is the whole design of the task: a target
	 * that is exactly as fast as the chaser can never be caught in a straight line and the
	 * episode is decided entirely by the arena wall. A target that is *slightly* slower is
	 * catchable by a policy that cuts it off and never by one that simply chases its
	 * current position - which is exactly the behaviour this phase is meant to train.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "PursuitAI|Target")
	float TargetSpeedScale = 0.82f;

	/**
	 * How hard the target swerves perpendicular to its escape direction.
	 *
	 * A target that runs in a perfectly straight line is trivially interceptable once the
	 * chaser learns to lead it, so the evasion has a lateral component. Scaled by how close
	 * the chaser is, so it swerves when it matters and runs clean when it does not.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "PursuitAI|Target")
	float TargetEvadeGain = 0.85f;

	/**
	 * Radians per step the swerve direction rotates.
	 *
	 * The perpendicular vector on its own is constant, so the evasion would be a fixed
	 * sidestep that a policy can simply aim off from. Rotating it every step turns that into
	 * a weave, which is what stops "aim where the target will be" from being a closed-form
	 * answer and keeps the task worth learning.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "PursuitAI|Target")
	float TargetEvadeTurnRate = 0.22f;

	// ---------------------------------------------------------------------
	// Episode
	// ---------------------------------------------------------------------

	/** Steps before the episode is truncated. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "PursuitAI|Episode")
	int32 MaxSteps = 300;

	/** Reward granted on capture. Training only. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "PursuitAI|Episode")
	float GoalReward = 10.0f;

	/** Reward granted when the step budget runs out without a capture. Training only. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "PursuitAI|Episode")
	float TimeoutPenalty = -1.0f;

	/** Fixed seed used at reset. 0 means "pick a fresh random seed every episode". */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "PursuitAI|Episode")
	int32 Seed = 0;

	// ---------------------------------------------------------------------
	// Drive mode and debug
	// ---------------------------------------------------------------------

	/**
	 * Set from the command line in BeginPlay, never by hand:
	 *   -PursuitDemo        -> Demo
	 *   -PursuitInference   -> Inference
	 *   (neither)           -> Train
	 *
	 * -PursuitVisual is orthogonal and works with any of the three: it turns the scenery
	 * on. Train is the only mode that defaults it off, because a training run wants
	 * throughput and the scenery is pure overhead - but "watch it learn" is a legitimate
	 * thing to want, and it is the same switch either way.
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "PursuitAI|Mode")
	EPursuitDriveMode DriveMode = EPursuitDriveMode::Train;

	/**
	 * The whole debug view: the on-screen panel plus the world-space arrows, rays, ring and
	 * labels. Set in BeginPlay rather than by hand, because it has to follow the same rule
	 * the scenery does - there is no point drawing a debug overlay into a launch that cannot
	 * render, and every visual launch wants it.
	 *
	 *   -PursuitNoDebug   hide it (a clean capture for a demo video)
	 *   -PursuitDebug     force it on
	 *   (neither)         on whenever the scenery is on
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "PursuitAI|Debug")
	bool bDrawDebug = true;

	/** Log one line per episode when it ends. Useful for a smoke test; noisy for long runs. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "PursuitAI|Debug")
	bool bLogEpisodes = true;

	// ---------------------------------------------------------------------
	// Watch mode - see the chase without a Python trainer attached
	// ---------------------------------------------------------------------

	/** Environment steps per second while watching. Lower it to inspect a single decision. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "PursuitAI|Watch")
	float DemoStepsPerSecond = 6.0f;

	/** Seconds to hold the final frame of an episode before the next one starts. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "PursuitAI|Watch")
	float DemoEpisodePause = 0.75f;

	/**
	 * Unix epoch before which a watched run holds its first frame instead of stepping.
	 *
	 * Two panes launched a few seconds apart otherwise start their episodes a few seconds
	 * apart, and a side-by-side comparison whose halves are out of phase is not a
	 * comparison. Both processes are given the same deadline, both sit still showing
	 * WAITING FOR GO, and both take their first step on the same wall-clock second - which
	 * is also free visual evidence that they did, because the clip opens with both panes
	 * frozen in the same pose. 0 means "go immediately".
	 *
	 * Overridable with -PursuitStartAt=<unix epoch>.
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "PursuitAI|Watch")
	double StartAtEpoch = 0.0;

	// ---------------------------------------------------------------------
	// Inference. Only used in EPursuitDriveMode::Inference.
	// ---------------------------------------------------------------------

	/**
	 * Where the ONNX model comes from. Two forms are accepted:
	 *   /Game/Models/X.X  an asset imported through the content browser
	 *   D:/path/to.onnx   a file on disk, read and tagged as "onnx" at runtime
	 * The disk form is what keeps the pipeline headless: train -> export -> run, with no
	 * editor step in between. Empty means <Project>/checkpoints/policy.onnx.
	 * Overridable with -PursuitModel=<path>.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "PursuitAI|Inference")
	FString InferenceModelPath;

	/** NNE runtime to execute on. NNERuntimeORTCpu is CPU; NNERuntimeORTDml uses DirectML. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "PursuitAI|Inference")
	FString InferenceRuntimeName = TEXT("NNERuntimeORTCpu");

	/** Loads the model, builds the policy and the stepper. No-op outside Inference mode. */
	void SetupInference();

	/** Resolves InferenceModelPath to a UNNEModelData, from the content browser or from disk. */
	UNNEModelData* ResolveInferenceModel();

	/** Runs one Observe -> Think -> Act cycle through UNNEPolicy. */
	void StepInference();

	/** Created in the constructor, initialised in SetupInference once a model is loaded. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "PursuitAI|Inference")
	TObjectPtr<UNNEPolicy> Policy;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "PursuitAI|Inference")
	TObjectPtr<USimpleStepper> Stepper;

	// --- Visual markers. Built on every instance, only shown when watching. ---

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "PursuitAI|Visuals")
	TObjectPtr<UStaticMeshComponent> AgentMarker;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "PursuitAI|Visuals")
	TObjectPtr<UStaticMeshComponent> TargetMarker;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "PursuitAI|Visuals")
	TObjectPtr<UStaticMeshComponent> ArenaFloor;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "PursuitAI|Visuals")
	TObjectPtr<UCameraComponent> DemoCamera;

	// ---------------------------------------------------------------------
	// Read-only accessors, handy for tests and debug drawing
	// ---------------------------------------------------------------------

	FVector GetAgentPos() const { return AgentPos; }
	FVector GetTargetPos() const { return TargetPos; }
	int32 GetCurrentStep() const { return CurrentStep; }
	int32 GetEpisodeCount() const { return EpisodeCount; }
	/** Distance travelled by the agent since the last reset, in cm. */
	float GetDistanceTravelled() const { return DistanceTravelled; }
	/** The two half extents as one vector, for scaling and clamping. */
	FVector GetArenaHalfExtent() const { return FVector(ArenaHalfSize, ArenaHalfSize, ArenaHalfHeight); }

	/** Reward accumulated in the current episode. Same meaning in all three drive modes. */
	float GetTotalReward() const { return TotalReward; }

	/** Reward of the most recent step. */
	float GetCurrentReward() const { return CurrentReward; }

protected:
	// --- ISingleAgentScholaEnvironment: the training side, driven over gRPC ---
	virtual void InitializeEnvironment_Implementation(FInteractionDefinition& OutAgentDefinition) override;
	virtual void SeedEnvironment_Implementation(int InSeed) override;
	virtual void SetEnvironmentOptions_Implementation(const TMap<FString, FString>& InOptions) override;
	virtual void Reset_Implementation(FInitialAgentState& OutAgentState) override;
	virtual void Step_Implementation(const FInstancedStruct& InAction, FAgentState& OutAgentState) override;

	// --- IAgent: the inference side, driven by a stepper over a local policy ---
	virtual EAgentStatus GetStatus_Implementation() override;
	virtual void SetStatus_Implementation(EAgentStatus NewStatus) override;
	virtual void Define_Implementation(FInteractionDefinition& OutInteractionDefinition) override;
	virtual void Observe_Implementation(FInstancedStruct& OutObservations) override;
	virtual void Act_Implementation(const FInstancedStruct& InAction) override;

	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;

private:
	// ---------------------------------------------------------------------
	// World state. Plain 3D vectors, no RL concepts.
	// ---------------------------------------------------------------------

	/** Current agent position. */
	FVector AgentPos = FVector::ZeroVector;

	/** Current target position. */
	FVector TargetPos = FVector::ZeroVector;

	/**
	 * Target position before the most recent step.
	 *
	 * Kept so the target's velocity can be differenced rather than stored. The velocity is a
	 * property of the *step*, and computing it from two positions is what guarantees the
	 * number in the observation is the same one the movement actually produced - a separately
	 * maintained velocity vector is a second source of truth that can drift.
	 */
	FVector PrevTargetPos = FVector::ZeroVector;

	/** Agent position before the most recent step, for the same reason. */
	FVector PrevAgentPos = FVector::ZeroVector;

	/**
	 * The target's last escape direction.
	 *
	 * Held across steps because the weave is defined as a *rotation* of it: without memory of
	 * which way it was running, "rotate the perpendicular" has no frame to rotate in and the
	 * evasion collapses to a constant offset.
	 */
	FVector TargetHeading = FVector::ForwardVector;

	/** Steps taken in the current episode. */
	int32 CurrentStep = 0;

	/** Episodes finished so far. Diagnostics only. */
	int32 EpisodeCount = 0;

	/**
	 * How many episodes this actor has started. Used only to seed each episode's streams.
	 *
	 * Separate from EpisodeCount because the two move at different times: this one
	 * increments when an episode BEGINS, so it is the number the seed is derived from,
	 * while EpisodeCount counts finished ones and is what the HUD shows.
	 */
	int32 EpisodeIndex = 0;

	/** Distance from the target at the previous step, used to shape the reward. */
	float PrevDistance = 0.0f;

	/** Total distance the agent has moved in this episode. */
	float DistanceTravelled = 0.0f;

	FRandomStream Rng;

	/** 0 while nothing is driving the actor; set by the stepper's own bookkeeping. */
	EAgentStatus AgentStatus = EAgentStatus::Running;

	/** True when the scenery should be shown. Set from the drive mode in BeginPlay. */
	bool bVisualsEnabled = false;

	/**
	 * Where the agent has actually been this episode, one point per step.
	 *
	 * The trail is the third thing a comparison needs and the two arrows cannot provide:
	 * an arrow shows one decision, the trail shows what a hundred of them added up to. It
	 * is the only element here that distinguishes "aimed well and got there" from "aimed
	 * well by accident once", which is exactly the difference between a trained and an
	 * untrained policy.
	 */
	TArray<FVector> Trail;

	/**
	 * The random policy's own stream.
	 *
	 * Separate from Rng on purpose. Rng drives the spawns, and if the walk drew from it
	 * then a pane taking a different number of steps would desynchronise the *spawn*
	 * sequence of the next episode, and the two panes would stop being the same
	 * experiment. Two streams means the walk can consume as much randomness as it likes
	 * without touching the world it is walking around in.
	 */
	FRandomStream ActionRng;

	/** Big centred text after an episode ends: CAUGHT, or TIMEOUT. Empty means none. */
	FString EpisodeMessage;

	/** Seconds left to keep that text up. */
	float MessageRemaining = 0.0f;

	/**
	 * Pane label burned into the top-left corner of the window.
	 *
	 * Two game windows have the same title, and a side-by-side clip whose halves are not
	 * labelled inside the picture has to be labelled afterwards - which means aligning
	 * overlaid text to two panes that were never guaranteed to be the same size. Burning
	 * it in the HUD costs one line and cannot drift out of alignment.
	 *
	 * Overridable with -PursuitPaneLabel=<text>. ASCII only: the debug font has no CJK.
	 */
	FString PaneLabel;

	/** Points kept in the trail before the oldest are dropped. -PursuitTrailMax=<n> */
	int32 TrailMax = 300;

	// ---------------------------------------------------------------------
	// Debug display state. Nothing below is read by the simulation - it exists so
	// the panel and the world markers can show what just happened.
	// ---------------------------------------------------------------------

	/** Which chaser this is, for the overhead label. Numbered in BeginPlay. */
	int32 AgentId = 0;

	/** This frame's line-of-sight answer, so the trace runs once rather than once per consumer. */
	bool bTargetVisible = true;

	/** Where that trace ended: the target, or the point that blocked it. */
	FVector SightEndPoint = FVector::ZeroVector;

	/** Whether the one-time occluder count has been logged. */
	bool bOccludersLogged = false;

	/** What the policy asked for, before the magnitude clamp. */
	FVector LastActionRequested = FVector::ZeroVector;

	/** What survived the clamp, i.e. the direction the step was actually taken along. */
	FVector LastActionApplied = FVector::ZeroVector;

	/** Where this step started, so the applied and discarded offsets can both be drawn. */
	FVector LastStepStart = FVector::ZeroVector;

	/** Direction * MoveStep, before the arena clamp got to it. */
	FVector LastIntendedOffset = FVector::ZeroVector;

	/** Where the agent actually ended up, relative to LastStepStart. */
	FVector LastActualOffset = FVector::ZeroVector;

	/** True when the arena bounds ate part of the step. Drawn red, and the "hit a wall" cue. */
	bool bActionClamped = false;

	/** Reward of the most recent step. */
	float CurrentReward = 0.0f;

	/** Reward accumulated over the current episode. */
	float TotalReward = 0.0f;

	/** How much of the gap to the target was closed by the most recent step, in cm. */
	float CurrentClosing = 0.0f;

	/** Best episode total so far. */
	float BestEpisodeReward = -TNumericLimits<float>::Max();

	/** Episodes that ended in a capture, and in a timeout. */
	int32 CaughtCount = 0;
	int32 TimeoutCount = 0;

	/** Totals of the last few episodes, for the mean on the panel. */
	TArray<float> RecentEpisodeRewards;

	/** Monotonic step counter. CurrentStep resets every episode, this one never does. */
	int64 TotalStepCount = 0;

	/** Measured over the last sampling window, not configured anywhere. */
	float MeasuredStepsPerSecond = 0.0f;
	float MeasuredEpisodesPerSecond = 0.0f;

	int64 StatsWindowSteps = 0;
	int32 StatsWindowEpisodes = 0;
	double StatsWindowStart = 0.0;
	bool bStatsWindowOpen = false;

	// ---------------------------------------------------------------------
	// The shared layer. Each of these has exactly one implementation, and both
	// the training path and the inference path go through it.
	// ---------------------------------------------------------------------

	/**
	 * Writes the Box(6) observation. Used by Observe, Reset and Step alike.
	 * Typed deliberately: both the training interface (FAgentState::Observations) and the
	 * inference one go through this, and Observe adapts the type-erased payload it is given.
	 */
	void BuildObservation(TInstancedStruct<FPoint>& OutObservation) const;

	/** Reads the Box(3) action payload and moves the agent. Returns the direction applied. */
	FVector ApplyAction(const FInstancedStruct& InAction);

	/** Moves the agent along InDirection, clamped to the arena. The only movement code. */
	void ApplyActionVector(const FVector& InDirection);

	/**
	 * Moves the target for one step according to TargetPolicy.
	 *
	 * Called from every step, in all three modes, right after the agent moves. Routing it
	 * through one function is what keeps the scripted evader identical in a training episode
	 * and in a watched one - a demo that evades differently from the environment the policy
	 * trained in would be showing the wrong picture entirely.
	 *
	 * Returns the direction the target moved, which is zero when the policy is Static.
	 */
	FVector StepTarget();

	/** Which way the target runs from where it currently is. Flee policy only. */
	FVector ComputeEscapeDirection(float DistanceToAgent) const;

	/** The only place the observation and action spaces are described. */
	void DefineSpaces(FInteractionDefinition& OutDefinition) const;

	/** Places the agent and target at random, separated positions. */
	void RandomizePositions();

	/** Clears the per-episode counters and re-randomises. Shared by all three modes. */
	void BeginEpisode();

	FVector ClampToArena(const FVector& Position) const;

	float DistanceToTarget() const;

	/**
	 * Scores one step and folds it into the running totals. Both the training path and the
	 * watched paths call this, which is what makes "reward" on screen mean the same thing
	 * whether the numbers came from the trainer, from ONNX or from the scripted driver.
	 */
	float ScoreStep(float NewDistance, bool bCaught, bool bOutOfTime);

	/** The one place the step counters move, so the HUD's rate is a rate of real steps. */
	void AdvanceStepCounter();

	/** Folds a finished episode into the best/mean/caught statistics. */
	void FinishEpisodeStats(bool bCaught);

	/** Closes an episode in a watched run: counts it, logs it, and holds or resets. */
	void FinishWatchedEpisode(bool bCaught);

	/** World-space debug: action arrow, goal arrow, line-of-sight ray, capture ring, labels. */
	void DrawDebug() const;

	/** The on-screen panel. ASCII only - the engine's debug font carries no CJK glyphs. */
	void DrawHud();

	/** Samples the step and episode counters, so the panel can show a live rate. */
	void UpdateStats();

	/**
	 * Traces the straight line from the agent to the target. Returns true when nothing blocks
	 * it; OutEnd is then the target, and otherwise the point where the line was stopped.
	 * This is the only honest source for "is the target visible" - it is a real trace, not a
	 * flag, so it starts telling the truth the moment something is put in the way.
	 */
	bool TraceLineOfSight(FVector& OutEnd) const;

	/** Runs that trace once per frame and caches it, so the panel and the scene share one answer. */
	void RefreshLineOfSight();

	/** Logs, once, how many things in the level could actually block that trace. */
	void LogOccluderCount();

	/** One line naming the driver, and saying where the weights actually live. */
	FString DescribePolicy() const;

	/** Mean reward over the last few episodes - the only progress number worth watching. */
	float MeanRecentReward() const;

	// ---------------------------------------------------------------------
	// Watching. Only compiled into the actor once; inert unless a mode enables it.
	// ---------------------------------------------------------------------

	/** Drops a floor, two directional lights and an angled perspective camera into the world. */
	void SetupDemoScene();

	/** Tints a mesh component through a dynamic material instance. */
	void ApplyMarkerColor(UStaticMeshComponent* Component, const FLinearColor& Color);

	/**
	 * Claims the player camera and hides the engine's default pawn, once each.
	 * Called every frame from every visual path: a -game launch spawns its controller a
	 * frame or two after BeginPlay, so there is no point at which it is safe to do this
	 * exactly once and be sure it stuck.
	 */
	void EnsureViewTarget();

	/** Per-frame visual upkeep: camera, pawn, marker positions. No-op when visuals are off. */
	void UpdateVisuals();

	/** Pushes AgentPos / TargetPos onto the marker components. */
	void UpdateMarkers();

	/** Advances a watched chase by however many whole steps are currently due. */
	void StepWatched(float DeltaSeconds);

	/** One step of the scripted chase, routed through Observe/Act like any other driver. */
	void AdvanceDemoEpisode();

	/** Fills OutAction with the direction that walks straight at the target. */
	void WriteGreedyAction(FInstancedStruct& OutAction) const;

	/** One step of the random baseline, routed through Observe/Act like any other driver. */
	void AdvanceRandomEpisode();

	/** Fills OutAction with a uniform random direction at full stick. */
	void WriteRandomAction(FInstancedStruct& OutAction);

	/** True while a -PursuitStartAt deadline is still in the future. */
	bool IsWaitingForGo() const;

	/** Left-over time from the previous frame, so the step rate is frame-rate independent. */
	float DemoStepAccumulator = 0.0f;

	/** Counts down while the last frame of an episode is held on screen. */
	float DemoPauseRemaining = 0.0f;

	/** True between the end of an episode and the reset that follows the on-screen hold. */
	bool bAwaitingReset = false;

	/** The player camera is claimed once, as soon as a controller exists. */
	bool bViewTargetSet = false;

	/** The engine's default pawn is hidden once, for the same reason the camera is claimed. */
	bool bPlayerPawnHidden = false;
};
