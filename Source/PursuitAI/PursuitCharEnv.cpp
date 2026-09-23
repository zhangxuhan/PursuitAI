// Copyright Epic Games, Inc. All Rights Reserved.

#include "PursuitCharEnv.h"

#include "PursuitAI.h"
#include "PursuitCharAgent.h"
#include "PursuitJumpActuator.h"
#include "PursuitTargetSensor.h"

#include "Common/InstancedStructUtils.h"
#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "Common/InteractionDefinition.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/CollisionProfile.h"
#include "Engine/DirectionalLight.h"
#include "Engine/Engine.h"
#include "Engine/SkyLight.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "Components/LightComponent.h"
#include "Components/SkyLightComponent.h"
#include "GymConnectors/gRPC/gRPCGymConnector.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "Misc/FileHelper.h"
#include "Misc/CommandLine.h"
#include "Misc/DateTime.h"
#include "Misc/DefaultValueHelper.h"
#include "Misc/OutputDeviceRedirector.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
// FGenericPlatformMisc::RequestExit for the validation exit; the Schola space types for
// the interface-dimension print in InitializeEnvironment.
#include "HAL/PlatformMisc.h"
#include "Spaces/BoxSpace.h"
#include "Spaces/DictSpace.h"
#include "NNEModelData.h"
#include "Points/Point.h"
#include "Points/BoxPoint.h"
#include "Points/DictPoint.h"
#include "TrainingDataTypes/AgentState.h"

namespace
{
	/**
	 * Same non-round offset trick as v1's ActionStreamSeedOffset: the watched-run driver
	 * stream must not share state with the spawn stream, and a round offset invites
	 * collisions with seeds a person might actually type.
	 */
	constexpr int32 DriveStreamSeedOffset = 1000003;

	/** Keep spawns this fraction inside the arena wall, so a capsule never starts touching it. */
	constexpr float SpawnMarginFraction = 0.9f;

	/** Capsule spawn Z is this above the env actor's Z; matches ACharacter's default capsule. */
	constexpr float CapsuleHalfHeight = 88.0f;

	/** Scripted-driver obstacle probe: how far ahead and how wide to sweep. */
	constexpr float ProbeDistance = 300.0f;
	constexpr float ProbeRadius = 45.0f;

	/**
	 * Fixed obstacle layout, in env-relative cm: {X, Y, ScaleX, ScaleY, ScaleZ, ClearRadius}.
	 * The Cube basic shape is a 100 cm cube, so Scale 1.5 = 150 cm. Deterministic BY
	 * DESIGN: both panes of an A/B and every training episode see the same arena, and
	 * spawn avoidance (RandomSpawnPoint) can reject draws against exactly these discs.
	 * Heights: anything <= 70 cm is jumpable by the chaser's default 420 JumpZVelocity
	 * (apex ~ 90 cm); the four 400 cm pillars must be run around.
	 */
	struct FObstacleSpec
	{
		float X, Y, SX, SY, SZ, ClearRadius;
	};

	const FObstacleSpec ObstacleSpecs[] =
	{
		// four tall pillars on the 45-degree diagonals, mid-radius - the "corners" of the arena
		{  550.0f,  550.0f, 1.5f, 1.5f, 4.0f, 220.0f },
		{ -550.0f,  550.0f, 1.5f, 1.5f, 4.0f, 220.0f },
		{ -550.0f, -550.0f, 1.5f, 1.5f, 4.0f, 220.0f },
		{  550.0f, -550.0f, 1.5f, 1.5f, 4.0f, 220.0f },
		// two low boxes on the cardinal axes - hop over them, or run around
		{    0.0f,  650.0f, 2.5f, 2.5f, 0.7f, 200.0f },
		{    0.0f, -650.0f, 2.5f, 2.5f, 0.7f, 200.0f },
		// two low cross-walls east and west - natural chokes to herd the chase through
		{  850.0f,    0.0f, 0.5f, 4.5f, 0.7f, 160.0f },
		{ -850.0f,    0.0f, 0.5f, 4.5f, 0.7f, 160.0f },
	};

	/** Dict key APursuitCharAgent composes the target sensor under. One constant, so the
	 * three read sites cannot drift. */
	const TCHAR* const TargetSensorKey = TEXT("TargetSensor");

	/**
	 * The ONE unpacker for "the chaser's target-sensor reading".
	 *
	 * Why it exists (2026-09-22, Stage 0 audit): the same unwrap was written out three
	 * times and the third copy was WRONG. The per-step wall-probe counter asked for an
	 * FBoxPoint at the top level, while what APursuitCharAgent actually hands over is an
	 * FDictPoint keyed by component name ("TargetSensor"). That cast failed on every step
	 * of every run, so WallProbeHitsThisEpisode stayed at 0 forever - and 0 is exactly
	 * what a correct implementation reports on a stage with no walls. A false negative
	 * that silently agrees with the expected answer is worse than a crash.
	 *
	 * Accepts both shapes on purpose: the Dict is what the agent composes (one key per
	 * sensor), the bare Box is what a single-sensor policy may hand over directly.
	 * Returns nullptr when neither matches, and callers MUST treat nullptr as "no
	 * reading" rather than as "all clear".
	 */
	const FBoxPoint* ResolveTargetSensorPoint(const FInstancedStruct& Observation)
	{
		if (const FDictPoint* Dict = Observation.GetPtr<FDictPoint>())
		{
			const TInstancedStruct<FPoint>* Entry = Dict->Points.Find(TargetSensorKey);
			return Entry ? Entry->GetPtr<FBoxPoint>() : nullptr;
		}
		return Observation.GetPtr<FBoxPoint>();
	}
}

APursuitCharEnv::APursuitCharEnv()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = true;

	// A scene root, because transforms have to MEAN something for this actor. Without
	// a root, SetActorLocation silently no-ops and a baked actor always saves at the
	// origin - the city stage's plaza placement (-250, 0, 70) never actually applied,
	// and every spawn/camera/leash calculation silently centred on (0,0,0) instead
	// (found 2026-09-21 when the baked city training level spawned its platforms at
	// ground z=-60). Arena centre, watch-camera aim and the plaza offsets above all
	// read GetActorLocation(); with a root they finally tell the truth.
	SetRootComponent(CreateDefaultSubobject<USceneComponent>(TEXT("SceneRoot")));

	// The connector owns the gRPC server and drives the Python trainer. Without this
	// subobject the base class's `if (Connector)` in BeginPlay silently skips - no
	// server, no connection, and the trainer dies in DEADLINE_EXCEEDED while the sim
	// sits there looking perfectly healthy. Measured: the first staged run did exactly
	// that. `-ScholaPort=<n>` on the command line overrides the port, which is how the
	// Schola simulators pass their chosen port in.
	Connector = CreateDefaultSubobject<URPCGymConnector>(TEXT("ScholaConnector"));
}

void APursuitCharEnv::BeginPlay()
{
	Super::BeginPlay();

	// CLI switches. Train stays the default so a bare headless launch behaves exactly as
	// the trainer expects; the watched modes have to be asked for by name, like v1.
	const TCHAR* Cmdline = FCommandLine::Get();
	if (FParse::Param(Cmdline, TEXT("PursuitCharRandom")))
	{
		DriveMode = EPursuitCharDriveMode::Random;
	}
	else if (FParse::Param(Cmdline, TEXT("PursuitCharGreedy")))
	{
		DriveMode = EPursuitCharDriveMode::Greedy;
	}
	else if (FParse::Param(Cmdline, TEXT("PursuitCharInference")))
	{
		DriveMode = EPursuitCharDriveMode::Inference;
	}

	FString ParsedModelText;
	if (FParse::Value(Cmdline, TEXT("PursuitCharModel="), ParsedModelText) && !ParsedModelText.IsEmpty())
	{
		InferenceModelPath = ParsedModelText;
	}

	if (FParse::Param(Cmdline, TEXT("PursuitCharStatic")))
	{
		bStaticTarget = true;
	}

	// Stage 3 blockage control (see bScriptedStraightChase): the default Greedy driver is
	// the obstacle oracle because it already reflects off blockers, so the "is the pillar
	// really in the way" question needs its opposite - the same driver with the probe and
	// the detour removed.
	if (FParse::Param(Cmdline, TEXT("PursuitCharStraight")))
	{
		bScriptedStraightChase = true;
		UE_LOG(LogPursuitAI, Log,
			TEXT("PursuitCharEnv: straight-chase control requested - the scripted drivers will ignore blockers entirely"));
	}

	// Stage 3 observability evidence, asked for by name. Never baked into a level: the run
	// that produces the probe sweep must be distinguishable from the runs it is evidence
	// for, and an unmodified level has to stay re-verifiable without regenerating it.
	if (FParse::Param(Cmdline, TEXT("PursuitStage3ProbeSelfTest")))
	{
		bStage3ProbeSelfTest = true;
		UE_LOG(LogPursuitAI, Log,
			TEXT("PursuitCharEnv: Stage 3 probe self-test requested - one sweep will be logged at the end of episode 1"));
	}

	// Stage 4A jump gate, overridable so the jump-DISABLED control runs the very same level
	// instead of a differently-baked copy. That matters more here than anywhere else in this
	// file: the entire stage is "one variable", and a control that needed its own bake would
	// be a second variable hiding inside the first - the exact confusion Stage 3A's
	// straight-vs-oracle pair was designed to avoid.
	FString JumpGateText;
	if (FParse::Value(Cmdline, TEXT("PursuitEnableJump="), JumpGateText, false) && !JumpGateText.IsEmpty())
	{
		bEnableAgentJump = JumpGateText == TEXT("1") || JumpGateText.Equals(TEXT("true"), ESearchCase::IgnoreCase);
		UE_LOG(LogPursuitAI, Log,
			TEXT("PursuitCharEnv: -PursuitEnableJump=%s overrides the baked jump gate (SyncJumpGate pushes this onto every actuator)"),
			bEnableAgentJump ? TEXT("1") : TEXT("0"));
	}

	// Stage 4A observability evidence, asked for by name, one sweep per process.
	if (FParse::Param(Cmdline, TEXT("PursuitStage4WallProbeSelfTest")))
	{
		bStage4ProbeSelfTest = true;
		UE_LOG(LogPursuitAI, Log,
			TEXT("PursuitCharEnv: Stage 4 wall probe self-test requested - one sweep will be logged at the end of episode 1"));
	}

	// Wall-height override. This exists because the wall height is the ONE number Stage 4A has to
	// establish empirically rather than assert: whether a given height is clearable depends on the
	// live jump apex, the capsule size and the driver's fixed 0.3 s launch cadence, and the honest
	// way to find the usable band is to sweep it on the SAME level instead of baking a new level
	// per trial. Parsed before BuildArenaRig/BuildStage4Wall below, so the built box is the box
	// this value names.
	double ParsedWallHeight = 0.0;
	if (FParse::Value(Cmdline, TEXT("PursuitStage4WallHeight="), ParsedWallHeight) && ParsedWallHeight >= 10.0)
	{
		Stage4WallHeightCm = static_cast<float>(ParsedWallHeight);
		UE_LOG(LogPursuitAI, Log,
			TEXT("PursuitCharEnv: -PursuitStage4WallHeight=%.0f overrides the baked wall height"),
			Stage4WallHeightCm);
	}

	// Pane label, same convention as v1: FParse::Value would stop at a comma, so pass
	// "false" to keep commas and join words with underscores (spaces end the value).
	FString ParsedLabelText;
	if (FParse::Value(Cmdline, TEXT("PursuitPaneLabel="), ParsedLabelText, false) && !ParsedLabelText.IsEmpty())
	{
		PaneLabel = ParsedLabelText.Replace(TEXT("_"), TEXT(" "));
	}

	// Watched panes must step at the same rate for a fair two-pane clip. Movement is
	// dt-driven and episodes end at a step count, so two windows at different fps drift
	// apart in wall-clock terms and the A/B stops comparing like with like. Uncapped by
	// default; the pair tool pins both panes to the same number.
	double ParsedMaxFps = 0.0;
	if (FParse::Value(Cmdline, TEXT("PursuitCharMaxFPS="), ParsedMaxFps) && ParsedMaxFps > 0.0 && GEngine)
	{
		GEngine->SetMaxFPS(static_cast<float>(ParsedMaxFps));
	}

	FString ParsedSeedText;
	if (FParse::Value(Cmdline, TEXT("PursuitCharSeed="), ParsedSeedText) && !ParsedSeedText.IsEmpty())
	{
		Seed = FCString::Atoi(*ParsedSeedText);
		Rng.Initialize(Seed);
	}

	FString ParsedStartAtText;
	if (FParse::Value(Cmdline, TEXT("PursuitStartAt="), ParsedStartAtText) && !ParsedStartAtText.IsEmpty())
	{
		StartAtEpoch = FCString::Atod(*ParsedStartAtText);
		bWaitingForGo = StartAtEpoch > 0.0;
	}

	FString ParsedQuitAfterText;
	if (FParse::Value(Cmdline, TEXT("PursuitCharQuitAfter="), ParsedQuitAfterText) && !ParsedQuitAfterText.IsEmpty())
	{
		QuitAfterSeconds = FCString::Atod(*ParsedQuitAfterText);
	}

	// Exact episode-count exit, validation only. QuitAfterSeconds above is a wall-clock
	// safety net; this is the thing that guarantees the run contains exactly N complete
	// episodes. See MaxEpisodes' declaration for why a wall-clock budget cannot do it.
	FString ParsedMaxEpisodesText;
	if (FParse::Value(Cmdline, TEXT("PursuitCharMaxEpisodes="), ParsedMaxEpisodesText) && !ParsedMaxEpisodesText.IsEmpty())
	{
		MaxEpisodes = FMath::Max(FCString::Atoi(*ParsedMaxEpisodesText), 0);
		if (MaxEpisodes > 0)
		{
			UE_LOG(LogPursuitAI, Log,
				TEXT("PursuitCharEnv: validation mode - will exit after exactly %d completed episode(s)"),
				MaxEpisodes);
		}
	}

	// Stage selection, v1's mechanism carried to the char env. The city stage drops
	// the rig and borrows the Demonstration map: its ground is the floor, its lights
	// light the shot, and the arena parks on the plaza tools/inspect_city_stage.py
	// measured clean (centre -250,0, ground z=10 -> arena centre z=70, the v1
	// convention; capsules spawn ~1.5 m above asphalt and free-fall onto it).
	// -PursuitArenaAt overrides the plaza for any other staging spot.
	// bStartOnCityStage is the baked equivalent for TRAINING levels (the gen script
	// sets it on the city training map's env), applied before anything stage-dependent.
	if (bStartOnCityStage)
	{
		bCityStage = true;
		// The city stage used to also force bBuildArenaRig = false here, on the theory
		// that the city ground serves as the floor. Measured 2026-09-22 (v3.10 and
		// tools/inspect_plaza_flatness.py): the plaza is NOT flat - pure ground=10 only
		// reaches a 200 cm radius, and by the 500 cm spawn radius 26% of the disc is a
		// different height (steps of 100 / 120 / 148). Capsules spawned at a fixed Z then
		// landed on different planes, fell through the gaps, and the whole run was
		// `Falling` with a 2.5% grounded fraction - no walk speed, no learnable signal.
		// The floor is now independent of the city's own geometry: bCityRigFloor keeps
		// the flat plane, and the greybox walls/obstacles stay off because they would be
		// placed on top of real streets.
		UE_LOG(LogPursuitAI, Log,
			TEXT("PursuitCharEnv: baked city stage - arena centre from the actor transform (%.0f, %.0f, %.0f), rig floor %s"),
			GetActorLocation().X, GetActorLocation().Y, GetActorLocation().Z,
			bCityRigFloor ? TEXT("ON") : TEXT("off"));
	}
	FString StageText;
	if (FParse::Value(Cmdline, TEXT("PursuitStage="), StageText, false) && StageText == TEXT("city"))
	{
		bCityStage = true;
		SetActorLocation(FVector(-250.0f, 0.0f, 70.0f));
		UE_LOG(LogPursuitAI, Log,
			TEXT("PursuitCharEnv: city stage - arena parked at the measured plaza (-250, 0, 70), rig floor %s"),
			bCityRigFloor ? TEXT("ON") : TEXT("off"));
	}

	// -PursuitCityFloor=1/0 overrides the baked property, so a watched/comparison run can
	// flip the floor without regenerating the level.
	FString CityFloorText;
	if (FParse::Value(Cmdline, TEXT("PursuitCityFloor="), CityFloorText, false) && !CityFloorText.IsEmpty())
	{
		bCityRigFloor = CityFloorText == TEXT("1")
			|| CityFloorText.Equals(TEXT("true"), ESearchCase::IgnoreCase);
	}

	// Comma-safe on purpose: three coordinates, and FParse::Value stops at a comma
	// unless told not to (the same failure -PursuitPlayerAt hit first).
	FString ArenaAtText;
	if (FParse::Value(Cmdline, TEXT("PursuitArenaAt="), ArenaAtText, false))
	{
		TArray<FString> Parts;
		ArenaAtText.ParseIntoArray(Parts, TEXT(","), true);
		if (Parts.Num() == 3)
		{
			SetActorLocation(FVector(
				FCString::Atof(*Parts[0]), FCString::Atof(*Parts[1]), FCString::Atof(*Parts[2])));
			UE_LOG(LogPursuitAI, Log,
				TEXT("PursuitCharEnv: arena moved to (%.0f, %.0f, %.0f) by -PursuitArenaAt"),
				GetActorLocation().X, GetActorLocation().Y, GetActorLocation().Z);
		}
		else
		{
			// Refuse out loud rather than half-apply: an arena under the road reads
			// downstream as "the agents sank into the asphalt".
			UE_LOG(LogPursuitAI, Error,
				TEXT("PursuitCharEnv: could not parse -PursuitArenaAt='%s' (want X,Y,Z); arena unmoved"),
				*ArenaAtText);
		}
	}

	// City spawn/leash radius override. Parsed beside the stage selection so the very
	// first SpawnAgents already sees the final value.
	double ParsedCityRadius = 0.0;
	if (FParse::Value(Cmdline, TEXT("PursuitCitySpawnRadius="), ParsedCityRadius) && ParsedCityRadius >= 200.0)
	{
		CityStageRadius = static_cast<float>(ParsedCityRadius);
	}

	// Watched-camera tilt knobs. The defaults give the shot depth; -PursuitWatchPitch=-90
	// restores the old flat top-down for pane A/Bs that want the v1 framing back.
	// Height is overridable too: on the city stage a lower shot ducks behind facades,
	// a higher one clears them (all of this prints on the HUD cam= line every frame).
	double ParsedWatchHeight = 0.0;
	if (FParse::Value(Cmdline, TEXT("PursuitWatchHeight="), ParsedWatchHeight)
		&& ParsedWatchHeight >= 500.0 && ParsedWatchHeight <= 12000.0)
	{
		WatchCamHeight = static_cast<float>(ParsedWatchHeight);
	}
	double ParsedWatchPitch = 0.0;
	if (FParse::Value(Cmdline, TEXT("PursuitWatchPitch="), ParsedWatchPitch))
	{
		WatchCamPitch = FMath::Clamp(static_cast<float>(ParsedWatchPitch), -89.5f, -10.0f);
	}
	double ParsedWatchYaw = 0.0;
	if (FParse::Value(Cmdline, TEXT("PursuitWatchYaw="), ParsedWatchYaw))
	{
		WatchCamYaw = FMath::Clamp(static_cast<float>(ParsedWatchYaw), -180.0f, 180.0f);
	}
	double ParsedWatchFov = 0.0;
	if (FParse::Value(Cmdline, TEXT("PursuitWatchFOV="), ParsedWatchFov)
		&& ParsedWatchFov >= 20.0 && ParsedWatchFov <= 120.0)
	{
		WatchCamFOV = static_cast<float>(ParsedWatchFov);
	}

	// Absolute camera position, comma-safe (same three-coordinate parsing as -PursuitArenaAt).
	// When set it wins over the height/pitch/yaw derivation entirely: park at the world
	// point, aim at the arena centre.
	FString WatchAtText;
	if (FParse::Value(Cmdline, TEXT("PursuitWatchAt="), WatchAtText, false))
	{
		TArray<FString> Parts;
		WatchAtText.ParseIntoArray(Parts, TEXT(","), true);
		if (Parts.Num() == 3)
		{
			WatchCamAtOverride = FVector(
				FCString::Atof(*Parts[0]), FCString::Atof(*Parts[1]), FCString::Atof(*Parts[2]));
			bHasWatchCamOverride = true;
		}
		else
		{
			UE_LOG(LogPursuitAI, Error,
				TEXT("PursuitCharEnv: could not parse -PursuitWatchAt='%s' (want X,Y,Z); default placement kept"),
				*WatchAtText);
		}
	}

	// -PursuitWatchForce: hold the watched view even in Train mode (Schola connector
	// owns the tick). Presentation only - see bForceWatchView's header comment.
	if (FParse::Param(Cmdline, TEXT("PursuitWatchForce")))
	{
		bForceWatchView = true;
		UE_LOG(LogPursuitAI, Log,
			TEXT("PursuitCharEnv: -PursuitWatchForce set - watched camera will be set up in Train mode"));
	}

	// Hosted watch runs (the -PursuitCharEnvBox game-mode host) always want a watchable
	// driver. The Train default is only meaningful under the Schola training connector;
	// a hosted run that reached here without a drive flag otherwise stays silent - no
	// camera, no episodes, the user keeps the raw pawn view and reads that as "nothing
	// spawned" (2026-09-21 watch report).
	if (bHostedWatchRun && DriveMode == EPursuitCharDriveMode::Train)
	{
		DriveMode = EPursuitCharDriveMode::Greedy;
		UE_LOG(LogPursuitAI, Warning,
			TEXT("PursuitCharEnv: hosted watch run with no drive mode - defaulting to greedy"));
	}

	BindHitEvents();

	if (DriveMode == EPursuitCharDriveMode::Inference)
	{
		LoadChaserPolicy();
	}

	// Watched modes get the top-down camera; training never does, the trainer owns the
	// view (usually none - headless). A camera here is what makes a recording possible
	// at all: the char train level has no PlayerStart and no CineCamera of its own.
	// Exception: -PursuitWatchForce asks for the camera under the connector too, for
	// checkpoint-driven eval recordings.
	if (DriveMode != EPursuitCharDriveMode::Train || bForceWatchView)
	{
		SetupWatchedView();
	}

	// Floor is available two ways and they are not the same request:
	//   bBuildArenaRig  - the full greybox (floor + boundary ring + obstacles), for the
	//                     rig level.
	//   bCityRigFloor   - the flat collidable plane ALONE, for the city stage, where a
	//                     ring and greybox obstacles would land on top of real streets.
	// See bCityRigFloor's declaration for the v3.10 measurement that made the city need a
	// floor at all.
	if (bBuildArenaRig || bCityRigFloor)
	{
		BuildArenaRig();
	}

	// Stage 3's single tall pillar, after the rig so the floor it stands on exists and the
	// obstacle table could not have been built with it missing. Gated on its own switch, so
	// every level that does not ask for it builds exactly the geometry it always did.
	if (bStage3PillarLayout)
	{
		BuildStage3Pillar();
	}

	// Stage 4A's single low wall - same place, same reason: after the rig, so the floor it
	// stands on exists and so the boundary ring is already in the world when its
	// spawn-avoidance discs are registered (the wall overlaps the ring by design).
	if (bStage4WallLayout)
	{
		BuildStage4Wall();
	}

	if (bCityStage && bSpawnCityJumpPlatforms)
	{
		SpawnCityJumpPlatforms();
	}

	if (bCityStage && bSpawnCityPillars)
	{
		SpawnCityPillars();
	}
}

void APursuitCharEnv::SpawnCityJumpPlatforms()
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	UStaticMesh* CubeMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (!CubeMesh)
	{
		UE_LOG(LogPursuitAI, Error, TEXT("PursuitCharEnv: city jump platforms skipped - BasicShapes/Cube not found"));
		return;
	}

	// Three 75 cm platforms at fixed offsets from the arena centre. Height maths:
	// MaxStepHeight (45) cannot walk up, CatchHeightTolerance (70) cannot reach a
	// capture from the ground (feet delta 75 -> capsule-centre delta 75), and the
	// JumpZVelocity apex (~90 cm) clears the top - jump is the only way across.
	// Footprint 160x160 keeps the plaza walkable around them. Deterministic offsets,
	// not draws: identical layouts across episodes make A/B clips comparable.
	//
	// PLACEMENT REVISED 2026-09-22 (v3.4_spawnclear), same bug as the pillars above.
	// A 160 cm platform plus the 34 cm capsule radius claims 114 cm of clearance, so the
	// original spots at radii 283 / 316 / 310 cm had their near edges at 169 / 202 / 196 cm
	// - squarely inside the 154-248 cm spawn band. Together with the old pillar ring they
	// blocked 38% of the plaza circumference, and the platforms were the larger
	// contributors (114 cm each vs the pillars' 69 cm). Moved out to radii >= 390 cm so
	// the near edge stays past MaxSpawnSeparation + 10.
	//
	// They remain hoppable and remain between the plaza and the street, so the "jump"
	// lesson is intact; the only thing removed is the guaranteed contact at spawn.
	//
	// Constraint for any future edit: keep hypot(X,Y) - (80 + 34) >= 260.
	struct FPlatformSpot { float X; float Y; };
	const FPlatformSpot Spots[] = { {330.0f, 260.0f}, {-390.0f, 280.0f}, {-160.0f, -430.0f} };

	const float PlatformHeight = 75.0f;
	const float GroundZ = GetActorLocation().Z - 60.0f; // arena z=70, plaza ground z=10

	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	Params.ObjectFlags |= RF_Transient; // runtime scenery: never saved into a map

	for (const FPlatformSpot& Spot : Spots)
	{
		AStaticMeshActor* Block = World->SpawnActor<AStaticMeshActor>(
			AStaticMeshActor::StaticClass(),
			FTransform(FRotator::ZeroRotator,
				FVector(GetActorLocation().X + Spot.X, GetActorLocation().Y + Spot.Y, GroundZ + PlatformHeight * 0.5f)),
			Params);
		if (!Block)
		{
			continue;
		}
		UStaticMeshComponent* Mesh = Block->GetStaticMeshComponent();
		Mesh->SetStaticMesh(CubeMesh);
		Mesh->SetWorldScale3D(FVector(1.6f, 1.6f, PlatformHeight / 100.0f));
		Mesh->SetCollisionProfileName(TEXT("BlockAll"));
		Mesh->SetMobility(EComponentMobility::Movable);
#if WITH_EDITOR
		Block->SetActorLabel(FString::Printf(TEXT("CityJumpPlatform_%.0f_%.0f"), Spot.X, Spot.Y));
#endif
		CityPlatforms.Add(Block);
	}

	UE_LOG(LogPursuitAI, Log,
		TEXT("PursuitCharEnv: spawned %d city jump platform(s) at %.0f cm (ground z=%.0f)"),
		CityPlatforms.Num(), PlatformHeight, GroundZ);
}

