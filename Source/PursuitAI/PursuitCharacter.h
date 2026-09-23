// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Character.h"

#include "PursuitCharacter.generated.h"

class UAnimSequence;
class UCameraComponent;
class USkeletalMesh;
class USpringArmComponent;

/**
 * Which art rig a pawn wears.
 *
 * The three RPG Hero Squad rigs are *not* interchangeable: each one has its own skeleton and
 * its own animation set, so the mesh and the clips have to move together. They also differ in
 * what they can do - only the Tiny Hero ships jump clips at all - which is why the pairing of
 * rig to role is a decision and not a detail.
 *
 * Note what the pack's own blueprints are not: BP_TinyHeroPBR and friends derive from
 * ASkeletalMeshActor, so they are display props with no capsule, no movement component and no
 * input. They can be placed in a level to look at; they cannot be possessed and cannot chase
 * anyone. What is reusable from the pack is the mesh, the skeleton and the animation set.
 */
UENUM(BlueprintType)
enum class EPursuitHeroModel : uint8
{
	/** Epic's mannequin from the MoverExamples plugin. The fallback when the pack is absent. */
	Mannequin,

	/** RPG Hero Squad's Tiny Hero - the only rig in the pack with a jump. */
	TinyHero,

	/** RPG Hero Squad's animal hero, which the pack models as a bipedal dog. */
	AnimalHero,

	/** RPG Hero Squad's RPG hero. */
	RPGHero
};

/**
 * What the jump check decided, and - more importantly - whether the chaser is now committed
 * to the obstacle in front of it.
 *
 * TickChaser needs the second answer, not the first. "Is there something jumpable ahead" and
 * "therefore do not steer around it" are the same decision seen from two sides, and letting
 * the steering run regardless is what made chasers walk around waist-high crates: avoidance
 * sees the crate from 300 cm, the jump only arms at 190, and in between the steering peels the
 * chaser off-axis so the crate never reaches the jump's own forward line.
 */
UENUM()
enum class EPursuitJumpVerdict : uint8
{
	/** Nothing worth jumping. Steer normally. */
	None,

	/**
	 * Something jumpable is ahead and within range, and the chaser should keep driving straight
	 * at it until the jump fires. Suppresses avoidance for this frame.
	 */
	Commit,

	/** A jump was issued this frame. Also suppresses avoidance, so the arc is not bent sideways. */
	Jumped,
};

/**
 * Which camera the player pawn renders from.
 *
 * Three views, because they answer three different questions. FirstPerson is the one a person
 * plays with - eye level, on the pawn, turned with the mouse, and the only one where the
 * city's scale (30 m street, 57 m towers) reads without the character's own back in the way.
 * Follow is the same session seen from behind the shoulder, which is what a -game window gets
 * so the character stays visible while it is being steered. God is not a play view at all: it
 * is a detached camera high above the chase, which exists so that a recording shows the
 * movement instead of showing the back of a car.
 *
 * The reason this is an enum with an Auto member rather than a bool is the rule the video
 * work landed on: the editor session and the recorded session want different cameras, and
 * "which session is this" is a runtime fact, not something to remember to pass on the command
 * line. Auto is the rule, the other three are the override.
 */
UENUM(BlueprintType)
enum class EPursuitCameraMode : uint8
{
	/**
	 * First person inside the editor, God everywhere else.
	 *
	 * "Inside the editor" means the editor process itself, which includes Play In Editor -
	 * that is the session a person is holding the mouse in. Everything that is not the editor
	 * (`UnrealEditor.exe -game`, which is what tools/record_city.py launches, and a packaged
	 * build) is a session somebody is looking at from outside, and gets the god camera.
	 */
	Auto,

	/** The spring-arm camera behind the shoulder. What the game has always used. */
	Follow,

	/** Eye level, on the pawn, body out of frame. */
	FirstPerson,

	/** Detached and high above: the chase seen whole, nothing between the lens and the street. */
	God,
};


/**
 * The character in the *playable* level: chase, or be chased, with WASD and a mouse.
 *
 * This is deliberately a plain UE character and has nothing to do with the RL environment.
 * The two live in different maps and share no state:
 *
 *   L_PursuitAITrain   APursuitAIEnv      position-stepped, no physics, 300-400 steps/s
 *   L_PursuitPlay      APursuitCharacter  a normal ACharacter on CharacterMovementComponent
 *
 * Keeping them apart is the whole point. The trainer needs a step that is worth exactly
 * MoveStep and never reads dt, which is what makes a headless run reproducible. A playable
 * character wants the opposite: gravity, acceleration, a capsule that slides along walls.
 * One actor cannot be both without making both worse, so it is not attempted.
 *
 * The same class plays both roles. bIsChaser decides which: the player gets the camera and
 * the input bindings, a chaser gets a steering tick that walks it at the player.
 */
UCLASS()
class PURSUITAI_API APursuitCharacter : public ACharacter
{
	GENERATED_BODY()

public:
	APursuitCharacter();

	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;
	virtual void SetupPlayerInputComponent(UInputComponent* PlayerInputComponent) override;

	/** Chasers are the same pawn with the camera and the input taken away. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Pursuit|Role")
	bool bIsChaser = false;

	// -- rig -----------------------------------------------------------------

	/**
	 * Which rig to wear. Everything else about the look follows from this.
	 *
	 * Setting it after construction is not enough on its own - the asset pointers below were
	 * already filled in by the constructor - so call ApplyHeroModel() afterwards. A deferred
	 * spawn has exactly that problem, which is why the play game mode calls it explicitly.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pursuit|Rig")
	EPursuitHeroModel HeroModel = EPursuitHeroModel::TinyHero;

	/**
	 * Fills the mesh, the clips and their reference speeds from HeroModel.
	 *
	 * Called from the constructor so a plain APursuitCharacter is already correct, and again
	 * whenever someone changes HeroModel. Safe to call at runtime: if the actor has already
	 * begun play it reloads the visuals, so a prop swap does not need a respawn.
	 */
	UFUNCTION(BlueprintCallable, Category = "Pursuit|Rig")
	void ApplyHeroModel();

