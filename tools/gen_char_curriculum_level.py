# Copyright Epic Games, Inc. All Rights Reserved.
#
# Generates the character-curriculum levels headlessly. One script, one variant per run -
# no duplicated copy of the bake logic per stage.
#
# Run headless:
#   tools\run_pyscript.ps1 -Script "<project>\tools\gen_char_curriculum_level.py" -Log <log>
#   tools\run_pyscript.ps1 -Script "<project>\tools\gen_char_curriculum_level.py" -Log <log> (with
#     PURSUIT_LEVEL_VARIANT=moving025 exported first)
#
# ---------------------------------------------------------------------------
# Variants (selected by the PURSUIT_LEVEL_VARIANT environment variable)
# ---------------------------------------------------------------------------
#   (unset) / "curriculum"  Stage 0 minimum scenario, L_PursuitCharCurriculum
#       flat floor            : bBuildArenaRig=True      (rig floor + boundary ring)
#       no internal obstacles : bSpawnArenaObstacles=False
#       static target         : bStaticTarget=True       (the evader exists, never flees)
#       jump not executed     : bEnableAgentJump=False    (the 3rd action dim still exists)
#
#   "moving025"             Stage 2A, L_PursuitCharMoving025
#       same floor / same obstacle-free arena / jump still disabled, but the evader now
#       FLEES at EvaderSpeedRatio=0.25 of the chaser's top speed. Nothing else changes:
#       EpisodeSeconds, the reward terms, the spawn rules and the 15D/3D interface are
#       inherited from the class defaults, which is the point - Stage 2A is meant to
#       change exactly ONE variable against the Stage 0/1 baseline.
#
#   "moving050"             Stage 2C, L_PursuitCharMoving050
#       identical bake to moving025 except the flee speed is raised to 0.50 of the
#       chaser's top speed. Still the one-variable-at-a-time contract.
#
#   "pillar"                Stage 3A, L_PursuitCharPillar
#       Moving050's arena and moving target, plus ONE 300 x 300 x 400 cm BlockAll pillar
#       through the arena centre (built in C++ by bStage3PillarLayout, so it is registered
#       with the same spawn-avoidance disc the greybox table uses). Jump stays disabled.
#       The spawn pair is CONSTRUCTED, not drawn, and alternates by episode parity:
#         even episode -> chaser (-300, 0), evader (+300, 0): the pillar is exactly on the
#                         straight line between them.
#         odd  episode -> the same pair translated +600 on Y: same separation, same facing,
#                         same target bearing, nothing on the line.
#       That pair of layouts is the whole point of the stage: an area-uniform draw over a
#       1200 cm disc almost never straddles a 300 cm central pillar, so "blocked" and
#       "clear" have to be constructed to be comparable. The observation (15D), the action
#       space (3D), the reward terms and the physics are identical to Moving050's.
#
# The interface is deliberately UNCHANGED in every variant: 15 observation dims,
# 3 action dims. A variant is not "a different task", it is the same task with a
# different single confound, which is the only way its numbers can be compared.
#
# ---------------------------------------------------------------------------
# Safety rules, enforced in code below
# ---------------------------------------------------------------------------
#  * A run writes exactly ONE level - the one its variant names. The greybox level
#    (L_PursuitCharTrain), the city levels (Demonstration / Demonstration_Train /
#    Overview) and the OTHER curriculum variant are never opened, never saved, and their
#    paths are refused outright.
#  * Re-running a variant is idempotent: the existing level is opened, the flags are
#    re-asserted, and it is saved again.
#  * Every declared property is asserted after the save, so a silently-ignored
#    set_editor_property (a renamed UPROPERTY, a bad specifier) fails the run instead of
#    producing a level that quietly keeps the wrong setting.
#  * The selector only ever picks from VARIANTS. An unknown value fails the run rather
#    than falling back to a default level, so a typo cannot silently re-bake the
#    static curriculum (or the moving one) when the other was intended.

import os

import unreal

LEVEL_DIR = "/Game/Maps"