void APursuitCharEnv::SpawnCityPillars()
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	UStaticMesh* CubeMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (!CubeMesh)
	{
		UE_LOG(LogPursuitAI, Error, TEXT("PursuitCharEnv: city pillars skipped - BasicShapes/Cube not found"));
		return;
	}

	// Full-height pillars in the plaza's OUTER annulus.
	//
	// WHY THESE EXIST (2026-09-22, measured): the plaza is a 500 cm disc, the spawn draw
	// lands the pair 170-430 cm from its centre, and the observation probes reach 600 cm.
	// A live dump of 13 greedy episodes showed what that geometry actually presents:
	// 5 episodes read 3/3 probes CLEAR, and every non-clear reading was a 0-31 cm ground
	// detail (kerbs, planters). Not one facade ever entered the cone. So the city stage
	// was, from the policy's point of view, an EMPTY ROOM - straight-line pursuit was the
	// optimal policy and got rewarded for it, which is exactly the behaviour that fails
	// the moment the chase leaves the plaza.
	//
	// The three 75 cm platforms above are hoppable and therefore teach "jump", but they
	// never teach "route around" - nothing in the stage required it. These pillars are
	// taller than the jump apex by design (300 cm), so the only way past is to steer
	// around, and the clearance channel (PursuitTargetSensor dims 8-10) tells the policy
	// which case it is looking at.
	//
	// PLACEMENT REVISED 2026-09-22 (v3.4_spawnclear) - THIS WAS THE wall_hits BUG.
	// ---------------------------------------------------------------------------
	// The first layout put pillars at radii 162 / 192 / 214 / 283 / 295 cm. That looked
	// like a reasonable "inside the plaza" ring, but the spawn band is only 154-248 cm
	// and a 70 cm pillar plus the 34 cm capsule radius claims 69 cm of clearance:
	//
	//     pillar(150, 60)  r=162  ->  block spans  93-231 cm   <- inside the spawn band
	//     pillar(-170,-90) r=192  ->  block spans 123-261 cm   <- inside the spawn band
	//     pillar(40,-210)  r=214  ->  block spans 145-283 cm   <- inside the spawn band
	//
	// Three pillars sat in the very lane the pair is spawned into, and with the three
	// 160 cm platforms (114 cm clearance each) they blocked 38% of the plaza
	// circumference. Measured over 45 episodes:
	//
	//     wall_hits: median ~10, but 77, 77, 141, 307, 2645  <- one episode ground along
	//     d_end    : median 952 cm against a 212 cm opening      geometry for all 6 s
	//
	// The stage was therefore teaching "grind into geometry" - the OPPOSITE of the
	// route-around skill these pillars were added for.
	//
	// The fix keeps the purpose and moves them clear of the spawn band: every pillar now
	// sits at radius >= 323 cm, so its near edge is >= 254 cm - outside the 248 cm spawn
	// cap plus margin. They still stand between the plaza and the street (leaving the
	// plaza still requires routing around them) and the 600 cm probes still see them from
	// the spawn ring, so the clearance channel keeps carrying the signal it was added for.
	//
	// Constraint for any future edit: keep hypot(X,Y) - (PillarFootprint/2 + 34) well
	// outside MaxSpawnSeparation. The guard below now enforces exactly that instead of the
	// old |X|<120 && |Y|<120 test, which checked the wrong region and passed every pillar
	// that actually caused the grinding.
	//
	// Deterministic ring layout, not a draw: identical across episodes keeps A/B clips
	// comparable, and the geometry stays inside CityStageRadius so the spawn and the
	// evader leash (both plaza-scoped) cannot place an agent inside one.
	struct FPillarSpot { float X; float Y; };
	const FPillarSpot Pillars[] = {
		{ 360.0f,  150.0f},   // r=390, inner edge 321
		{-340.0f, -180.0f},   // r=385, inner edge 316
		{  80.0f, -390.0f},   // r=398, inner edge 329
		{-300.0f,  330.0f},   // r=446, inner edge 377
		{ 400.0f, -220.0f},   // r=457, inner edge 388
	};

	const float PillarHeight = 300.0f;
	const float PillarFootprint = 70.0f;
	const float GroundZ = GetActorLocation().Z - 60.0f;

	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	Params.ObjectFlags |= RF_Transient;

	for (const FPillarSpot& Spot : Pillars)
	{
		// Skip any pillar whose block would reach into the spawn band. A spawn inside a
		// pillar is a guaranteed wall_hit at step 1 and poisons the episode, and a pillar
		// that merely OVERLAPS the band is what produced the 2645-hit grinding episode:
		// the pair opens inside it, the chaser immediately contacts it, and the 6 s budget
		// is spent against the face. The old guard (|X| < 120 && |Y| < 120) tested the
		// wrong region entirely and let every culprit through.
		const float DistFromCentre = FVector2D(Spot.X, Spot.Y).Size();
		const float BlockInnerEdge = DistFromCentre - (PillarFootprint * 0.5f) - 34.0f;
		if (BlockInnerEdge < MaxSpawnSeparation + 10.0f)
		{
			UE_LOG(LogPursuitAI, Warning,
				TEXT("PursuitCharEnv: pillar (%.0f, %.0f) skipped - its block starts at %.0f cm,")
				TEXT(" inside the %.0f cm spawn band (+10 margin)"),
				Spot.X, Spot.Y, BlockInnerEdge, MaxSpawnSeparation);
			continue;
		}

		AStaticMeshActor* Block = World->SpawnActor<AStaticMeshActor>(
			AStaticMeshActor::StaticClass(),
			FTransform(FRotator::ZeroRotator,
				FVector(GetActorLocation().X + Spot.X, GetActorLocation().Y + Spot.Y,
					GroundZ + PillarHeight * 0.5f)),
			Params);
		if (!Block)
		{
			continue;
		}
		UStaticMeshComponent* Mesh = Block->GetStaticMeshComponent();
		Mesh->SetStaticMesh(CubeMesh);
		Mesh->SetWorldScale3D(FVector(PillarFootprint / 100.0f, PillarFootprint / 100.0f,
			PillarHeight / 100.0f));
		Mesh->SetCollisionProfileName(TEXT("BlockAll"));
		Mesh->SetMobility(EComponentMobility::Movable);
#if WITH_EDITOR
		Block->SetActorLabel(FString::Printf(TEXT("CityPillar_%.0f_%.0f"), Spot.X, Spot.Y));
#endif
		CityPillars.Add(Block);
	}

	UE_LOG(LogPursuitAI, Log,
		TEXT("PursuitCharEnv: spawned %d city pillar(s) at %.0f cm tall, %.0f cm wide (ground z=%.0f)"),
		CityPillars.Num(), PillarHeight, PillarFootprint, GroundZ);
}

void APursuitCharEnv::LoadChaserPolicy()
{
	if (!ChaserAgent)
	{
		UE_LOG(LogPursuitAI, Error,
			TEXT("PursuitCharEnv: Inference mode needs a ChaserAgent reference - falling back to the greedy baseline"));
		DriveMode = EPursuitCharDriveMode::Greedy;
		return;
	}

	// Relative paths are project-relative, so the same -PursuitCharModel= command line
	// works no matter which directory the process was started from.
	FString FilePath = InferenceModelPath;
	if (FPaths::IsRelative(FilePath))
	{
		FilePath = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir(), FilePath);
	}

	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *FilePath) || Bytes.Num() == 0)
	{
		UE_LOG(LogPursuitAI, Error,
			TEXT("PursuitCharEnv: no ONNX at '%s' - falling back to the greedy baseline. Export one with tools\\export_policy.bat"),
			*FilePath);
		DriveMode = EPursuitCharDriveMode::Greedy;
		return;
	}

	// UNNEModelData::Init takes the *file extension* as the type tag; the ONNX runtime
	// uses it to pick a producer. Doing this at runtime is what keeps the whole
	// train -> export -> run path headless, exactly as v1's ResolveInferenceModel.
	UNNEModelData* Model = NewObject<UNNEModelData>(this, TEXT("PursuitCharModelData"));
	Model->Init(TEXT("onnx"),
		TConstArrayView64<uint8>(Bytes.GetData(), static_cast<int64>(Bytes.Num())));
	ChaserAgent->ModelData = Model;

	// Env and agent BeginPlay order is level-order dependent: if the chaser already ran
	// its BeginPlay as a scripted agent, wire its runtime now; if it has not run yet,
	// its own BeginPlay sees the ModelData and does the same thing.
	if (ChaserAgent->HasActorBegunPlay())
	{
		ChaserAgent->EnsureInferenceRuntime();
	}

	UE_LOG(LogPursuitAI, Log,
		TEXT("PursuitCharEnv: loaded %d bytes of ONNX into the chaser: %s"), Bytes.Num(), *FilePath);
}

void APursuitCharEnv::SetupWatchedView()
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	// Tilted orbit over the arena centre: the camera keeps WatchCamHeight of altitude
	// but walks back along -Yaw by WatchCamHeight / tan(|pitch|), so the view axis
	// passes exactly through the arena centre at any tilt. At -90 the back-off is zero
	// and this is the old straight-down view; at the -65 default the arena gains depth
	// and a jump reads as vertical motion instead of a shrinking silhouette.
	const float PitchRadians = FMath::DegreesToRadians(FMath::Abs(WatchCamPitch));
	const float BackOff = WatchCamHeight / FMath::Tan(PitchRadians);
	const float YawRadians = FMath::DegreesToRadians(WatchCamYaw);
	// Two placements: the derived orbit (centre + height/pitch/yaw back-off) or the
	// -PursuitWatchAt absolute override, which parks at the given world point and
	// aims at the arena centre instead.
	const FVector Focus = GetActorLocation();
	FVector CameraLocation = Focus
		- FVector(FMath::Cos(YawRadians) * BackOff, FMath::Sin(YawRadians) * BackOff, 0.0f)
		+ FVector(0.0f, 0.0f, WatchCamHeight);
	FRotator CameraRotation(WatchCamPitch, WatchCamYaw, 0.0f);
	if (bHasWatchCamOverride)
	{
		CameraLocation = WatchCamAtOverride;
		CameraRotation = (Focus - WatchCamAtOverride).Rotation();
	}

	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	WatchCamera = World->SpawnActor<ACameraActor>(
		ACameraActor::StaticClass(),
		CameraLocation,
		CameraRotation,
		Params);
	if (WatchCamera)
	{
		// FOV lives on the camera component, not the actor.
		if (UCameraComponent* CamComponent = WatchCamera->GetCameraComponent())
		{
			CamComponent->SetFieldOfView(WatchCamFOV);
		}
		if (APlayerController* PC = GEngine ? GEngine->GetFirstLocalPlayerController(World) : nullptr)
		{
			// Kill the PC's auto camera management BEFORE claiming the view: on the
			// EnvBox city stage the DefaultPawn spawns after this BeginPlay and its
			// auto target switch used to steal the view back - watched runs opened
			// first-person (2026-09-21 watch report).
			PC->bAutoManageActiveCameraTarget = false;
			PC->SetViewTarget(WatchCamera);
		}
	}
	else
	{
		UE_LOG(LogPursuitAI, Warning, TEXT("PursuitCharEnv: watched view camera failed to spawn"));
	}
}

void APursuitCharEnv::UpdateWatchedHud()
{
	if (!GEngine)
	{
		return;
	}

	// Refreshed every tick with a short duration, v1's pane-panel trick: a message that
	// is continuously re-added is stable in the frame, and vanishes the moment its
	// owner stops drawing it.
	if (!PaneLabel.IsEmpty())
	{
		GEngine->AddOnScreenDebugMessage(8100, 0.5f, FColor(255, 230, 120), PaneLabel,
			false, FVector2D(2.0f, 2.0f));
	}

	if (bWaitingForGo)
	{
		GEngine->AddOnScreenDebugMessage(8120, 0.5f, FColor(120, 220, 255),
			TEXT("WAITING FOR GO"), false, FVector2D(2.2f, 2.2f));
	}

	// Presentation overlay only. These values come from the same live actors and episode
	// clock as the evaluation log; this does not change the observation or reward.
	const float GapCm = (ChaserAgent && EvaderAgent)
		? FVector::Dist2D(ChaserAgent->GetActorLocation(), EvaderAgent->GetActorLocation())
		: 0.0f;
	const TCHAR* PolicyName = TEXT("TRAIN");
	switch (DriveMode)
	{
	case EPursuitCharDriveMode::Random: PolicyName = TEXT("RANDOM"); break;
	case EPursuitCharDriveMode::Greedy: PolicyName = TEXT("GREEDY SCRIPT"); break;
	case EPursuitCharDriveMode::Inference: PolicyName = TEXT("ONNX POLICY"); break;
	default: break;
	}
	GEngine->AddOnScreenDebugMessage(8101, 0.5f, FColor(235, 235, 235),
		FString::Printf(TEXT("%s  |  episode %d  |  %.1f / %.0f s  |  gap %.0f cm  |  catch < %.0f cm  |  target %.2fx"),
			PolicyName, EpisodeIndex, EpisodeSimTime, EpisodeSeconds, GapCm, CatchRadius, EvaderSpeedRatio),
		false, FVector2D(1.8f, 1.8f));

	// Camera diagnostic: which actor owns the view, where the watch camera sits and
	// where both agents are. A watch shot that shows nothing is either a stolen view
	// target or a camera looking through city geometry - this line answers which,
	// on screen, every frame (2026-09-21 "no characters visible" report).
	if (FParse::Param(FCommandLine::Get(), TEXT("PursuitWatchDebug")))
	{
	  if (APlayerController* PC = GEngine->GetFirstLocalPlayerController(GetWorld()))
	  {
		const AActor* ViewTarget = PC->GetViewTarget();
		const FVector CamPos = WatchCamera ? WatchCamera->GetActorLocation() : FVector::ZeroVector;
		const FVector DogPos = ChaserAgent ? ChaserAgent->GetActorLocation() : FVector::ZeroVector;
		const FVector HeroPos = EvaderAgent ? EvaderAgent->GetActorLocation() : FVector::ZeroVector;
		const FVector SolPos = SupportAgent ? SupportAgent->GetActorLocation() : FVector::ZeroVector;
		GEngine->AddOnScreenDebugMessage(8102, 0.5f, FColor(170, 200, 255),
			FString::Printf(TEXT("view=%s  cam=(%.0f, %.0f, %.0f)  dog=(%.0f, %.0f)  hero=(%.0f, %.0f)%s"),
				ViewTarget ? *ViewTarget->GetName() : TEXT("none"),
				CamPos.X, CamPos.Y, CamPos.Z, DogPos.X, DogPos.Y, HeroPos.X, HeroPos.Y,
				SupportAgent
					? *FString::Printf(TEXT("  sol=(%.0f, %.0f)"), SolPos.X, SolPos.Y)
					: TEXT("")),
			false, FVector2D(1.5f, 1.5f));
	  }
	}
}

void APursuitCharEnv::BuildArenaRig()
{
	// Engine basic shapes, loaded by soft path: /Engine content is always present, has
	// baked simple collision, and imports nothing. Plane is 100x100 cm, Cube is a
	// 100 cm cube, both centred on their origins.
	UStaticMesh* FloorMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Plane.Plane"));
	UStaticMesh* WallMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (!FloorMesh || !WallMesh)
	{
		// Fail loudly: a rig without a floor means two capsules falling forever, which
		// reads downstream as "the policy broke" rather than "the stage is missing".
		UE_LOG(LogPursuitAI, Error, TEXT("PursuitCharEnv: could not load engine basic shapes for the arena rig"));
		return;
	}

	const FVector Centre = GetActorLocation();

	// --- floor ---
	RigFloor = NewObject<UStaticMeshComponent>(this, TEXT("ArenaRigFloor"));
	RigFloor->SetStaticMesh(FloorMesh);
	RigFloor->SetCollisionProfileName(TEXT("BlockAll"));
	RigFloor->SetupAttachment(GetRootComponent());
	RigFloor->SetRelativeLocation(FVector(0.0f, 0.0f, 0.0f));
	RigFloor->SetRelativeScale3D(FVector(RigFloorScale, RigFloorScale, 1.0f));
	RigFloor->RegisterComponent();

	// --- boundary ring: tangential box segments centred on the arena radius ---
	// Skipped when only the city floor was asked for (bCityRigFloor): a ring would fence
	// off the streets that are supposed to be part of the city stage.
	RigWalls.Reset();
	if (bBuildArenaRig)
	{
	const int32 WallCount = 24;
	const float SegmentLength = 2.0f * UE_PI * ArenaRadius / WallCount;
	for (int32 Index = 0; Index < WallCount; ++Index)
	{
		const float AngleDegrees = 360.0f * Index / WallCount;
		const float AngleRadians = FMath::DegreesToRadians(AngleDegrees);

		UStaticMeshComponent* Wall = NewObject<UStaticMeshComponent>(this,
			*FString::Printf(TEXT("ArenaRigWall%02d"), Index));
		Wall->SetStaticMesh(WallMesh);
		Wall->SetCollisionProfileName(TEXT("BlockAll"));
		Wall->SetupAttachment(GetRootComponent());
		Wall->SetRelativeLocation(FVector(
			ArenaRadius * FMath::Cos(AngleRadians),
			ArenaRadius * FMath::Sin(AngleRadians),
			RigWallHeight * 0.5f));
		Wall->SetRelativeRotation(FRotator(0.0f, AngleDegrees + 90.0f, 0.0f));
		Wall->SetRelativeScale3D(FVector(SegmentLength / 100.0f, 50.0f / 100.0f, RigWallHeight / 100.0f));
		Wall->RegisterComponent();
		RigWalls.Add(Wall);
	}
	}

	// --- lighting: the generated training level carries NO lights (its gen script
	// says so: "no lights (training is headless)"), so a rendered watched window is
	// pitch black. The rig lights itself: an angled directional key + a sky fill.
	// Shadowless - shadows buy nothing at 2.6 km viewing height and cost GPU. ---
	FRotator SunRotation(-55.0f, 35.0f, 0.0f);
	if (ADirectionalLight* Sun = GetWorld()->SpawnActor<ADirectionalLight>(
		ADirectionalLight::StaticClass(), FVector(Centre.X, Centre.Y, 2000.0f), SunRotation))
	{
		Sun->GetLightComponent()->SetIntensity(5.0f);
		Sun->GetLightComponent()->SetCastShadows(false);
		Sun->GetLightComponent()->SetMobility(EComponentMobility::Movable);
	}
	if (ASkyLight* Sky = GetWorld()->SpawnActor<ASkyLight>(
		ASkyLight::StaticClass(), FVector(Centre.X, Centre.Y, 2000.0f), FRotator::ZeroRotator))
	{
		Sky->GetLightComponent()->SetIntensity(1.5f);
		Sky->GetLightComponent()->SetCastShadows(false);
		Sky->GetLightComponent()->SetMobility(EComponentMobility::Movable);
	}

	UE_LOG(LogPursuitAI, Log,
		TEXT("PursuitCharEnv: arena rig built - floor %.0f cm square at z=%.0f, %d wall segments, radius %.0f cm (lights spawned)"),
		100.0f * RigFloorScale, Centre.Z, RigWalls.Num(), ArenaRadius);

	// --- obstacles: the complexity layer. Tall pillars force pathing, low boxes are
	// hop-over-able with the jump actuator, low walls create chokes. The layout is the
	// fixed spec table above; spawn avoidance reads RigObstacleDiscs, so a capsule can
	// never materialise inside geometry. Wall-hit penalty flows through HandleChaserHit
	// unchanged: any solid that is not the evader is a wall hit, obstacles included. ---
	// City floor only => no greybox obstacles: they are specced in rig coordinates and
	// would be dropped on top of the streets. The city's own buildings already give the
	// chaser solids to hit, and HandleChaserHit counts them the same way.
	RigObstacles.Reset();
	RigObstacleDiscs.Reset();
	// Two conditions, not one (2026-09-22, Stage 0): the floor + boundary ring above are
	// gated on bBuildArenaRig, and the obstacle TABLE is gated on bSpawnArenaObstacles as
	// well. Until this split the only obstacle-free arena was "no rig at all", which also
	// deleted the boundary - and a curved wall is the one thing that proves the
	// wall-hit counter still fires (see HandleChaserHit's exclusions).
	if (bBuildArenaRig && bSpawnArenaObstacles)
	{
	for (int32 Index = 0; Index < UE_ARRAY_COUNT(ObstacleSpecs); ++Index)
	{
		const FObstacleSpec& Spec = ObstacleSpecs[Index];

		UStaticMeshComponent* Obstacle = NewObject<UStaticMeshComponent>(this,
			*FString::Printf(TEXT("ArenaRigObstacle%02d"), Index));
		Obstacle->SetStaticMesh(WallMesh);
		Obstacle->SetCollisionProfileName(TEXT("BlockAll"));
		Obstacle->SetupAttachment(GetRootComponent());
		// Cube's origin is its centre, so Z puts the base on the floor plane.
		Obstacle->SetRelativeLocation(FVector(Spec.X, Spec.Y, 100.0f * Spec.SZ * 0.5f));
		Obstacle->SetRelativeScale3D(FVector(Spec.SX, Spec.SY, Spec.SZ));
		Obstacle->RegisterComponent();
		RigObstacles.Add(Obstacle);

		FPursuitObstacle Disc;
		Disc.Centre = FVector2D(Spec.X, Spec.Y);
		// Half-diagonal of the footprint, plus one capsule radius, so "clear of the disc"
		// means the whole capsule clears the corner, not just its centre point.
		Disc.ClearRadius = Spec.ClearRadius + 34.0f;
		RigObstacleDiscs.Add(Disc);
	}
	}

	UE_LOG(LogPursuitAI, Log,
		TEXT("PursuitCharEnv: %d obstacles placed (%s)"),
		RigObstacles.Num(),
		!bBuildArenaRig ? TEXT("city floor only, city geometry is the obstacle")
			: (bSpawnArenaObstacles ? TEXT("pillars, low boxes and chokes") : TEXT("disabled - flat arena with boundary ring")));
}

void APursuitCharEnv::BindHitEvents()
{
	// Only the chaser is scored for wall hits (the evader's brushes are its own business),
	// and the binding is remove-then-add so a level reload cannot stack duplicates.
	if (ChaserAgent)
	{
		ChaserAgent->OnActorHit.RemoveDynamic(this, &APursuitCharEnv::HandleChaserHit);
		ChaserAgent->OnActorHit.AddDynamic(this, &APursuitCharEnv::HandleChaserHit);
	}
}

