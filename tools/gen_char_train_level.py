# Copyright Epic Games, Inc. All Rights Reserved.
#
# Generates the v2 character training level (L_PursuitCharTrain) headlessly.
#
# Run headless:
#   tools\run_pyscript.ps1 -Script "E:\Project\UE5+ai\tools\gen_char_train_level.py" -Log <log>
#   (which wraps UnrealEditor-Cmd -run=pythonscript ... -unattended -nosplash -nullrhi)
#
# The level contains exactly three actors: the environment, the chaser, the evader.
# The floor and the boundary ring are built by APursuitCharEnv::BuildArenaRig in C++
# (bBuildArenaRig=True is set here) - the editor-python commandlet in this 5.7 install
# crashes on spawn_actor_from_object for static meshes, so the level script cannot place
# meshes itself. No NavMesh (spawns are analytic), no lights (training is headless).
#
# Idempotent: re-running opens the level, tops up anything missing, and saves.

import unreal

LEVEL_DIR = "/Game/Maps"
LEVEL_PATH = LEVEL_DIR + "/L_PursuitCharTrain"

ENV_CLASS_PATH = "/Script/PursuitAI.PursuitCharEnv"
ENV_CLASS_NAME = "PursuitCharEnv"
AGENT_CLASS_PATH = "/Script/PursuitAI.PursuitCharAgent"
AGENT_CLASS_NAME = "PursuitCharAgent"

AGENT_SPAWN_Z = 95.0  # capsule centre above the floor (half height 88 + margin)


def log(message):
    unreal.log("[PursuitAI/gen_char_level] " + str(message))


def fail(message):
    unreal.log_error("[PursuitAI/gen_char_level] " + str(message))
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

    if not asset_library.does_directory_exist(LEVEL_DIR):
        asset_library.make_directory(LEVEL_DIR)

    if asset_library.does_asset_exist(LEVEL_PATH):
        log("level exists, opening " + LEVEL_PATH)
        if not level_editor.load_level(LEVEL_PATH):
            fail("failed to load existing level " + LEVEL_PATH)
    else:
        log("creating level " + LEVEL_PATH)
        if not level_editor.new_level(LEVEL_PATH):
            fail("failed to create level " + LEVEL_PATH)

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

    # The arena rig (floor + boundary ring) is this env's job, in C++. Never leave it on
    # by accident for a city demo - only the training map asks for it.
    env.set_editor_property("bBuildArenaRig", True)

    # --- 2. Exactly two character agents ---
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

    chaser, evader = agents[0], agents[1]
    chaser.set_actor_label("PursuitChaser")
    evader.set_actor_label("PursuitEvader")

    # --- 3. Wire the env's references and persist ---
    env.set_editor_property("ChaserAgent", chaser)
    env.set_editor_property("EvaderAgent", evader)
    log("env references wired: chaser=%s evader=%s" % (chaser.get_name(), evader.get_name()))

    if not level_editor.save_current_level():
        fail("save_current_level failed for " + LEVEL_PATH)

    log("OK - " + LEVEL_PATH + " saved")


main()
