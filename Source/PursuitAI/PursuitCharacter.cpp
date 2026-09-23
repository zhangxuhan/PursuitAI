// Copyright Epic Games, Inc. All Rights Reserved.

#include "PursuitCharacter.h"

#include "PursuitAI.h"
#include "PursuitPlayGameMode.h"

#include "AIController.h"
#include "Animation/AnimSequence.h"
#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/SpringArmComponent.h"
#include "Kismet/GameplayStatics.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Misc/App.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

/**
 * The steepest pitch the god camera will ever pick, shared because two places have to agree
 * on it: the fallback chain in TickGodCamera, which clamps to it, and the -PursuitGodPitch
 * switch, which must not let a hand-set angle go past the value the occlusion argument covers.
 *
 * At -89.5 the arm's horizontal reach is 2900 * cos(89.5 deg) = 25 cm, and the character's
 * capsule radius is 34, so a lens at this angle is always at least 9 cm clear of whatever wall
 * the pawn is standing against. That is the one angle that cannot be occluded, and it is the
 * bottom of the ladder rather than a guess.
 */
constexpr float GodCameraSteepestPitch = -89.5f;

// The rig table (PursuitCharacterPaths) lives in PursuitHeroAssets.h now, shared
// verbatim with the v2 agent (APursuitCharAgent) so both classes can never drift.
#include "PursuitHeroAssets.h"

APursuitCharacter::APursuitCharacter()
{
	PrimaryActorTick.bCanEverTick = true;

	// Manny is about 180 cm tall with his origin between the feet, so the capsule is trimmed
	// to match. A default 96/42 capsule leaves him visibly floating.
	GetCapsuleComponent()->InitCapsuleSize(34.0f, 88.0f);

	bUseControllerRotationPitch = false;
	bUseControllerRotationRoll = false;

	// Automatic possession is off; the game mode does it explicitly at the spawn site. The
	// controller class is set because that is what SpawnDefaultController() instantiates for a
	// chaser - the player pawn is possessed by the player controller and never asks for one.
	AIControllerClass = AAIController::StaticClass();
	AutoPossessAI = EAutoPossessAI::Disabled;

	UCharacterMovementComponent* Move = GetCharacterMovement();
	Move->GravityScale = 1.6f;
	Move->JumpZVelocity = 640.0f;
	Move->AirControl = 0.35f;
	Move->MaxWalkSpeed = PlayerSpeed;
	Move->BrakingDecelerationWalking = 2200.0f;
	Move->bOrientRotationToMovement = false;
	Move->RotationRate = FRotator(0.0f, 520.0f, 0.0f);

	// Switch for the A/B on the walkable-face short-circuit in SteerAroundObstacles. Parsed here
	// rather than in the game mode because the flag is a property of the chaser, so any spawn
	// path honours it. Off (the default, see the header) is the pre-change behaviour exactly,
	// which is what makes a comparison mean anything once play mode can be seeded.
	bStepOverLowFaces = FParse::Param(FCommandLine::Get(), TEXT("PursuitStepOver"));

	CameraBoom = CreateDefaultSubobject<USpringArmComponent>(TEXT("CameraBoom"));
	CameraBoom->SetupAttachment(RootComponent);
	CameraBoom->TargetArmLength = 420.0f;
	CameraBoom->SocketOffset = FVector(0.0f, 0.0f, 65.0f);
	CameraBoom->bUsePawnControlRotation = true;

	// Position lag only, not rotation lag. Rotation lag on the control rotation is what makes
	// mouse look feel like it is dragging behind the hand, and it buys nothing here; position
	// lag is the part that matters, because it is what stops the arm snapping in and out as
	// the collision test is tripped by a wall going past.
	CameraBoom->bEnableCameraLag = true;
	CameraBoom->CameraLagSpeed = 12.0f;
	CameraBoom->bEnableCameraRotationLag = false;
	CameraBoom->CameraLagMaxDistance = 200.0f;

	FollowCamera = CreateDefaultSubobject<UCameraComponent>(TEXT("FollowCamera"));
	FollowCamera->SetupAttachment(CameraBoom, USpringArmComponent::SocketName);
	FollowCamera->bUsePawnControlRotation = false;

	// The other two cameras exist on every pawn because default subobjects are the only thing
	// the constructor can make: a component added at BeginPlay has to be registered by hand
	// and cannot be a UPROPERTY anyone can set, which is worse than two dormant components on
	// a chaser. Both start inactive, and ApplyCameraMode switches at most one of the three on.
	//
	// Exactly one, and that is not tidiness. APawn::CalcCamera renders from the first ACTIVE
	// camera component it finds and component order is creation order - FollowCamera is built
	// first - so leaving it active alongside another would silently win and make the god camera
	// look like it does nothing at all.
	FirstPersonCamera = CreateDefaultSubobject<UCameraComponent>(TEXT("FirstPersonCamera"));
	FirstPersonCamera->SetupAttachment(RootComponent);
	FirstPersonCamera->SetRelativeLocation(FVector(0.0f, 0.0f, FirstPersonEyeHeight));
	// Takes the controller's rotation outright rather than inheriting the capsule's yaw, so
	// the view pitches with the mouse the way an eye does.
	FirstPersonCamera->bUsePawnControlRotation = true;
	FirstPersonCamera->SetActive(false);

	GodCamera = CreateDefaultSubobject<UCameraComponent>(TEXT("GodCamera"));
	GodCamera->SetupAttachment(RootComponent);
	// Absolute, because TickGodCamera places this one in world space and it must not then be
	// dragged around by the capsule it happens to be parented to.
	GodCamera->SetUsingAbsoluteLocation(true);
	GodCamera->SetUsingAbsoluteRotation(true);
	// Not the control rotation: the god camera is aimed by its own pitch/yaw properties, and
	// bUsePawnControlRotation would overwrite that from the player's mouse every frame.
	GodCamera->bUsePawnControlRotation = false;
	GodCamera->SetActive(false);

	USkeletalMeshComponent* MeshComponent = GetMesh();
	MeshComponent->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	MeshComponent->SetRelativeLocation(FVector(0.0f, 0.0f, -88.0f));
	// The rig is authored facing +Y, so a quarter turn puts it in line with the capsule's +X.
	// Every pack rig and the mannequin share this - measured in tools/inspect_hero_facing.py -
	// so the yaw is a constant here and ApplyHeroModel leaves it alone.
	MeshComponent->SetRelativeRotation(FRotator(0.0f, -90.0f, 0.0f));

	// The rig (mesh, clips, clip speeds, tint) is chosen by HeroModel. ApplyHeroModel fills the
	// soft pointers from that choice. Called here so a freshly constructed pawn is already
	// correct, and again in BeginPlay after the game mode has had a chance to set HeroModel on a
	// chaser that it spawned.
	ApplyHeroModel();
}

void APursuitCharacter::BeginPlay()
{
	Super::BeginPlay();

	ApplyRole();
	ApplyHeroModel();
	LoadVisuals();
	if (bTintCharacter)
	{
		ApplyTint();
	}

	// After ApplyRole, which is what turns the chaser's cameras off: this then turns exactly
	// one back on for the player, and does nothing at all for a chaser.
	if (!bIsChaser)
	{
		ApplyCameraMode();
	}
}

void APursuitCharacter::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	if (CatchCooldown > 0.0f)
	{
		CatchCooldown = FMath::Max(0.0f, CatchCooldown - DeltaSeconds);
	}

	if (JumpCooldown > 0.0f)
	{
		JumpCooldown = FMath::Max(0.0f, JumpCooldown - DeltaSeconds);
	}

	if (bIsChaser)
	{
		TickChaser(DeltaSeconds);
	}

	UpdateAnimation(DeltaSeconds);

	if (!bIsChaser)
	{
		// A pawn that arrived after its own BeginPlay still needs a camera decision. Nothing
		// does that today, and this costs one bool to stop depending on it.
		if (!bCameraModeResolved)
		{
			ApplyCameraMode();
		}

		if (ResolvedCameraMode == EPursuitCameraMode::God)
		{
			TickGodCamera(DeltaSeconds);
		}
	}
}

// ---------------------------------------------------------------------------
// Role
// ---------------------------------------------------------------------------

void APursuitCharacter::ApplyRole()
{
	UCharacterMovementComponent* Move = GetCharacterMovement();
	if (!Move)
	{
		return;
	}

	Move->MaxWalkSpeed = bIsChaser ? ChaserSpeed : PlayerSpeed;

	if (bIsChaser)
	{
		// No camera on a chaser. Four pawns all claiming a view target is four pawns fighting
		// over the viewport. All four of them, not just the follow rig: APawn::CalcCamera
		// renders from whichever camera component is active, and a chaser left holding the
		// god camera would be a chaser trying to render the picture.
		if (CameraBoom) { CameraBoom->SetActive(false); }
		if (FollowCamera) { FollowCamera->SetActive(false); }
		if (FirstPersonCamera) { FirstPersonCamera->SetActive(false); }
		if (GodCamera) { GodCamera->SetActive(false); }

		// Chasers face where they are going; the player faces where they are looking.
		Move->bOrientRotationToMovement = true;
		Move->RotationRate = FRotator(0.0f, 520.0f, 0.0f);
		bUseControllerRotationYaw = false;
	}
	else
	{
		Move->bOrientRotationToMovement = false;
		bUseControllerRotationYaw = true;
	}
}

// ---------------------------------------------------------------------------
// Camera
// ---------------------------------------------------------------------------

const TCHAR* APursuitCharacter::CameraModeName(EPursuitCameraMode Mode)
{
	// A switch and not an array index: adding a member to the enum then makes this fail to
	// compile rather than print the wrong name.
	switch (Mode)
	{
	case EPursuitCameraMode::Follow:      return TEXT("follow");
	case EPursuitCameraMode::FirstPerson: return TEXT("first");
	case EPursuitCameraMode::God:         return TEXT("god");
	default:                              return TEXT("auto");
	}
}

