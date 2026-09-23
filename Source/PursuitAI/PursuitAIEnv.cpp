// Copyright Epic Games, Inc. All Rights Reserved.

#include "PursuitAIEnv.h"

#include "PursuitAI.h"

#include "Camera/CameraComponent.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/DirectionalLight.h"
#include "Engine/Engine.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Misc/App.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "UObject/ConstructorHelpers.h"

#include "DrawDebugHelpers.h"
#include "Points/BoxPoint.h"
#include "Spaces/BoxSpace.h"

#include "GymConnectors/gRPC/gRPCGymConnector.h"

// Inference side. Included here rather than in the header so the NNE header tree stays
// out of every translation unit that only wants the actor.
#include "NNEModelData.h"
#include "Policies/NNEPolicy.h"

// windows.h defines GetObject as a macro (-> GetObjectW / GetObjectA), which rewrites the
// TScriptInterface::GetObject() calls inside Schola's SimpleStepper.h into nonsense:
//
//   error C2039: "GetObjectW": is not a member of "TScriptInterface<IAgent>"
//
// The plugin's own module gets away with it because its include order undefines the macro
// first; any external module that includes this header directly has to do the same.
#ifdef GetObject
#undef GetObject
#endif
#include "Steppers/SimpleStepper.h"

namespace PursuitAIEnvConstants
{
	/** The action is a direction in [-1, 1]^3. */
	constexpr int32 NumActionDimensions = 3;

	/**
	 * Added to the seed before seeding an episode's ACTION stream, so the walk and the
	 * spawn layout are not drawn from the same numbers.
	 *
	 * Arbitrary but fixed and deliberately not round: a round offset invites collisions
	 * with plausible seeds, and the whole point of the offset is that no seed a person
	 * might type gives the two streams the same state.
	 */
	constexpr int32 ActionStreamSeedOffset = 1000003;

	/**
	 * The observation is relative XYZ, own XYZ, then relative velocity XYZ.
	 *
	 * The velocity half is what makes a moving target learnable: from two positions in a row
	 * a policy can in principle infer motion, but a stateless feed-forward net sees one
	 * observation at a time, so the relative position alone presents "a target at bearing X"
	 * with no way to tell "and closing" from "and running away". Supplying it directly is
	 * the difference between a task that needs memory and one that does not.
	 */
	constexpr int32 NumObservations = 9;

	/** Keep the agent and target away from each other at spawn so an episode is never trivial. */
	constexpr int32 SpawnAttempts = 32;

	/**
	 * Radius of the agent's ball, in cm. The engine sphere mesh is 100 cm across, so the
	 * component scale is this divided by 50.
	 *
	 * Presentation only - the simulation's agent is a point. It is deliberately smaller
	 * than CatchRadius so that at the moment of capture the green ball is fully inside the
	 * red one: the capture condition is then something you can literally watch happen
	 * rather than a number in a log.
	 */
	constexpr float AgentVisualRadius = 25.0f;

	/**
	 * How many finished episodes the panel averages over. Long enough that the number stops
	 * jumping every episode, short enough that it still moves within one screen of watching:
	 * at the measured ~1.2 episodes/second that is a ~17 second window.
	 */
	constexpr int32 RecentEpisodeWindow = 20;
}

APursuitAIEnv::APursuitAIEnv()
{
	PrimaryActorTick.bCanEverTick = true;

	if (!RootComponent)
	{
		USceneComponent* SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
		RootComponent = SceneRoot;
	}

	// ---------------------------------------------------------------------------
	// Visuals
	// ---------------------------------------------------------------------------
	// Built on every instance so a level never has to be re-authored, but left invisible
	// until a watching drive mode asks for them: a headless training run renders nothing
	// anyway, and keeping one actor class avoids a second environment definition to
	// maintain - which is the whole point of routing training and inference through the
	// same helpers.

	ArenaFloor = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("ArenaFloor"));
	ArenaFloor->SetupAttachment(RootComponent);
	ArenaFloor->SetMobility(EComponentMobility::Movable);
	ArenaFloor->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	ArenaFloor->SetGenerateOverlapEvents(false);
	ArenaFloor->SetCastShadow(false); // it is the surface the shadows land on

	AgentMarker = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("AgentMarker"));
	AgentMarker->SetupAttachment(RootComponent);
	AgentMarker->SetMobility(EComponentMobility::Movable);
	AgentMarker->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	AgentMarker->SetGenerateOverlapEvents(false);
	// Casting a shadow is what turns "a ball somewhere in a box" into "a ball at a height";
	// without it the perspective camera alone is a weak depth cue.
	AgentMarker->SetCastShadow(true);

	TargetMarker = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("TargetMarker"));
	TargetMarker->SetupAttachment(RootComponent);
	TargetMarker->SetMobility(EComponentMobility::Movable);
	TargetMarker->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	TargetMarker->SetGenerateOverlapEvents(false);
	TargetMarker->SetCastShadow(true);

	DemoCamera = CreateDefaultSubobject<UCameraComponent>(TEXT("DemoCamera"));
	DemoCamera->SetupAttachment(RootComponent);

	// Engine primitives, so the project needs no imported assets of its own. Sphere is
	// 100 cm across and Plane is 100x100 cm; SetupDemoScene scales them to the arena.
	static ConstructorHelpers::FObjectFinder<UStaticMesh> SphereMesh(TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	if (SphereMesh.Succeeded())
	{
		AgentMarker->SetStaticMesh(SphereMesh.Object);
		TargetMarker->SetStaticMesh(SphereMesh.Object);
	}

	static ConstructorHelpers::FObjectFinder<UStaticMesh> PlaneMesh(TEXT("/Engine/BasicShapes/Plane.Plane"));
	if (PlaneMesh.Succeeded())
	{
		ArenaFloor->SetStaticMesh(PlaneMesh.Object);
	}

	// The connector is the object that owns the gRPC server and drives the Python trainer.
	// URPCGymConnector defaults to 127.0.0.1:8000; `-ScholaPort=<n>` on the command line
	// overrides the port, which is how the Schola simulators pass their chosen port in.
	Connector = CreateDefaultSubobject<URPCGymConnector>(TEXT("ScholaConnector"));
}