	/**
	 * Whether to override the rig's materials with a flat colour.
	 *
	 * On for the mannequin, which ships untextured and grey, and off for the pack rigs: their
	 * own PBR materials are the entire point of using them, and painting all three the same
	 * red or green would both throw the art away and stop you telling the player from the
	 * chasers at a glance - which the three distinct rigs now do on their own.
	 */
	UPROPERTY(EditAnywhere, Category = "Pursuit|Rig")
	bool bTintCharacter = false;

	/**
	 * Yaw applied to the mesh so its forward lines up with the capsule's +X.
	 *
	 * -90 for every rig here, and that is measured rather than assumed: all three pack
	 * skeletons are authored facing +Y (hands spread along X in the bind pose, and the ball of
	 * each foot offset to +Y from the ankle), which is the same convention as the mannequin.
	 * tools/inspect_hero_facing.py prints those bone offsets. A rig authored the other way
	 * round would run sideways until this is flipped to +90.
	 */
	UPROPERTY(EditAnywhere, Category = "Pursuit|Rig")
	float MeshRelativeYaw = -90.0f;

	// -- movement ------------------------------------------------------------

	/** Player speed while running, and while holding Shift. A chaser sits between the two. */
	UPROPERTY(EditAnywhere, Category = "Pursuit|Movement")
	float PlayerSpeed = 620.0f;

	UPROPERTY(EditAnywhere, Category = "Pursuit|Movement")
	float SprintSpeed = 980.0f;

	/** Deliberately a little under PlayerSpeed: a chaser that matches you exactly is a wall. */
	UPROPERTY(EditAnywhere, Category = "Pursuit|Movement")
	float ChaserSpeed = 575.0f;

	/**
	 * How close a chaser has to get *horizontally* before the round is lost.
	 *
	 * Measured centre to centre, and both capsules have a radius of 34 - so the number
	 * that reads as "it touched me" is a surface gap, not this value. At 130 the gap was
	 * 130 - 34 - 34 = 62 cm of visibly empty space, which is why a catch looked like it
	 * happened from across the street. 85 leaves 17 cm, which reads as contact without
	 * asking the chaser to land exactly on top of the player.
	 *
	 * Horizontal is the whole of this number's job - it says nothing about height, and it
	 * never did. What stops a chaser from catching a player who is standing above it is
	 * CatchHeightTolerance below, not this.
	 *
	 * -PursuitCatchRadius=<cm> overrides it per launch (see APursuitPlayGameMode), so the
	 * value can be judged by eye without a rebuild. 50 is what APursuitAIEnv uses.
	 */
	UPROPERTY(EditAnywhere, Category = "Pursuit|Chase")
	float CatchRadius = 85.0f;

	/**
	 * How far above or below a chaser the player can still be caught, in cm. 0 = no gate.
	 *
	 * This exists because the catch test used to be height-BLIND: TickChaser dropped Z before
	 * measuring it (ToPlayer.Z = 0), which made the catch volume an infinitely tall cylinder.
	 * Two things followed, and both were visible in play:
	 *
	 *   * a chaser standing on the ground beside a raised deck caught a player standing on top
	 *     of it, as soon as the horizontal distance came inside CatchRadius - so the map's
	 *     platforms, which exist precisely so the chaser has something to climb, shielded
	 *     nobody;
	 *   * a jump that merely grazed the lip of a deck on its way past counted as a catch, even
	 *     though the chaser never landed on it.
	 *
	 * The rule is now "same level", measured feet to feet. Both roles are this same class with
	 * the same capsule, so the feet gap IS the actor-origin gap - subtracting a half-height
	 * here would silently halve the gate.
	 *
	 * The value is boxed in on both sides by geometry that already exists on this map:
	 *
	 *   * it must stay ABOVE the tallest ground clutter (~61 cm - the city's kerbs and step
	 *     nosings), or standing on a kerb would make a player uncatchable from the pavement;
	 *   * it must stay BELOW the 110 cm first deck of the generated towers, which is the ledge
	 *     the player is meant to be safe on until the chaser actually climbs up to it.
	 *
	 * 70 sits between the two with room on either side. The consequence worth knowing: the
	 * player's jump apex is 131 cm, so a grounded chaser cannot catch a player at the top of a
	 * jump - jumping buys about a second. That is a dodge rather than a bug, and the chaser
	 * takes the catch back when they land.
	 *
	 * -PursuitCatchHeight=<cm> overrides it per launch and accepts 0, which switches the gate
	 * off and restores the old height-blind behaviour exactly - so the two can be compared
	 * without a rebuild, the same way CatchRadius can. Unset leaves this default alone.
	 */
	UPROPERTY(EditAnywhere, Category = "Pursuit|Chase")
	float CatchHeightTolerance = 70.0f;

	/** Length of the obstacle feelers. Longer sees further, and cuts corners worse. */
	UPROPERTY(EditAnywhere, Category = "Pursuit|Chase")
	float AvoidTraceLength = 300.0f;

	/**
	 * Height the feelers are cast from, in cm above the actor ORIGIN - so, on the default
	 * capsule, 158 cm above the ground.
	 *
	 * Kept as the main feeler because chest height is where buildings and pillars are, and a
	 * tall obstacle is the one a character most obviously has to route around.
	 *
	 * What it cannot see is the class of thing that actually stopped the chasers on the city
	 * map: a 61 cm kerb or step, which blocks a capsule at ground level while sitting entirely
	 * BELOW this beam. The feeler passes over it, SteerAroundObstacles reports a clear path,
	 * the chaser keeps driving straight into it, and the movement component refuses to climb
	 * something taller than MaxStepHeight - so the chaser jams there with velocity zero,
	 * reporting "face 300 cm" as if nothing were in front of it. LowAvoidTraceHeight exists
	 * for that case; this one is the pair to it.
	 */
	UPROPERTY(EditAnywhere, Category = "Pursuit|Chase")
	float AvoidTraceHeight = 70.0f;