EPursuitCameraMode APursuitCharacter::ResolveCameraMode() const
{
	// The command line wins over everything, so any single run can be forced regardless of
	// what kind of session it is - which is what makes an A/B on the camera possible without
	// a rebuild. An unrecognised word is reported rather than ignored: "I passed the flag and
	// nothing changed" is the exact failure this switch exists to make impossible.
	FString Requested;
	if (FParse::Value(FCommandLine::Get(), TEXT("PursuitCamera="), Requested,
		/*bShouldStopOnSeparator=*/false))
	{
		if (Requested.Equals(TEXT("god"), ESearchCase::IgnoreCase))
		{
			return EPursuitCameraMode::God;
		}
		if (Requested.Equals(TEXT("follow"), ESearchCase::IgnoreCase))
		{
			return EPursuitCameraMode::Follow;
		}
		if (Requested.Equals(TEXT("first"), ESearchCase::IgnoreCase)
			|| Requested.Equals(TEXT("firstperson"), ESearchCase::IgnoreCase))
		{
			return EPursuitCameraMode::FirstPerson;
		}
		if (!Requested.Equals(TEXT("auto"), ESearchCase::IgnoreCase))
		{
			UE_LOG(LogPursuitAI, Warning,
				TEXT("PursuitCamera: '%s' is not a camera - expected god, follow, first or auto. ")
				TEXT("Falling back to the rule."),
				*Requested);
		}
	}

	if (CameraMode != EPursuitCameraMode::Auto)
	{
		return CameraMode;
	}

	// The rule: an editor session gets the FIRST-PERSON camera, everything else gets the god
	// view. Two different jobs, and the split was asked for rather than derived.
	//
	// First person in the editor because that is the session driven by hand from behind a
	// monitor, and eye level is where the city's scale - 30 m street, 57 m towers - is legible
	// without the character's own back occupying the middle of the screen.
	//
	// The god view outside it because a recorded run is something somebody watches from
	// outside, and the pawn's own camera is exactly what put the back of a car in front of the
	// chase in the last clip.
	//
	// GIsEditor is the whole test and it is the right one rather than a flag: it is true in
	// the editor including Play In Editor, and false for `UnrealEditor.exe -game` and for a
	// packaged build. So tools/record_city.py lands on the god camera without asking for it,
	// and the person who double-clicks the project and presses Play lands on the eye-level
	// one. -PursuitCamera overrides both, which is what tools/play.bat uses to ask for the
	// third-person spring arm in the -game window - the one camera that is playable there,
	// since the god view is detached and the first-person one is aimed by mouse.
	const bool bEditorSession = GIsEditor && !FApp::IsGame();
	return bEditorSession ? EPursuitCameraMode::FirstPerson : EPursuitCameraMode::God;
}

void APursuitCharacter::ApplyGodCameraOverrides()
{
	// Two switches, because these two numbers have no other way in. The pawn is a plain C++
	// class with no Blueprint, so an EditAnywhere property is only reachable by recompiling -
	// and "the angle is wrong" is a judgement nobody can make until they have seen a
	// recording, which means a rebuild, a re-record and a re-watch per attempt. As switches
	// it is one flag on the next run.
	//
	// Pitch and yaw and not distance or FOV: those two change the framing without changing
	// what is in the way, and a wrong number there is obvious enough to fix in the source.
	// The pitch is the occlusion knob and the yaw is what decides which side of the street the
	// lens ends up over - it is worth trying 90 here, because this map's streets run north to
	// south and a 45 degree yaw therefore puts the lens over a building more often than over
	// the road.
	//
	// The pitch is clamped to the ladder's own floor: an angle steeper than
	// GodCameraSteepestPitch is outside the argument that makes the fallback safe, and a
	// switch that can silently break a guarantee is worse than no switch.
	// NOTE the four-argument call does not exist for numbers. FParse::Value has a float and a
	// double overload and both take three arguments; only the FString and TCHAR* ones take a
	// trailing bShouldStopOnSeparator. Adding it here is a C2665 that reads like the overload
	// is missing entirely, which is what it looks like - it is not missing, it is a different
	// shape. The separator argument is a string problem anyway (a comma would cut the text
	// short); reading a number stops at the end of the number, so nothing is needed here.
	float Value = 0.0f;
	if (FParse::Value(FCommandLine::Get(), TEXT("PursuitGodPitch="), Value))
	{
		GodCameraPitch = FMath::Clamp(Value, GodCameraSteepestPitch, 0.0f);
		UE_LOG(LogPursuitAI, Log,
			TEXT("PursuitCamera: god pitch overridden to %.1f by -PursuitGodPitch"),
			GodCameraPitch);
	}
	if (FParse::Value(FCommandLine::Get(), TEXT("PursuitGodYaw="), Value))
	{
		GodCameraYaw = FMath::Clamp(Value, -180.0f, 180.0f);
		UE_LOG(LogPursuitAI, Log,
			TEXT("PursuitCamera: god yaw overridden to %.1f by -PursuitGodYaw"),
			GodCameraYaw);
	}
}

void APursuitCharacter::ApplyCameraMode()
{
	ApplyGodCameraOverrides();

	ResolvedCameraMode = ResolveCameraMode();
	bCameraModeResolved = true;

	const bool bFollow = (ResolvedCameraMode == EPursuitCameraMode::Follow);
	const bool bFirstPerson = (ResolvedCameraMode == EPursuitCameraMode::FirstPerson);
	const bool bGod = (ResolvedCameraMode == EPursuitCameraMode::God);

	// Exactly one, every time. See the note in the constructor: an active FollowCamera would
	// shadow a later camera in the list, so "turn the god camera on" has to mean "and turn
	// the other one off".
	if (CameraBoom) { CameraBoom->SetActive(bFollow); }
	if (FollowCamera) { FollowCamera->SetActive(bFollow); }
	if (FirstPersonCamera) { FirstPersonCamera->SetActive(bFirstPerson); }
	if (GodCamera)
	{
		GodCamera->SetFieldOfView(GodCameraFOV);
		GodCamera->SetActive(bGod);
	}

	if (bGod)
	{
		// Place it before the first tick reads it, so the first rendered frame is already the
		// shot rather than one frame of the pawn's own view followed by a jump.
		bGodFocusValid = false;
		bGodPitchValid = false;
		TickGodCamera(0.0f);
	}

	UE_LOG(LogPursuitAI, Log,
		TEXT("PursuitCamera: property=%s resolved=%s (editor session=%d) -> %s"),
		CameraModeName(CameraMode), CameraModeName(ResolvedCameraMode),
		(GIsEditor && !FApp::IsGame()) ? 1 : 0,
		bGod
			? TEXT("detached spectator camera; the pawn's own camera is off")
			: (bFirstPerson ? TEXT("eye-level camera on the pawn")
			                : TEXT("spring-arm camera behind the pawn")));

	if (bGod)
	{
		// The framing and the occlusion budget, both as numbers, because both were guessed at
		// least once and the reach is the one that decides whether the shot survives a street.
		UE_LOG(LogPursuitAI, Log,
			TEXT("PursuitCamera: god arm %.0f at pitch %.1f yaw %.1f fov %.0f - ")
			TEXT("horizontal reach %.0f cm (blocked by anything nearer than that)"),
			GodCameraDistance, GodCameraPitch, GodCameraYaw, GodCameraFOV,
			FMath::Abs(FMath::Cos(FMath::DegreesToRadians(GodCameraPitch))) * GodCameraDistance);
	}
}