void APursuitAIEnv::BeginPlay()
{
	// One level, three drivers. The switch is a command line flag specifically so the
	// saved level never has to change between training, inference and watching.
	if (FParse::Param(FCommandLine::Get(), TEXT("PursuitRandom")))
	{
		DriveMode = EPursuitDriveMode::Random;
	}
	else if (FParse::Param(FCommandLine::Get(), TEXT("PursuitDemo")))
	{
		DriveMode = EPursuitDriveMode::Demo;
	}
	else if (FParse::Param(FCommandLine::Get(), TEXT("PursuitInference")))
	{
		DriveMode = EPursuitDriveMode::Inference;
	}
	else
	{
		DriveMode = EPursuitDriveMode::Train;
	}

	FString ModelOverride;
	if (FParse::Value(FCommandLine::Get(), TEXT("PursuitModel="), ModelOverride) && !ModelOverride.IsEmpty())
	{
		InferenceModelPath = ModelOverride;
	}

	// Stage overrides. Everything defaults to the training rig, so a launch that says
	// nothing about staging behaves exactly as it always did; the city stage has to be
	// asked for by name.
	//
	// -PursuitArenaAt is comma-safe on purpose: it is three coordinates and FParse::Value
	// stops at a comma unless told not to, so the plain float overload would silently
	// deliver "X" and drop Y and Z - the same failure -PursuitPlayerAt hit first.
	FString ArenaAtText;
	if (FParse::Value(FCommandLine::Get(), TEXT("PursuitArenaAt="), ArenaAtText, false))
	{
		TArray<FString> Parts;
		ArenaAtText.ParseIntoArray(Parts, TEXT(","), true);
		if (Parts.Num() == 3)
		{
			ArenaCenter = FVector(
				FCString::Atof(*Parts[0]), FCString::Atof(*Parts[1]), FCString::Atof(*Parts[2]));
			UE_LOG(LogPursuitAI, Log, TEXT("PursuitAIEnv: arena moved to (%.0f, %.0f, %.0f) by -PursuitArenaAt"),
				ArenaCenter.X, ArenaCenter.Y, ArenaCenter.Z);
		}
		else
		{
			// Refuse out loud rather than guess. A malformed coordinate that half-applied
			// would put the arena under the road, and the film would show balls sinking
			// into asphalt.
			UE_LOG(LogPursuitAI, Error,
				TEXT("PursuitAIEnv: could not parse -PursuitArenaAt='%s' (want X,Y,Z); arena stays at origin"),
				*ArenaAtText);
		}
	}
	float ParsedHeight = 0.0f;
	if (FParse::Value(FCommandLine::Get(), TEXT("PursuitArenaHeight="), ParsedHeight))
	{
		// Below 10 cm the Z observation never leaves a rounding error's range, which a
		// policy reads as "Z does not matter" whether or not that is true of the task.
		ArenaHalfHeight = FMath::Max(ParsedHeight, 10.0f);
		UE_LOG(LogPursuitAI, Log, TEXT("PursuitAIEnv: arena half-height set to %.0f by -PursuitArenaHeight"),
			ArenaHalfHeight);
	}
	if (FParse::Value(FCommandLine::Get(), TEXT("PursuitStage="), ArenaAtText, false)
		&& ArenaAtText == TEXT("city"))
	{
		bCityStage = true;
		UE_LOG(LogPursuitAI, Log,
			TEXT("PursuitAIEnv: city stage - no env floor, no env lights, camera vertical above the arena"));
	}

	// Target behaviour, switchable without touching the saved level so an A/B of "chase a
	// fixed point" against "run down a runner" is a pair of launches rather than a pair of
	// map edits. Both flags given is a contradiction; the property wins and says so.
	const bool bForceFlee = FParse::Param(FCommandLine::Get(), TEXT("PursuitFlee"));
	const bool bForceStatic = FParse::Param(FCommandLine::Get(), TEXT("PursuitStatic"));
	if (bForceFlee && bForceStatic)
	{
		UE_LOG(LogPursuitAI, Warning,
			TEXT("PursuitAIEnv: both -PursuitFlee and -PursuitStatic given; keeping the configured "
			     "TargetPolicy (%s)"), TargetPolicy == EPursuitTargetPolicy::Flee ? TEXT("Flee") : TEXT("Static"));
	}
	else if (bForceFlee)
	{
		TargetPolicy = EPursuitTargetPolicy::Flee;
	}
	else if (bForceStatic)
	{
		TargetPolicy = EPursuitTargetPolicy::Static;
	}

	// Logged unconditionally, because this is the one setting whose value changes what the
	// task *is* rather than merely how it is run, and a recorded result is worthless if you
	// cannot afterwards tell which of the two it was measured against.
	UE_LOG(LogPursuitAI, Log, TEXT("PursuitAIEnv: target policy = %s (speed scale %.2f)"),
		TargetPolicy == EPursuitTargetPolicy::Flee ? TEXT("Flee") : TEXT("Static"),
		TargetSpeedScale);

	if (InferenceModelPath.IsEmpty())
	{
		// Resolved here rather than in a property initialiser so the default follows the
		// project wherever it is checked out, and lines up with where the headless export
		// writes without anyone having to keep two paths in sync.
		InferenceModelPath = FPaths::ConvertRelativePathToFull(
			FPaths::ProjectDir() / TEXT("checkpoints/policy.onnx"));
	}

	// -PursuitSeed=12345 pins the spawn sequence from the command line. It exists so that
	// "the headless run and the windowed run agree" is something a reader can check rather
	// than something they have to take on faith: same seed, same launch, and every episode
	// outcome should come out identical. Without it the claim is not even testable - Seed
	// defaults to 0, and 0 means FMath::Rand().
	int32 SeedOverride = 0;
	if (FParse::Value(FCommandLine::Get(), TEXT("PursuitSeed="), SeedOverride) && SeedOverride != 0)
	{
		Seed = SeedOverride;
	}

	Rng.Initialize(Seed != 0 ? Seed : FMath::Rand());

	// Same seed, different stream. See ActionRng in the header for why it cannot simply be
	// Rng: two panes take different numbers of steps the moment their policies differ, and
	// sharing one stream would let that leak into where the next episode's spawn lands.
	ActionRng.Initialize(Seed != 0 ? Seed : FMath::Rand());

	// --- the switches a controlled comparison needs, and which are not properties -------
	//
	// All four exist for one reason: a side-by-side clip is only evidence if the two halves
	// differ in exactly one thing. Rate and step budget have to be identical or the panes
	// finish episodes at different times and the comparison becomes two unrelated scenes;
	// the label has to be inside the picture or it has to be aligned afterwards; and the
	// start gate is what makes "they began together" a fact rather than a hope.
	int32 MaxStepsOverride = 0;
	if (FParse::Value(FCommandLine::Get(), TEXT("PursuitMaxSteps="), MaxStepsOverride)
		&& MaxStepsOverride > 0)
	{
		MaxSteps = MaxStepsOverride;
	}

	float RateOverride = 0.0f;
	if (FParse::Value(FCommandLine::Get(), TEXT("PursuitRate="), RateOverride)
		&& RateOverride > 0.0f)
	{
		DemoStepsPerSecond = RateOverride;
	}

	int32 TrailMaxOverride = 0;
	if (FParse::Value(FCommandLine::Get(), TEXT("PursuitTrailMax="), TrailMaxOverride)
		&& TrailMaxOverride > 0)
	{
		TrailMax = TrailMaxOverride;
	}

	// Comma-safe, and with one extra step. FParse::Value stops at a comma by default, so
	// "RANDOM BASELINE, STEP 0" would arrive as "RANDOM BASELINE" - passing false fixes
	// that. It still stops at a space even then, because that is what WhiteSpaceChars means
	// and there is no switch for it, so a label has to be passed with underscores and
	// reassembled here. Worth the two lines: a label with a space in it is the readable one,
	// and the alternative is quoting the whole command line per pane.
	FString LabelOverride;
	if (FParse::Value(FCommandLine::Get(), TEXT("PursuitPaneLabel="), LabelOverride, false))
	{
		PaneLabel = LabelOverride.Replace(TEXT("_"), TEXT(" "));
	}

	double StartAtOverride = 0.0;
	if (FParse::Value(FCommandLine::Get(), TEXT("PursuitStartAt="), StartAtOverride)
		&& StartAtOverride > 0.0)
	{
		StartAtEpoch = StartAtOverride;
	}

	UE_LOG(LogPursuitAI, Log,
		TEXT("PursuitAIEnv: drive=%s  rate=%.2f steps/s  maxSteps=%d  seed=%d  startAt=%s"),
		*DescribePolicy(), DemoStepsPerSecond, MaxSteps, Seed,
		StartAtEpoch > 0.0 ? *FString::Printf(TEXT("%.0f"), StartAtEpoch) : TEXT("now"));

	// Identity for the overhead label. There is one chaser per environment today, but a level
	// holding two of them would number itself here instead of both claiming to be #0. The
	// index is positional and therefore only stable within a run, which is all a debug label
	// needs to be.
	AgentId = 0;
	for (TActorIterator<APursuitAIEnv> It(GetWorld()); It; ++It)
	{
		if (*It == this)
		{
			break;
		}
		++AgentId;
	}

	// -PursuitVisual is orthogonal to the mode: it means "show the scenery", whatever is
	// driving. On top of that, a training run gets the scenery automatically whenever the
	// launch can actually draw something - Schola's --headless turns into -nullRHI, where
	// FApp::CanEverRender() is false and there is nothing to build, while a --WINDOWED
	// launch has a viewport and gets the full scene. So "watch it learn" needs no flag at
	// all; dropping --headless is the whole change.
	//
	// Training stays scenery-free only when it is headless, which is also the only time a
	// rendered frame would have nothing to say: at a few hundred steps a second the markers
	// would jump episode to episode between frames, and the renderer would eat throughput.
	const bool bForceVisuals = FParse::Param(FCommandLine::Get(), TEXT("PursuitVisual"));
	bVisualsEnabled = (DriveMode != EPursuitDriveMode::Train) || bForceVisuals || FApp::CanEverRender();

	// The debug view rides along with the scenery. Both exist to be looked at and both are
	// drawn into the same frames, so tying them to one rule keeps them from disagreeing -
	// a headless run gets neither, a windowed one gets both. The two overrides are for the
	// cases where "both" is the wrong answer: a clean capture for a demo video, or a debug
	// view over a launch that would otherwise stay bare.
	bDrawDebug = bVisualsEnabled;
	if (FParse::Param(FCommandLine::Get(), TEXT("PursuitNoDebug"))) { bDrawDebug = false; }
	if (FParse::Param(FCommandLine::Get(), TEXT("PursuitDebug"))) { bDrawDebug = true; }

	// Logged after the two are settled, not where AgentId is worked out above: reporting a
	// field before it has been assigned prints its default and reads like a bug. The seed is
	// the one the stream actually took, not the property, so a run that fell back to
	// FMath::Rand() says so instead of quietly claiming to be reproducible.
	UE_LOG(LogPursuitAI, Log, TEXT("PursuitAIEnv: agent #%d, scenery %s, debug view %s, spawn seed %d%s"),
		AgentId,
		bVisualsEnabled ? TEXT("on") : TEXT("off"),
		bDrawDebug ? TEXT("on") : TEXT("off"),
		Rng.GetInitialSeed(),
		bDrawDebug ? TEXT(" (-PursuitNoDebug hides the panel and the world markers)") : TEXT(""));

	if (DriveMode == EPursuitDriveMode::Train)
	{
		if (!bVisualsEnabled)
		{
			if (AgentMarker) { AgentMarker->SetVisibility(false); }
			if (TargetMarker) { TargetMarker->SetVisibility(false); }
			if (ArenaFloor) { ArenaFloor->SetVisibility(false); }
		}

		// AGymConnectorManager::BeginPlay collects every actor implementing a Schola
		// environment interface (this actor included) and initializes the connector,
		// which is what calls InitializeEnvironment_Implementation below.
		Super::BeginPlay();

		// Only now: Super::BeginPlay is what opens the gRPC server and calls
		// InitializeEnvironment, so the arena size is final by the time the floor is scaled
		// to it. Spawning lights and a camera earlier would size a floor for default values.
		if (bVisualsEnabled)
		{
			SetupDemoScene();
			UpdateVisuals();
		}

		UE_LOG(LogPursuitAI, Log,
			TEXT("PursuitAIEnv: training%s. arena=%.0fx%.0fx%.0f step=%.0f catch=%.0f maxSteps=%d obs=Box(%d) action=Box(%d) connector=%s"),
			bVisualsEnabled ? TEXT(" (visual)") : TEXT(""),
			ArenaHalfSize, ArenaHalfSize, ArenaHalfHeight, MoveStep, CatchRadius, MaxSteps,
			PursuitAIEnvConstants::NumObservations, PursuitAIEnvConstants::NumActionDimensions,
			Connector ? *Connector->GetClass()->GetName() : TEXT("<none>"));
		return;
	}

	// AGymConnectorManager::BeginPlay is skipped deliberately: it would open the gRPC server
	// and then sit there waiting for a trainer that is never going to call.
	AActor::BeginPlay();

	SetupDemoScene();
	BeginEpisode();

	if (DriveMode == EPursuitDriveMode::Inference)
	{
		SetupInference();
	}

	// One-shot scene dump. A watched run is the only time the visuals matter, and when a
	// render disagrees with the code the component list is what settles it: which meshes
	// exist, where they actually are, and whether anything is drawing a mesh nobody asked
	// for. Cheap enough to leave in - it runs once per watched launch.
	{
		UE_LOG(LogPursuitAI, Display, TEXT("PursuitAI: scene dump for %s"), *GetName());
		TArray<UActorComponent*> Components;
		GetComponents(Components);
		for (UActorComponent* Component : Components)
		{
			if (const UStaticMeshComponent* Mesh = Cast<UStaticMeshComponent>(Component))
			{
				UE_LOG(LogPursuitAI, Display,
					TEXT("  %s class=%s mesh=%s loc=%s scale=%s visible=%d"),
					*Mesh->GetName(), *Mesh->GetClass()->GetName(),
					Mesh->GetStaticMesh() ? *Mesh->GetStaticMesh()->GetName() : TEXT("<none>"),
					*Mesh->GetComponentLocation().ToCompactString(),
					*Mesh->GetComponentScale().ToCompactString(),
					Mesh->IsVisible() ? 1 : 0);
			}
			else if (Component)
			{
				UE_LOG(LogPursuitAI, Display, TEXT("  %s class=%s"),
					*Component->GetName(), *Component->GetClass()->GetName());
			}
		}

		// The other half of the question: anything else in the world that could put a mesh
		// on screen. The level is generated and meant to hold this actor alone.
		UE_LOG(LogPursuitAI, Display, TEXT("PursuitAI: actors in world"));
		for (TActorIterator<AActor> It(GetWorld()); It; ++It)
		{
			UE_LOG(LogPursuitAI, Display, TEXT("  %s class=%s"),
				*It->GetName(), *It->GetClass()->GetName());
		}
	}

	UE_LOG(LogPursuitAI, Log,
		TEXT("PursuitAIEnv: watching. mode=%s arena=%.0fx%.0fx%.0f step=%.0f catch=%.0f maxSteps=%d rate=%.1f steps/s"),
		DriveMode == EPursuitDriveMode::Inference ? TEXT("inference") : TEXT("demo"),
		ArenaHalfSize, ArenaHalfSize, ArenaHalfHeight, MoveStep, CatchRadius, MaxSteps,
		DemoStepsPerSecond);
}

void APursuitAIEnv::Tick(float DeltaSeconds)
{
	if (DriveMode == EPursuitDriveMode::Train)
	{
		// Training: the connector manager owns the tick and pumps the environment.
		Super::Tick(DeltaSeconds);

		// ...and a visual training run then repaints. There is no throttling here, unlike
		// StepWatched: the step rate is the trainer's to choose, and these markers are
		// showing wherever the simulation happens to be when the frame is drawn. At speed
		// that reads as the target flickering between episodes, which is the honest picture
		// of what training looks like.
		UpdateVisuals();
		return;
	}

	StepWatched(DeltaSeconds);
}

// ---------------------------------------------------------------------------
// IAgent - the inference side. Driven by a stepper over a local policy.
// ---------------------------------------------------------------------------

EAgentStatus APursuitAIEnv::GetStatus_Implementation()
{
	return AgentStatus;
}