void APursuitCharEnv::HandleChaserHit(AActor* HitActor, AActor* OtherActor, FVector NormalImpulse, const FHitResult& Hit)
{
	(void)NormalImpulse;

	// NOTE on the counting window (2026-09-22). The counted class (WallHitCount) is
	// PER EPISODE - it is zeroed in BeginEpisode and printed in that episode's summary. The
	// IGNORED classes below are a RUN-LEVEL CENSUS: they are never reset, and every
	// classification line that is logged is also counted, so the log and the tally can
	// never disagree.
	//
	// Why the asymmetry: a per-episode window for the ignored classes LOSES the only
	// reachable evader contact. The catch fires at CatchRadius (120 cm) which is wider than
	// the ~68 cm capsule contact distance, so inside a scored episode the chaser wins
	// without ever touching the target - and the one contact that does occur is the level's
	// initial frame, where both capsules are placed on the same spot, which the first
	// BeginEpisode then wipes. The census keeps that contact as evidence; a per-episode
	// tally silently deletes it and leaves `target=0` next to a logged CLASS=ignored_target
	// line.

	// ---------------------------------------------------------------------------
	// What counts as a wall hit, and why the old body was wrong (2026-09-22, Stage 0
	// audit; the fix is deliberately conservative per the user's constraint).
	//
	// The old body was `++WallHitCount` UNCONDITIONALLY, with only the reward flag gated
	// on "not the evader". The dominant pollution was the EVADER ITSELF: in a chasing
	// episode the two capsules press together for much of the approach, so an episode
	// that successfully caught its target read `wall_hits=99` while the chaser had never
	// once touched a vertical obstacle (2026-09-22 greedy run: CAUGHT in 1473 steps,
	// wall_hits=99). Support surfaces are the second source - a capsule standing on the
	// floor reports contacts there, and the floor is not a wall. Both are excluded below,
	// and the per-episode tally of what was excluded is printed with every validation
	// summary, so the classification can be checked instead of trusted.
	//
	// Every StaticMeshActor is deliberately NOT excluded: the city facades and the rig
	// pillars/boxes ARE StaticMeshActors, so "ignore static meshes" would delete the
	// exact signal this counter exists for (steering into a building). Only
	// unambiguous non-walls are dropped:
	//
	//   1. no other actor at all - world geometry with no owning actor
	//   2. the chaser hitting itself
	//   3. the evader - that contact IS the goal, never a penalty
	//   4. the rig floor component, by identity
	//   5. a near-horizontal support/contact normal (|Z| > 0.7): floors, ramp tops, and
	//      the top face of a low box the agent has climbed onto. A wall's normal is
	//      horizontal, so this cannot swallow a facade or a pillar.
	//
	// Anything that survives all five is counted, and the first few are logged with
	// their classification so "standing on flat ground does not accumulate / hitting
	// the boundary wall does / touching the target does not" can be read straight off
	// the log instead of being taken on faith.
	// ---------------------------------------------------------------------------
	AActor* const SelfActor = HitActor ? HitActor : ChaserAgent;

	// Classification log: the first few contacts of EVERY class, so the three claims this
	// counter makes can be read off the log instead of taken on faith -
	//   平地站立不累计   -> CLASS=ignored_floor / ignored_support, counted stays 0
	//   撞边界墙会累计   -> CLASS=counted, normal is horizontal (|normal.Z| ~ 0)
	//   接触目标不累计   -> CLASS=ignored_target, other = the evader
	// Capped per class: a sustained contact fires OnActorHit many times per second, and an
	// uncapped log would bury the rest of the file. The target class has its own cap for
	// the reason spelled out on WallHitTargetClassLogCount.
	// Named LogWallContact and not LogClass: UE already declares a LogClass log category,
	// and shadowing it is a warning-as-error on this toolchain.
	auto LogWallContact = [this](const TCHAR* ClassName, AActor* ClassOther, const FHitResult& ClassHit,
		int32& Counter, int32 Cap)
	{
		if (!bLogEpisodes || Counter >= Cap)
		{
			return;
		}
		++Counter;
		UE_LOG(LogPursuitAI, Log,
			TEXT("PursuitCharEnv: wall-hit CLASS=%s other=%s comp=%s normal=(%.2f, %.2f, %.2f) |normal.Z|=%.2f"),
			ClassName, *GetNameSafe(ClassOther), *GetNameSafe(ClassHit.GetComponent()),
			ClassHit.ImpactNormal.X, ClassHit.ImpactNormal.Y, ClassHit.ImpactNormal.Z,
			FMath::Abs(ClassHit.ImpactNormal.Z));
	};

	if (OtherActor == nullptr)
	{
		++WallHitIgnoredSelfCount;
		LogWallContact(TEXT("ignored_no_actor"), OtherActor, Hit, WallHitClassLogCount, 8);
		return;
	}
	if (OtherActor == SelfActor || OtherActor == ChaserAgent)
	{
		++WallHitIgnoredSelfCount;
		LogWallContact(TEXT("ignored_self"), OtherActor, Hit, WallHitClassLogCount, 8);
		return;
	}
	if (OtherActor == EvaderAgent)
	{
		++WallHitIgnoredTargetCount;
		LogWallContact(TEXT("ignored_target"), OtherActor, Hit, WallHitTargetClassLogCount, 3);
		return;
	}
	if (RigFloor && Hit.GetComponent() == RigFloor)
	{
		++WallHitIgnoredFloorCount;
		LogWallContact(TEXT("ignored_floor"), OtherActor, Hit, WallHitClassLogCount, 8);
		return;
	}
	if (FMath::Abs(Hit.ImpactNormal.Z) > 0.7f)
	{
		++WallHitIgnoredSupportCount;
		LogWallContact(TEXT("ignored_support"), OtherActor, Hit, WallHitClassLogCount, 8);
		return;
	}

	LogWallContact(TEXT("counted"), OtherActor, Hit, WallHitClassLogCount, 8);

	// Stage 3 attribution. `wall_hits_counted` on its own cannot tell "the chaser bumped
	// the pillar it was supposed to route around" from "the chaser ran into the boundary
	// ring", and this stage's entire Go/No-Go turns on which of the two happened. The
	// component identity answers it: the pillar is built by this env and keeps its name,
	// while the ring segments are ArenaRigWallNN.
	if (bStage3PillarLayout)
	{
		if (Stage3Pillar && Hit.GetComponent() == Stage3Pillar)
		{
			++Stage3PillarHitsThisEpisode;
		}
		else
		{
			++Stage3OtherWallHitsThisEpisode;
		}
	}

	// The metric latches (Stage 3B): set on a counted contact and consumed exactly once per
	// control step by AccumulateValidationSample. `bWallContactStepLatch` is set for ANY
	// counted contact on ANY map, so the duration metric is not Stage-3-specific; the pillar
	// latch narrows it to this stage's obstacle. Neither is read by the reward - they are the
	// BOUNDED counterpart of `WallHitCount` below, which accumulates once per physics tick
	// and therefore has no upper limit per episode.
	bWallContactStepLatch = true;
	if (bStage3PillarLayout && Stage3Pillar && Hit.GetComponent() == Stage3Pillar)
	{
		bStage3PillarHitThisStep = true;
	}

	++WallHitCount;
	bWallHitThisStep = true;
}

// ---------------------------------------------------------------------------
// ISingleAgentScholaEnvironment - the training side, driven over gRPC
// ---------------------------------------------------------------------------

void APursuitCharEnv::InitializeEnvironment_Implementation(FInteractionDefinition& OutAgentDefinition)
{
	// The spaces come from the agent's own components (sensor + actuator), via the same
	// Define() an exported-ONNX policy will later be built from. There is no second
	// description of the spaces anywhere in v2, so train/deploy drift is structurally
	// impossible rather than merely avoided.
	if (!ChaserAgent)
	{
		UE_LOG(LogPursuitAI, Error,
			TEXT("PursuitCharEnv: InitializeEnvironment with no ChaserAgent - the level must assign one"));
		return;
	}

	IAgent::Execute_Define(ChaserAgent, OutAgentDefinition);

	// Interface-space dimension print (Stage 0 acceptance item: "observation/action dims
	// verified"). These spaces are enumerated from the agent's OWN components, so this
	// line is the ground truth the trainer's policy head and any exported ONNX are built
	// from - there is no second description of them anywhere in v2.
	//
	// Dims are summed recursively because the ACTION space is legitimately packaged as a
	// Dict {CharMoveInput: Box(2), JumpInput: Box(1)} on this side, while SB3 flattens it
	// to one Box(3) before the policy sees it. Both must report 3, and that equality is
	// the point: if a jump dimension were ever removed to "switch jumping off", this would
	// read 2 and the whole 15D/3D contract the curriculum is defined against would break
	// silently. That is exactly why the switch is an executor gate (bEnableAgentJump) and
	// not a deleted component.
	auto CountBoxDims = [](const TInstancedStruct<FSpace>& Space) -> int32
	{
		TArray<const TInstancedStruct<FSpace>*> Stack;
		Stack.Add(&Space);
		int32 Total = 0;
		while (Stack.Num() > 0)
		{
			const TInstancedStruct<FSpace>* Current = Stack.Pop();
			if (const FBoxSpace* BoxSpace = Current->GetPtr<FBoxSpace>())
			{
				Total += BoxSpace->Dimensions.Num();
			}
			else if (const FDictSpace* DictSpace = Current->GetPtr<FDictSpace>())
			{
				for (const TPair<FString, TInstancedStruct<FSpace>>& Pair : DictSpace->Spaces)
				{
					Stack.Add(&Pair.Value);
				}
			}
		}
		return Total;
	};

	UE_LOG(LogPursuitAI, Log,
		TEXT("PursuitCharEnv: INTERFACE SPACES obs_dims=%d preflatten action_dims=%d (expect obs 15, action 3)"),
		CountBoxDims(OutAgentDefinition.ObsSpaceDefn),
		CountBoxDims(OutAgentDefinition.ActionSpaceDefn));
}

void APursuitCharEnv::SeedEnvironment_Implementation(int InSeed)
{
	Seed = InSeed;
	Rng.Initialize(InSeed);
	UE_LOG(LogPursuitAI, Log, TEXT("PursuitCharEnv: SeedEnvironment(%d)"), InSeed);
}

void APursuitCharEnv::SetEnvironmentOptions_Implementation(const TMap<FString, FString>& InOptions)
{
	// Same "parse and log" shape as the official Tag environment: every knob the trainer
	// may legitimately turn from Python, visible in the first round-trip.
	auto ApplyInt = [&InOptions](const TCHAR* Key, int32& Value)
	{
		if (const FString* OptionValue = InOptions.Find(Key))
		{
			int32 Parsed = Value;
			if (FDefaultValueHelper::ParseInt(*OptionValue, Parsed))
			{
				Value = Parsed;
			}
		}
	};
	auto ApplyFloat = [&InOptions](const TCHAR* Key, float& Value)
	{
		if (const FString* OptionValue = InOptions.Find(Key))
		{
			float Parsed = Value;
			if (FDefaultValueHelper::ParseFloat(*OptionValue, Parsed))
			{
				Value = Parsed;
			}
		}
	};
	auto ApplyBool = [&InOptions](const TCHAR* Key, bool& Value)
	{
		if (const FString* OptionValue = InOptions.Find(Key))
		{
			if (OptionValue->Equals(TEXT("true"), ESearchCase::IgnoreCase) || *OptionValue == TEXT("1"))
			{
				Value = true;
			}
			else if (OptionValue->Equals(TEXT("false"), ESearchCase::IgnoreCase) || *OptionValue == TEXT("0"))
			{
				Value = false;
			}
		}
	};

	ApplyInt(TEXT("MaxSteps"), MaxSteps);
	ApplyFloat(TEXT("EpisodeSeconds"), EpisodeSeconds);
	ApplyFloat(TEXT("CatchRadius"), CatchRadius);
	ApplyFloat(TEXT("ArenaRadius"), ArenaRadius);
	ApplyFloat(TEXT("CatchReward"), CatchReward);
	ApplyFloat(TEXT("DistanceShapingScale"), DistanceShapingScale);
	ApplyFloat(TEXT("StepPenalty"), StepPenalty);
	ApplyFloat(TEXT("WallHitPenalty"), WallHitPenalty);
	ApplyFloat(TEXT("AirbornePenalty"), AirbornePenalty);
	ApplyFloat(TEXT("JumpActionPenalty"), JumpActionPenalty);
	ApplyFloat(TEXT("ProximityBonus"), ProximityBonus);
	ApplyFloat(TEXT("ProximityRadiusCm"), ProximityRadiusCm);
	ApplyFloat(TEXT("TimeoutPenalty"), TimeoutPenalty);
	ApplyFloat(TEXT("MaxSpawnSeparation"), MaxSpawnSeparation);
	ApplyFloat(TEXT("MinSpawnSeparation"), MinSpawnSeparation);
	ApplyFloat(TEXT("EvaderSpeedRatio"), EvaderSpeedRatio);
	ApplyBool(TEXT("bStaticTarget"), bStaticTarget);
	ApplyBool(TEXT("bLogEpisodes"), bLogEpisodes);
}

void APursuitCharEnv::BeginEpisode()
{
	CurrentStep = 0;
	EpisodeSimTime = 0.0f;
	TotalReward = 0.0f;
	CurrentReward = 0.0f;
	bCaught = false;
	bWallHitThisStep = false;
	bStage3PillarHitThisStep = false;
	bWallContactStepLatch = false;
	bJumpRequestedThisStep = false;

	// v1's anti-divergence rule, unchanged: the spawn layout is keyed to the EPISODE
	// number, not to how many spawns a particular run happens to have consumed. Two
	// panes on episode N get episode N's layout even if one has caught fourteen times
	// and the other is still inside its first episode.
	Rng.Initialize(Seed != 0 ? Seed + EpisodeIndex : FMath::Rand());
	DriveRng.Initialize(Seed != 0 ? Seed + DriveStreamSeedOffset + EpisodeIndex : FMath::Rand());

	// Drop the velocity history BEFORE SpawnAgents teleports anyone.
	//
	// Order matters and used to be wrong: ResetVelocityHistory ran AFTER the sanity
	// probe below, so the probe's target-speed reading was differenced against the
	// PREVIOUS episode's final position across a full-map teleport. Measured on the
	// v3.5 log: tgt_spd read exactly 1.000 (saturated) on episode 2 onward while the
	// evader was in fact standing still - a false "prey is sprinting away" signal on
	// the first step of every episode. Clearing here makes step-1 read 0.0, the truth.
	if (ChaserAgent && ChaserAgent->TargetSensor)
	{
		ChaserAgent->TargetSensor->ResetVelocityHistory();
	}
	if (EvaderAgent && EvaderAgent->TargetSensor)
	{
		EvaderAgent->TargetSensor->ResetVelocityHistory();
	}

	SpawnAgents();
	++EpisodeIndex;
	bEpisodeRunning = true;
	WallHitCount = 0;
	WallProbeHitsThisEpisode = 0;
	ClosestWallProbeThisEpisode = 1.0f;
	HopBlockedReadingsThisEpisode = 0;

	// Observation sanity probe: once per episode, dump what the chaser's sensor
	// actually reports against the known spawn geometry. If the forward/right
	// components don't point at the evader, the policy has been blind all along.
	if (ChaserAgent)
	{
		FInstancedStruct ProbeObs;
		IAgent::Execute_Observe(ChaserAgent, ProbeObs);
		// The agent composes its sensors into a Dict keyed by component name; the one
		// shared unpacker pulls the TargetSensor entry out - this used to be one of three
		// hand-written copies of the same three lines, and the copy in the wall-probe
		// counter was the broken one (see ResolveTargetSensorPoint).
		const FBoxPoint* ProbePoint = ResolveTargetSensorPoint(ProbeObs);
		if (ProbePoint && ProbePoint->Values.Num() >= 5)
		{
			// 2026-09-22: this used to print only the first five values, which hid the
			// three wall probes entirely - a "blind to walls" bug would have looked
			// exactly like this log. Print every dimension the sensor emits, and flag
			// whether the probes carry real geometry or are all saturating at "clear"
			// (1.00 everywhere = the sweep finds nothing, i.e. still blind).
			FString Tail;
			int32 ClearProbes = 0;
			const int32 NumValues = ProbePoint->Values.Num();
			// Layout: ranges at 5..5+N-1, clearances at 5+N..5+2N-1 (see the sensor header).
			// Derive the probe count from the tensor width so this print stays correct if
			// the fan grows again: (width - 5) / 2.
			const int32 ProbeCount = FMath::Max((NumValues - 5) / 2, 0);
			const int32 ClearanceBase = 5 + ProbeCount;
			for (int32 Index = 5; Index < 5 + ProbeCount; ++Index)
			{
				const float Range = ProbePoint->Values[Index];
				const int32 ClearIndex = (Index - 5) + ClearanceBase;
				const float Clearance = (ClearIndex < NumValues) ? ProbePoint->Values[ClearIndex] : -1.0f;
				Tail += FString::Printf(TEXT("%s%.2f"), Index > 5 ? TEXT(" ") : TEXT(""), Range);
				if (Range >= 0.999f)
				{
					++ClearProbes;
				}
				else if (Clearance >= 0.0f)
				{
					// 1 - h/400 inverted back to cm: a short range plus a tall height is
					// the "must route around" case, which is the one worth an annotation.
					const float HeightCm = (1.0f - Clearance) * 400.0f;
					Tail += FString::Printf(TEXT("(h%.0f%s)"), HeightCm,
						(HeightCm < 100.0f) ? TEXT("j") : TEXT("b"));
				}
			}
			UE_LOG(LogPursuitAI, Log,
				TEXT("PursuitCharEnv: episode %d obs = [dir=(%.2f, %.2f) dist=%.3f own_spd=%.3f tgt_spd=%.3f] walls=[%s] (%d/%d clear) ndim=%d"),
				EpisodeIndex,
				ProbePoint->Values[0], ProbePoint->Values[1],
				ProbePoint->Values[2], ProbePoint->Values[3],
				ProbePoint->Values[4],
				*Tail, ClearProbes, ProbeCount, NumValues);

			// Dimension invariant, stated as a contract and checked every episode: 2
			// direction + 3 scalar + 5 probe ranges + 5 probe clearances = 15. The ndim
			// is printed above whether or not it is right, so a silent contract change
			// cannot pass as a normal run - it shows up as an Error line next to it.
			if (NumValues != 15)
			{
				UE_LOG(LogPursuitAI, Error,
					TEXT("PursuitCharEnv: TargetSensor emitted %d dims, expected 15 - observation contract broken"),
					NumValues);
			}
		}
	}

	// Re-assert the jump gate on every episode, after SpawnAgents has resolved the cast:
	// a level or an option can flip bEnableAgentJump between episodes, and a stale gate
	// would show up as "the jump was disabled but the dog still hopped".
	SyncJumpGate();

	// Same per-episode re-assertion for the stray spectator: it is spawned by the game
	// mode, possibly after this env's BeginPlay, so clearing it once at startup is not
	// enough. Two cheap calls per episode.
	NeutralizeStraySpectator();

	const FVector ChaserLocation = ChaserAgent ? ChaserAgent->GetActorLocation() : FVector::ZeroVector;
	const FVector EvaderLocation = EvaderAgent ? EvaderAgent->GetActorLocation() : FVector::ZeroVector;
	const float Gap = (ChaserAgent && EvaderAgent)
		? static_cast<float>(FVector::Dist2D(ChaserLocation, EvaderLocation))
		: 0.0f;

	UE_LOG(LogPursuitAI, Log,
		TEXT("PursuitCharEnv: episode %d started  chaser=(%.0f, %.0f, %.0f)  evader=(%.0f, %.0f, %.0f)  gap=%.0f cm%s%s%s"),
		EpisodeIndex,
		ChaserLocation.X, ChaserLocation.Y, ChaserLocation.Z,
		EvaderLocation.X, EvaderLocation.Y, EvaderLocation.Z,
		Gap,
		bStaticTarget ? TEXT("  [static target]") : TEXT(""),
		bSpawnArenaObstacles ? TEXT("") : TEXT("  [flat arena]"),
		bEnableAgentJump ? TEXT("") : TEXT("  [jump disabled]"));

	// Stage 3 layout tag, on its OWN line: the episode-start line above is the one every
	// acceptance parser in tools/ anchors on, and appending a token to it would change that
	// shape. This line carries the geometry that DEFINES the group (pillar spec, offset,
	// spawn separation, spawn yaw), so the analyzer can re-derive the group from the logged
	// chaser position independently of the tag itself - two paths, one answer.
	if (bStage3PillarLayout)
	{
		UE_LOG(LogPursuitAI, Log,
			TEXT("PursuitCharEnv: episode %d STAGE3 layout=%s pillar=(%.0f, %.0f) footprint=%.0f height=%.0f chaser=(%.0f, %.0f) evader=(%.0f, %.0f) chaser_yaw=%.1f separation=%.0f clear_offset=%.0f"),
			EpisodeIndex,
			bStage3BlockedLayout ? TEXT("blocked") : TEXT("clear"),
			GetActorLocation().X, GetActorLocation().Y,
			Stage3PillarFootprintCm, Stage3PillarHeightCm,
			ChaserLocation.X, ChaserLocation.Y, EvaderLocation.X, EvaderLocation.Y,
			ChaserAgent ? ChaserAgent->GetActorRotation().Yaw : 0.0f,
			Stage3SpawnSeparationCm, Stage3ClearLateralOffsetCm);
	}

	// Stage 4A layout tag, on its OWN line for the same reason as the Stage 3 one: the
	// episode-start line is what every acceptance parser in tools/ anchors on, and appending a
	// token to it would change that shape. The line carries the wall spec AND the live jump
	// gate, so the analyzer can re-derive both the group and whether the control switch was
	// actually in force, independently of the tag itself - two paths, one answer.
	if (bStage4WallLayout)
	{
		UE_LOG(LogPursuitAI, Log,
			TEXT("PursuitCharEnv: episode %d STAGE4 layout=%s wall_x=%.0f height=%.0f thickness=%.0f length=%.0f chaser=(%.0f, %.0f) evader=(%.0f, %.0f) chaser_yaw=%.1f separation=%.0f clear_shift=%.0f jump_gate=%s"),
			EpisodeIndex,
			bStage4BlockedLayout ? TEXT("must_jump") : TEXT("no_jump"),
			GetActorLocation().X,
			Stage4WallHeightCm, Stage4WallThicknessCm, Stage4WallLengthCm,
			ChaserLocation.X, ChaserLocation.Y, EvaderLocation.X, EvaderLocation.Y,
			ChaserAgent ? ChaserAgent->GetActorRotation().Yaw : 0.0f,
			Stage4SpawnSeparationCm, Stage4ClearShiftCm,
			bEnableAgentJump ? TEXT("on") : TEXT("off"));

		// Stage 4B jump-cost audit. LOGGING ONLY - no coefficient, branch or gate below is
		// read from this line, and it changes no behaviour any other stage can see.
		//
		// Why it exists: Stage 4B's whole proposal is "the third action dimension has never
		// been scored, so prove the penalties are actually off before spending a rollout on
		// it". A level read-back proves what was BAKED; it cannot prove what the running
		// actor holds, because SetEnvironmentOptions can overwrite either coefficient from
		// the trainer after the map is loaded. Printing the two members here - the same
		// members ScoreStep charges against, read on the same object, once per episode -
		// closes that gap: the log line and the charge have one source.
		//
		// The two derived flags restate ScoreStep's own guards (see the JumpActionPenalty
		// and AirbornePenalty comments there): the action charge needs a non-zero
		// coefficient AND the execution gate, because a charge for an intent that can never
		// become a jump is pure noise in the advantage estimate; the airborne charge only
		// needs a non-zero coefficient.
		UE_LOG(LogPursuitAI, Log,
			TEXT("PursuitCharEnv: episode %d STAGE4 JUMPCOST jump_gate=%s JumpActionPenalty=%.3f AirbornePenalty=%.3f action_charge=%s airborne_charge=%s"),
			EpisodeIndex,
			bEnableAgentJump ? TEXT("on") : TEXT("off"),
			JumpActionPenalty, AirbornePenalty,
			(JumpActionPenalty != 0.0f && bEnableAgentJump) ? TEXT("ACTIVE") : TEXT("off"),
			(AirbornePenalty != 0.0f) ? TEXT("ACTIVE") : TEXT("off"));
	}

	// Velocity history was already cleared before the teleport (see above). Do NOT
	// clear it a second time here: that would wipe the baseline the sanity probe just
	// established, pushing the first truthful target-speed reading two steps out
	// instead of one.

	// Jump cooldowns follow the same rule: a fresh episode starts with a fresh jump,
	// regardless of where in the cooldown clock the previous episode ended.
	if (ChaserAgent && ChaserAgent->JumpActuator)
	{
		ChaserAgent->JumpActuator->ResetCooldown();
		// Snapshot the lifetime jump counter; the validation summary reports the delta,
		// which is the only honest way to show the gate works - jump REQUESTS can stay
		// high while actual jumps stay at zero, and the difference is the whole point.
		JumpCountAtEpisodeStart = ChaserAgent->JumpActuator->GetJumpCount();
	}
	if (EvaderAgent && EvaderAgent->JumpActuator)
	{
		EvaderAgent->JumpActuator->ResetCooldown();
	}

	// --- Stage 0 validation accumulators ---
	// Reset AFTER both agents are placed: start_d is a property of the placed pair, not
	// of the previous episode, and closest_d must START at the spawn gap rather than at
	// 0 - otherwise an episode in which the chaser never closes reads as a flawless
	// 0 cm closest approach.
	ResetEpisodeValidationState();
	EpisodeStartDistance = Gap;
	ClosestDistanceThisEpisode = Gap > 0.0f ? Gap : TNumericLimits<float>::Max();

	// PrevDistance is the REAL opening distance, always.
	//
	// It used to read `bStaticTarget ? 0.0f : <distance>`, i.e. the static-target task -
	// the exact task this curriculum stage is built on - reported a 0 cm opening gap.
	// Measured consequence: the first ScoreStep differenced (0 - d) and charged a large
	// phantom negative shaping term for a step in which nothing had moved yet, and the
	// "START d=0 cm" log line made the start/end metric unusable. A stationary target is
	// still a target at a distance; there is no special case here.
	PrevDistance = (ChaserAgent && EvaderAgent)
		? static_cast<float>(FVector::Dist2D(ChaserAgent->GetActorLocation(), EvaderAgent->GetActorLocation()))
		: 0.0f;

	// 2026-09-22 diagnostic: the end-of-episode line reports the FINAL distance, so
	// without the starting distance the log cannot answer the only question that matters
	// for a run that never catches - is the chaser closing at all, or not moving?
	if (bLogEpisodes && ChaserAgent && EvaderAgent)
	{
		UE_LOG(LogPursuitAI, Log, TEXT("PursuitCharEnv: episode %d START d=%.0f cm"), EpisodeIndex, PrevDistance);
	}

	// Stage 3 observability evidence: once per process, at the end of episode 1. Placed here
	// and not in BeginPlay because only now is the pairing wired - an unwired sensor emits
	// fifteen zeros and would "prove" the opposite of what this run exists to show. The
	// sweep teleports the chaser (and, for one sample, the evader) and puts both back, so
	// the episode itself runs exactly as it would without the flag.
	if (bStage3ProbeSelfTest && !bStage3ProbeSelfTestDone)
	{
		bStage3ProbeSelfTestDone = true;
		RunStage3ProbeSelfTest();
	}

	// Stage 4A observability evidence, same placement and same reasoning as the Stage 3 sweep
	// above: only now is the pairing wired, and a sweep that ran before it would emit fifteen
	// zeros and "prove" the opposite of what this run exists to show.
	if (bStage4ProbeSelfTest && !bStage4ProbeSelfTestDone)
	{
		bStage4ProbeSelfTestDone = true;
		RunStage4WallProbeSelfTest();
	}
}

