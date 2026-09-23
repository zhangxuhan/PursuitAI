// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"

#include "PursuitCharacter.h"

#include "PursuitPlayGameMode.generated.h"

class APursuitCharacter;

/**
 * The game mode for the playable map, L_PursuitPlay.
 *
 * It does three things: it makes the player a character, it spawns the chasers, and it puts the
 * player back on the start spot when one of them catches up. That is the whole game loop, and
 * it is deliberately scripted - there is no policy here and no model. The point of this map is
 * to answer "what does the chase feel like to play", which is a question a training curve
 * cannot answer and a person can in about ten seconds.
 *
 * Nothing in here touches APursuitAIEnv. The training map keeps its own game mode and is
 * unaffected by anything on this page.
 */
UCLASS()
class PURSUITAI_API APursuitPlayGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:
	APursuitPlayGameMode();

	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;

	/** How many chasers to spawn. Four leaves room to run without being a wall of green. */
	UPROPERTY(EditAnywhere, Category = "Pursuit|Play")
	int32 ChaserCount = 4;

	/** Radius of the ring the chasers start on, around the middle of the arena. */
	UPROPERTY(EditAnywhere, Category = "Pursuit|Play")
	float ChaserSpawnRadius = 950.0f;

	/** Turned on by -PursuitNoHud, for a clean capture. */
	UPROPERTY(EditAnywhere, Category = "Pursuit|Play")
	bool bDrawHud = true;

	/** Overhead labels on each chaser, so it is obvious which one is closing in. */
	UPROPERTY(EditAnywhere, Category = "Pursuit|Play")
	bool bDrawChaserLabels = true;

	/**
	 * Turned on by -PursuitAutoPlay: the player pawn runs away on its own.
	 *
	 * Not a policy and not trained - straight-line repulsion from the nearest chaser. It is for
	 * watching the scene drive itself and for checking the animation states without a person at
	 * the keyboard.
	 */
	UPROPERTY(EditAnywhere, Category = "Pursuit|Play")
	bool bAutoPlay = false;

	// -- player spawn --------------------------------------------------------

	/**
	 * When true, the player is placed at PlayerSpawnLocation instead of wherever the level's
	 * PlayerStart puts them.
	 *
	 * The engine spawns the default pawn at a PlayerStart on its own; this flag lets you pin the
	 * spawn to a known point on a hand-built map (a city street, a plaza) without moving the
	 * level's PlayerStart - which may belong to the map's own demo and break it if you do.
	 * When false, the first PlayerStart in the level is used, with PlayerSpawnLocation as the
	 * fallback if the level has none.
	 */
	UPROPERTY(EditAnywhere, Category = "Pursuit|Play|Player")
	bool bUseConfiguredSpawn = false;

	/**
	 * The player's spawn point when bUseConfiguredSpawn is true, and the fallback when the level
	 * has no PlayerStart at all.
	 *
	 * This is the "spawn condition" the chase cares about: the player must have somewhere valid
	 * to appear, and if the level cannot provide one we use this rather than dropping them at the
	 * world origin - which on a city map is somewhere under a building or a kilometre from the
	 * action.
	 */
	UPROPERTY(EditAnywhere, Category = "Pursuit|Play|Player")
	FVector PlayerSpawnLocation = FVector(0.0f, -1000.0f, 140.0f);

	// -- chaser spawn --------------------------------------------------------

	/** Centre the chaser ring on the player's spawn rather than on the world origin (0, 0). */
	UPROPERTY(EditAnywhere, Category = "Pursuit|Play|Chasers")
	bool bCenterChasersOnPlayer = true;

	/** Rig the even-indexed chasers wear. The player keeps the Tiny Hero. */
	UPROPERTY(EditAnywhere, Category = "Pursuit|Play|Chasers")
	EPursuitHeroModel ChaserHeroA = EPursuitHeroModel::RPGHero;

	/** Rig the odd-indexed chasers wear. The pack's dog. */
	UPROPERTY(EditAnywhere, Category = "Pursuit|Play|Chasers")
	EPursuitHeroModel ChaserHeroB = EPursuitHeroModel::AnimalHero;

	// -- deterministic jump test ---------------------------------------------

	/**
	 * Turned on by -PursuitJumpTest: pins one chaser directly opposite the player with a
	 * jumpable crate between them, and holds both still.
	 *
	 * The problem this solves is that the chase is *chaotic*: whether a chaser happens to
	 * meet a crate depends on where a fleeing player leads it, and on a city map with a
	 * running target that may never happen in a given run. "The AI jumps" is then not
	 * something a run can demonstrate. This mode removes the chance: the geometry is fixed,
	 * the two pawns face each other across one crate, and the only way the chaser reaches
	 * the player is over the top - so a jump either happens or the feature is broken.
	 *
	 * The fixture, concretely (all distances along Y, from the player start):
	 *
	 *     chaser 1      crate        player
	 *     +450 cm       +225 cm      -450 cm
	 *        |             |            |
	 *        +--- 225 ---->+<-- 675 --->+
	 *
	 * The chaser has to cross 900 cm. Its feeler is 300 cm, so the crate appears at 300 cm
	 * - before the jump can arm. The jump arms at 190 cm, by which point the crate is 260 cm
	 * ahead: the chaser has to decide at range whether to hop or to go round, which is the
	 * actual behaviour under test. Verify the crate's height on screen before believing a run
	 * either way - see the file-level note in PursuitPlayGameMode.cpp.
	 */
	UPROPERTY(EditAnywhere, Category = "Pursuit|Play|Test")
	bool bJumpTest = false;

	/**
	 * Env-box stage, turned on by -PursuitEnvBox.
	 *
	 * Demonstration is the one stage everything visual is shot on, and the A/B comparison
	 * lives in APursuitAIEnv - deterministic slab physics, seeded spawns, drive modes. This
	 * flag turns this game mode into a host for that actor: no chasers, no play-mode loop,
	 * no play-mode HUD, and the default pawn demoted to the engine's invisible ball (the
	 * env hides it anyway, but a PursuitCharacter would run its whole camera Tick for
	 * nothing). One launch on the city map then means "film the env", without touching the
	 * saved level or maintaining a second game mode.
	 */
	UPROPERTY(EditAnywhere, Category = "Pursuit|Play")
	bool bEnvBox = false;

	/**
	 * Char-env-box stage, turned on by -PursuitCharEnvBox: the same host trick as
	 * bEnvBox, but for the v2 character environment. Spawns two APursuitCharAgents
	 * (chaser + evader) and then one APursuitCharEnv wired to them (deferred, so the
	 * refs exist before its BeginPlay reads them), with the arena rig left off - on
	 * the city map the streets are the stage, and -PursuitStage=city on the env side
	 * parks the arena on the measured plaza. The watched-run switches
	 * (-PursuitCharGreedy/Random/Inference, seed, pane label, watch-camera tilt)
	 * parse on the env as usual, so the launch line stays the single source of truth.
	 */
	UPROPERTY(EditAnywhere, Category = "Pursuit|Play")
	bool bCharEnvBox = false;

	/**
	 * True when Schola is driving this simulator (detected from -ScholaDisableScript, which
	 * the trainer always passes and a watched/play run never does). Under this flag BeginPlay
	 * returns immediately: no player spawn, no SpawnChasers(), no jump rig.
	 *
	 * Added 2026-09-22 after the scripted chase was found to run CONCURRENTLY with training on
	 * Demonstration_Train (whose GameMode is this class, and which the training .bat launches
	 * without -PursuitEnvBox / -PursuitCharEnvBox). The scripted chasers caught the evader
	 * throughout every run, which inflated a naive `grep -c "caught by"` to 242 while the
	 * training environment's real count was 0 - and they walked the same plaza the policy was
	 * learning on. A host that owns the world must be the only thing placing actors in it.
	 */
	UPROPERTY(VisibleAnywhere, Category = "Pursuit|Play")
	bool bScholaHosted = false;

	/**
	 * Spawn the second chaser (the RPGHero soldier) in a char-env-box run, turned on
	 * by -PursuitSupportAgent. Off by default (user request 2026-09-21): the scripted
	 * soldier stood still in watched runs and read as a broken prop - until it comes
	 * back as a real trained agent (docs CHARACTER_ENV_PLAN §7 multi-agent plan), a
	 * second scripted chaser adds nothing to the shot. The env treats a null
	 * SupportAgent as "no second chaser", so the catch/scoring path is untouched.
	 */
	UPROPERTY(EditAnywhere, Category = "Pursuit|Play")
	bool bSpawnSupportAgent = false;

	/** How far apart the two test pawns stand, with the crate between them. */
	UPROPERTY(EditAnywhere, Category = "Pursuit|Play|Test")
	float JumpTestSeparation = 900.0f;

	/**
	 * Where along the player->chaser line the crate sits, as a fraction of the half separation.
	 *
	 * 0 would be the player start, 1.0 the chaser's spot. The default of 0.5 is the midpoint.
	 * The knob exists because the interesting question is not "can it jump a box" but "does it
	 * decide to jump at the right range": the chaser's feeler reaches 300 cm and its jump only
	 * arms inside 190 cm, and moving the crate along this line is what walks the fixture
	 * through that window.
	 */
	UPROPERTY(EditAnywhere, Category = "Pursuit|Play|Test")
	float CrateOffsetFraction = 0.5f;

	/**
	 * Which chaser the fixture keeps. Defaults to 1, not 0.
	 *
	 * Index 0 is the player's own pawn on some runs (the engines' spawn order is not something
	 * this game mode controls), and a fixture that parks "every chaser except index 0" therefore
	 * occasionally parks all of them and then measures nothing at all. Index 1 has never been
	 * the player in any run observed, and the field is here so a failing run can be re-pointed
	 * without a recompile.
	 */
	UPROPERTY(EditAnywhere, Category = "Pursuit|Play|Test")
	int32 JumpTestChaserIndex = 1;

	/**
	 * Where the parked chasers are put, as an X offset from the player start, and how tall the
	 * crate is.
	 *
	 * The offset is a property rather than a literal because "park them far away" and "park them
	 * somewhere they cannot possibly interfere" are different claims, and only the second one is
	 * safe: a parked chaser standing on the plaza is a chaser whose 240 cm separation push still
	 * leans on the one under test.
	 */
	UPROPERTY(EditAnywhere, Category = "Pursuit|Play|Test")
	float JumpTestParkOffset = 4000.0f;

	/**
	 * Height of the test crate, in centimetres.
	 *
	 * Must sit strictly between CharacterMovement's MaxStepHeight (45, which the capsule walks
	 * over for free) and PursuitCharacter's MaxJumpHeight (150, above which the chaser treats it
	 * as a wall and steers round). 90 is comfortably inside that band and is roughly waist high,
	 * which is what the "obstacle you can jump" in the brief means.
	 */
	UPROPERTY(EditAnywhere, Category = "Pursuit|Play|Test")
	float JumpTestCrateHeight = 90.0f;

	/**
	 * Seconds a run of -PursuitJumpTest keeps going before quitting by itself, or 0 to run until
	 * the window is closed.
	 *
	 * Set by -PursuitJumpTestSeconds=<n>. A headless verification run has to end on its own:
	 * leaving an UnrealEditor.exe alive blocks the next UBT build with "Unable to build while
	 * Live Coding is active", and killed-by-hand is exactly the kind of thing that gets
	 * forgotten once and then diagnosed for twenty minutes.
	 */
	UPROPERTY(EditAnywhere, Category = "Pursuit|Play|Test")
	float JumpTestSeconds = 0.0f;

	/**
	 * Seconds an ordinary run keeps going before quitting by itself, or 0 to run until the
	 * window is closed.
	 *
	 * Set by -PursuitRunSeconds=<n>. The same reasoning as JumpTestSeconds, applied to the
	 * scripted runs that are not the jump test - an autoplay pass over the city, an avoidance
	 * check, anything driven headless from a command line. Those runs are precisely the ones
	 * nobody is watching, so the process ending on its own is the difference between a
	 * verification step and a leaked editor that breaks the next build.
	 */
	UPROPERTY(EditAnywhere, Category = "Pursuit|Play|Test")
	float RunSeconds = 0.0f;

	/** Called by a chaser that has reached the player. Ends the round and resets it. */
	void HandlePlayerCaught(APursuitCharacter* Catcher);