void APursuitCharacter::TickGodCamera(float DeltaSeconds)
{
	if (!GodCamera)
	{
		return;
	}

	// Aimed at the chest, not the feet. The capsule origin is at the waist, and centring the
	// shot on it leaves the character hanging low in the frame with a street's worth of empty
	// road above them.
	constexpr float FocusLift = 90.0f;
	const FVector Focus = GetActorLocation() + FVector(0.0f, 0.0f, FocusLift);

	// A jump in the focus point is a teleport, not movement - ResetRound puts the player back
	// on the start spot from wherever the chase caught them - and a smoothed camera would
	// then spend several seconds flying across the city to catch up. That flight is a shot of
	// empty street at exactly the moment the video should be showing the reset.
	if (bGodFocusValid && FVector::Dist(Focus, GodFocus) > GodCameraSnapDistance)
	{
		bGodFocusValid = false;
	}

	if (!bGodFocusValid)
	{
		GodFocus = Focus;
		bGodFocusValid = true;
	}
	else
	{
		// Framerate-independent exponential smoothing: the same lag at 30 fps as at 120, which
		// a fixed per-frame lerp would not give. See GodCameraLagSpeed - the lag is deliberate,
		// it is what separates "a camera watching the chase" from "a camera glued to a pawn".
		const float Alpha = 1.0f - FMath::Exp(-GodCameraLagSpeed * DeltaSeconds);
		GodFocus = FMath::Lerp(GodFocus, Focus, Alpha);
	}

	// ---- the occlusion probe ---------------------------------------------
	//
	// The pitch and distance above are a fixed guess about a city nobody consulted when they
	// were chosen, and "nothing is between the lens and the street" is the one property a guess
	// cannot be trusted with. It is not a hypothetical failure either: every setting reasoned
	// out in this file before the probe existed filmed a wall, and the last one filmed an
	// entire orange screen for four seconds.
	//
	// The fallback is a STEEPER angle, and only the angle. The arm is never shortened.
	//
	// Why the angle: what blocks this camera is a wall, a wall is vertical, and the only way
	// past it is over the top - a shorter arm walks the camera back down the same blocked ray
	// and never gets above anything. That version was built first and it failed in a way worth
	// recording: the obstruction was 719 cm along the ray, the arm floor was 1600, and the
	// floor therefore parked the camera on the FAR side of the wall the probe had just
	// measured. The screen was solid orange. A wall can only be in the way of the ray's
	// horizontal reach, and that reach is |cos(pitch)| * distance, so the pitch is the knob.
	//
	// Why only the angle: the shot's framing is |distance| and the field of view, and nothing
	// else. Steepening therefore changes the perspective and leaves the zoom exactly where it
	// was - which is what makes this fallback nearly invisible instead of a camera that lurches
	// at every wall. Shortening the arm does the opposite: it zooms, and it does it hardest at
	// the moment the chase is least interesting to watch from close up.
	//
	// Five rays, not one, and only a majority blocks the shot: the question is "is the SUBJECT
	// hidden", not "does a ray hit anything". One ray is a coin flip on whether a 20 cm
	// traffic-light pole or a palm trunk happens to cross it, and a camera that ducks for those
	// reads as a broken zoom rather than as a city in the way - with a single probe, 3 of this
	// run's 25 seconds went on a traffic light and a palm. The subject is a 170 cm character,
	// so the question is asked with five rays across roughly their width; a pole can block one,
	// a wall blocks four or five.
	//
	// ECC_Visibility, not the camera channel: this is literally asking "can the lens see the
	// target", so it has to be the channel the city's walls block. The pawn itself is excluded
	// by the trace's ignore actor.
	//
	// The offset from the focus out to the camera is the view vector run BACKWARDS, and the
	// sign is load-bearing. FRotator::Vector() is the view's forward vector, which for a
	// downward-pitching camera points down at the street; tracing along that aims the probe
	// into the road, hits it on the first centimetre and reports the ground as the obstruction.
	// The log for that run said SM_road_001_578 was in the way, and it was - the probe had been
	// pointed at it.
	static constexpr float ProbeSpread = 130.0f;

	// The ladder: the requested angle first, then progressively closer to straight down.
	//
	// A LADDER rather than a couple of steps, and that is the whole point. With -4 and -8 as
	// the only rungs, anything a building had a say in landed on -89.5, and a 30 s recording
	// came back with 11 of its 31 logged seconds pinned to the flattest angle the camera owns -
	// a third of the clip looking straight down, which is the least interesting view of a city
	// there is. The angles in between are not decorative: what blocks the lens is a building
	// within roughly reach * (height - focus) / arm of the chase, so -85 tolerates 3.5 m of
	// clearance where -82 tolerates 3 m, -88 tolerates 1.5 m, and -89.5 tolerates nothing
	// because it does not need to. The loop takes the FIRST rung that is clear, so the cost of
	// a ladder is a few more traces while something is in the way, and the payoff is the
	// shallowest angle that works instead of the steepest one that exists.
	//
	// -8 is clamped to -89.5 and therefore lands on the guarantee; see GodCameraSteepestPitch.
	// Not -90: a camera exactly vertical has no yaw to speak of and the look-at degenerates.
	static constexpr float PitchSteps[] = { 0.0f, -3.0f, -6.0f, -8.0f };

	float UsedPitch = GodCameraPitch;
	FString BlockedBy;
	int32 Blocked = 0;

	if (UWorld* World = GetWorld())
	{
		FCollisionQueryParams Params(TEXT("PursuitGodCamera"), false, this);

		for (const float Step : PitchSteps)
		{
			const float CandidatePitch = FMath::Max(GodCameraPitch + Step, GodCameraSteepestPitch);
			const FRotator CandidateRotation(CandidatePitch, GodCameraYaw, 0.0f);
			const FVector CandidateBack = -CandidateRotation.Vector();
			const FVector CandidateArm = CandidateBack * GodCameraDistance;

			const FVector Aside =
				FVector::CrossProduct(CandidateBack, FVector::UpVector).GetSafeNormal();
			const FVector ScreenUp = FVector::CrossProduct(Aside, CandidateBack).GetSafeNormal();
			const FVector Offsets[5] =
			{
				FVector::ZeroVector,
				Aside * ProbeSpread,
				-Aside * ProbeSpread,
				ScreenUp * ProbeSpread,
				-ScreenUp * ProbeSpread,
			};

			int32 CandidateBlocked = 0;
			FString CandidateName;

			for (int32 Index = 0; Index < 5; ++Index)
			{
				FHitResult Hit;
				if (World->LineTraceSingleByChannel(
						Hit, GodFocus + Offsets[Index], GodFocus + Offsets[Index] + CandidateArm,
						ECC_Visibility, Params))
				{
					++CandidateBlocked;
					CandidateName = Hit.GetActor() ? Hit.GetActor()->GetName() : FString(TEXT("<world>"));
				}
			}

			UsedPitch = CandidatePitch;
			Blocked = CandidateBlocked;
			BlockedBy = CandidateName;

			// Fewer than three of five: the pawn is on screen, this angle is fine, stop looking.
			if (CandidateBlocked < 3)
			{
				BlockedBy.Reset();
				break;
			}
		}
	}

	// Snap steep, ease back. Snapping is the half that matters: easing into an obstruction puts
	// a wall on screen for a few frames every time the chase passes a building, which is the
	// artifact this probe exists to remove. Easing out is what stops the recovery from popping.
	//
	// When even the steepest step reads blocked the steepest step is still what gets used, and
	// that is not a compromise. A chase pressed against a 57 m wall reports five of five probes
	// hit at every angle, because a ray leaving a pawn 34 cm from a wall crosses that wall no
	// matter how steep it is - but at -89.5 the whole arm reaches only 25 cm sideways, so the
	// wall it keeps reporting is not in the lens's way and not near the subject either. The
	// number that would tell those apart is not in the hit result, and the framing does not
	// change either way. This is also why the probe is allowed to be pessimistic: the worst it
	// can do is pick the one angle that cannot be occluded at all.
	if (!bGodPitchValid || UsedPitch < GodPitch)
	{
		GodPitch = UsedPitch;
		bGodPitchValid = true;
	}
	else
	{
		const float Alpha = 1.0f - FMath::Exp(-GodCameraReturnSpeed * DeltaSeconds);
		GodPitch = FMath::Lerp(GodPitch, UsedPitch, Alpha);
	}

	// Placed along the SMOOTHED pitch, so the recovery keeps its bearing while it eases back to
	// the requested angle.
	const FRotator AppliedRotation(FMath::Clamp(GodPitch, GodCameraSteepestPitch, GodCameraPitch),
		GodCameraYaw, 0.0f);
	const FVector CameraPosition = GodFocus - AppliedRotation.Vector() * GodCameraDistance;

	// Aimed at the focus rather than left at the configured rotation. The two agree at the
	// requested pitch and differ by up to 7.5 degrees at the steepest one, and aiming keeps the
	// subject centred through the whole manoeuvre instead of sliding it off frame.
	GodCamera->SetWorldLocation(CameraPosition);
	GodCamera->SetWorldRotation((GodFocus - CameraPosition).Rotation());

	// Throttled, and only while the angle is actually off what was asked for: "the god camera
	// spent this run flattened against a building" has to be answerable from the log without
	// watching the clip.
	GodClipLogTimer -= DeltaSeconds;
	if (!BlockedBy.IsEmpty() && GodClipLogTimer <= 0.0f)
	{
		GodClipLogTimer = 1.0f;
		UE_LOG(LogPursuitAI, Log,
			TEXT("PursuitCamera: god view steepened to pitch %.1f (asked %.1f) - %d/5 probes hit %s"),
			GodPitch, GodCameraPitch, Blocked, *BlockedBy);
	}
}

// ---------------------------------------------------------------------------
// Looks
// ---------------------------------------------------------------------------

void APursuitCharacter::ApplyHeroModel()
{
	// The one function that knows which assets belong to which rig. Defined in the
	// PursuitCharacterPaths namespace at the top of this file.
	const PursuitCharacterPaths::FSkeletalHeroAssets Assets = PursuitCharacterPaths::HeroAssets(HeroModel);

	CharacterMesh = TSoftObjectPtr<USkeletalMesh>(Assets.Mesh);
	IdleAnim = TSoftObjectPtr<UAnimSequence>(Assets.Idle);
	WalkAnim = TSoftObjectPtr<UAnimSequence>(Assets.Walk);
	RunAnim = TSoftObjectPtr<UAnimSequence>(Assets.Run);
	SprintAnim = TSoftObjectPtr<UAnimSequence>(Assets.Sprint);
	JumpAnim = TSoftObjectPtr<UAnimSequence>(Assets.Jump);
	FallAnim = TSoftObjectPtr<UAnimSequence>(Assets.Fall);
	LandAnim = TSoftObjectPtr<UAnimSequence>(Assets.Land);

	// Flat colour only on the mannequin, which ships untextured and grey. The pack rigs carry
	// their own PBR materials and the tint would both throw that away and make the player and
	// the chasers indistinguishable once painted the same red and green.
	bTintCharacter = Assets.bTint;

	// A stride belongs to a rig, not to the class: a pack character's run is faster than the
	// mannequin's, so the playback-rate knobs are overwritten from the rig's table.
	WalkClipSpeed = Assets.WalkClipSpeed;
	RunClipSpeed = Assets.RunClipSpeed;
	SprintClipSpeed = Assets.SprintClipSpeed;

	MeshRelativeYaw = -90.0f;

	UE_LOG(LogPursuitAI, Log, TEXT("PursuitCharacter: rig -> %s, tint %s"),
		Assets.Label, bTintCharacter ? TEXT("on") : TEXT("off"));
}

void APursuitCharacter::LoadVisuals()
{
	USkeletalMeshComponent* MeshComponent = GetMesh();
	if (!MeshComponent)
	{
		return;
	}

	USkeletalMesh* Loaded = CharacterMesh.LoadSynchronous();
	if (!Loaded)
	{
		UE_LOG(LogPursuitAI, Error,
			TEXT("PursuitCharacter: nothing at %s. Is MoverExamples enabled in PursuitAI.uproject? "
			     "Without the mesh this pawn is an invisible capsule that still chases you."),
			*CharacterMesh.ToString());
		return;
	}

	// Mesh first, animation mode second: SetAnimationMode(AnimationSingleNode) builds the
	// single node instance, and it needs a mesh to build it against. Reversed, the instance is
	// never created and PlayAnimation silently does nothing.
	MeshComponent->SetSkeletalMesh(Loaded);
	MeshComponent->SetAnimationMode(EAnimationMode::AnimationSingleNode);

	IdleClip = IdleAnim.LoadSynchronous();
	WalkClip = WalkAnim.LoadSynchronous();
	RunClip = RunAnim.LoadSynchronous();
	SprintClip = SprintAnim.LoadSynchronous();
	JumpClip = JumpAnim.LoadSynchronous();
	FallClip = FallAnim.LoadSynchronous();
	LandClip = LandAnim.LoadSynchronous();

	// All three pack rigs and the mannequin are authored facing +Y; the capsule's forward is
	// +X, so this quarter turn is what makes them run the way they are going rather than
	// sideways. Measured in tools/inspect_hero_facing.py.
	MeshComponent->SetRelativeRotation(FRotator(0.0f, MeshRelativeYaw, 0.0f));

	const int32 Expected = (IdleAnim.IsNull() ? 0 : 1) + (WalkAnim.IsNull() ? 0 : 1)
		+ (RunAnim.IsNull() ? 0 : 1) + (SprintAnim.IsNull() ? 0 : 1) + (JumpAnim.IsNull() ? 0 : 1)
		+ (FallAnim.IsNull() ? 0 : 1) + (LandAnim.IsNull() ? 0 : 1);
	const int32 Resolved = (IdleClip ? 1 : 0) + (WalkClip ? 1 : 0) + (RunClip ? 1 : 0)
		+ (SprintClip ? 1 : 0) + (JumpClip ? 1 : 0) + (FallClip ? 1 : 0) + (LandClip ? 1 : 0);

	UE_LOG(LogPursuitAI, Log, TEXT("PursuitCharacter: %s is a %s - mesh %s, %d/%d clips, %.0f cm/s"),
		*GetName(), bIsChaser ? TEXT("chaser") : TEXT("player"), *Loaded->GetName(), Resolved,
		Expected, GetCharacterMovement() ? GetCharacterMovement()->MaxWalkSpeed : 0.0f);

	if (Resolved < Expected)
	{
		// Not fatal: a missing clip means that state holds its previous pose, which is ugly but
		// not broken. Logged because "the animation is stuck" is otherwise a mystery.
		UE_LOG(LogPursuitAI, Warning,
			TEXT("PursuitCharacter: only %d of %d locomotion clips resolved for %s; missing states "
			     "will hold their previous pose"), Resolved, Expected, *GetName());
	}
}

