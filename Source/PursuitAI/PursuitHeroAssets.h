// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "PursuitCharacter.h"
#include "UObject/SoftObjectPath.h"

/**
 * The shared rig table: which skeletal mesh + locomotion clips belong to each hero.
 *
 * Extracted verbatim from PursuitCharacter.cpp so BOTH character classes speak the same
 * asset language - v1's APursuitCharacter (play demo) and v2's APursuitCharAgent (RL
 * environment). A rig added here is available to both; an asset moved without updating
 * this table breaks both at once, which is the point: one source of truth.
 */
namespace PursuitCharacterPaths
{
	// ---- the engine mannequin: the fallback rig ---------------------------

	// Epic ships this rig inside the MoverExamples plugin (Experimental, off by default in the
	// engine - PursuitAI.uproject is what turns it on). It is the only complete mannequin in a
	// bare install: SKM_Manny_Simple plus 35 sequences, jump / fall / land among them. Nothing
	// to source and nothing to model, which is why the character layer costs no art time.
	// Internal-linkage pointer-to-const: `const TCHAR* const`, not `const TCHAR*` - the
	// latter is a non-const POINTER with external linkage and LNK2005s across TUs.
	const TCHAR* const MannequinMeshPath =
		TEXT("/MoverExamples/Characters/Mannequins/Meshes/SKM_Manny_Simple.SKM_Manny_Simple");
	const TCHAR* const MannequinAnimFolder = TEXT("/MoverExamples/Characters/Mannequins/Animations/Manny/");

	inline FSoftObjectPath MannequinClip(const TCHAR* Name)
	{
		return FSoftObjectPath(FString::Printf(TEXT("%s%s.%s"), MannequinAnimFolder, Name, Name));
	}

	// ---- RPG Hero Squad: the rigs the project actually wants ---------------

	const TCHAR* const PackRoot = TEXT("/Game/RPGHeroSquad");

	inline FSoftObjectPath PackMesh(const TCHAR* Name)
	{
		return FSoftObjectPath(FString::Printf(TEXT("%s/Mesh/Character/%s.%s"), PackRoot, Name, Name));
	}

	/**
	 * "Animation/TinyHero/Anim_Idle_Normal_TinyHero"
	 *   -> "/Game/RPGHeroSquad/Animation/TinyHero/Anim_Idle_Normal_TinyHero.Anim_Idle_Normal_TinyHero"
	 *
	 * The object name repeats the leaf, which is the pack's own convention. Derived from the
	 * folder path rather than spelled out at every call site because the calls come in groups
	 * of eight and the folder is the only part that repeats.
	 */
	inline FSoftObjectPath PackClip(const TCHAR* Relative)
	{
		const FString Path(Relative);
		int32 Slash = INDEX_NONE;
		Path.FindLastChar(TEXT('/'), Slash);
		const FString Leaf = (Slash == INDEX_NONE) ? Path : Path.RightChop(Slash + 1);
		return FSoftObjectPath(FString::Printf(TEXT("%s/%s.%s"), PackRoot, *Path, *Leaf));
	}

	// Carries a vector parameter called "Color", matching what the RL environment's spheres
	// are tinted through, so both maps speak the same colour language.
	//
	// The project's own material rather than /Engine/BasicShapes/BasicShapeMaterial, and the
	// difference is not cosmetic: the engine asset has no bUsedWithSkeletalMesh usage flag, so
	// an instance of it on a skeletal mesh is thrown away at runtime in favour of the default
	// material. The log is explicit about it ("missing bUsedWithSkeletalMesh=True! Default
	// Material will be used in game") and the symptom is a character that renders grey no
	// matter what the tint code does. tools/gen_tint_material.py creates the replacement.
	const TCHAR* const TintMaterialPath = TEXT("/Game/Materials/M_PursuitTint.M_PursuitTint");

	// Only used if the project asset is missing, which means the content was not generated.
	// Grey characters beat invisible ones, and the log says which happened.
	const TCHAR* const FallbackTintMaterialPath = TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial");

	/** One rig's complete asset set, already resolved to object paths. */
	struct FSkeletalHeroAssets
	{
		const TCHAR* Label = TEXT("Mannequin");
		FSoftObjectPath Mesh;
		FSoftObjectPath Idle;
		FSoftObjectPath Walk;
		FSoftObjectPath Run;
		FSoftObjectPath Sprint;
		FSoftObjectPath Jump;
		FSoftObjectPath Fall;
		FSoftObjectPath Land;

		bool bTint = true;

		// A stride belongs to a rig, not to the class, so these travel with the rig.
		float WalkClipSpeed = 220.0f;
		float RunClipSpeed = 600.0f;
		float SprintClipSpeed = 700.0f;

		/** How many clip slots this rig fills, so the load log can say "5/5" rather than "5". */
		int32 ExpectedClips() const
		{
			int32 Count = 0;
			for (const FSoftObjectPath* Path : { &Idle, &Walk, &Run, &Sprint, &Jump, &Fall, &Land })
			{
				Count += Path->IsValid() ? 1 : 0;
			}
			return Count;
		}
	};