	/**
	 * Second avoidance feeler, cast low above the feet - 6 cm by default, well under the 45 cm
	 * step-up the capsule handles for free and under the 20 cm the first version used.
	 *
	 * This is the one that catches anything the capsule physically cannot walk through or
	 * over, whatever the chest beam says. Both feelers run in SteerAroundObstacles and the
	 * nearer obstacle wins, so a kerb is steered around even though the chest beam sees
	 * nothing at all.
	 *
	 * 6 rather than 20, because 20 was measured to be too high. The city map has low lips -
	 * paving seams and step nosings a few centimetres proud - that stop a capsule dead, and
	 * the diagnostic that finally caught one reported "below-ankle face at 3 cm = 26" while
	 * the 20 cm beam saw nothing. A capsule is round at the bottom, so its lowest contact is
	 * made well below any height a beam at 20 cm samples.
	 *
	 * 6 rather than 0 for the opposite failure: a trace at exactly foot level grazes the
	 * ground plane and reports a hit on flat paving, which would make the chaser dodge the
	 * pavement it is walking on. 6 cm clears a flat floor and still catches a lip.
	 */
	UPROPERTY(EditAnywhere, Category = "Pursuit|Chase")
	float LowAvoidTraceHeight = 6.0f;

	/** Chasers push apart within this radius so four of them do not become one. */
	UPROPERTY(EditAnywhere, Category = "Pursuit|Chase")
	float SeparationRadius = 240.0f;

	UPROPERTY(EditAnywhere, Category = "Pursuit|Chase")
	float SeparationWeight = 0.85f;

	/** How hard the chaser will turn to avoid something. Lower reads as more hesitant. */
	UPROPERTY(EditAnywhere, Category = "Pursuit|Chase")
	float AvoidTurnAngle = 55.0f;

	/**
	 * How much of the desired direction to subtract when the obstacle is closer than the
	 * capsule's own radius, i.e. when the chaser is already touching it.
	 *
	 * Rotating a heading cannot help a capsule that is in contact: every diagonal is refused
	 * for the same reason the straight line was. Retracting a little first separates the
	 * contact so a diagonal has somewhere to go. Small on purpose - this is a nudge out of
	 * contact, not a retreat, and the next frame goes straight back to approaching.
	 */
	UPROPERTY(EditAnywhere, Category = "Pursuit|Chase")
	float AvoidBackoffWeight = 0.35f;

	// -- jumping -------------------------------------------------------------
	//
	// Chasers jump, and only over things worth jumping. Without a height test they would
	// hurdle every bin and lamp post in the level, which reads as a bug rather than as
	// agility: the city map's clutter is chest-high to a feeler but ankle-high to a jump.

	/** Master switch, so a jump that misbehaves can be turned off without a rebuild. */
	UPROPERTY(EditAnywhere, Category = "Pursuit|Jump")
	bool bCanJump = true;

	/**
	 * How close to an obstacle a chaser lets itself get before jumping, measured along its
	 * own forward direction. Too short and the jump starts after the capsule has already
	 * stalled against the face; too long and it launches early and lands in front of the
	 * thing it meant to clear.
	 */
	UPROPERTY(EditAnywhere, Category = "Pursuit|Jump")
	float JumpTriggerDistance = 190.0f;

	/**
	 * The tallest top face a chaser will attempt, in cm above its own feet.
	 *
	 * This is a *step-up* budget, not a clearance figure: the arc has to get the feet onto
	 * the top and leave somewhere to land. It is deliberately below the capsule's total
	 * height (176 cm) so a barrier the chaser could not stand on reads as a wall.
	 *
	 * The number is the JUMP ARC's apex less a landing margin, and the arc is not a free
	 * parameter - it falls straight out of the movement component:
	 *
	 *     Move->JumpZVelocity = 640, Move->GravityScale = 1.6, UE DefaultGravityZ = -980
	 *     apex = 640^2 / (2 * 980 * 1.6) = 131 cm above the feet
	 *
	 * So this knob must stay BELOW ~131. Anything it accepts above that is an obstacle the
	 * chaser will commit to, fail to clear, and then lean against with velocity zero - which
	 * is a stall, the failure this whole probe chain exists to avoid, not a wall.
	 *
	 * 150 was on the wrong side of that line and actively harmful. It was raised from 125 on
	 * the argument that 125 sat below an ordinary 150 cm city wall, so the chaser should be
	 * willing to jump those. The premise was right and the conclusion was backwards: this knob
	 * only controls *willingness*, never capability, so raising it did not clear a single wall
	 * - it converted a correct walk-around into a stall for every 120-150 cm obstacle in the
	 * level, and then made that worse by letting the map generator plant test platforms up
	 * there too.
	 *
	 * 120 is the empirically proven ceiling rather than a fitted value: the fixture run has a
	 * chaser clear a 120 cm crate repeatedly, and the Cartoon City run's logged jump heights
	 * top out at exactly 120. It is the apex less ~11 cm, which is what the landing needs.
	 */
	UPROPERTY(EditAnywhere, Category = "Pursuit|Jump")
	float MaxJumpHeight = 120.0f;

	/**
	 * Whether the avoidance steering ignores a face the capsule is allowed to walk up.
	 *
	 * On, a sub-MaxStepHeight face is driven into instead of steered around, on the theory that
	 * the movement component climbs it for free and that steering around it is what produces the
	 * measured stall - the chaser oscillating between the feeler and AvoidBackoffWeight at
	 * exactly CapsuleRadius, vel=0 with want=575, while the player walks off.
	 *
	 * DEFAULT OFF, because the measurement does not support it. Same map, three 240 s runs:
	 *
	 *     version that read the height probe's step branch (the ground 90 cm ahead)   15 stalls
	 *     version that measured at the face the beam actually hit                      4 stalls
	 *     off - the plain steering this replaced                                       2 stalls
	 *
	 * The first version was wrong for a knowable reason (see SteerAroundObstacles: on a kerb
	 * that runs along the foot of a wall it reads the kerb and drives into the wall) and is kept
	 * here as the reason the measurement has to be taken at the face. But the corrected version
	 * still does not beat doing nothing, so it does not ship switched on. Turning it on is a
	 * one-line change and `-PursuitStepOver` does it without a rebuild.
	 *
	 * The honest read: the stall is real and diagnosed, but this is not the fix, and n=1 per arm
	 * means none of these numbers is solid - play mode runs are NOT reproducible, because
	 * APursuitCharacter moves through CharacterMovementComponent and is dt-dependent, and there
	 * is no seed anywhere in it. A controlled A/B needs a seeded or fixed-step play mode first.
	 */
	UPROPERTY(EditAnywhere, Category = "Pursuit|Jump")
	bool bStepOverLowFaces = false;