void APursuitCharEnv::SpawnAgents()
{
	// Uniform on the disc, not on the square: drawing X and Y independently and
	// clamping to the arena would crowd the corners of the *inscribed* square. Radius
	// scaled by sqrt(u) is the textbook area-uniform draw, same argument as v1's sphere
	// sampling but in 2D.
	// Non-const: the city-stage path gate below redraws the whole pair when a facade
	// blocks the straight chase line between two otherwise-clear spawn columns.
	FVector2D ChaserPoint = RandomSpawnPoint();
	FVector2D EvaderPoint = ChaserPoint;

	if (bStage4WallLayout)
	{
		// Stage 4A constructs its pair for exactly the Stage 3 reason: "the wall is on the
		// chase line" and "it is not" cannot be produced by an area-uniform draw ON PURPOSE,
		// and a draw that stumbled into the must-jump case a few percent of the time would
		// leave that group too small for its number to mean anything.
		PlaceStage4SpawnPair(ChaserPoint, EvaderPoint);
	}
	else if (bStage3PillarLayout)
	{
		// Stage 3 constructs the pair instead of drawing it (see PlaceStage3SpawnPair):
		// "blocked" and "clear" are the two things this stage compares, and a draw cannot
		// produce either of them on purpose. Everything downstream - the facings, the
		// separation log line, the catch rules - is the same code path as every other stage.
		PlaceStage3SpawnPair(ChaserPoint, EvaderPoint);
	}
	else if (EvaderAgent)
	{
		// The evader always gets its own draw - including in Static mode, where "static"
		// means it does not FLEE, never that it shares the chaser's spawn (a shared spawn
		// reads downstream as "caught in one step", which is exactly what it was).
		//
		// REWRITTEN 2026-09-22 (v3.3_capfix). The previous version drew BOTH points
		// independently over the whole spawn disc and rejected pairs that missed
		// [RequiredSeparation, SeparationCap]. Measured on the 500 cm city plaza that
		// loop almost never succeeded, and its 20-attempt fallback then placed the
		// evader at -Direction * FallbackRadius, i.e. on the far RIM of the disc:
		//   spawn gap : min 165 / mean 336 / max 994 cm   (cap was set to 250)
		// A chaser drawn near the rim therefore still saw ~994 cm openings, the
		// separation cap was inert, and each episode burned its scarce 6 s on a chase
		// the policy had no hope of closing - which is why CAUGHT stayed at 0.
		//
		// Root cause is geometric, not a bad constant: with both points drawn
		// area-uniform over a 500 cm disc, P(separation <= 250) is only ~12%, and the
		// WorldStatic path gate on the city stage rejects most of what survives (the
		// plaza is ringed by facades). Rejection sampling against a ~12% acceptance
		// band was never going to work.
		//
		// Fix: CONSTRUCT the pair instead of rejecting draws. Pick the chaser, then
		// place the evader at a uniformly drawn distance inside the band and a
		// uniformly drawn bearing - every candidate satisfies the separation contract
		// by construction, so the loop only has to solve the path gate. The final
		// fallback keeps the band too, which is what the old code got wrong.
		const float RequiredSeparation = bCityStage
			? FMath::Min(MinSpawnSeparation, CityStageRadius * 0.8f)
			: MinSpawnSeparation;
		// Upper bound too: a pair drawn near-antipodal on the plaza puts the catch out of
		// reach of a policy that still has to discover pursuit (see MaxSpawnSeparation).
		// The cap can never be tighter than the floor, or every draw would be rejected.
		const float SeparationCap = (MaxSpawnSeparation > 0.0f)
			? FMath::Max(MaxSpawnSeparation, RequiredSeparation + 1.0f)
			: TNumericLimits<float>::Max();
		// The band has to fit inside the disc or no constructed pair can satisfy it.
		// MaxSpawnRadius is the same expression RandomSpawnPoint draws within, so the
		// evader lands on a point that function could itself have produced.
		const float MaxSpawnRadius = bCityStage
			? FMath::Min(ArenaRadius * SpawnMarginFraction, CityStageRadius)
			: ArenaRadius * SpawnMarginFraction;
		const float EffectiveCap = FMath::Min(SeparationCap, 2.0f * MaxSpawnRadius);
		const float EffectiveFloor = FMath::Min(RequiredSeparation, EffectiveCap);

		constexpr int32 MaxAttempts = 32;
		bool bPlaced = false;
		for (int32 Attempt = 0; Attempt < MaxAttempts; ++Attempt)
		{
			// Chaser first. On the city stage it needs the same column probe the old
			// loop applied, so it is redrawn here rather than only on attempt 0.
			if (bCityStage)
			{
				ChaserPoint = RandomSpawnPoint();
			}
			// Uniform in the annulus [floor, cap] - sqrt keeps it area-uniform, the
			// same argument RandomSpawnPoint uses for the disc itself.
			const float Separation = FMath::Lerp(
				EffectiveFloor, EffectiveCap, FMath::Sqrt(Rng.FRand()));
			const float Bearing = Rng.FRandRange(0.0f, 2.0f * UE_PI);
			EvaderPoint = ChaserPoint + FVector2D(
				FMath::Cos(Bearing) * Separation, FMath::Sin(Bearing) * Separation);

			// The constructed point must still be somewhere the spawn rules allow:
			// inside the spawn disc, outside every rig obstacle, and with a clear
			// column. Only the path gate can fail often - the rest are one-shot checks.
			if (FVector2D::Distance(EvaderPoint, FVector2D::ZeroVector) > MaxSpawnRadius)
			{
				continue;
			}
			bool bEvaderClear = true;
			for (const FPursuitObstacle& Disc : RigObstacleDiscs)
			{
				if (FVector2D::Distance(EvaderPoint, Disc.Centre) < Disc.ClearRadius)
				{
					bEvaderClear = false;
					break;
				}
			}
			if (bEvaderClear && bCityStage && !SpawnPointColumnClear(EvaderPoint))
			{
				bEvaderClear = false;
			}
			if (!bEvaderClear)
			{
				continue;
			}
			if (!bCityStage || SpawnPairPathClear(ChaserPoint, EvaderPoint))
			{
				bPlaced = true;
				break;
			}
		}

		if (!bPlaced)
		{
			// Deterministic last resort that STILL honours the contract: put the evader
			// at the cap along the outward bearing from the chaser, clamped so the pair
			// stays inside the disc. The old code used the full disc radius here, which
			// is the bug this rewrite exists to remove - a fallback that violates the
			// cap is worse than no cap, because the log then looks like the cap worked.
			FVector2D Outward = ChaserPoint.GetSafeNormal();
			if (Outward.IsNearlyZero())
			{
				const float AnyAngle = Rng.FRandRange(0.0f, 2.0f * UE_PI);
				Outward = FVector2D(FMath::Cos(AnyAngle), FMath::Sin(AnyAngle));
			}
			float Separation = FMath::Min(EffectiveCap, 2.0f * MaxSpawnRadius - ChaserPoint.Size());
			Separation = FMath::Max(Separation, FMath::Min(EffectiveFloor, MaxSpawnRadius));
			EvaderPoint = ChaserPoint + Outward * Separation;
			UE_LOG(LogPursuitAI, Warning,
				TEXT("PursuitCharEnv: spawn pair fell back on attempt %d - gap=%.0f cm (band %.0f-%.0f, disc %.0f)"),
				MaxAttempts, Separation, EffectiveFloor, EffectiveCap, MaxSpawnRadius);
		}
	}

	const float SpawnZ = GetActorLocation().Z + SpawnHeight;
	const FVector ChaserLocation(ChaserPoint.X + GetActorLocation().X, ChaserPoint.Y + GetActorLocation().Y, SpawnZ);
	const FVector EvaderLocation(EvaderPoint.X + GetActorLocation().X, EvaderPoint.Y + GetActorLocation().Y, SpawnZ);

	// Per-episode spawn FACINGS, drawn independently for the two scored agents.
	//
	// Both used to be "look at the other one", which made the chaser's direction
	// observation a constant (1.00, 0.00) and left the policy nothing to steer by
	// (see PlaceAgent). Independent facings mean dims 0-1 now encode where the target
	// really is relative to the agent's own body, from the very first step - which is
	// the only way a turn-toward-target behaviour can ever be reinforced.
	//
	// Drawn from the episode Rng (not DriveRng) so the layout, including facings,
	// stays reproducible for a given seed + episode index.
	// Stage 4A shares this field rather than adding a second one: the subject of that stage is
	// the WALL, so "where is the target" must not become a variable under test, and a drawn
	// facing would make it one.
	const float ChaserFaceYaw = (bStage3PillarLayout || bStage4WallLayout)
		? Stage3SpawnYawDegrees
		: Rng.FRandRange(0.0f, 360.0f);
	const float EvaderFaceYaw = (bStage3PillarLayout || bStage4WallLayout)
		? Stage3SpawnYawDegrees + 180.0f
		: Rng.FRandRange(0.0f, 360.0f);

	if (ChaserAgent)
	{
		PlaceAgent(ChaserAgent, ChaserLocation, ChaserFaceYaw);

		// Cast the rig. Loading every episode is cheap after the first (asset cache hits),
		// and without it the agent is an invisible capsule that still chases - which from
		// the top-down camera reads as "the watch window is frozen" (2026-09-21 report).
		// Role/look pairing (reversed 2026-09-21 at the user's request): the DOG is the
		// chaser, TinyHero is the one being chased.
		ChaserAgent->HeroModel = EPursuitHeroModel::AnimalHero;
		ChaserAgent->LoadVisuals();

		// Re-assert speeds every episode so a blueprint or option tweak cannot leave the
		// cast with yesterday's numbers.
		ChaserAgent->SetMaxWalkSpeed(ChaserAgent->GetChaseMaxSpeed());

		// THE pairing wire: the chaser's sensor chases the evader. Found missing during
		// the blind-policy investigation (2026-09-21): TargetActor was never assigned
		// anywhere, so CollectObservations always took the !Target early-out and emitted
		// five zeros - the policy trained blind through three 500k runs. Set it here,
		// every episode, after placement.
		if (ChaserAgent->TargetSensor && EvaderAgent)
		{
			ChaserAgent->TargetSensor->TargetActor = EvaderAgent;
		}
	}
	if (EvaderAgent)
	{
		PlaceAgent(EvaderAgent, EvaderLocation, EvaderFaceYaw);

		// The evader wears TinyHero: the pack's humanoid hero, now the one being chased
		// (role/look pairing reversed 2026-09-21; see the chaser side above). A different
		// silhouette from the dog keeps the two tellable apart at a glance.
		EvaderAgent->HeroModel = EPursuitHeroModel::TinyHero;
		EvaderAgent->LoadVisuals();

		EvaderAgent->SetMaxWalkSpeed(ChaserAgent ? ChaserAgent->GetChaseMaxSpeed() * EvaderSpeedRatio : 510.0f);

		// Moving-target census snapshot. Taken here, at the one place the evader is
		// PLACED, rather than in ResetEpisodeValidationState: that reset runs later in
		// BeginEpisode (deliberately, so start_d describes the placed pair), so a
		// snapshot stored there would also be wiped by any path that does not re-spawn.
		// One writer, one meaning. In the static task nothing below ever advances these.
		EvaderEpisodeStartLocation = EvaderAgent->GetActorLocation();
		EvaderPrevLocation = EvaderEpisodeStartLocation;
		EvaderPathLengthCm = 0.0f;
		EvaderSpeedSumCmPerSec = 0.0;
		EvaderMaxSpeedCmPerSec = 0.0f;

		// The evader's sensor mirrors the chase: it sees the chaser (used when the
		// evader ever becomes a learned agent; harmless rule-AI overhead today).
		if (EvaderAgent->TargetSensor && ChaserAgent)
		{
			EvaderAgent->TargetSensor->TargetActor = ChaserAgent;
		}
	}
	if (SupportAgent)
	{
		// The watched run's second chaser, spawned beside the dog on a perpendicular
		// offset so the pair converges from two angles instead of single-file. Re-drawn
		// with the same spawn placement every episode (PlaceAgent teleports it back
		// after a catch scatter). Purely cosmetic: never scored, never observed - the
		// catch test stays dog-vs-hero, so the training contract is untouched.
		const FVector2D PairAxis = (EvaderPoint - ChaserPoint).GetSafeNormal();
		const FVector2D SupportPoint = ChaserPoint
			+ (PairAxis.IsNearlyZero() ? FVector2D(0.0f, 1.0f) : PairAxis.GetRotated(90.0f)) * 250.0f;
		const FVector SupportLocation(
			SupportPoint.X + GetActorLocation().X,
			SupportPoint.Y + GetActorLocation().Y,
			SpawnZ);
		// The soldier is cosmetic and unobserved, so it keeps the "face the prey" spawn -
		// reads better in the watch window and cannot affect the learning signal.
		const float SupportFaceYaw = EvaderAgent
			? (EvaderAgent->GetActorLocation() - SupportLocation).Rotation().Yaw
			: 0.0f;
		PlaceAgent(SupportAgent, SupportLocation, SupportFaceYaw);

		// The soldier wears RPGHero - the pack's armoured humanoid, visually distinct
		// from both the dog (AnimalHero) and the hero (TinyHero) at watch distance.
		SupportAgent->HeroModel = EPursuitHeroModel::RPGHero;
		SupportAgent->LoadVisuals();
		SupportAgent->SetMaxWalkSpeed(ChaserAgent ? ChaserAgent->GetChaseMaxSpeed() : 510.0f);

		// Ignore pawns: two chasers converging on one hero otherwise jam capsule-to-
		// capsule - the soldier body-blocking the dog (or pinning the hero against a
		// wall) reads downstream as "the dog grinds in place". World collisions stay
		// on, so the soldier still climbs nothing and still respects the facades.
		if (UPrimitiveComponent* SolRoot = Cast<UPrimitiveComponent>(SupportAgent->GetRootComponent()))
		{
			SolRoot->SetCollisionResponseToChannel(ECC_Pawn, ECR_Ignore);
		}
	}
}

FVector2D APursuitCharEnv::RandomSpawnPoint()
{
	// Uniform on the disc, not on the square: drawing X and Y independently and
	// clamping to the arena would crowd the corners of the *inscribed* square. Radius
	// scaled by sqrt(u) is the textbook area-uniform draw, same argument as v1's sphere
	// sampling but in 2D. The obstacle layer adds one rejection pass: a point inside an
	// obstacle's clearance disc would materialise the capsule inside geometry, which
	// reads downstream as "the policy is stuck at spawn".
	FVector2D Point = FVector2D::ZeroVector;
	for (int32 Attempt = 0; Attempt < 20; ++Attempt)
	{
		// City stage: the rig's 1200 draw radius assumes walls that contain the chase.
		// Without them a street-side spawn column can be perfectly clear (the probe
		// below passes) while the chase itself is walled in by facades - cap the draw
		// to the measured clean plaza instead.
		const float MaxSpawnRadius = bCityStage
			? FMath::Min(ArenaRadius * SpawnMarginFraction, CityStageRadius)
			: ArenaRadius * SpawnMarginFraction;
		const float Angle = Rng.FRandRange(0.0f, 2.0f * UE_PI);
		const float Radius = MaxSpawnRadius * FMath::Sqrt(Rng.FRand());
		Point = FVector2D(FMath::Cos(Angle) * Radius, FMath::Sin(Angle) * Radius);

		bool bInsideObstacle = false;
		for (const FPursuitObstacle& Disc : RigObstacleDiscs)
		{
			if (FVector2D::Distance(Point, Disc.Centre) < Disc.ClearRadius)
			{
				bInsideObstacle = true;
				break;
			}
		}

		if (!bInsideObstacle)
		{
			// City stage: the map's own geometry is not in RigObstacleDiscs, so probe
			// the spawn column instead (checked by the helper below, shared with the
			// constructed-pair path in SpawnAgents).
			if (!bCityStage || SpawnPointColumnClear(Point))
			{
				return Point;
			}
		}
	}

	// Twenty collisions in a row is not going to happen with this layout; the last draw
	// is still the honest answer (better than asserting inside a reward loop).
	return Point;
}

bool APursuitCharEnv::SpawnPointColumnClear(const FVector2D& Point) const
{
	const UWorld* World = GetWorld();
	if (!World)
	{
		return true;
	}

	// Probe the spawn column: any static hit between spawn height and just under the
	// arena plane (a lamp post, a bench, a planter) rejects the draw.
	//
	// The lower end of the trace is the part that matters and it CHANGED on 2026-09-22.
	// It used to run to (arena z - 30), i.e. 30 cm below the arena plane, on the reasoning
	// that "the road itself is at z=10, so the trace ends above the asphalt and the road
	// never counts". That is true for the city's own road - but once the stage has a rig
	// floor at the arena plane (bCityRigFloor, added for the same v3.10 investigation),
	// the trace CROSSES that floor and every single draw is rejected, so FindSpawnPoint
	// burns all its attempts and falls back to the last sample. Stopping the trace at
	// (arena z + 20) keeps it above both the rig floor and the asphalt while still
	// catching anything standing in the column.
	const FVector ColumnTop(
		GetActorLocation().X + Point.X,
		GetActorLocation().Y + Point.Y,
		GetActorLocation().Z + 300.0f);
	FHitResult ColumnHit;
	FCollisionQueryParams ColumnParams(SCENE_QUERY_STAT(PursuitSpawnClearance), false);
	ColumnParams.AddIgnoredActor(ChaserAgent);
	ColumnParams.AddIgnoredActor(EvaderAgent);
	return !World->LineTraceSingleByChannel(ColumnHit, ColumnTop,
		ColumnTop - FVector(0.0f, 0.0f, 280.0f), ECC_WorldStatic, ColumnParams);
}

bool APursuitCharEnv::SpawnPairPathClear(const FVector2D& A, const FVector2D& B) const
{
	const UWorld* World = GetWorld();
	if (!World)
	{
		return true;
	}

	// One straight WorldStatic trace at torso height. The probe plane sits 60 cm above
	// the arena plane: on the city stage that is ~120 cm over the asphalt - below the
	// building facades that block a chase, above the kerbs the capsule steps over.
	const float ProbeZ = GetActorLocation().Z + 60.0f;
	const FVector Start(GetActorLocation().X + A.X, GetActorLocation().Y + A.Y, ProbeZ);
	const FVector End(GetActorLocation().X + B.X, GetActorLocation().Y + B.Y, ProbeZ);

	FHitResult Hit;
	FCollisionQueryParams Params(SCENE_QUERY_STAT(PursuitSpawnPath), false);
	Params.AddIgnoredActor(ChaserAgent);
	Params.AddIgnoredActor(EvaderAgent);
	return !World->LineTraceSingleByChannel(Hit, Start, End, ECC_WorldStatic, Params);
}

void APursuitCharEnv::PlaceAgent(APursuitCharAgent* Agent, const FVector& Location, float YawDegrees)
{
	if (!Agent)
	{
		return;
	}

	// Hard physics teleport + velocity zeroing: a soft move would preserve the capsule's
	// velocity across the reset and the first observation of the new episode would be a
	// motion smear of the last one (the official Tag environment's bStopAgentMovementOnReset).
	//
	// The yaw is now CALLER-SUPPLIED instead of derived from the target direction. The old
	// `(FaceToward - Location).Rotation().Yaw` pointed the chaser straight at the evader at
	// step 1 of every episode, which zeroed the information in observation dims 0-1: the
	// local-frame direction read (1.00, 0.00) in 45/45 measured episodes regardless of the
	// true bearing. Spawn faces are drawn per-episode by the caller so "the target is 40 deg
	// to my right" is a state the policy can actually see and act on.
	const FRotator Yaw(0.0f, YawDegrees, 0.0f);
	Agent->SetActorLocationAndRotation(Location, Yaw, false, nullptr, ETeleportType::TeleportPhysics);
	StopAgent(Agent);
}

void APursuitCharEnv::StopAgent(APursuitCharAgent* Agent)
{
	if (!Agent)
	{
		return;
	}

	if (UCharacterMovementComponent* Movement = Agent->GetCharacterMovement())
	{
		Movement->StopMovementImmediately();
		if (Movement->MovementMode == MOVE_None)
		{
			Movement->SetMovementMode(MOVE_Walking);
		}
	}
}

void APursuitCharEnv::Reset_Implementation(FInitialAgentState& OutAgentState)
{
	BeginEpisode();

	// IAgent's Observe hands over an untyped FInstancedStruct while the training state
	// carries a typed TInstancedStruct<FPoint>. They are layout-compatible by design;
	// ToTypedInstancedStruct is the sanctioned reinterpret between them (v1 does the same).
	if (ChaserAgent)
	{
		FInstancedStruct UnTypedObservation;
		IAgent::Execute_Observe(ChaserAgent, UnTypedObservation);
		OutAgentState.Observations = ToTypedInstancedStruct<FPoint>(UnTypedObservation);
	}

	OutAgentState.Info.Add(TEXT("episode"), FString::FromInt(EpisodeIndex));
}