void APursuitAIEnv::SetStatus_Implementation(EAgentStatus NewStatus)
{
	AgentStatus = NewStatus;
}

void APursuitAIEnv::Define_Implementation(FInteractionDefinition& OutInteractionDefinition)
{
	DefineSpaces(OutInteractionDefinition);
}

void APursuitAIEnv::Observe_Implementation(FInstancedStruct& OutObservations)
{
	// IAgent hands over a type-erased FInstancedStruct, while the core works in the typed
	// TInstancedStruct<FPoint> that the training side also uses. The two are layout-compatible
	// by design, which is exactly what ToTypedInstancedStruct relies on - they are not
	// related by inheritance and cannot simply be assigned.
	BuildObservation(ToTypedInstancedStruct<FPoint>(OutObservations));
}

void APursuitAIEnv::Act_Implementation(const FInstancedStruct& InAction)
{
	// A stepper knows nothing about episodes - it just hands over actions. Everything that
	// has to happen between two decisions therefore lives here, or an inference run would
	// walk the agent into the target and then leave it sitting there forever.
	//
	// The target moves after the agent and before the distance is read, in that order, so
	// this frame scores the same world state the trainer would have scored. Doing it the
	// other way round would let the agent's step be measured against a target that had
	// already fled one step further than it could see.
	ApplyAction(InAction);
	AdvanceStepCounter();
	StepTarget();

	const float Distance = DistanceToTarget();
	const bool bCaught = Distance <= CatchRadius;
	const bool bOutOfTime = !bCaught && CurrentStep >= MaxSteps;

	// Scored even though no trainer is listening. The reward is the same function the
	// training path uses, so the number on the panel during an inference run is directly
	// comparable with the number the trainer was optimising - which is the whole point of
	// watching a trained policy run.
	ScoreStep(Distance, bCaught, bOutOfTime);

	if (bCaught || bOutOfTime)
	{
		FinishWatchedEpisode(bCaught);
	}
}

// ---------------------------------------------------------------------------
// ISingleAgentScholaEnvironment - the training side. Driven over gRPC.
// ---------------------------------------------------------------------------

void APursuitAIEnv::InitializeEnvironment_Implementation(FInteractionDefinition& OutAgentDefinition)
{
	// Same DefineSpaces call IAgent::Define makes. The trainer and the ONNX export are
	// therefore guaranteed to agree on the layout, which is the failure this layering
	// exists to make impossible.
	DefineSpaces(OutAgentDefinition);
}

void APursuitAIEnv::SeedEnvironment_Implementation(int InSeed)
{
	Seed = InSeed;
	Rng.Initialize(InSeed);
	UE_LOG(LogPursuitAI, Log, TEXT("PursuitAIEnv: SeedEnvironment(%d)"), InSeed);
}

void APursuitAIEnv::SetEnvironmentOptions_Implementation(const TMap<FString, FString>& InOptions)
{
	// Nothing to configure at runtime yet; log the values so they are visible in the
	// trainer's first round-trip and can be wired up when the 1v1 task lands.
	for (const TPair<FString, FString>& Option : InOptions)
	{
		UE_LOG(LogPursuitAI, Log, TEXT("PursuitAIEnv: option %s=%s"), *Option.Key, *Option.Value);
	}
}

void APursuitAIEnv::Reset_Implementation(FInitialAgentState& OutAgentState)
{
	BeginEpisode();
	BuildObservation(OutAgentState.Observations);

	OutAgentState.Info.Add(TEXT("episode"), FString::FromInt(EpisodeCount));
}

void APursuitAIEnv::Step_Implementation(const FInstancedStruct& InAction, FAgentState& OutAgentState)
{
	ApplyAction(InAction);
	AdvanceStepCounter();

	// StepTarget, not a raw move, and in the same order as Act_Implementation. Both drivers
	// run the identical world update; the only difference between them is where the action
	// came from. If these two ever diverge, a policy trained here would be evaluated against
	// a different world in a watched run, which is precisely the train/deploy drift this
	// actor's layering exists to prevent.
	StepTarget();

	const float NewDistance = DistanceToTarget();

	const bool bCaught = NewDistance <= CatchRadius;
	const bool bOutOfTime = !bCaught && CurrentStep >= MaxSteps;

	OutAgentState.Reward = ScoreStep(NewDistance, bCaught, bOutOfTime);
	OutAgentState.bTerminated = bCaught;
	OutAgentState.bTruncated = bOutOfTime;

	BuildObservation(OutAgentState.Observations);

	OutAgentState.Info.Add(TEXT("step"), FString::FromInt(CurrentStep));
	OutAgentState.Info.Add(TEXT("distance"), FString::SanitizeFloat(NewDistance));
	OutAgentState.Info.Add(TEXT("travelled"), FString::SanitizeFloat(DistanceTravelled));
	// The episode total, not just this step's reward. It is what the panel shows and what a
	// curve actually wants to be plotted against, so the trainer can read it for free.
	OutAgentState.Info.Add(TEXT("total_reward"), FString::SanitizeFloat(TotalReward));

	if (OutAgentState.bTerminated || OutAgentState.bTruncated)
	{
		// No reset here: the trainer decides when the next episode starts, and it does so
		// by calling Reset_Implementation.
		FinishEpisodeStats(bCaught);
		if (bLogEpisodes)
		{
			UE_LOG(LogPursuitAI, Log,
				TEXT("PursuitAIEnv: episode %d %s in %d steps, travelled %.0f cm, reward %.3f (total %.3f)"),
				EpisodeCount, bCaught ? TEXT("CAUGHT") : TEXT("TIMEOUT"), CurrentStep, DistanceTravelled,
				OutAgentState.Reward, TotalReward);
		}
	}
}

// ---------------------------------------------------------------------------
// The shared layer. Exactly one implementation each, used by every driver.
// ---------------------------------------------------------------------------

void APursuitAIEnv::DefineSpaces(FInteractionDefinition& OutDefinition) const
{
	// Both halves are [-1, 1]: BuildObservation is written so it never leaves that range,
	// and ApplyActionVector clamps so it never leaves it either.
	TInstancedStruct<FSpace>& ObsSpaceInst = OutDefinition.ObsSpaceDefn;
	ObsSpaceInst.InitializeAs<FBoxSpace>();
	FBoxSpace& ObsSpace = ObsSpaceInst.GetMutable<FBoxSpace>();
	for (int32 Dim = 0; Dim < PursuitAIEnvConstants::NumObservations; ++Dim)
	{
		ObsSpace.Add(-1.0f, 1.0f);
	}

	TInstancedStruct<FSpace>& ActionSpaceInst = OutDefinition.ActionSpaceDefn;
	ActionSpaceInst.InitializeAs<FBoxSpace>();
	FBoxSpace& ActionSpace = ActionSpaceInst.GetMutable<FBoxSpace>();
	for (int32 Dim = 0; Dim < PursuitAIEnvConstants::NumActionDimensions; ++Dim)
	{
		ActionSpace.Add(-1.0f, 1.0f);
	}

	UE_LOG(LogPursuitAI, Log,
		TEXT("PursuitAIEnv: DefineSpaces -> obs=Box(%d)[-1,1], action=Box(%d)[-1,1]"),
		PursuitAIEnvConstants::NumObservations, PursuitAIEnvConstants::NumActionDimensions);
}

void APursuitAIEnv::BuildObservation(TInstancedStruct<FPoint>& OutObservation) const
{
	const FVector Extent = GetArenaHalfExtent();

	// Two scales, both picked so the result lands in [-1, 1] by construction, with no
	// clamping and therefore no saturation:
	//   the relative half spans at most the full arena   -> divide by 2 * half extent
	//   the self half spans at most one half extent      -> divide by the half extent
	// Clamping instead would blind the policy exactly when it is furthest from the target,
	// which is right after every reset.
	const FVector SafeExtent(
		FMath::Max(Extent.X, KINDA_SMALL_NUMBER),
		FMath::Max(Extent.Y, KINDA_SMALL_NUMBER),
		FMath::Max(Extent.Z, KINDA_SMALL_NUMBER));
	const FVector FullExtent = SafeExtent * 2.0;

	const FVector Delta = TargetPos - AgentPos;
	const FVector Relative = Delta / FullExtent;
	// Self is centre-relative, not world: on a staged arena the world origin is somewhere
	// else entirely, and an observation that leaked world coordinates would change every
	// value when the stage moved while the task itself had not.
	const FVector Self = (AgentPos - ArenaCenter) / SafeExtent;

	// Relative velocity, differenced from the last step rather than tracked separately, so
	// the observation cannot disagree with the motion that actually happened. The divisor is
	// the largest relative speed one step can produce - both moving at full deflection in
	// opposite directions, i.e. twice MoveStep - which puts the result in [-1, 1] with no
	// clamp. A static target gives exactly zero here, which is a true and useful signal
	// rather than a dead input: it is how the policy tells "it is not moving" apart from
	// "it is moving and I do not know which way".
	constexpr float RelativeVelocityScale = 2.0f;
	const FVector TargetVelocity = TargetPos - PrevTargetPos;
	const FVector AgentVelocity = AgentPos - PrevAgentPos;
	const FVector RelativeVelocity =
		(TargetVelocity - AgentVelocity) / (FMath::Max(MoveStep, KINDA_SMALL_NUMBER) * RelativeVelocityScale);

	OutObservation.InitializeAs<FBoxPoint>();
	FBoxPoint& Box = OutObservation.GetMutable<FBoxPoint>();
	Box.Values = {
		static_cast<float>(Relative.X),
		static_cast<float>(Relative.Y),
		static_cast<float>(Relative.Z),
		static_cast<float>(Self.X),
		static_cast<float>(Self.Y),
		static_cast<float>(Self.Z),
		static_cast<float>(RelativeVelocity.X),
		static_cast<float>(RelativeVelocity.Y),
		static_cast<float>(RelativeVelocity.Z),
	};
	Box.Shape = { PursuitAIEnvConstants::NumObservations };
}

FVector APursuitAIEnv::ApplyAction(const FInstancedStruct& InAction)
{
	FVector Direction = FVector::ZeroVector;

	if (InAction.IsValid())
	{
		if (const FBoxPoint* Box = InAction.GetPtr<FBoxPoint>())
		{
			if (Box->Values.Num() >= PursuitAIEnvConstants::NumActionDimensions)
			{
				Direction = FVector(Box->Values[0], Box->Values[1], Box->Values[2]);
			}
			else
			{
				UE_LOG(LogPursuitAI, Warning,
					TEXT("PursuitAIEnv: action carried %d values, expected %d; treated as idle"),
					Box->Values.Num(), PursuitAIEnvConstants::NumActionDimensions);
			}
		}
		else
		{
			// Loud on purpose. A payload mismatch means the space advertised in DefineSpaces
			// and the space the trainer or the ONNX export assumed have diverged - exactly
			// the failure this actor's layering exists to prevent, so it should never be
			// swallowed quietly.
			UE_LOG(LogPursuitAI, Warning,
				TEXT("PursuitAIEnv: unexpected action payload type %s; treated as idle"),
				InAction.GetScriptStruct() ? *InAction.GetScriptStruct()->GetName() : TEXT("<invalid>"));
		}
	}

	ApplyActionVector(Direction);
	return Direction;
}