	/**
	 * How far PAST the obstacle's face the first downward height probe is aimed, in cm.
	 *
	 * Past, not short of: the face feeler reports the distance to the surface, so probing at
	 * exactly that distance lands on the surface itself and probing short of it lands in open
	 * air - where a downward cast finds nothing and the obstacle reads as unjumpable. A few cm
	 * of depth puts the probe unambiguously inside the solid.
	 *
	 * 8 rather than the 25 this used to be, because a single depth is a bet on the obstacle
	 * being thicker than it. It is not a safe bet on a city map: the obstacle the chasers
	 * actually jam against is a 61 cm ledge only a couple of dozen cm deep, so a probe 25 cm in
	 * sailed straight past its far edge, cast down through open air, and reported "nothing
	 * there". See ProbeIntoFaceSteps for how the sweep covers the rest of the range.
	 */
	UPROPERTY(EditAnywhere, Category = "Pursuit|Jump")
	float ProbeIntoFaceDepth = 8.0f;

	/**
	 * How many downward height probes to fire into the obstacle, and how far apart, in cm.
	 *
	 * One probe at one depth answers "how tall is this obstacle" only if the depth happens to
	 * land inside its body, and the depth cannot be chosen correctly in advance because the
	 * obstacle's thickness is exactly what is not known yet - the same chicken-and-egg the
	 * height itself has. The first version of this probe lost a whole debug cycle to it.
	 *
	 * So the probe walks outward from the face in a few short strides and takes the FIRST
	 * solid top it finds, which is by construction the near edge's height. Thin ledges and
	 * deep crates are both covered by the same cast, and a stride that overshoots a thin
	 * obstacle simply finds nothing and is skipped rather than poisoning the answer.
	 */
	UPROPERTY(EditAnywhere, Category = "Pursuit|Jump")
	int32 ProbeIntoFaceSteps = 4;

	/** Spacing between the successive downward probes, in cm. */
	UPROPERTY(EditAnywhere, Category = "Pursuit|Jump")
	float ProbeIntoFaceStepSize = 12.0f;

	/**
	 * How far below the character's own feet the downward height probe may reach, in cm.
	 *
	 * The probe has to terminate BELOW the ground it stands on, or a cast down to exactly foot
	 * level can stop a centimetre or two short of the pavement - find no obstacle and no floor
	 * - and report the whole thing as empty. The measured case: feet at z=-9, ground under them
	 * at z=-10, probe bottom at z=-7. Three centimetres above the floor, and the probe reported
	 * "nothing there" for a 61 cm step it had a face distance for.
	 */
	UPROPERTY(EditAnywhere, Category = "Pursuit|Jump")
	float ProbeBelowFeetDepth = 60.0f;

	/**
	 * Height above the character's FEET at which the obstacle's face is sought, in cm.
	 *
	 * This exists because the avoidance feeler is cast at AvoidTraceHeight above the actor
	 * ORIGIN - which is the capsule centre, 88 cm up - putting it at roughly 158 cm above the
	 * ground. Everything the chaser is supposed to jump lives between MaxStepHeight (45) and
	 * MaxJumpHeight (120), i.e. wholly BELOW that feeler. Reusing the avoidance trace to find
	 * the obstacle's face therefore looks for a waist-high crate with a chest-height beam and
	 * misses it completely: the trace sails over the crate, hits the building behind it, and
	 * the jump logic is handed a wall to consider.
	 *
	 * The symptom is precise and was read wrong for two rounds: the probe log reports a face
	 * distance that proves "there is something ahead" while the height probe - aimed at that
	 * same distance - is inside nothing, so the obstacle is rejected as unjumpable and the
	 * chaser walks round a crate it should have hopped. Both halves were reporting honestly
	 * about two DIFFERENT objects.
	 *
	 * Mid-band rather than at the top: the obstacle's top face is exactly what is unknown at
	 * this point, and aiming at the height we are trying to measure would beg the question.
	 * 90 sits below MaxJumpHeight and above MaxStepHeight, so every obstacle in the jumpable
	 * band presents a face there.
	 */
	UPROPERTY(EditAnywhere, Category = "Pursuit|Jump")
	float JumpFaceProbeHeight = 90.0f;

	/** Minimum gap between two jumps, so a chaser cannot stutter-hop up a wall. */
	UPROPERTY(EditAnywhere, Category = "Pursuit|Jump")
	float JumpCooldownSeconds = 0.85f;

	/**
	 * How far ahead the "is there room to come down" probe looks, as a multiple of the
	 * trigger distance. Landing short of the far edge is what turns a hop into a wall-hug,
	 * so the far side is checked before committing.
	 */
	UPROPERTY(EditAnywhere, Category = "Pursuit|Jump")
	float JumpLandingProbeScale = 1.35f;

	UPROPERTY(EditAnywhere, Category = "Pursuit|Look")
	float LookSensitivity = 1.0f;

	// -- looks ---------------------------------------------------------------

	/**
	 * Tints. The bundled mannequin material exposes no colour parameter, so the colour comes
	 * from the project's own M_PursuitTint - a lit material with a vector parameter named
	 * "Color", which is the same parameter name the RL demo's spheres use.
	 *
	 * It cannot be the engine's BasicShapeMaterial: that one has no bUsedWithSkeletalMesh
	 * usage flag, so an instance of it on a skeletal mesh is silently replaced by the default
	 * material at runtime. The colour is still written and reads back correctly, which is
	 * exactly why this is worth a comment - the code looks right while the render is grey.
	 */
	UPROPERTY(EditAnywhere, Category = "Pursuit|Looks")
	FLinearColor PlayerTint = FLinearColor(0.72f, 0.12f, 0.10f);

	UPROPERTY(EditAnywhere, Category = "Pursuit|Looks")
	FLinearColor ChaserTint = FLinearColor(0.10f, 0.55f, 0.24f);

