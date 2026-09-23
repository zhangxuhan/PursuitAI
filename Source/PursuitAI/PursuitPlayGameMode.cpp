// Copyright Epic Games, Inc. All Rights Reserved.

#include "PursuitPlayGameMode.h"

#include "PursuitAI.h"
#include "PursuitAIEnv.h"
#include "PursuitCharacter.h"
#include "PursuitCharAgent.h"
#include "PursuitCharEnv.h"
#include "PursuitJumpCrate.h"

#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "EngineUtils.h"
#include "UObject/Class.h"
#include "Components/StaticMeshComponent.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/Controller.h"
#include "GameFramework/DefaultPawn.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerStart.h"
#include "Kismet/GameplayStatics.h"
#include "HAL/PlatformMisc.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

#include "DrawDebugHelpers.h"

APursuitPlayGameMode::APursuitPlayGameMode()
{
	// The player is the same pawn the chasers are, just driven by a person instead of by
	// TickChaser. One class, two roles, and no second thing to keep in sync.
	DefaultPawnClass = APursuitCharacter::StaticClass();

	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = true;

	bDrawHud = !FParse::Param(FCommandLine::Get(), TEXT("PursuitNoHud"));
	bDrawChaserLabels = bDrawHud;

	// Off unless asked for. It exists to watch the scene drive itself - for a recording, or to
	// check the animation states without holding WASD down for a minute - and a person who
	// wants to play does not want the game playing itself.
	bAutoPlay = FParse::Param(FCommandLine::Get(), TEXT("PursuitAutoPlay"));

	// A deterministic obstacle test rather than a game. See the property comment: the chase
	// is chaotic enough that "did the AI jump" is not answerable from a normal run.
	bJumpTest = FParse::Param(FCommandLine::Get(), TEXT("PursuitJumpTest"));

	// -PursuitEnvBox turns this mode into a host for APursuitAIEnv on whatever map it is
	// launched on - Demonstration included. Parsed in the constructor because the default
	// pawn class has to be decided before the engine spawns the player: the env hides and
	// de-collides whatever pawn exists, but an APursuitCharacter would still run its camera
	// resolution and its Tick for the whole session, and a grey ball costs nothing.
	bEnvBox = FParse::Param(FCommandLine::Get(), TEXT("PursuitEnvBox"));
	if (bEnvBox)
	{
		DefaultPawnClass = ADefaultPawn::StaticClass();
	}

	// The v2 character environment's host switch: same idea and same reasoning as
	// -PursuitEnvBox, but the spawned trio is the char env plus its two agents. The
	// rig stays off under this switch; -PursuitStage=city on the env side turns the
	// city map itself into the stage.
	bCharEnvBox = FParse::Param(FCommandLine::Get(), TEXT("PursuitCharEnvBox"));
	if (bCharEnvBox)
	{
		DefaultPawnClass = ADefaultPawn::StaticClass();
	}

	// Schola-hosted training: the trainer spawns the environment itself over gRPC, and this
	// game mode must keep its hands off the world entirely.
	//
	// Why this exists (found 2026-09-22, after it had been poisoning every run):
	//   The training .bat launches Schola on Demonstration_Train, whose GameMode is this
	//   class. It passes neither -PursuitEnvBox nor -PursuitCharEnvBox - it has no reason
	//   to - so BeginPlay fell through to SpawnChasers() and the SCRIPTED chaser pair ran
	//   concurrently with the training env for the entire run, catching the evader over and
	//   over. Two consequences, both bad:
	//     1. The log became unreadable: a naive `grep -c "caught by"` read 242 (and 31 on
	//        another run) while the training environment's own count was 0. That produced a
	//        wrong "breakthrough" conclusion before the prefixes were separated.
	//     2. Those scripted pawns walk the same plaza the policy trains on, so the world
	//        state the policy sees is not the clean 1-chaser-vs-1-evader task.
	//
	// Detection: Schola always passes -ScholaDisableScript and -ScholaPort=<n> when it
	// drives a simulator. Watched runs (-game) and PIE never do. That is a free, exact
	// discriminator - no new switch to remember, and it cannot misfire on a play session.
	//
	// Note -PursuitCharEnvBox is deliberately NOT the fix here: it spawns its OWN env plus
	// agents, which would then fight the trainer's env for the same job.
	bScholaHosted = FParse::Param(FCommandLine::Get(), TEXT("ScholaDisableScript"));
	if (bScholaHosted)
	{
		DefaultPawnClass = ADefaultPawn::StaticClass();
		UE_LOG(LogPursuitAI, Log,
			TEXT("PursuitPlay: Schola-hosted run detected - skipping chaser spawn and all ")
			TEXT("play-mode setup so the trainer owns the world"));
	}

	// Second chaser in a char-env-box run, opt-in only. Default off: the scripted
	// soldier stood still in watched runs and read as a broken prop (2026-09-21).
	bSpawnSupportAgent = FParse::Param(FCommandLine::Get(), TEXT("PursuitSupportAgent"));

	// Self-terminating variant, for verification runs that must not leave an editor process
	// alive: an UnrealEditor.exe still in memory makes the next UBT build fail instantly with
	// "Unable to build while Live Coding is active", which reads as a compile error and is
	// nothing of the sort.
	if (bJumpTest)
	{
		FParse::Value(FCommandLine::Get(), TEXT("PursuitJumpTestSeconds="), JumpTestSeconds);
	}

	// The same self-quit for ordinary headless runs - autoplay, avoidance checks, anything
	// scripted. Without it a headless `-game` run has to be killed from outside, and an
	// UnrealEditor.exe left in memory makes the next UBT build fail in three seconds with
	// "Unable to build while Live Coding is active" - which reads as a compile error and is
	// nothing of the sort. Having the process end itself removes that whole failure mode.
	FParse::Value(FCommandLine::Get(), TEXT("PursuitRunSeconds="), RunSeconds);

	// Pin the player's spawn from the command line: -PursuitPlayerAt=x,y,z.
	//
	// Added for the height gate on the catch, and it is worth being explicit about why a
	// switch was needed rather than a run. The branch that gate adds only executes when the
	// player is standing somewhere the chasers are not, and no ordinary run can be relied on
	// to put them there - the self-playing player never climbs, so a headless A/B of that
	// branch measured nothing at all. Pinning the spawn turns "I think the platform protects
	// you now" into two runs that differ in one flag.
	//
	// It sets bUseConfiguredSpawn as well, deliberately: a pinned point that the spawn path
	// then ignores would be a switch that silently does nothing.
	FString PlayerAtText;
	// bShouldStopOnSeparator = false, and that argument is the entire trick.
	//
	// FParse::Value's default terminator set is ",) \r\n\t" - UE reserves commas for
	// command-line lists - so the default form silently truncates "900,2120,400" to "900" and
	// the switch does nothing at all. That is not hypothetical: it is what the first run of
	// this switch did, and the only reason it was caught in one run is the warning below.
	// Anything that wants a comma inside a command-line value has to turn this off.
	if (FParse::Value(FCommandLine::Get(), TEXT("PursuitPlayerAt="), PlayerAtText, false))
	{
		TArray<FString> Parts;
		PlayerAtText.ParseIntoArray(Parts, TEXT(","), true);
		if (Parts.Num() == 3)
		{
			PlayerSpawnLocation = FVector(
				FCString::Atof(*Parts[0]), FCString::Atof(*Parts[1]), FCString::Atof(*Parts[2]));
			bUseConfiguredSpawn = true;
		}
		else
		{
			// Refusing a malformed value out loud rather than pinning the player to the world
			// origin: a spawn at (0,0,0) under a building would read as "the map is broken".
			UE_LOG(LogPursuitAI, Warning,
				TEXT("PursuitPlay: -PursuitPlayerAt wants x,y,z - got '%s', ignoring it"),
				*PlayerAtText);
		}
	}
}