void APursuitAIEnv::ApplyActionVector(const FVector& InDirection)
{
	// Kept for the debug view: what the policy asked for, before anything was clamped.
	LastActionRequested = InDirection;

	FVector Direction = InDirection;
	if (Direction.ContainsNaN())
	{
		UE_LOG(LogPursuitAI, Warning, TEXT("PursuitAIEnv: action contained NaN; treated as idle"));
		Direction = FVector::ZeroVector;
	}

	// A squashed-Gaussian policy can still hand over something outside the declared box.
	// Clamping the *magnitude* rather than each component is what keeps a single step worth
	// at most MoveStep in any direction, which is the assumption the reward shaping rests on.
	Direction = Direction.GetClampedToMaxSize(1.0);
	LastActionApplied = Direction;

	const FVector Before = AgentPos;
	const FVector Intended = AgentPos + Direction * MoveStep;
	AgentPos = ClampToArena(Intended);
	DistanceTravelled += static_cast<float>(FVector::Distance(Before, AgentPos));

	// The difference between those two positions is the arena boundary eating part of the
	// step, and it is the only place in this environment where an action is refused. Kept
	// because it is worth drawing: "I am pushing into a wall" is otherwise invisible from
	// outside - the agent simply moves less than it asked to.
	LastStepStart = Before;
	LastIntendedOffset = Intended - Before;
	LastActualOffset = AgentPos - Before;
	bActionClamped = !LastIntendedOffset.Equals(LastActualOffset, 0.01);

	// One point per step, appended from the only place the agent ever moves, so the trail
	// cannot disagree with the simulation about where it has been. The cap drops the oldest
	// rather than the newest: the interesting end of a path is where it is now, and an
	// episode that runs past the cap should still show the approach that ended it.
	Trail.Add(AgentPos);
	if (Trail.Num() > TrailMax)
	{
		Trail.RemoveAt(0, Trail.Num() - TrailMax, EAllowShrinking::No);
	}
}

FVector APursuitAIEnv::ClampToArena(const FVector& Position) const
{
	// Centre-relative: the clamp is a box around ArenaCenter, so a stage parked in a city
	// street clamps exactly like the rig at the origin. Position minus centre, clamp, add
	// the centre back - one expression, and nothing downstream can tell the difference.
	const FVector Extent = GetArenaHalfExtent();
	const FVector Local = Position - ArenaCenter;
	return ArenaCenter + FVector(
		FMath::Clamp(Local.X, -Extent.X, Extent.X),
		FMath::Clamp(Local.Y, -Extent.Y, Extent.Y),
		FMath::Clamp(Local.Z, -Extent.Z, Extent.Z));
}

FVector APursuitAIEnv::ComputeEscapeDirection(float DistanceToAgent) const
{
	// Straight away from the chaser. In a bounded box this alone is a losing strategy - run
	// far enough and the wall stops you - which is the point: the chaser wins by *herding*,
	// and a policy that only ever walks at the target's current position never learns that.
	FVector Away = TargetPos - AgentPos;
	Away.Z = 0.0f; // the arena is a slab and the target never needs to climb
	if (!Away.Normalize())
	{
		// Exactly coincident. Any direction is as good as any other; pick one that is not
		// zero so the weave below has a frame to work in.
		Away = FVector::ForwardVector;
	}

	// The weave. A pure "away" vector is a function of the current positions only, so the
	// chaser could intercept it with a plain pursuit law and the task would be solved before
	// it was learned. Rotating the perpendicular of the escape direction by a fixed amount
	// each step makes the *path* depend on the whole history of the episode, which is what
	// turns this into something a policy has to actually track.
	//
	// Scaled by proximity: far apart the target runs clean (and the chaser can see that),
	// close in it weaves hard. A constant swerve would look like a target with a limp.
	const float Proximity = FMath::Clamp(
		1.0f - DistanceToAgent / FMath::Max(ArenaHalfSize, KINDA_SMALL_NUMBER), 0.0f, 1.0f);
	const float Lateral = TargetEvadeGain * Proximity;

	const FVector Perpendicular = FVector::CrossProduct(FVector::UpVector, Away).GetSafeNormal();
	FVector Heading = (Away + Perpendicular * Lateral).GetSafeNormal();
	if (Heading.IsNearlyZero())
	{
		Heading = Away;
	}

	return Heading;
}

FVector APursuitAIEnv::StepTarget()
{
	if (TargetPolicy != EPursuitTargetPolicy::Flee)
	{
		// Static: the original behaviour, untouched. Kept as a real branch rather than a
		// scale of zero so nothing about the old episodes changes - no drift from a multiply
		// by 0 that used to be a no-op, no new ordering of floating point operations.
		TargetHeading = FVector::ZeroVector;
		return FVector::ZeroVector;
	}

	const float Distance = DistanceToTarget();
	const FVector Escape = ComputeEscapeDirection(Distance);

	// Rotate the previous heading towards the new one rather than snapping to it. This is
	// what makes the weave continuous: a target that teleports its heading every step traces
	// a sawtooth that a policy can average out, while one that turns at a bounded rate makes
	// a smooth arc that has to be tracked.
	const FRotator Current = TargetHeading.IsNearlyZero()
		? Escape.Rotation()
		: TargetHeading.Rotation();
	const FRotator Target = Escape.Rotation();
	const FRotator Blended = FMath::RInterpConstantTo(
		Current, Target, static_cast<float>(1.0 / FMath::Max(DemoStepsPerSecond, 0.1)),
		FMath::RadiansToDegrees(TargetEvadeTurnRate) * FMath::Max(DemoStepsPerSecond, 0.1));

	TargetHeading = Blended.Vector();
	TargetHeading.Z = 0.0f;
	if (!TargetHeading.Normalize())
	{
		TargetHeading = Escape;
	}

	const FVector Intended = TargetPos + TargetHeading * (MoveStep * TargetSpeedScale);
	TargetPos = ClampToArena(Intended);
	return TargetHeading * TargetSpeedScale;
}

void APursuitAIEnv::RandomizePositions()
{
	const FVector Extent = GetArenaHalfExtent();
	const FVector SpawnRange = Extent * 0.75;
	const double MinSeparation = static_cast<double>(FMath::Max(CatchRadius * 4.0f, MoveStep * 2.0f));

	for (int32 Attempt = 0; Attempt < PursuitAIEnvConstants::SpawnAttempts; ++Attempt)
	{
		// Local offsets first, centred on the origin, then shifted into the arena. The RNG
		// stream must stay a function of the seed and episode alone - drawing offsets and
		// adding the centre afterwards keeps the spawn layout identical between a rig at
		// the origin and a stage in a city street, which is what makes a clip shot in one
		// comparable to a run logged in the other.
		AgentPos = ArenaCenter + FVector(
			Rng.FRandRange(-SpawnRange.X, SpawnRange.X),
			Rng.FRandRange(-SpawnRange.Y, SpawnRange.Y),
			Rng.FRandRange(-SpawnRange.Z, SpawnRange.Z));
		TargetPos = ArenaCenter + FVector(
			Rng.FRandRange(-SpawnRange.X, SpawnRange.X),
			Rng.FRandRange(-SpawnRange.Y, SpawnRange.Y),
			Rng.FRandRange(-SpawnRange.Z, SpawnRange.Z));

		if (FVector::Distance(AgentPos, TargetPos) >= MinSeparation)
		{
			return;
		}
	}

	// Fall back to a guaranteed-separated layout if random sampling kept colliding.
	AgentPos = ArenaCenter - SpawnRange;
	TargetPos = ArenaCenter + SpawnRange;
}

void APursuitAIEnv::BeginEpisode()
{
	CurrentStep = 0;
	DistanceTravelled = 0.0f;
	DemoStepAccumulator = 0.0f;

	// Both streams are re-seeded per episode, from the episode's own number.
	//
	// Sequential seeding - one Initialize at BeginPlay and nothing after - makes the two
	// panes agree on the FIRST episode and on nothing after it. A pane that catches in four
	// steps has drawn fourteen spawns by the time a pane that never catches has drawn one,
	// so from episode two onwards they are being tested on different starting positions and
	// the comparison is measuring the RNG's consumption rate as much as the policy. Measured
	// rather than imagined: in the 30 s verification clip the greedy pane finished 14
	// episodes and the random one was still inside its first.
	//
	// Keyed on the episode index, every pane running episode N gets episode N's spawn no
	// matter how many episodes anybody has finished. The two offsets keep the spawn stream
	// and the action stream from being the same numbers, which would make the walk a
	// function of where the episode started.
	Rng.Initialize(Seed != 0 ? Seed + EpisodeIndex : FMath::Rand());
	ActionRng.Initialize(Seed != 0 ? Seed + PursuitAIEnvConstants::ActionStreamSeedOffset + EpisodeIndex
	                               : FMath::Rand());
	RandomizePositions();
	++EpisodeIndex;

	// Logged so "both panes started from the same place" is a thing a reader can check in
	// the log instead of a thing they take on faith from the code. It is the only number in
	// a two-pane comparison that has to match exactly, and it is the easiest one to get
	// wrong silently: a pane that finished fourteen episodes and one that finished none
	// agree on nothing unless the seed is keyed to the episode, which is what the two
	// Initialize calls above are for.
	UE_LOG(LogPursuitAI, Log,
		TEXT("PursuitAIEnv: episode %d started  agent=(%.0f, %.0f, %.0f)  target=(%.0f, %.0f, %.0f)  gap=%.0f cm"),
		EpisodeIndex, AgentPos.X, AgentPos.Y, AgentPos.Z,
		TargetPos.X, TargetPos.Y, TargetPos.Z, DistanceToTarget());

	// The trail starts at the spawn rather than empty, so the first step draws a line with
	// two ends instead of appearing as a single dead point.
	Trail.Reset();
	Trail.Add(AgentPos);

	// The velocity half of the observation is a difference of two positions, so both of the
	// "previous" positions have to be seeded to the freshly randomised ones. Left at zero -
	// where they start life - the first observation of every episode would claim the target
	// had just teleported in from the origin, and a policy is free to act on that.
	PrevAgentPos = AgentPos;
	PrevTargetPos = TargetPos;

	// The escape direction, likewise: it is a rotation, so it needs a frame. Pointing it at
	// the first escape direction rather than keeping last episode's heading stops episode n+1
	// from starting mid-weave inherited from episode n.
	TargetHeading = ComputeEscapeDirection(DistanceToTarget());

	PrevDistance = DistanceToTarget();
	UpdateMarkers();

	// Per-episode reward accounting, and the debug view's picture of the last action. Both
	// have to be cleared together: a stale arrow left over from the previous episode's last
	// step would look like a decision the policy never made.
	CurrentReward = 0.0f;
	TotalReward = 0.0f;
	CurrentClosing = 0.0f;
	LastActionRequested = FVector::ZeroVector;
	LastActionApplied = FVector::ZeroVector;
	LastIntendedOffset = FVector::ZeroVector;
	LastActualOffset = FVector::ZeroVector;
	LastStepStart = AgentPos;
	bActionClamped = false;
}

float APursuitAIEnv::DistanceToTarget() const
{
	return static_cast<float>(FVector::Distance(AgentPos, TargetPos));
}

