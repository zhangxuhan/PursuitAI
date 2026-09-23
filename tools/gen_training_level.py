# Copyright Epic Games, Inc. All Rights Reserved.
#
# Generates the training level for PursuitAI without opening the editor by hand.
#
# Run headless:
#   UnrealEditor-Cmd.exe <project>.uproject -run=pythonscript \
#       -script="<repo>/tools/gen_training_level.py" -unattended -nosplash -nullrhi
#
# Idempotent: re-running opens the existing level and only adds the environment
# actor if it is missing.

import unreal

LEVEL_DIR = "/Game/Maps"
LEVEL_PATH = LEVEL_DIR + "/L_PursuitAITrain"
ENV_CLASS_PATH = "/Script/PursuitAI.PursuitAIEnv"
ENV_CLASS_NAME = "PursuitAIEnv"


def log(message):
    unreal.log("[PursuitAI/gen_level] " + str(message))


def fail(message):
    unreal.log_error("[PursuitAI/gen_level] " + str(message))
    raise RuntimeError(message)


def main():
    level_editor = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    actor_subsystem = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    asset_library = unreal.EditorAssetLibrary

    if level_editor is None:
        fail("LevelEditorSubsystem unavailable - is this running as an editor commandlet?")
    if actor_subsystem is None:
        fail("EditorActorSubsystem unavailable - is this running as an editor commandlet?")

    # 1. Make sure /Game/Maps exists.
    if not asset_library.does_directory_exist(LEVEL_DIR):
        log("creating directory " + LEVEL_DIR)
        asset_library.make_directory(LEVEL_DIR)

    # 2. Create the level, or open it if a previous run already made it.
    if asset_library.does_asset_exist(LEVEL_PATH):
        log("level exists, opening " + LEVEL_PATH)
        if not level_editor.load_level(LEVEL_PATH):
            fail("failed to load existing level " + LEVEL_PATH)
    else:
        log("creating level " + LEVEL_PATH)
        if not level_editor.new_level(LEVEL_PATH):
            fail("failed to create level " + LEVEL_PATH)

    # 3. Ensure exactly one environment actor is present.
    env_class = unreal.load_class(None, ENV_CLASS_PATH)
    if env_class is None:
        fail("could not load class " + ENV_CLASS_PATH + " (is the module compiled and loaded?)")

    env_actors = [
        actor
        for actor in actor_subsystem.get_all_level_actors()
        if actor.get_class().get_name() == ENV_CLASS_NAME
    ]

    if env_actors:
        log("environment actor already present (%d), leaving it alone" % len(env_actors))
    else:
        spawned = actor_subsystem.spawn_actor_from_class(
            env_class, unreal.Vector(0.0, 0.0, 0.0), unreal.Rotator(0.0, 0.0, 0.0)
        )
        if spawned is None:
            fail("spawn_actor_from_class returned None for " + ENV_CLASS_PATH)
        log("spawned environment actor: " + spawned.get_name())
        env_actors = [spawned]

    # 4. Persist.
    if not level_editor.save_current_level():
        fail("save_current_level failed for " + LEVEL_PATH)

    log("OK - level saved at " + LEVEL_PATH + " with %d environment actor(s)" % len(env_actors))


main()