void APursuitPlayGameMode::BeginPlay()
{
	Super::BeginPlay();

	// Schola is driving: the trainer spawns and owns the environment, so this mode must not
	// place a player, spawn chasers, or build the jump rig. Doing nothing is the whole job.
	// See the constructor for why this switch is keyed off -ScholaDisableScript.
	if (bScholaHosted)
	{
		return;
	}

	// Env-box stage: spawn the environment and leave. No chasers, no player-side logic, no
	// play HUD - the env claims the view target, drives its own camera and draws its own
	// overlay, and everything this mode normally does would only fight it for the frame.
	// The env parses its own switches (-PursuitStage, -PursuitArenaAt, drive mode, seed),
	// so this side has nothing to configure: one actor, default spawn transform.
	if (bEnvBox)
	{
		FActorSpawnParameters EnvParams;
		EnvParams.SpawnCollisionHandlingOverride =
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		APursuitAIEnv* Env = GetWorld()->SpawnActor<APursuitAIEnv>(
			APursuitAIEnv::StaticClass(), FTransform::Identity, EnvParams);
		if (Env)
		{
			UE_LOG(LogPursuitAI, Log,
				TEXT("PursuitPlay: env-box stage - spawned %s, play-mode setup skipped"),
				*Env->GetName());
		}
		else
		{
			UE_LOG(LogPursuitAI, Error,
				TEXT("PursuitPlay: -PursuitEnvBox given but the env failed to spawn"));
		}
		return;
	}

	// Char-env-box stage: agents first, then the env DEFERRED, because the env's
	// BeginPlay (BindHitEvents, LoadChaserPolicy, watched view) reads the
	// ChaserAgent/EvaderAgent references - a plain SpawnActor would run BeginPlay
	// before the wiring and the wall-hit counter would silently never bind.
	if (bCharEnvBox)
	{
		// A LEVEL THAT CARRIES ITS OWN ENV WINS. This is the portfolio stage's requirement, and
		// it fixes a real defect rather than adding a feature.
		//
		// The generated levels bake their contract INTO the env actor they place: the arena
		// flags, every reward term, the spawn-separation band, EvaderSpeedRatio, the jump gate -
		// all UPROPERTYs on that actor, all read back and asserted by the generator after the
		// save. That is what "the level is the contract" means in this repository. This branch
		// used to ignore it completely: it spawned a SECOND env from CLASS DEFAULTS at the world
		// origin while the level's own env was still in the world running its own episodes, so a
		// watched run on such a level had two environments, several agents, and neither
		// environment the one the level's sha256 describes.
		//
		// Found 2026-09-23 while building the portfolio level, whose entire point is "Moving050's
		// contract, city backdrop". A watched run that silently used class defaults instead of
		// the baked actor would have measured a different task from the one the level names -
		// and would have looked perfectly healthy while doing it.
		APursuitCharEnv* LevelEnv = nullptr;
		for (TActorIterator<APursuitCharEnv> It(GetWorld()); It; ++It)
		{
			LevelEnv = *It;
			break;
		}
		if (LevelEnv)
		{
			LevelEnv->bHostedWatchRun = true;
			UE_LOG(LogPursuitAI, Log,
				TEXT("PursuitPlay: char-env-box stage - the LEVEL already carries %s at %s ")
				TEXT("(chaser=%s evader=%s); no second env spawned - its baked settings are the ")
				TEXT("contract for this run"),
				*LevelEnv->GetName(), *LevelEnv->GetActorLocation().ToString(),
				LevelEnv->ChaserAgent ? *LevelEnv->ChaserAgent->GetName() : TEXT("<none>"),
				LevelEnv->EvaderAgent ? *LevelEnv->EvaderAgent->GetName() : TEXT("<none>"));
			if (!LevelEnv->ChaserAgent || !LevelEnv->EvaderAgent)
			{
				// Fail loudly. An unwired env would run zero episodes and read as "the policy
				// learned nothing", which is the wrong conclusion from the wrong evidence.
				UE_LOG(LogPursuitAI, Error,
					TEXT("PursuitPlay: the level's %s has no chaser/evader wired - a watched run ")
					TEXT("would have no agents to drive. Fix the LEVEL (the gen_char_*.py scripts ")
					TEXT("wire ChaserAgent/EvaderAgent and verify it) - do not read this as a ")
					TEXT("policy result"),
					*LevelEnv->GetName());
			}
			return;
		}

		FActorSpawnParameters AgentParams;
		AgentParams.SpawnCollisionHandlingOverride =
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

		APursuitCharAgent* Chaser = GetWorld()->SpawnActor<APursuitCharAgent>(
			APursuitCharAgent::StaticClass(), FTransform::Identity, AgentParams);
		APursuitCharAgent* Evader = GetWorld()->SpawnActor<APursuitCharAgent>(
			APursuitCharAgent::StaticClass(), FTransform::Identity, AgentParams);

		// The watched run's second chaser - the RPGHero soldier. Opt-in via
		// -PursuitSupportAgent (default off, user request 2026-09-21): the env treats
		// a null SupportAgent as "no second chaser", so skipping the spawn degrades
		// nothing - catch and scoring only ever read the dog.
		APursuitCharAgent* Support = bSpawnSupportAgent
			? GetWorld()->SpawnActor<APursuitCharAgent>(
				APursuitCharAgent::StaticClass(), FTransform::Identity, AgentParams)
			: nullptr;

		APursuitCharEnv* Env = GetWorld()->SpawnActorDeferred<APursuitCharEnv>(
			APursuitCharEnv::StaticClass(), FTransform::Identity, nullptr, nullptr,
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
		if (Env && Chaser && Evader)
		{
			Env->bHostedWatchRun = true;
			Env->ChaserAgent = Chaser;
			Env->EvaderAgent = Evader;
			Env->SupportAgent = Support;
			Env->FinishSpawning(FTransform::Identity);
			UE_LOG(LogPursuitAI, Log,
				TEXT("PursuitPlay: char-env-box stage - spawned env %s with agents %s / %s / support %s"),
				*Env->GetName(), *Chaser->GetName(), *Evader->GetName(),
				Support ? *Support->GetName() : TEXT("<none>"));
		}
		else
		{
			UE_LOG(LogPursuitAI, Error,
				TEXT("PursuitPlay: -PursuitCharEnvBox given but the env/agents failed to spawn"));
		}
		return;
	}

	// Resolve where the player appears. This is pure scene/character initialisation: it decides
	// a spawn point and the spawn conditions, and it never touches APursuitAIEnv or Schola - no
	// training is started, only the actors are placed. (Schola training runs on a different map
	// with its own game mode and is launched separately.)
	StartLocation = ResolvePlayerSpawn();

	SpawnChasers();

	// The jump test's layout is NOT built here - see SetupJumpTest. At BeginPlay the player
	// pawn has not been spawned yet, so there is nothing to place on the far side of the crate.
	// Tick calls it on the first frame a pawn exists.

	// Honour a pinned spawn point. The engine may already have dropped the player pawn on the
	// level's PlayerStart; if the user pinned a location we move them there so the configured
	// point wins regardless of where the map's PlayerStart sits. This is the "spawn condition"
	// made explicit: the player ends up exactly where the configuration says.
	if (bUseConfiguredSpawn)
	{
		if (APawn* Player = UGameplayStatics::GetPlayerPawn(this, 0))
		{
			Player->SetActorLocation(StartLocation, false, nullptr, ETeleportType::TeleportPhysics);
		}
	}

	UE_LOG(LogPursuitAI, Log,
		TEXT("PursuitPlay: %d chaser(s) spawned around player start %s, hud %s, autoplay %s, ")
		TEXT("chaser rigs [%s / %s]"),
		Chasers.Num(), *StartLocation.ToString(), bDrawHud ? TEXT("on") : TEXT("off"),
		bAutoPlay ? TEXT("on") : TEXT("off"),
		*StaticEnum<EPursuitHeroModel>()->GetNameStringByValue((int64)ChaserHeroA),
		*StaticEnum<EPursuitHeroModel>()->GetNameStringByValue((int64)ChaserHeroB));
}

FVector APursuitPlayGameMode::ResolvePlayerSpawn()
{
	// A pinned point always wins. Used on hand-built maps where the level's PlayerStart belongs
	// to the map's own demo and should not be moved.
	if (bUseConfiguredSpawn)
	{
		UE_LOG(LogPursuitAI, Log, TEXT("PursuitPlay: player spawn pinned to configured %s"),
			*PlayerSpawnLocation.ToString());
		return PlayerSpawnLocation;
	}

	// Otherwise use the level's PlayerStart. Collected rather than taken on first hit so a future
	// "spawn at the tagged one" is a one-line change; today the first wins.
	TArray<APlayerStart*> Starts;
	for (TActorIterator<APlayerStart> It(GetWorld()); It; ++It)
	{
		StartLocation = It->GetActorLocation();
		Starts.Add(*It);
	}

	if (Starts.Num() > 0)
	{
		UE_LOG(LogPursuitAI, Log, TEXT("PursuitPlay: player spawn from level PlayerStart (%d found) %s"),
			Starts.Num(), *StartLocation.ToString());
		return StartLocation;
	}

	// No PlayerStart in the level: fall back to the configured point rather than the world
	// origin, which on a city map is under a building or a kilometre from the action.
	UE_LOG(LogPursuitAI, Warning,
		TEXT("PursuitPlay: no PlayerStart in level - falling back to configured %s. ")
		TEXT("Add a PlayerStart, or set bUseConfiguredSpawn with a real point."),
		*PlayerSpawnLocation.ToString());
	return PlayerSpawnLocation;
}

void APursuitPlayGameMode::SpawnChasers()
{
	// Centred on the player's spawn when bCenterChasersOnPlayer is set (the default, and the
	// only thing that makes sense on a hand-built map where the start is not at the origin), and
	// on the world origin otherwise - which is where the generated arena used to be built. At
	// the player's Z so nobody spawns inside the floor.
	const FVector Centre = bCenterChasersOnPlayer
		? FVector(StartLocation.X, StartLocation.Y, StartLocation.Z)
		: FVector(0.0f, 0.0f, StartLocation.Z);

	// Catch distance is a judgement call - "it caught me from across the street" and "it has
	// to be on top of me" are both defensible, and which one a person means is not knowable
	// from a spec. So it is tunable per launch and read once here, outside the loop, rather
	// than rebuilt for every value tried. A non-positive value means "keep the class default".
	float CatchRadiusOverride = 0.0f;
	const bool bOverrideCatchRadius =
		FParse::Value(FCommandLine::Get(), TEXT("PursuitCatchRadius="), CatchRadiusOverride)
		&& CatchRadiusOverride > 0.0f;
	if (bOverrideCatchRadius)
	{
		UE_LOG(LogPursuitAI, Log, TEXT("PursuitPlay: catch radius overridden to %.0f cm"),
			CatchRadiusOverride);
	}

	// The vertical half of the same judgement, tunable for the same reason. Zero is a legal and
	// meaningful value here - it means "no height gate", i.e. the old height-blind catch - so
	// unlike the radius this parse accepts it rather than treating it as "unset". That is what
	// makes -PursuitCatchHeight=0 a one-flag A/B against the previous behaviour.
	float CatchHeightOverride = 0.0f;
	const bool bOverrideCatchHeight =
		FParse::Value(FCommandLine::Get(), TEXT("PursuitCatchHeight="), CatchHeightOverride)
		&& CatchHeightOverride >= 0.0f;
	if (bOverrideCatchHeight)
	{
		UE_LOG(LogPursuitAI, Log,
			TEXT("PursuitPlay: catch height tolerance overridden to %.0f cm%s"),
			CatchHeightOverride,
			CatchHeightOverride > 0.0f ? TEXT("") : TEXT(" (height gate off)"));
	}

	for (int32 Index = 0; Index < ChaserCount; ++Index)
	{
		// Quarter turn off axis, so with the default four they arrive from the corners and the
		// player never starts with one already behind them.
		const float AngleDegrees = 45.0f + (360.0f / FMath::Max(ChaserCount, 1)) * Index;
		const float Radians = FMath::DegreesToRadians(AngleDegrees);
		const FVector SpawnLocation = Centre + FVector(
			FMath::Cos(Radians) * ChaserSpawnRadius,
			FMath::Sin(Radians) * ChaserSpawnRadius,
			0.0f);
		const FTransform SpawnTransform(FRotator(0.0f, AngleDegrees + 180.0f, 0.0f), SpawnLocation);

		// Deferred, because both flags have to be set before the pawn finishes spawning: its
		// BeginPlay reads them to decide whether it is a camera-carrying player or a chasing AI.
		APursuitCharacter* Chaser = GetWorld()->SpawnActorDeferred<APursuitCharacter>(
			APursuitCharacter::StaticClass(), SpawnTransform, nullptr, nullptr,
			ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn);

		if (!Chaser)
		{
			UE_LOG(LogPursuitAI, Warning, TEXT("PursuitPlay: chaser %d failed to spawn"), Index + 1);
			continue;
		}

		Chaser->bIsChaser = true;
		if (bOverrideCatchRadius)
		{
			Chaser->CatchRadius = CatchRadiusOverride;
		}
		if (bOverrideCatchHeight)
		{
			Chaser->CatchHeightTolerance = CatchHeightOverride;
		}
		// Alternate the two configured pack heroes among the chasers: even indices get
		// ChaserHeroA, odd ones ChaserHeroB. The player keeps the Tiny Hero - the only rig that
		// ships jump clips - which is the protagonist. BeginPlay's ApplyHeroModel reads HeroModel
		// before it loads the mesh, so the change survives FinishSpawning.
		Chaser->HeroModel = (Index % 2 == 0) ? ChaserHeroA : ChaserHeroB;
		Chaser->AutoPossessAI = EAutoPossessAI::Disabled;
		Chaser->FinishSpawning(SpawnTransform);

		// Explicitly, rather than leaving it to AutoPossessAI. The controller is what makes the
		// movement component consume input at all, so a chaser without one stands perfectly
		// still and reads as broken AI rather than as a missing controller. Calling it here also
		// means the log below can prove it happened.
		Chaser->SpawnDefaultController();

		Chasers.Add(Chaser);
	}

	UE_LOG(LogPursuitAI, Log, TEXT("PursuitPlay: %d chaser(s) each possessed by %s"),
		Chasers.Num(),
		(Chasers.Num() > 0 && Chasers[0] && Chasers[0]->GetController())
			? *Chasers[0]->GetController()->GetClass()->GetName()
			: TEXT("<none - the chasers will not move>"));
}

void APursuitPlayGameMode::SetupJumpTest()
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	// Everything is laid out on a clean axis through the player's spawn, so the geometry is
	// reproducible run to run and a failure is a real failure rather than a bad roll.
	const FVector Base = StartLocation;
	const float Half = JumpTestSeparation * 0.5f;

	// Called from Tick rather than BeginPlay, and that is forced by the engine: a game mode's
	// BeginPlay runs before the player pawn has been spawned and possessed, so at BeginPlay
	// there is no player to place. The first version did look for one there, quietly found
	// none, and returned - leaving the player on its own PlayerStart, on top of the crate at
	// the centre, with the chasers still in their default ring. That run *looked* like a pass
	// at a glance, because a chaser that happened to end up beside the crate jumped it, but no
	// fixture had been built and the test was really measuring the ordinary chase.
	//
	// Tick, one frame later, has a pawn. Waiting costs one frame and removes the whole class of
	// "which half has already run" bugs that splitting this across two functions invited.

	APawn* Player = UGameplayStatics::GetPlayerPawn(this, 0);
	if (!Player)
	{
		// Not yet. Keep asking on later frames - the same race the visual paths handle in
		// EnsureViewTarget, for the same reason.
		return;
	}

	// Take down the crate from a previous round before building a new one. Without this a
	// rebuild (which ResetRound does every time the chaser catches the player) would stack a
	// fresh 90 cm crate on top of the old one for as long as the round lasted.
	if (JumpTestCrate.IsValid())
	{
		JumpTestCrate->Destroy();
		JumpTestCrate.Reset();
	}

	// One chaser, directly opposite, so the crate sits exactly on the straight line between
	// them. The others are parked rather than destroyed, so the pawn count and possession stay
	// the same as a normal round and nothing downstream has to know.
	//
	// Parked HIGH, and that detail is the whole reason this test reads correctly. Placed 4000
	// cm out at ground level - the first attempt - a parked chaser still had the crate between
	// itself and a player on the opposite side, because it walks straight at that player
	// regardless of distance, and any spot on the ground here is on somebody's line to
	// somebody. The log then reported the parked chasers jumping. Lifting them clear of the
	// level's collision entirely is what makes "parked" mean parked.
	//
	// The keeper is JumpTestChaserIndex, NOT index 0. Index 0 is the player's own pawn in some
	// spawn orders, and a fixture that keeps "the chaser that is not index 0" then parks every
	// chaser it has and measures nothing - which is one of the ways this test has already lied.
	const int32 Keeper = FMath::Clamp(JumpTestChaserIndex, 0, FMath::Max(Chasers.Num() - 1, 0));

	constexpr float ParkedZ = 200000.0f;
	APursuitCharacter* UnderTest = Chasers.IsValidIndex(Keeper) ? Chasers[Keeper] : nullptr;
	for (int32 Index = 0; Index < Chasers.Num(); ++Index)
	{
		APursuitCharacter* Chaser = Chasers[Index];
		if (!Chaser)
		{
			continue;
		}

		// The one under test goes on the axis through the player; the rest go far enough out
		// that their separation push cannot reach it. The push is 240 cm, so 4000 is not a
		// margin - it is off the map, and that is deliberate.
		const FVector Spot = (Chaser == UnderTest)
			? FVector(Base.X, Base.Y + Half, Base.Z)
			: FVector(Base.X + JumpTestParkOffset + Index * 400.0f, Base.Y, ParkedZ);

		Chaser->SetActorLocation(Spot, false, nullptr, ETeleportType::TeleportPhysics);
		if (UCharacterMovementComponent* Move = Chaser->GetCharacterMovement())
		{
			Move->StopMovementImmediately();
		}
	}

	// The player, on the far side. Held still deliberately: the test is about the chaser, and a
	// target that walks away can lead the chaser round the crate instead of over it, which is a
	// pass that proves nothing.
	const FVector PlayerAt(Base.X, Base.Y - Half, Base.Z);
	Player->SetActorLocation(PlayerAt, false, nullptr, ETeleportType::TeleportPhysics);
	if (UCharacterMovementComponent* PlayerMove = Player->FindComponentByClass<UCharacterMovementComponent>())
	{
		PlayerMove->StopMovementImmediately();
	}

	// The crate, sitting on the floor exactly midway between the two pawns.
	//
	// Midway is the point of the fixture, and getting it wrong is what made the first two
	// versions of this test measure nothing. The pawns are JumpTestSeparation (900 cm) apart on
	// the Y axis, so the crate belongs at the midpoint, 450 cm from each. The chaser's feeler
	// is AvoidTraceLength (300 cm) long and its jump only arms inside JumpTriggerDistance
	// (190 cm), so as the chaser closes it sees the crate at 300 cm - before the trigger - and
	// reaches the trigger at 190 cm with the crate still 260 cm dead ahead. That gap between
	// "sees it" and "is close enough to act" is exactly the window the feature lives in.
	//
	// Placing it at Base instead (the player start, and therefore the point the player was
	// moved *away* from) put the crate 450 cm out, past the whole feeler: the probe reported
	// "face 300 cm" forever, which reads as "nothing ahead" and sent the chaser round on a
	// perfectly correct avoidance path. A fixture the thing under test cannot see is not a test.
	const float CrateHeight = FMath::Max(JumpTestCrateHeight, 10.0f);
	const FVector CrateXy(Base.X, Base.Y + Half * static_cast<double>(CrateOffsetFraction), Base.Z);

	// The floor, traced for at the CRATE's XY rather than at Base. They are 225+ cm apart, and
	// on a city plaza with kerbs and road camber those two points need not be at the same
	// height - anchoring the crate to the player's floor could leave it floating or sunk.
	float GroundZ = Base.Z - CrateHeight;
	{
		FCollisionQueryParams GroundParams(TEXT("PursuitJumpTestGround"), false, nullptr);
		FHitResult GroundHit;
		const FVector From = CrateXy + FVector(0.0f, 0.0f, 500.0f);
		const FVector To = CrateXy - FVector(0.0f, 0.0f, 5000.0f);
		if (World->LineTraceSingleByChannel(GroundHit, From, To, ECC_WorldStatic, GroundParams))
		{
			GroundZ = static_cast<float>(GroundHit.Location.Z);
		}
	}

	const FVector CrateAt(CrateXy.X, CrateXy.Y, GroundZ + CrateHeight * 0.5f);

	// APursuitJumpCrate, not AStaticMeshActor, and the reason is recorded on that class: a
	// StaticMeshActor's component is registered before a mesh is assigned to it, so assigning
	// one afterwards produces an actor that renders perfectly and that every collision query
	// passes straight through. This fixture already spent a debugging cycle on that failure
	// mode, so it now uses a class that cannot have it.
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

		APursuitJumpCrate* Crate = World->SpawnActor<APursuitJumpCrate>(
			APursuitJumpCrate::StaticClass(), FTransform(CrateAt), Params);
		if (Crate)
		{
			JumpTestCrate = Crate;
#if WITH_EDITOR
			// Editor-only nicety: SetActorLabel lives in the editor module, so the staged
			// game target (WITH_EDITOR=0) has no such member. Guard it or the packaged
			// training build fails to compile - which is exactly what it did.
			Crate->SetActorLabel(TEXT("JumpTest_Crate"));
#endif

			// The cube is 100 cm, so a footprint of 300 and a height of 90 give a 300 x 300 x
			// 90 cm box. Height is the number the chaser's jump decision measures, which is why
			// it is the one the log prints.
			Crate->Configure(300.0f, CrateHeight, GroundZ);

			// Read the geometry back rather than trusting the arithmetic above. The height the
			// jump decision sees comes from a trace against this actor's collision, so the trace
			// result is the only number that actually matters - and this read-back is exactly
			// what caught the earlier mesh-before-scale ordering.
			float MeasuredTop = CrateAt.Z;
			{
				FCollisionQueryParams ReadParams(TEXT("PursuitJumpTestCrateRead"), false, nullptr);
				FHitResult ReadHit;
				const FVector ReadFrom(CrateAt.X, CrateAt.Y, GroundZ + 3000.0f);
				const FVector ReadTo(CrateAt.X, CrateAt.Y, GroundZ - 100.0f);
				if (World->LineTraceSingleByChannel(ReadHit, ReadFrom, ReadTo, ECC_WorldStatic, ReadParams))
				{
					MeasuredTop = static_cast<float>(ReadHit.Location.Z);
				}
			}

			// The spawn-frame read is expected to be unreliable - the component was only just
			// registered - so it is a data point and not a verdict. The verdict is the deferred
			// trace in Tick, which is why the fixture SAYS which is which rather than printing
			// one number that could be either.
			UE_LOG(LogPursuitAI, Log,
				TEXT("PursuitPlay: jump test crate at %s, spawn-frame trace found top z=%.0f ")
				TEXT("(height %.0f, wanted %.0f), ground z=%.0f"),
				*CrateAt.ToString(), MeasuredTop, MeasuredTop - GroundZ, CrateHeight, GroundZ);

			CrateVerifyZ = MeasuredTop;
			bCrateVerifyPending = true;
		}
		else
		{
			UE_LOG(LogPursuitAI, Warning, TEXT("PursuitPlay: jump test could not spawn its crate"));
		}
	}

	bJumpTestLaidOut = true;

	UE_LOG(LogPursuitAI, Log,
		TEXT("PursuitPlay: JUMP TEST - player held at %s, chaser %d at %s, %.0f cm crate between ")
		TEXT("them, %.0f cm apart. The only route is over the top, so a jump either happens or ")
		TEXT("the feature is broken."),
		*PlayerAt.ToString(), Keeper + 1, *FVector(Base.X, Base.Y + Half, Base.Z).ToString(),
		CrateHeight, JumpTestSeparation);
}