float APursuitAIEnv::ScoreStep(float NewDistance, bool bCaught, bool bOutOfTime)
{
	// Reward shaping: closing the gap is worth up to +1 per step, backing off costs the same.
	// Dividing by MoveStep keeps the signal independent of the arena scale, and holds because
	// ApplyActionVector clamps one step to at most MoveStep in any direction.
	CurrentClosing = PrevDistance - NewDistance;
	const float Shaping = CurrentClosing / FMath::Max(MoveStep, KINDA_SMALL_NUMBER);
	PrevDistance = NewDistance;

	float Reward = Shaping;
	if (bCaught)
	{
		Reward += GoalReward;
	}
	else if (bOutOfTime)
	{
		Reward += TimeoutPenalty;
	}

	// Recorded here rather than by the caller, so the training path and the watched paths
	// cannot drift into reporting two different "rewards" for the same event.
	CurrentReward = Reward;
	TotalReward += Reward;
	return Reward;
}

void APursuitAIEnv::AdvanceStepCounter()
{
	++CurrentStep;
	// CurrentStep is per-episode and resets at every begin, so it cannot be differenced to
	// get a rate. This one only ever goes up.
	++TotalStepCount;
}

void APursuitAIEnv::FinishEpisodeStats(bool bCaught)
{
	++EpisodeCount;
	if (bCaught) { ++CaughtCount; } else { ++TimeoutCount; }

	BestEpisodeReward = FMath::Max(BestEpisodeReward, TotalReward);

	RecentEpisodeRewards.Add(TotalReward);
	while (RecentEpisodeRewards.Num() > PursuitAIEnvConstants::RecentEpisodeWindow)
	{
		RecentEpisodeRewards.RemoveAt(0);
	}
}

float APursuitAIEnv::MeanRecentReward() const
{
	if (RecentEpisodeRewards.Num() == 0)
	{
		return 0.0f;
	}

	float Sum = 0.0f;
	for (const float Value : RecentEpisodeRewards)
	{
		Sum += Value;
	}
	return Sum / static_cast<float>(RecentEpisodeRewards.Num());
}

void APursuitAIEnv::UpdateStats()
{
	const double Now = FPlatformTime::Seconds();

	if (!bStatsWindowOpen)
	{
		StatsWindowStart = Now;
		StatsWindowSteps = TotalStepCount;
		StatsWindowEpisodes = EpisodeCount;
		bStatsWindowOpen = true;
		return;
	}

	const double Elapsed = Now - StatsWindowStart;
	// One second, not half of one. The rate has to survive a lumpy tick rate: a windowed run
	// that the OS is throttling ticks in bursts, and a window shorter than the gap between two
	// bursts samples a legitimate zero - the panel then reports "0 steps/s" on a run that is
	// plainly progressing, which is worse than a number that lags slightly.
	if (Elapsed < 1.0)
	{
		return;
	}

	MeasuredStepsPerSecond = static_cast<float>(static_cast<double>(TotalStepCount - StatsWindowSteps) / Elapsed);
	MeasuredEpisodesPerSecond = static_cast<float>(static_cast<double>(EpisodeCount - StatsWindowEpisodes) / Elapsed);

	StatsWindowStart = Now;
	StatsWindowSteps = TotalStepCount;
	StatsWindowEpisodes = EpisodeCount;
}

void APursuitAIEnv::FinishWatchedEpisode(bool bCaught)
{
	FinishEpisodeStats(bCaught);

	if (bLogEpisodes)
	{
		UE_LOG(LogPursuitAI, Log,
			TEXT("PursuitAIEnv: watched episode %d %s in %d steps, travelled %.0f cm, reward %.3f"),
			EpisodeCount, bCaught ? TEXT("CAUGHT") : TEXT("TIMEOUT"), CurrentStep, DistanceTravelled,
			TotalReward);
	}

	if (bVisualsEnabled && DemoEpisodePause > 0.0f)
	{
		// Hold the last frame so the catch is actually visible; StepWatched resets after it.
		//
		// The word goes up with the hold and comes down with it, so the text and the pose it
		// is describing are on screen for exactly the same stretch of time. That is the whole
		// reason this is set here and not in DrawHud: "CAUGHT" over the frame in which the
		// two balls touch is a claim about that frame, and it has to be impossible for the
		// two to drift apart.
		EpisodeMessage = bCaught ? TEXT("CAUGHT") : TEXT("TIMEOUT");
		MessageRemaining = DemoEpisodePause;

		DemoPauseRemaining = DemoEpisodePause;
		bAwaitingReset = true;
		return;
	}

	BeginEpisode();
}

// ---------------------------------------------------------------------------
// Inference. Only reached in EPursuitDriveMode::Inference.
// ---------------------------------------------------------------------------

UNNEModelData* APursuitAIEnv::ResolveInferenceModel()
{
	// Two sources so the same code works whether the model went through the content browser
	// or was simply left on disk next to the training output.
	if (InferenceModelPath.StartsWith(TEXT("/Game/")) || InferenceModelPath.StartsWith(TEXT("/Script/")))
	{
		return LoadObject<UNNEModelData>(nullptr, *InferenceModelPath);
	}

	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *InferenceModelPath))
	{
		UE_LOG(LogPursuitAI, Error,
			TEXT("PursuitAIEnv: could not read an ONNX file at '%s'"), *InferenceModelPath);
		return nullptr;
	}

	// UNNEModelData::Init takes the *file extension* as the type tag; the ONNX runtime uses
	// it to pick a producer. Doing this at runtime is what keeps the whole train -> export
	// -> run path headless: no editor, no drag and drop, no content asset to check in.
	UNNEModelData* Model = NewObject<UNNEModelData>(this, TEXT("PursuitModelData"));
	Model->Init(TEXT("onnx"),
		TConstArrayView64<uint8>(Bytes.GetData(), static_cast<int64>(Bytes.Num())));

	UE_LOG(LogPursuitAI, Log,
		TEXT("PursuitAIEnv: loaded %d bytes of ONNX from disk: %s"), Bytes.Num(), *InferenceModelPath);

	return Model;
}

void APursuitAIEnv::SetupInference()
{
	UNNEModelData* Model = ResolveInferenceModel();
	if (!Model)
	{
		UE_LOG(LogPursuitAI, Error,
			TEXT("PursuitAIEnv: no ONNX model at '%s'. Train with --export-onnx, then point ")
			TEXT("-PursuitModel=<path> at the resulting .onnx. Falling back to the scripted policy."),
			*InferenceModelPath);
		DriveMode = EPursuitDriveMode::Demo;
		return;
	}

	if (!Policy)
	{
		Policy = NewObject<UNNEPolicy>(this, TEXT("PursuitNNEPolicy"));
	}
	if (!Stepper)
	{
		Stepper = NewObject<USimpleStepper>(this, TEXT("PursuitStepper"));
	}

	Policy->ModelData = Model;
	Policy->RuntimeName = InferenceRuntimeName;

	// The very same DefineSpaces the trainer was told about, so the observation the model
	// sees at inference is the observation it was trained on.
	FInteractionDefinition Definition;
	DefineSpaces(Definition);

	if (!Policy->Init(Definition))
	{
		UE_LOG(LogPursuitAI, Error,
			TEXT("PursuitAIEnv: UNNEPolicy::Init failed for '%s' on runtime '%s'. ")
			TEXT("Check that the NNERuntimeORT plugin is enabled for this project. ")
			TEXT("Falling back to the scripted policy."),
			*InferenceModelPath, *InferenceRuntimeName);
		DriveMode = EPursuitDriveMode::Demo;
		return;
	}

	TArray<TScriptInterface<IAgent>> Agents;
	TScriptInterface<IAgent> Self;
	Self.SetObject(this);
	Self.SetInterface(static_cast<IAgent*>(this));
	Agents.Add(Self);

	TScriptInterface<IPolicy> PolicyInterface;
	PolicyInterface.SetObject(Policy);
	PolicyInterface.SetInterface(static_cast<IPolicy*>(Policy));

	if (!Stepper->Init(Agents, PolicyInterface))
	{
		UE_LOG(LogPursuitAI, Error,
			TEXT("PursuitAIEnv: USimpleStepper::Init failed. Falling back to the scripted policy."));
		DriveMode = EPursuitDriveMode::Demo;
		return;
	}

	UE_LOG(LogPursuitAI, Log,
		TEXT("PursuitAIEnv: inference ready. model=%s runtime=%s obs=Box(%d) action=Box(%d)"),
		*InferenceModelPath, *InferenceRuntimeName,
		PursuitAIEnvConstants::NumObservations, PursuitAIEnvConstants::NumActionDimensions);
}

void APursuitAIEnv::StepInference()
{
	if (Stepper)
	{
		// The official loop: USimpleStepper collects the observation through IAgent::Observe,
		// runs UNNEPolicy, and hands the action back through IAgent::Act. Nothing here
		// bypasses the interface the scripted driver also goes through.
		Stepper->Step();
	}

	UpdateMarkers();
}

// ---------------------------------------------------------------------------
// Watching
// ---------------------------------------------------------------------------