ENV_CLASS_PATH = "/Script/PursuitAI.PursuitCharEnv"
ENV_CLASS_NAME = "PursuitCharEnv"
AGENT_CLASS_PATH = "/Script/PursuitAI.PursuitCharAgent"
AGENT_CLASS_NAME = "PursuitCharAgent"

AGENT_SPAWN_Z = 95.0  # capsule centre above the floor (half height 88 + margin)

VARIANT_ENV_VAR = "PURSUIT_LEVEL_VARIANT"

# Levels no variant may ever touch. Checked as a guard rather than assumed: a typo in a
# level path would otherwise silently overwrite the greybox or the city training map.
#
# L_PursuitCharLowWall joined this list on 2026-09-23 (Stage 4B). It is the level Stage 4A's
# three evidence chains were produced on - the oracle, the jump-disabled control and the
# frozen rep3 zero-shot evaluation - so every number in STAGE4A_RESULT.md is anchored to the
# bytes of that one asset. Stage 4B needs the SAME geometry with two reward coefficients
# zeroed, which is a new asset (see the "lowwall_jumpable" variant), not an edit to this one.
# Re-baking it "just to pick up a property" would silently invalidate all three chains while
# leaving the file name intact, so the generator refuses outright rather than trusting nobody
# will ask. Removing the entry below is the only way back in, and that is deliberate: it is a
# one-line, reviewable, deliberate act rather than an accident.
FORBIDDEN_PATHS = [
    LEVEL_DIR + "/L_PursuitCharTrain",
    LEVEL_DIR + "/L_PursuitAITrain",
    LEVEL_DIR + "/L_PursuitPlay",
    LEVEL_DIR + "/L_PursuitCharLowWall",
    "/Game/Cartoon_City_Free/Maps/Demonstration",
    "/Game/Cartoon_City_Free/Maps/Demonstration_Train",
    "/Game/Cartoon_City_Free/Maps/Overview",
]