void APursuitPlayGameMode::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	// The jump test's layout, deferred out of BeginPlay because the player pawn does not exist
	// yet at that point. bJumpTestLaidOut stays set once it succeeds, so this is genuinely a
	// one-shot and Tick's job here is done for the rest of the run.
	if (bJumpTest && !bJumpTestLaidOut)
	{
		SetupJumpTest();
	}

	// And the crate's second opinion, a frame after it was built. See bCrateVerifyPending:
	// a collision body assigned its mesh after registration reports no geometry, and the only
	// way to tell that apart from a crate that was never spawned is to ask again later.
	if (bCrateVerifyPending && JumpTestCrate.IsValid())
	{
		bCrateVerifyPending = false;

		UWorld* VerifyWorld = GetWorld();
		const FVector Where = JumpTestCrate->GetActorLocation();
		float SecondTop = -FLT_MAX;
		if (VerifyWorld)
		{
			FCollisionQueryParams VerifyParams(TEXT("PursuitJumpTestCrateVerify"), false, nullptr);
			FHitResult VerifyHit;
			const bool bHit = VerifyWorld->LineTraceSingleByChannel(
				VerifyHit,
				FVector(Where.X, Where.Y, Where.Z + 3000.0f),
				FVector(Where.X, Where.Y, Where.Z - 3000.0f),
				ECC_WorldStatic, VerifyParams);
			if (bHit)
			{
				SecondTop = static_cast<float>(VerifyHit.Location.Z);
			}
		}

		// Loud on purpose. This line is the fixture's own quality gate: if the second trace
		// still finds nothing where the crate is, then the chaser's probe will find nothing
		// either, and any "it did not jump" conclusion drawn from this run is about the
		// fixture and not about the AI.
		//
		// Two separate UE_LOG statements rather than a computed verbosity: the verbosity
		// argument has to be a compile-time token, so it cannot be a ternary.
		//
		// And the sentinel is tested FIRST, on its own. The first version of this check was
		//
		//     (SecondTop != -FLT_MAX) && (SecondTop > Where.Z - 200.0f)
		//
		// which is the right shape and was defeated by needing a number to print: it passed
		// -FLT_MAX through a ternary, -FLT_MAX is a very negative number, and the comparison
		// therefore came out true on a trace that had hit nothing at all. The check reported
		// VERIFIED on every round while the crate did not exist. A "did the trace hit"
		// question has to be answered before anything is done with the number.
		const bool bHitSomething = (SecondTop != -FLT_MAX);
		const bool bCollidable = bHitSomething && (SecondTop > Where.Z - 200.0f);
		if (bCollidable)
		{
			UE_LOG(LogPursuitAI, Log,
				TEXT("PursuitPlay: jump test crate collision VERIFIED - top z=%.0f a frame after ")
				TEXT("spawn (spawn-frame read said %.0f). The fixture is sound, so a run that does ")
				TEXT("not jump is a real result."),
				SecondTop, CrateVerifyZ);
		}
		else
		{
			UE_LOG(LogPursuitAI, Error,
				TEXT("PursuitPlay: jump test crate collision MISSING - the trace at %s found no ")
				TEXT("geometry in a 6000 cm column a frame after spawn (spawn-frame read said %.0f, ")
				TEXT("hit=%d). The chaser cannot see this crate, so fix the fixture before reading ")
				TEXT("anything into the run."),
				*Where.ToString(), CrateVerifyZ, bHitSomething ? 1 : 0);
		}
	}

	// And its clock. A verification run has to end by itself: an UnrealEditor.exe left in
	// memory blocks the next build with "Unable to build while Live Coding is active", and this
	// is the only thing standing between a completed test and that error.
	if (bJumpTest && JumpTestSeconds > 0.0f)
	{
		JumpTestElapsed += DeltaSeconds;
		if (JumpTestElapsed >= JumpTestSeconds)
		{
			UE_LOG(LogPursuitAI, Log,
				TEXT("PursuitPlay: jump test window of %.0f s elapsed - quitting."), JumpTestSeconds);
			FGenericPlatformMisc::RequestExit(false);
			return;
		}
	}

	// The same clock for ordinary scripted runs, so a headless autoplay or avoidance pass
	// finishes on its own rather than having to be killed from outside. Separate from the
	// jump-test counter because the two are set by different switches and a run could
	// reasonably want one without the other.
	if (!bJumpTest && RunSeconds > 0.0f)
	{
		RunElapsed += DeltaSeconds;
		if (RunElapsed >= RunSeconds)
		{
			UE_LOG(LogPursuitAI, Log,
				TEXT("PursuitPlay: run window of %.0f s elapsed - quitting."), RunSeconds);
			FGenericPlatformMisc::RequestExit(false);
			return;
		}
	}

	// Env-box stage: the clock above still runs (a headless env run quits itself like any
	// other), but nothing below it applies - TickChaser would drive chasers that do not
	// exist, the HUD would draw over the env's overlay, and the env's own Tick steps,
	// draws and self-reports without help.
	if (bEnvBox)
	{
		return;
	}

	if (MessageRemaining > 0.0f)
	{
		MessageRemaining = FMath::Max(0.0f, MessageRemaining - DeltaSeconds);
	}

	if (bAutoPlay && !bJumpTest)
	{
		TickAutoPlay(DeltaSeconds);
	}

	if (bDrawHud)
	{
		DrawHud();
	}

	// Once a second, say where the camera is and what it should be looking at. See the note
	// on CameraLogTimer: this is what tells "the level has no light" apart from "the camera
	// is somewhere useless", and those two have the same screenshot.
	CameraLogTimer -= DeltaSeconds;
	if (CameraLogTimer <= 0.0f)
	{
		CameraLogTimer = 1.0f;

		const APawn* Player = UGameplayStatics::GetPlayerPawn(this, 0);
		const UCharacterMovementComponent* Move = Player
			? Player->FindComponentByClass<UCharacterMovementComponent>()
			: nullptr;

		FVector ViewLocation = FVector::ZeroVector;
		FRotator ViewRotation = FRotator::ZeroRotator;
		if (const APlayerController* PC = UGameplayStatics::GetPlayerController(this, 0))
		{
			PC->GetPlayerViewPoint(ViewLocation, ViewRotation);
		}

		// Log, not Verbose: the default verbosity of a DEFINE_LOG_CATEGORY category is Log,
		// so a Verbose line here would never reach the file it exists to write to.
		UE_LOG(LogPursuitAI, Log,
			TEXT("PursuitPlay: cam=(%.0f, %.0f, %.0f) rot=(pitch %.0f, yaw %.0f) ")
			TEXT("pawn=%s pawnZ=%.0f grounded=%d speed=%.0f drive=(%.2f, %.2f)"),
			ViewLocation.X, ViewLocation.Y, ViewLocation.Z,
			ViewRotation.Pitch, ViewRotation.Yaw,
			Player ? *Player->GetName() : TEXT("<none>"),
			Player ? Player->GetActorLocation().Z : 0.0f,
			(Move && Move->IsMovingOnGround()) ? 1 : 0,
			Move ? Move->Velocity.Size2D() : 0.0f,
			AutoPlayInput.X, AutoPlayInput.Y);
	}
}