void APursuitAIEnv::SetupDemoScene()
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	// Floor. The engine plane is 100x100 cm, so scale it out until it covers the arena.
	// It is the only thing giving the box a visible bottom, which is most of what makes
	// the 3D read as 3D.
	//
	// It sits at the *bottom* of the arena, not at Z=0. The arena spans Z in
	// [-ArenaHalfHeight, +ArenaHalfHeight], so a floor at 0 would be a slab slicing
	// through the middle of it and would hide every ball that drops below the centre -
	// which, in a 3D arena, is half of them. Placing it at -ArenaHalfHeight turns it into
	// the arena's floor: nothing can be occluded by it, and every shadow lands on it.
	//
	// In the city stage the floor is skipped outright: the road IS the floor, and a
	// 1000 cm plane of env albedo floating in a street would z-fight the asphalt and read
	// as a rendering bug on film. The lights below are skipped for the same "the stage
	// already has one" reason.
	const FVector ArenaBottom = ArenaCenter - FVector(0.0f, 0.0f, ArenaHalfHeight);
	if (bCityStage)
	{
		if (ArenaFloor)
		{
			ArenaFloor->SetVisibility(false);
		}
	}
	else if (ArenaFloor)
	{
		const float FloorScale = (ArenaHalfSize * 2.0f) / 100.0f;
		ArenaFloor->SetWorldScale3D(FVector(FloorScale, FloorScale, 1.0f));
		ArenaFloor->SetWorldLocation(ArenaBottom);
		ArenaFloor->SetVisibility(true);
		// Albedo, not brightness. The exposure is automatic and histogram-driven, so what
		// actually decides whether the balls survive is the *ratio* between their albedo and
		// the floor's: the floor fills most of the frame, so the exposure normalises it to
		// middle grey and everything brighter than it by that ratio clips. A near-black floor
		// (0.06) against a 0.9 ball is a 15:1 ratio, and the measured result was a ball that
		// rendered as (255,213,214) on top and (4,2,2) underneath - pure white and pure black,
		// with no red left in it. Raising the floor and pulling the balls down to roughly
		// 3:1 keeps both inside the range the tonemapper can show.
		ApplyMarkerColor(ArenaFloor, FLinearColor(0.22f, 0.24f, 0.28f));
	}

	// Markers. A sphere is 100 cm across, so a scale of s gives a radius of 50*s cm, and
	// UpdateMarkers lifts each one by its own radius so it rests on the floor instead of
	// being bisected by it. The lift is presentation only: the simulation's Z stays 0.
	if (AgentMarker)
	{
		AgentMarker->SetWorldScale3D(FVector(PursuitAIEnvConstants::AgentVisualRadius / 50.0f));
		AgentMarker->SetVisibility(true);
		ApplyMarkerColor(AgentMarker, FLinearColor(0.14f, 0.62f, 0.22f));
	}

	if (TargetMarker)
	{
		// Deliberately sized to the capture radius: the agent is caught once its centre is
		// inside this sphere, so the green ball disappearing into the red one *is* the
		// success condition rather than an approximation of it.
		TargetMarker->SetWorldScale3D(FVector(FMath::Max(CatchRadius, 1.0f) / 50.0f));
		TargetMarker->SetVisibility(true);
		ApplyMarkerColor(TargetMarker, FLinearColor(0.70f, 0.09f, 0.09f));
	}

	// Camera. Two placements, one per stage, and the difference is structural.
	//
	// Rig (default): perspective, angled. An orthographic top-down view is exactly what
	// made the flat version readable, and exactly what would flatten Z back into a point
	// now. Yaw 45 puts the far corner of the arena in the upper middle and the pitch is
	// chosen so the view axis points at the origin, which keeps the whole floor - and
	// therefore every shadow that lands on it - inside the frame.
	//
	// City: vertical. Any oblique angle over a street canyon gets occluded by the buildings
	// it passes - measured, not feared: the playable god camera went through five fallback
	// schemes before settling on near-vertical for exactly this reason. Straight down above
	// the arena centre has no building between the lens and the road, at any altitude, in
	// any street. The height and FOV mirror the god camera's measured values (2900 cm, 34
	// degrees), so a pair pane and a single-window god clip frame the subject at the same
	// scale. Not -90: a camera exactly vertical has no yaw and the rotation degenerates.
	if (DemoCamera)
	{
		if (bCityStage)
		{
			DemoCamera->ProjectionMode = ECameraProjectionMode::Perspective;
			DemoCamera->SetFieldOfView(34.0f);
			DemoCamera->SetWorldLocation(
				ArenaBottom + FVector(0.0f, 0.0f, CityCameraHeight));
			DemoCamera->SetWorldRotation(FRotator(-89.5f, 0.0f, 0.0f));//zxh todo
		}
		else
		{
			const float CamDistance = ArenaHalfSize * 2.4f;
			DemoCamera->ProjectionMode = ECameraProjectionMode::Perspective;
			DemoCamera->SetFieldOfView(55.0f);
			DemoCamera->SetRelativeLocation(FVector(-CamDistance, -CamDistance, ArenaHalfSize * 2.2f));
			DemoCamera->SetRelativeRotation(FRotator(-33.0f, 45.0f, 0.0f));
		}
	}

	// Deliberately dim, and the reason is not taste.
	//
	// A generated level contains no post-process volume, and with none the exposure never
	// adapts: the earlier comment here claimed the histogram would re-normalise whatever it
	// saw, which is wrong. Measured by raising the floor's albedo from 0.06 to 0.22 and
	// watching the floor get brighter rather than stay put. Screen brightness is therefore
	// proportional to albedo times light intensity, and the gain is set by these intensities
	// alone - so unless they are pulled down, any albedo worth looking at clips to white.
	//
	// These two values put the gain at roughly one: an albedo of 1.0 lands at white, a 0.22
	// floor lands at mid grey, and a 0.7 ball keeps a saturated colour instead of washing
	// out. Two directional lights - a key and a low fill - keep both spheres and their
	// shadows readable without dragging in sky atmosphere assets (ASkyAtmosphere sits
	// outside the Engine module and is not worth the dependency here).
	//
	// City stage skips them: the map brings its own sun and skylight, and two extra
	// directional lights on top of those would wash the street out and put a second shadow
	// under every ball - the exact "two shadows look like two objects" problem below.
	FActorSpawnParameters SpawnParams;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	// Only the key casts a shadow. Two shadow-casting directional lights put *two*
	// shadows under every ball, and on a plain floor those extra blobs are genuinely
	// indistinguishable from extra objects - which is a bug you notice by staring at a
	// screenshot, not in the code. A fill light has no business casting a shadow anyway.
	//
	// ForwardShadingPriority is what the "Multiple directional lights are competing to be
	// the single one used for forward shading" warning asks for: giving the key a higher
	// priority than the fill names the winner instead of letting the engine guess.
	const auto SpawnSun = [World, &SpawnParams](
		const FRotator& Rotation, float Intensity, bool bCastShadows, int32 Priority)
	{
		ADirectionalLight* Sun = World->SpawnActor<ADirectionalLight>(
			ADirectionalLight::StaticClass(), FVector(0.0f, 0.0f, 1000.0f), Rotation, SpawnParams);
		if (Sun)
		{
			// DirectionalLightComponent, not LightComponent: ForwardShadingPriority only
			// exists on the directional flavour.
			if (UDirectionalLightComponent* Light = Cast<UDirectionalLightComponent>(Sun->GetLightComponent()))
			{
				Light->SetMobility(EComponentMobility::Movable);
				Light->SetIntensity(Intensity);
				Light->SetCastShadows(bCastShadows);
				Light->SetForwardShadingPriority(Priority);
			}
		}
	};

	if (!bCityStage)
	{
		// Kept deliberately dim: see the note above on what the gain does to the balls.
		// These two were tuned by measurement, not by eye - the first attempt put the gain
		// at 1.5 (floor at 157, the red ball's red channel clipped to 255), so they came
		// down together by that factor. Halving both keeps the fill at a quarter of the
		// key, which is what makes a shadow read as a shadow rather than as a dark side.
		SpawnSun(FRotator(-55.0f, 25.0f, 0.0f), 0.4f, true, 1);    // key: the only shadow caster
		SpawnSun(FRotator(35.0f, -140.0f, 0.0f), 0.1f, false, 0);  // fill: light, no shadow
	}
}