# The bake for each variant: property name -> expected value. Used both to set and to
# verify. Everything a variant does not name keeps the class default, on purpose.
VARIANTS = {
    "demo_circle": {
        "level_path": LEVEL_DIR + "/L_PursuitCharDemoCircle",
        "flags": {
            "bBuildArenaRig": True,
            "bSpawnArenaObstacles": False,
            "bStaticTarget": False,
            "bEnableAgentJump": False,
            "EvaderSpeedRatio": 0.50,
            "RigWallHeight": 400.0,
            "EpisodeSeconds": 6.0,
            "MinSpawnSeparation": 450.0,
            "MaxSpawnSeparation": 600.0,
        },
        "note": "standalone portfolio circle: floor, 400 cm boundary ring, lights, two characters, "
                "Moving050 policy, 450-600 cm opening, 6 s cap; no internal obstacles or jump",
    },
    "curriculum": {
        "level_path": LEVEL_DIR + "/L_PursuitCharCurriculum",
        "flags": {
            "bBuildArenaRig": True,        # flat floor + boundary ring
            "bSpawnArenaObstacles": False,  # no internal obstacles
            "bStaticTarget": True,         # the evader never flees
            "bEnableAgentJump": False,     # jump intent is read, never executed
            "EvaderSpeedRatio": 0.70,      # inert while bStaticTarget is True
        },
        "note": "flat floor + boundary ring, no internal obstacles, "
                "static target, jump disabled",
    },
    "moving025": {
        "level_path": LEVEL_DIR + "/L_PursuitCharMoving025",
        "flags": {
            "bBuildArenaRig": True,        # flat floor + boundary ring
            "bSpawnArenaObstacles": False,  # no internal obstacles (Stage 2A stays obstacle-free)
            "bStaticTarget": False,        # the evader FLEES - the one variable Stage 2A turns on
            "bEnableAgentJump": False,     # jump intent is read, never executed
            "EvaderSpeedRatio": 0.25,      # the Stage 2A speed ratio under test
        },
        "note": "flat floor + boundary ring, no internal obstacles, "
                "MOVING target at EvaderSpeedRatio=0.25, jump disabled",
    },
    "moving050": {
        "level_path": LEVEL_DIR + "/L_PursuitCharMoving050",
        "flags": {
            "bBuildArenaRig": True,        # flat floor + boundary ring
            "bSpawnArenaObstacles": False,  # no internal obstacles (Stage 2C stays obstacle-free)
            "bStaticTarget": False,        # the evader FLEES - the one variable Stage 2C turns up
            "bEnableAgentJump": False,     # jump intent is read, never executed
            "EvaderSpeedRatio": 0.50,      # the Stage 2C speed ratio under test
        },
        "note": "flat floor + boundary ring, no internal obstacles, "
                "MOVING target at EvaderSpeedRatio=0.50, jump disabled",
    },
    "pillar": {
        "level_path": LEVEL_DIR + "/L_PursuitCharPillar",
        "flags": {
            "bBuildArenaRig": True,        # floor + boundary ring (the arena the pair is built in)
            "bSpawnArenaObstacles": False,  # the ONE Stage 3 pillar is built by bStage3PillarLayout
            "bStaticTarget": False,        # same moving target as Moving050
            "bEnableAgentJump": False,     # Stage 3 keeps jump disabled - routing is the skill
            "EvaderSpeedRatio": 0.50,      # Moving050's ratio, unchanged, so behaviour is comparable
            # --- the Stage 3 additions, all defaulted OFF in the class ---
            "bStage3PillarLayout": True,
            "Stage3PillarFootprintCm": 300.0,
            "Stage3PillarHeightCm": 400.0,
            "Stage3SpawnSeparationCm": 600.0,
            "Stage3ClearLateralOffsetCm": 600.0,
            "Stage3SpawnYawDegrees": 0.0,
            # bStaticTarget/hop/etc. above are the ONLY moving parts; the observation (15D),
            # the action space (3D), the reward and the physics are untouched.
        },
        "note": "flat floor + boundary ring + ONE 300x300x400 cm pillar through the arena "
                "centre, moving target at EvaderSpeedRatio=0.50, jump disabled, and a "
                "CONSTRUCTED spawn pair that alternates by episode parity between "
                "'pillar blocks the straight line' and 'straight line clear' "
                "(separation 600 cm, 0 / +600 cm lateral offset).",
    },
    "lowwall": {
        "level_path": LEVEL_DIR + "/L_PursuitCharLowWall",
        "flags": {
            "bBuildArenaRig": True,        # floor + boundary ring; the ring is the ONLY 400 cm wall
            "bSpawnArenaObstacles": False,  # the ONE Stage 4 wall is built by bStage4WallLayout
            "bStaticTarget": False,        # same moving target, same ratio as Stage 3 / Moving050
            # ---- THE ONE VARIABLE STAGE 4A TURNS ON ----------------------------------
            # Every level before this one baked jump OFF, which is why no policy in this
            # repository has ever had a reason to shape its third action dimension: with
            # bEnableAgentJump false, JumpActionPenalty is gated off as well, so the jump
            # input was neither executed NOR scored. Stage 4A is the first level where the
            # third action dim can do anything, and that is the whole point.
            "bEnableAgentJump": True,
            "EvaderSpeedRatio": 0.50,
            # --- the Stage 4 additions, all defaulted OFF in the class ---
            "bStage4WallLayout": True,
            # 55 cm is chosen to sit strictly INSIDE the jump envelope and OUTSIDE the step
            # envelope for the RL chaser (ACharacter defaults: JumpZVelocity 420, GravityScale
            # 1.0 -> apex 90 cm; MaxStepHeight 45 cm). BuildStage4Wall re-measures all three off
            # the live character at BeginPlay and logs the verdict, so this number is checked by
            # the run rather than trusted by it.
            #
            # Not 70: the scripted driver launches on the actuator's fixed 0.3 s cooldown, so at
            # ~575 cm/s it starts a hop every ~172 cm, and a height whose clearable launch window
            # is narrower than that can be missed by timing alone. 70 cm gives a ~125 cm window
            # (missable); 55 cm gives ~199 cm (not missable). The band is swept on this same level
            # with -PursuitStage4WallHeight= and reported rather than baked once and believed.
            "Stage4WallHeightCm": 55.0,
            "Stage4WallThicknessCm": 40.0,
            # 0 = span the whole arena (2 * ArenaRadius + 200 = 2600 cm). Anything shorter
            # leaves a walk-around and the must-jump group silently becomes a second Stage 3.
            "Stage4WallLengthCm": 0.0,
            "Stage4SpawnSeparationCm": 600.0,
            # must-jump: (-300, +300) -> no-jump: (-900, -300). Same separation, same facing,
            # same target bearing; only the side of the wall changes.
            "Stage4ClearShiftCm": -600.0,
            # Shared with Stage 3 on purpose: the subject of this stage is the WALL, so where
            # the target is must not be a variable under test.
            "Stage3SpawnYawDegrees": 0.0,
        },
        "note": "flat floor + boundary ring + ONE ~40 x 2600 x 70 cm low wall through the arena "
                "centre, moving target at EvaderSpeedRatio=0.50, and JUMP ENABLED (the one "
                "variable this stage turns on). Spawn pair is CONSTRUCTED and alternates by "
                "episode parity: 'must_jump' puts the pair on opposite sides of a wall that "
                "spans the whole arena (no lateral detour exists), 'no_jump' translates the "
                "same pair so both agents are on one side (same separation, same bearing, wall "
                "never crossed). Observation (15D), action space (3D), reward and physics are "
                "identical to Stage 3's.",
    },
    # ---------------------------------------------------------------------------
    # "lowwall_jumpable"      Stage 4B, L_PursuitCharLowWallJumpable
    # ---------------------------------------------------------------------------
    # Stage 4A left one thing unresolved that it could not have resolved without training:
    # every level this repository had ever baked ran with bEnableAgentJump=False, and with
    # the gate off JumpActionPenalty is gated off too - so the third action dimension was
    # never EXECUTED and never SCORED, and "the zero-shot policy cannot jump the wall" is a
    # construction result, not a measurement of the policy. Stage 4B's question is therefore
    # "what would it take for a successful jump to be worth anything to PPO", and this level
    # is the first half of the answer: the SAME wall, the SAME spawn pair, the SAME 15D/3D
    # interface and the SAME seeds as Stage 4A, with the two jump-cost coefficients zeroed.
    #
    # Why a separate asset rather than a flag override: the coefficient has no command-line
    # switch (SetEnvironmentOptions can set it, but only a trainer calls that, and the
    # watched oracle/control runs have no trainer attached). A second level keeps the two
    # settings' evidence separable and hashable, which is exactly what the "don't touch the
    # frozen level" rule needs.
    #
    # The geometry block is a deliberate duplicate of "lowwall", not an import: these two
    # levels must be comparable field by field, and a shared helper would let the geometry
    # drift on one of them without the diff showing it. The read-back below asserts every
    # field it sets, and the differential check in logs/stage4b re-runs the SAME seed on
    # both levels and requires byte-identical episode outcomes - the reward coefficients are
    # invisible to the scripted drivers, so any behavioural difference would mean the copy
    # is not a copy. Two independent paths, one answer.
    #
    # What is NOT changed here, and must stay unchanged: the observation (15D), the action
    # space (3D), EpisodeSeconds, CatchRadius, the shaping terms, the wall spec and the spawn
    # pair. The only two fields that differ from "lowwall" are named in "note".
    "lowwall_jumpable": {
        "level_path": LEVEL_DIR + "/L_PursuitCharLowWallJumpable",
        "flags": {
            "bBuildArenaRig": True,        # floor + boundary ring; the ring is the ONLY 400 cm wall
            "bSpawnArenaObstacles": False,  # the ONE Stage 4 wall is built by bStage4WallLayout
            "bStaticTarget": False,        # same moving target, same ratio as Stage 3 / Moving050
            "bEnableAgentJump": True,      # identical to Stage 4A - the gate is NOT the variable here
            "EvaderSpeedRatio": 0.50,
            "bStage4WallLayout": True,
            "Stage4WallHeightCm": 55.0,     # identical to Stage 4A: 45 < 55 < 90
            "Stage4WallThicknessCm": 40.0,
            "Stage4WallLengthCm": 0.0,      # 0 = span the arena (2600 cm); nothing detours around it
            "Stage4SpawnSeparationCm": 600.0,
            "Stage4ClearShiftCm": -600.0,   # must_jump / no_jump pair, unchanged
            "Stage3SpawnYawDegrees": 0.0,
            # ---- THE ONLY TWO VARIABLES STAGE 4B TURNS ------------- ----------------
            # Measured, before being set to zero (Stage 4B audit, source of truth is the
            # class default): JumpActionPenalty -0.5 per request-step and AirbornePenalty
            # -0.05 per airborne step. On the 55 cm wall the scripted oracle needs ~33
            # airborne steps per crossing and the policy's only route to the catch that jump
            # unlocks is worth CatchReward +10 plus at most ~+3 of proximity shaping, so at
            # the default coefficients a crossing is a NET LOSS and PPO's advantage
            # estimate has no reason to move the jump dimension at all. Zeroing them does
            # not add a reward for jumping - it removes a charge that paid nothing for the
            # only action that solves the layout. Any incentive to jump still has to come
            # from the task itself.
            "JumpActionPenalty": 0.0,
            "AirbornePenalty": 0.0,
        },
        "note": "Stage 4A's low-wall level with JumpActionPenalty and AirbornePenalty set to 0. "
                "Geometry, spawn pair, wall spec (55 cm), seed set and the 15D/3D interface are "
                "byte-for-byte the same intent as L_PursuitCharLowWall; the ONLY difference is "
                "those two reward coefficients. The frozen Stage 4A level is in FORBIDDEN_PATHS "
                "and cannot be reached from here.",
    },
}