void APursuitCharEnv::Step_Implementation(const FInstancedStruct& InAction, FAgentState& OutAgentState)
{
	// Wall hits from the movement this action is about to cause are attributed to the
	// NEXT step's reward (movement integrates after this call returns), so the flag
	// carries over rather than being cleared here - it is cleared on the step that reads
	// it, right after scoring.
	const bool bWallHit = bWallHitThisStep;
	bWallHitThisStep = false;

	// NOTE (Stage 3B): the contact-duration counters are deliberately NOT advanced here.
	// This function is the TRAINING/gRPC path only - a watched run (`-game`, which is how
	// every scripted baseline and the probe self-test are driven) never calls it, and the
	// first version of this metric lived here and therefore read a hard zero on a run whose
	// chaser spent the entire episode grinding into the pillar. It now lives in
	// AccumulateValidationSample, the one per-step hook BOTH paths share, so the step count
	// and `samples` are on the same boundary by construction.
	//
	// `bWallHitThisStep` stays exactly as it was: it is the REWARD's flag, and clearing it
	// anywhere other than this line would stop the wall-hit penalty from ever being charged.

	++CurrentStep;

	// Read the jump INTENT out of the incoming action before it is executed. This has
	// to happen here, not in ScoreStep: ScoreStep only sees the world afterwards, and
	// the world cannot distinguish "asked to jump" from "was already airborne".
	// Measured (v3.7): the state-based AirbornePenalty was charged on ~every step yet
	// the jump count did not move, because a perpetual hopper pays it either way.
	bJumpRequestedThisStep = false;
	if (ChaserAgent && ChaserAgent->JumpActuator)
	{
		const FDictPoint* ActionDict = InAction.GetPtr<FDictPoint>();
		const TInstancedStruct<FPoint>* JumpEntry =
			ActionDict ? ActionDict->Points.Find(TEXT("JumpInput")) : nullptr;
		const FBoxPoint* JumpBox = JumpEntry ? JumpEntry->GetPtr<FBoxPoint>() : nullptr;
		if (JumpBox && JumpBox->Values.Num() >= 1)
		{
			bJumpRequestedThisStep = JumpBox->Values[0] >= ChaserAgent->JumpActuator->JumpThreshold;
			bJumpIntentResolved = true;
		}
		else if (const FBoxPoint* FlatAction = InAction.GetPtr<FBoxPoint>())
		{
			// Flat 3-dim action (SB3 merges the dict): move-x, move-y, jump. This is the
			// shape the trainer actually sends - confirmed by the ACTION PROBE below,
			// which prints the struct name on the first steps of every run.
			if (FlatAction->Values.Num() >= 3)
			{
				bJumpRequestedThisStep =
					FlatAction->Values[2] >= ChaserAgent->JumpActuator->JumpThreshold;
				bJumpIntentResolved = true;
			}
		}

		// ACTION PROBE. jump_stats.py showed 11.7 jumps per episode AFTER JumpActionPenalty
		// was wired in, which is only possible if the penalty is never charged - and the
		// only way that happens with this code is the two shapes above both missing. Two
		// lines per run, on steps 1 and 2, are enough to say which struct arrives and
		// whether its jump element clears the threshold; without it the whole actuator
		// penalty question stays a guess.
		if (!bActionProbeDone && CurrentStep <= 2)
		{
			const UScriptStruct* ActionStruct = InAction.GetScriptStruct();
			const FBoxPoint* ProbeFlat = InAction.GetPtr<FBoxPoint>();
			FString ShapeText = ActionStruct ? ActionStruct->GetName() : TEXT("null");
			if (ProbeFlat)
			{
				ShapeText += FString::Printf(TEXT(" [%d values:"), ProbeFlat->Values.Num());
				for (int32 Index = 0; Index < ProbeFlat->Values.Num(); ++Index)
				{
					ShapeText += FString::Printf(TEXT(" %.3f"), ProbeFlat->Values[Index]);
				}
				ShapeText += TEXT("]");
			}
			else if (ActionDict)
			{
				ShapeText += FString::Printf(TEXT(" [%d keys:"), ActionDict->Points.Num());
				for (const TPair<FName, TInstancedStruct<FPoint>>& Pair : ActionDict->Points)
				{
					ShapeText += TEXT(" ") + Pair.Key.ToString();
				}
				ShapeText += TEXT("]");
			}
			const int32 ResolvedFlag = bJumpIntentResolved ? 1 : 0;
			const int32 RequestedFlag = bJumpRequestedThisStep ? 1 : 0;
			UE_LOG(LogPursuitAI, Log,
				TEXT("PursuitCharEnv: ACTION PROBE step %d shape=%s resolved=%d jump_requested=%d threshold=%.2f"),
				CurrentStep, *ShapeText, ResolvedFlag, RequestedFlag,
				ChaserAgent->JumpActuator->JumpThreshold);
			if (CurrentStep >= 2)
			{
				bActionProbeDone = true;
			}
		}
	}

	if (ChaserAgent)
	{
		IAgent::Execute_Act(ChaserAgent, InAction);
	}

	// The evader moves after the chaser and before the distance is read, in that order,
	// so this step scores the same world state an inference run would score (v1's
	// StepTarget ordering, preserved).
	AdvanceScriptedDrivers(0.0f);

	const float NewDistance = (ChaserAgent && EvaderAgent)
		? static_cast<float>(FVector::Dist2D(ChaserAgent->GetActorLocation(), EvaderAgent->GetActorLocation()))
		: TNumericLimits<float>::Max();

	const float DeltaZ = (ChaserAgent && EvaderAgent)
		? FMath::Abs(ChaserAgent->GetActorLocation().Z - EvaderAgent->GetActorLocation().Z)
		: TNumericLimits<float>::Max();

	const bool bNewCatch = NewDistance <= CatchRadius && DeltaZ <= CatchHeightTolerance;

	// Time wall in simulated seconds (see EpisodeSeconds comment): step counts are NOT
	// a duration for dt-driven movement. NOTE: MaxSteps is deliberately NOT a timeout
	// here - at the trainer's ~430 fps a 2000-step cap fires at 4.6 sim-seconds, long
	// before the time wall, and re-creates the "catch unreachable" trap it was meant to
	// guard against (measured: the v2 retrain still timed out at exactly 2000 steps).
	const float Dt = GetWorld() ? GetWorld()->GetDeltaSeconds() : (1.0f / 60.0f);
	EpisodeSimTime += Dt;
	const bool bOutOfTime = !bNewCatch && EpisodeSimTime >= EpisodeSeconds;

	CurrentReward = ScoreStep(NewDistance, bNewCatch, bOutOfTime, bWallHit, Dt);
	TotalReward += CurrentReward;
	PrevDistance = NewDistance;

	// Stage 0 metrics, sampled on the same world state the reward was scored against.
	// Deliberately NOT derived from the reward: "the chaser closed the gap" and "the
	// chaser is running at the target" are different claims, and a reward curve cannot
	// tell them apart - which is why the acceptance run is not judged on TensorBoard.
	AccumulateValidationSample(NewDistance);

	if (ChaserAgent)
	{
		FInstancedStruct UnTypedObservation;
		IAgent::Execute_Observe(ChaserAgent, UnTypedObservation);
		OutAgentState.Observations = ToTypedInstancedStruct<FPoint>(UnTypedObservation);

		// 2026-09-22 diagnostic: dump what the policy actually sees, a few times per
		// episode. Two 500k runs ended with the pair FURTHER apart than they spawned,
		// which is what a blind policy looks like - and this project has that bug before
		// (TargetActor never assigned: the policy trained on five zeros for three runs).
		// Printing the tensor settles "blind" vs "not learning" in one run instead of one
		// more 20-minute guess.
		if (bLogEpisodes && (CurrentStep % 2000 == 1 || CurrentStep % 120 == 1))
		{
			// Mid-episode observation trace. The per-episode probe in BeginEpisode only
			// ever shows STEP 1 (before either agent has moved), which is why three runs
			// of "the direction channel looks fine at spawn" hid a policy that steered
			// nowhere. This dump fires ~2x per second inside the episode instead, and
			// pairs the tensor with the RAW geometry so the two can be compared:
			//   obs dir      = what the policy is told (target in the owner's local frame)
			//   owner yaw    = which way the body is actually facing
			//   vel yaw      = which way it is actually moving
			// A policy that has learned to chase shows obs dir approaching (1, 0) while
			// vel yaw approaches the bearing to the target. A policy that has learned
			// nothing shows obs dir and vel yaw drifting together at a fixed angle to
			// the target - the signature of "always run at full stick, never steer".
			// ONE unpacker for the sensor reading (see ResolveTargetSensorPoint): the
			// same three-line unwrap written out three times is how the wall-probe
			// counter ended up asking a Dict for a Box and reporting "0 walls" forever.
			const FBoxPoint* SensorBox = ResolveTargetSensorPoint(UnTypedObservation);
			if (SensorBox)
			{
				FString Dump;
				for (int32 Index = 0; Index < SensorBox->Values.Num(); ++Index)
				{
					Dump += FString::Printf(TEXT("%s%.2f"), Index > 0 ? TEXT(", ") : TEXT(""), SensorBox->Values[Index]);
				}
				UE_LOG(LogPursuitAI, Log, TEXT("PursuitCharEnv: episode %d step %d OBS[%d] = %s"),
					EpisodeIndex, CurrentStep, SensorBox->Values.Num(), *Dump);
			}
			// The agent composes sensors into a Dict; the flattened 15-dim Box the
			// trainer sees comes from the sensor that owns dims 0..14. Print the
			// direction pair plus the geometry it should be derived from.
			else if (SensorBox->Values.Num() >= 3 && ChaserAgent && EvaderAgent)
			{
					const FVector ChaserLoc = ChaserAgent->GetActorLocation();
					const FVector EvaderLoc = EvaderAgent->GetActorLocation();
					const FVector ToTarget = (EvaderLoc - ChaserLoc).GetSafeNormal2D();
					const FVector Facing = ChaserAgent->GetActorForwardVector().GetSafeNormal2D();
					const FVector Velocity = ChaserAgent->GetVelocity().GetSafeNormal2D();
					const UCharacterMovementComponent* ChaserMovement = ChaserAgent->GetCharacterMovement();
					const int32 bGrounded = (ChaserMovement && ChaserMovement->IsMovingOnGround()) ? 1 : 0;
					const int32 ChaserZ = FMath::RoundToInt(ChaserAgent->GetActorLocation().Z);
					const int32 EvaderZ = FMath::RoundToInt(EvaderAgent->GetActorLocation().Z);

					UE_LOG(LogPursuitAI, Log,
						TEXT("PursuitCharEnv: episode %d step %d TRACE obs_dir=(%.2f, %.2f) dist=%.3f ")
						TEXT("| owner_yaw=%.0f facing_toward_target=%.0f vel_yaw=%.0f bearing_to_target=%.0f ")
						TEXT("| dot(facing,target)=%.2f dot(vel,target)=%.2f spd=%.0f grounded=%d ")
						TEXT("| z=%d vs evader_z=%d vs ground_z=%d"),
						EpisodeIndex, CurrentStep,
						SensorBox->Values[0], SensorBox->Values[1], SensorBox->Values[2],
						ChaserAgent->GetActorRotation().Yaw,
						FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(
							static_cast<double>(FVector::DotProduct(Facing, ToTarget)), -1.0, 1.0))),
						Velocity.IsNearlyZero() ? -999.0f : ChaserAgent->GetVelocity().Rotation().Yaw,
						FMath::RadiansToDegrees(FMath::Atan2(ToTarget.Y, ToTarget.X)),
						FVector::DotProduct(Facing, ToTarget),
						FVector::DotProduct(Velocity, ToTarget),
						ChaserAgent->GetVelocity().Size2D(),
						bGrounded, ChaserZ, EvaderZ, FMath::RoundToInt(GetActorLocation().Z));
			}
		}

		// Wall-probe liveness. The tally itself lives in TallyWallProbes so that the
		// watched path - which is what Stage 0 is actually decided on, since Greedy and
		// Random are watched runs - measures the SAME thing through the SAME unpacker.
		//
		// This block used to carry its own copy of both, and its copy of the unwrap was
		// the broken one: it asked the top-level observation for a BoxPoint while
		// APursuitCharAgent hands over an FDictPoint keyed by component name. The cast
		// failed on every step of every run, so the counter reported 0 walls forever -
		// and 0 is ALSO what a correct counter reports on an empty stage, which is what
		// made the false negative invisible for a whole round.
		TallyWallProbes(ResolveTargetSensorPoint(UnTypedObservation));
	}

	OutAgentState.Reward = CurrentReward;
	OutAgentState.bTerminated = bNewCatch;
	OutAgentState.bTruncated = bOutOfTime;

	OutAgentState.Info.Add(TEXT("step"), FString::FromInt(CurrentStep));
	OutAgentState.Info.Add(TEXT("distance"), FString::SanitizeFloat(NewDistance));
	OutAgentState.Info.Add(TEXT("total_reward"), FString::SanitizeFloat(TotalReward));

	if (bNewCatch)
	{
		bCaught = true;
	}

	if (bNewCatch || bOutOfTime)
	{
		// The Stage 0 line first, and NOT behind the bLogEpisodes gate: it is the
		// measurement the acceptance report reads, one line per completed episode.
		LogEpisodeValidationSummary(bNewCatch ? TEXT("CAUGHT") : TEXT("TIMEOUT"), NewDistance);

		if (bLogEpisodes)
		{
			FinishEpisodeStats(bNewCatch);
			UE_LOG(LogPursuitAI, Log,
				TEXT("PursuitCharEnv: episode %d %s in %d steps (%.1f sim-s), d_end=%.0f cm, reward %.3f (total %.3f), wall_hits=%d"),
				EpisodeIndex, bNewCatch ? TEXT("CAUGHT") : TEXT("TIMEOUT"), CurrentStep, EpisodeSimTime, NewDistance, CurrentReward, TotalReward, WallHitCount);
			// Wall-probe liveness: "0 probes" over a whole run means the dog never sees a
			// facade inside its 800 cm cone (range/stage problem, not a learning problem).
			// blocked_readings counts the tall-obstacle subset that REQUIRES routing.
			UE_LOG(LogPursuitAI, Log,
				TEXT("PursuitCharEnv: episode %d probe_summary non_clear_readings=%d blocked_readings=%d closest=%.3f (%d steps sampled)"),
				EpisodeIndex, WallProbeHitsThisEpisode, HopBlockedReadingsThisEpisode,
				ClosestWallProbeThisEpisode, CurrentStep);
		}

		// Control flow, not diagnostics - outside the logging gate on purpose, so a run
		// with bLogEpisodes=false still honours -PursuitCharMaxEpisodes.
		NoteEpisodeCompleted(bNewCatch);
	}
}

float APursuitCharEnv::ScoreStep(float NewDistance, bool bNewCatch, bool bOutOfTime, bool bWallHit, float Dt)
{
	// ---------------------------------------------------------------------------
	// Potential-based distance shaping. EXACT form, and why (2026-09-22, third fix).
	//
	// The previous form was `DistanceShapingScale * (PrevDistance - NewDistance)` - a
	// raw per-cm reward. Measured consequence over the 400k run: the dog did not
	// pursue at all, it INCREASED the gap (spawn 400-660 cm -> d_end median 1300 cm)
	// and the run ended 0 catches in 24 episodes, with reward/step sitting at ~-0.004
	// against a -0.005 existence penalty, i.e. net closure exactly zero.
	//
	// That is not a policy failure, it is the reward telling the truth about the wrong
	// objective. Raw per-cm shaping is not just a shaping term, it is a PAYMENT FOR
	// MOTION: with 1000 cm of escape available it pays 0.02*1000 = +20, which is TWICE
	// CatchReward (10), and unlike the catch it can be collected every single step and
	// repeated every episode. Add "timing out costs nothing but a small existence
	// penalty" and running away is the strict optimum. (The action-space fix a few
	// hours earlier removed the crude "hold reverse" basin, but this term re-created
	// it in a subtler form: not walking backwards, just circling outwards.)
	//
	// The fix is the standard potential-based shaping, already used by v1's own
	// ProbePath convention: reward the CHANGE IN POTENTIAL, where potential is
	// -distance. The difference matters because this form is a telescoping sum - over
	// an episode it pays (start - end) * scale, bounded by the spawn separation and
	// INDEPENDENT of how much the agent moves. Escaping 1000 cm now pays +20 of
	// TRANSFER against the -20 you gave up walking out there: net zero. Only actually
	// finishing the chase pays, which is what CatchReward is for.
	//
	// Telescoping also means the shaping cannot be farmed by oscillating: any step
	// that closes d pays +scale*d, and the step that opens it back up pays -scale*d.
	float Reward = 0.0f;

	const bool bValidDistance = FMath::IsFinite(NewDistance) && FMath::IsFinite(PrevDistance);
	if (bValidDistance)
	{
		Reward += DistanceShapingScale * (PrevDistance - NewDistance);
	}

	// Per-step penalties normalized to 60 fps: dt*60 = 1 at 60 fps (legacy semantics),
	// proportionally smaller at the trainer's high frame rates, so the penalty RATE per
	// simulated second - the thing the policy actually experiences - is fps-invariant.
	const float StepScale = Dt * 60.0f;
	Reward += StepPenalty * StepScale;

	// Proximity shaping (see the header note): only the FINAL approach needs it, and it
	// is deliberately NOT potential-based - it pays for being close, every step, so the
	// policy gets a gradient through the last few metres where the evader dodges hardest.
	// Unlike the distance term above this one cannot be farmed by fleeing: its value is
	// a pure function of the current gap and is zero outside ProximityRadiusCm.
	if (FMath::IsFinite(NewDistance) && NewDistance < ProximityRadiusCm)
	{
		Reward += ProximityBonus * StepScale * (1.0f - NewDistance / ProximityRadiusCm);
	}

	// TRUNCATION penalty. Without this, "run out of clock" is as good as "catch", so in
	// the last second the argmax is to keep the gap open rather than gamble on contact.
	// Scaled up from a token amount (2026-09-22): it has to at least outweigh the
	// existence penalty the agent avoided by not being caught, or timing out stays free.
	if (bOutOfTime)
	{
		Reward += TimeoutPenalty;
	}

	if (bWallHit)
	{
		Reward += WallHitPenalty * StepScale;
	}

	// Jump cost, charged on the DECISION (see JumpActionPenalty). The airborne-state
	// version that preceded this was measured ineffective: a policy that hops
	// continuously pays an airborne penalty on essentially every step no matter what
	// it chooses, so the advantage baseline swallows it and no gradient survives.
	// Charging the action makes "hold jump" and "walk" different returns, which is the
	// only shape PPO can act on. Measured context: grounded fraction 0.00-0.01, chaser
	// Z sawtoothing 87..264 cm over ground Z 70, mean speed 58-87 cm/s against 575.
	// Gated on bEnableAgentJump (2026-09-22): when the stage has jump execution switched
	// off, the intent can never become a jump, so charging for it is a penalty with no
	// behaviour attached - pure noise in the advantage estimate. The INTENT is still read
	// in Step_Implementation (the ACTION PROBE stays informative); only the charge is
	// suppressed. No coefficient changes here: this is a gate, not a re-tune.
	if (JumpActionPenalty != 0.0f && bJumpRequestedThisStep && bEnableAgentJump)
	{
		Reward += JumpActionPenalty * StepScale;
	}

	// Legacy airborne cost, disabled by default (see its declaration): kept wired so a
	// run can still compare the two shapes without a rebuild.
	if (AirbornePenalty != 0.0f && ChaserAgent)
	{
		if (const UCharacterMovementComponent* ChaserMovement = ChaserAgent->GetCharacterMovement())
		{
			if (!ChaserMovement->IsMovingOnGround())
			{
				Reward += AirbornePenalty * StepScale;
			}
		}
	}

	if (bNewCatch)
	{
		Reward += CatchReward;
	}

	return Reward;
}

void APursuitCharEnv::FinishEpisodeStats(bool bWasCaught)
{
	(void)bWasCaught;
	// Hook for per-episode statistics beyond the log line (win rate windows etc.).
	// Kept minimal on purpose; v1's counters live on the HUD, which v2 does not have yet.
}

// ---------------------------------------------------------------------------
// Watched runs: no trainer attached. The env owns the episode lifecycle; the chaser is
// driven by the mode, the evader by the rule.
// ---------------------------------------------------------------------------

void APursuitCharEnv::Tick(float DeltaSeconds)
{
	if (DriveMode == EPursuitCharDriveMode::Train)
	{
		// Training: the connector manager owns the tick and pumps the environment.
		// -PursuitWatchForce still gets the per-frame view guard so a recording of a
		// connector-driven eval keeps the watch camera (presentation only).
		if (bForceWatchView)
		{
			MaintainWatchedView(DeltaSeconds);
		}
		Super::Tick(DeltaSeconds);
		return;
	}

	RunWatchedEpisode(DeltaSeconds);

	MaintainWatchedView(DeltaSeconds);

	UpdateWatchedHud();
}

void APursuitCharEnv::MaintainWatchedView(float DeltaSeconds)
{
	// Chase-follow focus (presentation only): ease the camera focus toward the
	// chaser/evader midpoint so the actual pursuit stays in frame at close watch
	// heights. A camera parked on the arena centre films empty floor for most
	// episodes - spawns scatter across the full arena radius, and only a lucky few
	// cross the centre (2026-09-23 v2 capture report). The -PursuitWatchAt absolute
	// override still wins: an explicitly parked camera is a deliberate framing.
	const bool bFollow = !bHasWatchCamOverride && ChaserAgent && EvaderAgent && WatchCamera;
	if (bFollow)
	{
		const FVector Mid = (ChaserAgent->GetActorLocation() + EvaderAgent->GetActorLocation()) * 0.5f;
		// Exponential lag tuned like GodCamera's: smooth pan while the chase moves,
		// instant snap when a new episode respawns the pair across the arena.
		constexpr float LagSpeed = 4.5f;
		constexpr float SnapDistance = 1500.0f;
		if (!bWatchFocusInit || FVector::Dist(Mid, WatchFocusSmoothed) > SnapDistance)
		{
			WatchFocusSmoothed = Mid;
			bWatchFocusInit = true;
		}
		else
		{
			const float Alpha = 1.0f - FMath::Exp(-LagSpeed * DeltaSeconds);
			WatchFocusSmoothed = FMath::Lerp(WatchFocusSmoothed, Mid, Alpha);
		}

		// Same placement math as SetupWatchedView's derived orbit, but around the
		// smoothed focus instead of the arena centre.
		const float PitchRadians = FMath::DegreesToRadians(FMath::Abs(WatchCamPitch));
		const float BackOff = WatchCamHeight / FMath::Tan(PitchRadians);
		const float YawRadians = FMath::DegreesToRadians(WatchCamYaw);
		const FVector CameraLocation = WatchFocusSmoothed
			- FVector(FMath::Cos(YawRadians) * BackOff, FMath::Sin(YawRadians) * BackOff, 0.0f)
			+ FVector(0.0f, 0.0f, WatchCamHeight);
		WatchCamera->SetActorLocation(CameraLocation);
		WatchCamera->SetActorRotation(FRotator(WatchCamPitch, WatchCamYaw, 0.0f));
	}

	// View-target guard, every watched frame: one comparison, and it beats any ordering
	// race between this BeginPlay, the pawn spawn and PIE view plumbing. If something
	// else claims the view the watch camera takes it back the same frame.
	if (WatchCamera)
	{
		if (APlayerController* PC = GEngine ? GEngine->GetFirstLocalPlayerController(GetWorld()) : nullptr)
		{
			if (PC->GetViewTarget() != WatchCamera)
			{
				PC->SetViewTarget(WatchCamera);
			}

			// The host's DefaultPawn is a grey spectator sphere parked near the map
			// origin - it used to photobomb every city shot as a mystery ball at 0,0
			// (2026-09-21 report). The watch camera owns the view, so the spectator
			// shell goes invisible once, the first frame we see it.
			if (APawn* Spectator = PC->GetPawn())
			{
				if (!Spectator->IsHidden() && Spectator->FindComponentByClass<UCameraComponent>() == nullptr)
				{
					Spectator->SetActorHiddenInGame(true);
				}
			}
		}
	}
}

void APursuitCharEnv::RunWatchedEpisode(float DeltaSeconds)
{
	if (!ChaserAgent)
	{
		return;
	}

	// Once the validation exit has been requested, stop driving entirely. RequestExit is
	// asynchronous and the process may tick several more frames; without this guard the
	// very next Tick would see bEpisodeRunning=false and begin episode N+1, so the log
	// would end on a half-finished episode and read as "the run died mid-episode".
	if (bValidationExitRequested)
	{
		return;
	}

	// Self-exit for headless runs: a watched run nobody can watch still has to end, or
	// the harness waits on a window that will never close (v1's RunSeconds, moved here).
	// This is the ANTI-HANG net only - it is not the episode-count mechanism and cannot
	// be: a headless run is far faster than realtime, so a wall-clock budget does not
	// land on an exact number of episodes. See MaxEpisodes.
	if (QuitAfterSeconds > 0.0)
	{
		if (WatchedStartTime == 0.0)
		{
			WatchedStartTime = FPlatformTime::Seconds();
		}
		else if (FPlatformTime::Seconds() - WatchedStartTime > QuitAfterSeconds)
		{
			UE_LOG(LogPursuitAI, Log, TEXT("PursuitCharEnv: QuitAfter reached (%.1f s), exiting"), QuitAfterSeconds);
			if (APlayerController* PC = GEngine ? GEngine->GetFirstLocalPlayerController(GetWorld()) : nullptr)
			{
				PC->ConsoleCommand(TEXT("quit"), true);
			}
			return;
		}
	}

	// The -PursuitStartAt gate, v1's dual-pane synchroniser: both processes sit frozen
	// at their spawns until the same wall-clock second, so a side-by-side comparison
	// opens on two panes in the same pose and the sync is visible in the first frame.
	// Coarse epoch seconds are the same convention v1 passes through its tooling.
	if (bWaitingForGo)
	{
		if (FDateTime::UtcNow().ToUnixTimestamp() < StartAtEpoch)
		{
			StopAgent(ChaserAgent);
			StopAgent(EvaderAgent);
			StopAgent(SupportAgent);
			return;
		}
		bWaitingForGo = false;
		BeginEpisode();
	}
	else if (!bEpisodeRunning)
	{
		BeginEpisode();
	}

	// The chaser: policy-driven agents step themselves (their own Tick); the baselines
	// are driven here, through the same AddMovementInput door as everything else. The
	// greedy baseline shares the evader's obstacle reflex (probe, hop low blockers,
	// rotate around tall ones) so "straight at the target" remains a meaningful baseline
	// in the obstacle arena - and so the jump is visible in watched runs before any
	// policy exists. Random stays reflex-free on purpose: it is the no-information
	// control, obstacles and all.
	if (DriveMode == EPursuitCharDriveMode::Greedy && EvaderAgent)
	{
		FVector ToTarget = EvaderAgent->GetActorLocation() - ChaserAgent->GetActorLocation();

		// City stage: past the plaza edge the chase direction bends inward, the same
		// bend the evader's leash uses - a straight pursuit around a building can walk
		// the dog two streets out of frame (episode 1 of the 2026-09-21 regression).
		// Still straight-at-target inside the plaza.
		if (bCityStage)
		{
			const FVector FromCentre = ChaserAgent->GetActorLocation() - GetActorLocation();
			const float DistFromCentre = FromCentre.Size2D();
			if (DistFromCentre > CityStageRadius)
			{
				const float Weight = FMath::Min((DistFromCentre - CityStageRadius) / (CityStageRadius * 0.3f), 3.0f);
				ToTarget = (ToTarget.GetSafeNormal2D() - FromCentre.GetSafeNormal2D() * Weight) * ToTarget.Size2D();
			}
		}
		SteerScriptedAgent(ChaserAgent, ToTarget);

		// The support soldier runs the same greedy steering and shares the reflex, so
		// both chasers read as one pack closing from two angles. Same inward bend on
		// the city stage - a straight pursuit past a facade walks the soldier out of
		// frame exactly like it walked the dog out.
		if (SupportAgent)
		{
			FVector SupportToTarget = EvaderAgent->GetActorLocation() - SupportAgent->GetActorLocation();
			if (bCityStage)
			{
				const FVector SupportFromCentre = SupportAgent->GetActorLocation() - GetActorLocation();
				const float SupportDist = SupportFromCentre.Size2D();
				if (SupportDist > CityStageRadius)
				{
					const float Weight = FMath::Min((SupportDist - CityStageRadius) / (CityStageRadius * 0.3f), 3.0f);
					SupportToTarget = (SupportToTarget.GetSafeNormal2D() - SupportFromCentre.GetSafeNormal2D() * Weight)
						* SupportToTarget.Size2D();
				}
			}
			SteerScriptedAgent(SupportAgent, SupportToTarget);
		}
	}
	else if (DriveMode == EPursuitCharDriveMode::Random)
	{
		// Hold the heading for RandomHeadingHoldSeconds of sim time. A per-frame
		// re-roll flips the move input faster than the movement component can build
		// speed - the dog visibly twitched in place at near-zero net speed (2026-09-21
		// watch report). The hold keeps it a wander, still with zero target knowledge.
		RandomHeadingHold -= DeltaSeconds;
		if (RandomHeadingHold <= 0.0f)
		{
			RandomHeadingAngle = DriveRng.FRandRange(0.0f, 2.0f * UE_PI);
			RandomHeadingHold = RandomHeadingHoldSeconds;
		}
		FVector Desired(FMath::Cos(RandomHeadingAngle), FMath::Sin(RandomHeadingAngle), 0.0f);

		// City stage: bend the wander back inside the plaza past the leash edge, same
		// bend as the evader's - an unbounded random walk ends the shot two streets
		// away, and the camera never follows. Still no target knowledge: the bend is
		// a function of the dog's own position, not of the evader.
		if (bCityStage)
		{
			const FVector FromCentre = ChaserAgent->GetActorLocation() - GetActorLocation();
			const float DistFromCentre = FromCentre.Size2D();
			if (DistFromCentre > CityStageRadius)
			{
				const float Weight = FMath::Min((DistFromCentre - CityStageRadius) / (CityStageRadius * 0.3f), 3.0f);
				Desired = (Desired.GetSafeNormal2D() - FromCentre.GetSafeNormal2D() * Weight) * Desired.Size2D();
			}
		}
		ChaserAgent->ApplyScriptedMove(Desired);

		// The soldier walks the same held heading as the dog - a pack wandering
		// together. No target knowledge either way: the heading is drawn from the
		// drive RNG alone, so the no-information property of the random baseline
		// survives the second chaser.
		if (SupportAgent)
		{
			if (bCityStage)
			{
				const FVector SupportFromCentre = SupportAgent->GetActorLocation() - GetActorLocation();
				const float SupportDist = SupportFromCentre.Size2D();
				if (SupportDist > CityStageRadius)
				{
					const float Weight = FMath::Min((SupportDist - CityStageRadius) / (CityStageRadius * 0.3f), 3.0f);
					Desired = (Desired.GetSafeNormal2D() - SupportFromCentre.GetSafeNormal2D() * Weight) * Desired.Size2D();
				}
			}
			SupportAgent->ApplyScriptedMove(Desired);
		}
	}

	AdvanceScriptedDrivers(DeltaSeconds);

	// Lifecycle: catch and timeout end the episode exactly as training would score it.
	// Either chaser counts: the dog is the scored agent, but in a watched run the
	// soldier closing the distance is the same pack capture - "soldier touches the
	// hero and nothing happens" read as a broken game (2026-09-21 report). Training
	// has no SupportAgent, so its scoring path is untouched.
	const float Distance = EvaderAgent
		? static_cast<float>(FVector::Dist2D(ChaserAgent->GetActorLocation(), EvaderAgent->GetActorLocation()))
		: TNumericLimits<float>::Max();
	const float DeltaZ = EvaderAgent
		? FMath::Abs(ChaserAgent->GetActorLocation().Z - EvaderAgent->GetActorLocation().Z)
		: TNumericLimits<float>::Max();
	bool bNewCatch = Distance <= CatchRadius && DeltaZ <= CatchHeightTolerance;
	const TCHAR* CaughtBy = bNewCatch ? TEXT("dog") : TEXT("");
	if (!bNewCatch && SupportAgent && EvaderAgent)
	{
		const float SupportDistance = static_cast<float>(
			FVector::Dist2D(SupportAgent->GetActorLocation(), EvaderAgent->GetActorLocation()));
		const float SupportDeltaZ = FMath::Abs(
			SupportAgent->GetActorLocation().Z - EvaderAgent->GetActorLocation().Z);
		if (SupportDistance <= CatchRadius && SupportDeltaZ <= CatchHeightTolerance)
		{
			bNewCatch = true;
			CaughtBy = TEXT("soldier");
		}
	}

	// Same simulated-seconds wall as training (Step_Implementation): episode duration
	// must not depend on the frame rate in watched runs either. Watched mode owns its
	// own accumulation because it never goes through Step_Implementation. MaxSteps is
	// not a timeout (same reasoning as the training path).
	EpisodeSimTime += DeltaSeconds;
	const bool bOutOfTime = !bNewCatch && EpisodeSimTime >= EpisodeSeconds;

	++CurrentStep;
	// Same sampler the training path uses - one definition of "grounded", "mean speed"
	// and "direction agreement" for both, so a Greedy baseline number and a PPO number
	// cannot mean subtly different things.
	AccumulateValidationSample(Distance);

	if (bNewCatch || bOutOfTime)
	{
		// End positions in the watched-run line: a chaser that never moved reads here as
		// start==end, which separates "policy did nothing" from "policy chased badly".
		UE_LOG(LogPursuitAI, Log,
			TEXT("PursuitCharEnv: episode %d %s%s in %d steps (%.1f sim-s)  chaser_end=(%.0f, %.0f)  evader_end=(%.0f, %.0f)  wall_hits=%d"),
			EpisodeIndex, bNewCatch ? TEXT("CAUGHT") : TEXT("TIMEOUT"),
			bNewCatch ? *FString::Printf(TEXT(" by %s"), CaughtBy) : TEXT(""),
			CurrentStep, EpisodeSimTime,
			ChaserAgent->GetActorLocation().X, ChaserAgent->GetActorLocation().Y,
			EvaderAgent ? EvaderAgent->GetActorLocation().X : 0.0f,
			EvaderAgent ? EvaderAgent->GetActorLocation().Y : 0.0f,
			WallHitCount);

		// Stage 0 acceptance data for the SAME episode. Greedy and Random are driven
		// through SteerScriptedAgent -> ApplyScriptedMove -> AddMovementInput, while PPO
		// goes FDictPoint -> actuator -> AddMovementInput: the two share the
		// AddMovementInput and CharacterMovement execution layer and NOTHING above it.
		// So this line measures the baseline's own behaviour and is not a stand-in for a
		// policy's, which is exactly how the report has to describe it.
		LogEpisodeValidationSummary(bNewCatch ? TEXT("CAUGHT") : TEXT("TIMEOUT"), Distance);

		bEpisodeRunning = false;
		NoteEpisodeCompleted(bNewCatch);
	}
}

