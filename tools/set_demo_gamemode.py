# Copyright Epic Games, Inc. All Rights Reserved.
#
# Pins the playable game mode onto the Cartoon City "Demonstration" map so that, when the
# project opens or plays that map, the player and the chasers spawn and initialise.
#
# This is a *per-level* override - the same mechanism tools/gen_play_level.py uses for
# L_PursuitPlay. Setting it here means the RL training map (L_PursuitAITrain) is completely
# untouched and never sees a player pawn or a chaser, which is the whole point of keeping the
# training and play scenes in separate maps with no shared state.
#
# Run headless:
#   UnrealEditor-Cmd.exe <project>.uproject -run=pythonscript \
#       -script="<repo>/tools/set_demo_gamemode.py" -unattended -nosplash -nullrhi
#
# Idempotent: re-running only re-asserts the same override and reports the PlayerStarts found.

import unreal

MAP_PATH = "/Game/Cartoon_City_Free/Maps/Demonstration"
GAME_MODE_CLASS = "/Script/PursuitAI.PursuitPlayGameMode"


def log(message):
    unreal.log("[PursuitAI/set_demo] " + str(message))


def fail(message):
    unreal.log_error("[PursuitAI/set_demo] " + str(message))
    raise RuntimeError(message)


def main():
    level_editor = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    if level_editor is None:
        fail("LevelEditorSubsystem unavailable - run via an editor commandlet")

    if not level_editor.load_level(MAP_PATH):
        fail("failed to load " + MAP_PATH)

    # Report the PlayerStarts the game mode will choose from. A hand-built city map may have
    # several; ResolvePlayerSpawn() in the game mode takes the first, so knowing which that is
    # here is what makes a "the player spawned in the wrong place" report answerable offline.
    actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    starts = [a for a in actors.get_all_level_actors() if a.get_class().get_name() == "PlayerStart"]
    log("PlayerStart actors in %s: %d" % (MAP_PATH, len(starts)))
    for start in starts:
        rot = start.get_actor_rotation()
        log("  '%s' at %s yaw=%.0f" % (start.get_actor_label(), start.get_actor_location(), rot.yaw))

    gm_class = unreal.load_class(None, GAME_MODE_CLASS)
    if gm_class is None:
        fail("could not load " + GAME_MODE_CLASS + " (is the PursuitAI module compiled and loaded?)")

    world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
    settings = world.get_world_settings()

    current = settings.get_editor_property("default_game_mode")
    current_name = current.get_name() if current is not None else "<none>"
    log("current default_game_mode = %s" % current_name)

    settings.set_editor_property("default_game_mode", gm_class)
    after = settings.get_editor_property("default_game_mode")
    after_name = after.get_name() if after is not None else "<none>"
    log("set default_game_mode -> %s" % after_name)

    if not level_editor.save_current_level():
        fail("save_current_level failed for " + MAP_PATH)

    log("OK - %s now uses %s" % (MAP_PATH, GAME_MODE_CLASS))


main()
