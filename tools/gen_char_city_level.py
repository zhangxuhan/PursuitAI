# Generates the v2 CITY training level (Demonstration_Train) headlessly.
#
#   UnrealEditor-Cmd PursuitAI.uproject -run=pythonscript -script=tools\gen_char_city_level.py -unattended -nosplash -nullrhi
#
# The training connector needs a map whose world contains the env actor with its
# agent references wired. For the city we do NOT touch the showcase map: this script
# duplicates Cartoon_City_Free/Maps/Demonstration into Demonstration_Train (a local,
# git-ignored Fab content path) and bakes exactly the same three actors as
# gen_char_train_level.py puts into the greybox - env, chaser, evader.
#
# City specifics, baked instead of parsed from the command line (the Schola
# connector's extra-args path is parser-fragile with dashed flags):
#   - env placed at the measured clean plaza (-250, 0, 70), v1's convention
#   - bBuildArenaRig=False (the city ground is the floor)
#   - bStartOnCityStage=True (baked -PursuitStage=city: spawn probing, leash,
#     plaza-capped spawn radius and the jump platforms all key off this)
#
# Idempotent: re-running tops up anything missing and re-saves.

import unreal

SOURCE_LEVEL = "/Game/Cartoon_City_Free/Maps/Demonstration"
TRAIN_LEVEL = "/Game/Cartoon_City_Free/Maps/Demonstration_Train"

ENV_CLASS_PATH = "/Script/PursuitAI.PursuitCharEnv"
ENV_CLASS_NAME = "PursuitCharEnv"
AGENT_CLASS_PATH = "/Script/PursuitAI.PursuitCharAgent"
AGENT_CLASS_NAME = "PursuitCharAgent"

# Plaza centre: arena z=70 over ground z=10 (tools/inspect_city_stage.py).
ENV_LOCATION = (-250.0, 0.0, 70.0)
AGENT_SPAWN_Z = 165.0  # capsule centre: arena 70 + 95 (half height 88 + margin)


def log(message):
    unreal.log("[PursuitAI/gen_char_city] " + str(message))


def fail(message):
    unreal.log_error("[PursuitAI/gen_char_city] " + str(message))
    raise RuntimeError(message)


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

    # --- 1. The train map: a duplicate of the demo map, created once ---
    if not asset_library.does_asset_exist(TRAIN_LEVEL):
        if not asset_library.does_asset_exist(SOURCE_LEVEL):
            fail("source level missing: " + SOURCE_LEVEL)
        if not asset_library.duplicate_asset(SOURCE_LEVEL, TRAIN_LEVEL):
            fail("failed to duplicate " + SOURCE_LEVEL + " -> " + TRAIN_LEVEL)
        log("duplicated demo map into " + TRAIN_LEVEL)
    else:
        log("train level already exists: " + TRAIN_LEVEL)

    if not level_editor.load_level(TRAIN_LEVEL):
        fail("failed to load " + TRAIN_LEVEL)

    # --- 2. Exactly one environment actor, at the plaza centre ---
    env_class = unreal.load_class(None, ENV_CLASS_PATH)
    if env_class is None:
        fail("could not load class " + ENV_CLASS_PATH + " (is the module compiled?)")

    env_actors = find_actors(actor_subsystem, ENV_CLASS_NAME)
    if env_actors:
        log("environment actor already present")
    else:
        spawned = actor_subsystem.spawn_actor_from_class(
            env_class, unreal.Vector(*ENV_LOCATION), unreal.Rotator(0.0, 0.0, 0.0))
        if spawned is None:
            fail("failed to spawn " + ENV_CLASS_PATH)
        spawned.set_actor_label("PursuitCharEnv")
        env_actors = [spawned]
        log("spawned environment actor")
    env = env_actors[0]

    # Baked city staging: no rig, city behaviours on, plaza centre from the transform.
    env.set_actor_location(unreal.Vector(*ENV_LOCATION), False, True)
    env.set_editor_property("bBuildArenaRig", False)
    env.set_editor_property("bStartOnCityStage", True)
    env.set_editor_property("bSpawnCityJumpPlatforms", True)

    # --- 3. Exactly two character agents ---
    agents = find_actors(actor_subsystem, AGENT_CLASS_NAME)
    if len(agents) < 2:
        missing = 2 - len(agents)
        for _ in range(missing):
            actor_subsystem.spawn_actor_from_class(
                unreal.load_class(None, AGENT_CLASS_PATH),
                unreal.Vector(ENV_LOCATION[0], ENV_LOCATION[1], AGENT_SPAWN_Z),
                unreal.Rotator(0.0, 0.0, 0.0))
        agents = find_actors(actor_subsystem, AGENT_CLASS_NAME)
        log("spawned %d agent(s)" % missing)
    else:
        log("agents already present (%d)" % len(agents))

    if len(agents) != 2:
        fail("expected exactly 2 %s actors, found %d" % (AGENT_CLASS_NAME, len(agents)))

    chaser, evader = agents[0], agents[1]
    chaser.set_actor_label("PursuitChaser")
    evader.set_actor_label("PursuitEvader")

    # --- 4. Wire the env's references and persist ---
    env.set_editor_property("ChaserAgent", chaser)
    env.set_editor_property("EvaderAgent", evader)
    log("env references wired: chaser=%s evader=%s" % (chaser.get_name(), evader.get_name()))

    if not level_editor.save_current_level():
        fail("save_current_level failed for " + TRAIN_LEVEL)

    log("OK - " + TRAIN_LEVEL + " saved")


main()