void APursuitCharEnv::AdvanceScriptedDrivers(float DeltaSeconds)
{
	(void)DeltaSeconds;

	// The evader flees: away from the chaser, at its own speed - with one obstacle-era
	// addition. A straight-line flee walks face-first into a pillar and then grinds
	// against it for the rest of the episode, so the flee direction is probed with a
	// capsule-sized sweep and steered (or hopped) by the shared reflex below.
	if (bStaticTarget || !EvaderAgent || !ChaserAgent)
	{
		return;
	}

	const FVector Away = EvaderAgent->GetActorLocation() - ChaserAgent->GetActorLocation();
	FVector Desired = Away;

	// City leash: the plaza is where the camera is, and a straight-line flee would
	// drag the chase two streets away within one 15 s episode. Past the leash radius
	// the flee direction bends inward, harder the farther out it gets; the shared
	// reflex below still handles buildings between here and there.
	if (bCityStage)
	{
		// Leash at the plaza edge, not at 0.6 * ArenaRadius: the spawn draw is capped
		// to CityStageRadius, so a 720 cm leash would still let the flee wander two
		// streets past where the spawns - and the camera - live.
		const FVector FromCentre = EvaderAgent->GetActorLocation() - GetActorLocation();
		const float DistFromCentre = FromCentre.Size2D();
		const float Leash = CityStageRadius;
		if (DistFromCentre > Leash)
		{
			const float Weight = FMath::Min((DistFromCentre - Leash) / (CityStageRadius * 0.3f), 3.0f);
			Desired = (Away.GetSafeNormal2D() - FromCentre.GetSafeNormal2D() * Weight) * Away.Size2D();
		}
	}

	SteerScriptedAgent(EvaderAgent, Desired);
}

void APursuitCharEnv::SteerScriptedAgent(APursuitCharAgent* Mover, const FVector& Desired)
{
	if (!Mover)
	{
		return;
	}

	// Stage 3 blockage control. The full driver below probes ahead and, when the ray is
	// blocked by something taller than a hop, tries eight detour bearings in order of
	// magnitude and takes the first clear one. That is precisely what makes it the
	// SOLVABILITY ORACLE - it is the driver that must succeed for "the blocked layout is
	// solvable" to be a supported claim. It is therefore the wrong instrument for the
	// opposite claim: a driver that routes around anything would also route around a
	// layout with nothing to route around, so "the oracle passed" cannot distinguish
	// "the pillar blocks the line and routing solves it" from "there was nothing on the
	// line". This branch is that missing half. It drives the raw bearing to the target
	// with no probe and no detour, which is what a policy with no obstacle awareness
	// does - so if it fails where the oracle succeeds, the blockage is real and the
	// failure is attributable to routing, not to the layout.
	if (bScriptedStraightChase)
	{
		Mover->ApplyScriptedMove(Desired);
		return;
	}

	FHitResult Blocker;
	if (!ProbePath(Mover->GetActorLocation(), Desired, Blocker))
	{
		// Straight ahead is clear: the common case, one probe.
		Mover->ApplyScriptedMove(Desired);
		return;
	}

	// Blocked: is it hoppable? The side-hit Z of a sphere sweep cannot tell a low box
	// from a pillar (both produce horizontal contacts around capsule-centre height), so
	// measure the blocker's actual top: a vertical line trace from above the impact
	// point. Tops under ~100 cm sit far below the ~90 cm jump apex (plus the capsule
	// rides up with it); the 400 cm pillars do not.
	const UWorld* World = GetWorld();
	const FVector ImpactXY(Blocker.ImpactPoint.X, Blocker.ImpactPoint.Y, 0.0f);
	const FVector TopStart = ImpactXY + FVector(0.0f, 0.0f, GetActorLocation().Z + 300.0f);

	FCollisionQueryParams TopParams(SCENE_QUERY_STAT(PursuitBlockerTop), false);
	TopParams.AddIgnoredActor(ChaserAgent);
	TopParams.AddIgnoredActor(EvaderAgent);
	FHitResult TopHit;
	const bool bTopFound = World && World->LineTraceSingleByChannel(
		TopHit, TopStart, TopStart - FVector(0.0f, 0.0f, 500.0f), ECC_WorldStatic, TopParams);
	const float BlockerTopHeight = bTopFound
		? TopHit.ImpactPoint.Z - GetActorLocation().Z
		: TNumericLimits<float>::Max();

	if (BlockerTopHeight < 100.0f)
	{
		// Hoppable: fire the jump through the agent's own actuator so the grounded +
		// cooldown gates apply exactly as they would for a trained policy, and keep
		// pushing straight through the arc.
		if (Mover->JumpActuator)
		{
			FBoxPoint JumpIntent;
			JumpIntent.Values.Add(1.0f);
			Mover->JumpActuator->TakeAction(JumpIntent);
		}
		Mover->ApplyScriptedMove(Desired);
		return;
	}

	static constexpr float ProbeOffsets[] = { 30.0f, -30.0f, 60.0f, -60.0f, 95.0f, -95.0f, 130.0f, -130.0f };
	for (const float Offset : ProbeOffsets)
	{
		const FVector Candidate = Desired.RotateAngleAxis(Offset, FVector::UpVector);
		if (!IsPathBlocked(Mover->GetActorLocation(), Candidate))
		{
			Mover->ApplyScriptedMove(Candidate);
			return;
		}
	}

	// Every bearing blocked (dead corner between pillars and the wall): push into the
	// straight bearing and let the collision decide.
	Mover->ApplyScriptedMove(Desired);
}

bool APursuitCharEnv::ProbePath(const FVector& From, const FVector& Dir, FHitResult& OutHit) const
{
	const UWorld* World = GetWorld();
	if (!World)
	{
		return false;
	}

	FCollisionQueryParams Params(SCENE_QUERY_STAT(PursuitPathProbe), false);
	Params.AddIgnoredActor(ChaserAgent);
	Params.AddIgnoredActor(EvaderAgent);

	// Swept at the capsule centre height with a capsule-radius sphere: low boxes block
	// the probe (hop it, or drive around), the floor does not.
	const FVector Start(From.X, From.Y, From.Z);
	const FVector End = Start + Dir.GetSafeNormal2D() * ProbeDistance;

	return World->SweepSingleByChannel(OutHit, Start, End, FQuat::Identity,
		ECC_WorldStatic, FCollisionShape::MakeSphere(ProbeRadius), Params);
}

bool APursuitCharEnv::IsPathBlocked(const FVector& From, const FVector& Dir) const
{
	FHitResult Unused;
	return ProbePath(From, Dir, Unused);
}

// ---------------------------------------------------------------------------
// Stage 0 validation helpers (2026-09-22)
//
// Everything below exists to answer ONE question with evidence instead of a reward
// curve: on a flat, static, jump-disabled arena, does the chaser actually close the gap
// at a sane speed, facing the right way, without the wall-hit counter inventing hits?
// Reward and TensorBoard cannot answer any part of that - a run can collect reward while
// running away, and it has (see DistanceShapingScale's declaration).
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Stage 3: one tall pillar, and only "route around it" (2026-09-23)
//
// The stage exists to answer four questions with evidence, in this order:
//   1. is the pillar actually there, BlockAll, and tall enough that jumping is not an
//      answer (jump is disabled anyway)?
//   2. can the EXISTING 15D observation see it, from the right probe, on the correct
//      side, and does it recover to "clear" once the pillar leaves the probe range?
//   3. is a blocked layout solvable at all by something that knows how to route (the
//      scripted obstacle oracle), so a policy failure cannot be blamed on the layout?
//   4. how does the frozen Moving050 default model (rep3) actually behave in each group?
//
// Nothing below touches the observation, the action space, the reward or the physics -
// it only builds stage geometry and places the pair - which is the only way the rep3
// numbers from this stage can mean anything next to its Moving050 numbers.
// ---------------------------------------------------------------------------

void APursuitCharEnv::BuildStage3Pillar()
{
	UStaticMesh* CubeMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (!CubeMesh)
	{
		// Fail loudly: a Stage 3 level with no pillar is a Moving050 run wearing a Stage 3
		// name, and every number it produced would be read as "the agent ignored the
		// pillar" instead of "the pillar was never built".
		UE_LOG(LogPursuitAI, Error,
			TEXT("PursuitCharEnv: Stage 3 could not load the engine cube - the stage would have no pillar at all"));
		return;
	}

	const FVector Centre = GetActorLocation();
	const float Footprint = FMath::Max(Stage3PillarFootprintCm, 60.0f);
	const float Height = FMath::Max(Stage3PillarHeightCm, 120.0f);

	Stage3Pillar = NewObject<UStaticMeshComponent>(this, TEXT("Stage3Pillar"));
	Stage3Pillar->SetStaticMesh(CubeMesh);
	// BlockAll, exactly like the greybox obstacle table and the boundary ring: the same
	// collision the wall probes sweep against (ECC_WorldStatic) and the same one
	// CharacterMovement collides with, so "what the policy sees" and "what stops the
	// capsule" cannot disagree - which is the whole reason a stage-placed mesh was
	// rejected in favour of building it here.
	Stage3Pillar->SetCollisionProfileName(TEXT("BlockAll"));
	Stage3Pillar->SetupAttachment(GetRootComponent());
	// The Cube's origin is its centre, so Z = half the height puts the base on the floor plane.
	Stage3Pillar->SetRelativeLocation(FVector(0.0f, 0.0f, Height * 0.5f));
	Stage3Pillar->SetRelativeScale3D(FVector(Footprint / 100.0f, Footprint / 100.0f, Height / 100.0f));
	Stage3Pillar->RegisterComponent();

	// Spawn avoidance, even though the Stage 3 pair is CONSTRUCTED rather than drawn: any
	// future path that draws a spawn point must still not materialise a capsule inside the
	// pillar. Half-diagonal plus one capsule radius, the same rule the greybox table uses.
	FPursuitObstacle Disc;
	Disc.Centre = FVector2D::ZeroVector;
	Disc.ClearRadius = Footprint * 0.5f * FMath::Sqrt(2.0f) + 34.0f;
	RigObstacleDiscs.Add(Disc);

	UE_LOG(LogPursuitAI, Log,
		TEXT("PursuitCharEnv: STAGE3 pillar built - footprint %.0f x %.0f cm, height %.0f cm, centre (%.0f, %.0f), base z=%.0f, capsule spawn Z=%.0f, collision profile BlockAll on ECC_WorldStatic, jumpable=%s"),
		Footprint, Footprint, Height, Centre.X, Centre.Y, Centre.Z,
		Centre.Z + SpawnHeight,
		(Height <= 90.0f) ? TEXT("yes") : TEXT("no"));
}

void APursuitCharEnv::PlaceStage3SpawnPair(FVector2D& OutChaserPoint, FVector2D& OutEvaderPoint)
{
	// Episode parity picks the group: even = blocked, odd = clear. Parity, not a coin flip,
	// because a run then contains BOTH groups in an exactly known order - and the two groups
	// differ by one translation only (same separation, same target bearing, same facing,
	// same arena), so a difference in outcome is attributable to routing rather than to two
	// subtly different tasks.
	bStage3BlockedLayout = (EpisodeIndex % 2 == 0);

	const float Half = FMath::Max(Stage3SpawnSeparationCm, 200.0f) * 0.5f;
	const float Lateral = bStage3BlockedLayout ? 0.0f : Stage3ClearLateralOffsetCm;

	OutChaserPoint = FVector2D(-Half, Lateral);
	OutEvaderPoint = FVector2D(+Half, Lateral);

	// Legality, measured rather than assumed - the "fake problem" list of this stage is
	// exactly "spawned inside the pillar" and "spawned somewhere the boundary makes the
	// layout unwinnable", and both are checkable from these three numbers.
	const float HalfFootprint = FMath::Max(Stage3PillarFootprintCm, 60.0f) * 0.5f;
	const float ChaserDx = FMath::Max(FMath::Abs(OutChaserPoint.X) - HalfFootprint, 0.0f);
	const float ChaserDy = FMath::Max(FMath::Abs(OutChaserPoint.Y) - HalfFootprint, 0.0f);
	const float ChaserFaceGapCm = FMath::Sqrt(ChaserDx * ChaserDx + ChaserDy * ChaserDy);
	const float ArenaMarginCm = ArenaRadius - FMath::Max(
		FVector2D::Distance(OutChaserPoint, FVector2D::ZeroVector),
		FVector2D::Distance(OutEvaderPoint, FVector2D::ZeroVector));

	UE_LOG(LogPursuitAI, Log,
		TEXT("PursuitCharEnv: STAGE3 spawn layout=%s chaser=(%.0f, %.0f) evader=(%.0f, %.0f) separation=%.0f ")
		TEXT("chaser_to_pillar_face=%.0f cm (capsule gap %.0f cm) nearest_wall=%.0f cm line_blocked=%s"),
		bStage3BlockedLayout ? TEXT("blocked") : TEXT("clear"),
		OutChaserPoint.X, OutChaserPoint.Y, OutEvaderPoint.X, OutEvaderPoint.Y,
		FVector2D::Distance(OutChaserPoint, OutEvaderPoint),
		ChaserFaceGapCm, ChaserFaceGapCm - 34.0f, ArenaMarginCm,
		bStage3BlockedLayout ? TEXT("yes") : TEXT("no"));

	if (ChaserFaceGapCm <= 34.0f || ArenaMarginCm <= 100.0f)
	{
		UE_LOG(LogPursuitAI, Error,
			TEXT("PursuitCharEnv: Stage 3 layout %s is not legal - a capsule is inside the pillar or within 100 cm of the boundary wall; the groups must differ ONLY by whether the pillar is on the chase line"),
			bStage3BlockedLayout ? TEXT("blocked") : TEXT("clear"));
	}
}

void APursuitCharEnv::RunStage3ProbeSelfTest()
{
	if (!ChaserAgent || !EvaderAgent || !Stage3Pillar)
	{
		UE_LOG(LogPursuitAI, Error,
			TEXT("PursuitCharEnv: Stage 3 probe self-test requested but the stage is incomplete (chaser=%s evader=%s pillar=%s) - no sweep logged"),
			ChaserAgent ? TEXT("ok") : TEXT("missing"),
			EvaderAgent ? TEXT("ok") : TEXT("missing"),
			Stage3Pillar ? TEXT("ok") : TEXT("missing"));
		return;
	}

	const FVector ArenaCentre = GetActorLocation();
	const float SpawnZ = ArenaCentre.Z + SpawnHeight;

	// The sweep is evidence ABOUT the stage; it must not become part of the episode. Both
	// agents are therefore put back exactly where the episode placed them.
	const FVector ChaserStart = ChaserAgent->GetActorLocation();
	const float ChaserStartYaw = ChaserAgent->GetActorRotation().Yaw;
	const FVector EvaderStart = EvaderAgent->GetActorLocation();
	const float EvaderStartYaw = EvaderAgent->GetActorRotation().Yaw;

	struct FProbeSample
	{
		const TCHAR* Label;
		float X;        // chaser position, env-relative cm
		float Y;
		float Yaw;      // chaser facing, degrees (0 = +X, +90 = +Y)
		bool bParkEvaderAhead;  // park the evader 200 cm in front of the chaser
	};

	// The list is chosen so that each sample isolates ONE claim, and so that the expected
	// reading is computable offline from the logged position and facing alone. Every
	// expectation below was worked out against the actual geometry before the run - the
	// pillar is a 300 x 300 x 400 box centred on the arena origin, the boundary ring is a
	// 50 cm thick wall whose INNER face sits at radius 1175, and a probe is a 34 cm sphere
	// swept from +35 to +800 cm along the bearing (so it contacts anything whose distance
	// along the ray is <= 834). tools/stage3a_probe_check.py re-derives all of this from
	// scratch and fails the sample when the engine disagrees with the derivation.
	//
	//   front_400            pillar dead ahead at 250 to the near face: only the 0-degree
	//                        probe fires (the +-45 rays pass ~283 cm wide of the near
	//                        corners and the ring is 1104 away) - "the pillar is seen, and
	//                        it is seen in the middle".
	//   front_300_sym        same bearing, 100 closer: the near corners now fall inside the
	//                        34 cm sphere of both +-45 rays, so left-front / middle /
	//                        right-front all fire, the two side readings being EQUAL to
	//                        each other and the middle the smallest - the symmetry check.
	//   pillar_right_90      chaser (0,-250) facing +X: the pillar is squarely to the RIGHT,
	//                        so only the right half of the fan fires and the middle stays
	//                        exactly clear. The -90 side reads the ring at 925 (clear). This
	//                        is the "the reading agrees with the bearing" check, right half.
	//   pillar_left_90       the same position and geometry MIRRORED (facing -X): only the
	//                        left half fires and the fired index moves by four. Same input
	//                        shape, opposite answer - a sensor that ignored owner yaw would
	//                        return identical vectors for these two.
	//   front_right_45 /     pillar on a front diagonal, hit on its near corner: the +-45
	//   front_left_45        probe fires ALONE (the 0-degree ray runs parallel to the box
	//                        at 300 cm lateral offset, the opposite diagonal is 212 cm away
	//                        from anything) - one isolated index per sample, mirrored.
	//   front_1100_out       same bearing as front_400, now 950 to the near face: the pillar
	//                        is outside the 834 cm reach in the WHOLE fan, so the 0-degree
	//                        reading is back to exactly 1.000. The +-90 readings are 0.386 -
	//                        and that is the point: they are the boundary ring hit at 413,
	//                        i.e. the sensor is demonstrably alive in this very sample while
	//                        the pillar has provably left it. A dead sensor would read 1.000
	//                        everywhere, which is exactly what this sample rules out.
	//   back_to_pillar_clear pillar present but directly BEHIND the fan, on flat ground:
	//                        all five must read exactly clear - "behind is not ahead".
	//   target_ahead         the evader parked 200 cm dead ahead: all five must STILL read
	//                        exactly clear (the sensor ignores its own target actor), while
	//                        dims 0-2 must read (1.0, 0.0, 0.333) - the measurement behind
	//                        "the target is not a wall".
	static const FProbeSample Samples[] =
	{
		{ TEXT("front_400"),            0.0f,  -400.0f,  90.0f, false },
		{ TEXT("front_300_sym"),        0.0f,  -300.0f,  90.0f, false },
		{ TEXT("pillar_right_90"),      0.0f,  -250.0f,   0.0f, false },
		{ TEXT("pillar_left_90"),       0.0f,  -250.0f, 180.0f, false },
		{ TEXT("front_right_45"),    -300.0f,  -300.0f,   0.0f, false },
		{ TEXT("front_left_45"),     -300.0f,   300.0f,   0.0f, false },
		{ TEXT("front_1100_out"),       0.0f, -1100.0f,  90.0f, false },
		{ TEXT("back_to_pillar_clear"), -300.0f, 0.0f,  180.0f, false },
		{ TEXT("target_ahead"),      -300.0f,   0.0f,  180.0f, true  },
	};

	UE_LOG(LogPursuitAI, Log,
		TEXT("PursuitCharEnv: STAGE3 PROBESWEEP begin - %d samples, pillar footprint=%.0f height=%.0f at (%.0f, %.0f), capsule Z=%.0f (probe range/radius come from UPursuitTargetSensor: 800 cm / 34 cm)"),
		UE_ARRAY_COUNT(Samples), Stage3PillarFootprintCm, Stage3PillarHeightCm,
		ArenaCentre.X, ArenaCentre.Y, SpawnZ);

	for (const FProbeSample& Sample : Samples)
	{
		const FVector ChaserLocation(
			ArenaCentre.X + Sample.X, ArenaCentre.Y + Sample.Y, SpawnZ);
		PlaceAgent(ChaserAgent, ChaserLocation, Sample.Yaw);

		if (Sample.bParkEvaderAhead)
		{
			const FVector Ahead = FRotator(0.0f, Sample.Yaw, 0.0f).Vector().GetSafeNormal2D();
			PlaceAgent(EvaderAgent, ChaserLocation + Ahead * 200.0f, Sample.Yaw + 180.0f);
		}

		// One Observe, exactly the call the training path makes - so these numbers are the
		// numbers a policy receives, not a re-derivation of them.
		FInstancedStruct ProbeObs;
		IAgent::Execute_Observe(ChaserAgent, ProbeObs);
		const FBoxPoint* Point = ResolveTargetSensorPoint(ProbeObs);

		if (!Point || Point->Values.Num() < 5)
		{
			UE_LOG(LogPursuitAI, Error,
				TEXT("PursuitCharEnv: STAGE3 PROBE label=%s - no target-sensor reading (the sweep cannot conclude anything)"),
				Sample.Label);
			continue;
		}

		FString Values;
		for (int32 Index = 0; Index < Point->Values.Num(); ++Index)
		{
			Values += FString::Printf(TEXT("%s%.4f"), Index > 0 ? TEXT(",") : TEXT(""), Point->Values[Index]);
		}

		const FVector ActualChaser = ChaserAgent->GetActorLocation();
		UE_LOG(LogPursuitAI, Log,
			TEXT("PursuitCharEnv: STAGE3 PROBE label=%s chaser=(%.1f, %.1f, %.1f) yaw=%.1f evader=(%.1f, %.1f) ndim=%d obs=[%s]"),
			Sample.Label,
			ActualChaser.X, ActualChaser.Y, ActualChaser.Z,
			ChaserAgent->GetActorRotation().Yaw,
			EvaderAgent->GetActorLocation().X, EvaderAgent->GetActorLocation().Y,
			Point->Values.Num(), *Values);

		if (Sample.bParkEvaderAhead)
		{
			PlaceAgent(EvaderAgent, EvaderStart, EvaderStartYaw);
		}
	}

	// Put the episode back exactly as it was, and drop the differenced-velocity history the
	// sweep just accumulated: BeginEpisode clears it so that step 1 of an episode reports
	// tgt_spd = 0.0, and a sweep that left the sensor mid-pair would quietly break that.
	PlaceAgent(ChaserAgent, ChaserStart, ChaserStartYaw);
	if (ChaserAgent->TargetSensor)
	{
		ChaserAgent->TargetSensor->ResetVelocityHistory();
	}

	UE_LOG(LogPursuitAI, Log,
		TEXT("PursuitCharEnv: STAGE3 PROBESWEEP end - chaser restored to (%.1f, %.1f, %.1f) yaw=%.1f, evader to (%.1f, %.1f); episode 1 continues unchanged"),
		ChaserStart.X, ChaserStart.Y, ChaserStart.Z, ChaserStartYaw,
		EvaderStart.X, EvaderStart.Y);
}