	/**
	 * The one place that knows which assets belong to which hero.
	 *
	 * Skeleton pairings are not free: each of the three pack rigs has its own skeleton, so a clip
	 * from one on the mesh of another produces nothing at all. Everything below is per rig for
	 * that reason, and tools/inspect_hero_bps.py prints the skeleton each clip actually names.
	 *
	 * What each rig cannot do is as important as what it can. Only the Tiny Hero has jump clips,
	 * so the player gets that rig; the chasers never leave the ground and so lose nothing.
	 */
	inline FSkeletalHeroAssets HeroAssets(EPursuitHeroModel Model)
	{
		FSkeletalHeroAssets Assets;

		switch (Model)
		{
		case EPursuitHeroModel::TinyHero:
			Assets.Label = TEXT("TinyHero");
			Assets.Mesh = PackMesh(TEXT("SK_TinyHeroPBR"));
			Assets.Idle = PackClip(TEXT("Animation/TinyHero/Anim_Idle_Normal_TinyHero"));
			Assets.Walk = PackClip(TEXT("Animation/TinyHero/InPlace/Anim_MoveFWD_Normal_InPlace_TinyHero"));
			Assets.Run = PackClip(TEXT("Animation/TinyHero/InPlace/Anim_MoveFWD_Battle_InPlace_TinyHero"));
			Assets.Sprint = PackClip(TEXT("Animation/TinyHero/InPlace/Anim_SprintFWD_Battle_InPlace_TinyHero"));
			// One airborne loop covers both halves of the arc - the pack has no separate rising
			// and falling clip for this rig. PlayClip() deduplicates by pointer, so the
			// rising/falling latch below is a no-op here rather than a strobe.
			Assets.Jump = PackClip(TEXT("Animation/TinyHero/InPlace/Anim_JumpAir_Normal_InPlace_TinyHero"));
			Assets.Fall = PackClip(TEXT("Animation/TinyHero/InPlace/Anim_JumpAir_Normal_InPlace_TinyHero"));
			Assets.Land = PackClip(TEXT("Animation/TinyHero/InPlace/Anim_JumpEnd_Normal_InPlace_TinyHero"));
			Assets.bTint = false;
			Assets.WalkClipSpeed = 200.0f;
			Assets.RunClipSpeed = 520.0f;
			Assets.SprintClipSpeed = 780.0f;
			break;

		case EPursuitHeroModel::AnimalHero:
			Assets.Label = TEXT("AnimalHero (the pack's dog)");
			Assets.Mesh = PackMesh(TEXT("SK_DogPBR"));
			// The only idle this rig ships is a combat one. That suits a chaser exactly, and it
			// is worth knowing before someone goes looking for a relaxed stand that isn't there.
			Assets.Idle = PackClip(TEXT("Animation/AnimalHero/Anim_Idle_Battle_AnimalHero"));
			Assets.Walk = PackClip(TEXT("Animation/AnimalHero/InPlace/Anim_WalkForwardBattle_IP_AnimalHero"));
			Assets.Run = PackClip(TEXT("Animation/AnimalHero/InPlace/Anim_RunForwardBattle_IP_AnimalHero"));
			Assets.Sprint = PackClip(TEXT("Animation/AnimalHero/InPlace/Anim_SprintForwardBattle_IP_AnimalHero"));
			// No jump clips at all for this rig, and no chaser ever jumps.
			Assets.bTint = false;
			Assets.WalkClipSpeed = 180.0f;
			Assets.RunClipSpeed = 470.0f;
			Assets.SprintClipSpeed = 640.0f;
			break;

		case EPursuitHeroModel::RPGHero:
			Assets.Label = TEXT("RPGHero");
			Assets.Mesh = PackMesh(TEXT("SK_RPGHeroPBR"));
			Assets.Idle = PackClip(TEXT("Animation/RPGHero/Anim_Idle_RPGHero"));
			Assets.Walk = PackClip(TEXT("Animation/RPGHero/InPlace/Anim_Walk_IP_RPGHero"));
			Assets.Run = PackClip(TEXT("Animation/RPGHero/InPlace/Anim_Run_IP_RPGHero"));
			Assets.Sprint = PackClip(TEXT("Animation/RPGHero/InPlace/Anim_Sprint_IP_RPGHero"));
			Assets.bTint = false;
			Assets.WalkClipSpeed = 200.0f;
			Assets.RunClipSpeed = 520.0f;
			Assets.SprintClipSpeed = 780.0f;
			break;

		case EPursuitHeroModel::Mannequin:
		default:
			Assets.Label = TEXT("Mannequin");
			Assets.Mesh = FSoftObjectPath(MannequinMeshPath);
			Assets.Idle = MannequinClip(TEXT("MM_Idle"));
			Assets.Walk = MannequinClip(TEXT("MM_Walk_Fwd"));
			Assets.Run = MannequinClip(TEXT("MM_Run_Fwd"));
			Assets.Jump = MannequinClip(TEXT("MM_Jump"));
			Assets.Fall = MannequinClip(TEXT("MM_Fall_Loop"));
			// MM_Land is deliberately absent. It is an *additive* sequence, and an
			// AnimSingleNodeInstance cannot play one - the engine says so every time it is tried
			// ("Setting an additive animation (MM_Land) on an AnimSingleNodeInstance is not
			// allowed. This will not function correctly in cooked builds!"), and it says it on
			// screen, where it sits on top of the HUD forever. Point LandAnim at a non-additive
			// clip from another rig and LandHoldSeconds starts working; the code path stays.
			Assets.Land = FSoftObjectPath();
			// Untextured and grey, so the tint is the only thing making it look like anything.
			Assets.bTint = true;
			Assets.WalkClipSpeed = 220.0f;
			Assets.RunClipSpeed = 600.0f;
			Assets.SprintClipSpeed = 700.0f;  // unused: this rig has no sprint clip
			break;
		}

		return Assets;
	}
}