void APursuitCharacter::ApplyTint()
{
	USkeletalMeshComponent* MeshComponent = GetMesh();
	if (!MeshComponent || !MeshComponent->GetSkeletalMeshAsset())
	{
		return;
	}

	// Only the mannequin is tinted. The pack rigs ship real PBR materials, and painting them a
	// flat red or green would throw that art away and make the player and chasers identical at
	// a glance. Called from BeginPlay only when bTintCharacter is set, but the guard is here so
	// a stray call on a textured rig never strips its materials.
	if (!bTintCharacter)
	{
		return;
	}

	UMaterialInterface* BaseMaterial = LoadObject<UMaterialInterface>(nullptr, PursuitCharacterPaths::TintMaterialPath);
	const bool bUsingFallback = (BaseMaterial == nullptr);
	if (bUsingFallback)
	{
		BaseMaterial = LoadObject<UMaterialInterface>(nullptr, PursuitCharacterPaths::FallbackTintMaterialPath);
		UE_LOG(LogPursuitAI, Warning,
			TEXT("PursuitCharacter: %s is missing - falling back to BasicShapeMaterial, so this ")
			TEXT("character will render grey. Run tools/gen_tint_material.py to create it."),
			PursuitCharacterPaths::TintMaterialPath);
	}

	if (!BaseMaterial)
	{
		UE_LOG(LogPursuitAI, Warning, TEXT("PursuitCharacter: no tint material at all, characters stay default grey"));
		return;
	}

	UMaterialInstanceDynamic* Dynamic = UMaterialInstanceDynamic::Create(BaseMaterial, this);
	if (!Dynamic)
	{
		return;
	}

	// Same vector parameter the RL environment writes to. Applied to every slot so it does not
	// matter whether this rig ships one material slot or three.
	const FLinearColor Tint = bIsChaser ? ChaserTint : PlayerTint;
	Dynamic->SetVectorParameterValue(TEXT("Color"), Tint);

	const int32 Slots = FMath::Max(MeshComponent->GetNumMaterials(), 1);
	for (int32 Slot = 0; Slot < Slots; ++Slot)
	{
		MeshComponent->SetMaterial(Slot, Dynamic);
	}

	// Read back and log. A parameter name that quietly stops matching looks exactly like a
	// lighting problem, and this is the line that tells the two apart.
	FLinearColor ReadBack = FLinearColor::Black;
	const bool bRead = Dynamic->GetVectorParameterValue(FMaterialParameterInfo(TEXT("Color")), ReadBack);

	UE_LOG(LogPursuitAI, Display, TEXT("PursuitCharacter: %s tint set=%s read-back=%s across %d slot(s)"),
		*GetName(), *Tint.ToString(), bRead ? *ReadBack.ToString() : TEXT("MISS"), Slots);
}

// ---------------------------------------------------------------------------
// Animation
//
// No animation blueprint. A single node, a clip per state, and a playback rate tied to speed.
// That covers idle, walk, run, jump and fall with none of the authoring an anim graph needs -
// and nothing an asset-generation script would have to create. Landing has a state here but no
// clip on this rig: see the LandAnim comment in the header.
// ---------------------------------------------------------------------------

void APursuitCharacter::UpdateAnimation(float DeltaSeconds)
{
	USkeletalMeshComponent* MeshComponent = GetMesh();
	const UCharacterMovementComponent* Move = GetCharacterMovement();
	if (!MeshComponent || !Move || !IdleClip)
	{
		return;
	}

	const float Speed = Move->Velocity.Size2D();
	const bool bFalling = Move->IsFalling();

	// Landing gets to finish. Without this the walk clip snaps back the instant the capsule
	// touches down, and a landing that is cut off after one frame never reads as one. Inert on
	// this rig, where there is no land clip - see the LandAnim comment in the header.
	if (LandHold > 0.0f)
	{
		LandHold -= DeltaSeconds;
		return;
	}

	if (!bFalling && bWasFalling && LandClip)
	{
		PlayClip(LandClip, false);
		LandHold = LandHoldSeconds;
		bWasFalling = bFalling;
		return;
	}
	bWasFalling = bFalling;

	UAnimSequence* Wanted = IdleClip;
	float Reference = 0.0f;

	if (bFalling)
	{
		// Rising and falling are separate clips on this rig, and the switch between them is the
		// arc of the jump rather than a timer. The latch is what keeps the apex from strobing:
		// Velocity.Z sits within a few cm/s of zero for several frames at the top of every jump,
		// so it is only worth changing the answer when the velocity is clearly one or the other.
		if (Move->Velocity.Z > RisingSpeed)
		{
			bAirborneRising = true;
		}
		else if (Move->Velocity.Z < FallingSpeed)
		{
			bAirborneRising = false;
		}

		Wanted = bAirborneRising
			? (JumpClip ? JumpClip : FallClip)
			: (FallClip ? FallClip : JumpClip);
	}
	else
	{
		// Engage SpeedHysteresis above a threshold, release at it. See the property comment for
		// what a single threshold does here.
		const float WalkOn = IdleThreshold + SpeedHysteresis;
		const float RunOn = WalkToRunThreshold + SpeedHysteresis;
		const float SprintOn = RunToSprintThreshold + SpeedHysteresis;

		switch (Locomotion)
		{
		case ELocomotion::Idle:
			if (Speed > WalkOn)
			{
				Locomotion = ELocomotion::Walk;
			}
			break;

		case ELocomotion::Walk:
			if (Speed > RunOn)
			{
				Locomotion = ELocomotion::Run;
			}
			else if (Speed < IdleThreshold)
			{
				Locomotion = ELocomotion::Idle;
			}
			break;

		case ELocomotion::Run:
			if (Speed > SprintOn)
			{
				Locomotion = ELocomotion::Sprint;
			}
			else if (Speed < WalkToRunThreshold)
			{
				Locomotion = ELocomotion::Walk;
			}
			break;

		case ELocomotion::Sprint:
			if (Speed < RunToSprintThreshold)
			{
				Locomotion = ELocomotion::Run;
			}
			break;
		}

		switch (Locomotion)
		{
		case ELocomotion::Run:
			Wanted = RunClip ? RunClip : WalkClip;
			Reference = RunClipSpeed;
			break;

		case ELocomotion::Sprint:
			// A rig without a sprint clip (the mannequin) falls back to its run clip, but keeps
			// the run reference so it does not suddenly speed up the moment Shift is held.
			Wanted = SprintClip ? SprintClip : RunClip;
			Reference = SprintClip ? SprintClipSpeed : RunClipSpeed;
			break;

		case ELocomotion::Walk:
			Wanted = WalkClip ? WalkClip : RunClip;
			Reference = WalkClipSpeed;
			break;

		default:
			// Idle: Wanted already points at IdleClip and the rate stays at 1.
			break;
		}
	}

	if (!Wanted)
	{
		return;
	}

	PlayClip(Wanted, true);

	// Playback rate is what ties the clip to the actual speed. Without it a 620 cm/s run and a
	// 980 cm/s sprint look identical on screen, and that difference is the one a player has to
	// be able to feel to know whether Shift is doing anything.
	MeshComponent->SetPlayRate(Reference > 0.0f ? FMath::Clamp(Speed / Reference, 0.7f, 1.9f) : 1.0f);
}

void APursuitCharacter::PlayClip(UAnimSequence* Clip, bool bLoop)
{
	// Only on a change: PlayAnimation restarts the clip, so calling it every frame would freeze
	// the character on its first pose.
	if (!Clip || Clip == CurrentClip)
	{
		return;
	}

	CurrentClip = Clip;
	GetMesh()->PlayAnimation(Clip, bLoop);

	// Log the transition, not the state. One line per clip change is quiet enough to leave on,
	// and it is the only way to tell a working animation state machine from a character that
	// happens to be standing in a convincing pose - a still screenshot cannot distinguish
	// "playing idle" from "playing nothing", and neither can a viewer.
	UE_LOG(LogPursuitAI, Log, TEXT("PursuitCharacter: %s anim -> %s (%s, rate %.2f)"),
		*GetName(), *Clip->GetName(), bLoop ? TEXT("loop") : TEXT("once"),
		GetMesh()->GetPlayRate());
}

// ---------------------------------------------------------------------------
// Chasing
// ---------------------------------------------------------------------------

void APursuitCharacter::TickChaser(float DeltaSeconds)
{
	const APawn* Player = UGameplayStatics::GetPlayerPawn(this, 0);
	if (!Player)
	{
		return;
	}

	FVector ToPlayer = Player->GetActorLocation() - GetActorLocation();

	// Height is read BEFORE Z is dropped, because dropping it is what the catch used to do and
	// is the whole reason a chaser on the ground could catch a player on a platform. Both roles
	// are this class on the same capsule, so origin-to-origin and feet-to-feet are the same
	// number here - no half-height correction belongs in this comparison.
	const float HeightGap = static_cast<float>(FMath::Abs(ToPlayer.Z));

	ToPlayer.Z = 0.0f;
	const float Distance = ToPlayer.Size();

	// 0 means "no gate", which is the pre-change behaviour kept switchable for an A/B.
	const bool bHeightGate = CatchHeightTolerance > 0.0f;
	const bool bSameLevel = !bHeightGate || HeightGap <= CatchHeightTolerance;
	const bool bInHorizontalReach = Distance <= CatchRadius;

	if (bInHorizontalReach && bSameLevel)
	{
		ReportCatch();
		return;
	}

	// Inside the catch radius but off the player's level: say so rather than swallowing it.
	// Silence here reads as "the platform never protected me" and "the gate is working" at the
	// same time, and only one of those is true - the same trap the jump probe's missing line
	// used to set, where a probe answering a narrower question looked like a broken feature.
	// Throttled to one line a second per chaser.
	if (bInHorizontalReach && !bSameLevel)
	{
		const UWorld* HoldWorld = GetWorld();
		const float Now = HoldWorld ? static_cast<float>(HoldWorld->GetTimeSeconds()) : 0.0f;
		if (Now - LastHoldLogTime >= ProbeLogInterval)
		{
			LastHoldLogTime = Now;
			UE_LOG(LogPursuitAI, Log,
				TEXT("PursuitCharacter: %s inside the reach (%.0f cm) but %.0f cm off the ")
				TEXT("player's level - held by the height gate (%.0f cm)"),
				*GetName(), Distance, HeightGap, CatchHeightTolerance);
		}
	}

	// Jumping is decided from the straight line to the player, before the avoidance gets a
	// say. The two answer different questions: avoidance answers "which way round", this
	// answers "is the thing in the way short enough to go over". Feeding it the dodged
	// direction instead would make a chaser that is already turning away from a wall decide
	// there is nothing to jump - exactly when there is.
	const FVector Straight = ToPlayer / FMath::Max(Distance, 1.0f);
	const EPursuitJumpVerdict Verdict = TickChaserJump(Straight, Distance, HeightGap);

	// An obstacle the chaser has decided to jump must NOT also be steered around. This is the
	// ordering bug that made the feature look broken for three rounds, and it is worth being
	// precise about because both halves behaved correctly on their own.
	//
	// The feeler sees a crate at AvoidTraceLength (300 cm) and the jump only arms at
	// JumpTriggerDistance (190). So between those two, SteerAroundObstacles does what it is
	// for and starts angling the chaser off the crate - correctly, since at that range going
	// round is the right answer. But the chaser then approaches the crate on a diagonal, the
	// crate leaves its forward line, the jump's own probe finds nothing ahead, and the chaser
	// walks round a waist-high box for the rest of the round. The log showed exactly this: a
	// chaser pinned at "face 300 cm" that had drifted 68 cm off the axis, with a crate 225 cm
	// away that neither probe could see any more.
	//
	// The fix is not a wider trigger - that just makes it launch from further out and land in
	// front of the crate. It is that the two decisions are not equal: once the jump has
	// decided the obstacle is clearable, steering around it is no longer an available option,
	// so the chaser commits and drives straight at it until the jump fires.
	//
	// Gated on the LATCH, not on this frame's verdict. The two are the same value on the frame
	// the commitment is made and diverge immediately afterwards, because a chaser on a
	// slightly diagonal approach drops the crate off its forward ray for a frame or two - and
	// a steering decision taken on those frames is exactly what re-introduces the orbit.
	const bool bGoingOver = bJumpCommitted || Verdict != EPursuitJumpVerdict::None;

	FVector Desired = Straight;
	if (!bGoingOver)
	{
		Desired = SteerAroundObstacles(Straight);
		Desired += SeparationPush() * SeparationWeight;
		Desired.Z = 0.0f;
	}

	if (!Desired.Normalize())
	{
		return;
	}

	AddMovementInput(Desired, 1.0f);
}