	// Soft pointers, not hard ones, so pointing the prototype at a different character pack
	// is a property change rather than a code change. The defaults are the rig Epic ships
	// inside the MoverExamples plugin, which is the only full mannequin in a bare install.
	UPROPERTY(EditAnywhere, Category = "Pursuit|Animation")
	TSoftObjectPtr<USkeletalMesh> CharacterMesh;

	UPROPERTY(EditAnywhere, Category = "Pursuit|Animation")
	TSoftObjectPtr<UAnimSequence> IdleAnim;

	UPROPERTY(EditAnywhere, Category = "Pursuit|Animation")
	TSoftObjectPtr<UAnimSequence> WalkAnim;

	UPROPERTY(EditAnywhere, Category = "Pursuit|Animation")
	TSoftObjectPtr<UAnimSequence> RunAnim;

	UPROPERTY(EditAnywhere, Category = "Pursuit|Animation")
	TSoftObjectPtr<UAnimSequence> JumpAnim;

	UPROPERTY(EditAnywhere, Category = "Pursuit|Animation")
	TSoftObjectPtr<UAnimSequence> FallAnim;

	/**
	 * Optional third locomotion tier, played above RunToSprintThreshold.
	 *
	 * Empty on the mannequin, which ships walk and run and nothing else. The pack rigs have a
	 * genuine sprint animation, and the player's Shift key is worth exactly that much: without
	 * a sprint clip, Shift only makes the run clip play faster, which reads as the same
	 * animation sped up rather than as running harder.
	 */
	UPROPERTY(EditAnywhere, Category = "Pursuit|Animation")
	TSoftObjectPtr<UAnimSequence> SprintAnim;

	/**
	 * Landing clip.
	 *
	 * The mannequin's only landing sequence, MM_Land, is additive, and the single-node
	 * animation mode this class uses cannot play additive sequences - the engine warns about it
	 * every time and the game is left with a warning pinned to the screen. So the mannequin
	 * leaves this empty and goes straight from falling to walking, which is what that rig
	 * allows. The pack rigs do have a usable one: the Tiny Hero's Anim_JumpEnd_Normal, which
	 * LandHoldSeconds below then gives time to finish.
	 */
	UPROPERTY(EditAnywhere, Category = "Pursuit|Animation")
	TSoftObjectPtr<UAnimSequence> LandAnim;

	/** Speed below which the character reads as standing still. */
	UPROPERTY(EditAnywhere, Category = "Pursuit|Animation")
	float IdleThreshold = 20.0f;

	/** Speed above which the run clip replaces the walk clip. */
	UPROPERTY(EditAnywhere, Category = "Pursuit|Animation")
	float WalkToRunThreshold = 340.0f;

	/** Speed above which the sprint clip replaces the run clip, when there is one. */
	UPROPERTY(EditAnywhere, Category = "Pursuit|Animation")
	float RunToSprintThreshold = 820.0f;

	/**
	 * Deadband, in cm/s, on both thresholds above.
	 *
	 * The clips engage Hysteresis above a threshold and release at it. Without this the state
	 * machine strobes: a speed resting on the threshold - which is exactly where it sits while
	 * someone runs into a wall, and where the collision response holds the capsule - crosses and
	 * re-crosses it every frame. Measured on the first build of this: six clip changes in 180 ms,
	 * Idle/Walk/Idle/Walk/Idle/Walk, which on screen is a character vibrating between two poses.
	 */
	UPROPERTY(EditAnywhere, Category = "Pursuit|Animation")
	float SpeedHysteresis = 60.0f;

	/**
	 * Vertical speed, up and down, that separates the jump clip from the fall clip.
	 *
	 * Two values rather than one for the same reason as SpeedHysteresis, and the apex is where
	 * it bites: Velocity.Z passes within a few cm/s of zero for several frames at the top of
	 * every arc, so a single threshold there produces a Jump/Fall/Jump/Fall strobe.
	 */
	UPROPERTY(EditAnywhere, Category = "Pursuit|Animation")
	float RisingSpeed = 30.0f;

	UPROPERTY(EditAnywhere, Category = "Pursuit|Animation")
	float FallingSpeed = -60.0f;

	/**
	 * The ground speed each locomotion clip is assumed to have been authored for, used to
	 * scale playback.
	 *
	 * ApplyHeroModel() overwrites all three from the rig's table, because a stride belongs to a
	 * rig and not to the class. They are the knobs for foot sliding: raise one and the legs slow
	 * down at that speed. The mannequin's values came from tuning against its own clips; the
	 * pack rigs currently inherit numbers of the same magnitude that have *not* been tuned, so
	 * expect their feet to skate a little. See docs/PLAY_MAP.md for how to measure them.
	 */
	UPROPERTY(EditAnywhere, Category = "Pursuit|Animation")
	float WalkClipSpeed = 220.0f;

	UPROPERTY(EditAnywhere, Category = "Pursuit|Animation")
	float RunClipSpeed = 600.0f;

	UPROPERTY(EditAnywhere, Category = "Pursuit|Animation")
	float SprintClipSpeed = 700.0f;

	/** How long the land clip is held before locomotion takes over again. */
	UPROPERTY(EditAnywhere, Category = "Pursuit|Animation")
	float LandHoldSeconds = 0.4f;

private:
	// -- input handlers ------------------------------------------------------
	void InputMoveForward(float Value);
	void InputMoveRight(float Value);
	void InputTurn(float Value);
	void InputLookUp(float Value);
	void InputJumpPressed();
	void InputJumpReleased();
	void InputSprintPressed();
	void InputSprintReleased();

	// -- pieces --------------------------------------------------------------
	void ApplyRole();
	void LoadVisuals();
	void ApplyTint();
	void TickChaser(float DeltaSeconds);
	void UpdateAnimation(float DeltaSeconds);

	/** Plays Clip if it is not already the one playing, then scales it to the current speed. */
	void PlayClip(UAnimSequence* Clip, bool bLoop);

	/** Distance a feeler cast along Direction runs before it hits static geometry. */
	float ProbeObstacle(const FVector& Direction) const;