// ---------------------------------------------------------------------------
// Stage 4A - single low wall + jump. Environment acceptance only: nothing here
// touches the observation, the action space, the reward or the physics. It builds
// geometry, places the pair, and CLASSIFIES evidence that already exists in the
// log - the same division of labour Stage 3A used, so rep3's numbers on this map
// can be read beside its Moving050 and Stage 3 numbers without a translation.
// ---------------------------------------------------------------------------

void APursuitCharEnv::BuildStage4Wall()
{
	UStaticMesh* CubeMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (!CubeMesh)
	{
		// Fail loudly: a Stage 4 level with no wall is a Moving050 run wearing a Stage 4 name,
		// and every number it produced would be read as "the agent ignored the wall".
		UE_LOG(LogPursuitAI, Error,
			TEXT("PursuitCharEnv: Stage 4 could not load the engine cube - the stage would have no wall at all"));
		return;
	}

	const FVector Centre = GetActorLocation();
	const float Thickness = FMath::Clamp(Stage4WallThicknessCm, 20.0f, 200.0f);
	const float Height = FMath::Clamp(Stage4WallHeightCm, 20.0f, 300.0f);
	// 0 (or anything too short to block) means "span the arena". The +200 overlap into the
	// boundary ring is deliberate: a wall that ended exactly at the ring's inner radius would
	// leave a capsule-width slot in each corner, which would surface as wall_cross_around > 0 -
	// the counter that exists to catch exactly this - instead of as a clean must-jump.
	const float Length = FMath::Max(Stage4WallLengthCm, 2.0f * ArenaRadius + 200.0f);

	// Write the resolved values back so the per-episode STAGE4 / STAGE4RESULT lines report the
	// geometry that was actually built. A log that echoes the REQUEST cannot catch a request the
	// builder silently overrode, and this stage's whole subject is the physical size of this box.
	Stage4WallThicknessCm = Thickness;
	Stage4WallHeightCm = Height;
	Stage4WallLengthCm = Length;

	Stage4Wall = NewObject<UStaticMeshComponent>(this, TEXT("Stage4Wall"));
	Stage4Wall->SetStaticMesh(CubeMesh);
	// BlockAll on ECC_WorldStatic, the same profile as the Stage 3 pillar, the greybox table and
	// the boundary ring: the wall probes sweep ECC_WorldStatic and CharacterMovement collides with
	// BlockAll, so "what the policy sees" and "what stops the capsule" cannot disagree.
	Stage4Wall->SetCollisionProfileName(TEXT("BlockAll"));
	Stage4Wall->SetupAttachment(GetRootComponent());
	// The Cube's origin is its centre, so Z = half the height puts the base on the floor plane.
	Stage4Wall->SetRelativeLocation(FVector(0.0f, 0.0f, Height * 0.5f));
	Stage4Wall->SetRelativeScale3D(FVector(Thickness / 100.0f, Length / 100.0f, Height / 100.0f));
	Stage4Wall->RegisterComponent();

	// Spawn avoidance. One disc would have to be as long as the wall to cover it, which would
	// wipe out the whole spawn area, so the wall registers a ROW of discs - each the same
	// "half-diagonal plus one capsule radius" rule the Stage 3 pillar and the greybox table use.
	const float DiscRadius = FMath::Sqrt(2.0f) * Thickness * 0.5f + 34.0f;
	const int32 DiscCount = FMath::Max(2, FMath::CeilToInt(Length / (2.0f * DiscRadius)) + 1);
	for (int32 Index = 0; Index < DiscCount; ++Index)
	{
		const float T = (DiscCount > 1)
			? (static_cast<float>(Index) / static_cast<float>(DiscCount - 1))
			: 0.5f;
		FPursuitObstacle Disc;
		Disc.Centre = FVector2D(0.0f, FMath::Lerp(-Length * 0.5f, Length * 0.5f, T));
		Disc.ClearRadius = DiscRadius;
		RigObstacleDiscs.Add(Disc);
	}

	// --- the acceptance measurement itself -------------------------------------------
	// Everything above is geometry. This block is why Stage 4A exists: read the walk and jump
	// envelopes off the LIVE chaser and state, in the log, whether this wall sits inside the jump
	// envelope and outside the step envelope. If it does not, the stage is measuring the wrong
	// thing and that is visible here instead of three stages later.
	float StepHeightCm = -1.0f;
	float JumpZVelocity = -1.0f;
	float GravityScale = -1.0f;
	float GravityZ = 0.0f;
	float ApexCm = -1.0f;
	if (ChaserAgent)
	{
		if (const UCharacterMovementComponent* Movement = ChaserAgent->GetCharacterMovement())
		{
			StepHeightCm = Movement->MaxStepHeight;
			JumpZVelocity = Movement->JumpZVelocity;
			GravityScale = Movement->GravityScale;
			GravityZ = Movement->GetGravityZ();
		}
	}
	if (JumpZVelocity > 0.0f && GravityZ != 0.0f)
	{
		// apex = v^2 / (2 g). g is read live off the movement component (GetGravityZ already folds
		// GravityScale in), so a future physics change cannot leave this derivation quietly stale.
		ApexCm = (JumpZVelocity * JumpZVelocity) / (2.0f * FMath::Abs(GravityZ));
	}
	const bool bOutsideStepEnvelope = Height > StepHeightCm + 5.0f;
	// 10 cm of clearance is required at the apex, not "apex >= height": the capsule has to be
	// ABOVE the wall for the whole time its 68 cm-wide body crosses the wall's 40 cm thickness,
	// so grazing the apex is not a pass.
	const bool bInsideJumpEnvelope = (ApexCm > 0.0f) && (ApexCm > Height + 10.0f);

	UE_LOG(LogPursuitAI, Log,
		TEXT("PursuitCharEnv: STAGE4 wall built - thickness %.0f cm (X), length %.0f cm (Y), height %.0f cm, centre (%.0f, %.0f), base z=%.0f, top z=%.0f, collision profile BlockAll on ECC_WorldStatic, spawn discs=%d"),
		Thickness, Length, Height, Centre.X, Centre.Y, Centre.Z, Centre.Z + Height, DiscCount);

	UE_LOG(LogPursuitAI, Log,
		TEXT("PursuitCharEnv: STAGE4 envelope - wall %.0f cm vs chaser max_step_height %.0f cm and jump_apex %.0f cm (JumpZVelocity %.0f, GravityScale %.1f, GravityZ %.1f, capsule_half_height %.1f); outside_step_envelope=%s inside_jump_envelope=%s"),
		Height, StepHeightCm, ApexCm, JumpZVelocity, GravityScale, GravityZ,
		ChaserAgent ? ChaserAgent->GetSimpleCollisionHalfHeight() : 0.0f,
		bOutsideStepEnvelope ? TEXT("yes") : TEXT("NO - this wall can be walked up"),
		bInsideJumpEnvelope ? TEXT("yes") : TEXT("NO - this wall cannot be jumped"));

	if (!bOutsideStepEnvelope || !bInsideJumpEnvelope)
	{
		UE_LOG(LogPursuitAI, Error,
			TEXT("PursuitCharEnv: Stage 4 wall height %.0f cm is OUTSIDE the window (step %.0f, apex %.0f) - must-jump would be untestable on this wall"),
			Height, StepHeightCm, ApexCm);
	}

	// The clearance channel's own limit, measured and logged rather than assumed. The sensor
	// encodes an obstacle as 1 - (top - OWNER_ACTOR_Z) / 400, i.e. relative to the capsule
	// CENTRE, and its vertical top-trace starts 300 cm above that same reference. Two
	// consequences this stage has to state out loud, because they decide whether a policy could
	// ever learn to jump from the observation alone:
	//   * anything whose top is at or below the capsule centre reads a clamped 1.0 - a value the
	//     "nothing here" case never produces (that is 0.5), but also a value that carries no
	//     height information at all;
	//   * anything whose top is 300 cm or more above the capsule centre reads a saturated 0.25,
	//     because the top-trace then begins INSIDE the blocker.
	// So a wall that is jumpable at all (top <= the ~90 cm apex, i.e. below the ~88 cm capsule
	// centre) is, by construction, invisible to the height channel. This is a REPORTING finding
	// for Stage 4A, not something this stage fixes: changing the encoding would change the 15D
	// semantics every existing model was trained on.
	const float CapsuleCentreZ = ChaserAgent ? ChaserAgent->GetActorLocation().Z : 0.0f;
	UE_LOG(LogPursuitAI, Log,
		TEXT("PursuitCharEnv: STAGE4 clearance-channel limit - wall top %.0f cm vs capsule centre %.0f cm and top-trace start %.0f cm; wall_reads_height_channel=%s (1.0 = shorter than the capsule centre, carries no height), tall_obstacles_saturate_at=0.25"),
		Centre.Z + Height, CapsuleCentreZ, CapsuleCentreZ + 300.0f,
		((Centre.Z + Height) <= CapsuleCentreZ) ? TEXT("1.0000 saturated-short") : TEXT("finite"));
}

void APursuitCharEnv::PlaceStage4SpawnPair(FVector2D& OutChaserPoint, FVector2D& OutEvaderPoint)
{
	// Episode parity picks the group: even = must-jump, odd = no-jump. Parity rather than a coin
	// flip for the Stage 3 reason - a run then contains BOTH groups in an exactly known order, and
	// the two differ by one translation only, so a difference in outcome is attributable to the
	// wall rather than to two subtly different tasks.
	bStage4BlockedLayout = (EpisodeIndex % 2 == 0);

	const float Half = FMath::Max(Stage4SpawnSeparationCm, 200.0f) * 0.5f;
	const float Shift = bStage4BlockedLayout ? 0.0f : Stage4ClearShiftCm;

	OutChaserPoint = FVector2D(-Half + Shift, 0.0f);
	OutEvaderPoint = FVector2D(+Half + Shift, 0.0f);

	// Legality, measured rather than assumed. Two DIFFERENT failure modes, so two different
	// checks: a capsule inside the wall (a spawn the geometry forbids) and a capsule close enough
	// to the boundary ring that the layout is unwinnable for reasons that have nothing to do with
	// the wall. Collapsing them into one number would hide which one fired.
	const float HalfThickness = FMath::Max(Stage4WallThicknessCm, 20.0f) * 0.5f;
	const float WallGapChaserCm = FMath::Abs(OutChaserPoint.X) - HalfThickness;
	const float WallGapEvaderCm = FMath::Abs(OutEvaderPoint.X) - HalfThickness;
	// "Which side" is what DEFINES the group, so it is derived from the placed points and logged
	// rather than asserted: a sign error in the shift would otherwise produce two groups that are
	// secretly the same experiment while both still pass every range check.
	const bool bOppositeSides = (OutChaserPoint.X > 0.0f) != (OutEvaderPoint.X > 0.0f);
	const float ArenaMarginCm = ArenaRadius - FMath::Max(
		FVector2D::Distance(OutChaserPoint, FVector2D::ZeroVector),
		FVector2D::Distance(OutEvaderPoint, FVector2D::ZeroVector));

	UE_LOG(LogPursuitAI, Log,
		TEXT("PursuitCharEnv: STAGE4 spawn layout=%s chaser=(%.0f, %.0f) evader=(%.0f, %.0f) separation=%.0f ")
		TEXT("chaser_to_wall_face=%.0f cm (capsule gap %.0f cm) evader_to_wall_face=%.0f cm nearest_wall=%.0f cm opposite_sides=%s straight_line_crosses_wall=%s"),
		bStage4BlockedLayout ? TEXT("must_jump") : TEXT("no_jump"),
		OutChaserPoint.X, OutChaserPoint.Y, OutEvaderPoint.X, OutEvaderPoint.Y,
		FVector2D::Distance(OutChaserPoint, OutEvaderPoint),
		WallGapChaserCm, WallGapChaserCm - 34.0f, WallGapEvaderCm, ArenaMarginCm,
		bOppositeSides ? TEXT("yes") : TEXT("no"),
		(bOppositeSides && bStage4BlockedLayout) ? TEXT("yes") : TEXT("no"));

	if (WallGapChaserCm <= 34.0f || WallGapEvaderCm <= 34.0f || ArenaMarginCm <= 100.0f)
	{
		UE_LOG(LogPursuitAI, Error,
			TEXT("PursuitCharEnv: Stage 4 layout %s is not legal - a capsule is inside the wall or within 100 cm of the boundary ring; the only difference between the groups must be which side of the wall the chase happens on"),
			bStage4BlockedLayout ? TEXT("must_jump") : TEXT("no_jump"));
	}
}

void APursuitCharEnv::AccumulateStage4Sample(float DistanceCm)
{
	if (!ChaserAgent)
	{
		return;
	}

	const UCharacterMovementComponent* Movement = ChaserAgent->GetCharacterMovement();
	const bool bGrounded = Movement && Movement->IsMovingOnGround();

	const float FloorZ = GetActorLocation().Z;
	const float WallX = GetActorLocation().X;
	const FVector ChaserLocation = ChaserAgent->GetActorLocation();
	// Named ...Cm rather than plain CapsuleHalfHeight: this translation unit pulls in engine
	// headers that already declare a global of that name, and UE builds with warnings-as-errors
	// (C4459), so the shorter name does not compile here.
	const float CapsuleHalfHeightCm = ChaserAgent->GetSimpleCollisionHalfHeight();
	// Height of the capsule's BOTTOM above the floor the rig built. Standing still this reads
	// ~0; at the top of a jump it reads the apex the physics actually produced. Measured off the
	// live capsule rather than off SpawnHeight, so it is evidence about the jump, not a restatement
	// of the spawn code.
	const float FeetAboveFloorCm = (ChaserLocation.Z - CapsuleHalfHeightCm) - FloorZ;

	// --- settle gate ---------------------------------------------------------------------
	// The pair is placed at SpawnHeight and free-falls the last few cm before its first grounded
	// frame. Without this gate that drop would be booked as air time, as the episode's jump apex
	// AND as a landing - three false readings inside the first fraction of a second, all three of
	// which this stage reports. Nothing is measured until the character has been grounded once;
	// the skipped steps are counted and logged so the gate stays auditable.
	if (!bStage4HasBeenGrounded)
	{
		if (!bGrounded)
		{
			++Stage4SettleStepsSkipped;
			Stage4PrevChaserX = ChaserLocation.X - WallX;
			if (EvaderAgent)
			{
				Stage4PrevEvaderX = EvaderAgent->GetActorLocation().X - WallX;
			}
			Stage4PrevJumpCount = ChaserAgent->JumpActuator ? ChaserAgent->JumpActuator->GetJumpCount() : 0;
			return;
		}
		bStage4HasBeenGrounded = true;
	}

	// --- jump intent vs execution --------------------------------------------------------
	// Intent is only observable through the POLICY path: the scripted oracle drives the actuator
	// directly (SteerScriptedAgent) and never touches the reward's request latch. That is why the
	// summary line prints jump_intent_channel beside jump_request_steps - on a watched run the
	// legitimate pair is "requests 0, jumps > 0", which without the token reads as a contradiction.
	if (bJumpRequestedThisStep)
	{
		++JumpRequestStepsThisEpisode;
	}
	const int32 JumpCountNow = ChaserAgent->JumpActuator ? ChaserAgent->JumpActuator->GetJumpCount() : 0;
	if (JumpCountNow > Stage4PrevJumpCount)
	{
		// A LAUNCH happened this step. Classified as meaningless when nothing was inside the
		// forward path probe - i.e. the driver jumped in open ground. Recomputed from the same
		// 300 cm sweep the scripted driver and the rollout both use, so the log can be re-derived
		// offline instead of taken on trust.
		const FVector Forward = ChaserAgent->GetActorRotation().Vector().GetSafeNormal2D();
		FHitResult Ahead;
		if (Forward.IsNearlyZero() || !ProbePath(ChaserLocation, Forward, Ahead))
		{
			++MeaninglessJumpCount;
		}
	}
	Stage4PrevJumpCount = JumpCountNow;

	// --- actual air time and the peak it reached -----------------------------------------
	if (!bGrounded)
	{
		++AirborneStepsThisEpisode;
		MaxJumpApexCmThisEpisode = FMath::Max(MaxJumpApexCmThisEpisode, FeetAboveFloorCm);
	}

	// --- wall-plane crossings, classified by the height at the crossing -------------------
	// "Over" means the feet were above the wall top as the plane was crossed. "Around" means they
	// were not - which, with a wall that spans the whole arena, cannot happen physically, so a
	// non-zero around-count is evidence of a GAP in the level and is reported separately instead
	// of being buried in a single crossing total.
	const float ChaserX = ChaserLocation.X - WallX;
	const bool bCrossed = (Stage4PrevChaserX < 0.0f && ChaserX >= 0.0f)
		|| (Stage4PrevChaserX > 0.0f && ChaserX <= 0.0f);
	if (bCrossed)
	{
		if (FeetAboveFloorCm >= Stage4WallHeightCm)
		{
			++WallCrossOverCount;
		}
		else
		{
			++WallCrossAroundCount;
		}
	}
	Stage4PrevChaserX = ChaserX;

	// The evader is tracked for one reason: if IT crosses, the "chaser had to jump" claim is
	// confounded by the target coming to the chaser, and the two cases must not be blended.
	if (EvaderAgent)
	{
		const float EvaderX = EvaderAgent->GetActorLocation().X - WallX;
		if ((Stage4PrevEvaderX < 0.0f && EvaderX >= 0.0f) || (Stage4PrevEvaderX > 0.0f && EvaderX <= 0.0f))
		{
			++EvaderWallCrossCount;
		}
		Stage4PrevEvaderX = EvaderX;
	}

	// --- landing, and whether the chase resumed -------------------------------------------
	// "Resumed" is deliberately weak: the question is only whether the chaser went back to
	// CLOSING after it touched down, not whether it closed well. A landing followed by 30 steps of
	// widening the gap is the failure this counter exists to expose, and it is exactly what a hop
	// that lands pointing the wrong way produces. The previous landing's window is closed out
	// BEFORE a new one is opened, so a landing that interrupts an open window still gets its own
	// verdict instead of silently inheriting the earlier one.
	if (Stage4LandingWindowRemaining > 0)
	{
		if (!Stage4LandingChaseResumed && DistanceCm < Stage4DistanceAtLastLandingCm)
		{
			Stage4LandingChaseResumed = true;
			++ChaseResumedAfterLandingCount;
		}
		--Stage4LandingWindowRemaining;
	}

	if (bStage4WasAirborneLastStep && bGrounded)
	{
		++LandingCountThisEpisode;
		Stage4DistanceAtLastLandingCm = DistanceCm;
		Stage4LandingWindowRemaining = 30;
		Stage4LandingChaseResumed = false;
	}
	bStage4WasAirborneLastStep = !bGrounded;
}