void APursuitPlayGameMode::HandlePlayerCaught(APursuitCharacter* Catcher)
{
	++CaughtCount;

	Message = FString::Printf(TEXT("CAUGHT by a chaser - %d time(s) so far"), CaughtCount);
	MessageRemaining = 2.0f;

	// The geometry behind the catch, recorded while the positions are still the ones that
	// produced it - ResetRound teleports both pawns and destroys the evidence.
	//
	// This is not decoration. "It caught me while I was up on the platform" and "it climbed the
	// platform and caught me" are the same sentence from the player's chair and demand opposite
	// fixes; the horizontal reach and the vertical gap are what tell them apart, and printing
	// them costs one line per catch. If the deck is doing its job, a catch up there shows a
	// small height gap - meaning the chaser really did get up onto it.
	FString Geometry;
	if (Catcher)
	{
		if (const APawn* Player = UGameplayStatics::GetPlayerPawn(this, 0))
		{
			FVector Delta = Player->GetActorLocation() - Catcher->GetActorLocation();
			const float HeightGap = static_cast<float>(FMath::Abs(Delta.Z));
			Delta.Z = 0.0f;
			Geometry = FString::Printf(
				TEXT(" - %.0f cm out, %.0f cm off level (reach %.0f, gate %.0f)"),
				static_cast<float>(Delta.Size()), HeightGap,
				Catcher->CatchRadius, Catcher->CatchHeightTolerance);
		}
	}

	UE_LOG(LogPursuitAI, Log, TEXT("PursuitPlay: caught by %s (%d total)%s, resetting the round"),
		Catcher ? *Catcher->GetName() : TEXT("<unknown>"), CaughtCount, *Geometry);

	ResetRound(Catcher ? Catcher->GetActorLocation() : StartLocation);
}