def log(message):
    unreal.log("[PursuitAI/gen_curriculum] " + str(message))


def fail(message):
    unreal.log_error("[PursuitAI/gen_curriculum] " + str(message))
    raise RuntimeError(message)


def select_variant():
    """Resolve the variant from the environment, defaulting to the Stage 0 curriculum."""
    raw = os.environ.get(VARIANT_ENV_VAR, "").strip().lower()
    key = raw if raw else "curriculum"
    if key not in VARIANTS:
        fail("unknown %s=%r - known variants: %s"
             % (VARIANT_ENV_VAR, raw, ", ".join(sorted(VARIANTS.keys()))))
    return key


def find_actors(actor_subsystem, class_name):
    return [
        actor
        for actor in actor_subsystem.get_all_level_actors()
        if actor.get_class().get_name() == class_name
    ]


def main():
    level_editor = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    actor_subsystem = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    asset_library = unreal.EditorAssetLibrary

    if level_editor is None or actor_subsystem is None:
        fail("editor subsystems unavailable - is this running as an editor commandlet?")

    variant_key = select_variant()
    variant = VARIANTS[variant_key]
    level_path = variant["level_path"]
    flags = variant["flags"]

    log("variant=%s level=%s" % (variant_key, level_path))

    if level_path in FORBIDDEN_PATHS:
        fail("level path is one of the protected maps: " + level_path)

    # Belt and braces: the variant's target must not collide with a protected map either.
    for forbidden in FORBIDDEN_PATHS:
        if level_path == forbidden:
            fail("variant %s targets a protected map: %s" % (variant_key, level_path))

    if not asset_library.does_directory_exist(LEVEL_DIR):
        asset_library.make_directory(LEVEL_DIR)

    # --- 0. Release the protected maps if the editor happens to have one open ---------
    # A commandlet starts with an empty world, but a stray editor session would not. Saving
    # this variant's level would then be harmless, but a protected map could be dirtied by
    # the property writes below if the wrong world were current. Cheap and explicit.
    current_level = level_editor.get_current_level().get_path_name() if level_editor.get_current_level() else ""
    log("current level before: " + current_level)

    if current_level and current_level.split(".")[0] in FORBIDDEN_PATHS:
        fail("a protected map is currently open: " + current_level)

    if asset_library.does_asset_exist(level_path):
        log("level exists, opening " + level_path)
        if not level_editor.load_level(level_path):
            fail("failed to load existing level " + level_path)
    else:
        log("creating level " + level_path)
        if not level_editor.new_level(level_path):
            fail("failed to create level " + level_path)

    # --- 1. Exactly one environment actor, at the arena centre ---
    env_class = unreal.load_class(None, ENV_CLASS_PATH)
    if env_class is None:
        fail("could not load class " + ENV_CLASS_PATH + " (is the module compiled?)")

    env_actors = find_actors(actor_subsystem, ENV_CLASS_NAME)
    if env_actors:
        log("environment actor already present")
    else:
        spawned = actor_subsystem.spawn_actor_from_class(
            env_class, unreal.Vector(0.0, 0.0, 0.0), unreal.Rotator(0.0, 0.0, 0.0))
        if spawned is None:
            fail("failed to spawn " + ENV_CLASS_PATH)
        spawned.set_actor_label("PursuitCharEnv")
        env_actors = [spawned]
        log("spawned environment actor")
    env = env_actors[0]

    # --- 2. The variant flag bake ---
    for name, value in flags.items():
        env.set_editor_property(name, value)
        log("set %s = %s" % (name, value))

    # --- 3. Exactly two character agents ---
    agents = find_actors(actor_subsystem, AGENT_CLASS_NAME)
    if len(agents) < 2:
        missing = 2 - len(agents)
        for _ in range(missing):
            actor_subsystem.spawn_actor_from_class(
                unreal.load_class(None, AGENT_CLASS_PATH),
                unreal.Vector(0.0, 0.0, AGENT_SPAWN_Z), unreal.Rotator(0.0, 0.0, 0.0))
        agents = find_actors(actor_subsystem, AGENT_CLASS_NAME)
        log("spawned %d agent(s)" % missing)
    else:
        log("agents already present (%d)" % len(agents))

    if len(agents) != 2:
        fail("expected exactly 2 %s actors, found %d" % (AGENT_CLASS_NAME, len(agents)))
    if len(agents) > 2:
        fail("more than 2 %s actors - refusing to guess which pair is the task"
             % AGENT_CLASS_NAME)

    chaser, evader = agents[0], agents[1]
    chaser.set_actor_label("PursuitChaser")
    evader.set_actor_label("PursuitEvader")

    # --- 4. Wire the env's references and persist ---
    env.set_editor_property("ChaserAgent", chaser)
    env.set_editor_property("EvaderAgent", evader)
    log("env references wired: chaser=%s evader=%s" % (chaser.get_name(), evader.get_name()))

    if not level_editor.save_current_level():
        fail("save_current_level failed for " + level_path)

    # --- 5. Verify the bake survived the save ----------------------------------------
    # Written-after-save reads: if set_editor_property was a no-op (renamed property, wrong
    # specifier, a flag that is not a UPROPERTY) the value reads back as the class default
    # and this fails, which is the whole point of the check. A Stage 2A level that silently
    # kept bStaticTarget=True would produce a "the target never moved" report that looks
    # like an environment bug; a Stage 0 level that silently picked up a moving evader
    # would invalidate the baseline it exists to be.
    readback = {}
    for name, expected in flags.items():
        actual = env.get_editor_property(name)
        readback[name] = actual
        if isinstance(expected, float) or isinstance(actual, float):
            if abs(float(actual) - float(expected)) > 1e-4:
                fail("%s read back as %s, expected %s - the bake did not take"
                     % (name, actual, expected))
        elif actual != expected:
            fail("%s read back as %s, expected %s - the bake did not take"
                 % (name, actual, expected))

    log("VERIFIED " + level_path + " -> " + ", ".join(
        "%s=%s" % (k, readback[k]) for k in sorted(readback.keys())))
    log("OK - %s saved (%s)" % (level_path, variant["note"]))


main()