void APursuitAIEnv::ApplyMarkerColor(UStaticMeshComponent* Component, const FLinearColor& Color)
{
	if (!Component)
	{
		return;
	}

	UMaterialInterface* BaseMaterial = LoadObject<UMaterialInterface>(
		nullptr, TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
	if (!BaseMaterial)
	{
		return;
	}

	UMaterialInstanceDynamic* Dynamic = UMaterialInstanceDynamic::Create(BaseMaterial, this);
	if (!Dynamic)
	{
		UE_LOG(LogTemp, Error, TEXT("PursuitAI: could not instance a dynamic material for %s"),
			*Component->GetName());
		return;
	}

	// BasicShapeMaterial exposes its albedo through a vector parameter named "Color".
	Dynamic->SetVectorParameterValue(TEXT("Color"), Color);

	// Read the parameter straight back. Without this a miss is silent, and the symptom -
	// a correctly sized ball in the wrong colour - looks exactly like a lighting problem.
	FLinearColor ReadBack = FLinearColor::Black;
	const bool bRead = Dynamic->GetVectorParameterValue(FMaterialParameterInfo(TEXT("Color")), ReadBack);

	Component->SetMaterial(0, Dynamic);

	// Logged, not printed on screen: a watched run is what gets recorded, and an overlay
	// explaining the material is exactly what a demo video does not want.
	UE_LOG(LogPursuitAI, Display, TEXT("PursuitAI: %s colour set=%s read-back=%s"),
		*Component->GetName(), *Color.ToString(), bRead ? *ReadBack.ToString() : TEXT("MISS"));
}

void APursuitAIEnv::UpdateMarkers()
{
	if (!bVisualsEnabled)
	{
		return;
	}

	// Position comes straight from the simulation; only the lift is added, so that a ball
	// whose centre is at Z=0 sits on the floor. Z is real state now - if the agent climbs,
	// the ball climbs with it.
	if (AgentMarker)
	{
		AgentMarker->SetWorldLocation(AgentPos + FVector(0.0f, 0.0f, PursuitAIEnvConstants::AgentVisualRadius));
	}

	if (TargetMarker)
	{
		TargetMarker->SetWorldLocation(TargetPos + FVector(0.0f, 0.0f, CatchRadius));
	}
}

void APursuitAIEnv::EnsureViewTarget()
{
	// Claim the player camera once, as soon as a controller exists. During BeginPlay it may
	// not have been spawned yet, and in -game mode it arrives a frame or two later.
	if (!bViewTargetSet)
	{
		if (APlayerController* Controller = UGameplayStatics::GetPlayerController(this, 0))
		{
			Controller->SetViewTarget(this);
			bViewTargetSet = true;
		}
	}

	// Hide the engine's pawn on the same first frame, and for the same reason.
	//
	// A -game launch spawns a DefaultPawn, and ADefaultPawn carries a visible sphere mesh
	// (DefaultPawn.cpp: MeshComponent / SphereMesh). The generated level has no
	// PlayerStart, so that pawn sits at the world origin - which, with the demo camera
	// looking straight at the origin, puts a stray grey ball dead centre of every shot.
	// It is the engine's pawn, not anything this scene placed, so it is hidden rather than
	// worked around. This is the ball that made the watch window look like it had a third
	// actor in it.
	if (!bPlayerPawnHidden)
	{
		if (APlayerController* Controller = UGameplayStatics::GetPlayerController(this, 0))
		{
			if (APawn* PlayerPawn = Controller->GetPawn())
			{
				PlayerPawn->SetActorHiddenInGame(true);
				// Hiding it is not enough. Its collision sphere is still there and still
				// traceable, and an invisible collidable ball sitting on the world origin is
				// exactly the kind of thing that turns the debug line-of-sight ray red for no
				// visible reason - the agent runs straight past it on the way to the target.
				// Nothing in this scene wants the pawn to collide with anything.
				PlayerPawn->SetActorEnableCollision(false);
				bPlayerPawnHidden = true;
				UE_LOG(LogPursuitAI, Display,
					TEXT("PursuitAI: hid the engine's default pawn (%s) and dropped its collision"),
					*PlayerPawn->GetName());
			}
		}
	}
}

void APursuitAIEnv::UpdateVisuals()
{
	if (!bVisualsEnabled)
	{
		return;
	}

	UpdateStats();
	EnsureViewTarget();
	// After EnsureViewTarget, which is what drops the hidden pawn's collision. Tracing first
	// would let that invisible sphere block the ray on the very first frame.
	RefreshLineOfSight();

	if (!bOccludersLogged)
	{
		bOccludersLogged = true;
		LogOccluderCount();
	}

	UpdateMarkers();

	// Drawn once per rendered frame, not once per simulation step. At a few hundred steps a
	// second a per-step draw would queue hundreds of overlapping arrows into a single frame,
	// which is not a debug view - it is a smear. The panel and the markers therefore always
	// show wherever the simulation happens to be at the moment the frame is drawn.
	DrawDebug();
	DrawHud();
}

void APursuitAIEnv::StepWatched(float DeltaSeconds)
{
	UpdateVisuals();

	if (MessageRemaining > 0.0f)
	{
		MessageRemaining -= DeltaSeconds;
		if (MessageRemaining <= 0.0f)
		{
			EpisodeMessage.Reset();
		}
	}

	// The start gate. Held *after* UpdateVisuals, so the pane still draws its scene, its
	// label and its WAITING FOR GO text while it is frozen - a pane that drew nothing until
	// the deadline would be indistinguishable from one that failed to load, which is the
	// one failure this gate must not be confused with.
	if (IsWaitingForGo())
	{
		// Not accumulated: time served before the deadline is not owed to the simulation, or
		// the slower pane would arrive at the deadline with a backlog and sprint to catch up.
		DemoStepAccumulator = 0.0f;
		return;
	}

	// Holding the final frame of a finished episode, so the catch is actually visible.
	if (bAwaitingReset)
	{
		DemoPauseRemaining -= DeltaSeconds;
		if (DemoPauseRemaining <= 0.0f)
		{
			bAwaitingReset = false;
			BeginEpisode();
		}
		return;
	}

	DemoStepAccumulator += DeltaSeconds;
	const float StepInterval = 1.0f / FMath::Max(DemoStepsPerSecond, 0.1f);

	// Cap the catch-up so a hitch can never spin an entire episode inside a single frame.
	int32 Budget = 32;
	while (DemoStepAccumulator >= StepInterval && Budget-- > 0)
	{
		DemoStepAccumulator -= StepInterval;

		// Throttling is not cosmetic. Without it an in-engine policy would run once per
		// rendered frame, finishing an episode in a handful of frames - unwatchable, and
		// the reason StepWatched exists at all rather than the stepper being ticked raw.
		if (DriveMode == EPursuitDriveMode::Inference)
		{
			StepInference();
		}
		else if (DriveMode == EPursuitDriveMode::Random)
		{
			AdvanceRandomEpisode();
		}
		else
		{
			AdvanceDemoEpisode();
		}

		if (bAwaitingReset)
		{
			break;
		}
	}
}

void APursuitAIEnv::AdvanceDemoEpisode()
{
	// The scripted driver deliberately goes through the same IAgent contract the ONNX
	// stepper uses, so watching the demo exercises Observe/Act. If the contract breaks,
	// the demo breaks first - long before a trained model is involved.
	FInstancedStruct Action;
	WriteGreedyAction(Action);
	IAgent::Execute_Act(this, Action);

	UpdateMarkers();
}

void APursuitAIEnv::WriteGreedyAction(FInstancedStruct& OutAction) const
{
	OutAction.InitializeAs<FBoxPoint>();
	FBoxPoint& Box = OutAction.GetMutable<FBoxPoint>();

	// Straight at the target, at full deflection. On this arena that is also the optimum of
	// the distance shaping the reward is built from, so the demo shows what PPO converges to
	// rather than an unrelated hand-written behaviour.
	const FVector Direction = (TargetPos - AgentPos).GetSafeNormal();

	Box.Values = {
		static_cast<float>(Direction.X),
		static_cast<float>(Direction.Y),
		static_cast<float>(Direction.Z),
	};
	Box.Shape = { PursuitAIEnvConstants::NumActionDimensions };
}

void APursuitAIEnv::AdvanceRandomEpisode()
{
	// Same IAgent contract as the scripted driver and as the ONNX stepper. The baseline has
	// to be a *policy* going through the same door, or what the clip compares is the code
	// around the policy rather than the policy.
	FInstancedStruct Action;
	WriteRandomAction(Action);
	IAgent::Execute_Act(this, Action);

	UpdateMarkers();
}

void APursuitAIEnv::WriteRandomAction(FInstancedStruct& OutAction)
{
	OutAction.InitializeAs<FBoxPoint>();
	FBoxPoint& Box = OutAction.GetMutable<FBoxPoint>();

	// Uniform on the sphere, not in the cube. Drawing three independent components and
	// normalising would over-sample the corners of the action box by a factor of about
	// 1.9 in the diagonal directions - and since magnitude is clamped to 1 before scaling,
	// a corner draw and a face draw end up the same length anyway, so cube sampling would
	// silently bias the *directions* toward the diagonals. This is the standard method:
	// z uniform in [-1,1], the azimuth uniform in [0, 2pi].
	//
	// Full stick on purpose. A baseline that sometimes moves half a step would confound
	// "does not know where the target is" with "is not trying", and the clip's whole claim
	// is about the first of those.
	const float Z = ActionRng.FRandRange(-1.0f, 1.0f);
	const float Azimuth = ActionRng.FRandRange(0.0f, 2.0f * UE_PI);
	const float Radial = FMath::Sqrt(FMath::Max(1.0f - Z * Z, 0.0f));

	Box.Values = {
		Radial * FMath::Cos(Azimuth),
		Radial * FMath::Sin(Azimuth),
		Z,
	};
	Box.Shape = { PursuitAIEnvConstants::NumActionDimensions };
}

bool APursuitAIEnv::IsWaitingForGo() const
{
	if (StartAtEpoch <= 0.0)
	{
		return false;
	}
	return static_cast<double>(FDateTime::UtcNow().ToUnixTimestamp()) < StartAtEpoch;
}

void APursuitAIEnv::DrawDebug() const
{
#if ENABLE_DRAW_DEBUG
	UWorld* World = GetWorld();
	if (!World || !bDrawDebug)
	{
		return;
	}

	// Everything below passes LifeTime = -1, which across the DrawDebug family means "this
	// frame only". Nothing survives between frames, so there is no state to clear when the
	// debug view is switched off mid-run.
	constexpr float ThisFrame = -1.0f;
	constexpr uint8 Depth = 0;

	// --- Arena bounds. This is what the agent is clamped against, and the clamp is what ends
	// every failed episode, so it has to be visible rather than inferred from the floor. ---
	DrawDebugBox(World, ArenaCenter, GetArenaHalfExtent(), FColor(70, 70, 85),
		false, ThisFrame, Depth, 1.5f);

	// --- Line of sight: a thin ray from the agent all the way to the target, green while the
	// target is reachable and red - cut short - at whatever blocks it. With nothing in the
	// arena there is nothing to block it, so this reads green on every step; the occluder
	// count logged once in SetupDemoScene is what keeps that from being a surprise. ---
	const bool bClear = bTargetVisible;
	const FVector SightEnd = SightEndPoint;
	DrawDebugLine(World, AgentPos, SightEnd,
		bClear ? FColor(0, 190, 90) : FColor(255, 40, 40), false, ThisFrame, Depth, 2.0f);
	if (!bClear)
	{
		DrawDebugSphere(World, SightEnd, 14.0f, 8, FColor(255, 40, 40), false, ThisFrame, Depth, 2.0f);
	}

	// --- Where the target is, as a short yellow arrow at the agent, and what the policy did,
	// as a green one from the same origin. Both are drawn at the same scale, and that scale is
	// four steps - so "green reaches exactly as far as yellow, in the same direction" is what
	// a full-stick decision aimed straight at the target looks like, and any gap between the
	// two arrowheads is the policy's error, readable without comparing numbers. ---
	const float ActionArrowScale = 4.0f;
	const float ArrowUnit = MoveStep * ActionArrowScale;
	const float ActualSize = static_cast<float>(LastActualOffset.Size());
	const FVector ToTarget = (TargetPos - AgentPos).GetSafeNormal();
	if (!ToTarget.IsNearlyZero())
	{
		DrawDebugDirectionalArrow(World, AgentPos, AgentPos + ToTarget * ArrowUnit,
			ArrowUnit * 0.18f, FColor(255, 210, 0), false, ThisFrame, Depth, 8.0f);
	}

	// Red is the part the arena bounds threw away, drawn continuing from where the green arrow
	// stops. An all-green arrow therefore means "taken as asked", and any red at all means the
	// agent is pushing into a wall - which is otherwise invisible, because a clipped step just
	// looks like a smaller one.
	const FVector AppliedEnd = AgentPos + LastActualOffset * ActionArrowScale;
	if (!LastActualOffset.IsNearlyZero())
	{
		// Blue, and the colour is a decision rather than a preference: green was already
		// spoken for twice in this scene - by the line of sight and by the trail - and the
		// one arrow a comparison clip is actually about has to be the one colour nothing
		// else is. A viewer tracing "did the blue arrow point at the yellow one" never has
		// to wonder which green they are looking at.
		DrawDebugDirectionalArrow(World, AgentPos, AppliedEnd,
			FMath::Max(ActualSize * ActionArrowScale * 0.25f, 12.0f), FColor(60, 120, 255),
			false, ThisFrame, Depth, 8.0f);
	}
	if (bActionClamped)
	{
		// Orange, not red: red now belongs to the capture volume, and this arrow means
		// something else entirely - it is the part of the step the arena wall threw away.
		DrawDebugDirectionalArrow(World, AppliedEnd, AgentPos + LastIntendedOffset * ActionArrowScale,
			18.0f, FColor(255, 140, 0), false, ThisFrame, Depth, 8.0f);
	}

	// --- Where the agent has actually been. The two arrows show one decision each; this
	// shows what the decisions added up to, which is the part that separates a policy that
	// aims well from one that arrived by accident. One segment per step, so its length is
	// literally the number of steps taken and a trail that wanders is a trail that wasted
	// steps. ---
	for (int32 Index = 1; Index < Trail.Num(); ++Index)
	{
		DrawDebugLine(World, Trail[Index - 1], Trail[Index], FColor(80, 255, 140),
			false, ThisFrame, Depth, 3.0f);
	}

	// --- Capture range, as both a flat ring and a wireframe sphere. The ring is the part that
	// reads from the demo camera's angle; the sphere is what makes it a volume, so a capture
	// that happened above or below the target's plane does not look impossible. ---
	// Red, because it is the thing that ends the episode: the agent is caught the moment it
	// gets inside this, so red here means "success happens in here" and the CAUGHT text is
	// in the same colour. It used to be orange, which read as "a suggestion".
	DrawDebugCircle(World, TargetPos, CatchRadius, 40, FColor(230, 40, 40), false, ThisFrame, Depth, 3.0f,
		FVector(1.0f, 0.0f, 0.0f), FVector(0.0f, 1.0f, 0.0f), false);
	DrawDebugSphere(World, TargetPos, CatchRadius, 20, FColor(230, 40, 40), false, ThisFrame, Depth, 1.0f);

	// --- Who is who, and what they have earned. ASCII only: the engine's debug font carries
	// no CJK glyphs, and a label that renders as a row of boxes is worse than no label. ---
	DrawDebugString(World,
		AgentPos + FVector(0.0f, 0.0f, PursuitAIEnvConstants::AgentVisualRadius + 110.0f),
		FString::Printf(TEXT("CHASER #%d    R %+.2f"), AgentId, TotalReward),
		nullptr, FColor(120, 255, 140), 0.0f, true, 1.3f);

	DrawDebugString(World, TargetPos + FVector(0.0f, 0.0f, CatchRadius + 50.0f),
		FString::Printf(TEXT("TARGET    %.0f cm"), DistanceToTarget()),
		nullptr, FColor(255, 130, 130), 0.0f, true, 1.3f);
#endif
}

bool APursuitAIEnv::TraceLineOfSight(FVector& OutEnd) const
{
	OutEnd = TargetPos;

	const UWorld* World = GetWorld();
	if (!World)
	{
		return true;
	}

	// ECC_Visibility, not a channel of our own: that is the channel a mesh blocks by default,
	// so anything dropped into the level to act as cover starts occluding on its own and this
	// ray turns red without a line of code changing. The two markers are ignored so the ray
	// reports on the space between the agent and the target rather than stopping at the balls
	// that represent them.
	FCollisionQueryParams Params(SCENE_QUERY_STAT(PursuitLineOfSight), false, this);
	if (AgentMarker) { Params.AddIgnoredComponent(AgentMarker.Get()); }
	if (TargetMarker) { Params.AddIgnoredComponent(TargetMarker.Get()); }

	FHitResult Hit;
	const bool bBlocked = World->LineTraceSingleByChannel(Hit, AgentPos, TargetPos, ECC_Visibility, Params);
	if (bBlocked)
	{
		OutEnd = Hit.ImpactPoint;
	}
	return !bBlocked;
}

void APursuitAIEnv::RefreshLineOfSight()
{
	// One trace per frame, two consumers. The panel and the scene ray must agree - running
	// the trace separately for each would let them report different things on a frame where
	// the simulation moved in between, which is a bug that only shows up as "the line says
	// blocked but the text says visible" and is very hard to enjoy debugging.
	SightEndPoint = TargetPos;
	bTargetVisible = TraceLineOfSight(SightEndPoint);
}

void APursuitAIEnv::LogOccluderCount()
{
	// One-time note on what the line-of-sight ray can possibly report. It is a real trace, so
	// on an empty arena the honest reading is "clear, every step" - and a debug view whose red
	// case never fires looks broken unless that is said out loud.
	//
	// Counted from UpdateVisuals rather than from SetupDemoScene, and skipping hidden actors,
	// because the only collidable thing in this level is the engine's default pawn - which is
	// hidden and de-collided one step later, in EnsureViewTarget. Counting earlier reported it
	// as a real occluder, which is the one thing this line exists not to do.
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	int32 Occluders = 0;
	FString Names;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		if (*It == this || It->IsHidden())
		{
			continue;
		}

		TArray<UPrimitiveComponent*> Primitives;
		It->GetComponents(Primitives);
		for (const UPrimitiveComponent* Primitive : Primitives)
		{
			if (Primitive && Primitive->IsCollisionEnabled())
			{
				// Named, not just counted. "1 collidable actor" is not a fact anyone can act
				// on - the useful version is "and it is the chaos debug drawer, which is
				// engine bookkeeping and will never be between the agent and the target".
				++Occluders;
				Names += (Names.IsEmpty() ? TEXT("") : TEXT(", "));
				Names += It->GetName();
				break;
			}
		}
	}

	UE_LOG(LogPursuitAI, Display,
		TEXT("PursuitAI: %d visible collidable actor(s) besides this one: %s. The debug ")
		TEXT("line-of-sight ray reads CLEAR on every step until something is actually in the way."),
		Occluders, Occluders > 0 ? *Names : TEXT("<none>"));
}