EPursuitJumpVerdict APursuitCharacter::TickChaserJump(
	const FVector& ToPlayerDirection, float DistanceToPlayer, float HeightGapToPlayer)
{
	if (!bCanJump || JumpCooldown > 0.0f)
	{
		return EPursuitJumpVerdict::None;
	}

	// Already airborne. Jump() is harmful only on the ground, so a second call mid-arc would
	// be swallowed by the movement component anyway - but the cooldown is what stops the
	// *next* jump from being armed the instant the feet touch down, which is what turns a
	// wall into a staircase the chaser walks up one hop at a time.
	if (GetCharacterMovement() && GetCharacterMovement()->IsFalling())
	{
		return EPursuitJumpVerdict::None;
	}

	// Don't bother when the player is basically already caught; a hop at that range is noise.
	//
	// And don't bother measuring "that range" horizontally either. This test used to be the
	// bare distance, which was consistent while the catch was height-blind and becomes actively
	// wrong now that it is not: a chaser standing under a deck with the player on top of it is
	// 0-100 cm away horizontally and a whole platform away vertically, so the bare test would
	// refuse the jump exactly where the jump is the only way forward. The platform would stop
	// shielding the player and start stranding the chaser under it instead.
	const bool bSameLevel =
		CatchHeightTolerance <= 0.0f || HeightGapToPlayer <= CatchHeightTolerance;
	if (bSameLevel && DistanceToPlayer <= CatchRadius * 1.25f)
	{
		return EPursuitJumpVerdict::None;
	}

	// How far away the obstacle is, so the log below can report the distance that actually
	// matters. The distance to the player is a different number by a wide margin - a chaser
	// usually notices a crate from well outside the trigger range - and reporting it here was
	// actively misleading: "jumped over an obstacle, 2843 cm away" reads as a chaser hopping
	// at nothing, which is exactly what it looked like before this was separated out.
	const float ObstacleDistance = ProbeObstacle(ToPlayerDirection);
	float FaceDistance = 0.0f;
	const float TopZ = ProbeObstacleTop(ToPlayerDirection, FaceDistance);
	if (!IsObstacleJumpable(TopZ))
	{
		// The latch is released here, and releasing it is what keeps the commitment honest:
		// a chaser that committed and then lost the obstacle entirely - the crate rebuilt
		// between rounds, or the player leading it somewhere else - goes back to normal
		// steering rather than driving at a wall it decided to jump a second ago.
		bJumpCommitted = false;

		// One line per chaser, at most once a second, saying what the probes saw. Silence here
		// is not evidence of anything: -PursuitJumpTest produced a chaser that reached the
		// player without jumping, and without this the only available conclusion was "the
		// feature is broken" when the probe was in fact answering a narrower question than the
		// one being asked. See the note on JumpTriggerDistance vs AvoidTraceLength.
		LogJumpProbe(ToPlayerDirection, ObstacleDistance, TopZ, DistanceToPlayer);
		return EPursuitJumpVerdict::None;
	}
	// The far side of the obstacle has to be reachable, or the chaser launches itself into a
	// face it cannot land on and drops straight back down in front of it - the hopping-at-a-
	// wall failure, which both looks wrong and never gets anywhere.
	//
	// The check is whether the geometry ENDS: a probe from above the obstacle, on its far
	// side, looking down at where the chaser would come down. If something is still there at
	// that range, this is a face with no room behind it and the jump would only put the
	// chaser back down on the near side.
	//
	// Two details that are easy to get wrong, both of which make the probe answer "blocked"
	// for every obstacle in the level:
	//   * the XY has to be measured from the OBSTACLE FACE, not from the chaser's own
	//     position, or the probe lands on the near side of the very thing being jumped;
	//   * the drop has to start above the obstacle's TOP, not at chest height, or the cast
	//     clips the obstacle's own face on the way down.
	const FVector Ahead = FVector(ToPlayerDirection.X, ToPlayerDirection.Y, 0.0f).GetSafeNormal();
	if (!Ahead.IsNearlyZero())
	{
		const UWorld* World = GetWorld();
		if (World)
		{
			// A body's length past the face: far enough that a normal deep crate leaves a
			// clear landing spot, close enough that the chaser does not overshoot past the
			// whole obstacle and land on the next one along.
			const float LandingReach = FMath::Max(
				FaceDistance + GetCapsuleComponent()->GetScaledCapsuleRadius() * 2.0f,
				JumpTriggerDistance * JumpLandingProbeScale);

			const FVector LandingXy = GetActorLocation() + Ahead * LandingReach;

			FCollisionQueryParams Params(TEXT("PursuitJumpLanding"), false, this);
			FHitResult Hit;
			const bool bStillSolid = World->LineTraceSingleByChannel(
				Hit,
				LandingXy + FVector(0.0f, 0.0f, TopZ + 60.0f),
				LandingXy + FVector(0.0f, 0.0f, TopZ + 5.0f),
				ECC_WorldStatic, Params);

			if (bStillSolid)
			{
				// Jumpable in height but with no room behind: this is the outer face of
				// something much deeper than it is tall, so going round is still the answer.
				// None, not Commit - committing would walk the chaser into a face it cannot
				// clear and hold it there.
				bJumpCommitted = false;
				return EPursuitJumpVerdict::None;
			}
		}
	}

	// Height is in band and the far side is clear, so this obstacle is going over. From here
	// to the moment the jump fires, the chaser drives straight at it - see the note in
	// TickChaser on why avoidance must not be allowed to peel it off first.
	//
	// Committed rather than jumped: the trigger range is deliberately shorter than the
	// avoidance range, so most frames land here and only the last few actually jump. Returning
	// Commit for all of them is what stops the steering turning the chaser off-axis in the
	// window between "saw the crate" and "close enough to clear it".
	bJumpCommitted = true;
	Jump();

	// After Jump(), not before: a failed jump would otherwise still pay the cooldown and the
	// chaser would stand at the obstacle waiting to be allowed to try again.
	JumpCooldown = JumpCooldownSeconds;

	// Every jump is logged, but the count is kept so a transcript can be summarised without
	// reading every line. Log-once-per-chaser was too quiet: it made "one jump in a whole run"
	// and "a jump every second" produce identical logs, which is exactly the question a
	// verification run is asking. The city map has bins and kerbs everywhere, so a chaser that
	// jumps freely would bury the log - hence the running total rather than silence.
	//
	// The height is reported as height-above-feet, the same number IsObstacleJumpable judged,
	// so the log and the decision can never disagree - printing the raw world Z here would
	// invite exactly the off-by-a-capsule that the check exists to avoid.
	++JumpCount;
	const float HalfHeight = GetCapsuleComponent() ? GetCapsuleComponent()->GetScaledCapsuleHalfHeight() : 88.0f;
	// The position is part of the line because a jump total on its own cannot be attributed to
	// anything. "213 jumps" across a city full of kerbs says only that the gate fires; only the
	// coordinates can say WHICH fixture was cleared, and whether the stacked towers were
	// climbed deck by deck (x, y of the landmark) or merely walked around. Costs three numbers
	// on a line that is already being written.
	const FVector Where = GetActorLocation();
	UE_LOG(LogPursuitAI, Log,
		TEXT("PursuitCharacter: %s jumped over an obstacle (%.0f cm tall, %.0f cm away; player %.0f cm) ")
		TEXT("at (%.0f, %.0f, %.0f) [jump %d]"),
		*GetName(), TopZ - (Where.Z - HalfHeight), ObstacleDistance, DistanceToPlayer,
		Where.X, Where.Y, Where.Z,
		JumpCount);

	return EPursuitJumpVerdict::Jumped;
}