	/**
	 * How tall the thing straight ahead is, as a world-space Z of its top face.
	 *
	 * This is the whole reason the chasers do not simply jump at everything: the city map is
	 * full of bins and lamp posts that stop a chest-height feeler (so the steering sees an
	 * obstacle) but are either too short to be worth a jump or far too tall to clear. A
	 * feeler alone cannot tell those apart, so a second, vertical trace asks the question the
	 * decision actually depends on.
	 *
	 * OutFaceDistance receives how far away that obstacle's face is - the caller needs it to
	 * work out where the chaser would land, and re-tracing to find it would risk the two
	 * probes disagreeing about which obstacle they are looking at.
	 *
	 * Returns -FLT_MAX when nothing is ahead within AvoidTraceLength, or nothing close enough
	 * to be worth jumping. OutFaceDistance is then untouched.
	 */
	float ProbeObstacleTop(const FVector& Direction, float& OutFaceDistance) const;

	/**
	 * Whether an obstacle tops out low enough to jump onto or over.
	 *
	 * The test is against the capsule's step-up height, not its total height: a chaser whose
	 * feet could clear the top has to also be able to *land* on it, and a barrier taller than
	 * the capsule's half height is a wall to walk around, not a kerb to hop.
	 */
	bool IsObstacleJumpable(float ObstacleTopZ) const;

	/**
	 * Rate-limited, one-line-per-second dump of what the jump decision just measured.
	 *
	 * Exists because "the chaser did not jump" has several very different causes - the feeler
	 * never reached the obstacle, it reached it too late to act, or the top is outside the
	 * jumpable band - and from outside they are indistinguishable. Recording the numbers next
	 * to the thresholds they are compared against makes that diagnosis a read rather than a
	 * round of guessing.
	 */
	void LogJumpProbe(const FVector& Direction, float ObstacleDistance, float ObstacleTopZ, float DistanceToPlayer);

	/** World time of the last LogJumpProbe line, so it fires at most once a second. */
	float LastJumpProbeLogTime = -1000.0f;

	/**
	 * World time of the last "in reach but not on the player's level" line.
	 *
	 * Throttled like the jump probe, and needed for the same reason: without it, a chaser held
	 * at the foot of a deck by CatchHeightTolerance is silent, and silence is indistinguishable
	 * between "the gate is doing its job" and "the gate is not running".
	 */
	float LastHoldLogTime = -1000.0f;

	/** Minimum gap between probe log lines, in seconds. Also the stall counter's step. */
	static constexpr float ProbeLogInterval = 1.0f;

	/**
	 * True while the chaser has committed to a jump and must not be steered around.
	 *
	 * Latched rather than recomputed every frame, and that is the whole point. The jump probe
	 * and the avoidance feeler look along slightly different rays, and on a diagonal approach
	 * the crate can leave the jump probe's line for a frame - at which point a stateless
	 * "is it jumpable right now" check would hand the decision back to the steering, which
	 * would peel further off, and the crate would never return to either probe's line. The
	 * chaser would orbit a crate forever.
	 *
	 * The latch is held only while the obstacle stays within reach of the forward probe, so a
	 * chaser that commits and then has the obstacle removed (a crate rebuilt between rounds)
	 * does not drive at nothing for the rest of the round.
	 */
	bool bJumpCommitted = false;

	/** Blends the straight-ahead direction away from whatever the feelers ran into. */
	FVector SteerAroundObstacles(const FVector& Desired) const;

	/** Sum of pushes away from the other chasers, normalised by how close they are. */
	FVector SeparationPush() const;

	/** Tells the game mode the player has been caught, at most once per catch cooldown. */
	void ReportCatch();

	/**
	 * Decides whether this frame's obstacle is worth a jump and, if so, jumps.
	 *
	 * Split out of TickChaser because it is the only part of chasing that reads height, and
	 * because "the chaser is hopping at nothing again" is far easier to debug from a function
	 * whose whole job is the decision. Called with the *unsteered* direction to the player:
	 * the decision is about what is between the chaser and the player, not about which way
	 * the avoidance happened to dodge.
	 *
	 * Returns whether the chaser is now committed to that obstacle. Commit and Jumped both
	 * mean "do not steer around it" - see EPursuitJumpVerdict for why burying that inside this
	 * function was not an option.
	 *
	 * HeightGapToPlayer is the player's vertical offset from this chaser, in cm, handed in
	 * rather than recomputed because TickChaser has just measured it for the catch. It is used
	 * for one decision only - whether "the player is already close enough that a hop is
	 * pointless" - and that decision cannot honestly be taken from the horizontal distance
	 * alone. See the note in the body: a chaser under a deck with the player on top of it is
	 * centimetres away horizontally and a whole platform away in the only direction that
	 * matters.
	 */
	EPursuitJumpVerdict TickChaserJump(
		const FVector& ToPlayerDirection, float DistanceToPlayer, float HeightGapToPlayer);

	/**
	 * Which ground clip the state machine has settled on.
	 *
	 * This is memory, and the hysteresis needs it: deciding purely from the current speed means
	 * there is no way to tell "walking slowly" from "standing up from a stop", and a speed on
	 * the boundary gets answered one way then the other on consecutive frames.
	 */
	enum class ELocomotion : uint8
	{
		Idle,
		Walk,
		Run,
		Sprint
	};

	ELocomotion Locomotion = ELocomotion::Idle;

	/** Which of the two airborne clips is playing. Latched between RisingSpeed and FallingSpeed. */
	bool bAirborneRising = true;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Pursuit|Camera", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<USpringArmComponent> CameraBoom;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Pursuit|Camera", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UCameraComponent> FollowCamera;