void APursuitCharEnv::RunStage4WallProbeSelfTest()
{
	if (!ChaserAgent || !EvaderAgent || !Stage4Wall)
	{
		UE_LOG(LogPursuitAI, Error,
			TEXT("PursuitCharEnv: Stage 4 probe self-test requested but the stage is incomplete (chaser=%s evader=%s wall=%s) - no sweep logged"),
			ChaserAgent ? TEXT("ok") : TEXT("missing"),
			EvaderAgent ? TEXT("ok") : TEXT("missing"),
			Stage4Wall ? TEXT("ok") : TEXT("missing"));
		return;
	}

	const FVector ArenaCentre = GetActorLocation();
	const float SpawnZ = ArenaCentre.Z + SpawnHeight;
	// Where the capsule actually rests: the engine puts the capsule's bottom on the floor, so the
	// centre sits one collision-half-height up. Read live rather than written as 7.0, so a change
	// to either property cannot leave the sweep sampling a pose that no longer exists.
	const float RestZ = ArenaCentre.Z + ChaserAgent->GetSimpleCollisionHalfHeight();

	// The sweep is evidence ABOUT the stage; it must not become part of the episode. Both agents
	// are put back exactly where the episode placed them.
	const FVector ChaserStart = ChaserAgent->GetActorLocation();
	const float ChaserStartYaw = ChaserAgent->GetActorRotation().Yaw;
	const FVector EvaderStart = EvaderAgent->GetActorLocation();
	const float EvaderStartYaw = EvaderAgent->GetActorRotation().Yaw;

	struct FProbeSample
	{
		const TCHAR* Label;
		float X;        // chaser position, env-relative cm
		float Y;
		float Yaw;      // chaser facing, degrees (0 = +X, +90 = +Y)
		bool bParkEvaderAhead;  // park the evader 200 cm in front of the chaser
		// Which height to sample at. This is not cosmetic, and the reason is the whole point of
		// the rest-height samples below: the pair is TELEPORTED to SpawnHeight (95 cm) and then
		// free-falls the last 7 cm to its resting centre (floor + capsule half height = 88 cm),
		// so a sweep taken at SpawnHeight is 7 cm above where the capsule actually sits while it
		// chases. The sensor sphere has a 34 cm radius, so its lower rim sits at 54 cm at rest and
		// at 61 cm at spawn - and a 55 cm wall top falls BETWEEN those two. Sampling only at
		// SpawnHeight therefore cannot answer "does the policy see this wall during play"; it
		// answers a question about a pose the capsule only occupies for a tenth of a second.
		bool bAtRestZ;
	};

	// Each sample isolates ONE claim, and the expected reading is computable offline from the
	// logged position, facing and wall size. The arithmetic the checker re-derives from scratch
	// (tools/stage4a_wall_check.py) is, for a probe that hits:
	//
	//     range     = clamp((D - 70) / 800, 0, 1)      D = |owner -> impact point|, cm
	//     clearance = clamp(1 - (railTop - ownerZ) / 400, 0, 1)
	//
	// and 0.5 in the clearance block means "hit nothing". The two constants that matter here are
	// the wall's own top (55 cm) and the boundary ring's (400 cm):
	//
	// WHAT THE FIRST RUN ACTUALLY MEASURED, because the prediction written here was WRONG and the
	// measurement is what the stage reports. At SpawnHeight the 55 cm wall is INVISIBLE: all five
	// range dims read 1.0000 and all five clearance dims 0.5000, i.e. bit-identical to empty
	// ground. The arithmetic below assumed the fan would strike the wall from 280 cm out, which
	// would be true for a horizontal ray; the sensor is a 34 cm SPHERE at capsule-centre height,
	// so its lower rim sits at 95 - 34 = 61 cm and clears a 55 cm top by 6 cm. Hence the rest-height
	// rows added below, where the rim is at 88 - 34 = 54 cm and overlaps by 1 cm.
	//
	//   wall_ahead_280   the wall is a PLANE, so when it IS tall enough to reach the sphere the
	//                    +-45 rays hit it too, at a longer range: measured 0.2641 at 0 deg and
	//                    0.3890 at +-45 for a 70 cm wall. At 55 cm: all clear, no exceptions.
	//   wall_ahead_480   same bearing, 200 further: 0.5134 at 0 deg for a 70 cm wall - monotone
	//                    in distance. At 55 cm: all clear.
	//   wall_ahead_680   at 680 the +-45 rays have left the reach, so only 0 deg fires: 0.7625
	//                    for a 70 cm wall. At 55 cm: all clear.
	//   wall_ahead_880_out  880 is past the reach: ALL FIVE read clear, and yet the +-90 rays
	//                    still find the boundary ring (0.2500) - so the sensor is demonstrably
	//                    alive in the very sample where the wall has provably left it.
	//   wall_ahead_offset_y  the same geometry 400 cm to the side: with a full-span wall every
	//                    reading must be IDENTICAL to wall_ahead_280 (Y-invariance).
	//   wall_behind      the wall behind the fan on flat ground: all five clear. "Behind is not
	//                    ahead."
	//   ring_ahead_275 / the SAME bearing against the 400 cm ring instead of the 55 cm wall. The
	//   ring_ahead_600   range readings are ordinary; the CLEARANCE readings are the point - the
	//                    ring saturates at 0.25 while a 55 cm wall reads 0.5 (not 1.0, as
	//                    predicted): below the sphere's lower rim the sensor reports "hit nothing"
	//                    rather than "hit something short". That is a stronger statement than the
	//                    one this comment used to make, and it was the measurement that made it.
	//   target_ahead     the evader parked 200 cm dead ahead: all five must STILL read exactly
	//                    clear (the sensor ignores its own target), while dims 0-2 read
	//                    (1.0, 0.0, 0.333) - "the target is not a wall".
	static const FProbeSample Samples[] =
	{
		// --- at SpawnHeight (95 cm): the pose the pair is teleported to, 7 cm above rest --------
		{ TEXT("wall_ahead_280"),        -300.0f,    0.0f,   0.0f, false, false },
		{ TEXT("wall_ahead_480"),        -500.0f,    0.0f,   0.0f, false, false },
		{ TEXT("wall_ahead_680"),        -700.0f,    0.0f,   0.0f, false, false },
		{ TEXT("wall_ahead_880_out"),    -900.0f,    0.0f,   0.0f, false, false },
		{ TEXT("wall_ahead_offset_y"),   -300.0f, -400.0f,   0.0f, false, false },
		{ TEXT("wall_behind"),           -300.0f,    0.0f, 180.0f, false, false },
		{ TEXT("ring_ahead_275"),         900.0f,    0.0f,   0.0f, false, false },
		{ TEXT("ring_ahead_600"),         575.0f,    0.0f,   0.0f, false, false },
		{ TEXT("target_ahead"),          -300.0f,    0.0f, 180.0f, true,  false },
		// --- at REST height (88 cm): the pose the capsule actually occupies during a chase ----
		// Same bearings as the first three, so any difference from the SpawnHeight rows is
		// attributable to the 7 cm alone. These are the rows that decide whether the policy can
		// see a jumpable wall at all, and the prediction they test is exact: the sensor sphere's
		// lower rim is at restZ - 34 = 54 cm, which is 1 cm BELOW a 55 cm wall top, so a wall
		// that is provably invisible at SpawnHeight should become faintly visible here.
		{ TEXT("wall_ahead_280_rest"),   -300.0f,    0.0f,   0.0f, false, true  },
		{ TEXT("wall_ahead_480_rest"),   -500.0f,    0.0f,   0.0f, false, true  },
		{ TEXT("wall_ahead_680_rest"),   -700.0f,    0.0f,   0.0f, false, true  },
		{ TEXT("ring_ahead_275_rest"),    900.0f,    0.0f,   0.0f, false, true  },
	};

	UE_LOG(LogPursuitAI, Log,
		TEXT("PursuitCharEnv: STAGE4 PROBESWEEP begin - %d samples, wall thickness=%.0f length=%.0f height=%.0f at x=%.0f, spawn Z=%.0f (sphere lower rim %.1f), rest Z=%.0f (sphere lower rim %.1f), ring height=%.0f inner radius=%.0f (probe range/radius come from UPursuitTargetSensor: 800 cm / 34 cm)"),
		UE_ARRAY_COUNT(Samples), Stage4WallThicknessCm, Stage4WallLengthCm, Stage4WallHeightCm,
		ArenaCentre.X, SpawnZ, SpawnZ - 34.0f, RestZ, RestZ - 34.0f, RigWallHeight, ArenaRadius);

	for (const FProbeSample& Sample : Samples)
	{
		const FVector ChaserLocation(ArenaCentre.X + Sample.X, ArenaCentre.Y + Sample.Y,
			Sample.bAtRestZ ? RestZ : SpawnZ);
		PlaceAgent(ChaserAgent, ChaserLocation, Sample.Yaw);

		if (Sample.bParkEvaderAhead)
		{
			const FVector Ahead = FRotator(0.0f, Sample.Yaw, 0.0f).Vector().GetSafeNormal2D();
			PlaceAgent(EvaderAgent, ChaserLocation + Ahead * 200.0f, Sample.Yaw + 180.0f);
		}

		// One Observe, exactly the call the training path makes - so these numbers are the numbers
		// a policy receives, not a re-derivation of them.
		FInstancedStruct ProbeObs;
		IAgent::Execute_Observe(ChaserAgent, ProbeObs);
		const FBoxPoint* Point = ResolveTargetSensorPoint(ProbeObs);

		if (!Point || Point->Values.Num() < 5)
		{
			UE_LOG(LogPursuitAI, Error,
				TEXT("PursuitCharEnv: STAGE4 PROBE label=%s - no target-sensor reading (the sweep cannot conclude anything)"),
				Sample.Label);
			continue;
		}

		FString Values;
		for (int32 Index = 0; Index < Point->Values.Num(); ++Index)
		{
			Values += FString::Printf(TEXT("%s%.4f"), Index > 0 ? TEXT(",") : TEXT(""), Point->Values[Index]);
		}

		const FVector ActualChaser = ChaserAgent->GetActorLocation();
		UE_LOG(LogPursuitAI, Log,
			TEXT("PursuitCharEnv: STAGE4 PROBE label=%s chaser=(%.1f, %.1f, %.1f) yaw=%.1f evader=(%.1f, %.1f) ndim=%d obs=[%s]"),
			Sample.Label,
			ActualChaser.X, ActualChaser.Y, ActualChaser.Z,
			ChaserAgent->GetActorRotation().Yaw,
			EvaderAgent->GetActorLocation().X, EvaderAgent->GetActorLocation().Y,
			Point->Values.Num(), *Values);

		if (Sample.bParkEvaderAhead)
		{
			PlaceAgent(EvaderAgent, EvaderStart, EvaderStartYaw);
		}
	}

	// Put the episode back exactly as it was, and drop the differenced-velocity history the sweep
	// just accumulated: BeginEpisode clears it so step 1 of an episode reports tgt_spd = 0.0, and a
	// sweep that left the sensor mid-pair would quietly break that.
	PlaceAgent(ChaserAgent, ChaserStart, ChaserStartYaw);
	if (ChaserAgent->TargetSensor)
	{
		ChaserAgent->TargetSensor->ResetVelocityHistory();
	}
	// The sweep teleported the chaser, so the Stage 4 crossing classifier must not difference the
	// next live step against the last sweep position: reset the previous-X to where the chaser
	// actually is now. Without this a sweep would inject a phantom wall crossing into episode 1.
	Stage4PrevChaserX = ChaserStart.X - ArenaCentre.X;
	Stage4PrevEvaderX = EvaderStart.X - ArenaCentre.X;
	Stage4PrevJumpCount = ChaserAgent->JumpActuator ? ChaserAgent->JumpActuator->GetJumpCount() : 0;

	UE_LOG(LogPursuitAI, Log,
		TEXT("PursuitCharEnv: STAGE4 PROBESWEEP end - chaser restored to (%.1f, %.1f, %.1f) yaw=%.1f, evader to (%.1f, %.1f); episode 1 continues unchanged"),
		ChaserStart.X, ChaserStart.Y, ChaserStart.Z, ChaserStartYaw,
		EvaderStart.X, EvaderStart.Y);
}

void APursuitCharEnv::SyncJumpGate()
{
	// Push bEnableAgentJump down onto every body that owns a jump actuator. Called once
	// per episode, after SpawnAgents, rather than only in BeginPlay: a level, a watched
	// run or a trainer option can change the switch between episodes, and a stale gate
	// would present as "jump is disabled but the dog still hopped".
	const bool bEnabled = bEnableAgentJump;
	auto ApplyGate = [bEnabled](APursuitCharAgent* Agent)
	{
		if (Agent && Agent->JumpActuator)
		{
			Agent->JumpActuator->bEnabled = bEnabled;
		}
	};
	ApplyGate(ChaserAgent);
	ApplyGate(EvaderAgent);
	ApplyGate(SupportAgent);
}

void APursuitCharEnv::NeutralizeStraySpectator()
{
	const UWorld* World = GetWorld();
	APlayerController* LocalController = GEngine ? GEngine->GetFirstLocalPlayerController(World) : nullptr;
	APawn* Spectator = LocalController ? LocalController->GetPawn() : nullptr;

	if (!Spectator || Spectator == ChaserAgent || Spectator == EvaderAgent || Spectator == SupportAgent)
	{
		return;
	}

	// Collision is the part that matters - a hidden pawn still blocks. The hide is repeated
	// here because the watched-run block in Tick only runs when a watch camera exists.
	Spectator->SetActorEnableCollision(false);
	if (!Spectator->IsHidden())
	{
		Spectator->SetActorHiddenInGame(true);
	}
}

void APursuitCharEnv::TallyWallProbes(const FBoxPoint* SensorBox)
{
	if (!SensorBox || SensorBox->Values.Num() < 5)
	{
		// nullptr is "no reading" (see ResolveTargetSensorPoint), not "all clear" - so it
		// must not be counted, and it must not silently look like a clean stage either.
		return;
	}

	const int32 NumValues = SensorBox->Values.Num();
	const int32 ProbeCount = FMath::Max((NumValues - 5) / 2, 0);
	const int32 ClearanceBase = 5 + ProbeCount;

	for (int32 Index = 5; Index < 5 + ProbeCount; ++Index)
	{
		const float Range = SensorBox->Values[Index];
		if (Range >= 0.999f)
		{
			continue;
		}

		++WallProbeHitsThisEpisode;
		// Track the closest reading seen, so a single near miss at the edge of the cone
		// is still visible in the summary.
		if (Range < ClosestWallProbeThisEpisode)
		{
			ClosestWallProbeThisEpisode = Range;
		}

		const int32 ClearIndex = (Index - 5) + ClearanceBase;
		if (ClearIndex < NumValues)
		{
			// Clearance -> height (1 - h/400); a blocker over 100 cm is past the ~90 cm
			// jump apex and needs routing, which is the case worth counting separately.
			const float HeightCm = (1.0f - SensorBox->Values[ClearIndex]) * 400.0f;
			if (HeightCm >= 100.0f)
			{
				++HopBlockedReadingsThisEpisode;
			}
		}
	}
}

void APursuitCharEnv::ResetEpisodeValidationState()
{
	EpisodeStartDistance = 0.0f;
	ClosestDistanceThisEpisode = TNumericLimits<float>::Max();
	ValidationSampleCount = 0;
	GroundedSampleCount = 0;
	SpeedSumCmPerSec = 0.0;
	DotVelocityToTargetSum = 0.0;
	// The wall-hit IGNORED counters are deliberately NOT reset here - they are a run-level
	// census, not a per-episode metric. See HandleChaserHit for the measurement that made
	// a per-episode window useless (it loses the one evader contact that actually occurs).
	//
	// The Stage 3 attribution counters ARE per-episode, because they are read per episode
	// beside this episode's `wall_hits_counted` and per-episode comparisons are the whole
	// point of the two-group report.
	Stage3PillarHitsThisEpisode = 0;
	Stage3OtherWallHitsThisEpisode = 0;
	// The Stage 3B contact-duration pair, reset on the same boundary as the attribution
	// counters above and for the same reason: they are read per episode, beside that
	// episode's `samples`, so a stale value from the previous episode would silently
	// corrupt the duration fraction.
	WallHitStepsThisEpisode = 0;
	Stage3PillarHitStepsThisEpisode = 0;
	bStage3PillarHitThisStep = false;
	bWallContactStepLatch = false;

	// Stage 4A jump / crossing diagnostics. Same boundary as the counters above and for the same
	// reason: every one of them is compared PER EPISODE between the two groups, so a value left
	// over from the previous episode would silently be attributed to this one.
	//
	// The three "previous" values are seeded from the agents as they now stand, which is
	// deliberate: this function is called AFTER the pair has been placed, so the first sample of
	// the episode differences against a real position rather than against zero. Seeding them at
	// 0 would make the first sample look like a crossing for whichever agent happens to spawn on
	// the positive side - a phantom over-wall crossing in the no-jump group, i.e. exactly the
	// kind of noise this stage cannot afford.
	JumpRequestStepsThisEpisode = 0;
	AirborneStepsThisEpisode = 0;
	MaxJumpApexCmThisEpisode = 0.0f;
	WallCrossOverCount = 0;
	WallCrossAroundCount = 0;
	MeaninglessJumpCount = 0;
	LandingCountThisEpisode = 0;
	ChaseResumedAfterLandingCount = 0;
	EvaderWallCrossCount = 0;
	bStage4WasAirborneLastStep = false;
	bStage4HasBeenGrounded = false;
	Stage4SettleStepsSkipped = 0;
	Stage4LandingWindowRemaining = 0;
	Stage4LandingChaseResumed = false;
	Stage4DistanceAtLastLandingCm = 0.0f;
	Stage4PrevChaserX = ChaserAgent ? ChaserAgent->GetActorLocation().X : 0.0f;
	Stage4PrevEvaderX = EvaderAgent ? EvaderAgent->GetActorLocation().X : 0.0f;
	Stage4PrevJumpCount = (ChaserAgent && ChaserAgent->JumpActuator)
		? ChaserAgent->JumpActuator->GetJumpCount()
		: 0;
	EvaderJumpCountAtEpisodeStart = (EvaderAgent && EvaderAgent->JumpActuator)
		? EvaderAgent->JumpActuator->GetJumpCount()
		: 0;
}

void APursuitCharEnv::AccumulateValidationSample(float DistanceCm)
{
	if (!ChaserAgent)
	{
		return;
	}

	++ValidationSampleCount;

	// Contact-DURATION accounting (Stage 3B). This is the ONE per-step hook that BOTH the
	// training path (Step_Implementation) and the watched path (RunWatchedEpisode) call, so
	// counting here is what makes `wall_hit_steps` comparable to the `samples` counter
	// incremented on the line above - same boundary, same steps, by construction. Placed in
	// Step_Implementation first, it read a hard zero on every watched run.
	//
	// One control step is counted ONCE, however many physics ticks inside it reported a
	// contact. That is the entire difference from `wall_hits_counted`, which counts those
	// ticks as separate events and is therefore unbounded. Only this step count can be
	// divided by `samples` to yield a duration fraction.
	//
	// The latches are cleared HERE so a watched run cannot leave one set for the rest of an
	// episode. `bWallHitThisStep` - the reward's own latch - is deliberately not touched:
	// clearing that here would silently stop the wall-hit penalty ever being charged.
	if (bWallContactStepLatch)
	{
		++WallHitStepsThisEpisode;
		bWallContactStepLatch = false;
	}
	if (bStage3PillarHitThisStep)
	{
		++Stage3PillarHitStepsThisEpisode;
		bStage3PillarHitThisStep = false;
	}

	// Stage 4A jump / crossing evidence, on this same shared hook and for exactly the reason
	// spelled out in the comment above: the watched path never calls Step_Implementation, so
	// anything counted there reads a hard zero in every oracle run - and the oracle run is
	// precisely where this stage's environment-acceptance evidence comes from. The counter that
	// would have measured the jump would have been blind in the one run that decides whether
	// the jump is testable at all.
	if (bStage4WallLayout)
	{
		AccumulateStage4Sample(DistanceCm);
	}

	if (DistanceCm < ClosestDistanceThisEpisode)
	{
		ClosestDistanceThisEpisode = DistanceCm;
	}

	const UCharacterMovementComponent* Movement = ChaserAgent->GetCharacterMovement();
	if (Movement && Movement->IsMovingOnGround())
	{
		++GroundedSampleCount;
	}

	const FVector Velocity = ChaserAgent->GetVelocity();
	const float SpeedCmPerSec = Velocity.Size2D();
	SpeedSumCmPerSec += SpeedCmPerSec;

	// Wall-probe liveness on the WATCHED path. A watched run has no policy consuming the
	// observation, so nothing else Observes for it - and Stage 0's acceptance runs are
	// watched runs, so a probe tally that only lived on the training path would leave this
	// metric unmeasured in exactly the runs the Go/No-Go is based on.
	//
	// Gated on DriveMode so the training path is untouched: Step_Implementation Observes
	// this same step and feeds the tally from that reading, and Observing a second time
	// would corrupt the sensor's differenced target-speed history.
	if (DriveMode != EPursuitCharDriveMode::Train)
	{
		FInstancedStruct WatchedObservation;
		IAgent::Execute_Observe(ChaserAgent, WatchedObservation);
		TallyWallProbes(ResolveTargetSensorPoint(WatchedObservation));
	}

	// Direction agreement: cos between the velocity and the bearing to the target.
	// +1 = running straight at it, -1 = running away, ~0 = circling. The greedy baseline
	// should sit near +1 on this; if it does not, either the steering or this metric is
	// broken, and both are worth knowing before a policy number is compared against it.
	if (EvaderAgent)
	{
		const FVector ToTarget =
			(EvaderAgent->GetActorLocation() - ChaserAgent->GetActorLocation()).GetSafeNormal2D();
		if (!ToTarget.IsNearlyZero())
		{
			const FVector Direction = Velocity.GetSafeNormal2D();
			if (!Direction.IsNearlyZero())
			{
				DotVelocityToTargetSum += FVector::DotProduct(Direction, ToTarget);
			}
		}
	}

	// Evader motion, sampled on exactly the same frames as the chaser's numbers above, so
	// "the chaser closed 200 cm while the target ran 480 cm" is a like-for-like statement
	// and not two different windows. Path length accumulates |delta| per sample, which is
	// what makes an evader that circles back to its spawn still measure as moving.
	if (EvaderAgent)
	{
		const FVector EvaderNow = EvaderAgent->GetActorLocation();
		EvaderPathLengthCm += static_cast<float>(FVector::Dist2D(EvaderPrevLocation, EvaderNow));
		EvaderPrevLocation = EvaderNow;

		const float EvaderSpeedCmPerSec = EvaderAgent->GetVelocity().Size2D();
		EvaderSpeedSumCmPerSec += EvaderSpeedCmPerSec;
		EvaderMaxSpeedCmPerSec = FMath::Max(EvaderMaxSpeedCmPerSec, EvaderSpeedCmPerSec);
	}
}

void APursuitCharEnv::LogEpisodeValidationSummary(const TCHAR* ResultKind, float EndDistance)
{
	const int32 Samples = FMath::Max(ValidationSampleCount, 1);
	const int32 Jumps = (ChaserAgent && ChaserAgent->JumpActuator)
		? ChaserAgent->JumpActuator->GetJumpCount() - JumpCountAtEpisodeStart
		: 0;

	UE_LOG(LogPursuitAI, Log,
		TEXT("PursuitCharEnv: episode %d VALIDATION result=%s start_d=%.0f end_d=%.0f closest_d=%.0f ")
		TEXT("grounded=%.2f mean_speed=%.0f dot_vel_target=%.2f jumps=%d ")
		TEXT("wall_hits_counted=%d ignored_total[floor=%d support=%d target=%d self=%d] samples=%d ")
		TEXT("probe_non_clear=%d probe_blocked=%d probe_closest=%.3f"),
		EpisodeIndex, ResultKind,
		EpisodeStartDistance, EndDistance, ClosestDistanceThisEpisode,
		static_cast<float>(GroundedSampleCount) / static_cast<float>(Samples),
		static_cast<float>(SpeedSumCmPerSec / static_cast<double>(Samples)),
		static_cast<float>(DotVelocityToTargetSum / static_cast<double>(Samples)),
		Jumps,
		WallHitCount, WallHitIgnoredFloorCount, WallHitIgnoredSupportCount,
		WallHitIgnoredTargetCount, WallHitIgnoredSelfCount, ValidationSampleCount,
		WallProbeHitsThisEpisode, HopBlockedReadingsThisEpisode, ClosestWallProbeThisEpisode);

	// The evader's own motion, on its OWN line rather than appended to the VALIDATION
	// line above. Two reasons: appending would change the shape of the one line every
	// acceptance parser in tools/ already anchors on, and this measurement has to be
	// read in runs where the chaser's line is not the subject (Stage 2A asks "did the
	// TARGET move", which is not a property of the chaser at all).
	//
	// `static=yes` with disp=path=0 is the Stage 0/1 result; `static=no` with a non-zero
	// path is the Stage 2A one. ratio is the level-baked EvaderSpeedRatio, so the
	// readback is visible in every single episode of the run, not only at bake time.
	if (EvaderAgent)
	{
		UE_LOG(LogPursuitAI, Log,
			TEXT("PursuitCharEnv: episode %d EVADER disp=%.0f path=%.0f mean_speed=%.0f max_speed=%.0f ")
			TEXT("ratio=%.2f static=%s start=(%.0f, %.0f) end=(%.0f, %.0f)"),
			EpisodeIndex,
			static_cast<float>(FVector::Dist2D(EvaderEpisodeStartLocation, EvaderAgent->GetActorLocation())),
			EvaderPathLengthCm,
			static_cast<float>(EvaderSpeedSumCmPerSec / static_cast<double>(Samples)),
			EvaderMaxSpeedCmPerSec,
			EvaderSpeedRatio,
			bStaticTarget ? TEXT("yes") : TEXT("no"),
			EvaderEpisodeStartLocation.X, EvaderEpisodeStartLocation.Y,
			EvaderAgent->GetActorLocation().X, EvaderAgent->GetActorLocation().Y);
	}

	// Stage 3 attribution line, on its own line for the same reason as the EVADER one: the
	// VALIDATION line's shape is what the existing tools anchor on, and this stage's report
	// needs "of the counted contacts, how many were the pillar" per episode.
	if (bStage3PillarLayout)
	{
		UE_LOG(LogPursuitAI, Log,
			TEXT("PursuitCharEnv: episode %d STAGE3RESULT layout=%s pillar_hits=%d other_wall_hits=%d wall_hits_counted=%d probe_non_clear=%d samples=%d wall_hit_steps=%d pillar_hit_steps=%d"),
			EpisodeIndex,
			bStage3BlockedLayout ? TEXT("blocked") : TEXT("clear"),
			Stage3PillarHitsThisEpisode, Stage3OtherWallHitsThisEpisode,
			WallHitCount, WallProbeHitsThisEpisode, ValidationSampleCount,
			WallHitStepsThisEpisode, Stage3PillarHitStepsThisEpisode);
	}

	// Stage 4A result line, on its own line for the same reason as the Stage 3 one: the
	// VALIDATION line's shape is what the existing tools anchor on, and this stage needs its
	// jump evidence readable per episode AND per group.
	//
	// Two tokens need reading carefully and are therefore named explicitly in the line:
	//
	//   jump_intent_channel - `on` once any action has been resolved through the policy path.
	//       `jump_request_steps` is only meaningful when this reads `on`: the scripted oracle
	//       drives the actuator DIRECTLY (see SteerScriptedAgent), so it never sets the reward's
	//       jump-request latch and a watched run legitimately reports 0 request steps beside a
	//       non-zero `jumps`. Without this token those two numbers look contradictory.
	//   settle_steps_skipped - steps between placement and the first grounded frame, excluded
	//       from every Stage 4A measurement (see AccumulateStage4Sample). Logged so the gate is
	//       auditable instead of invisible.
	if (bStage4WallLayout)
	{
		const int32 EvaderJumps = (EvaderAgent && EvaderAgent->JumpActuator)
			? EvaderAgent->JumpActuator->GetJumpCount() - EvaderJumpCountAtEpisodeStart
			: 0;
		UE_LOG(LogPursuitAI, Log,
			TEXT("PursuitCharEnv: episode %d STAGE4RESULT layout=%s wall_height=%.0f jump_gate=%s jump_intent_channel=%s samples=%d jumps=%d jump_request_steps=%d airborne_steps=%d max_apex_cm=%.1f wall_cross_over=%d wall_cross_around=%d meaningless_jumps=%d landings=%d chase_resumed_after_landing=%d probe_non_clear=%d probe_blocked=%d settle_steps_skipped=%d evader_cross=%d evader_jumps=%d"),
			EpisodeIndex,
			bStage4BlockedLayout ? TEXT("must_jump") : TEXT("no_jump"),
			Stage4WallHeightCm,
			bEnableAgentJump ? TEXT("on") : TEXT("off"),
			bJumpIntentResolved ? TEXT("on") : TEXT("absent"),
			ValidationSampleCount, Jumps, JumpRequestStepsThisEpisode,
			AirborneStepsThisEpisode, MaxJumpApexCmThisEpisode,
			WallCrossOverCount, WallCrossAroundCount, MeaninglessJumpCount,
			LandingCountThisEpisode, ChaseResumedAfterLandingCount,
			WallProbeHitsThisEpisode, HopBlockedReadingsThisEpisode, Stage4SettleStepsSkipped,
			EvaderWallCrossCount, EvaderJumps);
	}

	// Flush the log NOW (Stage 3B). Reason, measured on the Stage 3A zero-shot run: the
	// evaluator terminates the process as soon as it has collected its last episode, and the
	// final episode's VALIDATION / EVADER / STAGE3RESULT lines were still in the async file
	// writer's buffer when it died - that log ended mid-episode-101 with 101 layout lines but
	// only 100 summaries, which is exactly why the last episode's hit counters had to be
	// reported as a lower bound. Flushing inside the call that PRODUCES those lines wins the
	// race by construction: the evaluator cannot know the episode finished until this step's
	// result reaches it, so the bytes are on disk before any kill can be requested.
	//
	// Logging-only: this touches no state the simulation or the reward can observe.
	if (GLog)
	{
		GLog->Flush();
	}
}

void APursuitCharEnv::NoteEpisodeCompleted(bool bWasCaught)
{
	++CompletedEpisodeCount;

	if (MaxEpisodes <= 0 || CompletedEpisodeCount < MaxEpisodes || bValidationExitRequested)
	{
		return;
	}

	// The exact-episode-count exit. RequestExit rather than a kill, the same mechanism
	// APursuitPlayGameMode uses for its self-terminating headless runs. QuitAfterSeconds
	// remains the anti-hang net; this is what makes "exactly N completed episodes" true
	// by construction instead of a wall-clock estimate.
	bValidationExitRequested = true;
	UE_LOG(LogPursuitAI, Log,
		TEXT("PursuitCharEnv: VALIDATION COMPLETE - %d of %d requested episodes finished (last=%s), requesting exit"),
		CompletedEpisodeCount, MaxEpisodes, bWasCaught ? TEXT("CAUGHT") : TEXT("TIMEOUT"));
	FGenericPlatformMisc::RequestExit(false);
}