FString APursuitAIEnv::DescribePolicy() const
{
	switch (DriveMode)
	{
	case EPursuitDriveMode::Inference:
		return FString::Printf(TEXT("PPO Inference - ONNX on %s, no Python"), *InferenceRuntimeName);

	case EPursuitDriveMode::Demo:
		return TEXT("Scripted greedy - no model at all");

	case EPursuitDriveMode::Random:
		// Named as a baseline and not as a policy, because that is what it is. Calling this
		// "untrained" would be a claim about a network's behaviour, and no network is
		// involved - and the one claim a comparison must not make is a false one about
		// what its own control group was doing.
		return TEXT("Random baseline - uniform full-stick direction every step");

	default:
		// Spelled out because it is the one thing about training that cannot be shown: the
		// weights being updated live are in the trainer's process, not in this one. Everything
		// on this panel is the policy's inputs, its outputs and the scores it earned - never
		// its internals, and never a claim to be showing them.
		return TEXT("PPO Training - SB3 over gRPC, weights live in Python");
	}
}

void APursuitAIEnv::DrawHud()
{
	if (!bDrawDebug || !GEngine)
	{
		return;
	}

	const float Duration = 0.5f;
	const FVector2D TextScale(1.2f, 1.2f);

	// bNewerOnTop = false so the lines appear in the order they are added. With the default
	// (true) the first line added ends up below the last and the panel reads bottom-up, which
	// looks deliberate and is pure confusion.
	const auto Line = [this, Duration, &TextScale](uint64 Key, const FColor& Colour, const FString& Text)
	{
		GEngine->AddOnScreenDebugMessage(Key, Duration, Colour, Text, false, TextScale);
	};

	const FColor TitleColour(255, 200, 60);
	const FColor PlainColour(235, 235, 235);
	const FColor DimColour(160, 160, 160);
	const FColor GoodColour(110, 255, 140);
	const FColor BadColour(255, 120, 120);

	const float Distance = DistanceToTarget();
	const bool bVisible = bTargetVisible;

	// The observation is the policy's own input, read through the same function the trainer
	// and the ONNX export both go through - so what this line prints is literally what the
	// network was handed this step, not a re-derivation that could drift from it.
	//
	// TInstancedStruct<FPoint> only hands back the base pointer, so reading the numbers means
	// a downcast. Checking the script struct first rather than casting blindly: FPoint is
	// polymorphic and a wrong cast would be a silent misread of memory, which is a bad way
	// for a debug view to fail.
	TInstancedStruct<FPoint> Observation;
	BuildObservation(Observation);

	const FBoxPoint* Box =
		(Observation.GetScriptStruct() == FBoxPoint::StaticStruct())
			? static_cast<const FBoxPoint*>(Observation.GetPtr())
			: nullptr;

	FString ObservationText = TEXT("<unavailable>");
	if (Box && Box->Values.Num() >= PursuitAIEnvConstants::NumObservations)
	{
		ObservationText = FString::Printf(
			TEXT("rel %+.2f %+.2f %+.2f   |   self %+.2f %+.2f %+.2f   |   relVel %+.2f %+.2f %+.2f"),
			Box->Values[0], Box->Values[1], Box->Values[2],
			Box->Values[3], Box->Values[4], Box->Values[5],
			Box->Values[6], Box->Values[7], Box->Values[8]);
	}

	// The angle between where the policy decided to go and where the target actually is. It is
	// the single most informative number here: one axis of it is the policy's aim, the other
	// is how much of the gap a step can close, and a policy that has learned to pursue sits
	// near zero while a random one scatters.
	FString OffAxisText = TEXT("--");
	const FVector AppliedDirection = LastActionApplied.GetSafeNormal();
	const FVector ToTargetDirection = (TargetPos - AgentPos).GetSafeNormal();
	if (!AppliedDirection.IsNearlyZero() && !ToTargetDirection.IsNearlyZero())
	{
		const float Cosine = FMath::Clamp(
			static_cast<float>(FVector::DotProduct(AppliedDirection, ToTargetDirection)), -1.0f, 1.0f);
		OffAxisText = FString::Printf(TEXT("%.0f deg"), FMath::RadiansToDegrees(FMath::Acos(Cosine)));
	}

	const float IntendedCm = static_cast<float>(LastIntendedOffset.Size());
	const float ActualCm = static_cast<float>(LastActualOffset.Size());

	const FString BestText = EpisodeCount > 0
		? FString::Printf(TEXT("%+.2f"), BestEpisodeReward)
		: TEXT("--");
	const FString MeanText = RecentEpisodeRewards.Num() > 0
		? FString::Printf(TEXT("%+.2f"), MeanRecentReward())
		: TEXT("--");

	if (!PaneLabel.IsEmpty())
	{
		// Burned into the picture rather than overlaid afterwards. Two game windows carry
		// the same title, so without this a side-by-side clip can only be labelled by
		// aligning text over it in post - and both panes were never guaranteed to be the
		// same size or in the same place. Bigger than the panel: this is the one line that
		// has to be readable in a thumbnail.
		GEngine->AddOnScreenDebugMessage(8100, 5.0f, FColor(255, 230, 120), PaneLabel,
			false, FVector2D(2.0f, 2.0f));
	}

	if (IsWaitingForGo())
	{
		// Drawn while the gate is closed and gone the moment it opens, so the clip's first
		// frozen second is self-explaining instead of looking like a hang.
		GEngine->AddOnScreenDebugMessage(8120, 1.0f, FColor(120, 220, 255),
			TEXT("WAITING FOR GO"), false, FVector2D(2.2f, 2.2f));
	}

	Line(8101, TitleColour, FString::Printf(TEXT("PursuitAI   |   %s"), *DescribePolicy()));

	// EpisodeCount counts *finished* episodes, so the one running is that plus one - except
	// while the last frame of a finished one is being held, when the next one has not begun
	// and numbering it early would label someone else's final state.
	const int32 DisplayedEpisode = EpisodeCount + (bAwaitingReset ? 0 : 1);

	Line(8102, PlainColour, FString::Printf(
		TEXT("Episode %d   Step %d / %d   %.0f steps/s   %.1f episodes/s%s"),
		DisplayedEpisode, CurrentStep, MaxSteps, MeasuredStepsPerSecond, MeasuredEpisodesPerSecond,
		bAwaitingReset ? TEXT("   [holding the last frame]") : TEXT("")));

	Line(8103, PlainColour, FString::Printf(
		TEXT("Reward   step %+.3f   total %+.3f   closing %+.0f cm"),
		CurrentReward, TotalReward, CurrentClosing));

	Line(8104, DimColour, FString::Printf(
		TEXT("Episodes   %d caught / %d done   best %s   mean(last %d) %s"),
		CaughtCount, EpisodeCount, *BestText,
		PursuitAIEnvConstants::RecentEpisodeWindow, *MeanText));

	Line(8105, bVisible ? GoodColour : BadColour, FString::Printf(
		TEXT("Distance %.0f cm   Target visible %s"),
		Distance, bVisible ? TEXT("YES") : TEXT("NO - line of sight blocked")));

	Line(8106, DimColour, FString::Printf(TEXT("Observation (the policy's input)   %s"), *ObservationText));

	Line(8107, bActionClamped ? BadColour : PlainColour, FString::Printf(
		TEXT("Action   x %+.2f  y %+.2f  z %+.2f   |a| %.2f   off-axis %s   step %.0f of %.0f cm%s"),
		LastActionApplied.X, LastActionApplied.Y, LastActionApplied.Z,
		LastActionApplied.Size(), *OffAxisText, ActualCm, IntendedCm,
		bActionClamped ? TEXT("   CLIPPED BY ARENA") : TEXT("")));

	// The legend has to name colours, not concepts: the clip's whole argument is read off
	// these four things by eye, and an arrow whose colour the viewer has to guess is an
	// arrow that proves nothing.
	Line(8108, DimColour,
		TEXT("legend   YELLOW = to target   BLUE = action taken   ORANGE = step lost to the wall   ")
		TEXT("GREEN = path travelled / line of sight   RED = capture range"));

	if (!EpisodeMessage.IsEmpty())
	{
		// Big, and in the capture colour when it is a capture. It sits below the panel
		// rather than in the middle of the frame because AddOnScreenDebugMessage has no
		// notion of centring, and a line of text that moves would be worse than one that
		// is merely low.
		const bool bCaught = EpisodeMessage == TEXT("CAUGHT");
		GEngine->AddOnScreenDebugMessage(8110, 0.2f,
			bCaught ? FColor(255, 90, 90) : FColor(210, 210, 210),
			EpisodeMessage, false, FVector2D(4.0f, 4.0f));
	}
}