	/**
	 * Eye-level camera, used when the mode resolves to FirstPerson - which is what the Auto
	 * rule picks for an editor session.
	 *
	 * It casts no geometry and hides nothing: the body stays where it is, so the near plane
	 * sits inside the character's head. That is acceptable here and is not worth a
	 * first-person mesh pass - the mode exists to look out of the character, and no recording
	 * picks it.
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Pursuit|Camera", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UCameraComponent> FirstPersonCamera;

	/**
	 * The detached spectator camera: high above the chase, looking down at it.
	 *
	 * Placed in world space every tick (see TickGodCamera), which is why it needs absolute
	 * location and rotation. Without them it would inherit the capsule's position and yaw,
	 * and "high above the chase" would come out as "attached to the character's back", which
	 * is the camera it exists to replace.
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Pursuit|Camera", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UCameraComponent> GodCamera;

	/**
	 * Which camera this pawn renders from. Auto is the rule; the rest are the override, and
	 * -PursuitCamera=god|follow|first sets it from the command line.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pursuit|Camera", meta = (AllowPrivateAccess = "true"))
	EPursuitCameraMode CameraMode = EPursuitCameraMode::Auto;

	/**
	 * Distance from the god camera to the point it looks at, in cm. Larger = wider shot.
	 *
	 * With GodCameraPitch and GodCameraFOV this is the whole of the framing: 2900 at pitch -82
	 * and FOV 34 puts about 1770 x 1000 cm of street in frame - the chaser ring (950) and room
	 * to run - with a character standing about a sixth of the frame height.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pursuit|Camera|God", meta = (AllowPrivateAccess = "true"))
	float GodCameraDistance = 2900.0f;

	/**
	 * God-camera pitch, in degrees. -90 is straight down.
	 *
	 * The value is an occlusion budget, not a taste. A steeper angle shrinks the horizontal
	 * run between the lens and the street, and the horizontal run is the room a building has
	 * to get in the way. Over a 2900 cm arm at -82 that run is 404 cm, so it takes a building
	 * within roughly 3 m of the chase to matter at all. Two entries in this comment have
	 * already been wrong by being reasoned rather than measured - 1110 cm at the first setting
	 * and 830 at the second both read as safely clear, and both filmed the wall of the block
	 * the player start sits beside, because where the chase goes is not where a person would
	 * guess it goes. So this is a starting angle and not a promise: the probe in TickGodCamera
	 * climbs a ladder of steeper angles whenever a building gets in the way, all the way to
	 * -89.5, which is the one angle that cannot be occluded (25 cm of reach, inside the
	 * capsule's own 34 cm).
	 *
	 * -PursuitGodPitch=<deg> overrides it on the command line, so the look can be retuned
	 * without a rebuild. The value is clamped to -89.5 for the reason above.
	 *
	 * Height comes out at roughly 2870: below Cartoon City's two 5700 cm towers and above its
	 * 2441 cm mid-rises, which is what makes the street read as a canyon rather than as a roof.
	 * Steeper still would be safer and is not free - it flattens the shot toward a map.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pursuit|Camera|God", meta = (AllowPrivateAccess = "true"))
	float GodCameraPitch = -82.0f;

	/**
	 * God-camera yaw, in degrees, and fixed on purpose.
	 *
	 * A camera that turns to keep its back to the chase turns the whole city with it, and
	 * after ten seconds a viewer has no idea which way is which. Holding the yaw means the
	 * buildings stay put and the movement reads as movement.
	 *
	 * It is also half the occlusion budget, which is not obvious from the name: the lens sits
	 * at this bearing from the chase, so 45 degrees points it over the shoulder of a city whose
	 * streets run north to south. 90 would put it up the street instead - worth trying with
	 * -PursuitGodYaw=90 before changing the default, since how much a building is in the way is
	 * the difference between a clip and a wall.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pursuit|Camera|God", meta = (AllowPrivateAccess = "true"))
	float GodCameraYaw = 45.0f;

	/**
	 * God-camera field of view, in degrees. Wider shows more of the city and shrinks the pawns.
	 *
	 * This is the knob that frames the shot, and it is deliberately the narrow one rather than
	 * the distance. Moving the camera closer would frame the chase just as well and would undo
	 * the whole occlusion argument above - the horizontal run between the lens and the street
	 * shrinks with the arm. Narrowing the lens instead changes the framing and leaves the
	 * geometry exactly where it was measured to be safe: the same 2870 cm of altitude, the same
	 * 404 cm of offset, the same clear line.
	 *
	 * UE's default axis constraint is horizontal, so 34 here is 1770 cm across by 1000 cm down
	 * at the focus - the chaser ring at 950 cm fits, and a 170 cm character stands about a
	 * sixth of the frame height.
	 *
	 * 52 was the first try and it filmed a street with three 30-pixel pawns somewhere in it -
	 * a god's-eye view, which is not the same thing as a readable one. 34 puts a character at
	 * roughly a sixth of the frame height, which is where the movement reads.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pursuit|Camera|God", meta = (AllowPrivateAccess = "true"))
	float GodCameraFOV = 34.0f;

	/**
	 * How fast the god camera catches up to the chase. Higher is tighter, lower is silkier.
	 *
	 * It is smoothing the *focus point*, not the camera, and it is here because the chase does
	 * not move smoothly: a character's position is fixed up in 20 Hz steps and a teleport on
	 * every catch, and a camera bolted rigidly to that shakes. Deliberately low enough to lag
	 * visibly on a sprint - the lag is what tells a viewer the camera is not the player.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pursuit|Camera|God", meta = (AllowPrivateAccess = "true"))
	float GodCameraLagSpeed = 4.5f;

	/**
	 * A focus-point jump larger than this is a teleport, not movement, and the camera snaps.
	 *
	 * ResetRound puts the player back on the start spot from wherever they were caught, and a
	 * smoothed camera would spend the next several seconds flying the length of the street to
	 * catch up. That flight is the one thing a spectator camera must never do: it turns a
	 * reset into a shot of empty road.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pursuit|Camera|God", meta = (AllowPrivateAccess = "true"))
	float GodCameraSnapDistance = 1500.0f;

	/**
	 * How fast the camera eases back to the requested angle once the way is clear.
	 *
	 * Asymmetric on purpose - the probe snaps it steep and this eases it back. A camera that
	 * eased into the steepening would keep a wall on screen for a few frames every time the
	 * chase passed a building, which is the exact artifact the probe is here to remove; a
	 * camera that snapped back would pop. 3 returns to the requested angle in well under a
	 * second, which is the point: time spent steep is time the clip does not look like the
	 * rest of the clip.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pursuit|Camera|God", meta = (AllowPrivateAccess = "true"))
	float GodCameraReturnSpeed = 3.0f;

	/** Where the eye sits, in cm above the capsule centre. Capsule half-height is 88. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pursuit|Camera", meta = (AllowPrivateAccess = "true"))
	float FirstPersonEyeHeight = 70.0f;

public:
	/** The mode this pawn actually resolved to. God until BeginPlay has decided. */
	EPursuitCameraMode GetResolvedCameraMode() const { return ResolvedCameraMode; }