void APursuitPlayGameMode::ResetRound(const FVector& CatcherLocation)
{
	// Under the jump test, a reset is not a reset - it is a re-run. The chaser reaching the
	// player means it cleared the crate, which is the result; putting both pawns back on the
	// fixed axis is what lets the next approach happen instead of the round degenerating into
	// the ordinary chase. Handled first and returned from, because the ring/corner logic below
	// is exactly what this test exists to replace.
	//
	// The layout flag is deliberately NOT cleared: SetupJumpTest is idempotent (it takes down
	// its own crate first), and leaving the flag set is what stops Tick from treating the
	// rebuild as the initial layout and doing it again next frame, forever.
	if (bJumpTest)
	{
		SetupJumpTest();
		return;
	}

	// The player goes back to the start spot, the chasers back to the ring. Losing a round
	// costs a second rather than a restart, which is what makes it possible to try four or five
	// escapes in a row and actually feel the difference between them.
	if (APawn* Player = UGameplayStatics::GetPlayerPawn(this, 0))
	{
		Player->SetActorLocation(StartLocation + FVector(0.0f, 0.0f, 10.0f), false, nullptr, ETeleportType::TeleportPhysics);

		if (UCharacterMovementComponent* Move = Player->FindComponentByClass<UCharacterMovementComponent>())
		{
			// Velocity survives a teleport, and a player who arrives at the start spot still
			// travelling at 980 cm/s is not a reset.
			Move->StopMovementImmediately();
		}
	}

	const FVector Centre = bCenterChasersOnPlayer
		? FVector(StartLocation.X, StartLocation.Y, StartLocation.Z)
		: FVector(0.0f, 0.0f, StartLocation.Z);
	for (int32 Index = 0; Index < Chasers.Num(); ++Index)
	{
		APursuitCharacter* Chaser = Chasers[Index];
		if (!Chaser)
		{
			continue;
		}

		const float AngleDegrees = 45.0f + (360.0f / FMath::Max(Chasers.Num(), 1)) * Index;
		const float Radians = FMath::DegreesToRadians(AngleDegrees);
		const FVector SpawnLocation = Centre + FVector(
			FMath::Cos(Radians) * ChaserSpawnRadius,
			FMath::Sin(Radians) * ChaserSpawnRadius,
			0.0f);

		Chaser->SetActorLocation(SpawnLocation, false, nullptr, ETeleportType::TeleportPhysics);
		if (UCharacterMovementComponent* Move = Chaser->GetCharacterMovement())
		{
			Move->StopMovementImmediately();
		}
	}
}

