# Proves the generator's delete rule is safe to re-run over a level you have edited by hand.
#
# Run:
#   UnrealEditor-Cmd.exe <project>.uproject -run=pythonscript \
#       -script="<repo>/tools/verify_handmade_survives.py" -unattended -nosplash -nullrhi
#
# The claim being tested is the one in PLAY_MAP.md: "hand-placed actors survive a re-run of
# tools/gen_play_level.py". That claim is easy to assert and easy to get wrong - the failure
# mode is silent and it destroys work - so it gets an actual test rather than a comment.
#
# Everything happens in a throwaway level (TEST_LEVEL) which is deleted at the end, so the real
# play map is never touched. That is not just tidiness: if you have L_PursuitPlay open in the
# editor, the editor holds a lock on the .umap and any headless run that tries to save it dies
# with a sharing violation (Error Code 32) instead of testing anything.
#
# Method, one editor process:
#   1. generate the arena into TEST_LEVEL
#   2. plant three actors there - two that belong to you, one labelled PP_* that belongs to the
#      generator - and save
#   3. generate again: this reloads TEST_LEVEL and rebuilds everything the generator owns
#   4. check which of the three survived, and that the arena itself is still there
#   5. delete TEST_LEVEL
#
# Step 3 works because gen_play_level.py guards its main() behind __name__ == "__main__":
# exec'ing it as a module defines main() without running it.

import os

import unreal

TEST_LEVEL = "/Game/Maps/_VerifyHandmade"
REAL_LEVEL = "/Game/Maps/L_PursuitPlay"
GENERATOR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "gen_play_level.py")

# The generated arena is 21 actors: 18 of geometry (floor + 4 walls + 4 blocks + 5 pillars +
# 4 crates) plus 2 directional lights plus the PlayerStart.
# A test that only checked "nothing was deleted" would pass if the generator had broken and
# deleted nothing at all, so the arena is asserted too.
EXPECTED_ARENA = 21

# (label, want it alive after the generator runs, why)
CASES = [
    ("Handmade_Test", True, "a StaticMeshActor placed by hand - the class the generator rebuilds"),
    ("Handmade_Light", True, "a DirectionalLight placed by hand"),
    ("PP_HandmadeFake", False, "PP_ prefix on a generated class - the generator's own territory"),
]


def log(message):
    unreal.log("[PursuitAI/verify] " + str(message))


def labels_in_level():
    return {a.get_actor_label(): a.get_class().get_name()
            for a in unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors()}


def plant_test_actors():
    actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    mesh_class = unreal.load_class(None, "/Script/Engine.StaticMeshActor")
    sun_class = unreal.load_class(None, "/Script/Engine.DirectionalLight")
    if mesh_class is None or sun_class is None:
        raise RuntimeError("could not load the classes to plant; is the module compiled?")

    cube = unreal.EditorAssetLibrary.load_asset("/Engine/BasicShapes/Cube.Cube")

    # Parked above the arena so they are obvious if ever left behind, and clear of the
    # PlayerStart so this cannot interfere with a play test run afterwards.
    handmade_mesh = actors.spawn_actor_from_class(
        mesh_class, unreal.Vector(0.0, 0.0, 900.0), unreal.Rotator())
    handmade_mesh.set_actor_label("Handmade_Test")
    component = handmade_mesh.get_editor_property("static_mesh_component")
    component.set_editor_property("static_mesh", cube)
    component.set_editor_property("mobility", unreal.ComponentMobility.MOVABLE)

    for label, height in (("Handmade_Light", 2400.0), ("PP_HandmadeFake", 2500.0)):
        light = actors.spawn_actor_from_class(
            sun_class, unreal.Vector(0.0, 0.0, height), unreal.Rotator(pitch=-90.0))
        light.set_actor_label(label)


def load_generator():
    # exec rather than import: the editor's python commandlet does not put tools/ on sys.path.
    # The __name__ override stops gen_play_level's own guard from firing on the way in, so
    # main() runs exactly once - when it is called below.
    namespace = {"__name__": "gen_play_level", "__file__": GENERATOR}
    with open(GENERATOR, "r", encoding="utf-8") as handle:
        source = handle.read()
    exec(compile(source, GENERATOR, "exec"), namespace)
    return namespace["main"]


def main():
    level_editor = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    library = unreal.EditorAssetLibrary

    if not os.path.isfile(GENERATOR):
        raise RuntimeError("generator not found at " + GENERATOR)

    generate = load_generator()
    failures = 0

    # --- 1. build the arena in a throwaway level -----------------------------
    generate(TEST_LEVEL)
    if not library.does_asset_exist(TEST_LEVEL):
        raise RuntimeError("generator did not create " + TEST_LEVEL)

    # --- 2. plant ------------------------------------------------------------
    plant_test_actors()
    planted = [n for n, _, _ in CASES if n in labels_in_level()]
    log("planted %d actor(s): %s" % (len(planted), ", ".join(planted)))
    if len(planted) != len(CASES):
        raise RuntimeError("planting failed - expected %d, found %s" % (len(CASES), planted))

    if not level_editor.save_current_level():
        raise RuntimeError("could not save " + TEST_LEVEL + " with the test actors in it")

    # --- 3. run the generator over it again ---------------------------------
    generate(TEST_LEVEL)
    log("generator finished; checking what survived")

    # --- 4. check ------------------------------------------------------------
    # Re-read from scratch: main() reloaded the level, so anything held before that is stale.
    after = labels_in_level()
    for label, want_alive, why in CASES:
        alive = label in after
        ok = alive == want_alive
        if not ok:
            failures += 1
        log("verify %s - %s alive=%s expected=%s (%s)"
            % ("PASS" if ok else "FAIL", label, alive, want_alive, why))

    arena = [n for n in after if n.startswith("PP_") and n != "PP_HandmadeFake"]
    log("generated arena present: %d actor(s) labelled PP_* (expected %d)"
        % (len(arena), EXPECTED_ARENA))
    if len(arena) < EXPECTED_ARENA:
        failures += 1
        log("FAIL - the arena lost actors; the delete rule is over-matching")

    # --- 5. clean up ---------------------------------------------------------
    if library.does_asset_exist(TEST_LEVEL):
        if library.delete_asset(TEST_LEVEL):
            log("deleted " + TEST_LEVEL)
        else:
            log("WARNING - could not delete %s; remove it by hand" % TEST_LEVEL)

    log("real map untouched: %s %s"
        % (REAL_LEVEL, "still present" if library.does_asset_exist(REAL_LEVEL) else "MISSING"))
    log("RESULT %s (%d failure(s))" % ("PASS" if failures == 0 else "FAIL", failures))


main()
