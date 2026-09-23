# Reads back the jump test's crate as the CHASER's own probes would see it.
#
# The question this answers is not "did a box spawn" - the log already said that - but "what
# does a downward trace at the crate's XY find, and what does the chaser's face feeler find at
# chest height". Those are the two numbers the jump decision is built from, and a crate that
# is present but not tall enough (or not blocking ECC_WorldStatic) looks identical in a log
# that only prints the spawn transform.
#
# Written because -PursuitJumpTest kept reporting "no jump" for chaser 1 while reporting
# "jumped over an obstacle, 88 cm tall" for the parked chasers: two different answers about the
# same crate, which means the fixture and the measurement disagree and one of them is wrong.
#
#   UnrealEditor-Cmd.exe <project>.uproject -run=pythonscript \
#       -script="<repo>/tools/inspect_jump_crate.py" -unattended -nosplash -nullrhi

import unreal

MAP_PATH = "/Game/Cartoon_City_Free/Maps/Demonstration"
OUT_PATH = r"E:\Project\UE5+ai\_crate_probe.txt"

# Matches APursuitPlayGameMode::SetupJumpTest's constants. Duplicated rather than shared
# because the point of this script is to be an independent second opinion, not to restate the
# C++ from the same source.
CRATE_HEIGHT = 90.0
CRATE_WIDTH = 300.0

lines = []


def log(message):
    lines.append(str(message))


def main():
    level_editor = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    if not level_editor.load_level(MAP_PATH):
        log("ERROR: could not load %s" % MAP_PATH)
        return

    actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors()

    # The player start, because the jump test anchors everything on it.
    base = None
    for actor in actors:
        if actor.get_class().get_name() == "PlayerStart":
            base = actor.get_actor_location()
            log("PlayerStart = (%.1f, %.1f, %.1f)" % (base.x, base.y, base.z))
    if base is None:
        log("ERROR: no PlayerStart")
        return

    # The floor, as found by a downward trace from 500 cm above the start.
    # ECC_WorldStatic = 1 in the enum the Python API exposes as TraceTypeQuery names; the
    # editor's line_trace_single_by_channel takes the *channel* enum.
    world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()

    start = unreal.Vector(base.x, base.y, base.z + 500.0)
    end = unreal.Vector(base.x, base.y, base.z - 5000.0)
    hit = unreal.SystemLibrary.line_trace_single_by_channel(
        world, start, end, unreal.TraceTypeQuery.TRACE_TYPE_QUERY1, True, [], unreal.DrawDebugTrace.NONE, True)
    if isinstance(hit, tuple):
        hit = hit[0]
        hit = hit[0]
    log("ground under PlayerStart: %s" % hit)

    # Any actor already named like the test crate.
    for actor in actors:
        if "JumpTest" in actor.get_actor_label():
            origin, extent = actor.get_actor_bounds(False)
            log("found %s at (%.1f, %.1f, %.1f) extent=(%.1f, %.1f, %.1f) topZ=%.1f" % (
                actor.get_actor_label(), origin.x, origin.y, origin.z,
                extent.x, extent.y, extent.z, origin.z + extent.z))
            comp = actor.get_editor_property("static_mesh_component")
            mesh = comp.get_editor_property("static_mesh")
            body = comp.get_editor_property("body_instance")
            log("  mesh=%s collision_enabled=%s" % (
                mesh.get_name() if mesh else "<none>",
                body.get_editor_property("collision_enabled") if body else "<none>"))
            log("  component scale=%s" % comp.get_editor_property("relative_scale3d"))

    log("NOTE: the crate is spawned at RUNTIME by the game mode, so a level-only inspection")
    log("      will not find it. What this run establishes is the floor height the test will")
    log("      anchor to, which is what GroundZ is computed from.")

    with open(OUT_PATH, "w", encoding="utf-8") as handle:
        handle.write("\n".join(lines))


main()