void APursuitPlayGameMode::TickAutoPlay(float DeltaSeconds)
{
	AutoPlayTime += DeltaSeconds;

	APursuitCharacter* Player = Cast<APursuitCharacter>(UGameplayStatics::GetPlayerPawn(this, 0));
	if (!Player)
	{
		AutoPlayInput = FVector::ZeroVector;
		return;
	}

	const FVector PlayerLocation = Player->GetActorLocation();

	// Straight-line repulsion from every chaser, weighted by 1/distance so the nearest one
	// dominates. This is not a policy, it is not good, and it is not trained: it exists so the
	// character's walk, run, jump, fall and land states can be exercised by a machine - which
	// is the only way to check them without someone holding WASD down for a minute.
	//
	// The division by WeightSum at the end is not tidiness. The weights are 1/distance, so a
	// chaser 800 cm away contributes about 0.0013, and the sum of four of them is around
	// 0.002 - a number that is meaningless as a direction only because it is small. Testing
	// that against an epsilon is how the first version of this quietly did nothing at all:
	// it compared SizeSquared (4e-6) against KINDA_SMALL_NUMBER (1e-4) and returned before
	// ever calling AddMovementInput. The player stood still and got caught 56 times.
	// Normalising by the weight sum turns the result into a weighted mean of unit vectors,
	// which is in [0, 1] and can be tested against an epsilon honestly.
	FVector Flee = FVector::ZeroVector;
	float WeightSum = 0.0f;
	float Nearest = TNumericLimits<float>::Max();

	for (const APursuitCharacter* Chaser : Chasers)
	{
		if (!Chaser)
		{
			continue;
		}

		FVector Away = PlayerLocation - Chaser->GetActorLocation();
		Away.Z = 0.0f;
		const float Distance = FMath::Max(Away.Size(), 1.0f);
		const float Weight = 1.0f / Distance;

		Flee += (Away / Distance) * Weight;
		WeightSum += Weight;
		Nearest = FMath::Min(Nearest, Distance);
	}

	if (WeightSum > 0.0f)
	{
		Flee /= WeightSum;
	}

	// The arena is a 36 m square. Turn back before the wall instead of grinding along it, which
	// would look like the autopilot had given up.
	constexpr float Bound = 1500.0f;
	if (PlayerLocation.X > Bound) { Flee.X -= 1.0f; }
	if (PlayerLocation.X < -Bound) { Flee.X += 1.0f; }
	if (PlayerLocation.Y > Bound) { Flee.Y -= 1.0f; }
	if (PlayerLocation.Y < -Bound) { Flee.Y += 1.0f; }

	// A slowly rotating bias. Four chasers converging from four sides cancel out exactly, and a
	// perfectly cancelled direction is indistinguishable from broken input - so the autopilot
	// would freeze in the middle of a symmetric surround, which is the one moment it most
	// obviously should be running.
	Flee += FVector(FMath::Cos(AutoPlayTime * 0.4f), FMath::Sin(AutoPlayTime * 0.4f), 0.0f) * 0.35f;

	Flee.Z = 0.0f;
	if (Flee.SizeSquared() < KINDA_SMALL_NUMBER)
	{
		AutoPlayInput = FVector::ZeroVector;
		return;
	}

	const FVector Direction = Flee.GetSafeNormal();
	AutoPlayInput = Direction;
	Player->AddMovementInput(Direction, 1.0f);

	// Face where it is going. The playable character takes its yaw from the controller, so
	// without this it would slide sideways while looking straight ahead - and the follow
	// camera, which tracks the control rotation, would never turn either. Pitch is kept so the
	// camera does not snap; this is the same thing the mouse axis does.
	if (APlayerController* PC = UGameplayStatics::GetPlayerController(this, 0))
	{
		const FRotator Current = PC->GetControlRotation();
		PC->SetControlRotation(FRotator(Current.Pitch, Direction.Rotation().Yaw, 0.0f));
	}

	// Alternate the pace. Same speeds the Shift key switches between, so the playback rate
	// scaling on the run clip is visible in a recording with nobody pressing anything.
	AutoPlaySprintTimer -= DeltaSeconds;
	if (AutoPlaySprintTimer <= 0.0f)
	{
		AutoPlaySprintTimer = 4.0f;
		bAutoPlaySprinting = !bAutoPlaySprinting;
	}

	UCharacterMovementComponent* Move = Player->GetCharacterMovement();
	if (!Move)
	{
		return;
	}
	Move->MaxWalkSpeed = bAutoPlaySprinting ? Player->SprintSpeed : Player->PlayerSpeed;

	// Jump when a chaser is close. That is the only thing that produces the jump, fall and land
	// clips, and the cooldown stops it pogoing on the spot.
	AutoPlayJumpCooldown -= DeltaSeconds;
	if (Nearest < 420.0f && AutoPlayJumpCooldown <= 0.0f && !Move->IsFalling())
	{
		Player->Jump();
		AutoPlayJumpCooldown = 1.6f;
	}
}