void APursuitCharacter::LogJumpProbe(
	const FVector& Direction, float ObstacleDistance, float ObstacleTopZ, float DistanceToPlayer)
{
	// Rate-limited to one line a second per chaser. The decision runs every frame, so an
	// unthrottled line here would bury the log it is meant to explain.
	const UWorld* World = GetWorld();
	const float Now = World ? static_cast<float>(World->GetTimeSeconds()) : 0.0f;
	if (Now - LastJumpProbeLogTime < ProbeLogInterval)
	{
		return;
	}
	// Measured, not assumed: the stall counter is advanced by the time that actually elapsed,
	// because the 1.0 below would silently go wrong if the interval above were ever changed.
	const float Elapsed = (LastJumpProbeLogTime > 0.0f) ? (Now - LastJumpProbeLogTime) : ProbeLogInterval;
	LastJumpProbeLogTime = Now;

	const float HalfHeight = GetCapsuleComponent() ? GetCapsuleComponent()->GetScaledCapsuleHalfHeight() : 88.0f;

	// "none" rather than a number when the height probe found nothing. The previous version
	// printed -1, which reads as a measurement - "the obstacle tops out a centimetre below my
	// feet" - when it actually meant "the downward cast hit nothing". That ambiguity sent one
	// diagnosis down the wrong path, so the sentinel is spelled out now.
	const FString TopText = (ObstacleTopZ == -FLT_MAX)
		? FString(TEXT("none (downward probe found nothing)"))
		: FString::Printf(TEXT("%+.0f cm above feet"),
			ObstacleTopZ - (GetActorLocation().Z - HalfHeight));

	// Prints the four numbers the decision is actually made of, side by side with the two
	// thresholds they are compared against, plus the chaser's own position.
	//
	// The position is not decoration. Without it a line reading "face 300 cm" is ambiguous
	// between two completely different situations: an obstacle genuinely 300 cm away (nothing
	// close enough to act on), and a chaser standing inside or flush against something whose
	// face the trace started *behind*. Those two demand opposite fixes and the same log line
	// used to describe both.
	const FVector Where = GetActorLocation();
	const FVector Velocity = GetVelocity();
	const UCharacterMovementComponent* Move = GetCharacterMovement();
	UE_LOG(LogPursuitAI, Log,
		TEXT("PursuitCharacter: %s jump probe at (%.0f, %.0f, %.0f) feet z=%.0f vel=%.0f ")
		TEXT("want=%.0f falling=%d - face %.0f cm (at %.0f cm, trace %.0f, trigger %.0f), top %s ")
		TEXT("(jumpable %.0f to %.0f), cooldown %.2f, player %.0f cm"),
		*GetName(), Where.X, Where.Y, Where.Z, Where.Z - HalfHeight, Velocity.Size2D(),
		Move ? Move->MaxWalkSpeed : 0.0f, (Move && Move->IsFalling()) ? 1 : 0,
		ObstacleDistance, JumpFaceProbeHeight, AvoidTraceLength, JumpTriggerDistance,
		*TopText,
		Move ? Move->MaxStepHeight : 45.0f, MaxJumpHeight,
		JumpCooldown, DistanceToPlayer);

	// Why the chaser is not moving, when the feelers say the way is clear.
	//
	// Kept from the debugging pass that found the sub-ankle lip, but gated on a SUSTAINED
	// stall rather than on a single low-speed frame. The ungated version fired on the one
	// frame a chaser spends at rest after spawning or being reset, which made a healthy run
	// produce ninety lines of alarm and buried the two real stalls inside the noise. A chaser
	// that is still slow a full second after it was last given a clear path is the thing worth
	// reporting; a chaser that is slow for one frame is just a chaser that started moving.
	if (Velocity.SizeSquared() < BlockedSpeedSquared && Move && !Move->IsFalling())
	{
		BlockedTime += Elapsed;
	}
	else
	{
		BlockedTime = 0.0f;
	}

	if (BlockedTime >= BlockedReportSeconds)
	{
		BlockedTime = 0.0f;
		const UWorld* ProbeWorld = GetWorld();
		if (ProbeWorld)
		{
			const FVector Flat = FVector(Direction.X, Direction.Y, 0.0f).GetSafeNormal();
			const FVector Feet = FVector(Where.X, Where.Y, Where.Z - HalfHeight);

			FCollisionQueryParams BlockParams(TEXT("PursuitBlockedProbe"), false, this);
			BlockParams.AddIgnoredActor(this);

			// Ground immediately under the feet, and the same a body-length ahead.
			float UnderZ = -FLT_MAX;
			float AheadZ = -FLT_MAX;
			{
				FHitResult Hit;
				if (ProbeWorld->LineTraceSingleByChannel(Hit,
					Feet + FVector(0.0f, 0.0f, 60.0f), Feet - FVector(0.0f, 0.0f, 300.0f),
					ECC_WorldStatic, BlockParams))
				{
					UnderZ = static_cast<float>(Hit.Location.Z);
				}
			}
			{
				const FVector Ahead = Feet + Flat * 90.0f;
				FHitResult Hit;
				if (ProbeWorld->LineTraceSingleByChannel(Hit,
					Ahead + FVector(0.0f, 0.0f, 60.0f), Ahead - FVector(0.0f, 0.0f, 300.0f),
					ECC_WorldStatic, BlockParams))
				{
					AheadZ = static_cast<float>(Hit.Location.Z);
				}
			}

			// Forward feelers, at band heights chosen to bracket what a capsule can actually
			// touch: below the step-up the feelers are supposed to miss, at the step-up itself,
			// and at the waist. The 3 cm feeler is the one that found the city's paving lips -
			// a capsule is round at the bottom, so its lowest contact is made below any height
			// the 10 cm feeler samples.
			const auto ForwardFace = [&](float HeightAboveFeet) -> float
			{
				const FVector Start = Feet + FVector(0.0f, 0.0f, HeightAboveFeet);
				FHitResult Hit;
				if (ProbeWorld->LineTraceSingleByChannel(Hit, Start, Start + Flat * 200.0f,
					ECC_WorldStatic, BlockParams))
				{
					return static_cast<float>(Hit.Distance);
				}
				return -1.0f;
			};

			UE_LOG(LogPursuitAI, Warning,
				TEXT("PursuitCharacter: %s has been stalled for %.1f s with a clear feeler - ")
				TEXT("face %.0f cm reported clear, ground under feet z=%.0f (step 90 cm ahead %+.0f), ")
				TEXT("forward face at 3/10/45/90 cm above feet = %.0f / %.0f / %.0f / %.0f"),
				*GetName(), BlockedReportSeconds,
				ObstacleDistance,
				UnderZ == -FLT_MAX ? -9999.0f : UnderZ,
				(AheadZ == -FLT_MAX || UnderZ == -FLT_MAX) ? 0.0f : AheadZ - UnderZ,
				ForwardFace(3.0f), ForwardFace(10.0f), ForwardFace(45.0f), ForwardFace(90.0f));
		}
	}
}

FVector APursuitCharacter::SteerAroundObstacles(const FVector& Desired) const
{
	const float Straight = ProbeObstacle(Desired);
	if (Straight >= AvoidTraceLength)
	{
		return Desired;
	}

	// A face the capsule is allowed to walk up is not an obstacle, and steering around it is not
	// merely wasted - it is the stall.
	//
	// The avoidance beams answer "is there a face ahead", never "how tall is that face", so a
	// 42 cm kerb is indistinguishable from a wall as far as this function is concerned. Steering
	// then turns the chaser sideways, the sideways step is too short to clear the feeler, and the
	// backoff at the bottom of this function pulls it back in again. The result is a chaser
	// hovering with the face 37-42 cm out - i.e. pinned right at CapsuleRadius (34) - reporting
	// vel=0 with want=575 while the player walks off.
	//
	// MaxStepHeight is the movement component's own promise that it climbs 45 cm for free, so the
	// honest reading of a sub-45 face is "nothing here, drive into it".
	//
	// THE MEASUREMENT HAS TO BE OF THE FACE THE BEAM HIT, just past its far side. It must NOT be
	// the height probe's step branch, which reports the ground further along the line of travel:
	// where a kerb runs along the foot of a wall, that branch finds the kerb's 40 cm rise 90 cm
	// ahead, reports 40, and looks walkable - while the beam is in fact stopped 43 cm out by a
	// wall that the same log gives as a face at 3, 10, 45 AND 90 cm above the feet. Short-
	// circuiting on that answer drove the chaser straight into the wall at velocity zero: a new
	// stall class, fifteen in one 240 s run, replacing the three this was written to remove.
	//
	// So cast down here, at the reported face plus the usual few cm of depth, from just above
	// step height to below the feet. A low step is found - its top is under the cast's start. A
	// wall is not: its top is above the start, the ray begins inside its body, and the world
	// reports nothing, which leaves the old steering in place. Nothing found is therefore the
	// safe answer, and the only answer that short-circuits is a real, low, solid top.
	//
	// Only the step case short-circuits: a face taller than MaxStepHeight is still steered
	// around, and the jump decision - which runs before this in TickChaser - still commits or
	// refuses for anything in the 45..120 band.
	const FVector Flat = FVector(Desired.X, Desired.Y, 0.0f).GetSafeNormal();
	if (bStepOverLowFaces && !Flat.IsNearlyZero())
	{
		const UCapsuleComponent* StepCapsule = GetCapsuleComponent();
		const float StepHalfHeight = StepCapsule ? StepCapsule->GetScaledCapsuleHalfHeight() : 88.0f;
		const float FeetZ = GetActorLocation().Z - StepHalfHeight;
		const UCharacterMovementComponent* StepMove = GetCharacterMovement();
		const float StepUp = StepMove ? StepMove->MaxStepHeight : 45.0f;

		const FVector StepXy = GetActorLocation() + Flat * (Straight + ProbeIntoFaceDepth);
		const UWorld* StepWorld = GetWorld();
		FCollisionQueryParams StepParams(TEXT("PursuitStepUp"), false, this);
		FHitResult StepHit;
		if (StepWorld && StepWorld->LineTraceSingleByChannel(
				StepHit,
				FVector(StepXy.X, StepXy.Y, FeetZ + StepUp + 5.0f),
				FVector(StepXy.X, StepXy.Y, FeetZ - ProbeBelowFeetDepth),
				ECC_WorldStatic, StepParams)
			&& StepHit.ImpactPoint.Z - FeetZ <= StepUp)
		{
			return Desired;
		}
	}

	// Something is in the way. Feel left and right and take whichever is clearer, which is
	// enough to round a pillar without a navigation mesh. Deliberately worse than real
	// pathfinding: a chaser that visibly misses is better than one that walks a route the
	// player cannot see it earning.
	//
	// Strictly-better was the wrong comparison, and it produced a chaser that stood still.
	// When neither side ray beat the straight one - which is the normal case for a ledge or a
	// wall that runs ACROSS the line of travel, where all three beams hit the same face at
	// roughly the same range - the loop left Best = Desired and the chaser drove straight
	// into the face at velocity zero for the rest of the round. Measured: 20 consecutive probe
	// lines from the identical position, vel=0, face 34 cm.
	//
	// So the straight ray is compared too, and on a tie the side is taken. A chaser that
	// commits to a diagonal pushes into the face at an angle and slides along it - the
	// movement component resolves the blocked normal into sideways motion - which is how it
	// ends up somewhere new instead of pinned. Ties broken toward the side, not kept straight,
	// is the whole difference between sliding and standing.
	const float StraightClear = Straight;
	FVector Best = Desired;
	float BestClear = StraightClear;
	for (const float Angle : { AvoidTurnAngle, -AvoidTurnAngle })
	{
		const FVector Candidate = Desired.RotateAngleAxis(Angle, FVector::UpVector);
		const float Clear = ProbeObstacle(Candidate);
		if (Clear > BestClear)
		{
			BestClear = Clear;
			Best = Candidate;
		}
	}

	// Nothing was clearer than straight ahead: take a side anyway rather than stand still.
	// The chaser still makes forward progress toward the player, just on the diagonal, and the
	// face it is pressed against turns that into a slide.
	//
	// Always the same side, deliberately. Alternating between the two would cancel itself out
	// frame to frame and leave the chaser vibrating against the face; a fixed side means it
	// walks along the obstacle in one direction until a gap appears or the jump fires.
	if (Best.Equals(Desired, KINDA_SMALL_NUMBER))
	{
		Best = Desired.RotateAngleAxis(AvoidTurnAngle, FVector::UpVector);
	}

	// Pressed right up against it: back off before trying to go round.
	//
	// Rotating a direction does nothing when the capsule is already touching the obstacle. The
	// capsule's radius is what decides whether a chaser can move at all, and at 21-26 cm from
	// the feet it is overlapping - so every diagonal is refused by the movement component for
	// the same reason the straight one was, and the chaser grinds in place while the player
	// walks off. That was the measured "accel 2048, vel=0, nothing at knee or chest height,
	// something 24 cm in front of the toes" case on the city map.
	//
	// Retracting first separates the contact, and only then does a diagonal have anywhere to
	// go. The backward weight is small on purpose: this is a nudge out of contact, not a
	// retreat, and the very next frame - now clear - goes back to a pure forward diagonal.
	const UCapsuleComponent* Capsule = GetCapsuleComponent();
	const float CapsuleRadius = Capsule ? Capsule->GetScaledCapsuleRadius() : 34.0f;
	if (StraightClear < CapsuleRadius)
	{
		Best -= Desired * AvoidBackoffWeight;
	}

	return Best;
}