private:
	void SpawnChasers();
	void ResetRound(const FVector& CatcherLocation);
	void TickAutoPlay(float DeltaSeconds);
	void DrawHud();

	/**
	 * Lays out the fixed two-pawn / one-crate geometry used by -PursuitJumpTest: the chasers,
	 * the crate, and the player.
	 *
	 * Safe to call more than once - it takes down any crate it built before and builds it
	 * again - but it is called exactly once, from Tick, because a game mode's BeginPlay runs
	 * before the engine has spawned the player pawn and there is therefore no player to place
	 * until a frame or two later. Placing the chasers and crate in BeginPlay and the player
	 * from Tick was the shape that looked natural and was wrong: it split one layout across two
	 * functions that then had to agree about which half had already happened.
	 */
	void SetupJumpTest();

	/**
	 * True once the jump test has laid itself out.
	 *
	 * Distinct from "the crate exists", which is what it needs to gate: this stays set for the
	 * whole run, so Tick knows its one job is done, while ResetRound can rebuild the layout
	 * without the rebuild looking like the first one and spawning a second crate per frame.
	 */
	bool bJumpTestLaidOut = false;

	/**
	 * The warp test's crate, held so a rebuild can remove it.
	 *
	 * Held rather than found by label: the test creates this actor itself, so it owns it, and a
	 * label lookup would match a crate a user happened to spawn by hand in the editor.
	 */
	UPROPERTY(Transient)
	TWeakObjectPtr<AActor> JumpTestCrate;

	/**
	 * Set while the fixture still owes the log a second opinion on the crate's collision.
	 *
	 * The crate is spawned mid-Tick and read back immediately, and a component whose mesh is
	 * assigned after it was registered reports whatever it was registered with - which is
	 * nothing. Asking again a frame later is the difference between "the crate is missing" and
	 * "the crate had not finished registering", and those have different fixes.
	 */
	bool bCrateVerifyPending = false;

	/** The top Z the in-frame read-back measured, for the deferred comparison in Tick. */
	float CrateVerifyZ = 0.0f;

	/** Seconds before the autopilot may jump again, so it does not pogo on the spot. */
	float AutoPlayJumpCooldown = 0.0f;

	/** Counts down the current sprint phase; the autopilot swaps pace every four seconds. */
	float AutoPlaySprintTimer = 0.0f;
	bool bAutoPlaySprinting = true;

	/** Drives the rotating bias in the autopilot's escape direction. */
	float AutoPlayTime = 0.0f;

	/** Counts up while the jump test runs, against JumpTestSeconds. */
	float JumpTestElapsed = 0.0f;

	/** Counts up during an ordinary self-terminating run, against RunSeconds. */
	float RunElapsed = 0.0f;

	/**
	 * The direction the autopilot asked for this frame, or zero if it asked for nothing.
	 * Printed by the once-a-second diagnostic: "the player is not moving" and "the player is
	 * being pushed into a wall" produce the same still image, and this is what told them apart.
	 */
	FVector AutoPlayInput = FVector::ZeroVector;

	/** Where the player spawns and is put back. Resolved in ResolvePlayerSpawn(), not hard coded. */
	FVector StartLocation = FVector(0.0f, -1000.0f, 140.0f);

	UPROPERTY(Transient)
	TArray<TObjectPtr<APursuitCharacter>> Chasers;

	int32 CaughtCount = 0;

	/**
	 * Decides where the player appears, and records the rule so the reset logic and the chaser
	 * ring agree on the same point. See the bUseConfiguredSpawn / PlayerSpawnLocation notes:
	 * a level PlayerStart wins unless one is pinned, and a missing PlayerStart falls back to the
	 * configured point rather than to the world origin.
	 */
	FVector ResolvePlayerSpawn();

	/** Seconds the "CAUGHT" line stays on screen. */
	float MessageRemaining = 0.0f;
	FString Message;

	/**
	 * Counts down to the next camera diagnostic line.
	 *
	 * A screen that renders black is three different bugs wearing the same costume: no light
	 * in the level, a camera below the floor or inside a wall, and a camera pointed at empty
	 * space all look identical from a screenshot. These lines separate them in a single run
	 * for the cost of one log line a second, and they are the reason the lighting bug in
	 * gen_play_level.py was found rather than guessed at.
	 */
	float CameraLogTimer = 0.0f;
};