void APursuitPlayGameMode::DrawHud()
{
	if (!GEngine)
	{
		return;
	}

	const float Duration = 0.25f;
	const FVector2D TextScale(1.2f, 1.2f);

	// bNewerOnTop = false so the lines stay in the order they are written. Default true puts the
	// first line at the bottom and the panel reads upside down.
	const auto Line = [this, Duration, &TextScale](uint64 Key, const FColor& Colour, const FString& Text)
	{
		GEngine->AddOnScreenDebugMessage(Key, Duration, Colour, Text, /*bNewerOnTop=*/false, TextScale);
	};

	// ASCII only. The engine's debug font has no CJK glyphs, so anything else renders as empty
	// boxes - a lesson this project already paid for once.
	//
	// One thing about this HUD is not ours and cannot be switched off: whenever any on-screen
	// debug text is drawn, the engine follows it with its own faint grey
	// "'DisableAllScreenMessages' to suppress" hint (UnrealEngine.cpp, drawn at 5% grey so it
	// reads as a smudge). It looked like a stray warning for a while - it is not, it is the
	// price of using AddOnScreenDebugMessage. Removing it means drawing the HUD on a canvas
	// instead, which is not worth the code for a line nobody can read.
	// The control line is drawn from the camera's point of view and has to tell the truth about
	// it: under the god camera nobody is driving with WASD, and a line that still advertises it
	// reads as "the input is broken" to whoever watches the clip.
	const APursuitCharacter* PlayerCharacter = Cast<APursuitCharacter>(UGameplayStatics::GetPlayerPawn(this, 0));
	const bool bGodView = PlayerCharacter
		&& PlayerCharacter->GetResolvedCameraMode() == EPursuitCameraMode::God;

	Line(9100, FColor::White, bGodView
		? TEXT("PursuitAI play  |  god camera - autopilot steers, nobody at the keyboard")
		: TEXT("PursuitAI play  |  WASD move   Space jump   Shift sprint   mouse look"));

	const APawn* Player = UGameplayStatics::GetPlayerPawn(this, 0);
	const FVector PlayerLocation = Player ? Player->GetActorLocation() : FVector::ZeroVector;
	const float PlayerSpeed = Player ? Player->GetVelocity().Size2D() : 0.0f;

	float Nearest = -1.0f;
	const UCharacterMovementComponent* PlayerMove = Player ? Player->FindComponentByClass<UCharacterMovementComponent>() : nullptr;
	const TCHAR* Ground = !PlayerMove ? TEXT("?") : (PlayerMove->IsFalling() ? TEXT("airborne") : TEXT("grounded"));

	// Where a chaser's label sits relative to the chaser.
	//
	// Under the follow camera a plain +235 in Z is up the screen: the view is roughly level, so
	// world Z and the screen's up axis point the same way. Under the god camera the view is
	// almost straight down, so world Z projects to nearly nothing and that same lift drops the
	// label onto the character it names - watched on a god-camera frame, where the labels sat
	// across their own chasers' bodies. Offsetting along the ground in the camera's own
	// up-axis is the fix: it is the direction that reads as "up" from wherever the lens is,
	// and it costs one view-point query per frame.
	FVector LabelLift(0.0f, 0.0f, 235.0f);
	if (bGodView)
	{
		FVector ViewLocation = FVector::ZeroVector;
		FRotator ViewRotation = FRotator::ZeroRotator;
		if (const APlayerController* PC = UGameplayStatics::GetPlayerController(this, 0))
		{
			PC->GetPlayerViewPoint(ViewLocation, ViewRotation);
		}
		LabelLift = FRotator(0.0f, ViewRotation.Yaw, 0.0f).Vector() * 300.0f
			+ FVector(0.0f, 0.0f, 140.0f);
	}

	for (int32 Index = 0; Index < Chasers.Num(); ++Index)
	{
		APursuitCharacter* Chaser = Chasers[Index];
		if (!Chaser)
		{
			continue;
		}

		const float Distance = FVector::Dist2D(Chaser->GetActorLocation(), PlayerLocation);
		Nearest = (Nearest < 0.0f) ? Distance : FMath::Min(Nearest, Distance);

		if (bDrawChaserLabels)
		{
			// Duration 0 draws for this frame only. Cheaper than giving every chaser a different
			// mesh, and it answers the only question that matters while running away: which one.
			// Font scale, not a bigger offset: from the god camera the pawns are about a sixth
			// of the frame tall, and a label left at scale 1 next to them is unreadable in the
			// recording. It is the one thing in this HUD that has to survive being seen from
			// 29 m up, because it is what names which chaser is closing.
			DrawDebugString(GetWorld(),
				Chaser->GetActorLocation() + LabelLift,
				FString::Printf(TEXT("CHASER %d  %.0f cm"), Index + 1, Distance),
				nullptr, FColor(120, 220, 145), 0.0f, /*bDrawShadow=*/true,
				bGodView ? 2.0f : 1.0f);
		}
	}

	const FString NearestText = Nearest >= 0.0f ? FString::Printf(TEXT("%.0f cm"), Nearest) : TEXT("n/a");
	Line(9101, FColor(170, 215, 255), FString::Printf(
		TEXT("Chasers %d   caught %d   nearest %s   speed %.0f cm/s   %s"),
		Chasers.Num(), CaughtCount, *NearestText, PlayerSpeed, Ground));

	if (MessageRemaining > 0.0f && !Message.IsEmpty())
	{
		Line(9102, FColor(255, 150, 140), Message);
	}
}