float APursuitCharacter::ProbeObstacle(const FVector& Direction) const
{
	const UWorld* World = GetWorld();
	if (!World)
	{
		return AvoidTraceLength;
	}

	// ECC_WorldStatic rather than Visibility on purpose: this has to see walls and pillars and
	// must not see characters, whose capsules would otherwise make a chaser dodge its own pack.
	FCollisionQueryParams Params(TEXT("PursuitAvoid"), false, this);

	// TWO feelers, and the nearest hit wins. The chest beam alone is not enough to keep a
	// character out of trouble, which the city map made unarguable.
	//
	// The chest beam sits 70 cm above the capsule centre - 158 cm above the ground - because
	// that is where walls are. But a 61 cm kerb or a low wall stops a capsule dead while
	// sitting entirely below that beam: the beam passes over it, this function reports a clear
	// path, and the chaser drives into the step until the movement component refuses to climb
	// it. The observed result was a chaser frozen with velocity zero, reporting "face 300 cm"
	// - sincerely, because nothing was within 300 cm of its chest.
	//
	// The low beam is cast a few cm above the feet - below the 45 cm the capsule steps over for
	// free, and below the 20 cm that turned out to be too high - so anything it hits is
	// something the capsule genuinely cannot pass. Whichever beam finds something first is what
	// the steering reacts to.
	const float LowHeight = LowAvoidTraceHeight - (GetCapsuleComponent()
		? GetCapsuleComponent()->GetScaledCapsuleHalfHeight()
		: 88.0f);

	float Nearest = AvoidTraceLength;
	for (const float Height : { AvoidTraceHeight, LowHeight })
	{
		const FVector Start = GetActorLocation() + FVector(0.0f, 0.0f, Height);
		const FVector End = Start + Direction * AvoidTraceLength;

		FHitResult Hit;
		if (World->LineTraceSingleByChannel(Hit, Start, End, ECC_WorldStatic, Params))
		{
			Nearest = FMath::Min(Nearest, static_cast<float>(Hit.Distance));
		}
	}

	return Nearest;
}

float APursuitCharacter::ProbeObstacleTop(const FVector& Direction, float& OutFaceDistance) const
{
	const UWorld* World = GetWorld();
	if (!World)
	{
		return -FLT_MAX;
	}

	const FVector Flat = FVector(Direction.X, Direction.Y, 0.0f).GetSafeNormal();
	if (Flat.IsNearlyZero())
	{
		return -FLT_MAX;
	}

	// The actor's origin is the capsule CENTRE, not the feet. Everything below has to be
	// expressed relative to that, and the "height above the character" figure has to subtract
	// the capsule's half height to mean what it says.
	const float HalfHeight = GetCapsuleComponent() ? GetCapsuleComponent()->GetScaledCapsuleHalfHeight() : 88.0f;
	const FVector Origin = GetActorLocation();

	// The face is sought at the height of the obstacle BAND, not at the avoidance feeler's
	// chest height. This is the difference between the feature working and not, and it is not
	// a tuning value - it is geometry.
	//
	// AvoidTraceHeight is 70 cm above the actor origin, i.e. ~158 cm above the ground, because
	// the avoidance feeler is asking "what can I not walk through" and a body is a body at
	// chest height. The jump is asking a different question - "what lies between my ankle and
	// my head that is worth hopping" - and its whole subject matter (45 to 150 cm above the
	// feet) is underneath the avoidance beam. Casting at chest height over a 90 cm crate makes
	// the beam pass clean over it and land on whatever building is behind, and the jump check
	// then measures THAT. The probe reported an honest face distance and an honest height, for
	// two different objects, which is why the log looked self-consistent while the chaser
	// walked around a crate it should have cleared.
	const FVector FaceProbe = Origin + FVector(0.0f, 0.0f, JumpFaceProbeHeight - HalfHeight);

	FCollisionQueryParams Params(TEXT("PursuitJumpProbe"), false, this);
	Params.bReturnPhysicalMaterial = false;

	FHitResult FaceHit;
	const float FaceDistance = World->LineTraceSingleByChannel(
		FaceHit, FaceProbe, FaceProbe + Flat * AvoidTraceLength, ECC_WorldStatic, Params)
		? FaceHit.Distance
		: AvoidTraceLength;

	// What did the face feeler actually hit? Recorded once per chaser. Without it a probe that
	// reports a face distance while the height probe finds nothing is unresolvable from the
	// log alone, and that ambiguity cost this feature three rounds.
	if (FaceHit.bBlockingHit && !bHasLoggedFaceIdentity)
	{
		bHasLoggedFaceIdentity = true;
		const UPrimitiveComponent* Comp = FaceHit.GetComponent();
		UE_LOG(LogPursuitAI, Log,
			TEXT("PursuitCharacter: %s face feeler hit '%s' (component %s, actor %s) at (%.0f, %.0f, %.0f) dist %.0f"),
			*GetName(),
			FaceHit.GetActor() ? *FaceHit.GetActor()->GetName() : TEXT("?"),
			Comp ? *Comp->GetName() : TEXT("none"),
			Comp && Comp->GetOwner() ? *Comp->GetOwner()->GetClass()->GetName() : TEXT("?"),
			FaceHit.Location.X, FaceHit.Location.Y, FaceHit.Location.Z, FaceHit.Distance);
	}

	if (FaceDistance >= AvoidTraceLength)
	{
		return -FLT_MAX;
	}

	// STEP CASE FIRST, before the trigger-distance gate, because on the real map this is what
	// the chasers actually jam on and it is not an object with a face at all.
	//
	// A rising pavement is the GROUND, one level higher. There is no "inside" to probe a few
	// cm into: the surface runs away from the character for metres, so every downward cast
	// lands on it wherever it is aimed, and the face feeler's hit is whatever the low beam
	// clipped - a number unrelated to how far the chaser is from the step. Gating the step on
	// that number would be gating it on a coincidence.
	//
	// The measured case: chaser feet at z=-9, ground under it at z=-10, ground 90 cm ahead at
	// z=+51. That is a 61 cm step, walkable by nothing (MaxStepHeight 45) and jumpable by this
	// character (MaxJumpHeight 150) - and the old probe returned "top none" for it, because it
	// looked 42-78 cm ahead at the height of the FEELER (81) while the step's surface is below
	// that, so its casts passed over the step entirely.
	//
	// So the height of the ground is measured directly, a body-length ahead, and the rise over
	// what the character is standing on is the obstacle. That is the number IsObstacleJumpable
	// wants, and it is the same number whether the surface belongs to a crate, a kerb or a ramp
	// in the city - which is why this works on both the fixture and the map.
	//
	// Anchored on the FEET, not the capsule centre: the quantity is "how far up must I get",
	// and a capsule-centre anchor would silently add the character's own half height to every
	// measurement.
	{
		const float FeetZ = Origin.Z - HalfHeight;

		FCollisionQueryParams StepParams(TEXT("PursuitJumpStep"), false, this);
		StepParams.AddIgnoredActor(this);

		// Under the character's own feet, for the baseline.
		float StandingZ = -FLT_MAX;
		{
			FHitResult Hit;
			const FVector From = FVector(Origin.X, Origin.Y, FeetZ + 60.0f);
			const FVector To = FVector(Origin.X, Origin.Y, FeetZ - ProbeBelowFeetDepth);
			if (World->LineTraceSingleByChannel(Hit, From, To, ECC_WorldStatic, StepParams))
			{
				StandingZ = static_cast<float>(Hit.Location.Z);
			}
		}

		// A body-length ahead, where the character is about to be. 90 cm rather than the
		// feeler's 300: this asks "is the surface about to rise under me", and asking it at
		// 300 cm would have the chaser launching at every gentle slope it can see coming.
		const float LookAhead = 90.0f;

		// Sampled at a couple of ranges, because the step's edge has to be crossed to be seen:
		// a probe landing in front of the near edge reports the low surface and finds nothing,
		// which is exactly the false "clear" that started this.
		for (const float Range : { LookAhead, LookAhead * 1.6f })
		{
			const FVector AheadXy = FVector(Origin.X, Origin.Y, FeetZ) + Flat * Range;

			FHitResult Hit;
			const FVector From = AheadXy + FVector(0.0f, 0.0f, 120.0f);
			const FVector To = AheadXy - FVector(0.0f, 0.0f, ProbeBelowFeetDepth + 120.0f);
			if (World->LineTraceSingleByChannel(Hit, From, To, ECC_WorldStatic, StepParams))
			{
				const float AheadZ = static_cast<float>(Hit.Location.Z);

				// A rise ahead is the obstacle. A drop is a fall, which is a different situation
				// and must not be reported as something to jump.
				//
				// Whether the rise is TALL enough to be worth jumping is deliberately not
				// decided here - that is IsObstacleJumpable's single job, and it holds the
				// step-up and MaxJumpHeight thresholds. Splitting that judgement across two
				// functions is how the two ends drift apart and the feature starts disagreeing
				// with itself. This probe measures; that one rules.
				//
				// When the baseline is unknown the feet are used, which is the best available
				// estimate and errs toward seeing the step rather than missing it.
				const float Baseline = (StandingZ == -FLT_MAX) ? FeetZ : StandingZ;
				if (AheadZ > Baseline)
				{
					return AheadZ;
				}
			}
		}
	}

	// No step worth reporting. Fall through to the object-with-a-face case: a crate, a low
	// wall, anything that presents a vertical surface rather than being the ground itself.
	//
	// Close enough to be worth acting on. Further away the obstacle is still something to
	// steer around, and jumping early would land the chaser in front of it.
	if (FaceDistance > JumpTriggerDistance)
	{
		return -FLT_MAX;
	}

	// Handed back before the vertical probe, so a caller that gets -FLT_MAX above cannot
	// accidentally read a face distance that does not correspond to a jumpable obstacle.
	OutFaceDistance = FaceDistance;

	// The height, and this is the trace the whole feature turns on.
	//
	// It runs DOWNWARD from well above the capsule to the character's own foot level, at a
	// point just INSIDE the face. Downward rather than upward for a reason that cost a debug
	// cycle: an upward cast from foot level starts *inside or below* whatever is being tested
	// and hits the ground plane behind it, so every obstacle in a level reports the floor's
	// height instead of its own. The chasers then "jumped over" the pavement.
	//
	// "Just inside", and the sign of that offset is the other bug this trace has had. The first
	// version probed at FaceDistance - 15, i.e. 15 cm SHORT of the face - which is still outside
	// the obstacle, in open air. A downward cast there finds nothing, so the probe returned
	// -FLT_MAX, IsObstacleJumpable rejected it, and a chaser standing at a crate it could see
	// perfectly well (face 140 cm, well inside the trigger) never jumped. The symptom was a
	// probe log reading "top -1 cm above feet" - -1 being the "nothing found" sentinel - next
	// to a face distance that proved the crate was there.
	//
	// The lower end reaches below the character's own feet, so bare ground is reported as the
	// ground and rejected by IsObstacleJumpable as below the step-up - which is what keeps a
	// chaser on open paving from hopping at nothing.
	//
	// Which face the distance came from matters, and that was the third bug here: this trace
	// used to be anchored on the capsule centre while the face had been measured at chest
	// height (see FaceProbe above). The probe now walks forward from the height it measured
	// at, so the face distance and the depth behind it are two statements about the same point
	// in space.
	//
	// MULTIPLE depths, nearest first, first hit wins. A single depth is a bet that the
	// obstacle is thicker than the probe is deep, and that bet is lost on the real map: the
	// 61 cm ledge the chasers jam against is barely two dozen cm thick, so the old fixed 25 cm
	// stride cleared its far edge entirely, cast down through open air, and reported the
	// obstacle as having no top at all. The sweep turns "what depth is the solid at" from an
	// assumption into something measured: whatever the first stride that lands inside finds is
	// the near face's height by construction, and a stride that lands in air is simply skipped.
	const int32 Steps = FMath::Max(ProbeIntoFaceSteps, 1);
	for (int32 Step = 0; Step < Steps; ++Step)
	{
		const float Depth = ProbeIntoFaceDepth + ProbeIntoFaceStepSize * Step;
		const FVector Ahead = FaceProbe + Flat * (FaceDistance + Depth);

		// Down to below the character's own feet, not to foot level. Stopping exactly at the
		// sole leaves the cast a centimetre or two above the pavement - measured case: feet at
		// z=-9, ground at z=-10, cast bottom z=-7 - and a probe that reaches neither the
		// obstacle nor the floor reports the space as empty. That is indistinguishable in the
		// log from a genuinely thin obstacle, and it is what made "top none" appear next to a
		// face distance proving something was there.
		const FVector Feet = Origin - FVector(0.0f, 0.0f, HalfHeight);
		const FVector ProbeTop = Ahead + FVector(0.0f, 0.0f, MaxJumpHeight * 3.0f);
		const FVector ProbeBottom = Feet - FVector(0.0f, 0.0f, ProbeBelowFeetDepth);

		FHitResult TopHit;
		if (World->LineTraceSingleByChannel(TopHit, ProbeTop, ProbeBottom, ECC_WorldStatic, Params))
		{
			return static_cast<float>(TopHit.Location.Z);
		}
	}

	// Genuinely nothing solid at any tested depth inside the face: a thin railing, a decal
	// bound to collision, or an obstacle whose near edge the feeler clipped at an angle. Not
	// jumpable is the right answer - there is no top to clear.
	//
	// Reported once per chaser, because "top none" on its own does not say whether every
	// stride missed a real obstacle or whether the strides were aimed somewhere empty. The
	// depth of each cast distinguishes them, and this is the line that was missing when the
	// feature looked broken for three rounds.
	if (!bHasLoggedHeightMiss)
	{
		bHasLoggedHeightMiss = true;
		const float FeetZ2 = Origin.Z - HalfHeight;
		UE_LOG(LogPursuitAI, Warning,
			TEXT("PursuitCharacter: %s height probe found NOTHING at face %.0f cm - tried depths ")
			TEXT("%.0f..%.0f cm past the face (step %.0f), each cast from z=%.0f down to z=%.0f (feet %.0f)"),
			*GetName(), FaceDistance,
			ProbeIntoFaceDepth, ProbeIntoFaceDepth + ProbeIntoFaceStepSize * (Steps - 1),
			ProbeIntoFaceStepSize,
			Origin.Z + JumpFaceProbeHeight - HalfHeight + MaxJumpHeight * 3.0f,
			FeetZ2 - ProbeBelowFeetDepth, FeetZ2);
	}

	return -FLT_MAX;
}