	/** "god" / "follow" / "first" / "auto", for log lines that have to be readable. */
	static const TCHAR* CameraModeName(EPursuitCameraMode Mode);

private:
	/**
	 * Decides which camera this pawn renders from and switches exactly one of them on.
	 *
	 * Called from BeginPlay, for the player only. It has to be BeginPlay and not the
	 * constructor: the command line is what usually decides, and the Auto rule reads
	 * GIsEditor - neither of which is a sensible thing to evaluate while the class default
	 * object is being built.
	 */
	void ApplyCameraMode();

	/**
	 * Applies -PursuitGodPitch= and -PursuitGodYaw=, if they were passed.
	 *
	 * Called from ApplyCameraMode before the mode is resolved, because the override has to be
	 * in place before the first TickGodCamera places the camera. See the definition for why
	 * exactly these two numbers get a switch.
	 */
	void ApplyGodCameraOverrides();

	/** Places the god camera for this frame. No-op unless the mode resolved to God. */
	void TickGodCamera(float DeltaSeconds);

	/**
	 * Auto - the editor session gets the first-person camera, everything else gets the god view
	 * - unless -PursuitCamera=... on the command line overrides it outright.
	 */
	EPursuitCameraMode ResolveCameraMode() const;

	/** The god camera's smoothed focus point, and whether it holds a position yet. */
	FVector GodFocus = FVector::ZeroVector;
	bool bGodFocusValid = false;

	/**
	 * The pitch actually in use, in degrees.
	 *
	 * Separate from GodCameraPitch because the two are different things: one is what the shot
	 * is asked for, the other is what the occlusion probe settled on - a steeper angle whenever
	 * the requested one has a building in it. Keeping both means the requested framing survives
	 * the detour and comes back on its own.
	 */
	float GodPitch = 0.0f;
	bool bGodPitchValid = false;

	/**
	 * Throttles the "the view was pulled in" log line.
	 *
	 * The probe can fire most frames while the chase runs past a building, and a line per frame
	 * is not a diagnostic, it is noise. Once a second is what makes "the camera was clipped for
	 * 90% of this run" a thing that can be read off the log afterwards.
	 */
	float GodClipLogTimer = 0.0f;

	/**
	 * Set once BeginPlay has picked a camera.
	 *
	 * The tick path checks it, because "no camera mode resolved yet" and "the mode is Follow"
	 * would otherwise look the same, and the second one must not run the god camera.
	 */
	bool bCameraModeResolved = false;

	/** What ApplyCameraMode settled on. Kept so Tick knows whether to drive the god camera. */
	EPursuitCameraMode ResolvedCameraMode = EPursuitCameraMode::God;

	// Resolved once at BeginPlay. Kept as properties rather than raw pointers because the
	// soft pointers above do not keep the packages alive; a raw pointer would be a dangling
	// one after the next GC pass.
	UPROPERTY(Transient)
	TObjectPtr<UAnimSequence> IdleClip;

	UPROPERTY(Transient)
	TObjectPtr<UAnimSequence> WalkClip;

	UPROPERTY(Transient)
	TObjectPtr<UAnimSequence> RunClip;

	UPROPERTY(Transient)
	TObjectPtr<UAnimSequence> JumpClip;

	UPROPERTY(Transient)
	TObjectPtr<UAnimSequence> FallClip;

	UPROPERTY(Transient)
	TObjectPtr<UAnimSequence> SprintClip;

	UPROPERTY(Transient)
	TObjectPtr<UAnimSequence> LandClip;

	UPROPERTY(Transient)
	TObjectPtr<UAnimSequence> CurrentClip;

	/** Paces how often a chaser is allowed to report a catch. */
	float CatchCooldown = 0.0f;

	/** Paces how often a chaser is allowed to jump. Counts down in Tick. */
	float JumpCooldown = 0.0f;

	/** Running total of jumps this chaser has made, so a run's jump rate is readable. */
	int32 JumpCount = 0;

	/**
	 * Speed below which a chaser counts as stalled, in cm/s squared.
	 *
	 * 100 = 10 cm/s. Deliberately far below walking pace: this is meant to catch a character
	 * that is genuinely not moving, not one that is merely slowing into a turn.
	 */
	static constexpr float BlockedSpeedSquared = 100.0f;

	/**
	 * How long a chaser must stay stalled before it is worth reporting, in seconds.
	 *
	 * Three seconds, against a once-a-second sample. Two earlier values were tried and both
	 * were wrong in opposite directions:
	 *   * per-frame, which fired on the single low-speed frame every chaser spends at rest
	 *     after spawning or being reset, turning a healthy run into ninety lines of alarm;
	 *   * one second, which still fired on the ±55 degree avoidance slide - a chaser turning
	 *     into a detour momentarily nets close to zero speed, and a run of 120 s produced 48
	 *     warnings for chasers that were all moving again a beat later.
	 *
	 * Three seconds of no movement with a clear path in front is not a turn, not a reset, and
	 * not a stutter. It is a wedge, and it is worth a line in the log.
	 */
	static constexpr float BlockedReportSeconds = 3.0f;

	/** Counts seconds of continuous stall, reset the moment the chaser moves. */
	float BlockedTime = 0.0f;

	/**
	 * Logs the identity of the first thing the jump face feeler hits, once per chaser.
	 *
	 * mutable because ProbeObstacleTop is a const query - it asks the world a question and
	 * changes nothing - and a once-per-chaser log latch is bookkeeping about the ASKING, not
	 * about the character. Making the probe non-const so it could write this would be the
	 * tail wagging the dog.
	 */
	mutable bool bHasLoggedFaceIdentity = false;

	/** Logs the first failed height probe in full, once per chaser. mutable for the reason above. */
	mutable bool bHasLoggedHeightMiss = false;

	/** Counts down while the land clip plays, so locomotion does not cut it off. */
	float LandHold = 0.0f;

	bool bWasFalling = false;
};