bool APursuitCharacter::IsObstacleJumpable(float ObstacleTopZ) const
{
	if (ObstacleTopZ == -FLT_MAX)
	{
		return false;
	}

	// Measured from the character's FEET, not its origin. The capsule origin sits half a
	// capsule above the ground, so comparing a world Z against it directly would make every
	// obstacle look 88 cm taller than it is - and the effect of that error is a chaser that
	// refuses to jump anything, which reads as "the feature does not work" rather than as an
	// off-by-a-capsule.
	const float HalfHeight = GetCapsuleComponent() ? GetCapsuleComponent()->GetScaledCapsuleHalfHeight() : 88.0f;
	const float FeetZ = GetActorLocation().Z - HalfHeight;
	const float HeightAboveFeet = ObstacleTopZ - FeetZ;

	// Too tall to clear: this is a wall, and steering around it is the honest answer.
	if (HeightAboveFeet > MaxJumpHeight)
	{
		return false;
	}

	// Below the step-up the capsule already handles for free, so a jump would be wasted
	// motion - the movement component walks over a kerb this small without being asked.
	// This is also what rejects bare ground, which the downward probe reports at ~0 cm.
	const UCharacterMovementComponent* Move = GetCharacterMovement();
	const float StepUp = Move ? Move->MaxStepHeight : 45.0f;
	if (HeightAboveFeet < StepUp)
	{
		return false;
	}

	return true;
}

FVector APursuitCharacter::SeparationPush() const
{
	FVector Push = FVector::ZeroVector;
	const FVector Mine = GetActorLocation();

	for (TActorIterator<APursuitCharacter> It(GetWorld()); It; ++It)
	{
		if (*It == this || !It->bIsChaser)
		{
			continue;
		}

		FVector Away = Mine - It->GetActorLocation();
		Away.Z = 0.0f;
		const float Distance = Away.Size();

		// Weighted by how close they are, so the push fades out rather than switching off.
		if (Distance > KINDA_SMALL_NUMBER && Distance < SeparationRadius)
		{
			Push += (Away / Distance) * (1.0f - Distance / SeparationRadius);
		}
	}

	return Push;
}

void APursuitCharacter::ReportCatch()
{
	if (CatchCooldown > 0.0f)
	{
		return;
	}

	// One report per catch, not one per frame: four chasers arriving together should cost one
	// round, and without this each of them would report on the same frame.
	CatchCooldown = 1.5f;

	if (UWorld* World = GetWorld())
	{
		if (APursuitPlayGameMode* PlayMode = Cast<APursuitPlayGameMode>(World->GetAuthGameMode()))
		{
			PlayMode->HandlePlayerCaught(this);
		}
	}
}

// ---------------------------------------------------------------------------
// Input
//
// Legacy axis and action mappings from Config/DefaultInput.ini rather than Enhanced Input
// assets. Both still work in 5.7 - UEnhancedPlayerInput::EvaluateInputComponentDelegates calls
// Super:: and keeps the result as "bLegacyBlocksInput" - and this route needs no .uasset to
// exist, which is what lets the playable prototype be rebuilt from a clean checkout by running
// two commandlets and nothing else.
// ---------------------------------------------------------------------------

void APursuitCharacter::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
	Super::SetupPlayerInputComponent(PlayerInputComponent);

	if (!PlayerInputComponent)
	{
		return;
	}

	PlayerInputComponent->BindAxis(TEXT("MoveForward"), this, &APursuitCharacter::InputMoveForward);
	PlayerInputComponent->BindAxis(TEXT("MoveRight"), this, &APursuitCharacter::InputMoveRight);
	PlayerInputComponent->BindAxis(TEXT("Turn"), this, &APursuitCharacter::InputTurn);
	PlayerInputComponent->BindAxis(TEXT("LookUp"), this, &APursuitCharacter::InputLookUp);

	PlayerInputComponent->BindAction(TEXT("Jump"), IE_Pressed, this, &APursuitCharacter::InputJumpPressed);
	PlayerInputComponent->BindAction(TEXT("Jump"), IE_Released, this, &APursuitCharacter::InputJumpReleased);
	PlayerInputComponent->BindAction(TEXT("Sprint"), IE_Pressed, this, &APursuitCharacter::InputSprintPressed);
	PlayerInputComponent->BindAction(TEXT("Sprint"), IE_Released, this, &APursuitCharacter::InputSprintReleased);
}

void APursuitCharacter::InputMoveForward(float Value)
{
	if (FMath::IsNearlyZero(Value) || !Controller)
	{
		return;
	}

	// Yaw only: pitch belongs to the camera, and a character that walks into the floor when you
	// look down is the classic version of this bug.
	const FRotator YawOnly(0.0f, Controller->GetControlRotation().Yaw, 0.0f);
	AddMovementInput(FRotationMatrix(YawOnly).GetUnitAxis(EAxis::X), Value);
}

void APursuitCharacter::InputMoveRight(float Value)
{
	if (FMath::IsNearlyZero(Value) || !Controller)
	{
		return;
	}

	const FRotator YawOnly(0.0f, Controller->GetControlRotation().Yaw, 0.0f);
	AddMovementInput(FRotationMatrix(YawOnly).GetUnitAxis(EAxis::Y), Value);
}

void APursuitCharacter::InputTurn(float Value)
{
	AddControllerYawInput(Value * LookSensitivity);
}

void APursuitCharacter::InputLookUp(float Value)
{
	AddControllerPitchInput(Value * LookSensitivity);
}

void APursuitCharacter::InputJumpPressed()
{
	Jump();
}

void APursuitCharacter::InputJumpReleased()
{
	StopJumping();
}

void APursuitCharacter::InputSprintPressed()
{
	if (UCharacterMovementComponent* Move = GetCharacterMovement())
	{
		Move->MaxWalkSpeed = SprintSpeed;
	}
}

void APursuitCharacter::InputSprintReleased()
{
	if (UCharacterMovementComponent* Move = GetCharacterMovement())
	{
		Move->MaxWalkSpeed = PlayerSpeed;
	}
}
